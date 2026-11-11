/*
 * MH2P IPU1 subsystem and optional diagnostic direct-start fixture.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "elf.h"
#include "hw/arm/armv7m.h"
#include "hw/arm/mh2p_rcc_ipu.h"
#include "hw/arm/mh2p_rcc_mmu.h"
#include "hw/core/clock.h"
#include "hw/core/loader.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "system/address-spaces.h"
#include "system/reset.h"
#include "system/tcg.h"
#include "target/arm/cpu.h"
#include "target/arm/internals.h"

typedef struct IPUAMMUPage {
    MemoryRegion alias;
    uint32_t address, translated, policy;
    unsigned kind;
} IPUAMMUPage;

struct MH2PIPU1 {
    ARMv7MState core[2];
    MemoryRegion core_id[2], core_memory[2], wugen;
    unsigned core_count;
    uint32_t intercore_interrupt, wakeup_mask[2];
    Clock *clock;
    MemoryRegion memory;
    MemoryRegion l2;
    MemoryRegion outgoing;
    MemoryRegion private_alias[4];
    MemoryRegion public_cache, public_local;
    MemoryRegion native_system;
    AddressSpace l2_as;
    MemoryRegion identity;
    MemoryRegion cache;
    MemoryRegion local_alias;
    MemoryRegion unknown;
    MemoryRegion text;
    MemoryRegion data;
    MemoryRegion ipc;
    MemoryRegion local;
    MemoryRegion aliases[13];
    AddressSpace as;
    IPUAMMUPage pages[16];
    uint32_t cache_config, cache_ocp, cache_start, cache_end;
    uint32_t ammu_maint, ammu_start, ammu_end;
    uint32_t sctm_control, sctm_counter_control[8], sctm_count[8];
    uint32_t sctm_shadow[8];
    uint64_t sctm_ticks;
    uint32_t reset_control;
    bool native, clock_enabled, held[2], reset_pending[2];
};

#define IPU1_CLOCK_HZ 212800000
#define IPU1_NATIVE_CPU_CLOCK_HZ (212800000 / 2)

static bool ipu1_sctm_chained(MH2PIPU1 *s, unsigned index)
{
    return (index == 2 || index == 4) &&
           (s->sctm_counter_control[index] & 4) &&
           (s->sctm_counter_control[index + 1] & 4);
}

static void ipu1_sctm_sync(MH2PIPU1 *s)
{
    uint64_t ticks = muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                             IPU1_CLOCK_HZ, NANOSECONDS_PER_SECOND);
    uint64_t elapsed = ticks - s->sctm_ticks;

    s->sctm_ticks = ticks;
    if (!(s->sctm_control & 1)) {
        return;
    }
    for (unsigned i = 2; i < ARRAY_SIZE(s->sctm_count); i++) {
        uint64_t count = s->sctm_count[i];
        unsigned high = i;
        bool chained = ipu1_sctm_chained(s, i);

        if (chained) {
            count |= (uint64_t)s->sctm_count[i + 1] << 32;
            high++;
        }
        if (s->sctm_counter_control[i] & 1) {
            uint64_t updated = count + elapsed;

            s->sctm_count[i] = updated;
            if (chained) {
                s->sctm_count[high] = updated >> 32;
            }
            if (chained ? updated < count : updated > UINT32_MAX) {
                s->sctm_counter_control[high] |= BIT(6);
            }
        }
        i = high;
    }
}

static void ipu1_sctm_clear(MH2PIPU1 *s, unsigned index)
{
    s->sctm_count[index] = s->sctm_shadow[index] = 0;
    s->sctm_counter_control[index] &= ~BIT(6);
    if (ipu1_sctm_chained(s, index)) {
        ipu1_sctm_clear(s, index + 1);
    }
}

static bool ipu1_sctm_read(MH2PIPU1 *s, hwaddr address, uint64_t *value)
{
    ipu1_sctm_sync(s);
    switch (address) {
    case 0x400:
        *value = s->sctm_control | (2 << 13) | (8 << 7);
        return true;
    case 0x47c:
    case 0x4f8:
        *value = 0;
        return true;
    case 0x4f0:
        *value = 0;
        for (unsigned i = 0; i < ARRAY_SIZE(s->sctm_count); i++) {
            *value |= (s->sctm_counter_control[i] & 1) << i;
        }
        return true;
    }
    if (address >= 0x500 && address < 0x520) {
        unsigned index = (address - 0x500) / 4;

        *value = s->sctm_counter_control[index] |
                 ((index == 2 || index == 4) ? BIT(7) : 0);
        s->sctm_counter_control[index] &= ~BIT(6);
        return true;
    }
    if (address >= 0x580 && address < 0x5a0) {
        unsigned index = (address - 0x580) / 4;

        if ((index & 1) && ipu1_sctm_chained(s, index - 1)) {
            s->sctm_shadow[index - 1] = s->sctm_count[index - 1];
        }
        *value = ipu1_sctm_chained(s, index) ?
                 s->sctm_shadow[index] : s->sctm_count[index];
        return true;
    }
    return false;
}

static bool ipu1_sctm_write(MH2PIPU1 *s, hwaddr address, uint32_t value)
{
    ipu1_sctm_sync(s);
    switch (address) {
    case 0x400:
        if (value & ~0x3ffffU) {
            return false;
        }
        s->sctm_control = value & 7;
        if (!(value & 1)) {
            for (unsigned i = 0; i < ARRAY_SIZE(s->sctm_count); i++) {
                ipu1_sctm_clear(s, i);
            }
        }
        return true;
    case 0x4f0:
    case 0x4f8:
        /* Timer and external profiling events need their actual engines. */
        if (value & ~(address == 0x4f0 ? 0xfcU : 0xffU)) {
            return false;
        }
        for (unsigned i = 0; i < ARRAY_SIZE(s->sctm_count); i++) {
            if (address == 0x4f0) {
                s->sctm_counter_control[i] &= ~1U;
                s->sctm_counter_control[i] |= (value >> i) & 1;
            } else if (value & BIT(i)) {
                ipu1_sctm_clear(s, i);
            }
        }
        return true;
    }
    if (address >= 0x500 && address < 0x520) {
        unsigned index = (address - 0x500) / 4;
        uint32_t allowed = 0xf | BIT(6) | BIT(7);

        if (index < 2) {
            allowed = BIT(1) | BIT(6);
        } else if (index >= 6) {
            allowed &= ~4U;
        }
        if (value & ~allowed) {
            return false;
        }
        s->sctm_counter_control[index] =
            (s->sctm_counter_control[index] & BIT(6)) | (value & 0xd);
        if (value & BIT(1)) {
            ipu1_sctm_clear(s, index);
        }
        return true;
    }
    return false;
}

/*
 * P2865's resource table supplies these device-address to physical-address
 * pairs. This diagnostic fixture installs the L2 mappings directly, not MMU.
 * Zero-PA carveouts have private backing, not invented A15 DDR placement.
 */
static const struct {
    uint32_t device, physical, size;
} ipu1_maps[] = {
    { 0x60000000, 0x8cc00000, MiB },
    { 0x6a000000, 0x4a000000, 16 * MiB },
    { 0x68000000, 0x48000000, 2 * MiB },
    { 0x68400000, 0x48400000, 4 * MiB },
    { 0x68800000, 0x48800000, 8 * MiB },
    { 0x60200000, 0x8ca00000, MiB },
    { 0x98000000, 0x88000000, 10 * MiB },
    { 0x60400000, 0x8dc00000, 2 * MiB },
    { 0x9bc00000, 0x8cb00000, MiB },
    { 0x60800000, 0x8cb00000, MiB },
    { 0x60600000, 0x40300000, MiB },
    { 0x60900000, 0x08900000, MiB },
    { 0x63300000, 0x43300000, 4 * MiB },
};

static void ipu1_page_update(IPUAMMUPage *page)
{
    static const uint32_t sizes[3][2] = {
        { 32 * MiB, 512 * MiB }, { 128 * KiB, 256 * KiB },
        { 4 * KiB, 16 * KiB },
    };
    uint32_t size = sizes[page->kind][(page->policy >> 1) & 1];
    uint32_t source = page->address & ~(size - 1);
    uint32_t target = (page->translated & 1) ? source :
                      (page->translated & ~(size - 1));

    memory_region_transaction_begin();
    memory_region_set_enabled(&page->alias, false);
    memory_region_set_size(&page->alias, size);
    memory_region_set_alias_offset(&page->alias, target);
    memory_region_set_address(&page->alias, source);
    memory_region_set_enabled(&page->alias, page->policy & 1);
    memory_region_transaction_commit();
}

static uint32_t *ipu1_page_register(MH2PIPU1 *s, hwaddr address,
                                    IPUAMMUPage **page)
{
    static const struct {
        uint16_t base;
        uint8_t first, count, field;
    } registers[] = {
        { 0x800, 0, 4, 0 }, { 0x820, 0, 4, 1 }, { 0x840, 0, 4, 2 },
        { 0x860, 4, 2, 0 }, { 0x8a0, 4, 2, 1 }, { 0x8e0, 4, 2, 2 },
        { 0x920, 6, 10, 0 }, { 0x9a0, 6, 10, 1 }, { 0xa20, 6, 10, 2 },
    };

    for (unsigned i = 0; i < ARRAY_SIZE(registers); i++) {
        if (address >= registers[i].base &&
            address < registers[i].base + registers[i].count * 4) {
            *page = &s->pages[registers[i].first +
                              (address - registers[i].base) / 4];
            switch (registers[i].field) {
            case 0:
                return &(*page)->address;
            case 1:
                return &(*page)->translated;
            default:
                return &(*page)->policy;
            }
        }
    }
    return NULL;
}

static uint64_t ipu1_cache_read(void *opaque, hwaddr address, unsigned size)
{
    MH2PIPU1 *s = opaque;
    IPUAMMUPage *page;
    uint32_t *value = ipu1_page_register(s, address, &page);
    uint64_t counter;

    if (value) {
        return *value;
    }
    if (address >= 0x400 && address < 0x600 &&
        ipu1_sctm_read(s, address, &counter)) {
        return counter;
    }
    if (address >= 0xaa0 && address < 0xac8) {
        return 0;
    }
    switch (address) {
    case 4:
        return s->cache_config;
    case 8: /* No outstanding cache maintenance in a coherent memory model. */
    case 0x10:
        return 0;
    case 0xc:
        return s->cache_ocp;
    case 0x14:
        return s->cache_start;
    case 0x18:
        return s->cache_end;
    case 0xca8:
        return s->ammu_maint;
    case 0xcac:
        return s->ammu_start;
    case 0xcb0:
        return s->ammu_end;
    case 0xcb4:
        return 0;
    default:
        qemu_log_mask(LOG_UNIMP, "mh2p-ipu1: unimplemented cache read 0x%"
                      HWADDR_PRIx "\n", address);
        return 0;
    }
}

static void ipu1_cache_write(void *opaque, hwaddr address, uint64_t value,
                             unsigned size)
{
    MH2PIPU1 *s = opaque;
    IPUAMMUPage *page;
    uint32_t *reg = ipu1_page_register(s, address, &page);

    if (reg) {
        *reg = value;
        ipu1_page_update(page);
        return;
    }
    if (address >= 0x400 && address < 0x600 &&
        ipu1_sctm_write(s, address, value)) {
        return;
    }
    if (address >= 0xaa0 && address < 0xac8 && !(value & ~0xcU)) {
        return;
    }
    switch (address) {
    case 4:
        if (!(s->cache_config & 1)) {
            s->cache_config = value & 0x1f;
        }
        return;
    case 8:
        if (value) {
            break;
        }
        return;
    case 0xc:
        s->cache_ocp = value;
        return;
    case 0x10:
        /* Data is already coherent; do not synthesize an interrupt. */
        if (value & ~0x1f) {
            break;
        }
        return;
    case 0x14:
        s->cache_start = value;
        return;
    case 0x18:
        s->cache_end = value;
        return;
    case 0xca8:
        /* Legacy AMMU maintenance: coherent RAM needs no clean/invalidate. */
        if (value & ~0x798U) {
            break;
        }
        s->ammu_maint = value & 0x380;
        return;
    case 0xcac:
        s->ammu_start = value;
        return;
    case 0xcb0:
        s->ammu_end = value;
        return;
    }
    qemu_log_mask(LOG_UNIMP, "mh2p-ipu1: unimplemented cache write 0x%"
                  HWADDR_PRIx " = 0x%" PRIx64 "\n", address, value);
}

static const MemoryRegionOps ipu1_cache_ops = {
    .read = ipu1_cache_read,
    .write = ipu1_cache_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static uint64_t ipu1_core_id_read(void *opaque, hwaddr address, unsigned size)
{
    return *(const unsigned *)opaque;
}

static const MemoryRegionOps ipu1_core_id_ops = {
    .read = ipu1_core_id_read,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void ipu1_intercore_irq(MH2PIPU1 *s)
{
    for (unsigned i = 0; i < s->core_count; i++) {
        qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->core[i]), 3),
                     !!(s->intercore_interrupt & BIT(i * 16)));
    }
}

static uint64_t ipu1_wugen_read(void *opaque, hwaddr address, unsigned size)
{
    MH2PIPU1 *s = opaque;
    uint32_t value;

    switch (address & ~3) {
    case 0:
        value = s->intercore_interrupt;
        break;
    case 0xc:
    case 0x10:
        value = s->wakeup_mask[(address - 0xc) / 4];
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "mh2p-ipu1: unsupported WUGEN read 0x%"
                      HWADDR_PRIx "\n", address);
        return 0;
    }
    return extract32(value, (address & 3) * 8, size * 8);
}

static void ipu1_wugen_write(void *opaque, hwaddr address, uint64_t value,
                             unsigned size)
{
    MH2PIPU1 *s = opaque;

    switch (address & ~3) {
    case 0:
        value = deposit32(s->intercore_interrupt, (address & 3) * 8,
                          size * 8, value);
        if ((value & ~0x10001U) ||
            (s->core_count < 2 && (value & BIT(16)))) {
            break;
        }
        s->intercore_interrupt = value;
        ipu1_intercore_irq(s);
        return;
    case 0xc:
    case 0x10:
        s->wakeup_mask[(address - 0xc) / 4] =
            deposit32(s->wakeup_mask[(address - 0xc) / 4],
                      (address & 3) * 8, size * 8, value);
        return;
    }
    qemu_log_mask(LOG_UNIMP, "mh2p-ipu1: unsupported WUGEN write 0x%"
                  HWADDR_PRIx " = 0x%" PRIx64 "\n", address, value);
}

static const MemoryRegionOps ipu1_wugen_ops = {
    .read = ipu1_wugen_read,
    .write = ipu1_wugen_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void ipu1_reset(void *opaque)
{
    MH2PIPU1 *s = opaque;

    s->cache_config = 0x1c;
    s->cache_ocp = s->cache_start = s->cache_end = 0;
    s->ammu_maint = s->ammu_start = s->ammu_end = 0;
    s->sctm_control = 4;
    s->sctm_ticks = muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                           IPU1_CLOCK_HZ, NANOSECONDS_PER_SECOND);
    memset(s->sctm_counter_control, 0, sizeof(s->sctm_counter_control));
    memset(s->sctm_count, 0, sizeof(s->sctm_count));
    memset(s->sctm_shadow, 0, sizeof(s->sctm_shadow));
    s->intercore_interrupt = 0;
    s->wakeup_mask[0] = s->wakeup_mask[1] = 0;
    if (s->core_count) {
        ipu1_intercore_irq(s);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(s->pages); i++) {
        IPUAMMUPage *page = &s->pages[i];

        page->address = page->translated = page->policy = 0;
        if (i == 6) {
            page->policy = 1;
        } else if (i == 7) {
            page->address = 0x40000000;
            page->translated = 0x55080000;
            page->policy = 3;
        }
        ipu1_page_update(page);
    }
}

static void ipu1_cpu_reset(void *opaque)
{
    MH2PIPU1 *s = opaque;

    for (unsigned i = 0; i < s->core_count; i++) {
        ARMCPU *cpu = s->core[i].cpu;

        cpu_reset(CPU(cpu));
        s->reset_pending[i] = s->reset_control & (4 | BIT(i));
        s->held[i] = !s->clock_enabled || (s->reset_control & (4 | BIT(i)));
        arm_set_cpu_power_state(cpu, s->held[i] ? PSCI_OFF : PSCI_ON);
        CPU(cpu)->halted = s->held[i];
    }
}

void mh2p_rcc_ipu1_control(MH2PIPU1 *s, uint32_t reset, bool clock)
{
    if ((reset & 4) && !(s->reset_control & 4)) {
        ipu1_reset(s);
    }
    s->reset_control = reset;
    s->clock_enabled = clock;
    clock_set_hz(s->clock, clock ?
                 (s->native ? IPU1_NATIVE_CPU_CLOCK_HZ : IPU1_CLOCK_HZ) : 0);
    if (s->native) {
        memory_region_set_enabled(&s->public_cache, clock && !(reset & 4));
        memory_region_set_enabled(&s->public_local, clock && !(reset & 4));
    }
    for (unsigned i = 0; i < s->core_count; i++) {
        bool held = !clock || (reset & (4 | BIT(i)));
        ARMCPU *cpu = s->core[i].cpu;

        s->reset_pending[i] |= (reset & (4 | BIT(i))) != 0;
        if (held != s->held[i]) {
            if (!held && s->reset_pending[i]) {
                device_cold_reset(DEVICE(&s->core[i]));
                cpu_reset(CPU(cpu));
                s->reset_pending[i] = false;
            }
            arm_set_cpu_power_state(cpu, held ? PSCI_OFF : PSCI_ON);
            CPU(cpu)->halted = held;
            s->held[i] = held;
            qemu_cpu_kick(CPU(cpu));
        }
    }
}

static uint64_t ipu1_unknown_read(void *opaque, hwaddr address, unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "mh2p-ipu1: unimplemented read at 0x%"
                  HWADDR_PRIx " size %u\n", address, size);
    return 0;
}

static void ipu1_unknown_write(void *opaque, hwaddr address,
                              uint64_t value, unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "mh2p-ipu1: unimplemented write at 0x%"
                  HWADDR_PRIx " = 0x%" PRIx64 " size %u\n",
                  address, value, size);
}

static const MemoryRegionOps ipu1_unknown_ops = {
    .read = ipu1_unknown_read,
    .write = ipu1_unknown_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static bool ipu1_load_range(uint32_t address, uint32_t length)
{
    static const struct {
        uint32_t base, size;
    } ram[] = {
        { 0, 2 * MiB }, { 0x20000000, 64 * KiB },
        { 0x80000000, 10 * MiB }, { 0x9f000000, MiB },
    };

    for (unsigned i = 0; i < ARRAY_SIZE(ram); i++) {
        if (address >= ram[i].base &&
            (uint64_t)address + length <=
            (uint64_t)ram[i].base + ram[i].size) {
            return true;
        }
    }
    return false;
}

static void ipu1_validate_elf(const char *firmware)
{
    g_autofree char *contents = NULL;
    g_autoptr(GError) error = NULL;
    size_t size;
    const uint8_t *data;
    uint32_t offset;
    uint16_t count;
    bool boot_found = false;

    if (!g_file_get_contents(firmware, &contents, &size, &error)) {
        error_report("mh2p-ipu1: cannot read firmware: %s", error->message);
        exit(EXIT_FAILURE);
    }
    data = (const uint8_t *)contents;
    if (size < sizeof(Elf32_Ehdr) || memcmp(data, ELFMAG, SELFMAG) ||
        data[EI_CLASS] != ELFCLASS32 || data[EI_DATA] != ELFDATA2LSB ||
        data[EI_VERSION] != EV_CURRENT || ldl_le_p(data + 20) != EV_CURRENT ||
        lduw_le_p(data + 16) != ET_EXEC ||
        lduw_le_p(data + 18) != EM_ARM || ldl_le_p(data + 24) != 0x11 ||
        lduw_le_p(data + 40) != sizeof(Elf32_Ehdr) ||
        lduw_le_p(data + 42) != sizeof(Elf32_Phdr)) {
        error_report("mh2p-ipu1: expected ARM ELF32 direct-start entry 0x11");
        exit(EXIT_FAILURE);
    }
    offset = ldl_le_p(data + 28);
    count = lduw_le_p(data + 44);
    if (!count || offset < sizeof(Elf32_Ehdr) ||
        (uint64_t)offset + count * sizeof(Elf32_Phdr) > size) {
        error_report("mh2p-ipu1: invalid ELF program-header bounds");
        exit(EXIT_FAILURE);
    }
    for (unsigned i = 0; i < count; i++) {
        const uint8_t *header = data + offset + i * sizeof(Elf32_Phdr);
        uint32_t file = ldl_le_p(header + 4);
        uint32_t address = ldl_le_p(header + 12);
        uint32_t filesz = ldl_le_p(header + 16);
        uint32_t memsz = ldl_le_p(header + 20);

        if (ldl_le_p(header) != PT_LOAD) {
            continue;
        }
        if (filesz > memsz || (uint64_t)file + filesz > size ||
            ldl_le_p(header + 8) != address ||
            !ipu1_load_range(address, memsz)) {
            error_report("mh2p-ipu1: invalid or unmapped ELF load segment %u", i);
            exit(EXIT_FAILURE);
        }
        if (!address && filesz >= 0x12 &&
            (ldl_le_p(header + 24) & PF_X) &&
            ldl_le_p(data + file + 4) == 0x11) {
            boot_found = true;
        }
    }
    if (!boot_found) {
        error_report("mh2p-ipu1: missing executable reset vectors at zero");
        exit(EXIT_FAILURE);
    }
}

DeviceState *mh2p_rcc_ipu1_create(Object *owner, const char *firmware,
                                DeviceState *mmu, DeviceState **secondary,
                                MH2PIPU1 **state, unsigned subsystem)
{
    static const struct {
        uint32_t base, size;
    } private_regions[] = {
        { 0x55020000, 0x10000 },
        { 0x55080000, 0x1000 },
        { 0x55081000, 0x18 },
    };
    MH2PIPU1 *s = g_new0(MH2PIPU1, 1);
    g_autofree char *local_name =
        g_strdup_printf("mh2p-ipu%u-local-ram", subsystem);
    g_autofree char *clock_name =
        g_strdup_printf("ipu%u-clock", subsystem);
    hwaddr public_base = subsystem == 1 ? 0x58800000 : 0x55000000;
    uint64_t entry;
    ssize_t loaded;

    s->native = !firmware;
    s->reset_control = s->native ? 7 : 0;
    s->clock_enabled = !s->native;
    if (firmware) {
        ipu1_validate_elf(firmware);
    }
    if (!tcg_enabled() || qemu_tcg_mttcg_enabled()) {
        error_report("mh2p-ipu1: %sexecution requires "
                     "-accel tcg,thread=single (or -icount)",
                     firmware ? "diagnostic " : "");
        exit(EXIT_FAILURE);
    }
    memory_region_init(&s->memory, owner, "mh2p-ipu1-device-addresses",
                       UINT64_C(0x100000000));
    memory_region_init(&s->l2, owner, "mh2p-ipu1-l2-addresses",
                       UINT64_C(0x100000000));
    address_space_init(&s->l2_as, &s->l2, "ipu1-diagnostic-l2");
    memory_region_init(&s->outgoing, owner, "ipu1-outgoing", UINT64_C(1) << 32);
    memory_region_add_subregion(&s->outgoing, 0,
                                mh2p_ipu_mmu_memory(mmu, &s->l2_as));
    for (unsigned i = 0; i < ARRAY_SIZE(private_regions); i++) {
        memory_region_init_alias(&s->private_alias[i], owner, "ipu1-private",
                                  &s->l2, private_regions[i].base,
                                  private_regions[i].size);
        memory_region_add_subregion_overlap(&s->outgoing,
            private_regions[i].base, &s->private_alias[i], 1);
    }
    memory_region_init_alias(&s->private_alias[3], owner, "ipu1-private-mmu",
        sysbus_mmio_get_region(SYS_BUS_DEVICE(mmu), 0), 0, 0x1000);
    memory_region_add_subregion_overlap(&s->outgoing, 0x55082000,
                                        &s->private_alias[3], 1);
    memory_region_init_alias(&s->identity, owner, "mh2p-ipu1-identity",
                             &s->outgoing, 0, UINT64_C(0x100000000));
    memory_region_add_subregion_overlap(&s->memory, 0, &s->identity, -1);
    if (firmware) {
        memory_region_init_io(&s->unknown, owner, &ipu1_unknown_ops, s,
                              "mh2p-ipu1-unimplemented",
                              UINT64_C(0x100000000));
        memory_region_add_subregion_overlap(&s->l2, 0, &s->unknown, -1);
        memory_region_init_ram(&s->text, NULL, "mh2p-ipu1-text-carveout",
                               2 * MiB, &error_fatal);
        memory_region_add_subregion(&s->l2, 0, &s->text);
        memory_region_init_ram(&s->data, NULL, "mh2p-ipu1-data-carveout",
                               10 * MiB, &error_fatal);
        memory_region_add_subregion(&s->l2, 0x80000000, &s->data);
        memory_region_init_ram(&s->ipc, NULL, "mh2p-ipu1-ipc-carveout",
                               MiB, &error_fatal);
        memory_region_add_subregion(&s->l2, 0x9f000000, &s->ipc);
    } else {
        memory_region_init_alias(&s->native_system, owner, "ipu1-system-bus",
                                  get_system_memory(), 0, UINT64_C(1) << 32);
        memory_region_add_subregion_overlap(&s->l2, 0, &s->native_system, -1);
    }
    memory_region_init_ram(&s->local, NULL, local_name,
                           64 * KiB, &error_fatal);
    memory_region_add_subregion(&s->l2, 0x20000000, &s->local);
    memory_region_init_alias(&s->local_alias, owner, "mh2p-ipu1-local-physical",
                             &s->local, 0, 64 * KiB);
    memory_region_add_subregion(&s->l2, 0x55020000, &s->local_alias);
    memory_region_init_io(&s->cache, owner, &ipu1_cache_ops, s,
                          "mh2p-ipu1-unicache-ammu", 0x1000);
    memory_region_add_subregion(&s->l2, 0x55080000, &s->cache);
    memory_region_init_alias(&s->public_cache, owner, "ipu1-cache-l3",
                              &s->cache, 0, 0x1000);
    memory_region_add_subregion(get_system_memory(), public_base + 0x80000,
                                &s->public_cache);
    memory_region_init_alias(&s->public_local, owner, "ipu1-local-l3",
                              &s->local, 0, 64 * KiB);
    memory_region_add_subregion(get_system_memory(), public_base + 0x20000,
                                &s->public_local);
    if (s->native) {
        memory_region_set_enabled(&s->public_cache, false);
        memory_region_set_enabled(&s->public_local, false);
    }
    G_STATIC_ASSERT(ARRAY_SIZE(ipu1_maps) == ARRAY_SIZE(s->aliases));
    for (unsigned i = 0; firmware && i < ARRAY_SIZE(ipu1_maps); i++) {
        g_autofree char *name = g_strdup_printf("mh2p-ipu1-resource%u", i);

        memory_region_init_alias(&s->aliases[i], owner, name,
                                 get_system_memory(), ipu1_maps[i].physical,
                                 ipu1_maps[i].size);
        memory_region_add_subregion(&s->l2, ipu1_maps[i].device,
                                    &s->aliases[i]);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(s->pages); i++) {
        IPUAMMUPage *page = &s->pages[i];
        g_autofree char *name = g_strdup_printf("mh2p-ipu1-ammu-page%u", i);

        page->kind = i < 4 ? 0 : (i < 6 ? 1 : 2);
        memory_region_init_alias(&page->alias, owner, name,
                                  &s->outgoing, 0, 4 * KiB);
        memory_region_set_enabled(&page->alias, false);
        memory_region_add_subregion_overlap(&s->memory, 0, &page->alias, 1 + i);
    }
    ipu1_reset(s);
    qemu_register_reset(ipu1_reset, s);
    address_space_init(&s->as, &s->memory, "mh2p-ipu1-loader");
    if (firmware) {
        loaded = load_elf_as(firmware, NULL, NULL, NULL, &entry, NULL, NULL,
                             NULL, ELFDATA2LSB, EM_ARM, 0, 0, &s->as);
        if (loaded < 0) {
            error_report("mh2p-ipu1: cannot load ELF: %s",
                         load_elf_strerror(loaded));
            exit(EXIT_FAILURE);
        }
    }
    s->clock = clock_new(owner, clock_name);
    clock_set_hz(s->clock, s->clock_enabled ? IPU1_CLOCK_HZ : 0);
    memory_region_init_io(&s->wugen, owner, &ipu1_wugen_ops, s,
                          "mh2p-ipu1-wugen", 0x18);
    memory_region_add_subregion(&s->l2, 0x55081000, &s->wugen);
    for (unsigned i = 0; i < (secondary ? 2 : 1); i++) {
        static unsigned core_ids[] = { 0, 1 };
        g_autofree char *name = g_strdup_printf("ipu%u-core%u", subsystem, i);
        ARMv7MState *core = &s->core[i];

        object_initialize_child(owner, name, core, TYPE_ARMV7M);
        qdev_prop_set_string(DEVICE(core), "cpu-type",
                             ARM_CPU_TYPE_NAME("cortex-m4"));
        qdev_prop_set_uint32(DEVICE(core), "num-irq", 64);
        qdev_prop_set_bit(DEVICE(core), "vfp", false);
        qdev_connect_clock_in(DEVICE(core), "cpuclk", s->clock);
        memory_region_init_alias(&s->core_memory[i], owner, name,
                                 &s->memory, 0, UINT64_C(0x100000000));
        object_property_set_link(OBJECT(core), "memory",
                                 OBJECT(&s->core_memory[i]), &error_abort);
        sysbus_realize(SYS_BUS_DEVICE(core), &error_fatal);
        memory_region_init_io(&s->core_id[i], owner, &ipu1_core_id_ops,
                              &core_ids[i], "mh2p-ipu1-core-id", 4);
        memory_region_add_subregion_overlap(&core->container, 0xe00fffe0,
                                            &s->core_id[i], 1);
        s->core_count++;
    }
    qemu_register_reset(ipu1_cpu_reset, s);
    ipu1_cpu_reset(s);
    if (secondary) {
        *secondary = DEVICE(&s->core[1]);
    }
    *state = s;
    return DEVICE(&s->core[0]);
}

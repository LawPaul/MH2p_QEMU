/*
 * GK20A initialization subset, not a rendering or command-processing GPU.
 * Register fields follow NVIDIA's public GK20A MC, PBUS and trim definitions.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/gk20a.h"

/* Documented MC_ENABLE reset-release controls; no execution is implied. */
#define GK20A_MC_ENABLE_MASK 0x3830317cU
#define GK20A_MC_ELPG_ENABLE_MASK 0x20100004U

static const struct {
    uint32_t offset;
    uint32_t mask;
    bool pbdma;
} gk20a_engine_config[] = {
    { 0x2140, UINT32_MAX, false }, /* FIFO_INTR_EN_0 */
    { 0x2528, UINT32_MAX, false }, /* FIFO_INTR_EN_1 */
    { 0x2a04, 0x3fffffff, false }, /* FIFO_FB_TIMEOUT */
    { 0x2a08, 0x80000000, false }, /* FIFO_PB_TIMEOUT detection enable */
    { 0x2a0c, UINT32_MAX, false }, /* FIFO_ENG_TIMEOUT */
    { 0x4010c, UINT32_MAX, true }, /* PBDMA_INTR_EN_0 */
    { 0x4014c, UINT32_MAX, true }, /* PBDMA_INTR_EN_1 */
    { 0x4013c, UINT32_MAX, true }, /* PBDMA_INTR_STALL */
    { 0x4012c, UINT32_MAX, true }, /* PBDMA_TIMEOUT */
};

struct GK20AState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    MemoryRegion bar1;
    MemoryRegion *smmu_mr;
    AddressSpace smmu_as;
    bool reset_asserted;
    bool experimental_engine_init;
    bool experimental_permissive;
    uint32_t bus_intr_enable;
    uint32_t bar1_block;
    uint32_t gpc2clk_out;
    uint32_t mc_enable;
    uint32_t mc_enable_pb;
    uint32_t mc_elpg_enable;
    uint32_t mmu_page_size;
    uint32_t engine_config[ARRAY_SIZE(gk20a_engine_config)];
    uint32_t pri_timeout;
    bool ring_started;
    uint32_t ring_sys_decode;
};

static void gk20a_engine_reset(GK20AState *s, bool pbdma_only)
{
    for (unsigned i = 0; i < ARRAY_SIZE(gk20a_engine_config); i++) {
        if (!pbdma_only || gk20a_engine_config[i].pbdma) {
            s->engine_config[i] = 0;
        }
    }
}

static bool gk20a_engine_accessible(GK20AState *s, hwaddr offset)
{
    return s->experimental_engine_init && (s->mc_enable & 0x100) &&
           (offset < 0x40000 || (s->mc_enable_pb & 1));
}

static uint32_t *gk20a_config_register(GK20AState *s, hwaddr offset,
                                      uint32_t *mask)
{
    if (gk20a_engine_accessible(s, offset)) {
        for (unsigned i = 0; i < ARRAY_SIZE(gk20a_engine_config); i++) {
            if (gk20a_engine_config[i].offset == offset) {
                *mask = gk20a_engine_config[i].mask;
                return &s->engine_config[i];
            }
        }
    }
    return NULL;
}

static void gk20a_ring_reset(GK20AState *s)
{
    s->ring_started = false;
    s->ring_sys_decode = 0;
}

static MemTxResult gk20a_read(void *opaque, hwaddr offset, uint64_t *value,
                             unsigned size, MemTxAttrs attrs)
{
    GK20AState *s = opaque;
    uint32_t mask, *config;

    if (s->reset_asserted) {
        qemu_log_mask(LOG_GUEST_ERROR, "gk20a: read during reset/clamp\n");
        return MEMTX_ERROR;
    }
    config = gk20a_config_register(s, offset, &mask);
    if (config) {
        *value = *config;
        return MEMTX_OK;
    }
    switch (offset) {
    case 0x0: /* MC_BOOT_0: GK20A (architecture E0 + implementation A), A1. */
        *value = 0x0ea000a1;
        return MEMTX_OK;
    case 0x1100: /* PBUS interrupt status: no bus-error engine yet. */
        *value = 0;
        return MEMTX_OK;
    case 0x1140: /* PBUS_INTR_EN_0 */
        *value = s->bus_intr_enable;
        return MEMTX_OK;
    case 0x1704: /* PBUS_BAR1_BLOCK: instance pointer, target and mode. */
        if (s->experimental_engine_init) {
            *value = s->bar1_block;
            return MEMTX_OK;
        }
        break;
    case 0x137250: /* TRIM_SYS_GPC2CLK_OUT */
        *value = s->gpc2clk_out;
        return MEMTX_OK;
    case 0x200: /* MC_ENABLE: reset controls, not execution completion. */
        *value = s->mc_enable;
        return MEMTX_OK;
    case 0x204: /* MC_ENABLE_PB: one PBDMA reset control. */
        if (s->experimental_engine_init) {
            *value = s->mc_enable_pb;
            return MEMTX_OK;
        }
        break;
    case 0x20c: /* MC_ELPG_ENABLE: XBAR, PFB and HUB gating controls. */
        if (s->experimental_engine_init) {
            *value = s->mc_elpg_enable;
            return MEMTX_OK;
        }
        break;
    case 0x100c80: /* FB_MMU_CTRL: VM page size only; no PRI FIFO model. */
        if (s->experimental_engine_init && (s->mc_enable & 0x100000)) {
            *value = s->mmu_page_size;
            if (s->experimental_permissive) {
                /*
                 * hw_fb_gk20a.h: PRI_FIFO_EMPTY (bit 15) and PRI_FIFO_SPACE
                 * (23:16).  Invalidates complete at once: no TLB is modelled.
                 */
                *value |= (1U << 15) | (0x10U << 16);
            }
            return MEMTX_OK;
        }
        break;
    case 0x70000: /* FB_FLUSH: no accepted GPU memory work or dirty cache. */
        if (s->experimental_engine_init && (s->mc_enable & 0x100000)) {
            *value = 0;
            return MEMTX_OK;
        }
        break;
    case 0x2100: /* FIFO_INTR_0 */
    case 0x2a00: /* FIFO_INTR_RUNLIST */
    case 0x40108: /* PBDMA_INTR_0 */
    case 0x40148: /* PBDMA_INTR_1 */
        if (gk20a_engine_accessible(s, offset)) {
            /* No accepted jobs or asynchronous error sources. */
            *value = 0;
            return MEMTX_OK;
        }
        break;
    case 0x2390: /* FIFO_PBDMA_MAP(0): PBDMA 0 connects to runlist 0. */
        if (gk20a_engine_accessible(s, offset)) {
            *value = 1;
            return MEMTX_OK;
        }
        break;
    case 0x9080: /* PTIMER_PRI_TIMEOUT */
        *value = s->pri_timeout;
        return MEMTX_OK;
    case 0x12004c: /* Synchronous register-ring commands have completed. */
    case 0x120058: /* No asynchronous ring-error sources. */
    case 0x12005c:
        *value = 0;
        return MEMTX_OK;
    case 0x120050:
        *value = s->ring_started;
        return MEMTX_OK;
    case 0x120070: /* One SYS, one FBP and one GPC register station. */
    case 0x120074:
    case 0x120078:
        *value = s->ring_started;
        return MEMTX_OK;
    case 0x122204:
        *value = s->ring_sys_decode;
        return MEMTX_OK;
    default:
        break;
    }
    qemu_log_mask(LOG_UNIMP, "gk20a: unsupported read at 0x%"
                  HWADDR_PRIx "%s\n", offset,
                  s->experimental_permissive ? " (permissive: 0)" : "");
    if (s->experimental_permissive) {
        /* Local diagnostic only: no hardware evidence for this value. */
        *value = 0;
        return MEMTX_OK;
    }
    return MEMTX_DECODE_ERROR;
}

static MemTxResult gk20a_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size, MemTxAttrs attrs)
{
    GK20AState *s = opaque;
    uint32_t mask, *config;

    if (s->reset_asserted) {
        qemu_log_mask(LOG_GUEST_ERROR, "gk20a: write during reset/clamp\n");
        return MEMTX_ERROR;
    }
    config = gk20a_config_register(s, offset, &mask);
    if (config && !(value & ~mask)) {
        *config = value;
        return MEMTX_OK;
    }
    switch (offset) {
    case 0x1100:
        if (value & ~0xeU) {
            break;
        }
        return MEMTX_OK;
    case 0x1140:
        if (value & ~0xeU) {
            break;
        }
        s->bus_intr_enable = value;
        return MEMTX_OK;
    case 0x1704:
        if (s->experimental_engine_init && !(value & ~0xbfffffffU) &&
            (value & 0x30000000) != 0x10000000) {
            s->bar1_block = value;
            return MEMTX_OK;
        }
        break;
    case 0x137250:
        if (value & ~0x80003f3fU) {
            break;
        }
        s->gpc2clk_out = value;
        return MEMTX_OK;
    case 0x200:
        if (value & ~(s->experimental_engine_init ?
                      GK20A_MC_ENABLE_MASK : 0x20U)) {
            break;
        }
        if (!(value & 0x20)) {
            gk20a_ring_reset(s);
        }
        if (!(value & 0x100)) {
            gk20a_engine_reset(s, false);
        }
        if (!(value & 0x100000)) {
            s->mmu_page_size = 0;
        }
        s->mc_enable = value;
        return MEMTX_OK;
    case 0x204:
        if (!s->experimental_engine_init || value > 1) {
            break;
        }
        if (!value) {
            gk20a_engine_reset(s, true);
        }
        s->mc_enable_pb = value;
        return MEMTX_OK;
    case 0x20c:
        if (!s->experimental_engine_init ||
            (value & ~GK20A_MC_ELPG_ENABLE_MASK)) {
            break;
        }
        s->mc_elpg_enable = value;
        return MEMTX_OK;
    case 0x100c80:
        if (s->experimental_engine_init && (s->mc_enable & 0x100000) &&
            !(value & ~1U)) {
            s->mmu_page_size = value;
            return MEMTX_OK;
        }
        break;
    case 0x70000:
        if (s->experimental_engine_init && (s->mc_enable & 0x100000) &&
            value <= 1) {
            /* An empty flush has no memory transactions to drain. */
            return MEMTX_OK;
        }
        break;
    case 0x2100:
    case 0x2a00:
    case 0x40108:
    case 0x40148:
        if (gk20a_engine_accessible(s, offset)) {
            return MEMTX_OK; /* W1C cannot create an interrupt or completion. */
        }
        break;
    case 0x9080:
        if (value & ~0x80ffffffU) {
            break;
        }
        s->pri_timeout = value;
        return MEMTX_OK;
    case 0x12004c:
        if (!(s->mc_enable & 0x20)) {
            break;
        }
        switch (value) {
        case 0: /* NO_CMD */
        case 2: /* ACK_INTERRUPT: no pending asynchronous errors. */
            return MEMTX_OK;
        case 4: /* ENUMERATE_AND_START_RING */
            s->ring_started = true;
            return MEMTX_OK;
        default:
            break;
        }
        break;
    case 0x122204:
        if (!(s->mc_enable & 0x20) || value > 2) {
            break;
        }
        s->ring_sys_decode = value;
        return MEMTX_OK;
    default:
        break;
    }
    qemu_log_mask(LOG_UNIMP, "gk20a: unsupported write at 0x%"
                  HWADDR_PRIx " value 0x%" PRIx64 "%s\n", offset, value,
                  s->experimental_permissive ? " (permissive: ignored)" : "");
    if (s->experimental_permissive) {
        return MEMTX_OK;
    }
    return MEMTX_DECODE_ERROR;
}

/*
 * BAR1 aperture.  No GPU MMU translation is modelled; without
 * experimental-permissive every access is a logged bus error, as before
 * the aperture existed.  Permissive mode reads zero and drops writes so the
 * original driver's BAR1 coherence test fails cleanly instead of faulting.
 */
/*
 * Fermi/Kepler GMMU walk (public layout, as used by nouveau gf100 VMM):
 *   PBUS_BAR1_BLOCK[27:0]  instance block >> 12, bit 31 virtual mode
 *   instance +0x200/0x204  page directory base (bits 31:12 / 39:32)
 *   PDE (8 bytes)          lo: big-page table >> 8 (bits 31:4), aperture 1:0
 *                          hi: small-page table >> 8 (bits 31:4), aperture 1:0
 *   PTE (8 bytes)          lo: bit 0 valid, page >> 8 in bits 31:4
 * Each PDE spans 128 MiB with 128 KiB big pages or 64 MiB with 64 KiB
 * (FB_MMU_CTRL bit 0).  GK20A memory is system memory, so apertures other
 * than "invalid" are treated alike.  Only BAR1 CPU accesses use this.
 */
#define GK20A_SMMU_BIT (1ULL << 34)

/*
 * GPU memory address: bit 34 selects the Tegra SMMU path (nvgpu
 * NV_MC_SMMU_VADDR_TRANSLATE_BIT) with a 32-bit IOVA; otherwise physical.
 */
static MemTxResult gk20a_mem_rw(GK20AState *s, uint64_t addr, void *buf,
                                unsigned len, bool write)
{
    AddressSpace *as = &address_space_memory;

    if (addr & GK20A_SMMU_BIT) {
        if (!s->smmu_mr) {
            return MEMTX_DECODE_ERROR;
        }
        as = &s->smmu_as;
        addr &= 0xffffffffULL;
    }
    return address_space_rw(as, addr, MEMTXATTRS_UNSPECIFIED, buf, len, write);
}

static uint32_t gk20a_ldl(GK20AState *s, uint64_t addr)
{
    uint32_t value = 0;

    gk20a_mem_rw(s, addr, &value, 4, false);
    return le32_to_cpu(value);
}

static bool gk20a_bar1_translate(GK20AState *s, hwaddr va, hwaddr *pa)
{
    unsigned big_shift = (s->mmu_page_size & 1) ? 16 : 17;
    unsigned pde_shift = big_shift + 10;
    hwaddr inst, pdb, pt;
    uint32_t lo, hi, pte;

    if (!(s->bar1_block & 0x80000000U)) {
        return false;
    }
    inst = (hwaddr)(s->bar1_block & 0x0fffffffU) << 12;
    lo = gk20a_ldl(s, inst + 0x200);
    hi = gk20a_ldl(s, inst + 0x204);
    pdb = ((hwaddr)(hi & 0xff) << 32) | (lo & 0xfffff000U);
    lo = gk20a_ldl(s, pdb + (va >> pde_shift) * 8);
    hi = gk20a_ldl(s, pdb + (va >> pde_shift) * 8 + 4);
    if (hi & 3) {
        pt = (hwaddr)(hi & 0xfffffff0U) << 8;
        pte = gk20a_ldl(s, pt + ((va & ((1ULL << pde_shift) - 1)) >> 12) * 8);
        if (pte & 1) {
            *pa = ((hwaddr)(pte & 0xfffffff0U) << 8) | (va & 0xfff);
            return true;
        }
    }
    if (lo & 3) {
        pt = (hwaddr)(lo & 0xfffffff0U) << 8;
        pte = gk20a_ldl(s, pt + ((va & ((1ULL << pde_shift) - 1)) >> big_shift) * 8);
        if (pte & 1) {
            *pa = ((hwaddr)(pte & 0xfffffff0U) << 8) |
                  (va & ((1ULL << big_shift) - 1));
            return true;
        }
    }
    qemu_log_mask(LOG_GUEST_ERROR, "gk20a: BAR1 0x%" HWADDR_PRIx
                  " unmapped (block 0x%08x pdb 0x%" HWADDR_PRIx
                  " pde 0x%08x:%08x)\n", va, s->bar1_block, pdb, hi, lo);
    return false;
}

static MemTxResult gk20a_bar1_read(void *opaque, hwaddr offset,
                                   uint64_t *value, unsigned size,
                                   MemTxAttrs attrs)
{
    GK20AState *s = opaque;
    hwaddr pa;

    if (s->experimental_permissive && gk20a_bar1_translate(s, offset, &pa)) {
        *value = 0;
        return gk20a_mem_rw(s, pa, value, size, false);
    }
    qemu_log_mask(LOG_UNIMP, "gk20a: BAR1 read at 0x%" HWADDR_PRIx "%s\n",
                  offset, s->experimental_permissive ? " (permissive: 0)" : "");
    *value = 0;
    return s->experimental_permissive ? MEMTX_OK : MEMTX_DECODE_ERROR;
}

static MemTxResult gk20a_bar1_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size,
                                    MemTxAttrs attrs)
{
    GK20AState *s = opaque;
    hwaddr pa;

    if (s->experimental_permissive && gk20a_bar1_translate(s, offset, &pa)) {
        return gk20a_mem_rw(s, pa, &value, size, true);
    }
    qemu_log_mask(LOG_UNIMP, "gk20a: BAR1 write at 0x%" HWADDR_PRIx
                  " value 0x%" PRIx64 "%s\n", offset, value,
                  s->experimental_permissive ? " (permissive: ignored)" : "");
    return s->experimental_permissive ? MEMTX_OK : MEMTX_DECODE_ERROR;
}

static const MemoryRegionOps gk20a_bar1_ops = {
    .read_with_attrs = gk20a_bar1_read,
    .write_with_attrs = gk20a_bar1_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static const MemoryRegionOps gk20a_ops = {
    .read_with_attrs = gk20a_read,
    .write_with_attrs = gk20a_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void gk20a_reset(DeviceState *dev)
{
    GK20AState *s = GK20A(dev);

    s->bus_intr_enable = 0;
    s->bar1_block = 0;
    s->gpc2clk_out = 0;
    s->mc_enable = 0;
    s->mc_enable_pb = 0;
    s->mc_elpg_enable = 0;
    s->mmu_page_size = 0;
    gk20a_engine_reset(s, false);
    s->pri_timeout = 0;
    gk20a_ring_reset(s);
}

static void gk20a_reset_line(void *opaque, int n, int level)
{
    GK20AState *s = opaque;

    if (level && !s->reset_asserted) {
        gk20a_reset(DEVICE(s));
    }
    s->reset_asserted = level;
}

static void gk20a_init(Object *obj)
{
    GK20AState *s = GK20A(obj);

    memory_region_init_io(&s->iomem, obj, &gk20a_ops, s, TYPE_GK20A,
                          16 * 1024 * 1024);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    memory_region_init_io(&s->bar1, obj, &gk20a_bar1_ops, s, "gk20a-bar1",
                          16 * 1024 * 1024);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->bar1);
    qdev_init_gpio_in_named(DEVICE(obj), gk20a_reset_line, "reset", 1);
}

static void gk20a_realize(DeviceState *dev, Error **errp)
{
    GK20AState *s = GK20A(dev);

    if (s->smmu_mr) {
        address_space_init(&s->smmu_as, s->smmu_mr, "gk20a-smmu");
    }
}

static void gk20a_class_init(ObjectClass *klass, const void *data)
{
    static const Property properties[] = {
        DEFINE_PROP_BOOL("experimental-engine-init", GK20AState,
                         experimental_engine_init, false),
        DEFINE_PROP_BOOL("experimental-permissive", GK20AState,
                         experimental_permissive, false),
        DEFINE_PROP_LINK("smmu", GK20AState, smmu_mr, TYPE_MEMORY_REGION,
                         MemoryRegion *),
    };
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, properties);
    dc->realize = gk20a_realize;
    device_class_set_legacy_reset(dc, gk20a_reset);
}

static const TypeInfo gk20a_info = {
    .name = TYPE_GK20A,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GK20AState),
    .instance_init = gk20a_init,
    .class_init = gk20a_class_init,
};

static void gk20a_register_types(void)
{
    type_register_static(&gk20a_info);
}

type_init(gk20a_register_types)

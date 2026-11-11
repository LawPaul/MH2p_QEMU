/*
 * T124 VIC upload model and opt-in bounded Falcon v5 execution.
 * No video-job execution.
 * Register facts: NVIDIA hw_flcn.h and envytools Falcon memory ports.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/tegra124_host1x.h"
#include "hw/misc/tegra124_vic.h"

#define VIC_MEMORY_WINDOW 0x10000
#define VIC_PORT_MASK 0x0300fffc
#define VIC_CODE_USABLE 1
#define VIC_CODE_BUSY 2
#define VIC_EXPERIMENTAL_RAM 0x8000
#define VIC_CPU_BUDGET 65536
#define VIC_FLAGS_MASK 0x01770fff
#define VIC_FLAGS_IE 0x00070000
#define VIC_FIFO_DEPTH 16
#define VIC_SYNC_DEPTH 16

struct Tegra124VicState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    MemoryRegion *dma_mr;
    AddressSpace dma_as;
    Tegra124Host1xState *host1x;
    qemu_irq irq;
    uint8_t imem[VIC_MEMORY_WINDOW];
    uint8_t dmem[VIC_MEMORY_WINDOW];
    uint32_t imem_tags[VIC_MEMORY_WINDOW / 256];
    uint8_t imem_flags[VIC_MEMORY_WINDOW / 256];
    uint32_t tlb_command;
    uint32_t tlb_result;
    uint32_t imem_index;
    uint32_t dmem_index;
    uint32_t dma_base;
    uint32_t dma_offset;
    uint32_t dma_fb_offset;
    uint32_t dma_command;
    uint32_t fb_config;
    uint32_t irq_mask;
    uint32_t irq_dest;
    uint32_t interface_enable;
    uint32_t bootvec;
    bool reset_asserted;
    bool clock_enabled;
    bool dma_faulted;
    bool experimental_execution;
    bool cpu_stopped;
    bool cpu_sleeping;
    bool cpu_faulted;
    bool cpu_running;
    bool cpu_in_interrupt;
    uint32_t pc;
    uint32_t regs[16];
    uint32_t special[16];
    uint32_t irq_status;
    uint32_t irq_mode;
    uint32_t irq_lines;
    uint32_t scratch[4];
    uint32_t method_offset;
    uint32_t fifo_method[VIC_FIFO_DEPTH];
    uint32_t fifo_data[VIC_FIFO_DEPTH];
    unsigned fifo_head;
    unsigned fifo_count;
    uint8_t sync_queue[VIC_SYNC_DEPTH];
    unsigned sync_head;
    unsigned sync_count;
};

static bool vic_cpu_run(Tegra124VicState *s);

static bool vic_syncpt_update(Tegra124VicState *s)
{
    if (s->reset_asserted || !s->clock_enabled || s->cpu_running ||
        s->cpu_stopped || !s->cpu_sleeping || s->cpu_in_interrupt ||
        s->cpu_faulted || s->dma_faulted || s->fifo_count || s->irq_status ||
        !(s->dma_command & BIT(1))) {
        return true;
    }
    /* Transfers commit synchronously; unsupported video jobs fault. */
    while (s->sync_count) {
        if (!tegra124_host1x_syncpt_incr(s->host1x,
                                        s->sync_queue[s->sync_head])) {
            return false;
        }
        s->sync_head = (s->sync_head + 1) % VIC_SYNC_DEPTH;
        s->sync_count--;
    }
    return true;
}

static bool vic_irq_update(Tegra124VicState *s)
{
    uint32_t lines = s->fifo_count ? BIT(2) : 0;
    uint32_t active, high = s->irq_dest >> 16;

    if (!s->experimental_execution) {
        return true;
    }
    s->irq_status |= lines & ~s->irq_lines & ~s->irq_mode;
    s->irq_status = (s->irq_status & ~s->irq_mode) | (lines & s->irq_mode);
    s->irq_lines = lines;
    active = s->irq_status & s->irq_mask;
    qemu_set_irq(s->irq, !!(active & s->irq_dest & ~high));
    if (active & s->irq_dest & high) {
        qemu_log_mask(LOG_UNIMP,
                      "tegra124-vic: NRHOST interrupt unsupported\n");
        return false;
    }
    if (s->cpu_running || s->cpu_stopped || !s->clock_enabled) {
        return true;
    }
    return vic_cpu_run(s);
}

static unsigned vic_ram_size(Tegra124VicState *s)
{
    return s->experimental_execution ? VIC_EXPERIMENTAL_RAM : VIC_MEMORY_WINDOW;
}

static uint32_t vic_vtlb(Tegra124VicState *s, uint32_t address)
{
    uint32_t result = 0;
    unsigned matches = 0;

    for (unsigned i = 0; i < vic_ram_size(s) / 256; i++) {
        if (s->imem_flags[i] && s->imem_tags[i] == address >> 8) {
            result = (result & ~0xffU) | i |
                     ((uint32_t)s->imem_flags[i] << 24);
            matches++;
        }
    }
    if (!matches) {
        result |= BIT(31);
    } else if (matches > 1) {
        result |= BIT(30);
    }
    return result;
}

static bool vic_tlb_command(Tegra124VicState *s, uint32_t value)
{
    unsigned command = value >> 24, parameter = value & 0xffffff;

    if (command == 3) {
        s->tlb_result = vic_vtlb(s, parameter);
    } else if ((command == 1 || command == 2) &&
               parameter < vic_ram_size(s) / 256) {
        if (command == 1) {
            s->imem_flags[parameter] = 0;
            s->imem_tags[parameter] = 0;
        } else {
            s->tlb_result = (s->imem_tags[parameter] << 8) |
                           ((uint32_t)s->imem_flags[parameter] << 24);
        }
    } else {
        return false;
    }
    s->tlb_command = value;
    return true;
}

static bool vic_dma(Tegra124VicState *s, uint32_t command, uint32_t base,
                    uint32_t external, uint32_t local, bool cpu)
{
    unsigned size_code = (command >> 8) & 7;
    unsigned length;
    uint8_t buffer[256];
    uint64_t source = ((uint64_t)base << 8) + external;
    bool imem = command & BIT(4);

    if (!s->clock_enabled || s->dma_faulted || s->fb_config != 3 ||
        (command & ~0x710U) || (!cpu && size_code < 2) || size_code > 6 ||
        (imem && (size_code != 6 || external > 0xffffff))) {
        return false;
    }
    length = 4U << size_code;
    if ((source & (length - 1)) || (local & (length - 1)) ||
        source > UINT32_MAX - (length - 1) ||
        local > vic_ram_size(s) - length) {
        return false;
    }
    s->dma_command = cpu ? s->dma_command & ~BIT(1) : command;
    if (imem) {
        s->imem_tags[local / 256] = external >> 8;
        s->imem_flags[local / 256] = VIC_CODE_BUSY;
    }
    if (address_space_read(&s->dma_as, source, MEMTXATTRS_UNSPECIFIED,
                           buffer, length) != MEMTX_OK) {
        s->dma_faulted = true;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "tegra124-vic: DMA read fault at 0x%" PRIx64 "\n",
                      source);
        return false;
    }
    memcpy((imem ? s->imem : s->dmem) + local, buffer, length);
    if (imem) {
        s->imem_flags[local / 256] = VIC_CODE_USABLE;
    }
    s->dma_command |= BIT(1);
    return true;
}

static uint32_t vic_memory_read(uint8_t *memory, uint32_t *index)
{
    uint32_t result = ldl_le_p(memory + (*index & 0xfffc));

    if (*index & BIT(25)) {
        *index = (*index & ~0xfffcU) | ((*index + 4) & 0xfffc);
    }
    return result;
}

static MemTxResult vic_read(void *opaque, hwaddr addr, uint64_t *value,
                            unsigned size, MemTxAttrs attrs)
{
    Tegra124VicState *s = opaque;

    if (s->reset_asserted) {
        goto unsupported;
    }
    switch (addr) {
    case 0x1008:
    case 0x100c:
    case 0x1038:
    case 0x1040:
    case 0x1044:
    case 0x1080:
    case 0x1084:
    case 0x1108:
        if (!s->experimental_execution) {
            break;
        }
        if (addr == 0x1108) {
            *value = (VIC_EXPERIMENTAL_RAM / 256) * (1 + BIT(9));
        } else if (addr == 0x1008) {
            *value = s->irq_status;
        } else if (addr == 0x100c) {
            *value = s->irq_mode;
        } else if (addr == 0x1038) {
            *value = 0;
        } else {
            *value = s->scratch[(addr & 4) / 4 + (addr >= 0x1080 ? 2 : 0)];
        }
        return MEMTX_OK;
    case 0x1018:
        *value = s->irq_mask;
        return MEMTX_OK;
    case 0x101c:
        *value = s->irq_dest;
        return MEMTX_OK;
    case 0x1048:
        *value = s->interface_enable;
        return MEMTX_OK;
    case 0x104c:
        /* Only idle is established; busy bit layouts vary by engine. */
        if (!s->experimental_execution || s->cpu_faulted || s->dma_faulted ||
            !(s->dma_command & BIT(1)) || s->fifo_count ||
            (!s->cpu_stopped && !s->cpu_sleeping)) {
            break;
        }
        *value = 0;
        return MEMTX_OK;
    case 0x1064:
    case 0x1068:
        if (!s->experimental_execution || !s->fifo_count) {
            break;
        }
        *value = addr == 0x1064 ? s->fifo_data[s->fifo_head] :
                                 s->fifo_method[s->fifo_head];
        return MEMTX_OK;
    case 0x1100:
        *value = (s->cpu_stopped ? BIT(4) : 0) |
                 (s->cpu_sleeping ? BIT(5) : 0);
        return MEMTX_OK;
    case 0x1104:
        *value = s->bootvec;
        return MEMTX_OK;
    case 0x110c: /* RAM is cleared by reset. */
        *value = 0;
        return MEMTX_OK;
    case 0x1110:
        *value = s->dma_base;
        return MEMTX_OK;
    case 0x1114:
        *value = s->dma_offset;
        return MEMTX_OK;
    case 0x1118:
        *value = s->dma_command;
        return MEMTX_OK;
    case 0x111c:
        *value = s->dma_fb_offset;
        return MEMTX_OK;
    case 0x1140:
        *value = s->tlb_command;
        return MEMTX_OK;
    case 0x1144:
        *value = s->tlb_result;
        return MEMTX_OK;
    case 0x1180:
        *value = s->imem_index;
        return MEMTX_OK;
    case 0x1184:
        if ((s->imem_index & 0xfffc) >= vic_ram_size(s)) {
            break;
        }
        *value = vic_memory_read(s->imem, &s->imem_index);
        return MEMTX_OK;
    case 0x1188:
        if ((s->imem_index & 0xfffc) >= vic_ram_size(s)) {
            break;
        }
        *value = s->imem_tags[(s->imem_index & 0xfffc) / 256];
        return MEMTX_OK;
    case 0x11c0:
        *value = s->dmem_index;
        return MEMTX_OK;
    case 0x11c4:
        if ((s->dmem_index & 0xfffc) >= vic_ram_size(s)) {
            break;
        }
        *value = vic_memory_read(s->dmem, &s->dmem_index);
        return MEMTX_OK;
    case 0x1604:
        *value = s->fb_config;
        return MEMTX_OK;
    }
unsupported:
    qemu_log_mask(LOG_UNIMP, "tegra124-vic: unsupported read at 0x%"
                  HWADDR_PRIx "%s\n", addr,
                  s->reset_asserted ? " during reset" : "");
    return MEMTX_ERROR;
}

static MemTxResult vic_write(void *opaque, hwaddr addr, uint64_t value,
                             unsigned size, MemTxAttrs attrs)
{
    Tegra124VicState *s = opaque;

    if (s->reset_asserted) {
        goto unsupported;
    }
    switch (addr) {
    case 0:
        if (!s->experimental_execution || !s->clock_enabled ||
            (value & ~0x1ffU) || (value & 255) >= 192 ||
            s->cpu_faulted || s->dma_faulted) {
            break;
        }
        if (!(value & BIT(8))) {
            if (tegra124_host1x_syncpt_incr(s->host1x, value)) {
                return MEMTX_OK;
            }
            break;
        }
        if (s->sync_count == VIC_SYNC_DEPTH) {
            break;
        }
        s->sync_queue[(s->sync_head + s->sync_count) % VIC_SYNC_DEPTH] =
            value & 255;
        s->sync_count++;
        if (vic_syncpt_update(s)) {
            return MEMTX_OK;
        }
        break;
    case 0x40:
        if (!s->experimental_execution || value > 0x7ff) {
            break;
        }
        s->method_offset = value;
        return MEMTX_OK;
    case 0x44:
        if (!s->experimental_execution || !s->clock_enabled ||
            !(s->interface_enable & BIT(1)) || s->cpu_faulted ||
            s->dma_faulted || s->fifo_count == VIC_FIFO_DEPTH) {
            break;
        }
        {
            unsigned tail = (s->fifo_head + s->fifo_count) % VIC_FIFO_DEPTH;

            s->fifo_method[tail] = s->method_offset;
            s->fifo_data[tail] = value;
            s->fifo_count++;
        }
        if (vic_irq_update(s)) {
            return MEMTX_OK;
        }
        break;
    case 0x1000:
    case 0x1004:
    case 0x100c:
        if (!s->experimental_execution || (value & ~0xffffU)) {
            break;
        }
        if (addr == 0x1000) {
            s->irq_status |= value & ~s->irq_mode;
        } else if (addr == 0x1004) {
            s->irq_status &= ~(value & ~s->irq_mode);
        } else {
            s->irq_mode = value;
        }
        if (vic_irq_update(s)) {
            return MEMTX_OK;
        }
        break;
    case 0x1038:
    case 0x1040:
    case 0x1044:
    case 0x1080:
    case 0x1084:
        if (!s->experimental_execution) {
            break;
        }
        if (addr == 0x1038) {
            if (value) {
                break;
            }
        } else {
            s->scratch[(addr & 4) / 4 + (addr >= 0x1080 ? 2 : 0)] = value;
        }
        return MEMTX_OK;
    case 0x1010:
    case 0x1014:
        if (value & ~0xffffU) {
            break;
        }
        if (addr == 0x1010) {
            s->irq_mask |= value;
        } else {
            s->irq_mask &= ~value;
        }
        if (vic_irq_update(s)) {
            return MEMTX_OK;
        }
        break;
    case 0x101c:
        s->irq_dest = value;
        if (vic_irq_update(s)) {
            return MEMTX_OK;
        }
        break;
    case 0x1048:
        if (value & ~3U) {
            break;
        }
        s->interface_enable = value;
        return MEMTX_OK;
    case 0x1100:
        if (value == 2 && s->experimental_execution &&
            s->clock_enabled && s->cpu_stopped &&
            !s->cpu_faulted && !s->dma_faulted) {
            s->pc = s->bootvec;
            s->cpu_stopped = false;
            if (vic_cpu_run(s)) {
                return MEMTX_OK;
            }
        }
        if (!value) {
            return MEMTX_OK;
        }
        break;
    case 0x1074:
        if (!s->experimental_execution || value != 1 || !s->fifo_count) {
            break;
        }
        s->fifo_head = (s->fifo_head + 1) % VIC_FIFO_DEPTH;
        s->fifo_count--;
        if (vic_irq_update(s)) {
            return MEMTX_OK;
        }
        break;
    case 0x110c:
        if (!value) {
            return MEMTX_OK;
        }
        break;
    case 0x1104:
        s->bootvec = value;
        return MEMTX_OK;
    case 0x1110:
        s->dma_base = value;
        return MEMTX_OK;
    case 0x1114:
        if (value > 0xffff) {
            break;
        }
        s->dma_offset = value;
        return MEMTX_OK;
    case 0x1118:
        if (vic_dma(s, value, s->dma_base, s->dma_fb_offset,
                    s->dma_offset, false)) {
            return MEMTX_OK;
        }
        break;
    case 0x111c:
        s->dma_fb_offset = value;
        return MEMTX_OK;
    case 0x1140:
        if (vic_tlb_command(s, value)) {
            return MEMTX_OK;
        }
        break;
    case 0x1180:
    case 0x11c0:
        if ((value & ~VIC_PORT_MASK) ||
            (value & 0xfffc) >= vic_ram_size(s)) {
            break;
        }
        if (addr == 0x1180) {
            s->imem_index = value;
        } else {
            s->dmem_index = value;
        }
        return MEMTX_OK;
    case 0x11c4:
        if ((s->dmem_index & 0xfffc) >= vic_ram_size(s)) {
            break;
        }
        stl_le_p(s->dmem + (s->dmem_index & 0xfffc), value);
        if (s->dmem_index & BIT(24)) {
            s->dmem_index = (s->dmem_index & ~0xfffcU) |
                            ((s->dmem_index + 4) & 0xfffc);
        }
        return MEMTX_OK;
    case 0x1604:
        if (value & ~3U) {
            break;
        }
        s->fb_config = value;
        return MEMTX_OK;
    }
unsupported:
    qemu_log_mask(LOG_UNIMP, "tegra124-vic: unsupported write at 0x%"
                  HWADDR_PRIx " = 0x%" PRIx64 "%s\n", addr, value,
                  s->reset_asserted ? " during reset" : "");
    return MEMTX_ERROR;
}

static bool vic_fetch(Tegra124VicState *s, uint32_t address, uint8_t *byte)
{
    uint32_t mapping = vic_vtlb(s, address);

    if (address > 0xffffff || (mapping & (BIT(30) | BIT(31))) ||
        !(mapping & (VIC_CODE_USABLE << 24))) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "tegra124-vic: fetch fault at 0x%x, mapping 0x%x\n",
                      address, mapping);
        return false;
    }
    *byte = s->imem[(mapping & 255) * 256 + (address & 255)];
    return true;
}

static bool vic_data(Tegra124VicState *s, uint32_t address, unsigned bytes,
                     uint32_t *value, bool store)
{
    if ((address & (bytes - 1)) || address > vic_ram_size(s) - bytes) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "tegra124-vic: data fault at 0x%x, size %u\n",
                      address, bytes);
        return false;
    }
    if (store) {
        for (unsigned i = 0; i < bytes; i++) {
            s->dmem[address + i] = *value >> (i * 8);
        }
    } else {
        *value = 0;
        for (unsigned i = 0; i < bytes; i++) {
            *value |= (uint32_t)s->dmem[address + i] << (i * 8);
        }
    }
    return true;
}

static void vic_push(Tegra124VicState *s, uint32_t value)
{
    s->special[4] = (s->special[4] - 4) & (VIC_EXPERIMENTAL_RAM - 4);
    stl_le_p(s->dmem + s->special[4], value);
}

static uint32_t vic_pop(Tegra124VicState *s)
{
    uint32_t value = ldl_le_p(s->dmem + s->special[4]);

    s->special[4] = (s->special[4] + 4) & (VIC_EXPERIMENTAL_RAM - 4);
    return value;
}

static void vic_flags(Tegra124VicState *s, uint32_t mask, uint32_t value)
{
    s->special[8] = (s->special[8] & ~mask) | (value & mask);
}

static uint32_t vic_sz(uint32_t value, unsigned bits)
{
    uint32_t mask = UINT32_MAX >> (32 - bits);

    return (!!(value & BIT(bits - 1)) << 10) | (!(value & mask) << 11);
}

static void vic_xbit(Tegra124VicState *s, unsigned dst, uint32_t source,
                     uint32_t index)
{
    uint32_t value = (source >> (index & 31)) & 1;

    s->regs[dst] = value;
    vic_flags(s, 0xc00, !value << 11);
}

static bool vic_alu(Tegra124VicState *s, unsigned op, unsigned bits,
                    unsigned dst, uint32_t a, uint32_t b, bool compare)
{
    uint32_t mask = UINT32_MAX >> (32 - bits), sign = BIT(bits - 1);
    uint32_t result, flags;
    bool carry = false, overflow = false;

    a &= mask;
    b &= mask;
    if (compare) {
        result = (a - b) & mask;
        carry = op == 5 ? (a ^ sign) < (b ^ sign) : a < b;
        overflow = ((a ^ b) & (a ^ result) & sign) != 0;
        flags = vic_sz(result, bits) | (carry << 8) | (overflow << 9);
        vic_flags(s, op == 6 ? 0xf00 : 0x900, flags);
        return true;
    }
    if (op <= 3) {
        unsigned cin = (op & 1) && (s->special[8] & BIT(8));
        uint64_t operand = (uint64_t)b + cin;

        if (op & 2) {
            result = (a - operand) & mask;
            carry = a < operand;
            overflow = ((a ^ b) & (a ^ result) & sign) != 0;
        } else {
            uint64_t sum = a + operand;

            result = sum & mask;
            carry = sum > mask;
            overflow = (~(a ^ b) & (a ^ result) & sign) != 0;
        }
    } else if (op == 4 || op == 5 || op == 7) {
        unsigned shift = b & (bits - 1);

        result = a;
        if (shift) {
            if (op == 4) {
                carry = (a >> (bits - shift)) & 1;
                result = (a << shift) & mask;
            } else {
                carry = (a >> (shift - 1)) & 1;
                result = a >> shift;
                if (op == 7 && (a & sign)) {
                    result |= mask ^ (mask >> shift);
                }
            }
        }
    } else {
        return false;
    }
    s->regs[dst] = (s->regs[dst] & ~mask) | result;
    vic_flags(s, 0xf00,
              vic_sz(result, bits) | (carry << 8) | (overflow << 9));
    return true;
}

static bool vic_condition(Tegra124VicState *s, unsigned condition, bool *taken)
{
    uint32_t f = s->special[8];
    bool carry = f & BIT(8), zero = f & BIT(11);
    bool less = !!(f & BIT(10)) != !!(f & BIT(9));

    if ((condition & 15) <= 11) {
        *taken = !!(f & BIT(condition & 15)) ^ !!(condition & 16);
    } else {
        switch (condition) {
        case 12:
            *taken = !carry && !zero;
            break;
        case 13:
            *taken = carry || zero;
            break;
        case 14:
            *taken = true;
            break;
        case 28:
            *taken = !less && !zero;
            break;
        case 29:
            *taken = less || zero;
            break;
        case 30:
            *taken = less;
            break;
        case 31:
            *taken = !less;
            break;
        default:
            return false;
        }
    }
    return true;
}

static bool vic_cpu_io(Tegra124VicState *s, uint32_t address,
                       uint32_t *value, bool store)
{
    uint64_t data;
    hwaddr host = 0x1000 + (address >> 6);

    /* Only unindexed, old-style Falcon IO is understood here. */
    if ((address & 255) || address >= 0x3c000 || host == 0x1100) {
        qemu_log_mask(LOG_UNIMP, "tegra124-vic: unsupported CPU IO 0x%x\n",
                      address);
        return false;
    }
    if (store) {
        return vic_write(s, host, *value, 4,
                         MEMTXATTRS_UNSPECIFIED) == MEMTX_OK;
    }
    if (vic_read(s, host, &data, 4, MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        return false;
    }
    *value = data;
    return true;
}

static uint32_t vic_bitop(uint32_t value, unsigned op, uint32_t index)
{
    uint32_t bit = BIT(index & 31);

    return op == 9 ? value | bit : op == 10 ? value & ~bit : value ^ bit;
}

static bool vic_step(Tegra124VicState *s)
{
    uint8_t b[6] = { 0 };
    unsigned length = 2, op, kind, r1, r2, sub, bits, dst;
    uint32_t next, immediate, a, value, mask, address;
    bool taken;

    if (!vic_fetch(s, s->pc, &b[0]) || !vic_fetch(s, s->pc + 1, &b[1])) {
        return false;
    }
    op = b[0];
    kind = op & 63;
    r1 = b[1] >> 4;
    r2 = b[1] & 15;
    if ((op & 0xf0) == 0 || (op & 0xf0) == 0x40 ||
        (op & 0xf0) == 0x80 || (op & 0xf0) == 0xd0) {
        length = op >= 0xd0 ? 5 : 2 + (op >> 6);
    } else if (op == 0x3e || op == 0x7e) {
        length = 4;
    } else if (op < 0xc0) {
        if ((kind >= 0x10 && kind <= 0x18) || kind == 0x35 ||
            kind == 0x39 || kind == 0x3b || kind == 0x3c) {
            length = 3;
        } else if (kind == 0x30 || kind == 0x31 ||
                   kind == 0x36 || kind == 0x37) {
            length = 3 + (op & 1);
        } else if (kind == 0x33) {
            if (r2 != 0 && r2 != 4 && r2 != 9 && r2 != 10 && r2 != 11 &&
                r2 != 13 && r2 != 14 && r2 != 15) {
                return false;
            }
            length = r2 & 8 ? 5 + ((r2 & 3) == 3) : 4;
        } else if (kind == 0x38) {
            length = 5;
        }
    } else if (op < 0xf0) {
        length = op < 0xe0 ? 3 : 4;
    } else if (op == 0xf0 || op == 0xf1 || op == 0xf4 || op == 0xf5) {
        length = 3 + (op & 1);
    } else if (op == 0xf6 || op == 0xfa || op >= 0xfd) {
        length = 3;
    } else if (op == 0xfb) {
        unsigned pop = r2 & 7;

        if (pop >= 6) {
            return false;
        }
        length = pop >= 4 ? 3 : pop >= 2 ? 4 : 2;
    }
    for (unsigned i = 2; i < length; i++) {
        if (!vic_fetch(s, s->pc + i, &b[i])) {
            return false;
        }
    }
    next = s->pc + length;
    immediate = b[2] | ((uint32_t)b[3] << 8);
    bits = 8U << (op >> 6);
    mask = op < 0xc0 ? UINT32_MAX >> (32 - bits) : UINT32_MAX;
    a = s->regs[r1];
    value = s->regs[r2];
    dst = r2;
    sub = b[2] & 15;

    if ((op & 0xf0) == 0 || (op & 0xf0) == 0x40 ||
        (op & 0xf0) == 0x80 || (op & 0xf0) == 0xd0) {
        value = b[1] | ((uint32_t)b[2] << 8) |
                ((uint32_t)b[3] << 16) | ((uint32_t)b[4] << 24);
        s->regs[op & 15] = sextract32(value, 0, (length - 1) * 8);
    } else if (op == 0x3e || op == 0x7e) {
        if (op == 0x7e) {
            vic_push(s, next);
        }
        next = b[1] | ((uint32_t)b[2] << 8) | ((uint32_t)b[3] << 16);
    } else if (op < 0xc0) {
        if (kind == 0x3c && (sub == 8 || sub == 9)) {
            bool store = sub == 9;
            unsigned r3 = b[2] >> 4;

            address = a + s->regs[store ? r3 : r2] * (bits / 8);
            if (!vic_data(s, address, bits / 8, &value, store)) {
                return false;
            }
            if (!store) {
                s->regs[r3] = value;
            }
        } else if ((kind >= 0x10 && kind <= 0x15) || kind == 0x17) {
            if (!vic_alu(s, kind & 15, bits, dst, a, b[2], false)) {
                return false;
            }
        } else if (kind == 0x36 || kind == 0x37 || kind == 0x38 ||
                   kind == 0x3b || kind == 0x3c) {
            unsigned alu = r2;
            uint32_t rhs = immediate;

            dst = r1;
            if (kind == 0x38) {
                alu = b[4] & 15;
                dst = r2;
                if (alu > 3) {
                    return false;
                }
            } else if (kind == 0x3b || kind == 0x3c) {
                alu = sub;
                rhs = value;
                if (kind == 0x3c) {
                    dst = b[2] >> 4;
                }
            } else if (kind == 0x37 && alu > 3) {
                return false;
            }
            if (!vic_alu(s, alu, bits, dst, a, rhs, false)) {
                return false;
            }
        } else if (kind >= 0x24 && kind <= 0x26) {
            vic_alu(s, kind & 15, bits, 0, a, value, true);
        } else if ((kind == 0x30 || kind == 0x31) && r2 >= 4 && r2 <= 6) {
            if (r2 != 4) {
                immediate = sextract32(immediate, 0, (length - 2) * 8);
            }
            vic_alu(s, r2, bits, 0, a, immediate, true);
        } else if (kind == 0x32) {
            s->regs[r2] = (value & ~mask) | (a & mask);
        } else if (kind == 0x33) {
            unsigned offset = 3 + !!(r2 & 2);
            uint32_t displacement = b[offset] |
                                    ((uint32_t)b[offset + 1] << 8);

            taken = (a & mask) == ((r2 & 2 ? immediate : b[2]) & mask);
            if (taken ^ !!(r2 & 4)) {
                next = s->pc + sextract32(displacement, 0,
                                          (length - offset) * 8);
            }
        } else if (kind == 0x3d && r2 == 4) {
            s->regs[r1] &= ~mask;
        } else if (kind == 0x18 || kind == 0x3f ||
                   kind == 0x20 || kind == 0x35) {
            bool store = kind == 0x20 || kind == 0x35;

            address = a;
            if (kind == 0x18 || kind == 0x35) {
                address += b[2] * (bits / 8);
            }
            if (!vic_data(s, address, bits / 8, &value, store)) {
                return false;
            }
            if (!store) {
                s->regs[r2] = value;
            }
        } else {
            return false;
        }
    } else if (op == 0xfe) {
        if (sub == 0) {
            if (r2 == 5 || r2 == 9 || r2 == 10 || r2 >= 13) {
                return false;
            }
            if (r2 == 4) {
                a &= VIC_EXPERIMENTAL_RAM - 4;
            } else if (r2 == 8 && (a & ~VIC_FLAGS_MASK)) {
                return false;
            }
            s->special[r2] = a;
        } else if (sub == 1) {
            if (r1 == 9 || r1 == 10 || r1 >= 13) {
                return false;
            }
            s->regs[r2] = r1 == 5 ? s->pc : s->special[r1];
        } else if (sub == 12) {
            vic_xbit(s, r2, s->special[8], a);
        } else {
            return false;
        }
    } else if (op == 0xcf || op == 0xf6 || (op == 0xfa && sub == 0)) {
        address = a + (op == 0xfa ? 0 : b[2] * 4);
        if (!vic_cpu_io(s, address, &value, op != 0xcf)) {
            return false;
        }
        if (op == 0xcf) {
            s->regs[r2] = value;
        }
    } else if (op == 0xfa && (sub == 4 || sub == 5)) {
        bool code = sub == 4;
        unsigned context = (s->special[11] >> (code ? 0 : 8)) & 7;
        uint32_t command = code ? 0x610 : ((value >> 16) & 7) << 8;

        if (context || !vic_dma(s, command, s->special[code ? 6 : 7],
                                a, value & 0xffff, true)) {
            return false;
        }
    } else if (op == 0xf8) {
        if (r2 == 0) {
            next = vic_pop(s);
        } else if (r2 == 1 && s->cpu_in_interrupt) {
            next = vic_pop(s);
            vic_flags(s, VIC_FLAGS_IE,
                      (s->special[8] >> 4) & VIC_FLAGS_IE);
            s->cpu_in_interrupt = false;
        } else if (r2 == 2) {
            s->cpu_stopped = true;
            s->irq_status |= BIT(4) & ~s->irq_mode;
            if (!vic_irq_update(s)) {
                return false;
            }
        } else if ((r2 != 3 && r2 != 7) || s->dma_faulted) {
            return false;
        }
    } else if (op == 0xf9 && r2 == 0) {
        vic_push(s, a);
    } else if (op == 0xf9 && r2 == 2) {
        for (unsigned i = 0; i <= r1; i++) {
            vic_push(s, s->regs[i]);
        }
    } else if (op == 0xf9 && r2 == 5) {
        vic_push(s, next);
        next = a;
    } else if (op == 0xf9 && r2 >= 9 && r2 <= 11) {
        if (!(BIT(a & 31) & VIC_FLAGS_MASK)) {
            return false;
        }
        s->special[8] = vic_bitop(s->special[8], r2, a);
    } else if (op == 0xfb) {
        unsigned pop = r2 & 7;

        for (unsigned i = r1 + 1; i > 0; i--) {
            s->regs[i - 1] = vic_pop(s);
        }
        if (pop >= 2) {
            s->special[4] = (s->special[4] +
                            sextract32(immediate, 0, (length - 2) * 8)) &
                            (VIC_EXPERIMENTAL_RAM - 4);
        }
        if (pop & 1) {
            next = vic_pop(s);
        }
    } else if (op == 0xfc && r2 == 0) {
        s->regs[r1] = vic_pop(s);
    } else if (op == 0xf4 || op == 0xf5) {
        unsigned condition = b[1];

        if (condition < 32) {
            if (!vic_condition(s, condition, &taken)) {
                return false;
            }
            if (taken) {
                next = s->pc + sextract32(immediate, 0, (length - 2) * 8);
            }
        } else if (condition == 0x30) {
            s->special[4] = (s->special[4] +
                            sextract32(immediate, 0, (length - 2) * 8)) &
                            (VIC_EXPERIMENTAL_RAM - 4);
        } else if (op == 0xf4 && condition == 0x28) {
            if (b[2] > 31 || !(BIT(b[2]) & VIC_FLAGS_MASK)) {
                return false;
            }
            if (s->special[8] & BIT(b[2])) {
                s->cpu_sleeping = true;
                next = s->pc;
            }
        } else if (op == 0xf4 && condition >= 0x31 && condition <= 0x33) {
            if (!(BIT(b[2] & 31) & VIC_FLAGS_MASK)) {
                return false;
            }
            s->special[8] = vic_bitop(s->special[8], condition - 0x28, b[2]);
        } else {
            return false;
        }
    } else {
        unsigned operation;
        uint32_t rhs;

        if (op == 0xf0 || op == 0xf1) {
            operation = r2;
            dst = r1;
            rhs = immediate;
        } else if ((op & 0xf0) == 0xc0 || (op & 0xf0) == 0xe0) {
            operation = op & 15;
            rhs = immediate;
        } else if (op == 0xfd || op == 0xff) {
            operation = sub;
            rhs = value;
            dst = op == 0xfd ? r1 : b[2] >> 4;
        } else {
            return false;
        }
        if (op == 0xf0 && operation == 12) {
            vic_xbit(s, dst, s->special[8], rhs);
        } else if ((op == 0xf0 || op == 0xfd) &&
                   operation >= 9 && operation <= 11) {
            s->regs[dst] = vic_bitop(a, operation, rhs);
        } else if ((op == 0xc8 || op == 0xff) && operation == 8) {
            vic_xbit(s, dst, a, rhs);
        } else if ((op == 0xf0 || op == 0xf1) && operation == 3) {
            s->regs[dst] = (a & 0xffff) | (rhs << 16);
        } else if (operation >= 4 && operation <= 6) {
            value = operation == 4 ? a & rhs :
                    operation == 5 ? a | rhs : a ^ rhs;
            s->regs[dst] = value;
            vic_flags(s, 0xf00, vic_sz(value, 32));
        } else if ((op == 0xc7 || op == 0xe7 || op == 0xff) &&
                   operation == 7) {
            unsigned width = ((rhs >> 5) & 31) + 1;

            value = (a >> (rhs & 31)) & (UINT32_MAX >> (32 - width));
            s->regs[dst] = value;
            vic_flags(s, 0xc00, !value << 11);
        } else {
            return false;
        }
    }
    s->pc = next;
    return true;
}

static bool vic_cpu_interrupt(Tegra124VicState *s)
{
    uint32_t active = s->irq_status & s->irq_mask & ~s->irq_dest;
    uint32_t high = s->irq_dest >> 16;
    bool iv0 = (active & ~high) && (s->special[8] & BIT(16));
    bool iv1 = (active & high) && (s->special[8] & BIT(17));

    if (s->cpu_stopped || (!iv0 && !iv1)) {
        return true;
    }
    if ((iv0 && iv1) || s->cpu_in_interrupt) {
        qemu_log_mask(LOG_UNIMP,
                      "tegra124-vic: simultaneous/nested interrupt unsupported\n");
        return false;
    }
    vic_push(s, s->pc);
    vic_flags(s, VIC_FLAGS_IE | (VIC_FLAGS_IE << 4),
              (s->special[8] & VIC_FLAGS_IE) << 4);
    s->pc = s->special[iv1 ? 1 : 0];
    s->cpu_sleeping = false;
    s->cpu_in_interrupt = true;
    return true;
}

static bool vic_cpu_run(Tegra124VicState *s)
{
    if (s->cpu_faulted || s->dma_faulted) {
        return false;
    }
    s->cpu_running = true;
    for (unsigned i = 0; i < VIC_CPU_BUDGET; i++) {
        if (!vic_cpu_interrupt(s) ||
            (!s->cpu_sleeping && !s->cpu_stopped && !vic_step(s)) ||
            ((s->cpu_sleeping || s->cpu_stopped) &&
             !vic_cpu_interrupt(s))) {
            uint32_t mapping = vic_vtlb(s, s->pc);
            uint32_t physical = (mapping & 255) * 256 + (s->pc & 255);

            qemu_log_mask(LOG_UNIMP,
                          "tegra124-vic: CPU fault at PC 0x%x, TLB 0x%x,"
                          " after %u instructions\n", s->pc, mapping, i);
            if (!(mapping & (BIT(30) | BIT(31))) &&
                (mapping & (VIC_CODE_USABLE << 24))) {
                qemu_log_mask(LOG_UNIMP, "tegra124-vic: opcode 0x%02x\n",
                              s->imem[physical]);
            }
            s->cpu_faulted = true;
            s->cpu_running = false;
            return false;
        }
        if (s->cpu_stopped || s->cpu_sleeping) {
            s->cpu_running = false;
            return vic_syncpt_update(s);
        }
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "tegra124-vic: CPU instruction budget exhausted at PC 0x%x\n",
                  s->pc);
    s->cpu_faulted = true;
    s->cpu_running = false;
    return false;
}

static const MemoryRegionOps vic_ops = {
    .read_with_attrs = vic_read,
    .write_with_attrs = vic_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void vic_reset(DeviceState *dev)
{
    Tegra124VicState *s = TEGRA124_VIC(dev);

    memset(s->imem, 0, sizeof(s->imem));
    memset(s->dmem, 0, sizeof(s->dmem));
    memset(s->imem_tags, 0, sizeof(s->imem_tags));
    memset(s->imem_flags, 0, sizeof(s->imem_flags));
    s->tlb_command = s->tlb_result = 0;
    s->imem_index = s->dmem_index = 0;
    s->dma_base = s->dma_offset = s->dma_fb_offset = 0;
    s->dma_command = BIT(1);
    s->dma_faulted = false;
    s->fb_config = s->irq_mask = s->irq_dest = 0;
    s->interface_enable = s->bootvec = 0;
    s->cpu_stopped = true;
    s->cpu_sleeping = false;
    s->cpu_faulted = false;
    s->cpu_running = s->cpu_in_interrupt = false;
    s->pc = s->irq_status = s->irq_lines = 0;
    s->irq_mode = 0xfc04;
    s->method_offset = s->fifo_head = s->fifo_count = 0;
    s->sync_head = s->sync_count = 0;
    memset(s->sync_queue, 0, sizeof(s->sync_queue));
    memset(s->fifo_method, 0, sizeof(s->fifo_method));
    memset(s->fifo_data, 0, sizeof(s->fifo_data));
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->special, 0, sizeof(s->special));
    memset(s->scratch, 0, sizeof(s->scratch));
    qemu_set_irq(s->irq, 0);
}

static void vic_reset_line(void *opaque, int n, int level)
{
    Tegra124VicState *s = opaque;

    if (level && !s->reset_asserted) {
        vic_reset(DEVICE(s));
    }
    s->reset_asserted = level;
}

static void vic_clock_line(void *opaque, int n, int level)
{
    Tegra124VicState *s = opaque;
    bool rising = level && !s->clock_enabled;

    s->clock_enabled = level;
    if (rising && !s->reset_asserted && !vic_irq_update(s)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "tegra124-vic: CPU resume failed on clock enable\n");
    }
}

static void vic_init(Object *obj)
{
    Tegra124VicState *s = TEGRA124_VIC(obj);

    object_property_add_uint32_ptr(obj, "cpu-pc", &s->pc, OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "cpu-sp", &s->special[4],
                                  OBJ_PROP_FLAG_READ);
    memory_region_init_io(&s->iomem, obj, &vic_ops, s,
                          TYPE_TEGRA124_VIC, 0x40000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    qdev_init_gpio_in_named(DEVICE(obj), vic_reset_line, "reset", 1);
    qdev_init_gpio_in_named(DEVICE(obj), vic_clock_line, "clock-enable", 1);
}

static void vic_realize(DeviceState *dev, Error **errp)
{
    Tegra124VicState *s = TEGRA124_VIC(dev);

    if (!s->dma_mr || !s->host1x) {
        error_setg(errp, "tegra124-vic requires DMA and HOST1X links");
        return;
    }
    address_space_init(&s->dma_as, s->dma_mr, "tegra124-vic-dma");
}

static void vic_unrealize(DeviceState *dev)
{
    address_space_destroy(&TEGRA124_VIC(dev)->dma_as);
}

static const Property vic_properties[] = {
    DEFINE_PROP_BOOL("experimental-execution", Tegra124VicState,
                     experimental_execution, false),
    DEFINE_PROP_LINK("dma", Tegra124VicState, dma_mr, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_LINK("host1x", Tegra124VicState, host1x, TYPE_TEGRA124_HOST1X,
                     Tegra124Host1xState *),
};

static void vic_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = vic_realize;
    dc->unrealize = vic_unrealize;
    device_class_set_props(dc, vic_properties);
    device_class_set_legacy_reset(dc, vic_reset);
}

static const TypeInfo vic_info = {
    .name = TYPE_TEGRA124_VIC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Tegra124VicState),
    .instance_init = vic_init,
    .class_init = vic_class_init,
};

static void vic_register_types(void)
{
    type_register_static(&vic_info);
}

type_init(vic_register_types)

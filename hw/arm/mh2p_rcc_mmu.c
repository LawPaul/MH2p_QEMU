/*
 * DRA7 IPU L2 MMU (SPRUI30H, chapters 7 and 20).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "accel/tcg/cpu-loop.h"
#include "exec/cpu-common.h"
#include "hw/arm/mh2p_rcc_mmu.h"
#include "hw/core/cpu.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/address-spaces.h"

typedef struct MH2PIPUMMU {
    SysBusDevice parent_obj;
    MemoryRegion registers;
    IOMMUMemoryRegion iommu;
    MemoryRegion fault_memory;
    AddressSpace fault_as;
    AddressSpace *bypass;
    qemu_irq irq[3];
    uint32_t sysconfig, control, ttb, lock, cam, ram;
    uint32_t irqstatus, irqenable, fault_address, fault_status, gpr;
    uint32_t tlb_cam[32], tlb_ram[32];
    uint64_t blocked_cpus;
} MH2PIPUMMU;

OBJECT_DECLARE_SIMPLE_TYPE(MH2PIPUMMU, MH2P_IPU_MMU)

#define TYPE_MH2P_IPU_IOMMU "mh2p-ipu-iommu"

static uint32_t ipu_mmu_mask(uint32_t cam)
{
    static const uint32_t masks[] = {
        0xfffff, 0xffff, 0xfff, 0xffffff,
    };

    return masks[cam & 3];
}

static void ipu_mmu_irq(MH2PIPUMMU *s)
{
    for (unsigned i = 0; i < ARRAY_SIZE(s->irq); i++) {
        qemu_set_irq(s->irq[i],
                     !(s->gpr & 1) && (s->irqstatus & s->irqenable));
    }
}

static void ipu_mmu_unmap(MH2PIPUMMU *s)
{
    IOMMUTLBEvent event = {
        .type = IOMMU_NOTIFIER_UNMAP,
        .entry = {
            .target_as = &address_space_memory,
            .iova = 0,
            .translated_addr = 0,
            .addr_mask = UINT32_MAX,
            .perm = IOMMU_NONE,
        },
    };

    memory_region_notify_iommu(&s->iommu, 0, event);
}

static void ipu_mmu_invalidate(MH2PIPUMMU *s)
{
    CPUState *cpu;

    ipu_mmu_unmap(s);
    CPU_FOREACH(cpu) {
        if (cpu->cpu_index < 64 &&
            (s->blocked_cpus & (UINT64_C(1) << cpu->cpu_index))) {
            cpu->halted = false;
            qemu_cpu_kick(cpu);
        }
    }
    s->blocked_cpus = 0;
}

static void ipu_mmu_reset(DeviceState *dev)
{
    MH2PIPUMMU *s = MH2P_IPU_MMU(dev);

    s->sysconfig = s->control = s->ttb = s->lock = s->cam = s->ram = 0;
    s->irqstatus = s->irqenable = s->fault_address = 0;
    s->fault_status = s->gpr = 0;
    memset(s->tlb_cam, 0, sizeof(s->tlb_cam));
    memset(s->tlb_ram, 0, sizeof(s->tlb_ram));
    ipu_mmu_invalidate(s);
    ipu_mmu_irq(s);
}

static IOMMUTLBEntry ipu_mmu_translate(IOMMUMemoryRegion *iommu,
                                     hwaddr addr, IOMMUAccessFlags flag,
                                     int iommu_idx)
{
    MH2PIPUMMU *s = container_of(iommu, MH2PIPUMMU, iommu);
    IOMMUTLBEntry ret = {
        .target_as = &address_space_memory,
        .iova = addr & ~0xfffULL,
        .translated_addr = addr & ~0xfffULL,
        .addr_mask = 0xfff,
        .perm = IOMMU_RW,
    };
    uint32_t cam = 0, ram = 0, descriptor, fault;
    unsigned matches = 0, victim, base;
    MemTxResult result;

    if (!(s->control & 2)) {
        ret.target_as = s->bypass ? s->bypass : &address_space_memory;
        return ret;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(s->tlb_cam); i++) {
        uint32_t entry = s->tlb_cam[i];
        uint32_t mask = ipu_mmu_mask(entry);

        if ((entry & 4) && (addr & ~mask) == (entry & ~mask)) {
            cam = entry;
            ram = s->tlb_ram[i];
            matches++;
        }
    }
    if (matches > 1) {
        fault = BIT(4);
        goto fault;
    }
    if (matches) {
        goto translated;
    }
    if (!(s->control & 4)) {
        fault = BIT(0);
        goto fault;
    }
    /* TTB is aligned to 128 bytes; the VA index supplies bits 13:2. */
    descriptor = address_space_ldl_le(&address_space_memory,
        s->ttb | ((addr >> 18) & 0x3ffc), MEMTXATTRS_UNSPECIFIED, &result);
    if (result != MEMTX_OK) {
        fault = BIT(3);
        goto fault;
    }
    switch (descriptor & 3) {
    case 1:
        descriptor = address_space_ldl_le(&address_space_memory,
            (descriptor & 0xfffffc00U) | ((addr >> 10) & 0x3fc),
            MEMTXATTRS_UNSPECIFIED, &result);
        if (result != MEMTX_OK) {
            fault = BIT(3);
            goto fault;
        }
        if (!(descriptor & 3)) {
            fault = BIT(1);
            goto fault;
        }
        cam = (descriptor & 2) ? 2 : 1;
        break;
    case 2:
        cam = (descriptor & BIT(18)) ? 3 : 0;
        break;
    default:
        fault = BIT(1);
        goto fault;
    }
    ram = descriptor & ~ipu_mmu_mask(cam);
    cam |= (addr & ~ipu_mmu_mask(cam)) | 4;
    victim = (s->lock >> 4) & 31;
    base = (s->lock >> 10) & 31;
    victim = MAX(victim, base);
    if (s->tlb_cam[victim] & 4) {
        ipu_mmu_invalidate(s);
    }
    s->tlb_cam[victim] = cam;
    s->tlb_ram[victim] = ram;
    victim = victim == 31 ? base : victim + 1;
    s->lock = (s->lock & ~0x1f0U) | (victim << 4);
translated:
    ret.addr_mask = ipu_mmu_mask(cam);
    ret.iova = addr & ~ret.addr_mask;
    ret.translated_addr = ram & ~ret.addr_mask;
    if (addr < 0x1000) {
        qemu_log_mask(CPU_LOG_INT, "mh2p-ipu-mmu: low translation addr=0x%"
                      HWADDR_PRIx " cam=0x%x ram=0x%x pa=0x%"
                      HWADDR_PRIx " control=0x%x ttb=0x%x\n",
                      addr, cam, ram, ret.translated_addr, s->control, s->ttb);
    }
    return ret;

fault:
    /* CPU TLB fills request IOMMU_NONE; latch the actual access below. */
    if (flag != IOMMU_NONE) {
        s->fault_address = addr;
        s->fault_status = 1 | ((flag & IOMMU_RO) ? 8 : 0);
        s->irqstatus |= fault;
        ipu_mmu_irq(s);
        qemu_log_mask(LOG_GUEST_ERROR, "mh2p-ipu-mmu: fault 0x%x at 0x%"
                      HWADDR_PRIx "\n", fault, addr);
    }
    ret.target_as = &s->fault_as;
    return ret;
}

static MemTxResult ipu_mmu_fault_response(MH2PIPUMMU *s, hwaddr address)
{
    if (s->gpr & 1) {
        return MEMTX_ERROR;
    }
    if (!current_cpu || current_cpu->cpu_index >= 64) {
        qemu_log_mask(LOG_UNIMP, "mh2p-ipu-mmu: unsupported stalled "
                      "non-CPU transaction at 0x%" HWADDR_PRIx "\n", address);
        return MEMTX_ERROR;
    }
    s->blocked_cpus |= UINT64_C(1) << current_cpu->cpu_index;
    current_cpu->halted = true;
    current_cpu->exception_index = EXCP_HLT;
    cpu_loop_exit_restore(current_cpu, current_cpu->mem_io_pc);
}

static MemTxResult ipu_mmu_fault_read(void *opaque, hwaddr address,
                                    uint64_t *value, unsigned size,
                                    MemTxAttrs attrs)
{
    MH2PIPUMMU *s = opaque;
    IOMMUTLBEntry entry = ipu_mmu_translate(&s->iommu, address, IOMMU_RO, 0);
    uint8_t buffer[8] = {0};
    MemTxResult result;

    *value = 0;
    if (entry.target_as == &s->fault_as) {
        return ipu_mmu_fault_response(s, address);
    }
    result = address_space_read(entry.target_as,
        entry.translated_addr | (address & entry.addr_mask),
        attrs, buffer, size);
    *value = ldn_le_p(buffer, size);
    return result;
}

static MemTxResult ipu_mmu_fault_write(void *opaque, hwaddr address,
                                     uint64_t value, unsigned size,
                                     MemTxAttrs attrs)
{
    MH2PIPUMMU *s = opaque;
    IOMMUTLBEntry entry = ipu_mmu_translate(&s->iommu, address, IOMMU_WO, 0);
    uint8_t buffer[8];

    if (entry.target_as == &s->fault_as) {
        return ipu_mmu_fault_response(s, address);
    }
    stn_le_p(buffer, size, value);
    return address_space_write(entry.target_as,
        entry.translated_addr | (address & entry.addr_mask),
        attrs, buffer, size);
}

static const MemoryRegionOps ipu_mmu_fault_ops = {
    .read_with_attrs = ipu_mmu_fault_read,
    .write_with_attrs = ipu_mmu_fault_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

static uint64_t ipu_mmu_read(void *opaque, hwaddr address, unsigned size)
{
    MH2PIPUMMU *s = opaque;
    unsigned victim = (s->lock >> 4) & 31;

    switch (address) {
    case 0x10: return s->sysconfig;
    case 0x14: return 1;
    case 0x18: return s->irqstatus;
    case 0x1c: return s->irqenable;
    case 0x40: return 0;
    case 0x44: return s->control;
    case 0x48: return s->fault_address;
    case 0x4c: return s->ttb;
    case 0x50: return s->lock;
    case 0x54: case 0x60: case 0x64: return 0;
    case 0x58: return s->cam;
    case 0x5c: return s->ram;
    case 0x68: return s->tlb_cam[victim];
    case 0x6c: return s->tlb_ram[victim];
    case 0x84: return s->fault_status;
    case 0x88: return s->gpr;
    }
    qemu_log_mask(LOG_UNIMP, "mh2p-ipu-mmu: unsupported read at 0x%"
                  HWADDR_PRIx " size %u\n", address, size);
    return 0;
}

static void ipu_mmu_write(void *opaque, hwaddr address,
                          uint64_t value, unsigned size)
{
    MH2PIPUMMU *s = opaque;
    unsigned victim = (s->lock >> 4) & 31;

    qemu_log_mask(CPU_LOG_INT, "mh2p-ipu-mmu: register write addr=0x%"
                  HWADDR_PRIx " value=0x%" PRIx64 " size=%u\n",
                  address, value, size);
    switch (address) {
    case 0x10:
        if (value & ~0x1bU || (value & 0x18) == 0x18) {
            break;
        }
        if (value & 2) {
            ipu_mmu_reset(DEVICE(s));
        } else {
            s->sysconfig = value;
        }
        return;
    case 0x18:
        if (value & ~0x1fU) {
            break;
        }
        s->irqstatus &= ~value;
        ipu_mmu_irq(s);
        ipu_mmu_invalidate(s);
        return;
    case 0x1c:
        if (value & ~0x1fU) {
            break;
        }
        s->irqenable = value;
        ipu_mmu_irq(s);
        return;
    case 0x44:
        if (value & ~0xeU) {
            break;
        }
        s->control = value;
        ipu_mmu_invalidate(s);
        return;
    case 0x4c:
        if (value & 0x7f) {
            break;
        }
        s->ttb = value;
        return;
    case 0x50:
        if (value & ~0x7df0U) {
            break;
        }
        s->lock = value;
        return;
    case 0x58:
        if (value & 0xff0) {
            break;
        }
        s->cam = value;
        return;
    case 0x5c:
        if (value & 0xfff) {
            break;
        }
        s->ram = value;
        return;
    case 0x54:
        if (value & ~1U) {
            break;
        }
        if (value & 1) {
            s->tlb_cam[victim] = s->cam;
            s->tlb_ram[victim] = s->ram;
            ipu_mmu_invalidate(s);
        }
        return;
    case 0x60:
    case 0x64:
        if (value & ~1U) {
            break;
        }
        if (value & 1) {
            for (unsigned i = 0; i < ARRAY_SIZE(s->tlb_cam); i++) {
                uint32_t cam = s->tlb_cam[i];
                uint32_t mask = ipu_mmu_mask(cam);

                if (address == 0x60 ? !(cam & 8) :
                    (cam & ~mask) == (s->cam & ~mask)) {
                    s->tlb_cam[i] &= ~4U;
                }
            }
            ipu_mmu_invalidate(s);
        }
        return;
    case 0x84:
        if (value & ~1U) {
            break;
        }
        s->fault_status &= ~value;
        return;
    case 0x88:
        if (value & ~0xffff0001U) {
            break;
        }
        s->gpr = value;
        ipu_mmu_irq(s);
        ipu_mmu_invalidate(s);
        return;
    }
    qemu_log_mask(LOG_UNIMP, "mh2p-ipu-mmu: unsupported write at 0x%"
                  HWADDR_PRIx " = 0x%" PRIx64 " size %u\n",
                  address, value, size);
}

static const MemoryRegionOps ipu_mmu_ops = {
    .read = ipu_mmu_read,
    .write = ipu_mmu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

MemoryRegion *mh2p_ipu_mmu_memory(DeviceState *dev, AddressSpace *bypass)
{
    MH2PIPUMMU *s = MH2P_IPU_MMU(dev);

    s->bypass = bypass;
    return MEMORY_REGION(&s->iommu);
}

static int ipu_mmu_post_load(void *opaque, int version_id)
{
    MH2PIPUMMU *s = opaque;

    ipu_mmu_unmap(s);
    ipu_mmu_irq(s);
    return 0;
}

static const VMStateDescription ipu_mmu_vmstate = {
    .name = TYPE_MH2P_IPU_MMU,
    .version_id = 2,
    .minimum_version_id = 1,
    .post_load = ipu_mmu_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(sysconfig, MH2PIPUMMU),
        VMSTATE_UINT32(control, MH2PIPUMMU),
        VMSTATE_UINT32(ttb, MH2PIPUMMU),
        VMSTATE_UINT32(lock, MH2PIPUMMU),
        VMSTATE_UINT32(cam, MH2PIPUMMU),
        VMSTATE_UINT32(ram, MH2PIPUMMU),
        VMSTATE_UINT32(irqstatus, MH2PIPUMMU),
        VMSTATE_UINT32(irqenable, MH2PIPUMMU),
        VMSTATE_UINT32(fault_address, MH2PIPUMMU),
        VMSTATE_UINT32(fault_status, MH2PIPUMMU),
        VMSTATE_UINT32(gpr, MH2PIPUMMU),
        VMSTATE_UINT32_ARRAY(tlb_cam, MH2PIPUMMU, 32),
        VMSTATE_UINT32_ARRAY(tlb_ram, MH2PIPUMMU, 32),
        VMSTATE_UINT64_V(blocked_cpus, MH2PIPUMMU, 2),
        VMSTATE_END_OF_LIST()
    },
};

static void ipu_mmu_init(Object *obj)
{
    MH2PIPUMMU *s = MH2P_IPU_MMU(obj);

    memory_region_init_io(&s->registers, obj, &ipu_mmu_ops, s,
                          TYPE_MH2P_IPU_MMU, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->registers);
    for (unsigned i = 0; i < ARRAY_SIZE(s->irq); i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq[i]);
    }
    memory_region_init_iommu(&s->iommu, sizeof(s->iommu),
                             TYPE_MH2P_IPU_IOMMU, obj, "ipu1-iommu",
                             UINT64_C(1) << 32);
    memory_region_init_io(&s->fault_memory, obj, &ipu_mmu_fault_ops, s,
                          "ipu1-mmu-fault", UINT64_C(1) << 32);
    /* A stalled CPU exits the callback without unwinding MMIO dispatch. */
    s->fault_memory.disable_reentrancy_guard = true;
    address_space_init(&s->fault_as, &s->fault_memory, "ipu1-mmu-fault");
}

static void ipu_mmu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, ipu_mmu_reset);
    dc->vmsd = &ipu_mmu_vmstate;
    dc->user_creatable = false;
}

static void ipu_mmu_finalize(Object *obj)
{
    MH2PIPUMMU *s = MH2P_IPU_MMU(obj);

    address_space_destroy(&s->fault_as);
}

static int ipu_iommu_attrs_to_index(IOMMUMemoryRegion *iommu, MemTxAttrs attrs)
{
    return 0;
}

static void ipu_iommu_class_init(ObjectClass *klass, const void *data)
{
    IOMMU_MEMORY_REGION_CLASS(klass)->translate = ipu_mmu_translate;
    IOMMU_MEMORY_REGION_CLASS(klass)->attrs_to_index = ipu_iommu_attrs_to_index;
}

static const TypeInfo ipu_mmu_types[] = {
    {
        .name = TYPE_MH2P_IPU_IOMMU,
        .parent = TYPE_IOMMU_MEMORY_REGION,
        .class_init = ipu_iommu_class_init,
    }, {
        .name = TYPE_MH2P_IPU_MMU,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MH2PIPUMMU),
        .instance_init = ipu_mmu_init,
        .instance_finalize = ipu_mmu_finalize,
        .class_init = ipu_mmu_class_init,
    },
};

DEFINE_TYPES(ipu_mmu_types)

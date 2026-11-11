/*
 * MH2P / Alpine T124 direct-start experiment, not a complete Tegra K1 model.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qobject/qdict.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "hw/arm/boot.h"
#include "hw/block/flash.h"
#include "hw/char/serial-mm.h"
#include "hw/core/boards.h"
#include "hw/core/irq.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/sysbus.h"
#include "hw/cpu/a15mpcore.h"
#include "hw/gpio/tegra30_gpio.h"
#include "hw/i2c/tegra30_i2c.h"
#include "hw/ssi/tegra124_spi.h"
#include "hw/misc/tegra30_snor.h"
#include "hw/misc/tegra124_host1x.h"
#include "hw/misc/tegra124_dc.h"
#include "hw/misc/tegra124_hdmi.h"
#include "hw/misc/tegra124_sor.h"
#include "hw/misc/tegra124_vic.h"
#include "hw/misc/tegra124_se.h"
#include "hw/misc/gk20a.h"
#include "hw/misc/glbridge.h"
#include "hw/misc/mh2p_pcie_sideband.h"
#include "hw/misc/mh2p_pcie_config.h"
#include "hw/sd/sd.h"
#include "hw/sd/sdhci.h"
#include "hw/timer/tegra30_timer.h"
#include "system/reset.h"
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "target/arm/cpu.h"
#include "target/arm/internals.h"
#include "trace.h"

#define MH2P_RAM_BASE 0x80000000ULL
#define MH2P_STARTUP  0x80008000ULL
#define MH2P_DTB      0x82000000ULL
#define MH2P_ATAGS    0x80000100ULL
#define MH2P_PERIPHERAL_POWERGATES (BIT(2) | BIT(3) | BIT(4) | BIT(6) | \
                                   BIT(7) | BIT(8) | (0x7fU << 17))
#define MH2P_BOOT_POWERGATES (BIT(0) | BIT(14) | BIT(15) | BIT(24))
#define MH2P_MC_INTERRUPTS 0x13f40U

static const struct {
    hwaddr base;
    unsigned irq, reset_bank, reset_bit;
} mh2p_i2c[] = {
    { 0x7000c000, 38, 0, 12 },
    { 0x7000c400, 84, 1, 22 },
    { 0x7000c500, 92, 2, 3 },
    { 0x7000c700, 120, 6, 7 },
    { 0x7000d000, 53, 1, 15 },
    { 0x7000d100, 63, 10, 6 },
};

static const struct {
    hwaddr base;
    unsigned irq, reset_bank, reset_bit;
} mh2p_spi[] = {
    { 0x7000d400, 59, 1, 9 },
    { 0x7000d600, 82, 1, 12 },
    { 0x7000d800, 83, 1, 14 },
    { 0x7000da00, 93, 2, 4 },
    { 0x7000dc00, 94, 6, 8 },
    { 0x7000de00, 79, 6, 9 },
};

static const hwaddr mh2p_extra_clock_sources[] = {
    0x63b4, 0x63b8, /* MSELECT, TSENSOR */
    0x63c4, 0x63c8, 0x63cc, /* I2C4, SBC5, SBC6 */
    0x63d0, 0x63d8, 0x63dc, 0x63e0, /* audio, DAM0..2 */
    0x63e4, 0x6428, /* HDA2CODEC_2X, HDA */
    0x63e8, 0x63ec, 0x63f0, 0x63f4, /* ACTMON, EXTERN1..3 */
    0x63f8, 0x63fc, /* NDSPEED, I2CSLOW */
    0x6414, 0x6418, 0x6420, 0x6424, 0x642c, /* SOR, DPAUX, SATA, SE */
    0x6600, 0x6604, 0x6608, 0x660c, 0x6610, /* XUSB */
    0x6614, 0x6618, 0x661c, 0x6620, 0x6624, /* CIL, DSI LP */
    0x6628, 0x662c, 0x6630, 0x6634, /* entropy, DFLL, trace */
    0x6638, 0x663c, 0x6670, 0x6674, /* ADX0, AMX0, ADX1, AMX1 */
    0x6644, 0x664c, 0x6650, /* SOC_THERM, DMIC1..2 */
    0x6658, 0x665c, 0x6668, 0x666c, 0x6678, /* VI2, I2C6, HDMI, VIC */
};

static const uint16_t mh2p_smmu_groups[] = {
    0x238, 0x23c, 0x240, 0x244, /* AFI, AVPC, DC, DCB */
    0x250, 0x254, 0x258, 0x264, /* HC, HDA, ISP2, MSENC */
    0x270, 0x274, 0x27c, 0x280, /* PPCS, SATA, VDE, VI */
    0x284, 0x288, 0x28c, 0x290, 0x294, /* VIC, XUSB, A9AVP, TSEC */
    0xa88, /* DC12 */
    0xa94, 0xa98, 0xa9c, 0xaa0, /* SDMMC1..4 */
    0xaa4, 0xaa8, 0xaac, /* ISP2B, GPU, GPUB */
};

/* Local T124 controller setup, not the MHI2 synthetic RCC endpoint. */
static const struct {
    hwaddr offset;
    uint32_t mask, reset;
    uint32_t w1c;
    bool readonly;
} mh2p_pcie_registers[] = {
    { 0x0000, 0, 0x0e1210de, .readonly = true },
    { 0x1000, 0, 0x0e1310de, .readonly = true },
    { 0x0004, 0x00000547, 0x00100000, 0xf9000000 },
    { 0x1004, 0x00000547, 0x00100000, 0xf9000000 },
    { 0x0008, 0, 0x060400a1, .readonly = true },
    { 0x1008, 0, 0x060400a1, .readonly = true },
    { 0x000c, 0, 0x00010000, .readonly = true },
    { 0x100c, 0, 0x00010000, .readonly = true },
    { 0x0010, 0, 0, .readonly = true },
    { 0x1010, 0, 0, .readonly = true },
    { 0x0014, 0, 0, .readonly = true },
    { 0x1014, 0, 0, .readonly = true },
    { 0x0018, 0x00ffffff, 0 }, { 0x1018, 0x00ffffff, 0 },
    { 0x001c, 0x0000f0f0, 0x101, 0xf9000000 },
    { 0x101c, 0x0000f0f0, 0x101, 0xf9000000 },
    { 0x0020, 0xfff0fff0, 0 }, { 0x1020, 0xfff0fff0, 0 },
    { 0x0024, 0xfff0fff0, 0x10001 },
    { 0x1024, 0xfff0fff0, 0x10001 },
    { 0x0028, 0xffffffff, 0 }, { 0x1028, 0xffffffff, 0 },
    { 0x002c, 0xffffffff, 0 }, { 0x102c, 0xffffffff, 0 },
    { 0x0030, 0xffffffff, 0 }, { 0x1030, 0xffffffff, 0 },
    { 0x0034, 0, 0x80, .readonly = true },
    { 0x1034, 0, 0x80, .readonly = true },
    { 0x0038, 0, 0, .readonly = true },
    { 0x1038, 0, 0, .readonly = true },
    { 0x003c, 0x0fff00ff, 0 }, { 0x103c, 0x0fff00ff, 0 },
    { 0x0080, 0, 0x00420010, .readonly = true },
    { 0x1080, 0, 0x00420010, .readonly = true },
    { 0x0084, 0, 0, .readonly = true },
    { 0x1084, 0, 0, .readonly = true },
    { 0x0088, 0x0000000f, 0 }, { 0x1088, 0x0000000f, 0 },
    { 0x008c, 0, 0x42, .readonly = true },
    { 0x108c, 0, 0x12, .readonly = true },
    { 0x0090, 0x00000fc3, 0 }, { 0x1090, 0x00000fc3, 0 },
    { 0x0094, 0, 0, .readonly = true },
    { 0x1094, 0, 0, .readonly = true },
    { 0x0098, 0, 0, .readonly = true },
    { 0x1098, 0, 0, .readonly = true },
    { 0x009c, 0x0000001f, 0 }, { 0x109c, 0x0000001f, 0 },
    { 0x00a0, 0, 0, .readonly = true },
    { 0x10a0, 0, 0, .readonly = true },
    { 0x00a4, 0, 0, .readonly = true },
    { 0x10a4, 0, 0, .readonly = true },
    { 0x00a8, 0, 0, .readonly = true },
    { 0x10a8, 0, 0, .readonly = true },
    { 0x00ac, 0, 6, .readonly = true },
    { 0x10ac, 0, 6, .readonly = true },
    { 0x00b0, 0x0000000f, 0 }, { 0x10b0, 0x0000000f, 0 },
    { 0x0100, 0, 0, .readonly = true },
    { 0x1100, 0, 0, .readonly = true },
    /* Bounded post-link settings used by the original MMX initializer. */
    { 0x0494, 0x000003fe, 0 }, { 0x1494, 0x000003fe, 0 },
    { 0x0e00, 0x0000ff00, 0 }, { 0x1e00, 0x0000ff00, 0 },
    { 0x0f00, 0x1bfc0000, 0 }, { 0x1f00, 0x1bfc0000, 0 },
    { 0x0f04, 0x00200000, 0 }, { 0x1f04, 0x00200000, 0 },
    { 0x0f48, 0x00002000, 0 }, { 0x1f48, 0x00002000, 0 },
    { 0x0f4c, 0x10000000, 0 }, { 0x1f4c, 0x10000000, 0 },
    { 0x0fa8, BIT(11), 0 }, { 0x1fa8, BIT(11), 0 },
    { 0x0fd4, 0x10000000, 0 }, { 0x1fd4, 0x10000000, 0 },
    { 0x0fd8, 0x10000d00, 0 }, { 0x1fd8, 0x10000d00, 0 },
    { 0x0fe0, 0x8f8f000f, 0xf }, { 0x1fe0, 0x8f8f000f, 0xf },
    { 0x30c8, 0xffffffff, 0 }, /* Two 16-bit reference-clock pad fields. */
    { 0x30d0, 0x00000030, 0 },
    /* AXI sizes are pages; FPCI targets encode address bits 39:12 at 31:4. */
    { 0x3800, 0x000fffff, 0 }, { 0x3804, 0x000fffff, 0 },
    { 0x3808, 0x000fffff, 0 }, { 0x380c, 0x000fffff, 0 },
    { 0x3810, 0x000fffff, 0 }, { 0x3814, 0x000fffff, 0 },
    { 0x3818, 0xfffff000, 0 }, { 0x381c, 0xfffff000, 0 },
    { 0x3820, 0xfffff000, 0 }, { 0x3824, 0xfffff000, 0 },
    { 0x3828, 0xfffff000, 0 }, { 0x382c, 0xfffff000, 0 },
    { 0x3830, 0xfffffff1, 0 }, { 0x3834, 0xfffffff1, 0 },
    { 0x3838, 0xfffffff1, 0 }, { 0x383c, 0xfffffff1, 0 },
    { 0x3840, 0xfffffff1, 0 }, { 0x3844, 0xfffffff1, 0 },
    { 0x3860, 0x000fffff, 0 }, { 0x3864, 0xfffffff0, 0 },
    { 0x3868, 0xfffff000, 0 },
    { 0x38ac, 0x00000001, 0 },
    { 0x38b0, 0x00000000, 0 },
    { 0x38b4, 0x00000101, 0 },
    { 0x38b8, 0, 0 },
    { 0x386c, 0, 0, 0xffffffff }, { 0x3870, 0, 0, 0xffffffff },
    { 0x3874, 0, 0, 0xffffffff }, { 0x3878, 0, 0, 0xffffffff },
    { 0x387c, 0, 0, 0xffffffff }, { 0x3880, 0, 0, 0xffffffff },
    { 0x3884, 0, 0, 0xffffffff }, { 0x3888, 0, 0, 0xffffffff },
    { 0x388c, 0xffffffff, 0 }, { 0x3890, 0xffffffff, 0 },
    { 0x3894, 0xffffffff, 0 }, { 0x3898, 0xffffffff, 0 },
    { 0x389c, 0xffffffff, 0 }, { 0x38a0, 0xffffffff, 0 },
    { 0x38a4, 0xffffffff, 0 }, { 0x38a8, 0xffffffff, 0 },
    { 0x38c4, 0x000000ff, 0 },
    { 0x38c8, 0x0000017f, 0 },
    { 0x38f8, 0x00100006, 6 },
    { 0x3904, 0x00000004, 4 },
    { 0x3910, 0x0000001b, 0 }, { 0x3918, 0x0000001b, 0 },
    { 0x3934, 0x000fffff, 0 }, { 0x3938, 0x000fffff, 0 },
    { 0x393c, 0x000fffff, 0 },
    { 0x3940, 0xfffff000, 0 }, { 0x3944, 0xfffff000, 0 },
    { 0x3948, 0xfffff000, 0 },
    { 0x394c, 0xfffffff1, 0 }, { 0x3950, 0xfffffff1, 0 },
    { 0x3954, 0xfffffff1, 0 },
    { 0x3960, 0x00000202, 0x200 },
    { 0x3968, 0x00000000, 0 },
};

typedef struct MH2PMachineState MH2PMachineState;

typedef struct MH2PSecondaryCPU {
    ARMCPU *cpu;
    bool held;
    bool reset_pending;
    bool halted;
    ARMHaltReason halt_reason;
    uint32_t vector;
} MH2PSecondaryCPU;

typedef struct MH2PDisplayIOMMU {
    IOMMUMemoryRegion parent;
    MH2PMachineState *machine;
    unsigned head;
    unsigned window;
} MH2PDisplayIOMMU;

struct MH2PMachineState {
    MachineState parent;
    ARMCPU *cpu;
    MH2PSecondaryCPU secondary[3];
    uint32_t cpu_reset_vector;
    uint32_t cpu_reset_control;
    uint32_t cpu_clock_stop;
    /*
     * AVP (COP) facing registers.  The AVP core itself is not emulated, so
     * these only hold what the CPU writes; the COP never runs.
     */
    uint32_t shared_semaphore;
    uint32_t shared_mailbox[2];
    uint32_t cop_vectors[8];
    uint32_t halt_cop_events;
    bool experimental_psci;
    bool experimental_el3;
    MemoryRegion iram;
    MemoryRegion secure_memory, secure_ram, secure_carveout;
    MemoryRegion boot_regs;
    MemoryRegion pcie_config, pcie_outbound, mselect;
    uint32_t pcie_registers[ARRAY_SIZE(mh2p_pcie_registers)];
    DeviceState *pcie_sideband;
    DeviceState *pcie_transport;
    char *pcie_fd;
    DeviceState *pcie_outbound_transport;
    char *pcie_outbound_fd;
    qemu_irq pcie_msi_irq;
    uint32_t mselect_config;
    MemoryRegion dpaux_pad;
    IOMMUMemoryRegion host1x_iommu;
    IOMMUMemoryRegion vic_iommu;
    MH2PDisplayIOMMU display_iommu[2][3];
    Clock *display_clock[2];
    uint32_t dpaux_pad_control;
    uint32_t startup_timestamp[3];
    uint32_t clock_gates[12];
    uint32_t source_clocks[64];
    uint32_t extra_source_clocks[ARRAY_SIZE(mh2p_extra_clock_sources)];
    uint32_t osc_detect;
    uint32_t osc_detect_status;
    QEMUTimer *osc_detect_timer;
    uint32_t system_clock[3];
    uint32_t pll_c[4];
    uint32_t pll_cx[2][5];
    uint32_t pll_c4[5];
    uint32_t pll_re[2];
    uint32_t pll_aud[3][4];
    uint32_t pll_d2[5];
    uint32_t utmip_pll_config[2];
    uint32_t utmip_hw_control;
    uint32_t pllu_hw_control;
    bool pllu_hw_enabled;
    uint32_t xusb_pll_control;
    uint32_t xusb_pll_reference;
    uint32_t xusb_pcie_mux, xusb_elpg;
    uint32_t xusb_hw_control;
    bool xusb_hw_enabled;
    uint32_t pll_e_base;
    uint32_t pll_e_misc;
    uint32_t pll_e_ss;
    uint32_t pll_e_aux;
    bool pll_e_hw_enabled;
    uint32_t ahb_mem;
    uint32_t pmc_control;
    uint32_t pmc_sec_disable2;
    uint32_t pmc_secure_scratch[16];
    uint32_t secure_boot_csr;
    uint32_t clock_mask_arm;
    unsigned fuse_security_mode;
    uint32_t pex_dpd_request, pex_dpd_status;
    uint32_t gpu_clamp;
    uint32_t powergate_toggle;
    uint32_t powergate_status;
    uint32_t clamp_status;
    uint32_t mc_interrupt_mask;
    uint32_t mc_security_base, mc_security_size;
    uint32_t mc_hotreset[2];
    DeviceState *gic;
    qemu_irq *legacy_irq;
    uint32_t legacy_enable[5];
    uint32_t legacy_pending[5];
    uint32_t legacy_force[5];
    uint32_t smmu_config;
    uint32_t smmu_tlb_config;
    uint32_t smmu_ptc_config;
    uint32_t smmu_asid;
    uint32_t smmu_ptb[128];
    uint32_t smmu_translation_enable[4];
    uint32_t smmu_group[ARRAY_SIZE(mh2p_smmu_groups)];
    uint32_t smmu_flush_hi;
    uint32_t sd_clock[2];
    uint32_t sd_misc[2];
    uint32_t sd_pad[2];
    uint32_t sd_calibration[2];
    qemu_irq i2c_reset[ARRAY_SIZE(mh2p_i2c)];
    qemu_irq spi_reset[ARRAY_SIZE(mh2p_spi)];
    qemu_irq host1x_reset;
    qemu_irq dc_reset[2];
    qemu_irq hdmi_reset;
    qemu_irq sor_reset;
    qemu_irq sor_clock;
    qemu_irq vic_reset;
    qemu_irq vic_clock;
    qemu_irq gpu_reset;
    SDBus *mnand_bus;
    Notifier machine_done;
};

static IOMMUTLBEntry mh2p_smmu_translate(MH2PMachineState *s, unsigned group_id,
                                        unsigned client, const char *name,
                                        hwaddr addr, IOMMUAccessFlags flag)
{
    uint32_t group = s->smmu_group[group_id];
    uint32_t entry, permissions;
    MemTxResult result;
    IOMMUTLBEntry ret = {
        .target_as = &address_space_memory,
        .iova = addr & ~0xfffULL,
        .translated_addr = addr & ~0xfffULL,
        .addr_mask = 0xfff,
        .perm = IOMMU_NONE,
    };

    if (!(s->smmu_config & 1) || !(group & BIT(31)) ||
        !(s->smmu_translation_enable[client / 32] & BIT(client % 32))) {
        ret.perm = IOMMU_RW;
        return ret;
    }
    entry = s->smmu_ptb[group & 0x7f];
    permissions = entry;
    if (!(permissions & BIT(31))) {
        goto fault;
    }
    entry = address_space_ldl_le(&address_space_memory,
        ((hwaddr)(entry & 0x3fffff) << 12) + (addr >> 22) * 4,
        MEMTXATTRS_UNSPECIFIED, &result);
    if (result != MEMTX_OK) {
        goto fault;
    }
    permissions &= entry;
    if (entry & BIT(28)) {
        entry = address_space_ldl_le(&address_space_memory,
            ((hwaddr)(entry & 0x3fffff) << 12) +
            ((addr >> 12) & 0x3ff) * 4, MEMTXATTRS_UNSPECIFIED, &result);
        if (result != MEMTX_OK) {
            goto fault;
        }
        permissions &= entry;
        ret.translated_addr = (hwaddr)(entry & 0x3fffff) << 12;
    } else {
        ret.addr_mask = 0x3fffff;
        ret.iova = addr & ~ret.addr_mask;
        ret.translated_addr = (hwaddr)(entry & 0x3ffc00) << 12;
    }
    ret.perm = ((permissions & BIT(31)) ? IOMMU_RO : 0) |
               ((permissions & BIT(30)) ? IOMMU_WO : 0);
    if ((ret.perm & flag) == flag) {
        return ret;
    }
fault:
    ret.perm = IOMMU_NONE;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "mh2p-smmu: %s translation fault at 0x%" HWADDR_PRIx "\n",
                  name, addr);
    return ret;
}

static IOMMUTLBEntry mh2p_host1x_translate(IOMMUMemoryRegion *iommu,
                                         hwaddr addr, IOMMUAccessFlags flag,
                                         int iommu_idx)
{
    MH2PMachineState *s = container_of(iommu, MH2PMachineState, host1x_iommu);

    return mh2p_smmu_translate(s, 4, 0x16, "HOST1X", addr, flag);
}

static IOMMUTLBEntry mh2p_vic_translate(IOMMUMemoryRegion *iommu,
                                      hwaddr addr, IOMMUAccessFlags flag,
                                      int iommu_idx)
{
    MH2PMachineState *s = container_of(iommu, MH2PMachineState, vic_iommu);

    return mh2p_smmu_translate(s, 12, 0x6c, "VIC", addr, flag);
}

static void mh2p_iommu_class_init(ObjectClass *klass, const void *data)
{
    IOMMU_MEMORY_REGION_CLASS(klass)->translate = mh2p_host1x_translate;
}

static const TypeInfo mh2p_iommu_info = {
    .name = "mh2p-host1x-iommu",
    .parent = TYPE_IOMMU_MEMORY_REGION,
    .class_init = mh2p_iommu_class_init,
};

static void mh2p_vic_iommu_class_init(ObjectClass *klass, const void *data)
{
    IOMMU_MEMORY_REGION_CLASS(klass)->translate = mh2p_vic_translate;
}

static const TypeInfo mh2p_vic_iommu_info = {
    .name = "mh2p-vic-iommu",
    .parent = TYPE_IOMMU_MEMORY_REGION,
    .class_init = mh2p_vic_iommu_class_init,
};

static IOMMUTLBEntry mh2p_display_translate(IOMMUMemoryRegion *iommu,
                                           hwaddr addr, IOMMUAccessFlags flag,
                                           int iommu_idx)
{
    MH2PDisplayIOMMU *d = container_of(iommu, MH2PDisplayIOMMU, parent);

    return mh2p_smmu_translate(d->machine, 2 + d->head,
                               1 + d->head + d->window * 2,
                               "display", addr, flag);
}

static void mh2p_display_iommu_class_init(ObjectClass *klass, const void *data)
{
    IOMMU_MEMORY_REGION_CLASS(klass)->translate = mh2p_display_translate;
}

static const TypeInfo mh2p_display_iommu_info = {
    .name = "mh2p-display-iommu",
    .parent = TYPE_IOMMU_MEMORY_REGION,
    .instance_size = sizeof(MH2PDisplayIOMMU),
    .class_init = mh2p_display_iommu_class_init,
};

static void mh2p_register_iommu(void)
{
    type_register_static(&mh2p_iommu_info);
    type_register_static(&mh2p_vic_iommu_info);
    type_register_static(&mh2p_display_iommu_info);
}

type_init(mh2p_register_iommu)

static const struct {
    hwaddr base, set, clear;
} mh2p_clock_banks[] = {
    { 0x6004, 0x6300, 0x6304 }, { 0x6008, 0x6308, 0x630c },
    { 0x600c, 0x6310, 0x6314 }, { 0x6010, 0x6320, 0x6324 },
    { 0x6014, 0x6328, 0x632c }, { 0x6018, 0x6330, 0x6334 },
    { 0x6358, 0x6430, 0x6434 }, { 0x635c, 0x6438, 0x643c },
    { 0x6360, 0x6440, 0x6444 }, { 0x6364, 0x6448, 0x644c },
    { 0x628c, 0x6290, 0x6294 }, { 0x6280, 0x6284, 0x6288 },
};

static uint64_t mh2p_display_parent_hz(MH2PMachineState *s, unsigned parent)
{
    static const unsigned d2_dividers[] = {
        1, 2, 3, 4, 5, 6, 8, 10, 12, 16, 12, 16, 20, 24, 32,
    };
    uint32_t pll;
    unsigned m, n, p, divisor;

    if (parent == 2) {
        pll = s->pll_aud[2][0];
        m = pll & 31;
        n = (pll >> 8) & 0x7ff;
        divisor = (1U << ((pll >> 20) & 7)) * 2;
        if (!(s->pll_aud[2][3] & BIT(30)) ||
            (s->pll_aud[2][3] & BIT(23))) {
            return 0;
        }
    } else if (parent == 5) {
        pll = s->pll_d2[0];
        m = pll & 255;
        n = (pll >> 8) & 255;
        p = (pll >> 20) & 15;
        if (p >= ARRAY_SIZE(d2_dividers) ||
            (pll & (BIT(19) | BIT(25) | BIT(26))) ||
            (s->pll_d2[2] & (BIT(31) | BIT(30)))) {
            return 0;
        }
        divisor = d2_dividers[p];
    } else {
        return 0;
    }
    if (!(pll & BIT(30)) || (pll & (BIT(31) | BIT(29))) || !m || !n) {
        return 0;
    }
    return 12000000ULL * n / (m * divisor);
}

static void mh2p_update_peripheral_resets(MH2PMachineState *s)
{
    for (unsigned i = 0; i < ARRAY_SIZE(mh2p_pcie_registers); i++) {
        if ((s->clock_gates[2] & BIT(8)) ||
            !(s->powergate_status & BIT(3)) ||
            (mh2p_pcie_registers[i].offset < 0x3000 &&
             (s->clock_gates[2] & BIT(6)))) {
            s->pcie_registers[i] = mh2p_pcie_registers[i].reset;
        }
    }
    qemu_set_irq(s->gpu_reset, (s->clock_gates[10] & BIT(24)) || s->gpu_clamp);
    qemu_set_irq(s->host1x_reset, !!(s->clock_gates[0] & BIT(28)));
    qemu_set_irq(s->hdmi_reset, !!(s->clock_gates[1] & BIT(19)));
    qemu_set_irq(s->sor_reset, !!(s->clock_gates[10] & BIT(22)));
    qemu_set_irq(s->sor_clock, !!(s->clock_gates[11] & BIT(22)));
    qemu_set_irq(s->vic_reset, !!(s->clock_gates[10] & BIT(18)));
    qemu_set_irq(s->vic_clock, !!(s->clock_gates[11] & BIT(18)));
    for (unsigned i = 0; i < ARRAY_SIZE(s->dc_reset); i++) {
        qemu_set_irq(s->dc_reset[i], !!(s->clock_gates[0] & BIT(27 - i)));
        if (s->display_clock[i]) {
            uint64_t hz = 0;

            if (s->clock_gates[3] & BIT(27 - i)) {
                hz = mh2p_display_parent_hz(s,
                                           s->source_clocks[14 + i] >> 29);
            }
            clock_update_hz(s->display_clock[i], hz);
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(mh2p_i2c); i++) {
        qemu_set_irq(s->i2c_reset[i],
                     !!(s->clock_gates[mh2p_i2c[i].reset_bank] &
                        BIT(mh2p_i2c[i].reset_bit)));
    }
    for (unsigned i = 0; i < ARRAY_SIZE(mh2p_spi); i++) {
        qemu_set_irq(s->spi_reset[i],
                     !!(s->clock_gates[mh2p_spi[i].reset_bank] &
                        BIT(mh2p_spi[i].reset_bit)));
    }
}

static void mh2p_osc_detect_done(void *opaque)
{
    MH2PMachineState *s = opaque;

    /* 12 MHz oscillator counted over two 32.768 kHz reference periods. */
    s->osc_detect_status = 12000000 * 2 / 32768;
}

static uint32_t mh2p_clamp_mask(uint32_t partitions)
{
    /* PCIe and VDE use exchanged bits in the clamp registers. */
    return (partitions & ~(BIT(3) | BIT(4))) |
           ((partitions & BIT(3)) << 1) | ((partitions & BIT(4)) >> 1);
}

static bool mh2p_pllu_enabled(MH2PMachineState *s)
{
    uint32_t base = s->pll_aud[1][0];
    bool enabled = base & BIT(30);

    if (!(base & BIT(24)) && (s->pllu_hw_control & BIT(24)) &&
        !(s->pllu_hw_control & BIT(2))) {
        enabled = s->pllu_hw_enabled;
    }
    if (s->utmip_pll_config[0] & BIT(16)) {
        return false;
    }
    return enabled || (s->utmip_pll_config[0] & BIT(17));
}

static void mh2p_sequence_write(uint32_t *control, bool *enabled,
                                uint32_t value)
{
    /* No port power requests yet: latch the initial state on enable. */
    if ((value & BIT(24)) && !(*control & BIT(24))) {
        *enabled = value & BIT(25);
    }
    *control = value;
}

static bool mh2p_plle_locked(MH2PMachineState *s)
{
    bool enabled = s->pll_e_base & BIT(30);
    bool reference = true;

    if ((s->pll_e_aux & BIT(24)) && !(s->pll_e_aux & BIT(4))) {
        enabled = s->pll_e_hw_enabled;
    }
    if (s->pll_e_aux & BIT(28)) {
        reference = (s->pll_re[0] & BIT(30)) &&
                    (s->pll_re[0] & 0xff) &&
                    (s->pll_re[0] & 0xff00) &&
                    !(s->pll_re[1] & BIT(16));
    }
    return enabled && reference &&
           (s->pll_e_misc & (BIT(14) | BIT(13))) != (BIT(14) | BIT(13)) &&
           (s->pll_e_base & 0x0f000000) != 0x0f000000 &&
           (s->pll_e_base & 0xff) && (s->pll_e_base & 0xff00);
}

static bool mh2p_xusb_pll_locked(MH2PMachineState *s)
{
    bool enabled = s->xusb_pll_control & BIT(1);

    if ((s->xusb_hw_control & BIT(24)) &&
        !(s->xusb_hw_control & BIT(0))) {
        enabled = s->xusb_hw_enabled;
    }
    if ((s->xusb_pll_control & (BIT(3) | BIT(0))) == (BIT(3) | BIT(0))) {
        enabled = false;
    }
    return enabled && mh2p_plle_locked(s);
}

static uint32_t mh2p_secondary_powergates(MH2PMachineState *s)
{
    return ((BIT(MACHINE(s)->smp.cpus) - 1) & ~1U) << 8;
}

static bool mh2p_fuse_accessible(MH2PMachineState *s)
{
    return (s->clock_mask_arm & BIT(28)) &&
           (s->clock_gates[4] & BIT(7)) &&
           !(s->clock_gates[1] & BIT(7));
}

static void mh2p_secondary_sync(CPUState *cs, run_on_cpu_data data)
{
    MH2PSecondaryCPU *core = data.host_ptr;

    /*
     * Run architectural resets in the target CPU's context, including MTTCG.
     * Keep reset pulses pending even if the guest releases them before this
     * work runs; a clock stop alone must not destroy architectural state.
     */
    if (core->reset_pending) {
        cpu_reset(cs);
        if (!core->held) {
            cpu_set_pc(cs, core->vector);
            core->reset_pending = false;
        }
        core->halted = false;
        core->halt_reason = NOT_HALTED;
    }
    if (core->held) {
        if (core->cpu->power_state == PSCI_ON) {
            core->halted = cs->halted;
            core->halt_reason = qatomic_read(&core->cpu->env.halt_reason);
        }
        arm_set_cpu_power_state(core->cpu, PSCI_OFF);
        cs->halted = 1;
    } else if (core->cpu->power_state == PSCI_OFF) {
        arm_set_cpu_power_state(core->cpu, PSCI_ON);
        qatomic_set(&core->cpu->env.halt_reason, core->halt_reason);
        cs->halted = core->halted;
    }
}

static void mh2p_update_secondary_cpus(MH2PMachineState *s)
{
    for (unsigned i = 1; i < MACHINE(s)->smp.cpus; i++) {
        MH2PSecondaryCPU *core = &s->secondary[i - 1];
        uint32_t power = BIT(i + 8);
        uint32_t reset = BIT(i) | BIT(i + 4) | BIT(i + 12);
        bool resetting = (s->cpu_reset_control & reset) ||
                         !(s->powergate_status & power);
        bool held = resetting || (s->clamp_status & power) ||
                    (s->cpu_clock_stop & power);

        core->reset_pending |= resetting;
        if (held != core->held) {
            if (!held && core->reset_pending) {
                core->vector = s->cpu_reset_vector;
            }
            core->held = held;
            async_run_on_cpu(CPU(core->cpu), mh2p_secondary_sync,
                             RUN_ON_CPU_HOST_PTR(core));
        }
    }
}

static void mh2p_legacy_update(MH2PMachineState *s, unsigned bank)
{
    uint32_t active = (s->legacy_pending[bank] | s->legacy_force[bank]) &
                      s->legacy_enable[bank];

    for (unsigned bit = 0; bit < 32; bit++) {
        qemu_set_irq(qdev_get_gpio_in(s->gic, bank * 32 + bit),
                     !!(active & BIT(bit)));
    }
}

static void mh2p_legacy_irq(void *opaque, int irq, int level)
{
    MH2PMachineState *s = opaque;
    unsigned bank = irq / 32;

    if (level) {
        s->legacy_pending[bank] |= BIT(irq % 32);
    } else {
        s->legacy_pending[bank] &= ~BIT(irq % 32);
    }
    qemu_set_irq(qdev_get_gpio_in(s->gic, irq),
                 !!((s->legacy_pending[bank] | s->legacy_force[bank]) &
                    s->legacy_enable[bank] & BIT(irq % 32)));
}

static MemTxResult mh2p_carveout_read(void *opaque, hwaddr address,
                                    uint64_t *value, unsigned size,
                                    MemTxAttrs attrs)
{
    MH2PMachineState *s = opaque;

    *value = 0;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "mh2p: denied TZDRAM read at 0x%" HWADDR_PRIx "\n",
                  s->mc_security_base + address);
    return MEMTX_ERROR;
}

static MemTxResult mh2p_carveout_write(void *opaque, hwaddr address,
                                     uint64_t value, unsigned size,
                                     MemTxAttrs attrs)
{
    MH2PMachineState *s = opaque;

    qemu_log_mask(LOG_GUEST_ERROR,
                  "mh2p: denied TZDRAM write at 0x%" HWADDR_PRIx "\n",
                  s->mc_security_base + address);
    return MEMTX_ERROR;
}

static const MemoryRegionOps mh2p_carveout_ops = {
    .read_with_attrs = mh2p_carveout_read,
    .write_with_attrs = mh2p_carveout_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

static MemTxResult mh2p_security_write(MH2PMachineState *s, hwaddr address,
                                      uint32_t value, MemTxAttrs attrs)
{
    uint32_t base = address == 0x70019070 ? value : s->mc_security_base;
    uint32_t size = address == 0x70019074 ? value : s->mc_security_size;
    uint64_t length = (uint64_t)size * MiB;

    if (!attrs.secure) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "mh2p: nonsecure MC security configuration write denied\n");
        return MEMTX_OK;
    }
    if ((base & (MiB - 1)) ||
        (length && (base < MH2P_RAM_BASE ||
                    (uint64_t)base + length >
                    MH2P_RAM_BASE + MACHINE(s)->ram_size))) {
        qemu_log_mask(LOG_UNIMP,
                      "mh2p: unsupported MC secure carveout base 0x%x "
                      "size %u MiB\n", base, size);
        return MEMTX_ERROR;
    }
    memory_region_transaction_begin();
    memory_region_set_enabled(&s->secure_carveout, false);
    memory_region_set_size(&s->secure_carveout, length ?: 1);
    memory_region_set_address(&s->secure_carveout, base);
    s->mc_security_base = base;
    s->mc_security_size = size;
    memory_region_set_enabled(&s->secure_carveout, length != 0);
    memory_region_transaction_commit();
    return MEMTX_OK;
}

static MemTxResult mh2p_boot_read(void *opaque, hwaddr offset, uint64_t *value,
                                 unsigned size, MemTxAttrs attrs)
{
    MH2PMachineState *s = opaque;

    if (offset >= 0x1000e700 && offset < 0x1000e740 && size == 4) {
        unsigned index = (offset - 0x1000e700) / 4;

        if (s->pmc_sec_disable2 & BIT(index * 2 + 1)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "mh2p: read-disabled PMC secure scratch %u\n",
                          index + 8);
            *value = 0;
        } else {
            *value = s->pmc_secure_scratch[index];
        }
        return MEMTX_OK;
    }
    if (offset >= 0x4000 && offset < 0x4500 && size == 4) {
        unsigned bank = (offset - 0x4000) / 0x100;

        switch (offset & 0xff) {
        case 0x14:
            *value = s->legacy_force[bank];
            return MEMTX_OK;
        case 0x20:
            *value = s->legacy_enable[bank];
            return MEMTX_OK;
        case 0x2c:
            *value = 0; /* CPU interrupt classification: IRQ, not FIQ. */
            return MEMTX_OK;
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(mh2p_smmu_groups); i++) {
        if (offset == 0x10019000 + mh2p_smmu_groups[i]) {
            *value = s->smmu_group[i];
            return MEMTX_OK;
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(mh2p_extra_clock_sources); i++) {
        if (offset == mh2p_extra_clock_sources[i]) {
            *value = s->extra_source_clocks[i];
            return MEMTX_OK;
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(mh2p_clock_banks); i++) {
        if (offset == mh2p_clock_banks[i].base ||
            offset == mh2p_clock_banks[i].set ||
            offset == mh2p_clock_banks[i].clear) {
            *value = s->clock_gates[i];
            return MEMTX_OK;
        }
    }
    switch (offset + 0x60000000) {
    case 0x6000f100:
        *value = s->cpu_reset_vector;
        return MEMTX_OK;
    case 0x60001000: /* SHRD_SMP_STA */
        *value = s->shared_semaphore;
        return MEMTX_OK;
    case 0x60001010: /* SHRD_INBOX */
    case 0x60001020: /* SHRD_OUTBOX */
        *value = s->shared_mailbox[(offset - 0x1010) / 0x10];
        return MEMTX_OK;
    case 0x6000f200 ... 0x6000f21c: /* EVP COP exception vectors */
        *value = s->cop_vectors[(offset - 0xf200) / 4];
        return MEMTX_OK;
    case 0x60007004: /* FLOW_CTLR_HALT_COP_EVENTS */
        *value = s->halt_cop_events;
        return MEMTX_OK;
    case 0x60006058:
        *value = s->osc_detect;
        return MEMTX_OK;
    case 0x6000605c:
        *value = s->osc_detect_status;
        return MEMTX_OK;
    case 0x700b05e0:
    case 0x700b07e0:
        *value = s->sd_pad[(offset & 0x200) ? 0 : 1];
        return MEMTX_OK;
    case 0x700b05e4:
    case 0x700b07e4:
        *value = s->sd_calibration[(offset & 0x200) ? 0 : 1];
        return MEMTX_OK;
    case 0x700b05ec:
    case 0x700b07ec: /* Pad calibration completes synchronously. */
        *value = 0;
        return MEMTX_OK;
    case 0x700b0500:
    case 0x700b0700:
        *value = s->sd_clock[(offset & 0x200) ? 0 : 1];
        return MEMTX_OK;
    case 0x700b0520:
    case 0x700b0720:
        *value = s->sd_misc[(offset & 0x200) ? 0 : 1];
        return MEMTX_OK;
    case 0x60006020:
    case 0x60006368: /* CCLKG burst policy: RUN on CLK_M */
        *value = 0x20000000;
        return MEMTX_OK;
    case 0x60006024:
    case 0x6000636c:
        *value = 0;
        return MEMTX_OK;
    case 0x6000604c:
        *value = s->cpu_clock_stop;
        return MEMTX_OK;
    case 0x60006028:
    case 0x6000602c:
    case 0x60006030:
        *value = s->system_clock[(offset - 0x6028) / 4];
        return MEMTX_OK;
    case 0x7000f40c: /* AHB_GIZMO_AHB_MEM arbitration configuration */
        *value = s->ahb_mem;
        return MEMTX_OK;
    case 0x7000e400: /* PMC_CNTRL: external INTR polarity configuration */
        *value = s->pmc_control;
        return MEMTX_OK;
    case 0x7000e6c4: /* SEC_DISABLE2: read/write disables for scratch 8..23 */
        *value = s->pmc_sec_disable2;
        return MEMTX_OK;
    case 0x6000c200: /* SB_CSR: nonsecure reset-vector write disable */
        *value = s->secure_boot_csr;
        return MEMTX_OK;
    case 0x60006048: /* CLK_MASK_ARM: FUSE visibility control */
        *value = s->clock_mask_arm;
        return MEMTX_OK;
    case 0x7000f800: /* FUSECTRL: no command in progress, STATE_IDLE */
        if (!mh2p_fuse_accessible(s)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "mh2p: fuse status read while hidden or clock/reset "
                          "disabled\n");
            return MEMTX_ERROR;
        }
        *value = 4 << 16;
        return MEMTX_OK;
    case 0x7000f9a0: /* FUSE_SECURITY_MODE, explicit experimental input */
        if (!s->fuse_security_mode) {
            qemu_log_mask(LOG_UNIMP,
                          "mh2p: FUSE_SECURITY_MODE is unspecified\n");
            return MEMTX_ERROR;
        }
        if (!mh2p_fuse_accessible(s)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "mh2p: FUSE_SECURITY_MODE read while hidden or "
                          "clock/reset disabled\n");
            return MEMTX_ERROR;
        }
        *value = s->fuse_security_mode == 2;
        return MEMTX_OK;
    case 0x7000e6d4: /* GPU_RG_CNTRL: dedicated GPU clamp, not power status. */
        *value = s->gpu_clamp;
        return MEMTX_OK;
    case 0x7000e5b4: /* PMC_RST_STATUS: direct-start power-on reset */
        *value = 0;
        return MEMTX_OK;
    case 0x7000e5b8:
        *value = s->pex_dpd_request;
        return MEMTX_OK;
    case 0x7000e5bc:
        *value = s->pex_dpd_status;
        return MEMTX_OK;
    case 0x7000e430:
        *value = s->powergate_toggle;
        return MEMTX_OK;
    case 0x7000e42c:
        *value = s->clamp_status;
        return MEMTX_OK;
    case 0x7000e434:
        *value = 0;
        return MEMTX_OK;
    case 0x7000e438:
        *value = s->powergate_status;
        return MEMTX_OK;
    case 0x70019000: /* MC_INTSTATUS: no memory faults generated yet. */
        *value = 0;
        return MEMTX_OK;
    case 0x70019004:
        *value = s->mc_interrupt_mask;
        return MEMTX_OK;
    case 0x70019070:
        *value = s->mc_security_base;
        return MEMTX_OK;
    case 0x70019074:
        *value = s->mc_security_size;
        return MEMTX_OK;
    case 0x700199bc: /* MC_SECURITY_CFG3: high address bits unsupported. */
        *value = 0;
        return MEMTX_OK;
    case 0x70019010:
        *value = s->smmu_config;
        return MEMTX_OK;
    case 0x70019014:
        *value = s->smmu_tlb_config;
        return MEMTX_OK;
    case 0x70019018:
        *value = s->smmu_ptc_config;
        return MEMTX_OK;
    case 0x7001901c:
        *value = s->smmu_asid;
        return MEMTX_OK;
    case 0x70019020:
        *value = s->smmu_ptb[s->smmu_asid];
        return MEMTX_OK;
    case 0x70019228 ... 0x70019234:
        *value = s->smmu_translation_enable[(offset - 0x10019228) / 4];
        return MEMTX_OK;
    case 0x700199b8:
        *value = s->smmu_flush_hi;
        return MEMTX_OK;
    case 0x70019030: /* SMMU cache flush commands: no pending cache work. */
    case 0x70019034:
    case 0x70019038: /* ASID_SECURITY: nonsecure, unlocked address spaces. */
    case 0x7001903c:
    case 0x700199e0 ... 0x700199f4:
        *value = 0;
        return MEMTX_OK;
    case 0x70019200:
    case 0x70019204:
        *value = s->mc_hotreset[0];
        return MEMTX_OK;
    case 0x70019970:
    case 0x70019974:
        *value = s->mc_hotreset[1];
        return MEMTX_OK;
    case 0x60006100 ... 0x600061fc:
        *value = s->source_clocks[(offset - 0x6100) / 4];
        return MEMTX_OK;
    /* Direct-start clock snapshot, not a programmable T124 CAR model. */
    case 0x60006050: /* OSC_CTRL: 12 MHz crystal */
        *value = 8U << 28;
        return MEMTX_OK;
    case 0x600060a0: /* PLLP: 12 MHz * 34 */
        *value = 0x48002201;
        return MEMTX_OK;
    case 0x600060e0: /* PLLX: 12 MHz * 83 */
        *value = 0x48005301;
        return MEMTX_OK;
    case 0x60006080: /* PLLC: lock requires an enabled, valid divider. */
        *value = s->pll_c[0] & ~BIT(27);
        if ((*value & BIT(30)) && (*value & 0xff) &&
            (*value & 0xff00) && !(s->pll_c[3] & BIT(26))) {
            *value |= BIT(27);
        }
        return MEMTX_OK;
    case 0x60006084 ... 0x6000608c:
        *value = s->pll_c[(offset - 0x6080) / 4];
        return MEMTX_OK;
    case 0x600064e8 ... 0x6000650c: {
        unsigned pll = (offset - 0x64e8) / 20;
        unsigned reg = ((offset - 0x64e8) % 20) / 4;

        *value = s->pll_cx[pll][reg];
        if (reg == 0 && (*value & BIT(30)) && (*value & 3) &&
            (*value & 0xff00) && !(s->pll_cx[pll][1] & BIT(30))) {
            *value |= BIT(27);
        }
        return MEMTX_OK;
    }
    case 0x600065a4 ... 0x600065b4:
        *value = s->pll_c4[(offset - 0x65a4) / 4];
        if (offset == 0x65a4 && (*value & BIT(30)) &&
            (*value & 0xff) && (*value & 0xff00) && !(*value & BIT(19))) {
            *value |= BIT(27);
        }
        return MEMTX_OK;
    case 0x600064c4:
        *value = s->pll_re[0];
        return MEMTX_OK;
    case 0x600064c8:
        *value = s->pll_re[1];
        if ((s->pll_re[0] & BIT(30)) && (s->pll_re[0] & 0xff) &&
            (s->pll_re[0] & 0xff00) && !(*value & BIT(16))) {
            *value |= BIT(24);
        }
        return MEMTX_OK;
    case 0x600060b0: /* PLLA */
    case 0x600060c0: /* PLLU */
    case 0x600060d0: { /* PLLD */
        bool enabled;

        *value = s->pll_aud[(offset - 0x60b0) / 16][0];
        enabled = offset == 0x60c0 ? mh2p_pllu_enabled(s) : *value & BIT(30);
        if (enabled && (*value & 0x1f) &&
            (*value & (offset == 0x60d0 ? 0x7ff00 : 0x3ff00))) {
            *value |= BIT(27);
        }
        return MEMTX_OK;
    }
    case 0x600060b4:
    case 0x600060bc:
    case 0x600060cc:
    case 0x600060dc:
        *value = s->pll_aud[(offset - 0x60b0) / 16][(offset & 15) / 4];
        return MEMTX_OK;
    case 0x600064b8:
        *value = s->pll_d2[0];
        if (mh2p_display_parent_hz(s, 5) && (s->pll_d2[1] & BIT(30))) {
            *value |= BIT(27) | BIT(28);
        }
        return MEMTX_OK;
    case 0x600064bc:
        *value = s->pll_d2[1];
        return MEMTX_OK;
    case 0x60006570:
    case 0x60006574:
    case 0x60006578:
        *value = s->pll_d2[2 + (offset - 0x6570) / 4];
        return MEMTX_OK;
    case 0x60006484:
    case 0x60006488:
        *value = s->utmip_pll_config[(offset - 0x6484) / 4];
        return MEMTX_OK;
    case 0x6000652c:
        *value = s->utmip_hw_control;
        return MEMTX_OK;
    case 0x60006530:
        *value = s->pllu_hw_control;
        return MEMTX_OK;
    case 0x6000651c:
        *value = s->xusb_hw_control;
        return MEMTX_OK;
    case 0x7009f040:
        *value = s->xusb_pll_control |
                 (mh2p_xusb_pll_locked(s) ? BIT(19) : 0);
        return MEMTX_OK;
    case 0x7009f044:
        *value = s->xusb_pll_reference;
        return MEMTX_OK;
    case 0x7009f134:
        *value = s->xusb_pcie_mux;
        return MEMTX_OK;
    case 0x7009f01c:
        *value = s->xusb_elpg;
        return MEMTX_OK;
    case 0x600060e8:
        *value = s->pll_e_base;
        return MEMTX_OK;
    case 0x600060ec:
        *value = s->pll_e_misc | (mh2p_plle_locked(s) ? BIT(11) : 0);
        return MEMTX_OK;
    case 0x60006068:
        *value = s->pll_e_ss;
        return MEMTX_OK;
    case 0x6000648c:
        *value = s->pll_e_aux;
        if ((s->pll_e_aux & BIT(24)) && s->pll_e_hw_enabled) {
            *value |= (mh2p_plle_locked(s) ? 1U : 2U) << 26;
        }
        return MEMTX_OK;
    case 0x60006090: /* PLLM */
    case 0x600060f0: /* PLLS */
    case 0x60006094:
    case 0x600060a4:
    case 0x600060a8:
    case 0x600060ac: /* PLLP_MISC */
    case 0x60006590: /* PLLDP disabled */
    case 0x6000667c: /* PLLP_OUTC disabled */
    case 0x60006680: /* PLLP_MISC1 */
    case 0x60006370:
    case 0x6000702c: /* FLOW_CLUSTER_CONTROL: G cluster */
    case 0x60007000: /* HALT_CPU0_EVENTS: no flow-controlled halt */
    case 0x60007014:
    case 0x6000701c:
    case 0x60007024:
    case 0x60007008: /* CPU0_CSR: flow-controlled idle is disabled */
    case 0x60007018:
    case 0x60007020:
    case 0x60007028:
    case 0x7000f9f4: /* Startup disabled-core count [4:3]: no fused-off cores */
    case 0x7000f9fc: /* FUSE_RESERVED_CALIB: no CPU disable mask */
    case 0x70000460: /* APB_MISC_GP_EMU_REVID: no emulation revision */
    case 0x7000f9c4: /* FUSE_OPT_VP8_ENABLE: no VP8 hardware */
    case 0x7000e864: /* PMC scratch handoff: reset boot-device field */
        *value = 0;
        return MEMTX_OK;
    case 0x70000804: /* APB_MISC_GP_HIDREV: T124 */
        *value = 0x00014013;
        return MEMTX_OK;
    case 0x7000ead0:
    case 0x7000ead4:
    case 0x7000ead8:
        *value = s->startup_timestamp[(offset - 0x1000ead0) / 4];
        return MEMTX_OK;
    default:
        qemu_log_mask(LOG_UNIMP, "mh2p: unsupported boot register read 0x%"
                      HWADDR_PRIx "\n", offset + 0x60000000);
        return MEMTX_DECODE_ERROR;
    }
}

static MemTxResult mh2p_boot_write_register(void *opaque, hwaddr offset,
                                           uint64_t value, unsigned size,
                                           MemTxAttrs attrs)
{
    MH2PMachineState *s = opaque;

    if (offset >= 0x1000e700 && offset < 0x1000e740 && size == 4) {
        unsigned index = (offset - 0x1000e700) / 4;

        if (s->pmc_sec_disable2 & BIT(index * 2)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "mh2p: write-disabled PMC secure scratch %u\n",
                          index + 8);
        } else {
            s->pmc_secure_scratch[index] = value;
        }
        return MEMTX_OK;
    }
    if (offset >= 0x4000 && offset < 0x4500 && size == 4) {
        unsigned bank = (offset - 0x4000) / 0x100;

        switch (offset & 0xff) {
        case 0x14:
            s->legacy_force[bank] = value;
            break;
        case 0x18:
            s->legacy_force[bank] |= value;
            break;
        case 0x1c:
            s->legacy_force[bank] &= ~value;
            break;
        case 0x20:
            s->legacy_enable[bank] = value;
            break;
        case 0x24:
            s->legacy_enable[bank] |= value;
            break;
        case 0x28:
            s->legacy_enable[bank] &= ~value;
            break;
        case 0x2c:
            if (value) {
                qemu_log_mask(LOG_UNIMP,
                              "mh2p: legacy FIQ classification unsupported\n");
                return MEMTX_ERROR;
            }
            return MEMTX_OK;
        default:
            qemu_log_mask(LOG_UNIMP,
                          "mh2p: unsupported legacy interrupt write "
                          "0x%" HWADDR_PRIx "\n", offset + 0x60000000);
            return MEMTX_ERROR;
        }
        mh2p_legacy_update(s, bank);
        return MEMTX_OK;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(mh2p_smmu_groups); i++) {
        if (offset == 0x10019000 + mh2p_smmu_groups[i]) {
            s->smmu_group[i] = value & 0x8000007f;
            return MEMTX_OK;
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(mh2p_extra_clock_sources); i++) {
        if (offset == mh2p_extra_clock_sources[i]) {
            s->extra_source_clocks[i] = value;
            return MEMTX_OK;
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(mh2p_clock_banks); i++) {
        if (offset == mh2p_clock_banks[i].base) {
            s->clock_gates[i] = value;
        } else if (offset == mh2p_clock_banks[i].set) {
            s->clock_gates[i] |= value;
        } else if (offset == mh2p_clock_banks[i].clear) {
            s->clock_gates[i] &= ~value;
        } else {
            continue;
        }
        mh2p_update_peripheral_resets(s);
        return MEMTX_OK;
    }

    switch (offset + 0x60000000) {
    case 0x6000f100:
        if ((s->secure_boot_csr & BIT(1)) && !attrs.secure) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "mh2p: nonsecure reset-vector write disabled\n");
            return MEMTX_OK;
        }
        s->cpu_reset_vector = value;
        return MEMTX_OK;
    case 0x6000c200:
        if (!attrs.secure) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "mh2p: nonsecure SB_CSR write denied\n");
            return MEMTX_OK;
        }
        if (value & ~BIT(1)) {
            qemu_log_mask(LOG_UNIMP,
                          "mh2p: unsupported SB_CSR value 0x%" PRIx64 "\n",
                          value);
            return MEMTX_DECODE_ERROR;
        }
        s->secure_boot_csr = value;
        return MEMTX_OK;
    case 0x60006048:
        if (value & ~BIT(28)) {
            qemu_log_mask(LOG_UNIMP,
                          "mh2p: unsupported CLK_MASK_ARM value 0x%" PRIx64 "\n",
                          value);
            return MEMTX_DECODE_ERROR;
        }
        s->clock_mask_arm = value;
        return MEMTX_OK;
    case 0x60001004: /* SHRD_SMP_SET */
        s->shared_semaphore |= value;
        return MEMTX_OK;
    case 0x60001008: /* SHRD_SMP_CLR */
        s->shared_semaphore &= ~value;
        return MEMTX_OK;
    case 0x60001010:
    case 0x60001020:
        s->shared_mailbox[(offset - 0x1010) / 0x10] = value;
        return MEMTX_OK;
    case 0x6000f200 ... 0x6000f21c:
        s->cop_vectors[(offset - 0xf200) / 4] = value;
        return MEMTX_OK;
    case 0x60007004:
        s->halt_cop_events = value;
        return MEMTX_OK;
    case 0x60006340:
    case 0x60006344: {
        uint32_t cores = mh2p_secondary_powergates(s) >> 8;
        uint32_t mask = cores | (cores << 4) | (cores << 12);

        if ((value & ~mask) ||
            (s->experimental_psci && offset == 0x6340 && value)) {
            qemu_log_mask(LOG_UNIMP, "mh2p: unsupported CPU reset command "
                          "0x%" PRIx64 "\n", value);
            return MEMTX_DECODE_ERROR;
        }
        if (offset == 0x6340) {
            s->cpu_reset_control |= value;
        } else {
            s->cpu_reset_control &= ~value;
        }
        mh2p_update_secondary_cpus(s);
        return MEMTX_OK;
    }
    case 0x6000604c:
        if ((value & ~mh2p_secondary_powergates(s)) ||
            (s->experimental_psci && value)) {
            qemu_log_mask(LOG_UNIMP, "mh2p: unsupported CPU clock stop "
                          "0x%" PRIx64 "\n", value);
            return MEMTX_DECODE_ERROR;
        }
        s->cpu_clock_stop = value;
        mh2p_update_secondary_cpus(s);
        return MEMTX_OK;
    case 0x60007000:
    case 0x60007014:
    case 0x6000701c:
    case 0x60007024:
        /* CPUs run without a flow-controlled halt/power transition. */
        if (value) {
            qemu_log_mask(LOG_UNIMP, "mh2p: unsupported CPU halt events "
                          "0x%" PRIx64 " at 0x%" HWADDR_PRIx "\n",
                          value, offset + 0x60000000);
            return MEMTX_DECODE_ERROR;
        }
        return MEMTX_OK;
    case 0x60007008:
    case 0x60007018:
    case 0x60007020:
    case 0x60007028:
        /* INTR_FLAG and EVENT_FLAG are W1C; idle entry is not modeled. */
        if (value & ~(BIT(15) | BIT(14))) {
            qemu_log_mask(LOG_UNIMP, "mh2p: unsupported CPU flow CSR "
                          "0x%" PRIx64 " at 0x%" HWADDR_PRIx "\n",
                          value, offset + 0x60000000);
            return MEMTX_DECODE_ERROR;
        }
        return MEMTX_OK;
    case 0x7000e5b8: {
        uint32_t pads = value & 0x70;
        unsigned command = value >> 30;

        if ((value & ~0xc0000070U) || !pads ||
            (command != 1 && command != 2)) {
            qemu_log_mask(LOG_UNIMP, "mh2p: unsupported PEX DPD request "
                          "0x%" PRIx64 "\n", value);
            return MEMTX_DECODE_ERROR;
        }
        s->pex_dpd_request = pads;
        if (command == 2) {
            s->pex_dpd_status |= pads;
        } else {
            s->pex_dpd_status &= ~pads;
        }
        return MEMTX_OK;
    }
    case 0x60006080 ... 0x6000608c:
        s->pll_c[(offset - 0x6080) / 4] =
            offset == 0x6080 ? value & ~BIT(27) : value;
        return MEMTX_OK;
    case 0x600064e8 ... 0x6000650c: {
        unsigned pll = (offset - 0x64e8) / 20;
        unsigned reg = ((offset - 0x64e8) % 20) / 4;

        s->pll_cx[pll][reg] = reg == 0 ? value & ~BIT(27) : value;
        return MEMTX_OK;
    }
    case 0x600065a4 ... 0x600065b4:
        s->pll_c4[(offset - 0x65a4) / 4] =
            offset == 0x65a4 ? value & ~BIT(27) : value;
        return MEMTX_OK;
    case 0x600064c4:
        s->pll_re[0] = value;
        return MEMTX_OK;
    case 0x600064c8:
        s->pll_re[1] = value & ~BIT(24);
        return MEMTX_OK;
    case 0x600060b0:
    case 0x600060c0:
    case 0x600060d0:
        s->pll_aud[(offset - 0x60b0) / 16][0] = value & ~BIT(27);
        mh2p_update_peripheral_resets(s);
        return MEMTX_OK;
    case 0x600060b4:
    case 0x600060bc:
    case 0x600060cc:
    case 0x600060dc:
        s->pll_aud[(offset - 0x60b0) / 16][(offset & 15) / 4] = value;
        mh2p_update_peripheral_resets(s);
        return MEMTX_OK;
    case 0x600064b8:
        s->pll_d2[0] = value & 0xe7f9ffff;
        mh2p_update_peripheral_resets(s);
        return MEMTX_OK;
    case 0x600064bc:
        s->pll_d2[1] = value & 0xc7ffffff;
        mh2p_update_peripheral_resets(s);
        return MEMTX_OK;
    case 0x60006570:
        if (value & ~0x18400000U) {
            qemu_log_mask(LOG_UNIMP, "mh2p: unsupported PLLD2 modulation\n");
            return MEMTX_ERROR;
        }
        s->pll_d2[2] = value;
        mh2p_update_peripheral_resets(s);
        return MEMTX_OK;
    case 0x60006574:
    case 0x60006578:
        s->pll_d2[2 + (offset - 0x6570) / 4] = value;
        return MEMTX_OK;
    case 0x60006484:
    case 0x60006488:
        s->utmip_pll_config[(offset - 0x6484) / 4] = value;
        return MEMTX_OK;
    case 0x6000652c:
        if (value & ~0x03000077U) {
            qemu_log_mask(LOG_UNIMP, "mh2p: unsupported UTMIP sequencer "
                          "control 0x%" PRIx64 "\n", value);
            return MEMTX_DECODE_ERROR;
        }
        s->utmip_hw_control = value;
        return MEMTX_OK;
    case 0x60006530:
        if (value & ~0x03000045U) {
            qemu_log_mask(LOG_UNIMP, "mh2p: unsupported PLLU sequencer "
                          "control 0x%" PRIx64 "\n", value);
            return MEMTX_DECODE_ERROR;
        }
        mh2p_sequence_write(&s->pllu_hw_control, &s->pllu_hw_enabled, value);
        return MEMTX_OK;
    case 0x6000651c:
        if (value & ~0x03000045U) {
            qemu_log_mask(LOG_UNIMP, "mh2p: unsupported XUSB sequencer "
                          "control 0x%" PRIx64 "\n", value);
            return MEMTX_DECODE_ERROR;
        }
        mh2p_sequence_write(&s->xusb_hw_control, &s->xusb_hw_enabled, value);
        return MEMTX_OK;
    case 0x7009f040:
        if (value & ~(BIT(19) | BIT(3) | BIT(1) | BIT(0))) {
            qemu_log_mask(LOG_UNIMP, "mh2p: unsupported XUSB pad PLL "
                          "control 0x%" PRIx64 "\n", value);
            return MEMTX_DECODE_ERROR;
        }
        s->xusb_pll_control = value & ~BIT(19);
        return MEMTX_OK;
    case 0x7009f044:
        if (value & ~0x70U) {
            break;
        }
        s->xusb_pll_reference = value;
        return MEMTX_OK;
    case 0x7009f134:
        if (value & ~0x03ff003eU) {
            break;
        }
        s->xusb_pcie_mux = value;
        return MEMTX_OK;
    case 0x7009f01c:
        if (value & ~0x07000000U) {
            break;
        }
        s->xusb_elpg = value;
        return MEMTX_OK;
    case 0x600060e8:
        s->pll_e_base = value;
        return MEMTX_OK;
    case 0x600060ec:
        s->pll_e_misc = value & ~BIT(11);
        return MEMTX_OK;
    case 0x60006068:
        s->pll_e_ss = value;
        return MEMTX_OK;
    case 0x6000648c:
        if (value & ~0x9fffffffU) {
            qemu_log_mask(LOG_UNIMP, "mh2p: unsupported PLLE auxiliary "
                          "control 0x%" PRIx64 "\n", value);
            return MEMTX_DECODE_ERROR;
        }
        mh2p_sequence_write(&s->pll_e_aux, &s->pll_e_hw_enabled,
                            value & ~(3U << 26));
        return MEMTX_OK;
    case 0x7000e430: {
        uint32_t mask = BIT(value & 0x1f);

        if ((value & ~0x11fU) ||
            !(mask & (MH2P_PERIPHERAL_POWERGATES |
                      mh2p_secondary_powergates(s))) ||
            (s->experimental_psci && (value & BIT(8)) &&
             (mask & mh2p_secondary_powergates(s)))) {
            qemu_log_mask(LOG_UNIMP, "mh2p: unsupported powergate command "
                          "0x%" PRIx64 "\n", value);
            return MEMTX_DECODE_ERROR;
        }
        if (value & BIT(8)) {
            s->powergate_status ^= mask;
            if (!(s->powergate_status & mask)) {
                s->clamp_status |= mh2p_clamp_mask(mask);
            }
        }
        s->powergate_toggle = value & 0x1f;
        mh2p_update_peripheral_resets(s);
        mh2p_update_secondary_cpus(s);
        return MEMTX_OK;
    }
    case 0x7000e434:
        if (value & ~(MH2P_PERIPHERAL_POWERGATES |
                      mh2p_secondary_powergates(s))) {
            qemu_log_mask(LOG_UNIMP, "mh2p: unsupported clamp command "
                          "0x%" PRIx64 "\n", value);
            return MEMTX_DECODE_ERROR;
        }
        s->clamp_status &= ~(value & mh2p_clamp_mask(s->powergate_status));
        mh2p_update_secondary_cpus(s);
        return MEMTX_OK;
    case 0x70019000:
        /* W1C: there are no modeled memory-controller error sources. */
        return MEMTX_OK;
    case 0x70019004:
        s->mc_interrupt_mask = value & MH2P_MC_INTERRUPTS;
        return MEMTX_OK;
    case 0x70019070:
    case 0x70019074:
        return mh2p_security_write(s, offset + 0x60000000, value, attrs);
    case 0x700199bc:
        if (!attrs.secure) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "mh2p: nonsecure MC high security address write denied\n");
            return MEMTX_OK;
        }
        if (value) {
            break;
        }
        return MEMTX_OK;
    case 0x70019010:
        if (value & ~1U) {
            break;
        }
        s->smmu_config = value;
        return MEMTX_OK;
    case 0x70019014:
        if (value & ~0x3000003fU) {
            break;
        }
        s->smmu_tlb_config = value;
        return MEMTX_OK;
    case 0x70019018:
        if (value & ~0x2f00003fU) {
            break;
        }
        s->smmu_ptc_config = value;
        return MEMTX_OK;
    case 0x7001901c:
        s->smmu_asid = value & 0x7f;
        return MEMTX_OK;
    case 0x70019020:
        /* T124 has 34 physical address bits plus R/W/nonsecure attributes. */
        s->smmu_ptb[s->smmu_asid] = value & 0xe03fffff;
        return MEMTX_OK;
    case 0x70019228 ... 0x70019234:
        s->smmu_translation_enable[(offset - 0x10019228) / 4] = value;
        return MEMTX_OK;
    case 0x700199b8:
        s->smmu_flush_hi = value & 3;
        return MEMTX_OK;
    case 0x70019030:
    case 0x70019034:
        /* Initialization only: no SMMU DMA clients or cached translations. */
        return MEMTX_OK;
    case 0x70019038:
    case 0x7001903c:
    case 0x700199e0 ... 0x700199f4:
        if (value) {
            break; /* Secure/locked address spaces are not implemented. */
        }
        return MEMTX_OK;
    case 0x70019200:
        /* No queued MC transactions: requested client flushes are drained. */
        s->mc_hotreset[0] = value & 0x02ffcfcf;
        return MEMTX_OK;
    case 0x70019970:
        s->mc_hotreset[1] = value & 7;
        return MEMTX_OK;
    case 0x60006028:
    case 0x6000602c:
    case 0x60006030:
        s->system_clock[(offset - 0x6028) / 4] = value;
        return MEMTX_OK;
    case 0x60006058:
        if ((value & ~BIT(31)) != 1) {
            qemu_log_mask(LOG_UNIMP, "mh2p: unsupported oscillator "
                          "measurement window 0x%" PRIx64 "\n", value);
            return MEMTX_DECODE_ERROR;
        }
        s->osc_detect = value & ~BIT(31);
        if (value & BIT(31)) {
            s->osc_detect_status = BIT(31);
            timer_mod(s->osc_detect_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      DIV_ROUND_UP(2 * NANOSECONDS_PER_SECOND, 32768));
        }
        return MEMTX_OK;
    case 0x700b05e0:
    case 0x700b07e0:
        s->sd_pad[(offset & 0x200) ? 0 : 1] = value;
        return MEMTX_OK;
    case 0x700b05e4:
    case 0x700b07e4:
        s->sd_calibration[(offset & 0x200) ? 0 : 1] = value & ~BIT(31);
        return MEMTX_OK;
    case 0x700b0500:
    case 0x700b0700:
        s->sd_clock[(offset & 0x200) ? 0 : 1] = value;
        return MEMTX_OK;
    case 0x700b0520:
    case 0x700b0720:
        s->sd_misc[(offset & 0x200) ? 0 : 1] = value;
        return MEMTX_OK;
    case 0x7000e400:
        if (value & ~BIT(17)) {
            qemu_log_mask(LOG_UNIMP, "mh2p: unsupported PMC_CNTRL bits "
                          "0x%" PRIx64 "\n", value);
            return MEMTX_DECODE_ERROR;
        }
        s->pmc_control = value;
        return MEMTX_OK;
    case 0x7000e6c4:
        s->pmc_sec_disable2 = value;
        return MEMTX_OK;
    case 0x7000e6d4:
        if (value & ~1U) {
            break;
        }
        s->gpu_clamp = value;
        mh2p_update_peripheral_resets(s);
        return MEMTX_OK;
    case 0x7000f40c:
        s->ahb_mem = value;
        return MEMTX_OK;
    case 0x60006100 ... 0x600061fc:
        s->source_clocks[(offset - 0x6100) / 4] = value;
        mh2p_update_peripheral_resets(s);
        return MEMTX_OK;
    case 0x7000ead0:
    case 0x7000ead4:
    case 0x7000ead8:
        s->startup_timestamp[(offset - 0x1000ead0) / 4] = value;
        return MEMTX_OK;
    default:
        break;
    }
    qemu_log_mask(LOG_UNIMP, "mh2p: unsupported boot register write 0x%"
                  HWADDR_PRIx " value 0x%" PRIx64 "\n",
                  offset + 0x60000000, value);
    return MEMTX_DECODE_ERROR;
}

static bool mh2p_pcie_accessible(MH2PMachineState *s, hwaddr offset)
{
    uint32_t bits = BIT(8) | (offset < 0x3000 ? BIT(6) : 0);

    return (s->powergate_status & BIT(3)) && !(s->clamp_status & BIT(4)) &&
           !(s->clock_gates[2] & bits) &&
           (s->clock_gates[5] & bits) == bits;
}

static uint32_t mh2p_pcie_value(MH2PMachineState *s, hwaddr offset)
{
    for (unsigned i = 0; i < ARRAY_SIZE(mh2p_pcie_registers); i++) {
        if (mh2p_pcie_registers[i].offset == offset) {
            return s->pcie_registers[i];
        }
    }
    g_assert_not_reached();
}

static bool mh2p_pcie_reference(MH2PMachineState *s)
{
    /* Qualified path: 12 MHz oscillator, DIVCML encoding 13 (divide by 24). */
    return mh2p_pcie_accessible(s, 0x3910) && mh2p_xusb_pll_locked(s) &&
                !(s->pll_e_aux & (BIT(28) | BIT(2))) &&
                ((s->pll_e_base >> 24) & 0xf) == 13 &&
                12000000ULL * ((s->pll_e_base >> 8) & 0xff) ==
                    100000000ULL * (s->pll_e_base & 0xff) * 24 &&
                s->xusb_pll_reference == 0x70 &&
                (s->xusb_pcie_mux & 0x03ff003e) == 0x38 &&
                !(s->xusb_elpg & 0x07000000) &&
                !(s->pex_dpd_status & 0x30) &&
                (mh2p_pcie_value(s, 0x3960) & 0x202) == 2 &&
                (mh2p_pcie_value(s, 0x3910) & BIT(3));
}

static bool mh2p_pcie_root_ready(MH2PMachineState *s)
{
    return mh2p_pcie_reference(s) && mh2p_pcie_accessible(s, 0x1000) &&
           (mh2p_pcie_value(s, 0x3910) & BIT(0)) &&
           (mh2p_pcie_value(s, 0x38ac) & 1);
}

static bool mh2p_pcie_memory_ready(void *opaque)
{
    return mh2p_pcie_root_ready(opaque);
}

static void mh2p_pcie_update_msi(MH2PMachineState *s)
{
    bool pending = false;

    for (unsigned bank = 0; bank < 8; bank++) {
        pending |= (mh2p_pcie_value(s, 0x386c + bank * 4) &
                    mh2p_pcie_value(s, 0x388c + bank * 4)) != 0;
    }
    qemu_set_irq(s->pcie_msi_irq, pending && mh2p_pcie_root_ready(s) &&
                 (mh2p_pcie_value(s, 0x38b4) & BIT(8)));
}

static MemTxResult mh2p_pcie_inbound_memory(void *opaque, unsigned address,
                                          unsigned size, uint32_t *value,
                                          bool write)
{
    MH2PMachineState *s = opaque;
    uint8_t bytes[4];
    MemTxResult result;
    uint64_t msi_base = (uint64_t)mh2p_pcie_value(s, 0x3864) << 8;
    uint64_t msi_size = (uint64_t)mh2p_pcie_value(s, 0x3860) << 12;

    if (write && size == 4 && address >= msi_base &&
        (uint64_t)address - msi_base + size <= msi_size) {
        unsigned vector = *value & 0xff;
        unsigned offset = 0x386c + (vector / 32) * 4;

        for (unsigned i = 0; i < ARRAY_SIZE(mh2p_pcie_registers); i++) {
            if (mh2p_pcie_registers[i].offset == offset) {
                s->pcie_registers[i] |= BIT(vector % 32);
                mh2p_pcie_update_msi(s);
                return MEMTX_OK;
            }
        }
        g_assert_not_reached();
    }

    /* The native root uses identity DMA addresses into its actual DRAM. */
    if (address < 0x80000000 ||
        (uint64_t)address + size > 0x80000000ULL + MACHINE(s)->ram_size) {
        qemu_log_mask(LOG_UNIMP, "mh2p: unsupported inbound PCIe %s "
                      "at 0x%x size %u\n", write ? "write" : "read",
                      address, size);
        return MEMTX_ERROR;
    }
    if (write) {
        stl_le_p(bytes, *value);
        return address_space_write(&address_space_memory, address,
                                   MEMTXATTRS_UNSPECIFIED, bytes, size);
    }
    result = address_space_read(&address_space_memory, address,
                                MEMTXATTRS_UNSPECIFIED, bytes, size);
    if (result == MEMTX_OK) {
        *value = size == 4 ? ldl_le_p(bytes) :
                 size == 2 ? lduw_le_p(bytes) : bytes[0];
    }
    return result;
}

static void mh2p_update_pcie_sideband(MH2PMachineState *s)
{
    mh2p_pcie_sideband_set(s->pcie_sideband, mh2p_pcie_reference(s),
                          mh2p_pcie_accessible(s, 0x3910) &&
                          (mh2p_pcie_value(s, 0x3910) & BIT(0)));
    mh2p_pcie_update_msi(s);
}

static MemTxResult mh2p_boot_write(void *opaque, hwaddr offset, uint64_t value,
                                  unsigned size, MemTxAttrs attrs)
{
    MemTxResult result = mh2p_boot_write_register(opaque, offset, value,
                                                 size, attrs);

    if (result == MEMTX_OK) {
        mh2p_update_pcie_sideband(opaque);
    }
    return result;
}

static const MemoryRegionOps mh2p_boot_ops = {
    .read_with_attrs = mh2p_boot_read,
    .write_with_attrs = mh2p_boot_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static MemTxResult mh2p_pcie_read(void *opaque, hwaddr offset, uint64_t *value,
                                 unsigned size, MemTxAttrs attrs)
{
    MH2PMachineState *s = opaque;

    if (!mh2p_pcie_accessible(s, offset)) {
        goto unsupported;
    }
    if (offset == 0xf00 || offset == 0x1f00) {
        *value = mh2p_pcie_value(s, offset);
        if (offset == 0x1f00 &&
            mh2p_pcie_config_link(s->pcie_transport,
                                  mh2p_pcie_root_ready(s))) {
            *value |= BIT(30);
        }
        return MEMTX_OK;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(mh2p_pcie_registers); i++) {
        if (offset == mh2p_pcie_registers[i].offset) {
            *value = s->pcie_registers[i];
            if (offset == 0x1090 &&
                mh2p_pcie_config_link(s->pcie_transport,
                                      mh2p_pcie_root_ready(s))) {
                *value |= BIT(29) | BIT(20) | BIT(16); /* Gen1 x1. */
            }
            return MEMTX_OK;
        }
    }
unsupported:
    qemu_log_mask(LOG_UNIMP, "mh2p: unsupported/reset/clock-gated PCIe read "
                  "0x%" HWADDR_PRIx "\n", offset + 0x01000000);
    *value = 0;
    return MEMTX_DECODE_ERROR;
}

static MemTxResult mh2p_pcie_write(void *opaque, hwaddr offset, uint64_t value,
                                  unsigned size, MemTxAttrs attrs)
{
    MH2PMachineState *s = opaque;

    if (!mh2p_pcie_accessible(s, offset)) {
        goto unsupported;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(mh2p_pcie_registers); i++) {
        if (offset == mh2p_pcie_registers[i].offset) {
            uint32_t mask = mh2p_pcie_registers[i].mask;
            uint32_t allowed = mask | mh2p_pcie_registers[i].w1c |
                               mh2p_pcie_registers[i].reset;

            if (offset == 0xf00 || offset == 0x1f00) {
                allowed |= BIT(30); /* Read-only live DL_UP in RMW writes. */
            }
            if (mh2p_pcie_registers[i].readonly) {
                return MEMTX_OK;
            }
            /* The original initializer writes all ones to interrupt enables. */
            if (offset != 0x38c4 && (value & ~allowed)) {
                goto unsupported;
            }
            s->pcie_registers[i] = (s->pcie_registers[i] & ~mask) |
                                  (value & mask);
            s->pcie_registers[i] &= ~(value & mh2p_pcie_registers[i].w1c);
            mh2p_update_pcie_sideband(s);
            return MEMTX_OK;
        }
    }
unsupported:
    qemu_log_mask(LOG_UNIMP, "mh2p: unsupported/reset/clock-gated PCIe write "
                  "0x%" HWADDR_PRIx " value 0x%" PRIx64 "\n",
                  offset + 0x01000000, value);
    return MEMTX_DECODE_ERROR;
}

static const MemoryRegionOps mh2p_pcie_ops = {
    .read_with_attrs = mh2p_pcie_read,
    .write_with_attrs = mh2p_pcie_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static bool mh2p_pcie_config_target(MH2PMachineState *s, hwaddr address,
                                    unsigned size, uint64_t *fpci,
                                    unsigned *window, unsigned *type,
                                    unsigned *bus, unsigned *devfn,
                                    unsigned *reg)
{
    static const struct {
        uint16_t size, axi, fpci;
    } windows[] = {
        { 0x3800, 0x3818, 0x3830 }, { 0x3804, 0x381c, 0x3834 },
        { 0x3808, 0x3820, 0x3838 }, { 0x380c, 0x3824, 0x383c },
        { 0x3810, 0x3828, 0x3840 }, { 0x3814, 0x382c, 0x3844 },
        { 0x3934, 0x3940, 0x394c }, { 0x3938, 0x3944, 0x3950 },
        { 0x393c, 0x3948, 0x3954 },
    };
    unsigned selected = ARRAY_SIZE(windows);
    bool extended;
    uint32_t target;

    if (!mh2p_pcie_accessible(s, 0) ||
        !(mh2p_pcie_value(s, 0x38ac) & 1) ||
        (address & (size - 1))) {
        return false;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(windows); i++) {
        uint64_t length = (uint64_t)mh2p_pcie_value(s, windows[i].size) << 12;
        uint32_t base = mh2p_pcie_value(s, windows[i].axi);
        uint64_t offset;

        if (!length || address < base || address - base >= length) {
            continue;
        }
        offset = address - base;
        if (selected != ARRAY_SIZE(windows) || size > length - offset) {
            return false; /* Overlapping windows are not modeled. */
        }
        selected = i;
    }
    if (selected == ARRAY_SIZE(windows)) {
        return false;
    }
    target = mh2p_pcie_value(s, windows[selected].fpci);
    *window = selected;
    *fpci = ((uint64_t)(target & ~1U) << 8) +
            address - mh2p_pcie_value(s, windows[selected].axi);
    if (*fpci >= (1ULL << 40)) {
        return false;
    }
    if (target & 1) {
        if (!s->pcie_transport || *fpci > UINT32_MAX ||
            size - 1 > UINT32_MAX - *fpci) {
            return false;
        }
        *type = 2;
        *bus = *devfn = *reg = 0;
        return true;
    }
    if (*fpci >= 0xfdfe000000ULL && *fpci < 0xfe00000000ULL) {
        extended = false;
        *type = (*fpci >> 24) & 1;
    } else if (*fpci >= 0xfe00000000ULL && *fpci < 0xfe20000000ULL) {
        extended = true;
        *type = (*fpci >> 28) & 1;
    } else {
        /* Memory, I/O and reserved FPCI targets are not routed. */
        return false;
    }
    *bus = (*fpci >> 16) & 0xff;
    *devfn = (*fpci >> 8) & 0xff;
    *reg = (*fpci & 0xff) | (extended ? ((*fpci >> 16) & 0xf00) : 0);
    return true;
}

static MemTxResult mh2p_pcie_outbound_access(MH2PMachineState *s,
                                            hwaddr offset, uint64_t *value,
                                            unsigned size, bool write)
{
    hwaddr address = offset + 0x01004000;
    uint64_t fpci;
    unsigned window, type, bus, devfn, reg;

    if (!mh2p_pcie_config_target(s, address, size, &fpci, &window,
                                &type, &bus, &devfn, &reg)) {
        qemu_log_mask(LOG_UNIMP, "mh2p: unsupported/reset/clock-gated PCIe "
                      "outbound %s at 0x%" HWADDR_PRIx " size %u\n",
                      write ? "write" : "read", address, size);
        if (!write) {
            *value = 0;
        }
        return MEMTX_DECODE_ERROR;
    }
    if (type == 2) {
        uint32_t data = write ? *value : 0;
        MemTxResult result = mh2p_pcie_memory_access(
            s->pcie_transport, mh2p_pcie_root_ready(s),
            fpci, size, &data, write);

        if (!write) {
            *value = data & MAKE_64BIT_MASK(0, size * 8);
        }
        return result;
    }
    trace_mh2p_pcie_config(address, fpci, window, type, bus, devfn, reg,
                           size, write);
    if (s->pcie_transport && !devfn) {
        uint32_t buses = mh2p_pcie_value(s, 0x1018);
        unsigned secondary = (buses >> 8) & 0xff;
        unsigned subordinate = (buses >> 16) & 0xff;

        if (secondary && bus == secondary && bus <= subordinate) {
            uint32_t data = write ? *value : 0;
            MemTxResult result = mh2p_pcie_config_access(
                s->pcie_transport, mh2p_pcie_root_ready(s),
                reg, size, &data, write);

            if (!write) {
                *value = data & MAKE_64BIT_MASK(0, size * 8);
            }
            return result;
        }
    }
    if (!write) {
        *value = MAKE_64BIT_MASK(0, size * 8);
    }
    return MEMTX_OK;
}

static MemTxResult mh2p_pcie_outbound_read(void *opaque, hwaddr offset,
                                          uint64_t *value, unsigned size,
                                          MemTxAttrs attrs)
{
    return mh2p_pcie_outbound_access(opaque, offset, value, size, false);
}

static MemTxResult mh2p_pcie_outbound_write(void *opaque, hwaddr offset,
                                           uint64_t value, unsigned size,
                                           MemTxAttrs attrs)
{
    return mh2p_pcie_outbound_access(opaque, offset, &value, size, true);
}

static const MemoryRegionOps mh2p_pcie_outbound_ops = {
    .read_with_attrs = mh2p_pcie_outbound_read,
    .write_with_attrs = mh2p_pcie_outbound_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static MemTxResult mh2p_mselect_read(void *opaque, hwaddr offset,
                                    uint64_t *value, unsigned size,
                                    MemTxAttrs attrs)
{
    MH2PMachineState *s = opaque;

    *value = s->mselect_config;
    return MEMTX_OK;
}

static MemTxResult mh2p_mselect_write(void *opaque, hwaddr offset, uint64_t value,
                                     unsigned size, MemTxAttrs attrs)
{
    MH2PMachineState *s = opaque;

    if (value & ~BIT(5)) {
        qemu_log_mask(LOG_UNIMP, "mh2p: unsupported MSELECT configuration "
                      "0x%" PRIx64 "\n", value);
        return MEMTX_DECODE_ERROR;
    }
    s->mselect_config = value;
    return MEMTX_OK;
}

static const MemoryRegionOps mh2p_mselect_ops = {
    .read_with_attrs = mh2p_mselect_read,
    .write_with_attrs = mh2p_mselect_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static MemTxResult mh2p_dpaux_pad_read(void *opaque, hwaddr offset,
                                      uint64_t *value, unsigned size,
                                      MemTxAttrs attrs)
{
    MH2PMachineState *s = opaque;

    *value = s->dpaux_pad_control;
    return MEMTX_OK;
}

static MemTxResult mh2p_dpaux_pad_write(void *opaque, hwaddr offset,
                                       uint64_t value, unsigned size,
                                       MemTxAttrs attrs)
{
    MH2PMachineState *s = opaque;

    /* Only I2C6 mux selection and its SDA/SCL receiver enables are modeled. */
    if (value & ~0xc001u) {
        qemu_log_mask(LOG_UNIMP, "mh2p: unsupported DPAUX pad control "
                      "0x%" PRIx64 "\n", value);
        return MEMTX_DECODE_ERROR;
    }
    s->dpaux_pad_control = value;
    return MEMTX_OK;
}

static const MemoryRegionOps mh2p_dpaux_pad_ops = {
    .read_with_attrs = mh2p_dpaux_pad_read,
    .write_with_attrs = mh2p_dpaux_pad_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void mh2p_reset(void *opaque)
{
    MH2PMachineState *s = opaque;
    MachineState *ms = MACHINE(s);

    cpu_reset(CPU(s->cpu));
    s->cpu_reset_vector = 0;
    s->shared_semaphore = 0;
    memset(s->shared_mailbox, 0, sizeof(s->shared_mailbox));
    memset(s->cop_vectors, 0, sizeof(s->cop_vectors));
    s->halt_cop_events = 0;
    s->cpu_clock_stop = mh2p_secondary_powergates(s);
    s->cpu_reset_control = (s->cpu_clock_stop >> 8) |
                          (s->cpu_clock_stop >> 4) |
                          (s->cpu_clock_stop << 4);
    if (s->experimental_psci) {
        s->cpu_clock_stop = s->cpu_reset_control = 0;
    }
    for (unsigned i = 1; i < ms->smp.cpus; i++) {
        MH2PSecondaryCPU *core = &s->secondary[i - 1];

        cpu_reset(CPU(core->cpu));
        core->held = core->reset_pending = !s->experimental_psci;
        core->vector = 0;
    }
    memset(s->startup_timestamp, 0, sizeof(s->startup_timestamp));
    memset(s->clock_gates, 0, sizeof(s->clock_gates));
    memset(s->source_clocks, 0, sizeof(s->source_clocks));
    memset(s->extra_source_clocks, 0, sizeof(s->extra_source_clocks));
    timer_del(s->osc_detect_timer);
    s->osc_detect = 0;
    s->osc_detect_status = 0;
    s->system_clock[0] = 0x20000000; /* RUN on CLK_M */
    s->system_clock[1] = 0;
    s->system_clock[2] = 0;
    memset(s->pll_c, 0, sizeof(s->pll_c));
    memset(s->pll_cx, 0, sizeof(s->pll_cx));
    memset(s->pll_c4, 0, sizeof(s->pll_c4));
    memset(s->pll_re, 0, sizeof(s->pll_re));
    memset(s->pll_aud, 0, sizeof(s->pll_aud));
    s->pll_aud[2][0] = 0x10c;
    s->pll_aud[2][3] = 0x100;
    s->pll_d2[0] = 0x8010c;
    s->pll_d2[1] = 0;
    s->pll_d2[2] = 0x10000000;
    s->pll_d2[3] = s->pll_d2[4] = 0;
    memset(s->utmip_pll_config, 0, sizeof(s->utmip_pll_config));
    s->utmip_hw_control = 0;
    s->pllu_hw_control = 0;
    s->pllu_hw_enabled = false;
    s->xusb_pll_control = BIT(3) | BIT(0);
    s->xusb_pll_reference = 0;
    s->xusb_pcie_mux = 0;
    s->xusb_elpg = 0x07000000;
    s->xusb_hw_control = 0;
    s->xusb_hw_enabled = false;
    s->pll_e_base = 0;
    s->pll_e_misc = 0;
    s->pll_e_ss = 0;
    s->pll_e_aux = 0;
    s->pll_e_hw_enabled = false;
    s->ahb_mem = 0;
    s->pmc_control = 0;
    s->pmc_sec_disable2 = 0;
    s->secure_boot_csr = 0;
    s->clock_mask_arm = 0;
    memset(s->pmc_secure_scratch, 0, sizeof(s->pmc_secure_scratch));
    s->pex_dpd_request = s->pex_dpd_status = 0;
    s->mselect_config = 0;
    for (unsigned i = 0; i < ARRAY_SIZE(mh2p_pcie_registers); i++) {
        s->pcie_registers[i] = mh2p_pcie_registers[i].reset;
    }
    s->gpu_clamp = 1;
    mh2p_update_peripheral_resets(s);
    s->powergate_toggle = 0;
    s->powergate_status = MH2P_BOOT_POWERGATES;
    s->clamp_status = MH2P_PERIPHERAL_POWERGATES |
                      mh2p_secondary_powergates(s);
    if (s->experimental_psci) {
        /*
         * Emulated monitor handoff: CPU domains are supplied and unclamped;
         * secondaries remain parked until the standard PSCI CPU_ON call.
         */
        s->powergate_status |= mh2p_secondary_powergates(s);
        s->clamp_status &= ~mh2p_secondary_powergates(s);
    }
    s->mc_interrupt_mask = 0;
    s->mc_security_base = s->mc_security_size = 0;
    memory_region_set_enabled(&s->secure_carveout, false);
    for (unsigned bank = 0; bank < ARRAY_SIZE(s->legacy_enable); bank++) {
        /* Direct QNX boot inherits enabled gates; secure entry starts cold. */
        s->legacy_enable[bank] = s->experimental_el3 ? 0 : UINT32_MAX;
        s->legacy_force[bank] = 0;
        mh2p_legacy_update(s, bank);
    }
    memset(s->mc_hotreset, 0, sizeof(s->mc_hotreset));
    s->smmu_config = 0;
    s->smmu_tlb_config = 0x20000000;
    s->smmu_ptc_config = 0x2000003f;
    s->smmu_asid = 0;
    memset(s->smmu_ptb, 0, sizeof(s->smmu_ptb));
    memset(s->smmu_translation_enable, 0, sizeof(s->smmu_translation_enable));
    memset(s->smmu_group, 0, sizeof(s->smmu_group));
    s->smmu_flush_hi = 0;
    s->dpaux_pad_control = 0;
    memset(s->sd_clock, 0, sizeof(s->sd_clock));
    memset(s->sd_misc, 0, sizeof(s->sd_misc));
    memset(s->sd_pad, 0, sizeof(s->sd_pad));
    memset(s->sd_calibration, 0, sizeof(s->sd_calibration));
    s->cpu->env.regs[0] = 0;
    s->cpu->env.regs[1] = 0;
    s->cpu->env.regs[2] = ms->dtb ? MH2P_DTB : MH2P_ATAGS;
    cpu_set_pc(CPU(s->cpu), MH2P_STARTUP);
    mh2p_update_pcie_sideband(s);
}

static void mh2p_mnand_handoff(void *opaque)
{
    MH2PMachineState *s = opaque;
    SDRequest request = { .cmd = 1, .arg = 0x40ff8000 };
    uint8_t response[4];

    /* Reconstruct the in-progress power-up expected by stock skipinit. */
    if (sdbus_do_command(s->mnand_bus, &request, response,
                        sizeof(response)) != sizeof(response) ||
        (ldl_be_p(response) & 0x80000000) ||
        !(ldl_be_p(response) & 0x00ff8000)) {
        error_report("MH2P mNAND power-up handoff failed");
        exit(EXIT_FAILURE);
    }
}

static void mh2p_machine_done(Notifier *notifier, void *data)
{
    MH2PMachineState *s = container_of(notifier, MH2PMachineState,
                                      machine_done);

    /* Run the bootloader handoff after the sysbus/card reset. */
    qemu_register_reset(mh2p_mnand_handoff, s);
}

static void mh2p_init(MachineState *ms)
{
    MH2PMachineState *s = (MH2PMachineState *)ms;
    MemoryRegion *sysmem = get_system_memory();
    DeviceState *gic, *timer, *gpio, *snor, *host1x, *gpu, *hdmi, *sor, *vic, *se;
    static const hwaddr uart_base[] = {
        0x70006000, 0x70006040, 0x70006200, 0x70006300,
    };
    static const unsigned uart_irq[] = { 36, 37, 46, 90 };
    static const unsigned timer_irq[] = { 0, 1, 41, 42, 121, 122 };
    static const unsigned gpio_irq[] = { 32, 33, 34, 35, 55, 87, 89, 125 };
    static const hwaddr sd_base[] = { 0x700b0600, 0x700b0400 };
    static const unsigned sd_irq[] = { 31, 19 };
    g_autofree char *cmdline = NULL;
    g_autofree uint32_t *tags = NULL;
    size_t cmdline_words, tag_words;

    s->osc_detect_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                       mh2p_osc_detect_done, s);
    if (!ms->kernel_filename) {
        error_report("mh2p requires -kernel <decompressed QNX startup image>");
        exit(EXIT_FAILURE);
    }
    if (ms->ram_size < 256 * MiB || ms->ram_size > 2 * GiB) {
        error_report("mh2p requires between 256 MiB and 2 GiB of RAM");
        exit(EXIT_FAILURE);
    }
    if (s->experimental_el3 && (s->experimental_psci || ms->smp.cpus != 1)) {
        error_report("experimental-el3 requires one CPU and "
                     "experimental-psci=off");
        exit(EXIT_FAILURE);
    }
    memory_region_add_subregion(sysmem, MH2P_RAM_BASE, ms->ram);
    memory_region_init_io(&s->secure_carveout, OBJECT(ms),
                          &mh2p_carveout_ops, s, "mh2p-tzdram-denied", 1);
    memory_region_set_enabled(&s->secure_carveout, false);
    memory_region_add_subregion_overlap(sysmem, MH2P_RAM_BASE,
                                        &s->secure_carveout, 1);
    if (s->experimental_el3) {
        memory_region_init(&s->secure_memory, OBJECT(ms),
                           "mh2p-secure-memory", UINT64_MAX);
        memory_region_add_subregion_overlap(&s->secure_memory, 0, sysmem, -1);
        memory_region_init_alias(&s->secure_ram, OBJECT(ms),
                                 "mh2p-secure-ram", ms->ram, 0, ms->ram_size);
        memory_region_add_subregion(&s->secure_memory, MH2P_RAM_BASE,
                                    &s->secure_ram);
    }
    memory_region_init_ram(&s->iram, NULL, "mh2p.iram", 256 * KiB,
                           &error_fatal);
    memory_region_add_subregion(sysmem, 0x40000000, &s->iram);
    /* Unknown accesses fault instead of pretending a peripheral works. */
    memory_region_init_io(&s->boot_regs, OBJECT(ms), &mh2p_boot_ops, s,
                          "mh2p-boot-registers", 0x11000000);
    memory_region_add_subregion_overlap(sysmem, 0x60000000, &s->boot_regs, -1);
    memory_region_init_io(&s->pcie_config, OBJECT(ms), &mh2p_pcie_ops, s,
                          "mh2p-pcie-local-config", 0x4000);
    memory_region_add_subregion(sysmem, 0x01000000, &s->pcie_config);
    memory_region_init_io(&s->pcie_outbound, OBJECT(ms),
                          &mh2p_pcie_outbound_ops, s,
                          "mh2p-pcie-outbound", 0x40000000 - 0x01004000);
    memory_region_add_subregion(sysmem, 0x01004000, &s->pcie_outbound);
    memory_region_init_io(&s->mselect, OBJECT(ms), &mh2p_mselect_ops, s,
                          "mh2p-mselect-config", 4);
    memory_region_add_subregion(sysmem, 0x50060000, &s->mselect);
    memory_region_init_io(&s->dpaux_pad, OBJECT(ms), &mh2p_dpaux_pad_ops, s,
                          "mh2p-dpaux-i2c-pad", 4);
    memory_region_add_subregion(sysmem, 0x545c0124, &s->dpaux_pad);

    for (unsigned i = 0; i < ms->smp.cpus; i++) {
        ARMCPU *cpu = ARM_CPU(object_new(ms->cpu_type));
        g_autofree char *name = i ? g_strdup_printf("cpu%u", i) :
                                   g_strdup("cpu");

        if (i) {
            s->secondary[i - 1].cpu = cpu;
        } else {
            s->cpu = cpu;
        }
        object_property_add_child(OBJECT(ms), name, OBJECT(cpu));
        qdev_prop_set_bit(DEVICE(cpu), "has_el3", s->experimental_el3);
        if (s->experimental_el3) {
            object_property_set_link(OBJECT(cpu), "secure-memory",
                                     OBJECT(&s->secure_memory), &error_fatal);
        }
        qdev_prop_set_bit(DEVICE(cpu), "has_el2", false);
        qdev_prop_set_bit(DEVICE(cpu), "start-powered-off", i != 0);
        if (s->experimental_psci) {
            qdev_prop_set_uint32(DEVICE(cpu), "psci-conduit",
                                 QEMU_PSCI_CONDUIT_SMC);
        }
        qdev_prop_set_uint64(DEVICE(cpu), "reset-cbar", 0x50040000);
        qdev_prop_set_uint64(DEVICE(cpu), "cntfrq", 12000000);
        qdev_realize(DEVICE(cpu), NULL, &error_fatal);
        object_unref(OBJECT(cpu));
    }

    gic = qdev_new(TYPE_A15MPCORE_PRIV);
    object_property_add_child(OBJECT(ms), "interrupt-controller", OBJECT(gic));
    qdev_prop_set_uint32(gic, "num-cpu", ms->smp.cpus);
    qdev_prop_set_uint32(gic, "num-irq", 192);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(gic), &error_fatal);
    s->gic = gic;
    s->legacy_irq = qemu_allocate_irqs(mh2p_legacy_irq, s, 160);
    s->pcie_msi_irq = s->legacy_irq[99];
    sysbus_mmio_map(SYS_BUS_DEVICE(gic), 0, 0x50040000);
    for (unsigned i = 0; i < ms->smp.cpus; i++) {
        ARMCPU *cpu = i ? s->secondary[i - 1].cpu : s->cpu;

        sysbus_connect_irq(SYS_BUS_DEVICE(gic), i,
                           qdev_get_gpio_in(DEVICE(cpu), ARM_CPU_IRQ));
        sysbus_connect_irq(SYS_BUS_DEVICE(gic), i + ms->smp.cpus,
                           qdev_get_gpio_in(DEVICE(cpu), ARM_CPU_FIQ));
    }

    for (unsigned i = 0; i < ARRAY_SIZE(uart_base); i++) {
        serial_mm_init(sysmem, uart_base[i], 2,
                       s->legacy_irq[uart_irq[i]], 115200,
                       serial_hd(i), DEVICE_LITTLE_ENDIAN);
    }
    /* T124 retains the Tegra30 GPIO layout and eight bank interrupts. */
    gpio = qdev_new(TYPE_TEGRA30_GPIO);
    object_property_add_child(OBJECT(ms), "gpio", OBJECT(gpio));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(gpio), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(gpio), 0, 0x6000d000);
    for (unsigned i = 0; i < ARRAY_SIZE(gpio_irq); i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(gpio), i,
                           s->legacy_irq[gpio_irq[i]]);
    }
    /* Original devg-nvgpio maps active-low usb_overcnt2 to V1. */
    qemu_set_irq(qdev_get_gpio_in(gpio, TEGRA30_GPIO_NR(21, 1)), 1);
    snor = qdev_new(TYPE_TEGRA30_SNOR);
    object_property_add_child(OBJECT(ms), "snor", OBJECT(snor));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(snor), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(snor), 0, 0x70009000);
    sysbus_connect_irq(SYS_BUS_DEVICE(snor), 0, s->legacy_irq[96]);

    se = qdev_new(TYPE_TEGRA124_SE);
    object_property_add_child(OBJECT(ms), "se", OBJECT(se));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(se), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(se), 0, 0x70012000);
    sysbus_mmio_map(SYS_BUS_DEVICE(se), 1, 0x7c010000);
    sysbus_connect_irq(SYS_BUS_DEVICE(se), 0, s->legacy_irq[58]);

    host1x = qdev_new(TYPE_TEGRA124_HOST1X);
    object_property_add_child(OBJECT(ms), "host1x", OBJECT(host1x));
    memory_region_init_iommu(&s->host1x_iommu, sizeof(s->host1x_iommu),
                             "mh2p-host1x-iommu", OBJECT(ms),
                             "host1x-dma", 1ULL << 32);
    object_property_set_link(OBJECT(host1x), "dma",
                             OBJECT(&s->host1x_iommu), &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(host1x), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(host1x), 0, 0x50000000);
    sysbus_connect_irq(SYS_BUS_DEVICE(host1x), 0, s->legacy_irq[65]);
    s->host1x_reset = qdev_get_gpio_in_named(host1x, "reset", 0);

    vic = qdev_new(TYPE_TEGRA124_VIC);
    object_property_add_child(OBJECT(ms), "vic", OBJECT(vic));
    memory_region_init_iommu(&s->vic_iommu, sizeof(s->vic_iommu),
                             "mh2p-vic-iommu", OBJECT(ms),
                             "vic-dma", 1ULL << 32);
    object_property_set_link(OBJECT(vic), "dma",
                             OBJECT(&s->vic_iommu), &error_fatal);
    object_property_set_link(OBJECT(vic), "host1x", OBJECT(host1x),
                             &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(vic), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(vic), 0, 0x54340000);
    sysbus_connect_irq(SYS_BUS_DEVICE(vic), 0, s->legacy_irq[72]);
    s->vic_reset = qdev_get_gpio_in_named(vic, "reset", 0);
    s->vic_clock = qdev_get_gpio_in_named(vic, "clock-enable", 0);

    hdmi = qdev_new(TYPE_TEGRA124_HDMI);
    object_property_add_child(OBJECT(ms), "hdmi", OBJECT(hdmi));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(hdmi), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(hdmi), 0, 0x54280000);
    s->hdmi_reset = qdev_get_gpio_in_named(hdmi, "reset", 0);

    sor = qdev_new(TYPE_TEGRA124_SOR);
    object_property_add_child(OBJECT(ms), "sor", OBJECT(sor));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(sor), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(sor), 0, 0x54540000);
    s->sor_reset = qdev_get_gpio_in_named(sor, "reset", 0);
    s->sor_clock = qdev_get_gpio_in_named(sor, "clock-enable", 0);

    for (unsigned i = 0; i < ARRAY_SIZE(s->dc_reset); i++) {
        DeviceState *dc = qdev_new(TYPE_TEGRA124_DC);
        g_autofree char *name = g_strdup_printf("display%u", i);
        g_autofree char *clock_name = g_strdup_printf("display%u-clock", i);

        dc->id = g_strdup(name);
        object_property_add_child(OBJECT(ms), name, OBJECT(dc));
        s->display_clock[i] = clock_new(OBJECT(ms), clock_name);
        qdev_connect_clock_in(dc, "dc", s->display_clock[i]);
        object_property_set_link(OBJECT(dc), "host1x", OBJECT(host1x),
                                 &error_fatal);
        for (unsigned window = 0; window < 3; window++) {
            MH2PDisplayIOMMU *d = &s->display_iommu[i][window];
            g_autofree char *dma_name =
                g_strdup_printf("display%u-window%u-dma", i, window);
            g_autofree char *property = g_strdup_printf("dma-%c", 'a' + window);

            memory_region_init_iommu(&d->parent, sizeof(*d),
                                     "mh2p-display-iommu", OBJECT(ms),
                                     dma_name, 1ULL << 32);
            d->machine = s;
            d->head = i;
            d->window = window;
            object_property_set_link(OBJECT(dc), property, OBJECT(&d->parent),
                                     &error_fatal);
        }
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dc), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(dc), 0, 0x54200000 + i * 0x40000);
        sysbus_connect_irq(SYS_BUS_DEVICE(dc), 0,
                           s->legacy_irq[73 + i]);
        s->dc_reset[i] = qdev_get_gpio_in_named(dc, "reset", 0);
        qdev_connect_gpio_out_named(dc, "frame", 0,
                                    qdev_get_gpio_in_named(hdmi, "frame", i));
    }

    gpu = qdev_new(TYPE_GK20A);
    object_property_add_child(OBJECT(ms), "gpu", OBJECT(gpu));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(gpu), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(gpu), 0, 0x57000000);
    glbridge_create(OBJECT(ms));
    s->gpu_reset = qdev_get_gpio_in_named(gpu, "reset", 0);

    for (unsigned i = 0; i < ARRAY_SIZE(mh2p_i2c); i++) {
        DeviceState *i2c = qdev_new(TYPE_TEGRA124_I2C);
        g_autofree char *name = g_strdup_printf("i2c%u", i + 1);

        object_property_add_child(OBJECT(ms), name, OBJECT(i2c));
        sysbus_realize_and_unref(SYS_BUS_DEVICE(i2c), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(i2c), 0, mh2p_i2c[i].base);
        sysbus_connect_irq(SYS_BUS_DEVICE(i2c), 0,
                           s->legacy_irq[mh2p_i2c[i].irq]);
        s->i2c_reset[i] = qdev_get_gpio_in_named(i2c, "reset", 0);
    }

    for (unsigned i = 0; i < ARRAY_SIZE(mh2p_spi); i++) {
        DeviceState *spi = qdev_new(TYPE_TEGRA124_SPI);
        g_autofree char *name = g_strdup_printf("spi%u", i + 1);

        object_property_add_child(OBJECT(ms), name, OBJECT(spi));
        sysbus_realize_and_unref(SYS_BUS_DEVICE(spi), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(spi), 0, mh2p_spi[i].base);
        sysbus_connect_irq(SYS_BUS_DEVICE(spi), 0,
                           s->legacy_irq[mh2p_spi[i].irq]);
        s->spi_reset[i] = qdev_get_gpio_in_named(spi, "reset", 0);
    }

    for (unsigned i = 0; i < ARRAY_SIZE(sd_base); i++) {
        DeviceState *host = qdev_new(TYPE_SYSBUS_SDHCI);
        DriveInfo *drive = drive_get(IF_SD, 0, i);

        object_property_add_child(OBJECT(ms), i ? "sdmmc3" : "sdmmc4",
                                  OBJECT(host));
        qdev_prop_set_uint8(host, "sd-spec-version", 3);
        qdev_prop_set_uint32(host, "capareg", 0x05780a8a);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(host), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(host), 0, sd_base[i]);
        sysbus_connect_irq(SYS_BUS_DEVICE(host), 0,
                           s->legacy_irq[sd_irq[i]]);
        if (drive) {
            DeviceState *card = qdev_new(TYPE_EMMC);

            qdev_prop_set_drive(card, "drive", blk_by_legacy_dinfo(drive));
            if (i == 0) {
                qdev_prop_set_uint32(card, "ocr-power-delay-ns", 100000000);
                s->mnand_bus = SD_BUS(qdev_get_child_bus(host, "sd-bus"));
            }
            qdev_realize_and_unref(card, qdev_get_child_bus(host, "sd-bus"),
                                   &error_fatal);
        }
    }

    if (ms->firmware) {
        QDict *options = qdict_new();
        BlockBackend *blk;
        DeviceState *flash = qdev_new(TYPE_PFLASH_CFI02);

        qdict_put_str(options, "driver", "raw");
        blk = blk_new_open(ms->firmware, NULL, options,
                           BDRV_O_RDWR | BDRV_O_SNAPSHOT, &error_fatal);
        if (blk_getlength(blk) != 64 * MiB) {
            error_report("MH2P -bios NOR image must be exactly 64 MiB");
            exit(EXIT_FAILURE);
        }
        qdev_prop_set_drive(flash, "drive", blk);
        blk_unref(blk);
        qdev_prop_set_uint32(flash, "num-blocks", 512);
        qdev_prop_set_uint32(flash, "sector-length", 128 * KiB);
        qdev_prop_set_uint8(flash, "width", 2);
        qdev_prop_set_uint16(flash, "unlock-addr0", 0x555);
        qdev_prop_set_uint16(flash, "unlock-addr1", 0x2aa);
        /* Spansion S29GL512S, as identified by devf-generic-tegra2. */
        qdev_prop_set_uint16(flash, "id0", 0x0001);
        qdev_prop_set_uint16(flash, "id1", 0x227e);
        qdev_prop_set_uint16(flash, "id2", 0x2223);
        qdev_prop_set_uint16(flash, "id3", 0x2201);
        qdev_prop_set_uint32(flash, "write-buffer-size", 512);
        qdev_prop_set_bit(flash, "mirrorbit-eclipse", true);
        qdev_prop_set_string(flash, "name", "mh2p-nor");
        sysbus_realize_and_unref(SYS_BUS_DEVICE(flash), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(flash), 0, 0x48000000);
    }
    /* The six legacy timer channels retain the T30 register layout on T124. */
    timer = qdev_new(TYPE_TEGRA30_TIMER);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(timer), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(timer), 0, 0x60005000);
    for (unsigned i = 0; i < ARRAY_SIZE(timer_irq); i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(timer), i,
                           s->legacy_irq[timer_irq[i]]);
    }
    if (load_image_targphys(ms->kernel_filename, MH2P_STARTUP,
                           MH2P_DTB - MH2P_STARTUP, NULL) <= 0) {
        error_report("Cannot load MH2P startup image '%s'",
                     ms->kernel_filename);
        exit(EXIT_FAILURE);
    }
    if (ms->dtb && load_image_targphys(ms->dtb, MH2P_DTB, MiB, NULL) <= 0) {
        error_report("Cannot load MH2P device tree '%s'", ms->dtb);
        exit(EXIT_FAILURE);
    }
    cmdline = g_strdup_printf(
        "nvsku=699-61857-5017-202 SkuVer=00 "
        "mem=%" PRIu64 "M@2048M nr_cpus=%u lp=0 %s",
        (uint64_t)(ms->ram_size / MiB), ms->smp.cpus,
        ms->kernel_cmdline ? ms->kernel_cmdline : "");
    cmdline_words = (strlen(cmdline) + 1 + 3) / 4;
    tag_words = 5 + 4 + 2 + cmdline_words + 2;
    if (tag_words * 4 > MH2P_STARTUP - MH2P_ATAGS) {
        error_report("MH2P boot command line is too long");
        exit(EXIT_FAILURE);
    }
    tags = g_new0(uint32_t, tag_words);
    tags[0] = cpu_to_le32(5);
    tags[1] = cpu_to_le32(0x54410001); /* ATAG_CORE */
    tags[5] = cpu_to_le32(4);
    tags[6] = cpu_to_le32(0x54410002); /* ATAG_MEM */
    tags[7] = cpu_to_le32(ms->ram_size);
    tags[8] = cpu_to_le32(MH2P_RAM_BASE);
    tags[9] = cpu_to_le32(cmdline_words + 2);
    tags[10] = cpu_to_le32(0x54410009); /* ATAG_CMDLINE */
    memcpy(&tags[11], cmdline, strlen(cmdline) + 1);
    rom_add_blob_fixed("mh2p-boot-tags", tags, tag_words * 4, MH2P_ATAGS);
    s->pcie_sideband = mh2p_pcie_sideband_create(OBJECT(ms), true);
    s->pcie_transport = mh2p_pcie_config_create(
        OBJECT(ms), "pcie-config", s->pcie_fd, NULL, NULL, NULL, NULL);
    s->pcie_outbound_transport = mh2p_pcie_config_create(
        OBJECT(ms), "pcie-outbound", s->pcie_outbound_fd,
        mh2p_pcie_memory_ready, NULL, mh2p_pcie_inbound_memory, s);
    qemu_register_reset(mh2p_reset, s);
    if (s->mnand_bus) {
        s->machine_done.notify = mh2p_machine_done;
        qemu_add_machine_init_done_notifier(&s->machine_done);
    }
}

static char *mh2p_get_pcie_fd(Object *obj, Error **errp)
{
    return g_strdup(((MH2PMachineState *)obj)->pcie_fd);
}

static void mh2p_set_pcie_fd(Object *obj, const char *value, Error **errp)
{
    MH2PMachineState *s = (MH2PMachineState *)obj;

    g_free(s->pcie_fd);
    s->pcie_fd = g_strdup(value);
}

static char *mh2p_get_pcie_outbound_fd(Object *obj, Error **errp)
{
    return g_strdup(((MH2PMachineState *)obj)->pcie_outbound_fd);
}

static void mh2p_set_pcie_outbound_fd(Object *obj, const char *value,
                                     Error **errp)
{
    MH2PMachineState *s = (MH2PMachineState *)obj;

    g_free(s->pcie_outbound_fd);
    s->pcie_outbound_fd = g_strdup(value);
}

static bool mh2p_get_experimental_psci(Object *obj, Error **errp)
{
    return ((MH2PMachineState *)obj)->experimental_psci;
}

static void mh2p_set_experimental_psci(Object *obj, bool value, Error **errp)
{
    ((MH2PMachineState *)obj)->experimental_psci = value;
}

static bool mh2p_get_experimental_el3(Object *obj, Error **errp)
{
    return ((MH2PMachineState *)obj)->experimental_el3;
}

static void mh2p_set_experimental_el3(Object *obj, bool value, Error **errp)
{
    ((MH2PMachineState *)obj)->experimental_el3 = value;
}

static char *mh2p_get_fuse_security_mode(Object *obj, Error **errp)
{
    unsigned mode = ((MH2PMachineState *)obj)->fuse_security_mode;

    return g_strdup(mode == 2 ? "production" :
                    mode == 1 ? "development" : "unknown");
}

static void mh2p_set_fuse_security_mode(Object *obj, const char *value,
                                       Error **errp)
{
    MH2PMachineState *s = (MH2PMachineState *)obj;

    if (!strcmp(value, "unknown")) {
        s->fuse_security_mode = 0;
    } else if (!strcmp(value, "development")) {
        s->fuse_security_mode = 1;
    } else if (!strcmp(value, "production")) {
        s->fuse_security_mode = 2;
    } else {
        error_setg(errp, "experimental-fuse-security-mode requires "
                   "unknown, development or production");
    }
}

static void mh2p_machine_init(MachineClass *mc)
{
    static const char * const cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-a15"), NULL,
    };

    mc->desc = "MH2P Alpine T124 (experimental QNX direct start)";
    mc->init = mh2p_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a15");
    mc->valid_cpu_types = cpu_types;
    mc->default_cpus = mc->min_cpus = 1;
    mc->max_cpus = 4;
    mc->default_ram_size = 2 * GiB;
    mc->default_ram_id = "mh2p.ram";
    object_class_property_add_bool(OBJECT_CLASS(mc), "experimental-psci",
                                   mh2p_get_experimental_psci,
                                   mh2p_set_experimental_psci);
    object_class_property_add_bool(OBJECT_CLASS(mc), "experimental-el3",
                                   mh2p_get_experimental_el3,
                                   mh2p_set_experimental_el3);
    object_class_property_add_str(OBJECT_CLASS(mc),
                                  "experimental-fuse-security-mode",
                                  mh2p_get_fuse_security_mode,
                                  mh2p_set_fuse_security_mode);
    object_class_property_add_str(OBJECT_CLASS(mc), "pcie-fd",
                                  mh2p_get_pcie_fd, mh2p_set_pcie_fd);
    object_class_property_add_str(OBJECT_CLASS(mc), "pcie-outbound-fd",
                                  mh2p_get_pcie_outbound_fd,
                                  mh2p_set_pcie_outbound_fd);
}

DEFINE_MACHINE_EXTENDED("mh2p", MACHINE, MH2PMachineState,
                        mh2p_machine_init, false, false, NULL)

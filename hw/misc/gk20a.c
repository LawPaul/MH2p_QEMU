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
#include "hw/core/irq.h"
#include "hw/misc/gk20a.h"

/*
 * PMU falcon data memory (model choice: 32 KiB, HWCFG reports it).  It must
 * be below 64 KiB: the driver checks that DMEMC's 16-bit auto-increment
 * offset ends at "top of DMEM" after writing its boot arguments there.
 */
#define GK20A_PMU_DMEM_SIZE 0x8000

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
    /* TRIM_SYS GPCPLL (GPU core PLL): CFG, COEFF, CFG2, NDIV_SLOWDOWN, SEL_VCO */
    uint32_t gpcpll_cfg, gpcpll_coeff, gpcpll_cfg2, gpcpll_slowdown, sel_vco;
    uint32_t mc_enable;
    uint32_t mc_enable_pb;
    uint32_t mc_elpg_enable;
    uint32_t mmu_page_size;
    uint32_t engine_config[ARRAY_SIZE(gk20a_engine_config)];
    uint32_t pri_timeout;
    bool ring_started;
    uint32_t ring_sys_decode;
    /* experimental-ctxsw: FECS/GPCCS context-switch controller protocol */
    bool experimental_ctxsw;
    uint32_t ctxsw_golden_size;
    uint32_t ctxsw_zcull_size;
    uint32_t ctxsw_pm_size;
    uint32_t ctxsw_reglist_size;
    uint32_t fecs_cpuctl;
    uint32_t gpccs_cpuctl;
    uint32_t fecs_falcon_mailbox[2];
    uint32_t fecs_ctxsw_mailbox[16];
    uint32_t fecs_method_data;
    uint32_t fecs_current_ctx;
    uint32_t fecs_new_ctx;
    /* experimental-pmu: PMU falcon message protocol and MC interrupts */
    bool experimental_pmu;
    qemu_irq irq_stall;
    qemu_irq irq_nonstall;
    uint32_t mc_intr_en[2];
    uint32_t mc_intr_mask[2];
    uint8_t pmu_dmem[GK20A_PMU_DMEM_SIZE];
    uint32_t pmu_dmemc[4];
    uint32_t pmu_cpuctl;
    uint32_t pmu_irqstat;
    uint32_t pmu_irqmask;
    uint32_t pmu_irqdest;
    uint32_t pmu_irqmode;
    uint32_t pmu_mailbox[2];
    uint32_t pmu_queue_head[4];
    uint32_t pmu_queue_tail[4];
    uint32_t pmu_msgq_head;
    uint32_t pmu_msgq_tail;
    uint32_t pmu_mutex[16];
    uint32_t pmu_next_mutex_id;
    uint32_t pmu_regs[0x400];   /* plain storage for other PMU registers */
};

/*
 * Context-switch controller (FECS) protocol, as the original nvhost driver
 * (a QNX port of NVIDIA's gk20a gr_gk20a.c) drives it:
 *   - it DMA-boots FECS and GPCCS (DMACTL, DMEM bootloader args,
 *     BOOTVEC, CPUCTL.STARTCPU) and waits for CTXSW_MAILBOX(0) == 1
 *     ("init complete");
 *   - each method is CTXSW_MAILBOX_CLEAR(0) = clr, METHOD_DATA = data,
 *     METHOD_PUSH = method; it then polls CTXSW_MAILBOX(0) against the
 *     method's ok/fail condition.
 * Only the controller's answers are modelled; the microcode itself is not
 * run.  The context image sizes are model parameters: on hardware the
 * microcode derives them from its register lists.
 */
#define FECS_BASE 0x409000
#define GPCCS_BASE 0x41a000
#define FALCON_CPUCTL 0x100
#define FALCON_CPUCTL_STARTCPU 0x2
#define FALCON_CPUCTL_HALTED 0x10
#define FALCON_DMACTL 0x10c
#define FECS_CTXSW_MAILBOX(i) (0x409800 + 4 * (i))
#define FECS_CTXSW_MAILBOX_CLEAR(i) (0x409840 + 4 * (i))
#define FECS_METHOD_DATA 0x409500
#define FECS_METHOD_PUSH 0x409504
#define FECS_CURRENT_CTX 0x409b00
#define FECS_NEW_CTX 0x409b04
#define FECS_ARB_CTX_CMD 0x409a10
#define GK20A_MC_ENABLE_PGRAPH 0x1000

static void gk20a_ctxsw_reset(GK20AState *s)
{
    s->fecs_cpuctl = FALCON_CPUCTL_HALTED;
    s->gpccs_cpuctl = FALCON_CPUCTL_HALTED;
    memset(s->fecs_falcon_mailbox, 0, sizeof(s->fecs_falcon_mailbox));
    memset(s->fecs_ctxsw_mailbox, 0, sizeof(s->fecs_ctxsw_mailbox));
    s->fecs_method_data = 0;
    s->fecs_current_ctx = 0;
    s->fecs_new_ctx = 0;
}

static bool gk20a_ctxsw_active(GK20AState *s)
{
    return s->experimental_ctxsw && (s->mc_enable & GK20A_MC_ENABLE_PGRAPH);
}

static void gk20a_fecs_method(GK20AState *s, uint32_t method)
{
    uint32_t *mb0 = &s->fecs_ctxsw_mailbox[0];

    switch (method) {
    case 0x03: /* BIND_POINTER: ok bit 4, fail bit 5 */
        *mb0 |= 0x10;
        break;
    case 0x09: /* WFI_GOLDEN_SAVE: ok bit 0, fail bit 1 */
        *mb0 |= 0x1;
        break;
    case 0x10: /* DISCOVER_IMAGE_SIZE */
        *mb0 = s->ctxsw_golden_size;
        break;
    case 0x16: /* DISCOVER_ZCULL_IMAGE_SIZE */
        *mb0 = s->ctxsw_zcull_size;
        break;
    case 0x25: /* DISCOVER_PM_IMAGE_SIZE */
        *mb0 = s->ctxsw_pm_size;
        break;
    case 0x30: /* DISCOVER_REGLIST_IMAGE_SIZE */
        *mb0 = s->ctxsw_reglist_size;
        break;
    case 0x31: /* SET_REGLIST_BIND_INSTANCE (instance in mailbox 4) */
    case 0x32: /* SET_REGLIST_VIRTUAL_ADDRESS (address in mailbox 4) */
        *mb0 = 1; /* ok: mailbox 0 == 1 */
        break;
    case 0x21: /* SET_WATCHDOG_TIMEOUT: no reply is awaited */
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "gk20a: FECS method 0x%x data 0x%x "
                      "(no reply modelled)\n", method, s->fecs_method_data);
        return;
    }
    qemu_log_mask(LOG_UNIMP, "gk20a: FECS method 0x%x data 0x%x -> "
                  "mailbox0 0x%x\n", method, s->fecs_method_data, *mb0);
}

static bool gk20a_ctxsw_read(GK20AState *s, hwaddr offset, uint64_t *value)
{
    if (!gk20a_ctxsw_active(s)) {
        return false;
    }
    switch (offset) {
    case FECS_BASE + FALCON_CPUCTL:
        *value = s->fecs_cpuctl;
        return true;
    case GPCCS_BASE + FALCON_CPUCTL:
        *value = s->gpccs_cpuctl;
        return true;
    case FECS_BASE + FALCON_DMACTL: /* memory scrubbing already finished */
    case GPCCS_BASE + FALCON_DMACTL:
        *value = 0;
        return true;
    case FECS_BASE + 0x40:
    case FECS_BASE + 0x44:
        *value = s->fecs_falcon_mailbox[(offset - FECS_BASE - 0x40) / 4];
        return true;
    case FECS_CTXSW_MAILBOX(0) ... FECS_CTXSW_MAILBOX(15):
        *value = s->fecs_ctxsw_mailbox[(offset - FECS_CTXSW_MAILBOX(0)) / 4];
        return true;
    case FECS_METHOD_DATA:
        *value = s->fecs_method_data;
        return true;
    case FECS_CURRENT_CTX:
        *value = s->fecs_current_ctx;
        return true;
    case FECS_NEW_CTX:
        *value = s->fecs_new_ctx;
        return true;
    case FECS_ARB_CTX_CMD: /* the arbiter completes commands at once */
        *value = 0;
        return true;
    /*
     * GR unit configuration of a GK20A (Tegra K1): one GPC with one TPC,
     * one zcull bank and one PES, one FBP (public nvgpu hw_*_gk20a.h).
     */
    case 0x22430: /* TOP_NUM_GPCS */
    case 0x22434: /* TOP_TPC_PER_GPC */
    case 0x22438: /* TOP_NUM_FBPS */
        *value = 1;
        return true;
    case 0x502608: /* GPC0_FS_GPC: available TPCs 4:0, zculls 20:16 */
        *value = 0x00010001;
        return true;
    case 0x500c30: /* GPC0_GPM_PD_PES_TPC_ID_MASK(0): PES 0 owns TPC 0 */
        *value = 1;
        return true;
    case 0x17e8dc:
        /*
         * LTCS_LTSS_CBC_PARAM: comptags per cache line 15:0, cache line
         * size 27:24 (512 << n), slices per FBP 31:28.  Model choice:
         * 4 comptags, 1 KiB lines, 2 slices.
         */
        *value = 0x21000004;
        return true;
    case 0x50469c:
        /*
         * GPC0_TPC0_SM_ARCH: warp count 7:0, SPA version 11:8.  The driver
         * accepts only SPA 0xc (Kepler LP, GK20A's SM 3.2); any other value
         * prints "Unknown SM version".  Kepler SMs hold 64 warps.
         */
        *value = 0x00000c40;
        return true;
    default:
        return false;
    }
}

static bool gk20a_ctxsw_write(GK20AState *s, hwaddr offset, uint32_t value)
{
    if (!gk20a_ctxsw_active(s)) {
        return false;
    }
    switch (offset) {
    case FECS_BASE + FALCON_CPUCTL:
        if (value & FALCON_CPUCTL_STARTCPU) {
            /* The booted microcode reports "init complete" in mailbox 0. */
            s->fecs_cpuctl = 0;
            s->fecs_ctxsw_mailbox[0] = 1;
            qemu_log_mask(LOG_UNIMP, "gk20a: FECS started\n");
        }
        return true;
    case GPCCS_BASE + FALCON_CPUCTL:
        if (value & FALCON_CPUCTL_STARTCPU) {
            s->gpccs_cpuctl = 0;
        }
        return true;
    case FECS_BASE + 0x40:
    case FECS_BASE + 0x44:
        s->fecs_falcon_mailbox[(offset - FECS_BASE - 0x40) / 4] = value;
        return true;
    case FECS_CTXSW_MAILBOX(0) ... FECS_CTXSW_MAILBOX(15):
        s->fecs_ctxsw_mailbox[(offset - FECS_CTXSW_MAILBOX(0)) / 4] = value;
        return true;
    case FECS_CTXSW_MAILBOX_CLEAR(0) ... FECS_CTXSW_MAILBOX_CLEAR(15):
        s->fecs_ctxsw_mailbox[(offset - FECS_CTXSW_MAILBOX_CLEAR(0)) / 4] &=
            ~value;
        return true;
    case FECS_METHOD_DATA:
        s->fecs_method_data = value;
        return true;
    case FECS_METHOD_PUSH:
        gk20a_fecs_method(s, value & 0xfff);
        return true;
    case FECS_CURRENT_CTX:
        s->fecs_current_ctx = value;
        return true;
    case FECS_NEW_CTX:
        s->fecs_new_ctx = value;
        return true;
    default:
        return false;
    }
}

/*
 * PMU (power-management falcon) message protocol, as the original nvhost
 * driver (pmu_gk20a.c; gpmu_ucode.bin interface 0x01013663) drives it.  The
 * PMU microcode is not run.  When the driver starts the PMU, the model posts
 * the INIT message the booted microcode sends (unit 7, type 0): the DMEM
 * layout of the five queues and of the driver-managed DMEM area.  The driver
 * creates its queue locks only on that message, and destroys them whenever
 * it powers the GPU off.  Commands queued by the driver are logged and
 * consumed; replies are modelled for the requests the driver waits on.
 * Messages are signalled with SWGEN0 through MC_INTR_0 on the stall line.
 */
#define PMU_BASE 0x10a000
#define PMU_IRQ_SWGEN0 (1U << 6)
#define MC_INTR_PMU (1U << 24)
#define GK20A_MC_ENABLE_PWR 0x2000
#define PMU_UNIT_REWIND 0x00
#define PMU_UNIT_PG 0x03
#define PMU_UNIT_INIT 0x07
#define PMU_UNIT_PERFMON 0x12
#define PMU_MSG_FLAGS_EVENT 0x04
#define PMU_PG_STAT_OFFSET 0x5000 /* PG statistics buffer (model choice) */

/* DMEM layout the modelled microcode reports (model choice). */
#define PMU_SW_AREA_OFFSET 0x3000
#define PMU_SW_AREA_SIZE 0x2000
static const struct {
    uint16_t offset, size;
    uint8_t index;
} gk20a_pmu_queues[5] = {
    { 0x6000, 0x200, 0 }, /* command queue: high priority */
    { 0x6200, 0x200, 1 }, /* command queue: low priority */
    { 0x6400, 0x100, 2 },
    { 0x6500, 0x100, 3 },
    { 0x6800, 0x400, 0 }, /* message queue (MSGQ registers) */
};
#define PMU_MSGQ 4

static void gk20a_update_irq(GK20AState *s)
{
    uint32_t intr0 = 0;

    if (!s->experimental_pmu) {
        return;
    }
    if (s->pmu_irqstat & s->pmu_irqmask & s->pmu_irqdest & 0xffff) {
        intr0 |= MC_INTR_PMU;
    }
    qemu_set_irq(s->irq_stall,
                 (s->mc_intr_en[0] & 1) && (intr0 & s->mc_intr_mask[0]));
    qemu_set_irq(s->irq_nonstall, 0);
}

static uint32_t gk20a_mc_intr0(GK20AState *s)
{
    return (s->pmu_irqstat & s->pmu_irqmask & s->pmu_irqdest & 0xffff) ?
           MC_INTR_PMU : 0;
}

static void gk20a_pmu_reset(GK20AState *s)
{
    memset(s->pmu_dmem, 0, sizeof(s->pmu_dmem));
    memset(s->pmu_dmemc, 0, sizeof(s->pmu_dmemc));
    s->pmu_cpuctl = FALCON_CPUCTL_HALTED;
    s->pmu_irqstat = 0;
    s->pmu_irqmask = 0;
    s->pmu_irqdest = 0;
    s->pmu_irqmode = 0;
    memset(s->pmu_mailbox, 0, sizeof(s->pmu_mailbox));
    memset(s->pmu_queue_head, 0, sizeof(s->pmu_queue_head));
    memset(s->pmu_queue_tail, 0, sizeof(s->pmu_queue_tail));
    s->pmu_msgq_head = 0;
    s->pmu_msgq_tail = 0;
    memset(s->pmu_mutex, 0, sizeof(s->pmu_mutex));
    s->pmu_next_mutex_id = 1;
    memset(s->pmu_regs, 0, sizeof(s->pmu_regs));
    gk20a_update_irq(s);
}

static void gk20a_pmu_post_msg(GK20AState *s, const uint8_t *msg,
                               unsigned len)
{
    unsigned q_off = gk20a_pmu_queues[PMU_MSGQ].offset;
    unsigned q_end = q_off + gk20a_pmu_queues[PMU_MSGQ].size;
    unsigned alen = ROUND_UP(len, 4);
    uint32_t head = s->pmu_msgq_head;

    if (head < q_off || head + alen > q_end) {
        if (head >= q_off && head + 4 <= q_end) {
            static const uint8_t rewind[4] = { PMU_UNIT_REWIND, 4, 0, 0 };
            memcpy(&s->pmu_dmem[head], rewind, 4);
        }
        head = q_off;
    }
    memcpy(&s->pmu_dmem[head], msg, len);
    s->pmu_msgq_head = head + alen;
    s->pmu_irqstat |= PMU_IRQ_SWGEN0;
    gk20a_update_irq(s);
}

static void gk20a_pmu_boot(GK20AState *s)
{
    uint8_t m[0x28] = { PMU_UNIT_INIT, sizeof(m), 0, 0, /* type 0: INIT */ };

    for (unsigned i = 0; i < 4; i++) {
        s->pmu_queue_head[i] = s->pmu_queue_tail[i] =
            gk20a_pmu_queues[i].offset;
    }
    s->pmu_msgq_head = s->pmu_msgq_tail = gk20a_pmu_queues[PMU_MSGQ].offset;
    for (unsigned i = 0; i < 5; i++) {
        uint8_t *q = &m[4 + 2 + 6 * i];

        stw_le_p(q, gk20a_pmu_queues[i].size);
        stw_le_p(q + 2, gk20a_pmu_queues[i].offset);
        q[4] = gk20a_pmu_queues[i].index;
    }
    stw_le_p(&m[4 + 0x20], PMU_SW_AREA_OFFSET);
    stw_le_p(&m[4 + 0x22], PMU_SW_AREA_SIZE);
    qemu_log_mask(LOG_UNIMP, "gk20a: PMU started, INIT message posted\n");
    gk20a_pmu_post_msg(s, m, sizeof(m));
}

static void gk20a_pmu_command(GK20AState *s, const uint8_t *c, unsigned size)
{
    g_autoptr(GString) hex = g_string_new(NULL);

    for (unsigned i = 0; i < size; i++) {
        g_string_append_printf(hex, " %02x", c[i]);
    }
    if (c[0] == PMU_UNIT_PG && size >= 8 && c[4] == 0) {
        /* ELPG command -> ELPG message: INIT, DISALLOW, ALLOW, FREEZE ack */
        static const uint8_t ack[] = { 0, 1, 2, 3, 5 };
        uint16_t cmd = lduw_le_p(&c[6]);

        if (cmd < ARRAY_SIZE(ack)) {
            uint8_t m[8] = { PMU_UNIT_PG, 8, 0, c[3], 0, c[5] };

            stw_le_p(&m[6], ack[cmd]);
            qemu_log_mask(LOG_UNIMP, "gk20a: PMU command%s -> ELPG ack %u\n",
                          hex->str, ack[cmd]);
            gk20a_pmu_post_msg(s, m, sizeof(m));
            return;
        }
    }
    if (c[0] == PMU_UNIT_PG && size >= 12 && c[4] == 3 && c[6] == 0) {
        /* PG_STAT ALLOC_DMEM -> RESP_DMEM_OFFSET with the stats buffer */
        uint8_t m[12] = { PMU_UNIT_PG, 12, 0, c[3], 3, c[5], 0, 0 };

        stl_le_p(&m[8], PMU_PG_STAT_OFFSET);
        qemu_log_mask(LOG_UNIMP, "gk20a: PMU command%s -> PG stat DMEM "
                      "0x%x\n", hex->str, PMU_PG_STAT_OFFSET);
        gk20a_pmu_post_msg(s, m, sizeof(m));
        return;
    }
    if (c[0] == PMU_UNIT_PERFMON && size >= 5 && c[4] == 2) {
        /* PERFMON INIT -> INIT_EVENT (an event message, not a reply) */
        uint8_t m[8] = { PMU_UNIT_PERFMON, 8, PMU_MSG_FLAGS_EVENT, 0, 2 };

        qemu_log_mask(LOG_UNIMP, "gk20a: PMU command%s -> perfmon "
                      "INIT_EVENT\n", hex->str);
        gk20a_pmu_post_msg(s, m, sizeof(m));
        return;
    }
    qemu_log_mask(LOG_UNIMP, "gk20a: PMU command%s (no reply modelled)\n",
                  hex->str);
}

static void gk20a_pmu_cmdq(GK20AState *s, unsigned index)
{
    unsigned q;
    uint32_t pos = s->pmu_queue_tail[index];
    uint32_t head = s->pmu_queue_head[index];

    for (q = 0; q < 4 && gk20a_pmu_queues[q].index != index; q++) {
    }
    for (unsigned n = 0; q < 4 && pos != head && n < 64; n++) {
        uint8_t *c = &s->pmu_dmem[pos & (GK20A_PMU_DMEM_SIZE - 1)];

        if (pos + 4 > GK20A_PMU_DMEM_SIZE || c[1] < 4 ||
            pos + c[1] > GK20A_PMU_DMEM_SIZE) {
            qemu_log_mask(LOG_GUEST_ERROR, "gk20a: PMU queue %u: bad command "
                          "at 0x%x\n", index, pos);
            break;
        }
        if (c[0] == PMU_UNIT_REWIND) {
            pos = gk20a_pmu_queues[q].offset;
            continue;
        }
        gk20a_pmu_command(s, c, c[1]);
        pos += ROUND_UP(c[1], 4);
    }
    s->pmu_queue_tail[index] = head;
}

static bool gk20a_pmu_read(GK20AState *s, hwaddr offset, uint64_t *value)
{
    if (!s->experimental_pmu) {
        return false;
    }
    switch (offset) {
    case 0x100: /* MC_INTR_0 */
        *value = gk20a_mc_intr0(s);
        return true;
    case 0x104: /* MC_INTR_1 */
        *value = 0;
        return true;
    case 0x140: /* MC_INTR_EN_0/1 */
    case 0x144:
        *value = s->mc_intr_en[(offset - 0x140) / 4];
        return true;
    case 0x640: /* MC_INTR_MASK_0/1 */
    case 0x644:
        *value = s->mc_intr_mask[(offset - 0x640) / 4];
        return true;
    }
    if (offset < PMU_BASE || offset >= PMU_BASE + 0x1000 ||
        !(s->mc_enable & GK20A_MC_ENABLE_PWR)) {
        return false;
    }
    switch (offset - PMU_BASE) {
    case 0x008: /* IRQSTAT */
        *value = s->pmu_irqstat;
        return true;
    case 0x00c:
        *value = s->pmu_irqmode;
        return true;
    case 0x018: /* IRQMASK */
        *value = s->pmu_irqmask;
        return true;
    case 0x01c:
        *value = s->pmu_irqdest;
        return true;
    case 0x040:
    case 0x044:
        *value = s->pmu_mailbox[(offset - PMU_BASE - 0x40) / 4];
        return true;
    case FALCON_CPUCTL:
        *value = s->pmu_cpuctl;
        return true;
    case 0x108: /* HWCFG: IMEM 8:0, DMEM 17:9, in 256-byte blocks */
        *value = ((GK20A_PMU_DMEM_SIZE >> 8) << 9) | (0x8000 >> 8);
        return true;
    case FALCON_DMACTL: /* memory scrubbing already finished */
        *value = 0;
        return true;
    case 0x1c0: case 0x1c8: case 0x1d0: case 0x1d8: /* DMEMC(i) */
        *value = s->pmu_dmemc[(offset - PMU_BASE - 0x1c0) / 8];
        return true;
    case 0x1c4: case 0x1cc: case 0x1d4: case 0x1dc: { /* DMEMD(i) */
        uint32_t *c = &s->pmu_dmemc[(offset - PMU_BASE - 0x1c4) / 8];
        uint32_t a = *c & 0xfffc;

        *value = ldl_le_p(&s->pmu_dmem[a & (GK20A_PMU_DMEM_SIZE - 4)]);
        if (*c & (1U << 25)) {
            *c = (*c & ~0xffffU) | ((a + 4) & 0xfffc);
        }
        return true;
    }
    case 0x488: /* PMU_MUTEX_ID: hand out an owner id */
        *value = s->pmu_next_mutex_id;
        s->pmu_next_mutex_id = s->pmu_next_mutex_id % 0xfe + 1;
        return true;
    case 0x4a0 ... 0x4ac:
        *value = s->pmu_queue_head[(offset - PMU_BASE - 0x4a0) / 4];
        return true;
    case 0x4b0 ... 0x4bc:
        *value = s->pmu_queue_tail[(offset - PMU_BASE - 0x4b0) / 4];
        return true;
    case 0x4c8:
        *value = s->pmu_msgq_head;
        return true;
    case 0x4cc:
        *value = s->pmu_msgq_tail;
        return true;
    case 0x580 ... 0x5bc:
        *value = s->pmu_mutex[(offset - PMU_BASE - 0x580) / 4];
        return true;
    default:
        *value = s->pmu_regs[(offset - PMU_BASE) / 4];
        return true;
    }
}

static bool gk20a_pmu_write(GK20AState *s, hwaddr offset, uint32_t value)
{
    if (!s->experimental_pmu) {
        return false;
    }
    switch (offset) {
    case 0x140:
    case 0x144:
        s->mc_intr_en[(offset - 0x140) / 4] = value;
        gk20a_update_irq(s);
        return true;
    case 0x640:
    case 0x644:
        s->mc_intr_mask[(offset - 0x640) / 4] = value;
        gk20a_update_irq(s);
        return true;
    }
    if (offset < PMU_BASE || offset >= PMU_BASE + 0x1000 ||
        !(s->mc_enable & GK20A_MC_ENABLE_PWR)) {
        return false;
    }
    switch (offset - PMU_BASE) {
    case 0x000: /* IRQSSET */
        s->pmu_irqstat |= value;
        break;
    case 0x004: /* IRQSCLR */
        s->pmu_irqstat &= ~value;
        break;
    case 0x00c:
        s->pmu_irqmode = value;
        return true;
    case 0x010: /* IRQMSET */
        s->pmu_irqmask |= value;
        break;
    case 0x014: /* IRQMCLR */
        s->pmu_irqmask &= ~value;
        break;
    case 0x01c:
        s->pmu_irqdest = value;
        break;
    case 0x040:
    case 0x044:
        s->pmu_mailbox[(offset - PMU_BASE - 0x40) / 4] = value;
        return true;
    case FALCON_CPUCTL:
        if ((value & FALCON_CPUCTL_STARTCPU) &&
            (s->pmu_cpuctl & FALCON_CPUCTL_HALTED)) {
            s->pmu_cpuctl = 0;
            gk20a_pmu_boot(s);
        }
        return true;
    case 0x1c0: case 0x1c8: case 0x1d0: case 0x1d8:
        s->pmu_dmemc[(offset - PMU_BASE - 0x1c0) / 8] = value;
        return true;
    case 0x1c4: case 0x1cc: case 0x1d4: case 0x1dc: {
        uint32_t *c = &s->pmu_dmemc[(offset - PMU_BASE - 0x1c4) / 8];
        uint32_t a = *c & 0xfffc;

        stl_le_p(&s->pmu_dmem[a & (GK20A_PMU_DMEM_SIZE - 4)], value);
        if (*c & (1U << 24)) {
            *c = (*c & ~0xffffU) | ((a + 4) & 0xfffc);
        }
        return true;
    }
    case 0x48c: /* PMU_MUTEX_ID_RELEASE */
        return true;
    case 0x4a0 ... 0x4ac: {
        unsigned i = (offset - PMU_BASE - 0x4a0) / 4;

        s->pmu_queue_head[i] = value;
        if (!(s->pmu_cpuctl & FALCON_CPUCTL_HALTED)) {
            gk20a_pmu_cmdq(s, i);
        }
        return true;
    }
    case 0x4b0 ... 0x4bc:
        s->pmu_queue_tail[(offset - PMU_BASE - 0x4b0) / 4] = value;
        return true;
    case 0x4c8:
        s->pmu_msgq_head = value;
        return true;
    case 0x4cc:
        s->pmu_msgq_tail = value;
        return true;
    case 0x580 ... 0x5bc: { /* PMU_MUTEX(i): take if free, 0 releases */
        uint32_t *m = &s->pmu_mutex[(offset - PMU_BASE - 0x580) / 4];

        if (!value || !*m) {
            *m = value & 0xff;
        }
        return true;
    }
    default:
        s->pmu_regs[(offset - PMU_BASE) / 4] = value;
        return true;
    }
    gk20a_update_irq(s);
    return true;
}

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
    if (gk20a_ctxsw_read(s, offset, value) ||
        gk20a_pmu_read(s, offset, value)) {
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
    case 0x137000: /* TRIM_SYS_GPCPLL_CFG: locks while enabled, out of IDDQ */
        *value = s->gpcpll_cfg & ~BIT(17);
        if ((s->gpcpll_cfg & BIT(0)) && !(s->gpcpll_cfg & BIT(1))) {
            *value |= BIT(17);
        }
        return MEMTX_OK;
    case 0x137004: /* TRIM_SYS_GPCPLL_COEFF: M[7:0] N[15:8] PL[21:16] */
        *value = s->gpcpll_coeff;
        return MEMTX_OK;
    case 0x13700c: /* TRIM_SYS_GPCPLL_CFG2 */
        *value = s->gpcpll_cfg2;
        return MEMTX_OK;
    case 0x13701c: /* TRIM_SYS_GPCPLL_NDIV_SLOWDOWN */
        *value = s->gpcpll_slowdown;
        return MEMTX_OK;
    case 0x137100: /* TRIM_SYS_SEL_VCO: GPC2CLK from VCO (1) or bypass */
        *value = s->sel_vco;
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
    if (gk20a_ctxsw_write(s, offset, value) ||
        gk20a_pmu_write(s, offset, value)) {
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
    case 0x137000:
        s->gpcpll_cfg = value & ~BIT(17);
        return MEMTX_OK;
    case 0x137004:
        s->gpcpll_coeff = value;
        return MEMTX_OK;
    case 0x13700c:
        s->gpcpll_cfg2 = value;
        return MEMTX_OK;
    case 0x13701c:
        s->gpcpll_slowdown = value;
        return MEMTX_OK;
    case 0x137100:
        s->sel_vco = value;
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
        if (!(value & GK20A_MC_ENABLE_PGRAPH)) {
            gk20a_ctxsw_reset(s);
        }
        if (!(value & GK20A_MC_ENABLE_PWR)) {
            gk20a_pmu_reset(s);
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
    /* Power-on: PLL in IDDQ, GPC2CLK on the bypass path. */
    s->gpcpll_cfg = BIT(1);
    s->gpcpll_coeff = 0;
    s->gpcpll_cfg2 = 0;
    s->gpcpll_slowdown = 0;
    s->sel_vco = 0;
    s->mc_enable = 0;
    s->mc_enable_pb = 0;
    s->mc_elpg_enable = 0;
    s->mmu_page_size = 0;
    gk20a_engine_reset(s, false);
    s->pri_timeout = 0;
    gk20a_ring_reset(s);
    gk20a_ctxsw_reset(s);
    memset(s->mc_intr_en, 0, sizeof(s->mc_intr_en));
    memset(s->mc_intr_mask, 0, sizeof(s->mc_intr_mask));
    gk20a_pmu_reset(s);
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
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq_stall);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq_nonstall);
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
        DEFINE_PROP_BOOL("experimental-ctxsw", GK20AState,
                         experimental_ctxsw, false),
        DEFINE_PROP_BOOL("experimental-pmu", GK20AState,
                         experimental_pmu, false),
        /* context image sizes the modelled FECS reports (bytes) */
        DEFINE_PROP_UINT32("ctxsw-golden-size", GK20AState,
                           ctxsw_golden_size, 0x10000),
        DEFINE_PROP_UINT32("ctxsw-zcull-size", GK20AState,
                           ctxsw_zcull_size, 0x1000),
        DEFINE_PROP_UINT32("ctxsw-pm-size", GK20AState,
                           ctxsw_pm_size, 0x1000),
        DEFINE_PROP_UINT32("ctxsw-reglist-size", GK20AState,
                           ctxsw_reglist_size, 0x1000),
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

/*
 * BCM4359 PCIe Wi-Fi function: the host-visible part of the chip.
 *
 * The chip is an AMBA/AXI ("AI") backplane of cores.  The host reaches it
 * through BAR0, a set of 4 KiB windows whose backplane addresses are
 * chosen in vendor configuration registers, and through BAR2, which maps
 * the low 4 MiB of the backplane, including the ARM CR4 TCM.  Drivers
 * discover the chip from the ChipCommon chip ID and the enumeration ROM,
 * so both are modelled here, together with each core's AI wrapper
 * (ioctrl/reset), clock requests that are granted at once, and TCM RAM.
 *
 * Not modelled yet: the dongle firmware itself.  Core revisions are
 * provisional values for a 4359 C0 (chip rev 9).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/bswap.h"
#include "qemu/cutils.h"
#include "hw/arm/mh2p_bcm4359.h"

#define BP_CORE_BASE    0x18000000u
#define BP_WRAP_BASE    0x18100000u
#define BP_EROM         0x180ff000u
#define BP_PAGE         0x1000u

#define CHIP_ID         0x4359
#define CHIP_REV        9
#define RAM_BASE        0x180000u      /* CR4 TCM base the 4359 driver uses */
#define RAM_BANKS_A     3
#define RAM_BANKS_B     4
#define RAM_BANK_SIZE   (128 * 1024)
#define RAM_SIZE        ((RAM_BANKS_A + RAM_BANKS_B) * RAM_BANK_SIZE)

#define VEC_SIZE        0x1000u        /* CR4 vectors at backplane 0 */

#define MFG_BRCM        0x4bf

enum { CORE_CC, CORE_D11, CORE_D11_AUX, CORE_ARMCR4, CORE_PCIE2, NCORES };

static const struct {
    uint16_t id;
    uint8_t rev;
} cores[NCORES] = {
    [CORE_CC]      = { 0x800, 55 },    /* ChipCommon */
    [CORE_D11]     = { 0x812, 59 },    /* 802.11 MAC */
    [CORE_D11_AUX] = { 0x812, 59 },    /* second MAC (RSDB) */
    [CORE_ARMCR4]  = { 0x83e, 9 },     /* ARM Cortex-R4 */
    [CORE_PCIE2]   = { 0x83c, 22 },    /* PCIe Gen2 device core */
};

/*
 * Configuration registers (byte offsets).  Capabilities as on Broadcom
 * PCIe Gen2 parts: PM at 0x48, MSI (64-bit, 1 vector) at 0x58, PCIe at 0xac.
 */
#define CFG_PM_CAP      0x48
#define CFG_PM_CSR      0x4c
#define CFG_MSI_CAP     0x58
#define CFG_MSI_ADDR_LO 0x5c
#define CFG_MSI_ADDR_HI 0x60
#define CFG_MSI_DATA    0x64
#define MSI_ENABLE      BIT(16)
#define CFG_PCIE_CAP    0xac
#define CFG_PCIE_DEVCTL 0xb4
#define CFG_PCIE_LINKCAP 0xb8
#define CFG_PCIE_LINKCTL 0xbc
#define CFG_BAR0_WIN2   0x70           /* PCIe Gen2: BAR0 0x1000-0x1fff */
#define CFG_BAR0_WIN    0x80           /* BAR0 0x0000-0x0fff */

/* BAR0 layout of a PCIe Gen2 Broadcom device */
#define BAR0_WIN1       0x0000
#define BAR0_WIN2       0x1000
#define BAR0_PCIE_REGS  0x2000
#define BAR0_CC_REGS    0x3000

/* Core registers */
#define CC_CHIPID       0x000
#define CC_CAPS         0x004
#define CC_CAP_PMU      BIT(28)
#define CC_EROMPTR      0x0fc
#define CLK_CTL_ST      0x1e0
#define CCS_ALP_AVAIL   BIT(16)
#define CCS_HT_AVAIL    BIT(17)
#define PMU_CAPS        0x604
#define PMU_STATUS      0x608
#define PST_HT_AVAIL    BIT(2)
#define PST_ALP_AVAIL   BIT(3)
#define PMU_RES_STATE   0x60c
#define PMU_MIN_RES     0x618
#define PMU_MAX_RES     0x61c
#define PMU_TIMER       0x624
#define CR4_CAPS        0x004
#define CR4_BANKIDX     0x040
#define CR4_BANKINFO    0x044
#define PCIE_CFGADDR    0x120
#define PCIE_CFGDATA    0x124
#define PCIE_INTSTATUS  0x048          /* PCIMailBoxInt, write-1-to-clear */
#define PCIE_INTMASK    0x04c          /* PCIMailBoxMask */
#define PCIE_H2D_DB0    0x140          /* PCIH2D_MailBox: ring doorbell */
#define PCIE_H2D_DB1    0x144          /* PCIH2D_DB1: mailbox data */
#define CC_WATCHDOG     0x080

/* AI wrapper registers */
#define AI_IOCTRL       0x408
#define SICF_CLOCK_EN   BIT(0)
#define SICF_CPUHALT    BIT(5)
#define AI_RESETCTRL    0x800
#define AI_RESETSTATUS  0x804

/* Time from releasing the CR4 to the firmware publishing its shared area. */
#define FW_BOOT_NS      (100 * SCALE_MS)

typedef struct FwState FwState;

struct MH2PBCM4359 {
    uint32_t cfg[64];                  /* vendor area 0x40-0xff used */
    uint32_t core[NCORES][BP_PAGE / 4];
    uint32_t wrap[NCORES][BP_PAGE / 4];
    uint32_t pcie_cfg[BP_PAGE / 4];    /* PCIe core indirect config */
    uint32_t erom[32];
    unsigned erom_len;
    uint8_t *ram;
    uint8_t vec[VEC_SIZE];             /* reset vector written by the host */

    MH2PBCM4359Host host;
    QEMUTimer *boot_timer;
    FwState *fw;
    bool fw_running;
    bool irq_level;
};

static void fw_boot(void *opaque);
static FwState *fw_new(void);
static void fw_stop(MH2PBCM4359 *b);
static void fw_doorbell(MH2PBCM4359 *b, unsigned which, uint32_t value);

static bool msi_enabled(MH2PBCM4359 *b)
{
    return b->cfg[CFG_MSI_CAP / 4] & MSI_ENABLE;
}

static void send_msi(MH2PBCM4359 *b)
{
    uint64_t addr = b->cfg[CFG_MSI_ADDR_LO / 4] |
                    (uint64_t)b->cfg[CFG_MSI_ADDR_HI / 4] << 32;

    b->host.msi(b->host.opaque, addr, b->cfg[CFG_MSI_DATA / 4]);
}

/* INTA follows the masked mailbox status; with MSI it is never asserted. */
static void update_irq(MH2PBCM4359 *b)
{
    uint32_t *pcie = b->core[CORE_PCIE2];
    bool level = (pcie[PCIE_INTSTATUS / 4] & pcie[PCIE_INTMASK / 4]) &&
                 !msi_enabled(b);

    if (level != b->irq_level) {
        b->irq_level = level;
        b->host.irq(b->host.opaque, level);
    }
}

/* Dongle to host doorbell: raise mailbox interrupt bits (one MSI each). */
static void raise_host_int(MH2PBCM4359 *b, uint32_t bits)
{
    uint32_t *pcie = b->core[CORE_PCIE2];

    pcie[PCIE_INTSTATUS / 4] |= bits;
    if (msi_enabled(b) && (bits & pcie[PCIE_INTMASK / 4])) {
        send_msi(b);
    }
    update_irq(b);
}

/* Watchdog or power-on: cores back in reset, firmware stopped, RAM kept. */
static void chip_reset(MH2PBCM4359 *b)
{
    fw_stop(b);
    memset(b->core, 0, sizeof(b->core));
    memset(b->wrap, 0, sizeof(b->wrap));
    for (unsigned i = 0; i < NCORES; i++) {
        b->wrap[i][AI_RESETCTRL / 4] = 1;
    }
    b->core[CORE_CC][PMU_MAX_RES / 4] = 0x7fffffff;
    update_irq(b);
}

/* The CR4 runs once out of reset, clocked and not halted. */
static void cr4_check_release(MH2PBCM4359 *b)
{
    uint32_t *w = b->wrap[CORE_ARMCR4];
    bool run = !(w[AI_RESETCTRL / 4] & 1) && (w[AI_IOCTRL / 4] & SICF_CLOCK_EN) &&
               !(w[AI_IOCTRL / 4] & SICF_CPUHALT);

    if (run && !b->fw_running && !timer_pending(b->boot_timer)) {
        timer_mod(b->boot_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + FW_BOOT_NS);
    } else if (!run) {
        fw_stop(b);
    }
}

static void build_erom(MH2PBCM4359 *b)
{
    unsigned n = 0;

    for (unsigned i = 0; i < NCORES; i++) {
        /* CIA: manufacturer, core id, class 1; CIB: rev, 1 MP, 1 MW, 1 SP */
        b->erom[n++] = MFG_BRCM << 20 | cores[i].id << 8 | 1 << 4 | 1;
        b->erom[n++] = (uint32_t)cores[i].rev << 24 | 1 << 14 | 1 << 9 |
                       1 << 4 | 1;
        b->erom[n++] = i << 8 | 2 | 1;                     /* master port */
        b->erom[n++] = (BP_CORE_BASE + i * BP_PAGE) | 4 | 1;    /* slave */
        b->erom[n++] = (BP_WRAP_BASE + i * BP_PAGE) | 3 << 6 | 4 | 1;
    }
    b->erom[n++] = 0xf;                                    /* end */
    b->erom_len = n;
}

MH2PBCM4359 *mh2p_bcm4359_new(const MH2PBCM4359Host *host)
{
    MH2PBCM4359 *b = g_new0(MH2PBCM4359, 1);

    b->host = *host;
    b->fw = fw_new();
    b->boot_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, fw_boot, b);
    b->ram = g_malloc0(RAM_SIZE);
    build_erom(b);
    mh2p_bcm4359_reset(b);
    return b;
}

void mh2p_bcm4359_reset(MH2PBCM4359 *b)
{
    memset(b->cfg, 0, sizeof(b->cfg));
    memset(b->pcie_cfg, 0, sizeof(b->pcie_cfg));
    memset(b->ram, 0, RAM_SIZE);
    memset(b->vec, 0, sizeof(b->vec));
    b->cfg[CFG_BAR0_WIN / 4] = BP_CORE_BASE;
    b->cfg[CFG_BAR0_WIN2 / 4] = BP_CORE_BASE;
    b->cfg[CFG_PM_CAP / 4] = 0x0003 << 16 | CFG_MSI_CAP << 8 | 0x01;
    b->cfg[CFG_MSI_CAP / 4] = 0x0080 << 16 | CFG_PCIE_CAP << 8 | 0x05;
    b->cfg[CFG_PCIE_CAP / 4] = 0x0002 << 16 | 0x10;  /* v2 endpoint, last */
    b->cfg[CFG_PCIE_LINKCAP / 4] = 1 << 4 | 2;       /* x1, 5 GT/s */
    b->cfg[CFG_PCIE_LINKCTL / 4] = 0x0011 << 16;     /* trained x1 2.5 GT/s */
    chip_reset(b);
}

bool mh2p_bcm4359_config(MH2PBCM4359 *b, unsigned reg, uint32_t *value,
                         bool write)
{
    unsigned index = reg / 4;

    if (reg < 0x40 || index >= ARRAY_SIZE(b->cfg)) {
        return false;
    }
    if (!write) {
        *value = b->cfg[index];
        return true;
    }
    switch (reg) {
    case CFG_BAR0_WIN:
    case CFG_BAR0_WIN2:
        b->cfg[index] = *value & ~(BP_PAGE - 1);
        break;
    case CFG_PM_CAP:
    case CFG_PCIE_CAP:
    case CFG_PCIE_LINKCAP:
        break;                                       /* read-only */
    case CFG_PM_CSR:
        b->cfg[index] = *value & 3;                  /* power state */
        break;
    case CFG_MSI_CAP:                                /* enable, MME */
        b->cfg[index] = (b->cfg[index] & ~(MSI_ENABLE | 0x700000)) |
                        (*value & (MSI_ENABLE | 0x700000));
        update_irq(b);
        break;
    case CFG_MSI_ADDR_LO:
        b->cfg[index] = *value & ~3u;
        break;
    case CFG_MSI_DATA:
        b->cfg[index] = *value & 0xffff;
        break;
    case CFG_PCIE_LINKCTL:
        b->cfg[index] = (b->cfg[index] & 0xffff0000) | (*value & 0xffff);
        break;
    default:
        b->cfg[index] = *value;
        break;
    }
    return true;
}

static uint32_t core_read(MH2PBCM4359 *b, unsigned i, uint32_t off)
{
    uint32_t v = b->core[i][off / 4];

    if (off == CLK_CTL_ST) {
        return v | CCS_ALP_AVAIL | CCS_HT_AVAIL;   /* clocks granted */
    }
    switch (i) {
    case CORE_CC:
        switch (off) {
        case CC_CHIPID:
            return CHIP_ID | CHIP_REV << 16 | NCORES << 24 | 1u << 28;
        case CC_CAPS:
            return CC_CAP_PMU;
        case CC_EROMPTR:
            return BP_EROM;
        case PMU_CAPS:
            return 0x1f00 | 28;        /* 32 resources, PMU rev 28 */
        case PMU_STATUS:
            return PST_HT_AVAIL | PST_ALP_AVAIL;
        case PMU_RES_STATE:
            return b->core[i][PMU_MAX_RES / 4];
        case PMU_TIMER:                /* free-running ILP (32 kHz) */
            return muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), 32768,
                            NANOSECONDS_PER_SECOND);
        }
        break;
    case CORE_ARMCR4:
        switch (off) {
        case CR4_CAPS:
            return RAM_BANKS_A | RAM_BANKS_B << 4;
        case CR4_BANKINFO:
            return RAM_BANK_SIZE / 8192 - 1;
        }
        break;
    case CORE_PCIE2:
        if (off == PCIE_CFGDATA) {
            return b->pcie_cfg[(b->core[i][PCIE_CFGADDR / 4] & 0xffc) / 4];
        }
        break;
    }
    return v;
}

static void core_write(MH2PBCM4359 *b, unsigned i, uint32_t off, uint32_t v)
{
    if (i == CORE_PCIE2) {
        switch (off) {
        case PCIE_CFGDATA:
            b->pcie_cfg[(b->core[i][PCIE_CFGADDR / 4] & 0xffc) / 4] = v;
            return;
        case PCIE_INTSTATUS:
            b->core[i][off / 4] &= ~v;
            update_irq(b);
            return;
        case PCIE_INTMASK: {
            uint32_t unmasked = v & ~b->core[i][off / 4];

            b->core[i][off / 4] = v;
            if (msi_enabled(b) &&
                (unmasked & b->core[i][PCIE_INTSTATUS / 4])) {
                send_msi(b);                   /* pending source unmasked */
            }
            update_irq(b);
            return;
        }
        case PCIE_H2D_DB0:
        case PCIE_H2D_DB1:
            b->core[i][off / 4] = v;
            fw_doorbell(b, off == PCIE_H2D_DB1, v);
            return;
        }
    }
    if (i == CORE_CC && off == CC_WATCHDOG) {
        if (v) {
            chip_reset(b);             /* the count is short: reset now */
        }
        return;
    }
    if (i == CORE_CC && (off == CC_CHIPID || off == CC_CAPS ||
                         off == CC_EROMPTR || off == PMU_STATUS ||
                         off == PMU_TIMER)) {
        return;
    }
    b->core[i][off / 4] = v;
}

/* 32-bit backplane access; sub-word accesses are merged by the caller. */
static uint32_t bp_read32(MH2PBCM4359 *b, uint32_t addr)
{
    uint32_t page = addr & ~(BP_PAGE - 1), off = addr & (BP_PAGE - 1);

    if (addr - RAM_BASE < RAM_SIZE) {
        return ldl_le_p(b->ram + (addr - RAM_BASE));
    }
    if (page == BP_EROM) {
        return off / 4 < b->erom_len ? b->erom[off / 4] : 0;
    }
    if (page - BP_CORE_BASE < NCORES * BP_PAGE) {
        return core_read(b, (page - BP_CORE_BASE) / BP_PAGE, off);
    }
    if (page - BP_WRAP_BASE < NCORES * BP_PAGE) {
        unsigned i = (page - BP_WRAP_BASE) / BP_PAGE;

        if (off == AI_RESETSTATUS) {
            return 0;                  /* reset sequencing completes at once */
        }
        return b->wrap[i][off / 4];
    }
    qemu_log_mask(LOG_UNIMP, "bcm4359: backplane read 0x%08x\n", addr);
    return 0;
}

static void bp_write32(MH2PBCM4359 *b, uint32_t addr, uint32_t v)
{
    uint32_t page = addr & ~(BP_PAGE - 1), off = addr & (BP_PAGE - 1);

    if (addr - RAM_BASE < RAM_SIZE) {
        stl_le_p(b->ram + (addr - RAM_BASE), v);
    } else if (page - BP_CORE_BASE < NCORES * BP_PAGE) {
        core_write(b, (page - BP_CORE_BASE) / BP_PAGE, off, v);
    } else if (page - BP_WRAP_BASE < NCORES * BP_PAGE) {
        unsigned i = (page - BP_WRAP_BASE) / BP_PAGE;

        if (off != AI_RESETSTATUS) {
            b->wrap[i][off / 4] = v;
        }
        if (i == CORE_ARMCR4 && (off == AI_IOCTRL || off == AI_RESETCTRL)) {
            cr4_check_release(b);
        }
    } else if (page != BP_EROM) {
        qemu_log_mask(LOG_UNIMP, "bcm4359: backplane write 0x%08x = 0x%x\n",
                      addr, v);
    }
}

static void bp_access(MH2PBCM4359 *b, uint32_t addr, unsigned size,
                      uint64_t *value, bool write)
{
    uint32_t word = addr & ~3u, shift = (addr & 3) * 8;
    uint32_t mask = (uint32_t)MAKE_64BIT_MASK(shift, size * 8);

    if ((addr - RAM_BASE < RAM_SIZE && addr - RAM_BASE + size <= RAM_SIZE) ||
        addr + size <= VEC_SIZE) {
        uint8_t *p = addr < VEC_SIZE ? b->vec + addr
                                     : b->ram + (addr - RAM_BASE);

        if (write) {
            stn_le_p(p, size, *value);
        } else {
            *value = ldn_le_p(p, size);
        }
        return;
    }
    if (write) {
        uint32_t old = size == 4 ? 0 : bp_read32(b, word);

        bp_write32(b, word, (old & ~mask) | (((uint32_t)*value << shift) & mask));
    } else {
        *value = (bp_read32(b, word) & mask) >> shift;
    }
}

void mh2p_bcm4359_bar0(MH2PBCM4359 *b, uint32_t offset, unsigned size,
                       uint64_t *value, bool write)
{
    uint32_t off = offset & (BP_PAGE - 1), base;

    switch (offset & ~(BP_PAGE - 1)) {
    case BAR0_WIN1:
        base = b->cfg[CFG_BAR0_WIN / 4];
        break;
    case BAR0_WIN2:
        base = b->cfg[CFG_BAR0_WIN2 / 4];
        break;
    case BAR0_PCIE_REGS:
        base = BP_CORE_BASE + CORE_PCIE2 * BP_PAGE;
        break;
    case BAR0_CC_REGS:
        base = BP_CORE_BASE + CORE_CC * BP_PAGE;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "bcm4359: BAR0 %s 0x%x\n",
                      write ? "write" : "read", offset);
        if (!write) {
            *value = 0;
        }
        return;
    }
    bp_access(b, base + off, size, value, write);
}

void mh2p_bcm4359_bar2(MH2PBCM4359 *b, uint32_t offset, unsigned size,
                       uint64_t *value, bool write)
{
    bp_access(b, offset, size, value, write);
}

/*
 * Dongle firmware, host-interface side only: the PCIe full-dongle "msgbuf"
 * protocol (pciedev_shared version 5, as in the GPL bcmdhd 1.363 headers
 * bcmpcie.h/bcmmsgbuf.h).  The firmware image the host downloads is kept in
 * TCM but not executed; this model publishes the shared area, serves the
 * message rings and answers control requests as a firmware with its radio
 * idle would.  No frames are received; transmitted frames are completed.
 */
#define SH_ADDR         (RAM_BASE + 0xc0000)   /* above the image, below NVRAM */
#define SH_RINGINFO     (SH_ADDR + 0x080)
#define SH_RINGMEM      (SH_ADDR + 0x100)
#define SH_H2D_W        (SH_ADDR + 0x200)
#define SH_H2D_R        (SH_ADDR + 0x300)
#define SH_D2H_W        (SH_ADDR + 0x400)
#define SH_D2H_R        (SH_ADDR + 0x410)
#define SH_H2D_MB       (SH_ADDR + 0x420)
#define SH_D2H_MB       (SH_ADDR + 0x424)
#define SH_FLAGS        (5 | 0x100000)         /* v5, 2-byte ring indices */
#define MAX_H2D_RINGS   34                     /* 2 common + 32 flow rings */

#define RING_CTRL_SUBMIT    0
#define RING_RXPOST         1
#define D2H_CTRL_CPL        0                  /* D2H ring index (id - 2) */
#define D2H_TX_CPL          1

enum {
    MSG_GEN_STATUS = 0x1, MSG_FLOW_CREATE = 0x3, MSG_FLOW_CREATE_CMPLT = 0x4,
    MSG_FLOW_DELETE = 0x5, MSG_FLOW_FLUSH = 0x7,
    MSG_IOCTLPTR_REQ = 0x9, MSG_IOCTLPTR_REQ_ACK = 0xa,
    MSG_IOCTLRESP_BUF_POST = 0xb, MSG_IOCTL_CMPLT = 0xc,
    MSG_EVENT_BUF_POST = 0xd, MSG_TX_POST = 0xf, MSG_TX_STATUS = 0x10,
};

#define PCIE_MB_FN0_0       0x100              /* D2H mailbox data */
#define PCIE_MB_D2H_DB0     0x10000            /* D2H ring doorbell */
#define H2D_HOST_D3_INFORM  0x1
#define D2H_DEV_D3_ACK      0x1

#define BCME_BADARG     (-2)

/* wlioctl command numbers */
#define WLC_GET_MAGIC       0
#define WLC_GET_VERSION     1
#define WLC_UP              2
#define WLC_DOWN            3
#define WLC_GET_UP          162
#define WLC_GET_REVINFO     98
#define WLC_GET_BANDLIST    140
#define WLC_GET_VAR         262
#define WLC_SET_VAR         263
#define WLC_IOCTL_MAGIC     0x14e46c77
#define WLC_IOCTL_VERSION   2

typedef struct HostBuf {
    uint32_t pktid;
    uint64_t addr;
    uint16_t len;
} HostBuf;

#define NBUFS 256

typedef struct BufQueue {
    HostBuf buf[NBUFS];
    unsigned head, count;
} BufQueue;

struct FwState {
    BufQueue ioctl_bufs, event_bufs, rx_bufs;
    uint16_t h2d_rd[MAX_H2D_RINGS];
    uint16_t d2h_wr[3];
    struct {
        bool active;
        uint64_t base;
        uint16_t max_items, len_item;
    } flow[MAX_H2D_RINGS];
    uint8_t ioctl_req[40];                     /* waiting for a response buffer */
    bool ioctl_pending;
    bool d2h_posted;
    bool up;                                   /* WLC_UP / WLC_DOWN state */
};

static bool fw_log_on(void)
{
    static int on = -1;

    if (on < 0) {
        on = getenv("MH2P_BCM4359_LOG") != NULL;
    }
    return on;
}

#define FW_LOG(...) do { \
        if (fw_log_on()) { \
            fprintf(stderr, "bcm4359: " __VA_ARGS__); \
        } \
    } while (0)

static uint16_t tcm16(MH2PBCM4359 *b, uint32_t a)
{
    return lduw_le_p(b->ram + (a - RAM_BASE));
}

static void tcm16_set(MH2PBCM4359 *b, uint32_t a, uint16_t v)
{
    stw_le_p(b->ram + (a - RAM_BASE), v);
}

static uint32_t tcm32(MH2PBCM4359 *b, uint32_t a)
{
    return ldl_le_p(b->ram + (a - RAM_BASE));
}

static void tcm32_set(MH2PBCM4359 *b, uint32_t a, uint32_t v)
{
    stl_le_p(b->ram + (a - RAM_BASE), v);
}

static bool dma(MH2PBCM4359 *b, uint64_t addr, void *buf, unsigned len,
                bool write)
{
    MemTxResult r = b->host.dma(b->host.opaque, addr, buf, len, write);

    if (r != MEMTX_OK) {
        FW_LOG("DMA %s 0x%" PRIx64 " len %u failed\n",
               write ? "write" : "read", addr, len);
        if (!write) {
            memset(buf, 0, len);
        }
        return false;
    }
    return true;
}

static void ringmem(MH2PBCM4359 *b, unsigned id, uint64_t *base,
                    uint16_t *max, uint16_t *len)
{
    uint32_t m = SH_RINGMEM + id * 16;

    *max = tcm16(b, m + 4);
    *len = tcm16(b, m + 6);
    *base = tcm32(b, m + 8) | (uint64_t)tcm32(b, m + 12) << 32;
}

static void bufq_push(BufQueue *q, uint32_t pktid, uint64_t addr, uint16_t len)
{
    if (q->count == NBUFS) {
        FW_LOG("buffer queue full, dropping pktid %u\n", pktid);
        return;
    }
    q->buf[(q->head + q->count++) % NBUFS] = (HostBuf){ pktid, addr, len };
}

static bool bufq_pop(BufQueue *q, HostBuf *out)
{
    if (!q->count) {
        return false;
    }
    *out = q->buf[q->head];
    q->head = (q->head + 1) % NBUFS;
    q->count--;
    return true;
}

/* Append one item to a D2H common ring. */
static void d2h_post(MH2PBCM4359 *b, unsigned d2h, const uint8_t *msg,
                     unsigned msglen)
{
    FwState *f = b->fw;
    uint64_t base;
    uint16_t max, len, rd, wr = f->d2h_wr[d2h];
    uint8_t item[64] = { 0 };

    ringmem(b, 2 + d2h, &base, &max, &len);
    rd = tcm16(b, SH_D2H_R + d2h * 2);
    if (!max || !len || len > sizeof(item) || (wr + 1) % max == rd) {
        FW_LOG("D2H ring %u full or unset (max %u len %u rd %u wr %u)\n",
               d2h, max, len, rd, wr);
        return;
    }
    memcpy(item, msg, MIN(msglen, len));
    dma(b, base + (uint64_t)wr * len, item, len, true);
    f->d2h_wr[d2h] = (wr + 1) % max;
    tcm16_set(b, SH_D2H_W + d2h * 2, f->d2h_wr[d2h]);
    f->d2h_posted = true;
}

static void put_hdr(uint8_t *m, uint8_t type, uint8_t ifidx, uint32_t reqid)
{
    m[0] = type;
    m[1] = ifidx;
    stl_le_p(m + 4, reqid);
}

/* Look up "name=" in the NVRAM the host placed at the top of TCM. */
static bool nvram_get(MH2PBCM4359 *b, const char *name, char *out, size_t n)
{
    size_t nl = strlen(name);
    const uint8_t *start = b->ram + RAM_SIZE - 0x4000;
    const uint8_t *end = b->ram + RAM_SIZE;

    for (const uint8_t *p = start; p + nl + 1 < end; p++) {
        if ((p == start || p[-1] == 0) && !memcmp(p, name, nl) &&
            p[nl] == '=') {
            size_t i = 0;

            for (p += nl + 1; p < end && *p && i + 1 < n; p++) {
                out[i++] = *p;
            }
            out[i] = 0;
            return true;
        }
    }
    return false;
}

static void fw_macaddr(MH2PBCM4359 *b, uint8_t mac[6])
{
    static const uint8_t def[6] = { 0x00, 0x90, 0x4c, 0x00, 0x43, 0x59 };
    char s[32];
    unsigned v[6];

    memcpy(mac, def, 6);
    if (nvram_get(b, "macaddr", s, sizeof(s)) &&
        sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4],
               &v[5]) == 6) {
        for (int i = 0; i < 6; i++) {
            mac[i] = v[i];
        }
    }
}

/*
 * Control request: returns the BCME status.  'buf' holds the request
 * payload (inlen bytes) and receives the response; *outlen is the host's
 * output length on entry and the response length on return.
 */
static int fw_ioctl(MH2PBCM4359 *b, uint32_t cmd, uint8_t *buf, unsigned inlen,
                    unsigned *outlen)
{
    unsigned len = *outlen;

    switch (cmd) {
    case WLC_GET_MAGIC:
        if (len >= 4) {
            stl_le_p(buf, WLC_IOCTL_MAGIC);
        }
        return 0;
    case WLC_GET_VERSION:
        if (len >= 4) {
            stl_le_p(buf, WLC_IOCTL_VERSION);
        }
        return 0;
    case WLC_UP:
    case WLC_DOWN:
        b->fw->up = cmd == WLC_UP;
        FW_LOG("%s\n", cmd == WLC_UP ? "up" : "down");
        *outlen = 0;
        return 0;
    case WLC_GET_UP:
        memset(buf, 0, len);
        if (len >= 4) {
            stl_le_p(buf, b->fw->up);
        }
        return 0;
    case WLC_GET_REVINFO: {
        /* wlc_rev_info_t: vendor, device, radiorev, chiprev, corerev,
         * boardid, boardvendor, boardrev, driverrev, ucoderev, bus,
         * chipnum, phytype, phyrev, anarev, chippkg, nvramrev */
        static const uint32_t rev[] = {
            0x14e4, 0x43ef, 0, CHIP_REV, 59, 0, 0x14e4, 0x1100, 0, 0,
            2, CHIP_ID, 11, 0, 0, 0, 0,
        };
        memset(buf, 0, len);
        for (unsigned i = 0; i < ARRAY_SIZE(rev) && (i + 1) * 4 <= len; i++) {
            stl_le_p(buf + i * 4, rev[i]);
        }
        return 0;
    }
    case WLC_GET_BANDLIST:
        memset(buf, 0, len);
        if (len >= 12) {
            stl_le_p(buf, 2);
            stl_le_p(buf + 4, 1);              /* 5 GHz */
            stl_le_p(buf + 8, 2);              /* 2.4 GHz */
        }
        return 0;
    case WLC_GET_VAR: {
        char name[64];
        size_t nl = strnlen((char *)buf, MIN(inlen, sizeof(name) - 1));

        memcpy(name, buf, nl);
        name[nl] = 0;
        FW_LOG("get iovar '%s' (out %u)\n", name, len);
        memset(buf, 0, len);
        if (!strcmp(name, "cur_etheraddr") && len >= 6) {
            fw_macaddr(b, buf);
        } else if (!strcmp(name, "ver")) {
            pstrcpy((char *)buf, len,
                    "wl0: BCM4359 emulated 9.40.100 (r0) FWID 01-0\n");
        } else if (!strcmp(name, "cap")) {
            pstrcpy((char *)buf, len, "ap sta wme 802.11d 802.11h rm cqa cac "
                    "dualband ampdu ampdu_tx ampdu_rx radio_pwrsave rsdb\n");
        }
        return 0;
    }
    case WLC_SET_VAR:
        FW_LOG("set iovar '%.*s'\n", (int)strnlen((char *)buf, MIN(inlen, 48)),
               (char *)buf);
        *outlen = 0;
        return 0;
    default:
        FW_LOG("ioctl %u (in %u out %u)\n", cmd, inlen, len);
        if (cmd & 1) {                         /* WLC_SET_*: accepted */
            *outlen = 0;
        } else {
            memset(buf, 0, len);
        }
        return 0;
    }
}

/* Serve an IOCTLPTR_REQ once a response buffer is available. */
static bool fw_do_ioctl(MH2PBCM4359 *b, const uint8_t *req)
{
    FwState *f = b->fw;
    uint32_t cmd = ldl_le_p(req + 8);
    uint16_t trans = lduw_le_p(req + 0xc), inlen = lduw_le_p(req + 0xe);
    uint16_t outmax = lduw_le_p(req + 0x10);
    uint64_t addr = ldl_le_p(req + 0x18) | (uint64_t)ldl_le_p(req + 0x1c) << 32;
    HostBuf rb;
    uint8_t msg[24] = { 0 }, *buf;
    unsigned len;
    int status;

    if (!bufq_pop(&f->ioctl_bufs, &rb)) {
        return false;
    }
    buf = g_malloc0(MAX(inlen, outmax) + 1);
    dma(b, addr, buf, inlen, false);
    len = outmax;
    status = fw_ioctl(b, cmd, buf, inlen, &len);
    len = MIN(len, rb.len);

    put_hdr(msg, MSG_IOCTLPTR_REQ_ACK, req[1], ldl_le_p(req + 4));
    stl_le_p(msg + 12, cmd);
    d2h_post(b, D2H_CTRL_CPL, msg, sizeof(msg));

    memset(msg, 0, sizeof(msg));
    if (len) {
        dma(b, rb.addr, buf, len, true);
    }
    put_hdr(msg, MSG_IOCTL_CMPLT, req[1], rb.pktid);
    stw_le_p(msg + 8, (uint16_t)status);
    stw_le_p(msg + 12, len);
    stw_le_p(msg + 14, trans);
    stl_le_p(msg + 16, cmd);
    d2h_post(b, D2H_CTRL_CPL, msg, sizeof(msg));
    g_free(buf);
    return true;
}

static void fw_ctrl_msg(MH2PBCM4359 *b, const uint8_t *m)
{
    FwState *f = b->fw;
    uint8_t rsp[24] = { 0 };
    uint16_t flowid;

    switch (m[0]) {
    case MSG_IOCTLPTR_REQ:
        if (!fw_do_ioctl(b, m)) {
            memcpy(f->ioctl_req, m, sizeof(f->ioctl_req));
            f->ioctl_pending = true;
        }
        break;
    case MSG_IOCTLRESP_BUF_POST:
    case MSG_EVENT_BUF_POST:
        bufq_push(m[0] == MSG_EVENT_BUF_POST ? &f->event_bufs : &f->ioctl_bufs,
                  ldl_le_p(m + 4),
                  ldl_le_p(m + 0x10) | (uint64_t)ldl_le_p(m + 0x14) << 32,
                  lduw_le_p(m + 8));
        if (f->ioctl_pending && fw_do_ioctl(b, f->ioctl_req)) {
            f->ioctl_pending = false;
        }
        break;
    case MSG_FLOW_CREATE:
        /* tx_flowring_create_request_t: da, sa, tid, if_flags, id at 0x16 */
        flowid = lduw_le_p(m + 0x16);
        if (flowid >= 2 && flowid < MAX_H2D_RINGS) {
            f->flow[flowid].active = true;
            f->flow[flowid].max_items = lduw_le_p(m + 0x1c);
            f->flow[flowid].len_item = lduw_le_p(m + 0x1e);
            f->flow[flowid].base = ldl_le_p(m + 0x20) |
                                   (uint64_t)ldl_le_p(m + 0x24) << 32;
            f->h2d_rd[flowid] = 0;
            tcm16_set(b, SH_H2D_R + flowid * 2, 0);
        }
        FW_LOG("flow ring %u created\n", flowid);
        put_hdr(rsp, MSG_FLOW_CREATE_CMPLT, m[1], ldl_le_p(m + 4));
        stw_le_p(rsp + 8, flowid >= 2 && flowid < MAX_H2D_RINGS ? 0
                                                    : (uint16_t)BCME_BADARG);
        stw_le_p(rsp + 10, flowid);
        d2h_post(b, D2H_CTRL_CPL, rsp, sizeof(rsp));
        break;
    case MSG_FLOW_DELETE:
    case MSG_FLOW_FLUSH:
        flowid = lduw_le_p(m + 8);
        if (m[0] == MSG_FLOW_DELETE && flowid < MAX_H2D_RINGS) {
            f->flow[flowid].active = false;
        }
        put_hdr(rsp, m[0] + 1, m[1], ldl_le_p(m + 4));
        stw_le_p(rsp + 10, flowid);
        d2h_post(b, D2H_CTRL_CPL, rsp, sizeof(rsp));
        break;
    default:
        FW_LOG("control message type 0x%x not handled\n", m[0]);
        put_hdr(rsp, MSG_GEN_STATUS, m[1], ldl_le_p(m + 4));
        stw_le_p(rsp + 8, (uint16_t)BCME_BADARG);
        d2h_post(b, D2H_CTRL_CPL, rsp, sizeof(rsp));
        break;
    }
}

/* Consume new items of an H2D ring. */
static void fw_h2d_ring(MH2PBCM4359 *b, unsigned id)
{
    FwState *f = b->fw;
    uint64_t base;
    uint16_t max, len, wr;
    uint8_t item[64];

    if (id < 2) {
        ringmem(b, id, &base, &max, &len);
    } else {
        base = f->flow[id].base;
        max = f->flow[id].max_items;
        len = f->flow[id].len_item;
    }
    if (!max || !len || len > sizeof(item)) {
        return;
    }
    wr = tcm16(b, SH_H2D_W + id * 2);
    while (f->h2d_rd[id] != wr && wr < max) {
        dma(b, base + (uint64_t)f->h2d_rd[id] * len, item, len, false);
        f->h2d_rd[id] = (f->h2d_rd[id] + 1) % max;
        if (id == RING_CTRL_SUBMIT) {
            fw_ctrl_msg(b, item);
        } else if (id == RING_RXPOST) {
            /* host_rxbuf_post_t: data_buf_len 0xa, data_buf_addr 0x18 */
            bufq_push(&f->rx_bufs, ldl_le_p(item + 4),
                      ldl_le_p(item + 0x18) |
                      (uint64_t)ldl_le_p(item + 0x1c) << 32,
                      lduw_le_p(item + 0xa));
        } else if (item[0] == MSG_TX_POST) {
            uint8_t rsp[16] = { 0 };

            put_hdr(rsp, MSG_TX_STATUS, item[1], ldl_le_p(item + 4));
            stw_le_p(rsp + 10, id);
            d2h_post(b, D2H_TX_CPL, rsp, sizeof(rsp));
        }
    }
    tcm16_set(b, SH_H2D_R + id * 2, f->h2d_rd[id]);
}

static void fw_doorbell(MH2PBCM4359 *b, unsigned which, uint32_t value)
{
    FwState *f = b->fw;

    if (!b->fw_running) {
        return;
    }
    f->d2h_posted = false;
    if (which == 0) {
        fw_h2d_ring(b, RING_CTRL_SUBMIT);
        fw_h2d_ring(b, RING_RXPOST);
        for (unsigned id = 2; id < MAX_H2D_RINGS; id++) {
            if (f->flow[id].active) {
                fw_h2d_ring(b, id);
            }
        }
    } else {
        uint32_t mb = tcm32(b, SH_H2D_MB);

        tcm32_set(b, SH_H2D_MB, 0);
        FW_LOG("H2D mailbox data 0x%x\n", mb);
        if (mb & H2D_HOST_D3_INFORM) {
            tcm32_set(b, SH_D2H_MB, D2H_DEV_D3_ACK);
            raise_host_int(b, PCIE_MB_FN0_0);
        }
    }
    if (f->d2h_posted) {
        raise_host_int(b, PCIE_MB_D2H_DB0);
    }
}

static void fw_stop(MH2PBCM4359 *b)
{
    if (b->boot_timer) {
        timer_del(b->boot_timer);
    }
    b->fw_running = false;
}

/* Firmware initialised: publish pciedev_shared in the last word of RAM. */
static void fw_boot(void *opaque)
{
    MH2PBCM4359 *b = opaque;
    uint8_t *sh = b->ram + (SH_ADDR - RAM_BASE);

    memset(b->fw, 0, sizeof(*b->fw));
    memset(sh, 0, 0x430);
    tcm32_set(b, SH_ADDR + 0x00, SH_FLAGS);
    stw_le_p(sh + 0x22, 256);                  /* max_host_rxbufs */
    tcm32_set(b, SH_ADDR + 0x28, SH_H2D_MB);
    tcm32_set(b, SH_ADDR + 0x2c, SH_D2H_MB);
    tcm32_set(b, SH_ADDR + 0x30, SH_RINGINFO);
    tcm32_set(b, SH_RINGINFO + 0x00, SH_RINGMEM);
    tcm32_set(b, SH_RINGINFO + 0x04, SH_H2D_W);
    tcm32_set(b, SH_RINGINFO + 0x08, SH_H2D_R);
    tcm32_set(b, SH_RINGINFO + 0x0c, SH_D2H_W);
    tcm32_set(b, SH_RINGINFO + 0x10, SH_D2H_R);
    tcm16_set(b, SH_RINGINFO + 0x34, MAX_H2D_RINGS);
    for (unsigned i = 0; i < 5; i++) {
        tcm16_set(b, SH_RINGMEM + i * 16, i);
    }
    b->fw_running = true;
    tcm32_set(b, RAM_BASE + RAM_SIZE - 4, SH_ADDR);
    FW_LOG("firmware up, shared area at 0x%x\n", SH_ADDR);
}

static FwState *fw_new(void)
{
    return g_new0(FwState, 1);
}

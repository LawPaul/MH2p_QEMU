/*
 * MH2P DRA72x RCC direct-start experiment, not a complete DRA7 SoC.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "hw/block/flash.h"
#include "hw/char/serial-mm.h"
#include "hw/core/boards.h"
#include "hw/core/irq.h"
#include "net/can_emu.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties.h"
#include "hw/cpu/a15mpcore.h"
#include "hw/arm/mh2p_rcc_ipu.h"
#include "hw/arm/mh2p_rcc_mmu.h"
#include "hw/gpio/dra7_gpio.h"
#include "hw/i2c/dra7_i2c.h"
#include "hw/ssi/dra7_mcspi.h"
#include "hw/misc/omap4_mailbox.h"
#include "hw/misc/mh2p_pcie_sideband.h"
#include "hw/misc/mh2p_pcie_config.h"
#include "hw/usb/hcd-dwc3.h"
#include "hw/core/irq.h"
#include "system/address-spaces.h"
#include "system/blockdev.h"
#include "system/reset.h"
#include "system/system.h"
#include "target/arm/cpu.h"
#include "target/arm/cpregs.h"
#include "trace.h"

#define RCC_RAM_BASE 0x80000000ULL
#define RCC_IMAGE_BASE 0x80100000ULL
#define RCC_GIC_BASE 0x48210000
#define RCC_EXTERNAL_IRQS 160
#define RCC_MONITOR_BASE 0x40000000
#define RCC_XBAR_SELECTORS 152
#define RCC_XBAR_INPUTS 512
#define RCC_UART6_CLKCTRL 0x4a005580

static const hwaddr rcc_gmac_resets[] = {
    0x48484008, /* CPSW */
    0x4848481c, /* CPDMA */
    0x48484d8c, /* SL1 */
    0x48484dcc, /* SL2 */
    0x48485204, /* WR */
};

static const struct {
    hwaddr address;
    uint32_t mask, reset;
} rcc_gmac_config[] = {
    { 0x48484024, 0x7, 0 }, /* CPSW_FLOW_CONTROL */
    { 0x48484110, 0x00f3f3ff, 0x40c0 }, /* P0_TX_IN_CTL */
    { 0x48484118, 0x33333333, 0x33221100 }, /* P0_TX_PRI_MAP */
    { 0x48484120, 0x77777777, 0 }, /* P0_CPDMA_RX_CH_MAP */
    { 0x48484208, 0x1ff, 0x113 }, /* P1_MAX_BLKS */
    { 0x48484210, 0x0ff3f3ff, 0x080040c0 }, /* P1_TX_IN_CTL */
    { 0x48484214, 0xffff, 0 }, /* P1_PORT_VLAN */
    { 0x48484218, 0x33333333, 0x33221100 }, /* P1_TX_PRI_MAP */
    { 0x48484308, 0x1ff, 0x113 }, /* P2_MAX_BLKS */
    { 0x48484310, 0x0ff3f3ff, 0x080040c0 }, /* P2_TX_IN_CTL */
    { 0x48484314, 0xffff, 0 }, /* P2_PORT_VLAN */
    { 0x48484318, 0x33333333, 0x33221100 }, /* P2_TX_PRI_MAP */
    { 0x48485214, 0xff, 0 }, /* WR_C0_RX_EN */
    { 0x48485218, 0xff, 0 }, /* WR_C0_TX_EN */
};

static const hwaddr rcc_usb_clkctrl[] = {
    0x4a0093f0, 0x4a009340, 0x4a009348, 0x4a009350,
};

static const hwaddr rcc_usb_phy_clkctrl[] = { 0x4a008640, 0x4a008688 };
static const hwaddr rcc_usb_phy_ana_config1[] = { 0x4a08404c, 0x4a08504c };

static const hwaddr rcc_mcasp_gblctl[] = {
    0x48460044, 0x48464044, 0x48468044, 0x4846c044,
    0x48470044, 0x48474044, 0x48478044, 0x4847c044,
};

static const struct {
    hwaddr base;
    bool fast_relock;
} rcc_bypass_dplls[] = {
    { 0x4a005234, true }, /* DSP */
    { 0x4a0052a8, true }, /* GMAC */
    { 0x4a0052d8, true }, /* GPU */
    { 0x4a008180, false }, /* USB: mode 6 is reserved. */
    { 0x4a0051a0, true }, /* IVA */
    { 0x4a0051e0, true }, /* ABE */
};

static const struct {
    hwaddr address;
    uint32_t reset;
} rcc_disabled_clkctrl[] = {
    { 0x4a005550, 0x30000 }, /* MCASP1 */
    { 0x4a009860, 0x30000 }, /* MCASP2 */
    { 0x4a009868, 0x30000 }, /* MCASP3 */
    { 0x4a009898, 0x30000 }, /* MCASP4 */
    { 0x4a009878, 0x30000 }, /* MCASP5 */
    { 0x4a009904, 0x30000 }, /* MCASP6 */
    { 0x4a009908, 0x30000 }, /* MCASP7 */
    { 0x4a009890, 0x30000 }, /* MCASP8 */
    { 0x4a009838, 0x30000 }, /* QSPI */
    { 0x4a009220, 0x70000 }, /* GPU */
    { 0x4a008f20, 0x70000 }, /* IVA */
    { 0x4a008f28, 0x30000 }, /* SL2 */
    { 0x4a009120, 0x70000 }, /* DSS */
    { 0x4a009130, 0x70000 }, /* BB2D */
    { 0x4a009020, 0x70000 }, /* VIP1 */
    { 0x4a005764, 0x70000 }, /* VPE */
    { 0x4a009388, 0x70000 }, /* SATA */
    { 0x4a0093b8, 0x70000 }, /* PCIESS2 */
    { 0x4a0093d0, 0x08070000 }, /* GMAC: CLKSEL_RFT reset is 4. */
};

static const hwaddr rcc_peripheral_clkctrl[] = {
    0x4a0097f0, /* MCSPI1 */
    0x4a0097f8, /* MCSPI2 */
    0x4a009800, /* MCSPI3 */
    0x4a009808, /* MCSPI4 */
    0x4a0097a0, /* I2C1 */
    0x4a0097a8, /* I2C2 */
    0x4a0097b0, /* I2C3 */
    0x4a0097b8, /* I2C4 */
    0x4a005578, /* I2C5 */
    RCC_UART6_CLKCTRL, /* UART6: default 48 MHz source only */
    0x4a009840, /* UART1 */
    0x4a009848, /* UART2 */
    0x4a009850, /* UART3 */
    0x4a009858, /* UART4 */
    0x4a009870, /* UART5 */
    0x4a0098d0, /* UART7 */
    0x4a0098e0, /* UART8 */
    0x4a0098e8, /* UART9 */
    0x4ae07880, /* UART10 */
};

static const struct {
    hwaddr address;
    uint32_t mask;
} rcc_peripheral_context[] = {
    { 0x4ae0735c, BIT(8) | BIT(0) }, /* MLB memory and DFF */
    { 0x4ae074f4, BIT(0) }, /* MCSPI1 DFF */
    { 0x4ae074fc, BIT(0) }, /* MCSPI2 DFF */
    { 0x4ae07504, BIT(0) }, /* MCSPI3 DFF */
    { 0x4ae0750c, BIT(0) }, /* MCSPI4 DFF */
    { 0x4ae074a4, BIT(1) }, /* I2C1 RFF, not DFF */
    { 0x4ae074ac, BIT(0) }, /* I2C2 DFF */
    { 0x4ae074b4, BIT(0) }, /* I2C3 DFF */
    { 0x4ae074bc, BIT(0) }, /* I2C4 DFF */
    { 0x4ae0657c, BIT(0) }, /* I2C5 DFF */
};

static const struct {
    hwaddr base, clock;
    unsigned irq, number;
} rcc_gp_timers[] = {
    { 0x48034000, 0x4a009740, 34, 3 },
    { 0x48088000, 0x4a009730, 42, 11 },
    { 0x4ae20000, 0x4ae07848, 90, 12 },
    { 0x48820000, 0x4a005558, 36, 5 },
    { 0x48822000, 0x4a005560, 37, 6 },
    { 0x48824000, 0x4a005568, 38, 7 },
    { 0x48826000, 0x4a005570, 39, 8 },
};

static const hwaddr rcc_gpio_clocks[] = {
    0x4ae07838, 0x4a009760, 0x4a009768, 0x4a009770,
    0x4a009778, 0x4a009780, 0x4a009810, 0x4a009818,
};

static const uint16_t rcc_ipu1_crossbar_defaults[] = {
    20, 48, 0, 96, 126, 127, 128, 129, 130, 19, 131, 7, 8, 9, 10, 132,
    98, 99, 51, 52, 56, 57, 69, 5, 133, 14, 66, 134, 24, 25, 34, 35,
    40, 42, 60, 61, 50, 0, 0, 22, 83, 108, 120, 78, 81, 89, 0, 0,
    119, 118, 72, 73, 117, 87, 88, 62, 63, 0,
};

static const struct {
    hwaddr address;
    uint32_t mask;
} rcc_pcie_clkctrl[] = {
    { 0x4a0093e0, 1 }, /* OCP2SCP1: HW_AUTO */
    { 0x4a0093e8, 1 }, /* OCP2SCP3: HW_AUTO */
    { 0x4a0093b0, 0x702 }, /* PCIESS1 and its three optional clocks */
};

static const struct {
    hwaddr base, clock;
    unsigned irq, serial;
} rcc_uarts[] = {
    { 0x4806a000, 0x4a009840, 67, 0 },
    { 0x4806c000, 0x4a009848, 68, 1 },
    { 0x48020000, 0x4a009850, 69, 2 },
    { 0x4806e000, 0x4a009858, 65, 3 },
    { 0x48068000, RCC_UART6_CLKCTRL, 101, 5 },
    { 0x48066000, 0x4a009870, 100, 4 },
    { 0x48420000, 0x4a0098d0, 218, 6 },
    { 0x48422000, 0x4a0098e0, 219, 7 },
    { 0x48424000, 0x4a0098e8, 220, 8 },
    { 0x4ae2b000, 0x4ae07880, 221, 9 },
};

static const struct {
    hwaddr address;
    uint32_t reset;
    uint16_t mux_modes;
    uint32_t write_mask;
} rcc_pads[] = {
    /* DRA72x SPRUIC2D: reset values and implemented MUXMODE encodings. */
    { 0x4a003400, 0x0005000f, 0xc00d }, /* GPMC_AD0 */
    { 0x4a003404, 0x0005000f, 0xc00d }, /* GPMC_AD1 */
    { 0x4a003408, 0x0005000f, 0xc00d }, /* GPMC_AD2 */
    { 0x4a00340c, 0x0005000f, 0xc00d }, /* GPMC_AD3 */
    { 0x4a003410, 0x0005000f, 0xc00d }, /* GPMC_AD4 */
    { 0x4a003414, 0x0005000f, 0xc00d }, /* GPMC_AD5 */
    { 0x4a003418, 0x0005000f, 0xc00d }, /* GPMC_AD6 */
    { 0x4a00341c, 0x0005000f, 0xc00d }, /* GPMC_AD7 */
    { 0x4a003420, 0x0005000f, 0xc00d }, /* GPMC_AD8 */
    { 0x4a003424, 0x0005000f, 0xc00d }, /* GPMC_AD9 */
    { 0x4a003428, 0x0005000f, 0xc00d }, /* GPMC_AD10 */
    { 0x4a00342c, 0x0005000f, 0xc00d }, /* GPMC_AD11 */
    { 0x4a003430, 0x0005000f, 0xc00d }, /* GPMC_AD12 */
    { 0x4a003434, 0x0005000f, 0xc00d }, /* GPMC_AD13 */
    { 0x4a003438, 0x0005000f, 0xc00d }, /* GPMC_AD14 */
    { 0x4a00343c, 0x0005000f, 0xc00d }, /* GPMC_AD15 */
    { 0x4a003440, 0x0004000f, 0xc1dd }, /* GPMC_A0 */
    { 0x4a003444, 0x0004000f, 0xc1dd }, /* GPMC_A1 */
    { 0x4a003448, 0x0004000f, 0xc1dd }, /* GPMC_A2 */
    { 0x4a00344c, 0x0004000f, 0xc1df }, /* GPMC_A3 */
    { 0x4a003450, 0x0004000f, 0xc1df }, /* GPMC_A4 */
    { 0x4a003454, 0x0004000f, 0xc1dd }, /* GPMC_A5 */
    { 0x4a003458, 0x0004000f, 0xc1dd }, /* GPMC_A6 */
    { 0x4a00345c, 0x0004000f, 0xc1dd }, /* GPMC_A7 */
    { 0x4a003460, 0x0004000f, 0xc1cd }, /* GPMC_A8 */
    { 0x4a003464, 0x0004000f, 0xc1cd }, /* GPMC_A9 */
    { 0x4a003468, 0x0004000f, 0xc1cd }, /* GPMC_A10 */
    { 0x4a00346c, 0x0004000f, 0xc1dd }, /* GPMC_A11 */
    { 0x4a003470, 0x0004000f, 0xc3f1 }, /* GPMC_A12 */
    { 0x4a003474, 0x0004000f, 0xc393 }, /* GPMC_A13 */
    { 0x4a003478, 0x0004000f, 0xc193 }, /* GPMC_A14 */
    { 0x4a00347c, 0x0004000f, 0xc093 }, /* GPMC_A15 */
    { 0x4a003480, 0x0004000f, 0xc013 }, /* GPMC_A16 */
    { 0x4a003484, 0x0004000f, 0xc013 }, /* GPMC_A17 */
    { 0x4a003488, 0x0004000f, 0xc013 }, /* GPMC_A18 */
    { 0x4a00348c, 0x0004000f, 0xc057 }, /* GPMC_A19 */
    { 0x4a003490, 0x0004000f, 0xc057 }, /* GPMC_A20 */
    { 0x4a003494, 0x0004000f, 0xc057 }, /* GPMC_A21 */
    { 0x4a003498, 0x0004000f, 0xc057 }, /* GPMC_A22 */
    { 0x4a00349c, 0x0004000f, 0xc057 }, /* GPMC_A23 */
    { 0x4a0034a0, 0x0004000f, 0xc057 }, /* GPMC_A24 */
    { 0x4a0034a4, 0x0004000f, 0xc057 }, /* GPMC_A25 */
    { 0x4a0034a8, 0x0004000f, 0xc057 }, /* GPMC_A26 */
    { 0x4a0034ac, 0x0004000f, 0xc057 }, /* GPMC_A27 */
    { 0x4a0034b0, 0x0006000f, 0xc057 }, /* GPMC_CS1 */
    { 0x4a0034b4, 0x0006000f, 0xc001 }, /* GPMC_CS0 */
    { 0x4a0034b8, 0x0006000f, 0xc003 }, /* GPMC_CS2 */
    { 0x4a0034bc, 0x0006000f, 0xc02f }, /* GPMC_CS3 */
    { 0x4a0034c0, 0x0006000f, 0xc3ff }, /* GPMC_CLK */
    { 0x4a0034c4, 0x0006000f, 0xc3ff }, /* GPMC_ADVN_ALE */
    { 0x4a0034c8, 0x0006000f, 0xc001 }, /* GPMC_OEN_REN */
    { 0x4a0034cc, 0x0006000f, 0xc001 }, /* GPMC_WEN */
    { 0x4a0034d0, 0x0006000f, 0xc2c3 }, /* GPMC_BEN0 */
    { 0x4a0034d4, 0x0006000f, 0xc2f3 }, /* GPMC_BEN1 */
    { 0x4a0034d8, 0x000e000f, 0xc001 }, /* GPMC_WAIT0 */
    { 0x4a003554, 0x0004000f, 0xc631 }, /* VIN2A_CLK0 */
    { 0x4a003558, 0x0004000f, 0xc63f }, /* VIN2A_DE0 */
    { 0x4a00355c, 0x0004000f, 0xc435 }, /* VIN2A_FLD0 */
    { 0x4a003570, 0x0004000f, 0xc731 }, /* VIN2A_D2 */
    { 0x4a003574, 0x0004000f, 0xc731 }, /* VIN2A_D3 */
    { 0x4a003578, 0x0004000f, 0xc731 }, /* VIN2A_D4 */
    { 0x4a00357c, 0x0004000f, 0xc731 }, /* VIN2A_D5 */
    { 0x4a003580, 0x0004000f, 0xc731 }, /* VIN2A_D6 */
    { 0x4a003584, 0x0004000f, 0xc731 }, /* VIN2A_D7 */
    { 0x4a003588, 0x0004000f, 0xc731 }, /* VIN2A_D8 */
    { 0x4a00358c, 0x0004000f, 0xc731 }, /* VIN2A_D9 */
    { 0x4a003590, 0x0004000f, 0xc619 }, /* VIN2A_D10 */
    { 0x4a003594, 0x0004000f, 0xc619 }, /* VIN2A_D11 */
    { 0x4a003598, 0x0004000f, 0xc719 }, /* VIN2A_D12 */
    { 0x4a00359c, 0x0004000f, 0xc719 }, /* VIN2A_D13 */
    { 0x4a0035a0, 0x0004000f, 0xc519 }, /* VIN2A_D14 */
    { 0x4a0035a4, 0x0004000f, 0xc519 }, /* VIN2A_D15 */
    { 0x4a0035a8, 0x0004000f, 0xc51d }, /* VIN2A_D16 */
    { 0x4a0035ac, 0x0004000f, 0xc51d }, /* VIN2A_D17 */
    { 0x4a0035b0, 0x0004000f, 0xc51d }, /* VIN2A_D18 */
    { 0x4a0035b4, 0x0004000f, 0xc51d }, /* VIN2A_D19 */
    { 0x4a0035b8, 0x0004000f, 0xc51d }, /* VIN2A_D20 */
    { 0x4a0035bc, 0x0004000f, 0xc11d }, /* VIN2A_D21 */
    { 0x4a0035c0, 0x0004000f, 0xc11d }, /* VIN2A_D22 */
    { 0x4a0035c4, 0x0004000f, 0xc11d }, /* VIN2A_D23 */
    { 0x4a0035c8, 0x0004000f, 0xc119 }, /* VOUT1_CLK */
    { 0x4a0035cc, 0x0004000f, 0xc119 }, /* VOUT1_DE */
    { 0x4a0035d0, 0x0004000f, 0xc119 }, /* VOUT1_FLD */
    { 0x4a0035d4, 0x0004000f, 0xc119 }, /* VOUT1_HSYNC */
    { 0x4a0035d8, 0x0004000f, 0xc119 }, /* VOUT1_VSYNC */
    { 0x4a00363c, 0x000e000f, 0xc03b }, /* MDIO_MCLK */
    { 0x4a003640, 0x000e000f, 0xc03b }, /* MDIO_D */
    { 0x4a003644, 0x0004000f, 0xc011 }, /* RMII_MHZ_50_CLK */
    { 0x4a003648, 0x000c000f, 0xc0bd }, /* UART3_RXD */
    { 0x4a00364c, 0x000c000f, 0xc1bd }, /* UART3_TXD */
    { 0x4a003650, 0x0004000f, 0xc1ff }, /* RGMII0_TXC */
    { 0x4a003654, 0x0004000f, 0xc1ff }, /* RGMII0_TXCTL */
    { 0x4a003658, 0x0004000f, 0xc1fb }, /* RGMII0_TXD3 */
    { 0x4a00365c, 0x0004000f, 0xc1fb }, /* RGMII0_TXD2 */
    { 0x4a003660, 0x0004000f, 0xc1fb }, /* RGMII0_TXD1 */
    { 0x4a003664, 0x0004000f, 0xc1db }, /* RGMII0_TXD0 */
    { 0x4a003668, 0x0004000f, 0xc07d }, /* RGMII0_RXC */
    { 0x4a00366c, 0x0004000f, 0xc07d }, /* RGMII0_RXCTL */
    { 0x4a003670, 0x0004000f, 0xc07d }, /* RGMII0_RXD3 */
    { 0x4a003674, 0x0004000f, 0xc05b }, /* RGMII0_RXD2 */
    { 0x4a003678, 0x0004000f, 0xc05b }, /* RGMII0_RXD1 */
    { 0x4a00367c, 0x0004000f, 0xc07b }, /* RGMII0_RXD0 */
    { 0x4a003680, 0x000c000f, 0xc081 }, /* USB1_DRVVBUS */
    { 0x4a003684, 0x000c000f, 0xc081 }, /* USB2_DRVVBUS */
    { 0x4a003690, 0x0006000f, 0xc743 }, /* GPIO6_16 */
    { 0x4a003694, 0x0004000f, 0xc7bf }, /* XREF_CLK0 */
    { 0x4a003698, 0x0004000f, 0xc4bf }, /* XREF_CLK1 */
    { 0x4a00369c, 0x0004000f, 0xc57f }, /* XREF_CLK2 */
    { 0x4a0036a0, 0x0004000f, 0xc7ff }, /* XREF_CLK3 */
    { 0x4a0036a4, 0x0004000f, 0xc481 }, /* MCASP1_ACLKX */
    { 0x4a0036a8, 0x000c000f, 0xc481 }, /* MCASP1_FSX */
    { 0x4a0036ac, 0x0004000f, 0xc543 }, /* MCASP1_ACLKR */
    { 0x4a0036b0, 0x0004000f, 0xc543 }, /* MCASP1_FSR */
    { 0x4a0036b4, 0x000c000f, 0xc489 }, /* MCASP1_AXR0 */
    { 0x4a0036b8, 0x000c000f, 0xc489 }, /* MCASP1_AXR1 */
    { 0x4a0036bc, 0x0004000f, 0xc14b }, /* MCASP1_AXR2 */
    { 0x4a0036c0, 0x0004000f, 0xc14b }, /* MCASP1_AXR3 */
    { 0x4a0036c4, 0x0004000f, 0xc143 }, /* MCASP1_AXR4 */
    { 0x4a0036c8, 0x0004000f, 0xc143 }, /* MCASP1_AXR5 */
    { 0x4a0036cc, 0x0004000f, 0xc143 }, /* MCASP1_AXR6 */
    { 0x4a0036d0, 0x0004000f, 0xc543 }, /* MCASP1_AXR7 */
    { 0x4a0036d4, 0x000c000f, 0xc48b }, /* MCASP1_AXR8 */
    { 0x4a0036d8, 0x000c000f, 0xc48b }, /* MCASP1_AXR9 */
    { 0x4a0036dc, 0x000c000f, 0xc48f }, /* MCASP1_AXR10 */
    { 0x4a0036e0, 0x000c000f, 0xc48f }, /* MCASP1_AXR11 */
    { 0x4a0036e4, 0x000c000f, 0xc48b }, /* MCASP1_AXR12 */
    { 0x4a0036e8, 0x000c000f, 0xc483 }, /* MCASP1_AXR13 */
    { 0x4a0036ec, 0x000c000f, 0xc487 }, /* MCASP1_AXR14 */
    { 0x4a0036f0, 0x000c000f, 0xc487 }, /* MCASP1_AXR15 */
    { 0x4a0036f4, 0x0004000f, 0x8081 }, /* MCASP2_ACLKX */
    { 0x4a0036f8, 0x000c000f, 0x8081 }, /* MCASP2_FSX */
    { 0x4a0036fc, 0x0004000f, 0x8143 }, /* MCASP2_ACLKR */
    { 0x4a003700, 0x0004000f, 0x8143 }, /* MCASP2_FSR */
    { 0x4a003704, 0x0004000f, 0x8141 }, /* MCASP2_AXR0 */
    { 0x4a003708, 0x0004000f, 0x8141 }, /* MCASP2_AXR1 */
    { 0x4a00370c, 0x000c000f, 0xc083 }, /* MCASP2_AXR2 */
    { 0x4a003710, 0x000c000f, 0xc083 }, /* MCASP2_AXR3 */
    { 0x4a003714, 0x0004000f, 0xc143 }, /* MCASP2_AXR4 */
    { 0x4a003718, 0x0004000f, 0xc143 }, /* MCASP2_AXR5 */
    { 0x4a00371c, 0x0004000f, 0xc147 }, /* MCASP2_AXR6 */
    { 0x4a003720, 0x0004000f, 0xc147 }, /* MCASP2_AXR7 */
    { 0x4a003724, 0x0004000f, 0xc08f }, /* MCASP3_ACLKX */
    { 0x4a003728, 0x000c000f, 0xc08f }, /* MCASP3_FSX */
    { 0x4a00372c, 0x000c000f, 0x809d }, /* MCASP3_AXR0 */
    { 0x4a003730, 0x000c000f, 0x829d }, /* MCASP3_AXR1 */
    { 0x4a003734, 0x0004000f, 0x835f }, /* MCASP4_ACLKX */
    { 0x4a003738, 0x0004000f, 0x835f }, /* MCASP4_FSX */
    { 0x4a00373c, 0x0004000f, 0xc35d }, /* MCASP4_AXR0 */
    { 0x4a003740, 0x0004000f, 0xc35d }, /* MCASP4_AXR1 */
    { 0x4a003744, 0x0004000f, 0x837f }, /* MCASP5_ACLKX */
    { 0x4a003748, 0x0004000f, 0x835f }, /* MCASP5_FSX */
    { 0x4a00374c, 0x0004000f, 0x837d }, /* MCASP5_AXR0 */
    { 0x4a003750, 0x0004000f, 0x837d }, /* MCASP5_AXR1 */
    { 0x4a003754, 0x0006000f, 0xc001, 0x010701ff }, /* MMC1_CLK */
    { 0x4a003758, 0x0006000f, 0xc001, 0x010701ff }, /* MMC1_CMD */
    { 0x4a00375c, 0x0006000f, 0xc001, 0x010701ff }, /* MMC1_DAT0 */
    { 0x4a003760, 0x0006000f, 0xc001, 0x010701ff }, /* MMC1_DAT1 */
    { 0x4a003764, 0x0006000f, 0xc001, 0x010701ff }, /* MMC1_DAT2 */
    { 0x4a003768, 0x0006000f, 0xc001, 0x010701ff }, /* MMC1_DAT3 */
    { 0x4a00376c, 0x000e000f, 0xc019 }, /* MMC1_SDCD */
    { 0x4a003770, 0x000c000f, 0xc019 }, /* MMC1_SDWP */
    { 0x4a003774, 0x0006000f, 0xc61f }, /* GPIO6_10 */
    { 0x4a003778, 0x0006000f, 0xc61f }, /* GPIO6_11 */
    { 0x4a00377c, 0x0006000f, 0xc619 }, /* MMC3_CLK */
    { 0x4a003780, 0x0006000f, 0xc61b }, /* MMC3_CMD */
    { 0x4a003784, 0x0006000f, 0xc61f }, /* MMC3_DAT0 */
    { 0x4a003788, 0x0006000f, 0xc61f }, /* MMC3_DAT1 */
    { 0x4a00378c, 0x0006000f, 0xc61f }, /* MMC3_DAT2 */
    { 0x4a003790, 0x0006000f, 0xc61f }, /* MMC3_DAT3 */
    { 0x4a003794, 0x0006000f, 0xc61f }, /* MMC3_DAT4 */
    { 0x4a003798, 0x0006000f, 0xc61f }, /* MMC3_DAT5 */
    { 0x4a00379c, 0x0006000f, 0xc61f }, /* MMC3_DAT6 */
    { 0x4a0037a0, 0x0006000f, 0xc61f }, /* MMC3_DAT7 */
    { 0x4a0037a4, 0x0004000f, 0xc001 }, /* SPI1_SCLK */
    { 0x4a0037a8, 0x0004000f, 0xc001 }, /* SPI1_D1 */
    { 0x4a0037ac, 0x0004000f, 0xc001 }, /* SPI1_D0 */
    { 0x4a0037b0, 0x0006000f, 0xc001 }, /* SPI1_CS0 */
    { 0x4a0037b4, 0x0006000f, 0xc00d }, /* SPI1_CS1 */
    { 0x4a0037b8, 0x000e000f, 0xc07f }, /* SPI1_CS2 */
    { 0x4a0037bc, 0x000e000f, 0xc07f }, /* SPI1_CS3 */
    { 0x4a0037c0, 0x0004000f, 0xc003 }, /* SPI2_SCLK */
    { 0x4a0037c4, 0x000c000f, 0xc003 }, /* SPI2_D1 */
    { 0x4a0037c8, 0x000c000f, 0xc007 }, /* SPI2_D0 */
    { 0x4a0037cc, 0x000e000f, 0xc007 }, /* SPI2_CS0 */
    { 0x4a0037e0, 0x000e000f, 0xc009 }, /* UART1_RXD */
    { 0x4a0037e4, 0x000e000f, 0xc009 }, /* UART1_TXD */
    { 0x4a0037e8, 0x0006000f, 0xc00d }, /* UART1_CTSN */
    { 0x4a0037ec, 0x0006000f, 0xc00d }, /* UART1_RTSN */
    { 0x4a0037f0, 0x0006000f, 0xc03f }, /* UART2_RXD */
    { 0x4a0037f4, 0x0006000f, 0xc03f }, /* UART2_TXD */
    { 0x4a0037fc, 0x0006000f, 0xc03f }, /* UART2_RTSN */
    { 0x4a003800, 0x00060000, 0x0001, 0x01070000 }, /* I2C1_SDA */
    { 0x4a003804, 0x00060000, 0x0001, 0x01070000 }, /* I2C1_SCL */
    { 0x4a003808, 0x0006000f, 0x8003, 0x0107000f }, /* I2C2_SDA */
    { 0x4a00380c, 0x0006000f, 0x8003, 0x0107000f }, /* I2C2_SCL */
    { 0x4a003818, 0x0001000f, 0xc003, 0x010301ff }, /* WAKEUP0 */
    { 0x4a003824, 0x0001000f, 0xc003, 0x010301ff }, /* WAKEUP3 */
    { 0x4a003828, 0x00020000, 0x0001, 0x00030000 }, /* ON_OFF */
    { 0x4a00382c, 0x00010000, 0x0001, 0x00030000 }, /* RTC_PORZ */
    { 0x4a003830, 0x00060000, 0x0001, 0x000f0000 }, /* TMS */
    { 0x4a003834, 0x000e0000, 0x4001, 0x000f01ff }, /* TDI */
    { 0x4a003838, 0x00060000, 0x4001, 0x000f01ff }, /* TDO */
    { 0x4a00383c, 0x00060000, 0x0001, 0x00070000 }, /* TCLK */
    { 0x4a003840, 0x00040000, 0x0001, 0x000f0000 }, /* TRSTN */
    { 0x4a003844, 0x00050000, 0x4001, 0x000f01ff }, /* RTCK */
    { 0x4a003848, 0x00060000, 0x4001, 0x000f01ff }, /* EMU0 */
    { 0x4a00384c, 0x00060000, 0x4001, 0x000f01ff }, /* EMU1 */
    { 0x4a00385c, 0x00020000, 0x0001, 0x00030000 }, /* RESETN */
    { 0x4a003860, 0x00000000, 0x0001, 0x00030000 }, /* NMIN_DSP */
    { 0x4a003864, 0x00000000, 0x0001, 0x00030000 }, /* RSTOUTN */
    { 0x4a0037f8, 0x0006000f, 0xc03d }, /* UART2_CTSN */
    { 0x4a0035dc, 0x0004000f, 0xc11d }, /* VOUT1_D0 */
    { 0x4a0035e0, 0x0004000f, 0xc01d }, /* VOUT1_D1 */
    { 0x4a0035e4, 0x0004000f, 0xc0fd }, /* VOUT1_D2 */
    { 0x4a0035e8, 0x0004000f, 0xc0fd }, /* VOUT1_D3 */
    { 0x4a0035ec, 0x0004000f, 0xc07d }, /* VOUT1_D4 */
    { 0x4a0035f0, 0x0004000f, 0xc07d }, /* VOUT1_D5 */
    { 0x4a0035f4, 0x0004000f, 0xc07d }, /* VOUT1_D6 */
    { 0x4a0035f8, 0x0004000f, 0xc01d }, /* VOUT1_D7 */
    { 0x4a0035fc, 0x0004000f, 0xc01d }, /* VOUT1_D8 */
    { 0x4a003600, 0x0004000f, 0xc01d }, /* VOUT1_D9 */
    { 0x4a003604, 0x0004000f, 0xc0fd }, /* VOUT1_D10 */
    { 0x4a003608, 0x0004000f, 0xc0fd }, /* VOUT1_D11 */
    { 0x4a00360c, 0x0004000f, 0xc07d }, /* VOUT1_D12 */
    { 0x4a003610, 0x0004000f, 0xc07d }, /* VOUT1_D13 / GPIO8_13 */
    { 0x4a003614, 0x0004000f, 0xc07d }, /* VOUT1_D14 */
    { 0x4a003618, 0x0004000f, 0xc07d }, /* VOUT1_D15 */
    { 0x4a00361c, 0x0004000f, 0xc01d }, /* VOUT1_D16 */
    { 0x4a003620, 0x0004000f, 0xc01d }, /* VOUT1_D17 */
    { 0x4a003624, 0x0004000f, 0xc07d }, /* VOUT1_D18 */
    { 0x4a003628, 0x0004000f, 0xc07d }, /* VOUT1_D19 */
    { 0x4a00362c, 0x0004000f, 0xc07d }, /* VOUT1_D20 */
    { 0x4a003630, 0x0004000f, 0xc07d }, /* VOUT1_D21 */
    { 0x4a003634, 0x0004000f, 0xc07d }, /* VOUT1_D22 */
    { 0x4a003638, 0x0004000f, 0xc11d }, /* VOUT1_D23 */
    { 0x4a003688, 0x0006000f, 0xc74f }, /* GPIO6_14 / DCAN2_TX */
    { 0x4a00368c, 0x0006000f, 0xc74f }, /* GPIO6_15 / DCAN2_RX */
    { 0x4a0037d0, 0x000e000f, 0xc04d }, /* DCAN1_TX */
    { 0x4a0037d4, 0x000e000f, 0xc05d }, /* DCAN1_RX */
    { 0x4a003560, 0x0004000f, 0xc7b9 }, /* VIN2A_HSYNC0 */
    { 0x4a003564, 0x0004000f, 0xc7b9 }, /* VIN2A_VSYNC0 */
    { 0x4a003568, 0x0004000f, 0xc7b1 }, /* VIN2A_D0 */
    { 0x4a00356c, 0x0004000f, 0xc7b1 }, /* VIN2A_D1 */
};

enum {
    RCC_SCP_SYSCONFIG,
    RCC_SCP_TIMING,
    RCC_PIPE3_CFG1,
    RCC_PIPE3_CFG2,
    RCC_PIPE3_CFG3,
    RCC_PIPE3_CFG4,
    RCC_PIPE3_POWER,
    RCC_SCP3_SYSCONFIG,
    RCC_SCP3_TIMING,
    RCC_PIPE3_REG_COUNT,
};

static const struct {
    hwaddr address;
    uint32_t rw, w0c, w1c, reset;
} rcc_mlb_registers[] = {
    { 0x4842c004, 1, 0, 0, 0 },                    /* MLBSSPWR */
    { 0x4842c100, 0x17733, 0, 0, 0x4010 },         /* MLBSSPRF */
    { 0x4842c400, 0x3d03d, 0, 0, 0 },              /* MLBC0 */
    { 0x4842c408, 0, 0, 0, 0 },                    /* Reserved */
    { 0x4842c40c, 0, UINT32_MAX, 0, 0 },            /* MS0 */
    { 0x4842c414, 0, UINT32_MAX, 0, 0 },            /* MS1 */
    { 0x4842c420, 0x20, 0x1f, 0, 0 },              /* MSS */
    { 0x4842c424, 0, 0, 0, 0 },                    /* MSD */
    { 0x4842c42c, 0x3f7f0003, 0, 0, 0 },           /* MIEN */
    { 0x4842c43c, 0xff00, 0xc0, 0, 0 },            /* MLBC1 */
    { 0x4842c480, 0x8000, 0, 0, 0 },               /* DIENR */
    { 0x4842c488, UINT32_MAX, 0, 0, 0 },            /* DICER0 */
    { 0x4842c48c, UINT32_MAX, 0, 0, 0 },            /* DICER1 */
    { 0x4842c7c0, 0x17, 0, 0, 0 },                 /* DCTL */
    { 0x4842c7d0, 0, 0, UINT32_MAX, 0 },            /* DCSR0 */
    { 0x4842c7d4, 0, 0, UINT32_MAX, 0 },            /* DCSR1 */
    { 0x4842c7d8, UINT32_MAX, 0, 0, 0 },            /* DCMR0 */
    { 0x4842c7dc, UINT32_MAX, 0, 0, 0 },            /* DCMR1 */
};

static const struct {
    hwaddr address;
    uint32_t mask, reset;
} rcc_ddr_registers[] = {
    { 0x4a002e30, 0xffffffff, 0x4a4a4a4a },
    { 0x4a002e38, 0xffffffff, 0x4a4a4a4a },
    { 0x4a002e3c, 0xffffffff, 0x4a4a4a4a },
    { 0x4a002e48, 0x00ffff00, 0x004a4a00 },
    { 0x4a002e50, 0x000ffc00, 0x0009ce60 },
    { 0x4ae0c144, 0x0003feef, 0x00000107 },
};

static const struct {
    hwaddr address;
    uint32_t mask, reset;
} rcc_pipe3_registers[RCC_PIPE3_REG_COUNT] = {
    [RCC_SCP_SYSCONFIG] = { 0x4a080010, 0x1b, 0x11 },
    [RCC_SCP_TIMING] = { 0x4a080018, 0x3ff, 1 },
    [RCC_PIPE3_CFG1] = { 0x4a084c0c, 0x1ffffe, 0 },
    [RCC_PIPE3_CFG2] = { 0x4a084c10, 0x60f, 0x2008 },
    [RCC_PIPE3_CFG3] = { 0x4a084c14, 0x3fc00, 0 },
    [RCC_PIPE3_CFG4] = { 0x4a084c20, 0x3ffff, 0 },
    [RCC_PIPE3_POWER] = { 0x4a002370, 0xffffc000, 0 },
    [RCC_SCP3_SYSCONFIG] = { 0x4a090010, 0x1b, 0x11 },
    [RCC_SCP3_TIMING] = { 0x4a090018, 0x3ff, 1 },
};

static const struct {
    unsigned offset;
    uint32_t mask, reset;
} rcc_pcie_rx_registers[] = {
    { 0x0c, 0xffffff60, 0 },
    { 0x1c, 0xc0000000, 0 },
    { 0x24, 0xc0000000, 0 },
    { 0x28, 0xdcfff800, 0x02e33000 },
    { 0x38, 0xffffff86, 0 },
};

typedef struct RCCUART {
    MemoryRegion registers;
    SerialMM *core;
    hwaddr base;
    uint8_t mdr1;
    uint8_t sysconfig;
    qemu_irq irq;
    bool clock_enabled, irq_level;
} RCCUART;

typedef struct RCCCrossbar {
    struct RCCCrossbar *peer;
    const uint16_t *defaults;
    const uint32_t *mmr_lock;
    MemoryRegion registers;
    uint16_t selection[RCC_XBAR_SELECTORS];
    bool levels[RCC_XBAR_INPUTS];
    qemu_irq *inputs;
    qemu_irq outputs[RCC_XBAR_SELECTORS];
    qemu_irq secondary_outputs[RCC_XBAR_SELECTORS];
    unsigned selector_count;
    bool mpu;
} RCCCrossbar;

typedef struct RCCTimer {
    unsigned number;
    MemoryRegion registers;
    QEMUTimer *expiry;
    /* Retain fractional cycles (1/1e9 units) across reads and clock gating. */
    uint32_t count, phase;
    int64_t epoch_ns;
    qemu_irq irq;
    uint32_t config, control, load, match, posted;
    uint32_t status, enabled, wakeup;
    uint32_t clock_control;
} RCCTimer;

typedef struct RCCDCANMessage {
    uint32_t mask, arbitration, control, data[2];
} RCCDCANMessage;

typedef struct RCCDCAN {
    MemoryRegion registers;
    CanBusClientState bus_client;
    uint32_t control, bit_timing, status, clock_control, auto_bus_on;
    QEMUTimer *transmit_timer;
    int64_t transmit_remaining;
    unsigned transmit_errors;
    unsigned transmit_object;
    bool status_pending;
    RCCDCANMessage interface[2], messages[128];
    uint32_t command[2], interrupt_mux[4];
    qemu_irq irq[2];
} RCCDCAN;

static const hwaddr rcc_dcan_clocks[] = { 0x4ae07888, 0x4a0098f0 };

static const struct {
    hwaddr base, clock;
    unsigned number, irq;
} rcc_spis[] = {
    { 0x48098000, 0x4a0097f0, 1, 60 },
    { 0x4809a000, 0x4a0097f8, 2, 61 },
    { 0x480b8000, 0x4a009800, 3, 86 },
    { 0x480ba000, 0x4a009808, 4, 43 },
};

static const struct {
    hwaddr base, clock;
    unsigned irq;
} rcc_i2cs[] = {
    { 0x48070000, 0x4a0097a0, 51 },
    { 0x48072000, 0x4a0097a8, 52 },
    { 0x48060000, 0x4a0097b0, 56 },
    { 0x4807a000, 0x4a0097b8, 57 },
    { 0x4807c000, 0x4a005578, 55 },
};

/* DRA7 endpoint capabilities, SPRUI30H tables 24-565 through 24-579. */
static const struct {
    unsigned offset;
    uint32_t reset, writable, w1c;
} rcc_pcie_capabilities[] = {
    { 0x40, 0x5a035001, 0, 0 },
    { 0x44, 0, 0x103, 0x8000 },
    { 0x50, 0x00807005, 0x00710000, 0 },
    { 0x54, 0, 0xfffffffc, 0 },
    { 0x58, 0, 0xffffffff, 0 },
    { 0x5c, 0, 0x0000ffff, 0 },
    { 0x70, 0x00020010, 0, 0 },
    { 0x74, 0x00008701, 0, 0 },
    { 0x78, 0x2010, 0x7fff, 0x000f0000 },
    { 0x80, 0x10110008, 0x0dcb, 0 },
    /* Slot/root registers are reserved for a type-0 endpoint. */
    { 0x84, 0, 0, 0 },
    { 0x88, 0, 0, 0 },
    { 0x8c, 0, 0, 0 },
    { 0x90, 0, 0, 0 },
    { 0x94, 0x11, 0, 0 },
    { 0x98, 0, 0x10, 0 },
    { 0x9c, 6, 0, 0 },
    { 0xa0, 0x10001, 0xf, 0x00200000 },
    /* No extended capabilities are implemented or advertised. */
    { 0x100, 0, 0, 0 },
};

typedef struct RCCPCIERegion {
    uint32_t control1, control2, target, base, limit;
} RCCPCIERegion;

typedef struct RCCGMACConfig {
    MemoryRegion registers;
    uint32_t value, mask;
} RCCGMACConfig;

typedef struct RCCPCIEController {
    MemoryRegion registers;
    MemoryRegion outbound_window;
    uint32_t type, command;
    uint32_t identity, pci_command, class_code, link_cap;
    uint32_t bar[6], bar_mask[6];
    uint32_t cache_line, interrupt_line;
    uint32_t subsystem;
    uint32_t capabilities[ARRAY_SIZE(rcc_pcie_capabilities)];
    uint32_t viewport;
    RCCPCIERegion inbound[4], outbound[4];
} RCCPCIEController;

typedef struct MH2PRCCMachineState {
    MachineState parent;
    Notifier init_done;
    ARMCPU *cpu;
    MemoryRegion peripherals;
    MemoryRegion monitor;
    MemoryRegion gpmc, nor_window;
    uint32_t gpmc_sysconfig, gpmc_irqenable, gpmc_timeout, gpmc_config;
    uint32_t gpmc_cs0[7];
    uint32_t l2actlr;
    MemoryRegion iodelay;
    uint32_t iodelay_lock;
    uint32_t pad_delay[(0xd1c - 0x30) / 4 + 1];
    MemoryRegion edma;
    MemoryRegion gmac_reset[ARRAY_SIZE(rcc_gmac_resets)];
    MemoryRegion gmac_cppi_ram;
    MemoryRegion gmac_dma;
    uint32_t gmac_dma_control[2], gmac_dma_mask[3];
    uint32_t gmac_dma_rx_offset;
    uint32_t gmac_dma_hdp[2][8];
    MemoryRegion gmac_ale;
    uint32_t gmac_ale_control, gmac_ale_index;
    uint32_t gmac_ale_words[3], gmac_ale_table[1024][3];
    uint32_t gmac_ale_port[3];
    MemoryRegion gmac_mdio;
    QEMUTimer *gmac_mdio_timer;
    uint32_t gmac_mdio_control, gmac_mdio_user[2], gmac_mdio_phy[2];
    uint32_t gmac_mdio_raw;
    int gmac_mdio_active;
    unsigned gmac_mdio_next;
    RCCGMACConfig gmac_config[ARRAY_SIZE(rcc_gmac_config)];
    uint32_t edma_map[64];
    uint32_t edma_access[24];
    uint32_t edma_qmap[8], edma_param[512][8];
    uint32_t edma_ier[9][2], edma_qeer[9], edma_qemr;
    uint32_t edma_ipr[9][2];
    uint32_t edma_epoch;
    uint32_t edma_queue[9];
    uint32_t usb_clkctrl[4];
    uint32_t usb_phy_clkctrl[2];
    uint32_t usb_phy_ana_config1[2];
    uint32_t usb_60m_clkctrl, usb2_phy_power;
    uint32_t pbias_control;
    uint32_t ddr_regs[ARRAY_SIZE(rcc_ddr_registers)];
    uint32_t atl_clkctrl, atl_clkstctrl;
    uint32_t mlb_clkctrl;
    uint32_t mlb_l3_pressure;
    uint32_t mlb_clk_bg_ctrl;
    uint32_t mlb_io_ctrl[2];
    uint32_t mlb_dll;
    uint32_t mlb_clock;
    uint32_t mlb_mdat[4], mlb_mdwe[4];
    uint32_t mlb_mctl, mlb_madr;
    uint32_t mlb_channel_table[256][4];
    uint32_t mlb_regs[ARRAY_SIZE(rcc_mlb_registers)];
    uint32_t ipu1_clkctrl, ipu1_clkstctrl;
    uint32_t ipu1_reset_control, ipu1_reset_status, ipu1_reset_asserted;
    MH2PIPU1 *ipu1;
    bool ipu1_native, ipu1_clock_running;
    uint32_t ipu2_clkctrl, ipu2_clkstctrl;
    uint32_t ipu2_reset_control, ipu2_reset_status, ipu2_reset_asserted;
    MH2PIPU1 *ipu2;
    bool ipu2_native, ipu2_clock_running;
    uint32_t ipu_per_clkstctrl;
    bool ipu_per_clock_active;
    uint32_t mmr_lock2;
    uint32_t control_io1;
    uint32_t peripheral_clkctrl[ARRAY_SIZE(rcc_peripheral_clkctrl)];
    uint32_t peripheral_context[ARRAY_SIZE(rcc_peripheral_context)];
    uint32_t pcie_clkctrl[ARRAY_SIZE(rcc_pcie_clkctrl)];
    uint32_t pcie_clkstctrl;
    MemoryRegion pcie_pll;
    uint32_t pcie_dpll_mode, pcie_dpll_select, pcie_dpll_divider;
    uint32_t mpu_dpll_mode;
    MemoryRegion mpu_watchdog;
    QEMUTimer *mpu_watchdog_timer;
    uint32_t mpu_wdt_load, mpu_wdt_warning, mpu_wdt_prescaler;
    uint32_t mpu_wdt_sampled_prescaler, mpu_wdt_control, mpu_wdt_status;
    uint64_t mpu_wdt_remaining, mpu_wdt_phase;
    int64_t mpu_wdt_epoch;
    bool mpu_wdt_warned, mpu_wdt_expired, mpu_wdt_reset_requested;
    bool power_clocks;
    uint32_t bypass_dpll_mode[ARRAY_SIZE(rcc_bypass_dplls)];
    uint32_t disabled_clkctrl[ARRAY_SIZE(rcc_disabled_clkctrl)];
    uint32_t pcie_apll_mode, pcie_acs_control;
    MemoryRegion pcie_rx;
    uint32_t pcie_rx_regs[ARRAY_SIZE(rcc_pcie_rx_registers)];
    uint32_t pcie_phy_power, pcie_pcs_delay;
    uint32_t pcie_reset_control, pcie_reset_status;
    RCCPCIEController pcie;
    bool pcie_dpll_initialized;
    bool pcie_external_reference;
    DeviceState *pcie_transport;
    char *pcie_fd;
    DeviceState *pcie_outbound_transport;
    char *pcie_outbound_fd;
    char *ipu1_firmware;
    bool ipu1_smp;
    uint32_t pad_config[ARRAY_SIZE(rcc_pads)];
    MemoryRegion atl;
    uint32_t atl_config[4][8];
    MemoryRegion timer2;
    MemoryRegion ocmc_ram;
    int64_t timer2_epoch_ns;
    RCCTimer gp_timer[ARRAY_SIZE(rcc_gp_timers)];
    uint32_t pipe3_regs[RCC_PIPE3_REG_COUNT];
    bool pipe3_active_valid;
    RCCCrossbar crossbar;
    RCCCrossbar dsp_crossbar[2];
    RCCCrossbar ipu_crossbar;
    RCCCrossbar ipu2_crossbar;
    DeviceState *usb[2];
    RCCUART uart[ARRAY_SIZE(rcc_uarts)];
    DeviceState *gpio[ARRAY_SIZE(rcc_gpio_clocks)];
    uint32_t gpio_clkctrl[ARRAY_SIZE(rcc_gpio_clocks)];
    MemoryRegion spinlock;
    uint32_t spinlocks[8];
    RCCDCAN dcan[2];
    CanBusState *canbus[2];
    DeviceState *spi[ARRAY_SIZE(rcc_spis)];
    DeviceState *i2c[ARRAY_SIZE(rcc_i2cs)];
    MemoryRegion watchdog;
    uint32_t watchdog_stop;
    MemoryRegion dma;
    uint32_t dma_sysconfig, dma_gcr, dma_irqenable[4];
    uint32_t dma_channel[32][3];
    uint32_t dma_index[32][4];
    uint32_t entry;
} MH2PRCCMachineState;

static const uint32_t rcc_gpmc_masks[] = {
    0xffef3f13, 0x001f1f8f, 0x771f1fff, 0x1f8fffff,
    0x0f1f1f1f, 0x1f0f0fcf, 0x00000f7f,
};

static void rcc_gpmc_map(MH2PRCCMachineState *s)
{
    uint32_t config = s->gpmc_cs0[6];

    memory_region_transaction_begin();
    memory_region_set_enabled(&s->nor_window, false);
    if (config & BIT(6)) {
        uint64_t size = (16 * MiB) * (16 - ((config >> 8) & 0xf));

        memory_region_set_address(&s->nor_window,
                                  (config & 0x3f) * (16 * MiB));
        memory_region_set_size(&s->nor_window, size);
        memory_region_set_enabled(&s->nor_window, true);
    }
    memory_region_transaction_commit();
}

static void rcc_gpmc_reset(MH2PRCCMachineState *s, bool handoff)
{
    /* SPRUIC2D tables 15-513..527; explicit 16-bit NOR fixture straps. */
    static const uint32_t reset[] = {
        0x00001000, 0x00101001, 0x22060514, 0x10057016,
        0x010f1111, 0x8f070000, 0x00000f40,
    };

    s->gpmc_sysconfig = 0;
    s->gpmc_irqenable = 0;
    s->gpmc_timeout = 0x1ff0;
    s->gpmc_config = 0x200;
    memcpy(s->gpmc_cs0, reset, sizeof(reset));
    if (handoff) {
        s->gpmc_cs0[6] = 0xc48;
    }
    rcc_gpmc_map(s);
}

static MemTxResult rcc_gpmc_read(void *opaque, hwaddr offset,
                               uint64_t *value, unsigned size,
                               MemTxAttrs attrs)
{
    MH2PRCCMachineState *s = opaque;

    if (size != 4 || (offset & 3)) {
        goto unsupported;
    }
    if (offset >= 0x60 && offset <= 0x78) {
        *value = s->gpmc_cs0[(offset - 0x60) / 4];
        return MEMTX_OK;
    }
    switch (offset) {
    case 0x10:
        *value = s->gpmc_sysconfig;
        return MEMTX_OK;
    case 0x14:
        *value = 1;
        return MEMTX_OK;
    case 0x18:
        /* No WAIT inputs or NAND prefetch events are connected. */
        *value = 0;
        return MEMTX_OK;
    case 0x1c:
        *value = s->gpmc_irqenable;
        return MEMTX_OK;
    case 0x40:
        *value = s->gpmc_timeout;
        return MEMTX_OK;
    case 0x50:
        *value = s->gpmc_config;
        return MEMTX_OK;
    }
unsupported:
    qemu_log_mask(LOG_UNIMP,
                  "mh2p-rcc: unsupported GPMC read at 0x%" HWADDR_PRIx
                  " size=%u\n", offset, size);
    return MEMTX_ERROR;
}

static MemTxResult rcc_gpmc_write(void *opaque, hwaddr offset,
                                uint64_t value, unsigned size,
                                MemTxAttrs attrs)
{
    MH2PRCCMachineState *s = opaque;

    if (size != 4 || (offset & 3)) {
        goto unsupported;
    }
    if (offset >= 0x60 && offset <= 0x78) {
        unsigned index = (offset - 0x60) / 4;

        if (value & ~(rcc_gpmc_masks[index] | (index == 5 ? BIT(31) : 0))) {
            goto unsupported;
        }
        if (index == 0 &&
            ((value & (BIT(22) | BIT(21) | 0xf00)) ||
             (value & 0x3000) != 0x1000)) {
            goto unsupported;
        }
        if (index == 6 && (value & BIT(6))) {
            unsigned mask = (value >> 8) & 0xf;

            if ((mask != 0xc && mask != 0xe && mask != 0xf) ||
                (value & 0xf & ~mask)) {
                goto unsupported;
            }
        }
        s->gpmc_cs0[index] = value & rcc_gpmc_masks[index];
        if (index == 5) {
            s->gpmc_cs0[index] |= BIT(31);
        }
        if (index == 6) {
            rcc_gpmc_map(s);
        }
        return MEMTX_OK;
    }
    switch (offset) {
    case 0x10:
        if (!(value & ~0x1bU) && (value & 0x18) != 0x18) {
            if (value & 2) {
                rcc_gpmc_reset(s, false);
            } else {
                s->gpmc_sysconfig = value;
            }
            return MEMTX_OK;
        }
        break;
    case 0x18:
        if (!(value & ~0x303U)) {
            return MEMTX_OK;
        }
        break;
    case 0x1c:
        if (!(value & ~0x303U)) {
            s->gpmc_irqenable = value;
            return MEMTX_OK;
        }
        break;
    case 0x40:
        if (!(value & ~0x1ff0U)) {
            s->gpmc_timeout = value;
            return MEMTX_OK;
        }
        break;
    case 0x50:
        if (!(value & ~0x300U)) {
            s->gpmc_config = value;
            return MEMTX_OK;
        }
        break;
    }
unsupported:
    qemu_log_mask(LOG_UNIMP,
                  "mh2p-rcc: unsupported GPMC write at 0x%" HWADDR_PRIx
                  " size=%u value=0x%" PRIx64 "\n", offset, size, value);
    return MEMTX_ERROR;
}

static const MemoryRegionOps rcc_gpmc_ops = {
    .read_with_attrs = rcc_gpmc_read,
    .write_with_attrs = rcc_gpmc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static bool rcc_ipu1_clock_active(MH2PRCCMachineState *s)
{
    return s->ipu1_clock_running;
}

static void rcc_ipu1_control(MH2PRCCMachineState *s)
{
    bool clock = (s->ipu1_clkctrl & 1) &&
                 (s->ipu1_clkstctrl >= 2 ||
                  (!s->ipu1_clkstctrl && s->ipu1_clock_running));
    uint32_t requested = s->ipu1_reset_control |
                         ((s->ipu1_reset_control & 4) ? 3 : 0);
    uint32_t asserted = clock ? requested :
                        s->ipu1_reset_asserted | requested;

    s->ipu1_clock_running = clock;
    s->ipu1_reset_status |= s->ipu1_reset_asserted & ~asserted;
    s->ipu1_reset_asserted = asserted;
    mh2p_rcc_ipu1_control(s->ipu1, asserted, clock);
}

static void rcc_ipu2_control(MH2PRCCMachineState *s)
{
    bool clock = (s->ipu2_clkctrl & 1) &&
                 (s->ipu2_clkstctrl >= 2 ||
                  (!s->ipu2_clkstctrl && s->ipu2_clock_running));
    uint32_t requested = s->ipu2_reset_control |
                         ((s->ipu2_reset_control & 4) ? 3 : 0);
    uint32_t asserted = clock ? requested :
                        s->ipu2_reset_asserted | requested;

    s->ipu2_clock_running = clock;
    s->ipu2_reset_status |= s->ipu2_reset_asserted & ~asserted;
    s->ipu2_reset_asserted = asserted;
    mh2p_rcc_ipu1_control(s->ipu2, asserted, clock);
}

static uint32_t rcc_ipu_per_clock_status(MH2PRCCMachineState *s)
{
    bool uart6 = false, i2c5 = false, timers = false;
    uint32_t status = s->ipu_per_clkstctrl;

    for (unsigned i = 0; i < ARRAY_SIZE(rcc_peripheral_clkctrl); i++) {
        if (rcc_peripheral_clkctrl[i] == RCC_UART6_CLKCTRL) {
            uart6 = s->peripheral_clkctrl[i] & 2;
        } else if (rcc_peripheral_clkctrl[i] == 0x4a005578) {
            i2c5 = s->peripheral_clkctrl[i] & 2;
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(s->gp_timer); i++) {
        RCCTimer *timer = &s->gp_timer[i];

        if (timer->number >= 5 && timer->number <= 8 &&
            (timer->clock_control & 2)) {
            status |= BIT(timer->number + 4);
            timers = true;
        }
    }
    if (uart6 || i2c5 || timers || s->ipu_per_clkstctrl == 2) {
        s->ipu_per_clock_active = true;
    } else if (s->ipu_per_clkstctrl != 0) {
        s->ipu_per_clock_active = false;
    }
    if (s->ipu_per_clock_active) {
        status |= BIT(8);
    }
    if (uart6 || i2c5) {
        status |= BIT(13);
    }
    if (uart6) {
        status |= BIT(14);
    }
    return status;
}

static unsigned rcc_crossbar_destination(unsigned selector)
{
    if (selector < 2) {
        return selector ? 7 : 4;
    }
    return selector + (selector < 125 ? 6 : 8);
}

static void rcc_crossbar_update(RCCCrossbar *s)
{
    if (!s->mpu && !s->defaults) {
        return;
    }
    for (unsigned i = 0; i < s->selector_count; i++) {
        qemu_set_irq(s->outputs[i],
                     s->selection[i] && s->levels[s->selection[i]]);
        qemu_set_irq(s->secondary_outputs[i],
                     s->selection[i] && s->levels[s->selection[i]]);
    }
}

static void rcc_gmac_dma_reset(MH2PRCCMachineState *s);

static void rcc_gmac_ale_reset(MH2PRCCMachineState *s)
{
    s->gmac_ale_control = s->gmac_ale_index = 0;
    memset(s->gmac_ale_words, 0, sizeof(s->gmac_ale_words));
    memset(s->gmac_ale_table, 0, sizeof(s->gmac_ale_table));
    memset(s->gmac_ale_port, 0, sizeof(s->gmac_ale_port));
}

static MemTxResult rcc_gmac_reset_read(void *opaque, hwaddr offset,
                                     uint64_t *value, unsigned size,
                                     MemTxAttrs attrs)
{
    *value = 0;
    return MEMTX_OK;
}

static MemTxResult rcc_gmac_reset_write(void *opaque, hwaddr offset,
                                      uint64_t value, unsigned size,
                                      MemTxAttrs attrs)
{
    MH2PRCCMachineState *s = opaque;

    if (value & ~1ULL) {
        qemu_log_mask(LOG_UNIMP, "mh2p-rcc: reserved GMAC reset bits "
                      "value=0x%" PRIx64 "\n", value);
        return MEMTX_ERROR;
    }
    if (s && (value & 1)) {
        rcc_gmac_ale_reset(s);
        for (unsigned i = 0; i < ARRAY_SIZE(s->gmac_config); i++) {
            if (rcc_gmac_config[i].address < 0x48485200) {
                s->gmac_config[i].value = rcc_gmac_config[i].reset;
            }
        }
    }
    /* No GMAC datapath exists yet; reset has no pending work to drain. */
    return MEMTX_OK;
}

static const MemoryRegionOps rcc_gmac_reset_ops = {
    .read_with_attrs = rcc_gmac_reset_read,
    .write_with_attrs = rcc_gmac_reset_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static MemTxResult rcc_gmac_dma_reset_write(void *opaque, hwaddr offset,
                                          uint64_t value, unsigned size,
                                          MemTxAttrs attrs)
{
    MemTxResult result = rcc_gmac_reset_write(NULL, offset, value, size, attrs);

    if (result == MEMTX_OK && (value & 1)) {
        rcc_gmac_dma_reset(opaque);
    }
    return result;
}

static const MemoryRegionOps rcc_gmac_dma_reset_ops = {
    .read_with_attrs = rcc_gmac_reset_read,
    .write_with_attrs = rcc_gmac_dma_reset_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static MemTxResult rcc_gmac_wrapper_reset_write(void *opaque, hwaddr offset,
                                              uint64_t value, unsigned size,
                                              MemTxAttrs attrs)
{
    MH2PRCCMachineState *s = opaque;
    MemTxResult result = rcc_gmac_reset_write(NULL, offset, value, size, attrs);

    if (result == MEMTX_OK && (value & 1)) {
        for (unsigned i = 0; i < ARRAY_SIZE(s->gmac_config); i++) {
            if (rcc_gmac_config[i].address >= 0x48485200) {
                s->gmac_config[i].value = rcc_gmac_config[i].reset;
            }
        }
    }
    return result;
}

static const MemoryRegionOps rcc_gmac_wrapper_reset_ops = {
    .read_with_attrs = rcc_gmac_reset_read,
    .write_with_attrs = rcc_gmac_wrapper_reset_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static uint64_t rcc_gmac_config_read(void *opaque, hwaddr offset, unsigned size)
{
    RCCGMACConfig *r = opaque;

    return r->value;
}

static void rcc_gmac_config_write(void *opaque, hwaddr offset,
                                 uint64_t value, unsigned size)
{
    RCCGMACConfig *r = opaque;

    r->value = value & r->mask;
}

static const MemoryRegionOps rcc_gmac_config_ops = {
    .read = rcc_gmac_config_read,
    .write = rcc_gmac_config_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void rcc_gmac_dma_reset(MH2PRCCMachineState *s)
{
    memset(s->gmac_dma_control, 0, sizeof(s->gmac_dma_control));
    memset(s->gmac_dma_mask, 0, sizeof(s->gmac_dma_mask));
    memset(s->gmac_dma_hdp, 0, sizeof(s->gmac_dma_hdp));
    s->gmac_dma_rx_offset = 0;
}

static MemTxResult rcc_gmac_dma_read(void *opaque, hwaddr offset,
                                   uint64_t *value, unsigned size,
                                   MemTxAttrs attrs)
{
    MH2PRCCMachineState *s = opaque;

    switch (offset) {
    case 0x004:
    case 0x014:
        *value = s->gmac_dma_control[offset == 0x014];
        return MEMTX_OK;
    case 0x028:
        *value = s->gmac_dma_rx_offset;
        return MEMTX_OK;
    case 0x088:
    case 0x08c:
        *value = s->gmac_dma_mask[0];
        return MEMTX_OK;
    case 0x0a8:
    case 0x0ac:
        *value = s->gmac_dma_mask[1];
        return MEMTX_OK;
    case 0x0b8:
    case 0x0bc:
        *value = s->gmac_dma_mask[2];
        return MEMTX_OK;
    case 0x080:
    case 0x084:
    case 0x090:
    case 0x0a0:
    case 0x0a4:
    case 0x0b0:
    case 0x0b4:
        *value = 0;
        return MEMTX_OK;
    }
    if (offset >= 0x200 && offset < 0x240) {
        *value = s->gmac_dma_hdp[(offset - 0x200) / 0x20][(offset / 4) & 7];
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP, "mh2p-rcc: unsupported GMAC DMA read "
                  "offset=0x%" HWADDR_PRIx "\n", offset);
    return MEMTX_ERROR;
}

static MemTxResult rcc_gmac_dma_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size,
                                    MemTxAttrs attrs)
{
    MH2PRCCMachineState *s = opaque;

    switch (offset) {
    case 0x004:
    case 0x014:
        s->gmac_dma_control[offset == 0x014] = value & 1;
        return MEMTX_OK;
    case 0x028:
        s->gmac_dma_rx_offset = value & 0xffff;
        return MEMTX_OK;
    case 0x088:
    case 0x0a8:
    case 0x0b8:
        s->gmac_dma_mask[offset == 0x088 ? 0 : offset == 0x0a8 ? 1 : 2] |=
            value & (offset == 0x0b8 ? 3 : 0xff);
        return MEMTX_OK;
    case 0x08c:
    case 0x0ac:
    case 0x0bc:
        s->gmac_dma_mask[offset == 0x08c ? 0 : offset == 0x0ac ? 1 : 2] &=
            ~value;
        return MEMTX_OK;
    }
    if (offset >= 0x200 && offset < 0x240 && !(value & 3)) {
        unsigned direction = (offset - 0x200) / 0x20;

        if (direction == 1 || value == 0) {
            s->gmac_dma_hdp[direction][(offset / 4) & 7] = value;
            return MEMTX_OK;
        }
    }
    qemu_log_mask(LOG_UNIMP, "mh2p-rcc: unsupported GMAC DMA write "
                  "offset=0x%" HWADDR_PRIx " value=0x%" PRIx64 "\n",
                  offset, value);
    return MEMTX_ERROR;
}

static const MemoryRegionOps rcc_gmac_dma_ops = {
    .read_with_attrs = rcc_gmac_dma_read,
    .write_with_attrs = rcc_gmac_dma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void rcc_gmac_mdio_schedule(MH2PRCCMachineState *s)
{
    uint32_t control = s->gmac_mdio_control;

    if (s->gmac_mdio_active >= 0 || !(control & BIT(30)) ||
        !(control & 0xffff)) {
        return;
    }
    for (unsigned i = 0; i < 2; i++) {
        unsigned channel = (s->gmac_mdio_next + i) % 2;

        if (s->gmac_mdio_user[channel] & BIT(31)) {
            unsigned clocks = control & BIT(20) ? 32 : 64;

            s->gmac_mdio_active = channel;
            /* MAIN_CLK is 125 MHz; each divider input cycle is 8 ns. */
            timer_mod(s->gmac_mdio_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      (uint64_t)clocks * ((control & 0xffff) + 1) * 8);
            return;
        }
    }
}

static void rcc_gmac_mdio_complete(void *opaque)
{
    MH2PRCCMachineState *s = opaque;
    unsigned channel = s->gmac_mdio_active;
    uint32_t command = s->gmac_mdio_user[channel];

    /* No external PHY is attached: the bus is pulled high, with no ACK. */
    s->gmac_mdio_user[channel] = command & 0x43ffffff;
    if (!(command & BIT(30))) {
        s->gmac_mdio_user[channel] |= 0xffff;
    }
    s->gmac_mdio_raw |= BIT(channel);
    s->gmac_mdio_active = -1;
    s->gmac_mdio_next = channel ^ 1;
    rcc_gmac_mdio_schedule(s);
}

static MemTxResult rcc_gmac_mdio_read(void *opaque, hwaddr offset,
                                    uint64_t *value, unsigned size,
                                    MemTxAttrs attrs)
{
    MH2PRCCMachineState *s = opaque;

    switch (offset) {
    case 0x04:
        *value = s->gmac_mdio_control | BIT(24) |
                 (!(s->gmac_mdio_control & BIT(30)) &&
                  s->gmac_mdio_active < 0 ? BIT(31) : 0);
        return MEMTX_OK;
    case 0x08:
    case 0x0c:
    case 0x10:
    case 0x14:
    case 0x24:
    case 0x28:
    case 0x2c:
        *value = 0;
        return MEMTX_OK;
    case 0x20:
        *value = s->gmac_mdio_raw;
        return MEMTX_OK;
    case 0x80:
    case 0x88:
        *value = s->gmac_mdio_user[(offset - 0x80) / 8];
        return MEMTX_OK;
    case 0x84:
    case 0x8c:
        *value = s->gmac_mdio_phy[(offset - 0x84) / 8];
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP, "mh2p-rcc: unsupported GMAC MDIO read "
                  "offset=0x%" HWADDR_PRIx "\n", offset);
    return MEMTX_ERROR;
}

static MemTxResult rcc_gmac_mdio_write(void *opaque, hwaddr offset,
                                     uint64_t value, unsigned size,
                                     MemTxAttrs attrs)
{
    MH2PRCCMachineState *s = opaque;

    switch (offset) {
    case 0x04:
        if (value & BIT(17)) {
            break;
        }
        s->gmac_mdio_control = value & 0x4014ffff;
        rcc_gmac_mdio_schedule(s);
        return MEMTX_OK;
    case 0x08:
    case 0x10:
    case 0x14:
    case 0x2c:
        return MEMTX_OK;
    case 0x20:
    case 0x24:
        s->gmac_mdio_raw &= ~value;
        return MEMTX_OK;
    case 0x28:
        if (!(value & 3)) {
            return MEMTX_OK;
        }
        break;
    case 0x80:
    case 0x88: {
        unsigned channel = (offset - 0x80) / 8;

        if (!(s->gmac_mdio_user[channel] & BIT(31))) {
            uint32_t mask = s->gmac_mdio_control & BIT(30) ?
                            0xe3ffffff : 0x63ffffff;

            s->gmac_mdio_user[channel] = value & mask;
            rcc_gmac_mdio_schedule(s);
        }
        return MEMTX_OK;
    }
    case 0x84:
    case 0x8c:
        if (value & 0xc0) {
            break;
        }
        s->gmac_mdio_phy[(offset - 0x84) / 8] = value & 0x1f;
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP, "mh2p-rcc: unsupported GMAC MDIO write "
                  "offset=0x%" HWADDR_PRIx " value=0x%" PRIx64 "\n",
                  offset, value);
    return MEMTX_ERROR;
}

static const MemoryRegionOps rcc_gmac_mdio_ops = {
    .read_with_attrs = rcc_gmac_mdio_read,
    .write_with_attrs = rcc_gmac_mdio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static MemTxResult rcc_gmac_ale_read(void *opaque, hwaddr offset,
                                   uint64_t *value, unsigned size,
                                   MemTxAttrs attrs)
{
    MH2PRCCMachineState *s = opaque;

    switch (offset) {
    case 0x08:
        *value = s->gmac_ale_control;
        return MEMTX_OK;
    case 0x20:
        *value = s->gmac_ale_index;
        return MEMTX_OK;
    case 0x34:
    case 0x38:
    case 0x3c:
        *value = s->gmac_ale_words[(0x3c - offset) / 4];
        return MEMTX_OK;
    case 0x40:
    case 0x44:
    case 0x48:
        *value = s->gmac_ale_port[(offset - 0x40) / 4];
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP, "mh2p-rcc: unsupported GMAC ALE read "
                  "offset=0x%" HWADDR_PRIx "\n", offset);
    return MEMTX_ERROR;
}

static MemTxResult rcc_gmac_ale_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size,
                                    MemTxAttrs attrs)
{
    MH2PRCCMachineState *s = opaque;

    switch (offset) {
    case 0x08:
        if (value & (1U << 29)) {
            break;
        }
        if (value & (1U << 30)) {
            memset(s->gmac_ale_table, 0, sizeof(s->gmac_ale_table));
        }
        s->gmac_ale_control = value & 0x800001ff;
        return MEMTX_OK;
    case 0x20:
        s->gmac_ale_index = value & 1023;
        if (value & (1U << 31)) {
            memcpy(s->gmac_ale_table[s->gmac_ale_index], s->gmac_ale_words,
                   sizeof(s->gmac_ale_words));
        } else {
            memcpy(s->gmac_ale_words, s->gmac_ale_table[s->gmac_ale_index],
                   sizeof(s->gmac_ale_words));
        }
        return MEMTX_OK;
    case 0x34:
    case 0x38:
    case 0x3c:
        s->gmac_ale_words[(0x3c - offset) / 4] =
            value & (offset == 0x34 ? 0xff : UINT32_MAX);
        return MEMTX_OK;
    case 0x40:
    case 0x44:
    case 0x48:
        s->gmac_ale_port[(offset - 0x40) / 4] = value & 0xffff003f;
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP, "mh2p-rcc: unsupported GMAC ALE write "
                  "offset=0x%" HWADDR_PRIx " value=0x%" PRIx64 "\n",
                  offset, value);
    return MEMTX_ERROR;
}

static const MemoryRegionOps rcc_gmac_ale_ops = {
    .read_with_attrs = rcc_gmac_ale_read,
    .write_with_attrs = rcc_gmac_ale_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void rcc_crossbar_input(void *opaque, int irq, int level)
{
    RCCCrossbar *s = opaque;

    s->levels[irq] = level != 0;
    rcc_crossbar_update(s);
    for (RCCCrossbar *peer = s->peer; peer; peer = peer->peer) {
        peer->levels[irq] = level != 0;
        rcc_crossbar_update(peer);
    }
}

static void rcc_crossbar_reset(RCCCrossbar *s)
{
    for (unsigned i = 0; i < s->selector_count; i++) {
        unsigned destination = rcc_crossbar_destination(i);

        s->selection[i] = s->defaults ? s->defaults[i] :
                         (!s->mpu || destination < 133 ? i + 1 : 0);
        if (s->mpu && destination == 100) {
            s->selection[i] = 395;
        }
    }
    rcc_crossbar_update(s);
}

static MemTxResult rcc_crossbar_read(void *opaque, hwaddr offset,
                                    uint64_t *value, unsigned size,
                                    MemTxAttrs attrs)
{
    RCCCrossbar *s = opaque;

    *value = s->selection[offset / 2];
    if (size == 4) {
        *value |= (uint32_t)s->selection[offset / 2 + 1] << 16;
    }
    return MEMTX_OK;
}

static MemTxResult rcc_crossbar_write(void *opaque, hwaddr offset,
                                     uint64_t value, unsigned size,
                                     MemTxAttrs attrs)
{
    RCCCrossbar *s = opaque;
    uint32_t mask = size == 4 ? 0x01ff01ff : 0x01ff;

    if (s->mmr_lock && *s->mmr_lock != 0xf757fdc0) {
        qemu_log_mask(LOG_GUEST_ERROR, "mh2p-rcc: locked crossbar write\n");
        return MEMTX_ERROR;
    }
    if (value & ~mask) {
        qemu_log_mask(LOG_UNIMP, "mh2p-rcc: reserved crossbar bits at 0x%"
                      HWADDR_PRIx " size=%u value=0x%" PRIx64 "\n",
                      offset, size, value);
        return MEMTX_ERROR;
    }
    s->selection[offset / 2] = value & 0x1ff;
    if (size == 4) {
        s->selection[offset / 2 + 1] = value >> 16;
    }
    rcc_crossbar_update(s);
    return MEMTX_OK;
}

static const MemoryRegionOps rcc_crossbar_ops = {
    .read_with_attrs = rcc_crossbar_read,
    .write_with_attrs = rcc_crossbar_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 2, .max_access_size = 4 },
    .impl = { .min_access_size = 2, .max_access_size = 4 },
};

static uint64_t rcc_l2actlr_read(CPUARMState *env, const ARMCPRegInfo *ri)
{
    MH2PRCCMachineState *s = (MH2PRCCMachineState *)qdev_get_machine();

    return s->l2actlr;
}

static void rcc_l2actlr_write(CPUARMState *env, const ARMCPRegInfo *ri,
                             uint64_t value)
{
    MH2PRCCMachineState *s = (MH2PRCCMachineState *)qdev_get_machine();

    s->l2actlr = value;
}

static CPAccessResult rcc_l2actlr_access(CPUARMState *env,
                                       const ARMCPRegInfo *ri, bool isread)
{
    return isread || arm_is_secure(env) ? CP_ACCESS_OK : CP_ACCESS_UNDEFINED;
}

static const ARMCPRegInfo rcc_cpregs[] = {
    /*
     * TCG has no cache/bus timing, but retain the secure cache-policy write.
     * This board-specific override replaces the CPU's generic C15 RAZ/WI.
     */
    { .name = "RCC_L2ACTLR", .cp = 15, .crn = 15, .crm = 0, .opc1 = 1,
      .opc2 = 0, .access = PL1_RW, .type = ARM_CP_NO_RAW | ARM_CP_OVERRIDE,
      .accessfn = rcc_l2actlr_access, .readfn = rcc_l2actlr_read,
      .writefn = rcc_l2actlr_write },
};

static MemTxResult rcc_iodelay_read(void *opaque, hwaddr offset,
                                   uint64_t *value, unsigned size,
                                   MemTxAttrs attrs)
{
    MH2PRCCMachineState *s = opaque;

    switch (offset) {
    case 0x14:
        *value = 0x2ef;
        return MEMTX_OK;
    case 0x18:
        /* Explicit handoff fixture: CPDE = 5 * 16192 / 88 = 920 ps. */
        *value = (0x2ef << 16) | 16192;
        return MEMTX_OK;
    case 0x1c:
        /* FPDE = 5 * 3168 / 264 = 60 ps; not measured RCC silicon. */
        *value = (0x2ef << 16) | 3168;
        return MEMTX_OK;
    case 0x2c:
        *value = s->iodelay_lock;
        return MEMTX_OK;
    }
    if (offset >= 0x30 && offset <= 0xd1c) {
        *value = s->pad_delay[(offset - 0x30) / 4];
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP, "mh2p-rcc: unsupported IODELAY read 0x%"
                  HWADDR_PRIx "\n", offset);
    *value = 0;
    return MEMTX_ERROR;
}

static MemTxResult rcc_iodelay_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size,
                                    MemTxAttrs attrs)
{
    MH2PRCCMachineState *s = opaque;

    if (offset == 0x2c && (value == 0xaaaa || value == 0xaaab)) {
        s->iodelay_lock = value;
        return MEMTX_OK;
    }
    if (offset >= 0x30 && offset <= 0xd1c &&
        s->iodelay_lock == 0xaaaa &&
        !(s->pad_delay[(offset - 0x30) / 4] & BIT(10)) &&
        (value & 0x3f000) == 0x29000 && !(value & ~0x3f7ff)) {
        s->pad_delay[(offset - 0x30) / 4] = value;
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP, "mh2p-rcc: unsupported/locked IODELAY write 0x%"
                  HWADDR_PRIx " = 0x%" PRIx64 "\n", offset, value);
    return MEMTX_ERROR;
}

static const MemoryRegionOps rcc_iodelay_ops = {
    .read_with_attrs = rcc_iodelay_read,
    .write_with_attrs = rcc_iodelay_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void rcc_edma_update_irq(MH2PRCCMachineState *s)
{
    for (unsigned region = 0; region < ARRAY_SIZE(s->edma_ier); region++) {
        bool pending = (s->edma_ier[region][0] & s->edma_ipr[region][0]) ||
                       (s->edma_ier[region][1] & s->edma_ipr[region][1]);

        qemu_set_irq(s->crossbar.inputs[360 + region], pending);
    }
}

static bool rcc_edma_memory_address(MH2PRCCMachineState *s, uint32_t address,
                                    unsigned size)
{
    uint64_t end = (uint64_t)address + size;

    return (address >= RCC_RAM_BASE &&
            end <= RCC_RAM_BASE + MACHINE(s)->ram_size) ||
           (address >= 0x20000000 && end <= 0x30000000);
}

static MemTxResult rcc_edma_qdma_transfer(MH2PRCCMachineState *s,
                                         unsigned channel, unsigned slot)
{
    uint32_t *p = s->edma_param[slot];
    unsigned transferred = 0;
    uint32_t epoch = s->edma_epoch;

    for (unsigned descriptor = 0; descriptor < 512; descriptor++) {
        unsigned acnt = p[2] & 0xffff, bcnt = p[2] >> 16;
        unsigned tcc = (p[0] >> 12) & 63, link = p[5] & 0xffff;
        uint32_t source = p[1], target = p[3];
        int16_t source_step = p[4], target_step = p[4] >> 16;
        g_autofree uint8_t *bytes = NULL;

        /* Bound native one-frame AB-sync packet chains, not general EDMA. */
        if ((p[0] & ~0x0013f00cU) || !(p[0] & BIT(2)) ||
            p[7] != 1 || !acnt || !bcnt ||
            (uint64_t)acnt * bcnt + transferred > 64 * KiB ||
            (!(p[0] & BIT(3)) && link != 0xffff &&
             (link < 0x4000 || link >= 0x8000 || (link & 31)))) {
            goto unsupported;
        }
        for (unsigned i = 0; i < bcnt; i++) {
            if (!rcc_edma_memory_address(s, source, acnt) ||
                !rcc_edma_memory_address(s, target, acnt)) {
                goto unsupported;
            }
            source += source_step;
            target += target_step;
        }
        source = p[1];
        target = p[3];
        bytes = g_malloc(acnt);
        for (unsigned i = 0; i < bcnt; i++) {
            if (address_space_read(&address_space_memory, source,
                                   MEMTXATTRS_UNSPECIFIED, bytes,
                                   acnt) != MEMTX_OK ||
                epoch != s->edma_epoch ||
                address_space_write(&address_space_memory, target,
                                    MEMTXATTRS_UNSPECIFIED, bytes,
                                    acnt) != MEMTX_OK) {
                goto unsupported;
            }
            if (epoch != s->edma_epoch) {
                goto unsupported;
            }
            source += source_step;
            target += target_step;
        }
        transferred += acnt * bcnt;
        trace_mh2p_rcc_qdma(channel, p[1], p[3], acnt * bcnt, tcc);
        if (p[0] & BIT(20)) {
            unsigned bank = tcc / 32;
            uint32_t bit = BIT(tcc % 32);

            s->edma_ipr[0][bank] |= bit;
            for (unsigned region = 1;
                 region < ARRAY_SIZE(s->edma_ier); region++) {
                if (s->edma_access[(region - 1) * 2 + bank] & bit) {
                    s->edma_ipr[region][bank] |= bit;
                }
            }
            rcc_edma_update_irq(s);
        }
        if (p[0] & BIT(3)) {
            return MEMTX_OK;
        }
        if (link == 0xffff) {
            memset(p, 0, 32);
            p[5] = 0xffff;
            return MEMTX_OK;
        }
        memmove(p, s->edma_param[(link - 0x4000) / 32], 32);
    }
unsupported:
    if (epoch != s->edma_epoch) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "mh2p-rcc: QDMA reset during transfer\n");
        return MEMTX_ERROR;
    }
    s->edma_qemr |= BIT(channel);
    qemu_log_mask(LOG_UNIMP, "mh2p-rcc: unsupported/failed QDMA%u "
                  "slot %u opt=0x%x src=0x%x dst=0x%x count=0x%x\n",
                  channel, slot, p[0], p[1], p[3], p[2]);
    return MEMTX_ERROR;
}

static bool rcc_edma_idle_register(hwaddr offset)
{
    switch (offset) {
    case 0x300: case 0x304: case 0x310: case 0x318: case 0x640:
    case 0x1000: case 0x1004: /* ER: no input events or software requests. */
    case 0x1020: case 0x1024: /* EER: channels remain disabled. */
    case 0x1038: case 0x103c: /* SER: no secondary events. */
    case 0x1050: case 0x1054: /* IER */
    case 0x1068: case 0x106c: /* IPR: no fabricated completions. */
    case 0x1080: case 0x1084: case 0x1090:
        return true;
    default:
        return false;
    }
}

static MemTxResult rcc_edma_read(void *opaque, hwaddr offset,
                                uint64_t *value, unsigned size,
                                MemTxAttrs attrs)
{
    MH2PRCCMachineState *s = opaque;
    unsigned region = 0;

    if (offset >= 0x100 && offset < 0x200) {
        *value = s->edma_map[(offset - 0x100) / 4];
        return MEMTX_OK;
    }
    if (offset >= 0x200 && offset < 0x220) {
        *value = s->edma_qmap[(offset - 0x200) / 4];
        return MEMTX_OK;
    }
    if (offset >= 0x240 && offset <= 0x260) {
        *value = s->edma_queue[(offset - 0x240) / 4];
        return MEMTX_OK;
    }
    if (offset >= 0x340 && offset < 0x3a0) {
        *value = s->edma_access[(offset - 0x340) / 4];
        return MEMTX_OK;
    }
    if (offset >= 0x4000 && offset < 0x8000) {
        *value = s->edma_param[(offset - 0x4000) / 32][(offset / 4) & 7];
        return MEMTX_OK;
    }
    if (offset >= 0x2000 && offset < 0x2800) {
        region = 1 + (offset - 0x2000) / 0x100;
        offset = 0x1000 + (offset & 0xff);
    }
    switch (offset) {
    case 0x310:
        *value = s->edma_qemr;
        return MEMTX_OK;
    case 0x1050: case 0x1054:
        *value = s->edma_ier[region][(offset - 0x1050) / 4];
        return MEMTX_OK;
    case 0x1068: case 0x106c:
        *value = s->edma_ipr[region][(offset - 0x1068) / 4];
        return MEMTX_OK;
    case 0x1084:
        *value = s->edma_qeer[region];
        return MEMTX_OK;
    }
    if (rcc_edma_idle_register(offset)) {
        *value = 0;
        return MEMTX_OK;
    }
    if (offset >= 0x1098 && offset < 0x2000) {
        qemu_log_mask(LOG_GUEST_ERROR, "mh2p-rcc: reserved EDMA read 0x%"
                      HWADDR_PRIx " (RAZ)\n", offset);
        *value = 0;
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP, "mh2p-rcc: unsupported EDMA read 0x%"
                  HWADDR_PRIx "\n", offset);
    *value = 0;
    return MEMTX_ERROR;
}

static MemTxResult rcc_edma_write(void *opaque, hwaddr offset,
                                 uint64_t value, unsigned size,
                                 MemTxAttrs attrs)
{
    MH2PRCCMachineState *s = opaque;
    unsigned region = 0;

    if (offset >= 0x100 && offset < 0x200 && !(value & ~0x3fe0)) {
        s->edma_map[(offset - 0x100) / 4] = value;
        return MEMTX_OK;
    }
    if (offset >= 0x200 && offset < 0x220 && !(value & ~0x3ffc)) {
        s->edma_qmap[(offset - 0x200) / 4] = value;
        return MEMTX_OK;
    }
    if (offset >= 0x240 && offset <= 0x260 && !(value & 0xcccccccc)) {
        s->edma_queue[(offset - 0x240) / 4] = value;
        return MEMTX_OK;
    }
    if (offset >= 0x340 && offset < 0x3a0 &&
        (offset < 0x380 || !(value & ~0xffU))) {
        s->edma_access[(offset - 0x340) / 4] = value;
        return MEMTX_OK;
    }
    if (offset >= 0x4000 && offset < 0x8000) {
        unsigned slot = (offset - 0x4000) / 32;
        unsigned word = (offset / 4) & 7;
        uint32_t enabled = 0;
        MemTxResult result = MEMTX_OK;

        s->edma_param[slot][word] = value;
        for (unsigned i = 0; i < ARRAY_SIZE(s->edma_qeer); i++) {
            enabled |= s->edma_qeer[i];
        }
        for (unsigned i = 0; i < ARRAY_SIZE(s->edma_qmap); i++) {
            if ((enabled & BIT(i)) &&
                (s->edma_qmap[i] >> 5) == slot &&
                ((s->edma_qmap[i] >> 2) & 7) == word) {
                result |= rcc_edma_qdma_transfer(s, i, slot);
            }
        }
        return result;
    }
    if (offset >= 0x2000 && offset < 0x2800) {
        region = 1 + (offset - 0x2000) / 0x100;
        offset = 0x1000 + (offset & 0xff);
    }
    switch (offset) {
    case 0x314:
        s->edma_qemr &= ~value;
        return MEMTX_OK;
    case 0x1058: case 0x105c:
        s->edma_ier[region][(offset - 0x1058) / 4] &= ~value;
        rcc_edma_update_irq(s);
        return MEMTX_OK;
    case 0x1060: case 0x1064:
        s->edma_ier[region][(offset - 0x1060) / 4] |= value;
        rcc_edma_update_irq(s);
        return MEMTX_OK;
    case 0x1070: case 0x1074:
        s->edma_ipr[region][(offset - 0x1070) / 4] &= ~value;
        rcc_edma_update_irq(s);
        return MEMTX_OK;
    case 0x1078:
        rcc_edma_update_irq(s);
        return MEMTX_OK;
    case 0x1088:
        s->edma_qeer[region] &= ~value;
        return MEMTX_OK;
    case 0x108c:
        if (!(value & ~0xffU)) {
            s->edma_qeer[region] |= value;
            return MEMTX_OK;
        }
        break;
    case 0x308: case 0x30c: case 0x31c:
    case 0x1008: case 0x100c:
    case 0x1028: case 0x102c:
    case 0x1040: case 0x1044:
    case 0x1094:
        /* W1C event/error/enable state, with no enabled DMA transfers. */
        return MEMTX_OK;
    }
    if (offset >= 0x1098 && offset < 0x2000) {
        /*
         * Reserved gap before shadow regions, not aliases for EMCR/QEMCR.
         * Preserve diagnostics without inventing error-clear side effects.
         */
        qemu_log_mask(LOG_GUEST_ERROR, "mh2p-rcc: reserved EDMA write 0x%"
                      HWADDR_PRIx " = 0x%" PRIx64 " (ignored)\n",
                      offset, value);
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP, "mh2p-rcc: unsupported EDMA write 0x%"
                  HWADDR_PRIx " = 0x%" PRIx64 "\n", offset, value);
    return MEMTX_ERROR;
}

static const MemoryRegionOps rcc_edma_ops = {
    .read_with_attrs = rcc_edma_read,
    .write_with_attrs = rcc_edma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static MemTxResult rcc_dma_read(void *opaque, hwaddr offset,
                               uint64_t *value, unsigned size, MemTxAttrs attrs)
{
    MH2PRCCMachineState *s = opaque;

    switch (offset) {
    case 0x08 ... 0x14: /* No DMA transfers have been enabled. */
        *value = 0;
        return MEMTX_OK;
    case 0x18 ... 0x24:
        *value = s->dma_irqenable[(offset - 0x18) / 4];
        return MEMTX_OK;
    case 0x2c:
        *value = s->dma_sysconfig;
        return MEMTX_OK;
    case 0x78:
        *value = s->dma_gcr;
        return MEMTX_OK;
    }
    if (offset >= 0x80 && offset < 0xc80) {
        unsigned channel = (offset - 0x80) / 0x60;
        unsigned reg = (offset - 0x80) % 0x60;

        if (reg >= 0x24 && reg <= 0x30) {
            *value = s->dma_index[channel][(reg - 0x24) / 4];
            return MEMTX_OK;
        }
        if (reg == 0 || reg == 8 || reg == 12) {
            *value = s->dma_channel[channel][reg ? reg / 4 - 1 : 0];
            return MEMTX_OK;
        }
    }
    qemu_log_mask(LOG_UNIMP, "mh2p-rcc: unsupported SDMA read 0x%"
                  HWADDR_PRIx "\n", offset);
    *value = 0;
    return MEMTX_ERROR;
}

static MemTxResult rcc_dma_write(void *opaque, hwaddr offset,
                                uint64_t value, unsigned size, MemTxAttrs attrs)
{
    MH2PRCCMachineState *s = opaque;

    switch (offset) {
    case 0x08 ... 0x14: /* W1C IRQSTATUS, with no pending transfers. */
        return MEMTX_OK;
    case 0x18 ... 0x24:
        s->dma_irqenable[(offset - 0x18) / 4] = value;
        return MEMTX_OK;
    case 0x2c:
        if (value & 2) {
            break; /* Soft reset is not yet implemented. */
        }
        s->dma_sysconfig = value;
        return MEMTX_OK;
    case 0x78:
        s->dma_gcr = value;
        return MEMTX_OK;
    }
    if (offset >= 0x80 && offset < 0xc80) {
        unsigned channel = (offset - 0x80) / 0x60;
        unsigned reg = (offset - 0x80) % 0x60;

        if (reg >= 0x24 && reg <= 0x30) {
            unsigned index = (reg - 0x24) / 4;

            if (!(index & 1) && (value & ~UINT16_MAX)) {
                goto unsupported;
            }
            s->dma_index[channel][index] = value;
            return MEMTX_OK;
        }
        if ((reg == 0 && !(value & BIT(7))) || reg == 8 || reg == 12) {
            s->dma_channel[channel][reg ? reg / 4 - 1 : 0] = value;
            return MEMTX_OK;
        }
    }
unsupported:
    qemu_log_mask(LOG_UNIMP, "mh2p-rcc: unsupported SDMA write 0x%"
                  HWADDR_PRIx " = 0x%" PRIx64 "\n", offset, value);
    return MEMTX_ERROR;
}

static const MemoryRegionOps rcc_dma_ops = {
    .read_with_attrs = rcc_dma_read,
    .write_with_attrs = rcc_dma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static MemTxResult rcc_watchdog_read(void *opaque, hwaddr offset,
                                    uint64_t *value, unsigned size,
                                    MemTxAttrs attrs)
{
    MH2PRCCMachineState *s = opaque;

    switch (offset) {
    case 0x34: /* WWPS: register writes complete synchronously. */
        *value = 0;
        return MEMTX_OK;
    case 0x48:
        *value = s->watchdog_stop;
        return MEMTX_OK;
    default:
        qemu_log_mask(LOG_UNIMP, "mh2p-rcc: unsupported watchdog read 0x%"
                      HWADDR_PRIx "\n", offset);
        *value = 0;
        return MEMTX_ERROR;
    }
}

static MemTxResult rcc_watchdog_write(void *opaque, hwaddr offset,
                                     uint64_t value, unsigned size,
                                     MemTxAttrs attrs)
{
    MH2PRCCMachineState *s = opaque;

    if (offset == 0x48 &&
        (value == 0xaaaa ||
         (s->watchdog_stop == 0xaaaa && value == 0x5555))) {
        s->watchdog_stop = value;
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP, "mh2p-rcc: unsupported watchdog write 0x%"
                  HWADDR_PRIx " = 0x%" PRIx64 "\n", offset, value);
    return MEMTX_ERROR;
}

static const MemoryRegionOps rcc_watchdog_ops = {
    .read_with_attrs = rcc_watchdog_read,
    .write_with_attrs = rcc_watchdog_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static bool rcc_uart_tx_irq(RCCUART *s)
{
    return s->core && (s->core->serial.ier & BIT(1)) &&
           (s->core->serial.lsr & BIT(5));
}

static void rcc_uart_trace_console(RCCUART *s, const char *operation,
                                   uint64_t value)
{
    if (s->base == 0x48020000 && s->core &&
        trace_event_get_state_backends(TRACE_MH2P_RCC_UART3_STATE)) {
        SerialState *serial = &s->core->serial;

        trace_mh2p_rcc_uart3_state(operation, value, s->clock_enabled,
                                  s->mdr1, serial->ier, serial->lcr,
                                  serial->lsr, serial->iir,
                                  fifo8_num_used(&serial->recv_fifo),
                                  s->irq_level);
    }
}

static void rcc_uart_update_irq(RCCUART *s)
{
    qemu_set_irq(s->irq, s->clock_enabled &&
                 (s->irq_level || rcc_uart_tx_irq(s)));
    if (trace_event_get_state_backends(TRACE_MH2P_RCC_UART3_STATE) &&
        s->core && fifo8_num_used(&s->core->serial.recv_fifo)) {
        rcc_uart_trace_console(s, "rx-irq", 0);
    }
}

static void rcc_uart_irq(void *opaque, int irq, int level)
{
    RCCUART *s = opaque;

    s->irq_level = level != 0;
    rcc_uart_update_irq(s);
}

static void rcc_uart_set_clock(RCCUART *s, bool enabled)
{
    s->clock_enabled = enabled;
    rcc_uart_update_irq(s);
    rcc_uart_trace_console(s, "clock", enabled);
}

static MemTxResult rcc_uart_read(void *opaque, hwaddr offset,
                               uint64_t *value, unsigned size, MemTxAttrs attrs)
{
    RCCUART *s = opaque;

    if (offset == 0x54 && (size == 1 || size == 4)) {
        *value = s->sysconfig;
        return MEMTX_OK;
    }
    if (offset == 0x58 && (size == 1 || size == 4)) {
        *value = 1;
        return MEMTX_OK;
    }
    if (offset == 0x20 && (size == 1 || size == 4)) {
        *value = s->mdr1;
        return MEMTX_OK;
    }
    if (offset == 0x40 && (size == 1 || size == 4)) {
        *value = 0; /* Supplementary controls remain in normal, non-DMA mode. */
        return MEMTX_OK;
    }
    if (offset == 0x44 && (size == 1 || size == 4)) {
        *value = BIT(2) | fifo8_is_full(&s->core->serial.xmit_fifo);
        return MEMTX_OK;
    }
    if (offset < 0x20 && !(offset & 3) && (size == 1 || size == 4) &&
        (offset != 0 || (s->clock_enabled && s->mdr1 == 0) ||
         (s->core->serial.lcr & 0x80))) {
        *value = serial_io_ops.read(&s->core->serial, offset >> 2, 1);
        /*
         * SPRUIC2D Table 24-135: reading IIR does not clear the OMAP
         * transmit condition, unlike the underlying 16550 THRE latch.
         */
        if (offset == 8 && rcc_uart_tx_irq(s) &&
            ((*value & 0x3f) == 1 || (*value & 0x3f) == 0)) {
            *value = (*value & ~0x3f) | 2;
        }
        rcc_uart_update_irq(s);
        if (offset == 0 && !(s->core->serial.lcr & 0x80)) {
            rcc_uart_trace_console(s, "rhr", *value);
            if (s->base == 0x48020000 &&
                trace_event_get_state_backends(
                    TRACE_MH2P_RCC_UART3_RHR_ORIGIN)) {
                trace_mh2p_rcc_uart3_rhr_origin(
                    current_cpu ? current_cpu->cpu_index : -1,
                    current_cpu ?
                    CPU_GET_CLASS(current_cpu)->get_pc(current_cpu) : 0,
                    *value);
            }
        }
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP, "mh2p-rcc: unsupported UART read at 0x%"
                  HWADDR_PRIx " size=%u mode=%u clock=%u\n",
                  offset, size, s->mdr1, s->clock_enabled);
    *value = 0;
    return MEMTX_ERROR;
}

static MemTxResult rcc_uart_write(void *opaque, hwaddr offset,
                                uint64_t value, unsigned size, MemTxAttrs attrs)
{
    RCCUART *s = opaque;

    if (offset == 0x54 && (size == 1 || size == 4) &&
        !(value & ~0x1b)) {
        if (value & 2) {
            device_cold_reset(DEVICE(s->core));
            s->mdr1 = 7;
            s->sysconfig = 0;
        } else {
            s->sysconfig = value;
        }
        return MEMTX_OK;
    }
    if (offset == 0x40 && (size == 1 || size == 4) && value == 0) {
        return MEMTX_OK;
    }
    if (offset == 0x20 && (size == 1 || size == 4) &&
        (value == 0 || value == 7)) {
        /* Disabling an in-flight transmission needs OMAP-specific semantics. */
        if (value == 0 || (s->core->serial.lsr & 0x60) == 0x60) {
            s->mdr1 = value;
            return MEMTX_OK;
        }
    } else if (offset < 0x20 && !(offset & 3) && (size == 1 || size == 4) &&
               (offset != 0 || (s->clock_enabled && s->mdr1 == 0) ||
                (s->core->serial.lcr & 0x80))) {
        serial_io_ops.write(&s->core->serial, offset >> 2, value & 0xff, 1);
        rcc_uart_update_irq(s);
        if (offset == 4 && !(s->core->serial.lcr & 0x80)) {
            rcc_uart_trace_console(s, "ier", value);
        }
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP, "mh2p-rcc: unsupported UART write at 0x%"
                  HWADDR_PRIx " size=%u value=0x%" PRIx64 " mode=%u clock=%u\n",
                  offset, size, value, s->mdr1, s->clock_enabled);
    return MEMTX_ERROR;
}

static const MemoryRegionOps rcc_uart_ops = {
    .read_with_attrs = rcc_uart_read,
    .write_with_attrs = rcc_uart_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static MemTxResult rcc_atl_read(void *opaque, hwaddr offset,
                               uint64_t *value, unsigned size,
                               MemTxAttrs attrs)
{
    MH2PRCCMachineState *s = opaque;
    unsigned bank = (offset - 0x200) / 0x80;
    unsigned reg = ((offset - 0x200) % 0x80) / 4;

    if (s->atl_clkctrl && offset >= 0x200 && offset < 0x400 &&
        (offset & 0x7f) <= 0x1c && reg != 1 && reg != 3) {
        *value = s->atl_config[bank][reg];
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP, "mh2p-rcc: unsupported ATL read at 0x%"
                  HWADDR_PRIx " clock=%u\n", offset, s->atl_clkctrl);
    *value = 0;
    return MEMTX_ERROR;
}

static MemTxResult rcc_atl_write(void *opaque, hwaddr offset,
                                uint64_t value, unsigned size,
                                MemTxAttrs attrs)
{
    static const uint32_t masks[] = { 0x81ff, 0, 0x1f, 0, 1, 0xf, 0xf, 1 };
    MH2PRCCMachineState *s = opaque;
    unsigned bank = (offset - 0x200) / 0x80;
    unsigned reg = ((offset - 0x200) % 0x80) / 4;

    if (s->atl_clkctrl && offset >= 0x200 && offset < 0x400 &&
        (offset & 0x7f) <= 0x1c && masks[reg] && !(value & ~masks[reg])) {
        s->atl_config[bank][reg] = value;
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP, "mh2p-rcc: unsupported ATL write at 0x%"
                  HWADDR_PRIx " value=0x%" PRIx64 " clock=%u\n",
                  offset, value, s->atl_clkctrl);
    return MEMTX_ERROR;
}

static const MemoryRegionOps rcc_atl_ops = {
    .read_with_attrs = rcc_atl_read,
    .write_with_attrs = rcc_atl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static MemTxResult rcc_timer2_read(void *opaque, hwaddr offset,
                                  uint64_t *value, unsigned size,
                                  MemTxAttrs attrs)
{
    MH2PRCCMachineState *s = opaque;

    if (offset == 0x3c) {
        int64_t elapsed = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) -
                          s->timer2_epoch_ns;

        /* Bootloader fixture: SYS_CLKIN1 counter, auto-reload zero. */
        *value = (uint32_t)muldiv64(elapsed, 20000000, NANOSECONDS_PER_SECOND);
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP, "mh2p-rcc: unsupported TIMER2 read at 0x%"
                  HWADDR_PRIx "\n", offset);
    *value = 0;
    return MEMTX_ERROR;
}

static MemTxResult rcc_timer2_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size,
                                   MemTxAttrs attrs)
{
    qemu_log_mask(LOG_UNIMP, "mh2p-rcc: unsupported TIMER2 write at 0x%"
                  HWADDR_PRIx " value=0x%" PRIx64 "\n", offset, value);
    return MEMTX_ERROR;
}

static const MemoryRegionOps rcc_timer2_ops = {
    .read_with_attrs = rcc_timer2_read,
    .write_with_attrs = rcc_timer2_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static MemTxResult rcc_spinlock_read(void *opaque, hwaddr offset,
                                     uint64_t *value, unsigned size,
                                     MemTxAttrs attrs)
{
    MH2PRCCMachineState *s = opaque;

    if (offset == 0x10) {
        *value = 0x11;
        return MEMTX_OK;
    }
    if (offset == 0x14) {
        *value = 0x08000001;
        for (unsigned i = 0; i < ARRAY_SIZE(s->spinlocks); i++) {
            *value |= (uint32_t)!!s->spinlocks[i] << (8 + i);
        }
        return MEMTX_OK;
    }
    if (offset >= 0x800 && offset < 0xc00) {
        unsigned lock = (offset - 0x800) / 4;

        *value = !!(s->spinlocks[lock / 32] & BIT(lock % 32));
        s->spinlocks[lock / 32] |= BIT(lock % 32);
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP, "mh2p-rcc: unsupported spinlock read 0x%"
                  HWADDR_PRIx "\n", offset);
    *value = 0;
    return MEMTX_ERROR;
}

static MemTxResult rcc_spinlock_write(void *opaque, hwaddr offset,
                                      uint64_t value, unsigned size,
                                      MemTxAttrs attrs)
{
    MH2PRCCMachineState *s = opaque;

    if (offset == 0x10) {
        if (value & 2) {
            memset(s->spinlocks, 0, sizeof(s->spinlocks));
        }
        return MEMTX_OK;
    }
    if (offset >= 0x800 && offset < 0xc00) {
        unsigned lock = (offset - 0x800) / 4;

        if (!(value & 1)) {
            s->spinlocks[lock / 32] &= ~BIT(lock % 32);
        }
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP, "mh2p-rcc: unsupported spinlock write 0x%"
                  HWADDR_PRIx " = 0x%" PRIx64 "\n", offset, value);
    return MEMTX_ERROR;
}

static const MemoryRegionOps rcc_spinlock_ops = {
    .read_with_attrs = rcc_spinlock_read,
    .write_with_attrs = rcc_spinlock_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static uint32_t rcc_dcan_interrupts(RCCDCAN *s)
{
    uint32_t pending[2] = {0};

    for (unsigned i = 0; i < ARRAY_SIZE(s->messages); i++) {
        unsigned line = !!(s->interrupt_mux[i / 32] & BIT(i % 32));

        if (!pending[line] && (s->messages[i].control & BIT(13))) {
            pending[line] = i + 1;
        }
    }
    if (s->status_pending) {
        pending[0] = 0x8000;
    }
    qemu_set_irq(s->irq[0], (s->clock_control & 2) &&
                 (s->control & BIT(1)) && pending[0]);
    qemu_set_irq(s->irq[1], (s->clock_control & 2) &&
                 (s->control & BIT(17)) && pending[1]);
    return pending[0] | (pending[1] << 16);
}

static void rcc_dcan_reset(RCCDCAN *s)
{
    timer_del(s->transmit_timer);
    s->transmit_object = 0;
    s->transmit_remaining = s->transmit_errors = 0;
    s->status_pending = false;
    s->control = 0x1401;
    s->bit_timing = 0x2301;
    s->status = 0;
    s->auto_bus_on = 0;
    memset(s->interface, 0, sizeof(s->interface));
    memset(s->interrupt_mux, 0, sizeof(s->interrupt_mux));
    for (unsigned i = 0; i < ARRAY_SIZE(s->interface); i++) {
        s->interface[i].mask = UINT32_MAX;
        s->command[i] = 1;
    }
    rcc_dcan_interrupts(s);
}

static bool rcc_dcan_message_flag(RCCDCANMessage *message, unsigned kind)
{
    switch (kind) {
    case 0: return message->control & BIT(8);
    case 1: return message->control & BIT(15);
    case 2: return message->control & BIT(13);
    case 3: return message->arbitration & BIT(31);
    default: g_assert_not_reached();
    }
}

static void rcc_dcan_schedule(RCCDCAN *s)
{
    if (s->control & 1) {
        timer_del(s->transmit_timer);
        s->transmit_remaining = 0;
        s->transmit_object = 0;
        return;
    }
    if (!(s->clock_control & 2)) {
        if (timer_pending(s->transmit_timer)) {
            s->transmit_remaining = MAX(1,
                timer_expire_time_ns(s->transmit_timer) -
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
            timer_del(s->transmit_timer);
        }
        return;
    }
    if (timer_pending(s->transmit_timer)) {
        return;
    }
    if (s->transmit_remaining) {
        timer_mod(s->transmit_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  s->transmit_remaining);
        s->transmit_remaining = 0;
        return;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(s->messages); i++) {
        RCCDCANMessage *message = &s->messages[i];

        if ((message->arbitration & BIT(31)) &&
            (message->control & BIT(8))) {
            unsigned prescaler = (s->bit_timing & 0x3f) |
                                 ((s->bit_timing >> 10) & 0x3c0);
            unsigned quanta = 3 + ((s->bit_timing >> 8) & 15) +
                              ((s->bit_timing >> 12) & 7);
            unsigned bits = 47 + ((message->arbitration & BIT(30)) ? 20 : 0);

            if (message->arbitration & BIT(29)) {
                bits += MIN(message->control & 15, 8) * 8;
            }
            /*
             * Frame-level timing uses a conservative stuffing allowance,
             * error delimiter/intermission and error-passive suspend time.
             * The direct-start fixture supplies SYS_CLK1 at 20 MHz.
             */
            bits += (bits - 1) / 4 + 17 +
                    (s->transmit_errors >= 128 ? 8 : 0);
            s->transmit_object = i + 1;
            if (s->control & BIT(5)) {
                message->control &= ~BIT(8);
            } else {
                message->control &= ~BIT(15);
            }
            timer_mod(s->transmit_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      (uint64_t)bits * (prescaler + 1) * quanta * 50);
            return;
        }
    }
}

static bool rcc_dcan_can_receive(CanBusClientState *client)
{
    RCCDCAN *s = container_of(client, RCCDCAN, bus_client);

    return (s->clock_control & 2) && !(s->control & 1);
}

static ssize_t rcc_dcan_receive(CanBusClientState *client,
                                const qemu_can_frame *frames, size_t count)
{
    RCCDCAN *s = container_of(client, RCCDCAN, bus_client);
    bool acknowledged = false;

    for (size_t f = 0; f < count; f++) {
        const qemu_can_frame *frame = &frames[f];
        uint32_t arbitration = frame->can_id & QEMU_CAN_EFF_MASK;

        if (frame->can_dlc > 8 || frame->flags ||
            (frame->can_id & QEMU_CAN_ERR_FLAG)) {
            qemu_log_mask(LOG_UNIMP, "mh2p-rcc: unsupported DCAN bus frame\n");
            continue;
        }
        acknowledged = true;
        s->status = (s->status & ~7U) | BIT(4);
        if (s->control & BIT(2)) {
            s->status_pending = true;
        }
        if (frame->can_id & QEMU_CAN_RTR_FLAG) {
            qemu_log_mask(LOG_UNIMP,
                          "mh2p-rcc: DCAN remote-frame response not modeled\n");
            continue;
        }
        if (frame->can_id & QEMU_CAN_EFF_FLAG) {
            arbitration |= BIT(30);
        } else {
            arbitration = (arbitration & QEMU_CAN_SFF_MASK) << 18;
        }
        for (unsigned i = 0; i < ARRAY_SIZE(s->messages); i++) {
            RCCDCANMessage *message = &s->messages[i];
            uint32_t mask = BIT(30) | BIT(29) | QEMU_CAN_EFF_MASK;

            if (!(message->arbitration & BIT(31))) {
                continue;
            }
            if (message->control & BIT(12)) {
                mask = message->mask & QEMU_CAN_EFF_MASK;
                if (message->mask & BIT(31)) {
                    mask |= BIT(30);
                }
                if (message->mask & BIT(30)) {
                    mask |= BIT(29);
                }
            }
            if (!(message->arbitration & BIT(30))) {
                mask &= ~0x3ffffU;
            }
            if ((message->arbitration ^ arbitration) & mask) {
                continue;
            }
            if ((message->control & BIT(15)) &&
                !(message->control & BIT(7)) &&
                i + 1 < ARRAY_SIZE(s->messages)) {
                continue;
            }
            if (message->control & BIT(15)) {
                message->control |= BIT(14);
            }
            for (unsigned byte = 0; byte < frame->can_dlc; byte++) {
                message->data[byte / 4] =
                    deposit32(message->data[byte / 4], (byte % 4) * 8,
                              8, frame->data[byte]);
            }
            message->arbitration = BIT(31) | arbitration;
            message->control = (message->control & ~(15U | BIT(8))) |
                               BIT(15) | frame->can_dlc;
            if (message->control & BIT(10)) {
                message->control |= BIT(13);
            }
            break;
        }
    }
    rcc_dcan_interrupts(s);
    return acknowledged;
}

static CanBusClientInfo rcc_dcan_bus_info = {
    .can_receive = rcc_dcan_can_receive,
    .receive = rcc_dcan_receive,
};

static void rcc_dcan_transmit(void *opaque)
{
    RCCDCAN *s = opaque;
    bool warned = s->transmit_errors >= 96;
    RCCDCANMessage *message = &s->messages[s->transmit_object - 1];
    qemu_can_frame frame = {
        .can_id = message->arbitration & QEMU_CAN_EFF_MASK,
        .can_dlc = MIN(message->control & 15, 8),
    };

    if (message->arbitration & BIT(30)) {
        frame.can_id |= QEMU_CAN_EFF_FLAG;
    } else {
        frame.can_id >>= 18;
    }
    if (!(message->arbitration & BIT(29))) {
        frame.can_id |= QEMU_CAN_RTR_FLAG;
    }
    stl_le_p(frame.data, message->data[0]);
    stl_le_p(frame.data + 4, message->data[1]);
    if (can_bus_client_send(&s->bus_client, &frame, 1) > 0) {
        message->control &= ~(BIT(8) | BIT(15));
        if (message->control & BIT(11)) {
            message->control |= BIT(13);
        }
        s->transmit_errors -= s->transmit_errors != 0;
        s->status = BIT(3) | (s->status & BIT(4)) |
                    (s->transmit_errors >= 96 ? BIT(6) : 0) |
                    (s->transmit_errors >= 128 ? BIT(5) : 0);
        if ((s->control & BIT(2)) ||
            ((s->control & BIT(3)) && warned && s->transmit_errors < 96)) {
            s->status_pending = true;
        }
        rcc_dcan_interrupts(s);
        rcc_dcan_schedule(s);
        return;
    }

    /*
     * An isolated transmitter sees ACK errors, not successful sends.
     * A passive transmitter with no dominant ACK/error flag stops increasing
     * TEC at 128; a disconnected bus must not manufacture bus-off.
     */
    s->transmit_errors = MIN(s->transmit_errors + 8, 128);
    s->status = (s->status & ~7U) | 3;
    if (s->transmit_errors >= 96) {
        s->status |= BIT(6);
    }
    if (s->transmit_errors >= 128) {
        s->status |= BIT(5);
    }
    if ((s->control & BIT(2)) ||
        ((s->control & BIT(3)) && !warned && s->transmit_errors >= 96)) {
        s->status_pending = true;
    }
    rcc_dcan_interrupts(s);
    rcc_dcan_schedule(s);
}

static bool rcc_dcan_transfer(RCCDCAN *s, unsigned index, uint32_t command)
{
    unsigned number = command & 0xff;
    RCCDCANMessage *interface = &s->interface[index];
    RCCDCANMessage *message, *source, *destination;
    RCCDCANMessage updated_message, updated_interface;
    bool write = command & BIT(23);

    if (!number || number > ARRAY_SIZE(s->messages) ||
        (command & BIT(14))) {
        return false;
    }
    message = &s->messages[number - 1];
    updated_message = *message;
    updated_interface = *interface;
    source = write ? &updated_interface : &updated_message;
    destination = write ? &updated_message : &updated_interface;
    if (command & BIT(22)) {
        destination->mask = source->mask;
    }
    if (command & BIT(21)) {
        destination->arbitration = source->arbitration;
    }
    if (command & BIT(20)) {
        destination->control = source->control;
    }
    if (command & BIT(17)) {
        destination->data[0] = source->data[0];
    }
    if (command & BIT(16)) {
        destination->data[1] = source->data[1];
    }
    if (write && (command & BIT(18))) {
        updated_message.control |= BIT(15) | BIT(8);
    }
    if (!write && (command & BIT(19))) {
        updated_message.control &= ~BIT(13);
    }
    if (!write && (command & BIT(18))) {
        updated_message.control &= ~BIT(15);
    }
    if (write && (updated_message.arbitration & BIT(31)) &&
        (updated_message.control & BIT(8)) &&
        (s->clock_control & BIT(24))) {
        return false; /* SYS_CLK2 timing is not modeled. */
    }
    *message = updated_message;
    *interface = updated_interface;
    s->command[index] = command & 0xff00ff;
    rcc_dcan_interrupts(s);
    rcc_dcan_schedule(s);
    return true;
}

static MemTxResult rcc_dcan_read(void *opaque, hwaddr offset,
                                uint64_t *value, unsigned size,
                                MemTxAttrs attrs)
{
    RCCDCAN *s = opaque;

    if (offset >= 0x100 && offset < 0x138 && (offset & 0x1f) < 0x18) {
        unsigned index = (offset - 0x100) / 0x20;
        unsigned reg = offset & 0x1f;

        if (reg < 4) {
            if (reg + size > 4) {
                goto unsupported;
            }
            *value = extract32(s->command[index], (reg & 3) * 8, size * 8);
            return MEMTX_OK;
        }
        if (size != 4) {
            goto unsupported;
        }
        switch (reg) {
        case 4: *value = s->interface[index].mask | BIT(29); break;
        case 8: *value = s->interface[index].arbitration; break;
        case 0xc: *value = s->interface[index].control; break;
        case 0x10: *value = s->interface[index].data[0]; break;
        case 0x14: *value = s->interface[index].data[1]; break;
        default: goto unsupported;
        }
        return MEMTX_OK;
    }
    if (size != 4) {
        goto unsupported;
    }
    if (offset >= 0x84 && offset <= 0xd0) {
        unsigned kind = (offset - 0x84) / 0x14;
        unsigned part = ((offset - 0x84) % 0x14) / 4;

        *value = 0;
        for (unsigned i = 0; i < ARRAY_SIZE(s->messages); i++) {
            if (rcc_dcan_message_flag(&s->messages[i], kind) &&
                (!part || i / 32 == part - 1)) {
                *value |= BIT(part ? i % 32 : i / 8);
            }
        }
        return MEMTX_OK;
    }
    if (offset >= 0xd8 && offset <= 0xe4) {
        *value = s->interrupt_mux[(offset - 0xd8) / 4];
        return MEMTX_OK;
    }
    switch (offset) {
    case 0:
        *value = s->control;
        return MEMTX_OK;
    case 4:
        *value = s->status;
        s->status = (s->status & 0xe0) | 7;
        s->status_pending = false;
        rcc_dcan_interrupts(s);
        return MEMTX_OK;
    case 8:
        *value = s->transmit_errors;
        return MEMTX_OK;
    case 0x10:
        *value = rcc_dcan_interrupts(s);
        return MEMTX_OK;
    case 0xc:
        *value = s->bit_timing;
        return MEMTX_OK;
    case 0x14:
        *value = 0x80; /* Unconnected CAN input remains recessive. */
        return MEMTX_OK;
    case 0x80:
        *value = s->auto_bus_on;
        return MEMTX_OK;
    }
unsupported:
    qemu_log_mask(LOG_UNIMP, "mh2p-rcc: unsupported DCAN read 0x%"
                  HWADDR_PRIx "\n", offset);
    *value = 0;
    return MEMTX_ERROR;
}

static MemTxResult rcc_dcan_write(void *opaque, hwaddr offset,
                                 uint64_t value, unsigned size,
                                 MemTxAttrs attrs)
{
    RCCDCAN *s = opaque;

    if (offset >= 0x100 && offset < 0x138 && (offset & 0x1f) < 0x18) {
        unsigned index = (offset - 0x100) / 0x20;
        unsigned reg = offset & 0x1f;
        RCCDCANMessage *interface = &s->interface[index];

        if (reg < 4) {
            if (reg + size > 4) {
                goto unsupported;
            }
            uint32_t command = deposit32(s->command[index], reg * 8,
                                         size * 8, value);

            if (command & BIT(14)) {
                goto unsupported;
            }
            if (reg == 0) {
                if (!rcc_dcan_transfer(s, index, command)) {
                    goto unsupported;
                }
            } else {
                s->command[index] = command & 0xff00ff;
            }
            return MEMTX_OK;
        }
        if (size != 4) {
            goto unsupported;
        }
        switch (reg) {
        case 4: interface->mask = value | BIT(29); break;
        case 8: interface->arbitration = value; break;
        case 0xc: interface->control = value & 0xff8f; break;
        case 0x10: interface->data[0] = value; break;
        case 0x14: interface->data[1] = value; break;
        default: goto unsupported;
        }
        return MEMTX_OK;
    }
    if (size != 4) {
        goto unsupported;
    }
    if (offset >= 0xd8 && offset <= 0xe4) {
        s->interrupt_mux[(offset - 0xd8) / 4] = value;
        rcc_dcan_interrupts(s);
        return MEMTX_OK;
    }
    if (offset == 0) {
        /*
         * DMA, parity, test, power-down and debug
         * operations require their actual engines, not a ready-bit fixture.
         * WUBA can be configured but an unattached bus cannot cause a wakeup.
         */
        if ((value & ~0x202be6fU) || (value & 0x3c00) != 0x1400) {
            goto unsupported;
        }
        if (!(value & 1) && (s->clock_control & BIT(24))) {
            goto unsupported;
        }
        if (value & BIT(15)) {
            if (!(s->control & 1) || !(value & 1)) {
                goto unsupported;
            }
            rcc_dcan_reset(s);
        } else {
            s->control = value;
            rcc_dcan_interrupts(s);
            rcc_dcan_schedule(s);
        }
        return MEMTX_OK;
    }
    if (offset == 0xc && !(value & ~0xf7fffU)) {
        if ((s->control & 0x41) == 0x41) {
            s->bit_timing = value;
        }
        return MEMTX_OK;
    }
    if (offset == 0x14 && !(s->control & BIT(7))) {
        return MEMTX_OK; /* Test controls are not writable outside TEST mode. */
    }
    if (offset == 0x80) {
        s->auto_bus_on = value;
        return MEMTX_OK;
    }
unsupported:
    qemu_log_mask(LOG_UNIMP, "mh2p-rcc: unsupported DCAN write 0x%"
                  HWADDR_PRIx " = 0x%" PRIx64 "\n", offset, value);
    return MEMTX_ERROR;
}

static const MemoryRegionOps rcc_dcan_ops = {
    .read_with_attrs = rcc_dcan_read,
    .write_with_attrs = rcc_dcan_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void rcc_gp_timer_update_irq(RCCTimer *s)
{
    qemu_set_irq(s->irq, !!(s->status & s->enabled));
}

static bool rcc_gp_timer_clock_enabled(RCCTimer *s)
{
    return s->number == 12 || (s->clock_control & 2);
}

static unsigned rcc_gp_timer_frequency(RCCTimer *s)
{
    unsigned divisor = (s->control & 0x20) ?
                       1U << (((s->control >> 2) & 7) + 1) : 1;
    unsigned frequency = (s->number == 12 || (s->clock_control & BIT(24))) ?
                         32768 : 20000000;

    return frequency / divisor;
}

static void rcc_gp_timer_sync(RCCTimer *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (rcc_gp_timer_clock_enabled(s) && (s->control & 1)) {
        uint64_t elapsed = now - s->epoch_ns;
        unsigned hz = rcc_gp_timer_frequency(s);
        uint64_t fraction = (elapsed % NANOSECONDS_PER_SECOND) * hz %
                            NANOSECONDS_PER_SECOND + s->phase;
        uint64_t ticks = muldiv64(elapsed, hz, NANOSECONDS_PER_SECOND) +
                         fraction / NANOSECONDS_PER_SECOND;
        uint64_t remaining = UINT64_C(0x100000000) - s->count;

        s->phase = fraction % NANOSECONDS_PER_SECOND;
        if (ticks >= remaining) {
            s->status |= 2;
            if (s->control & 2) {
                s->count = s->load + (ticks - remaining) %
                           (UINT64_C(0x100000000) - s->load);
            } else {
                s->count = s->phase = 0;
                s->control &= ~1U;
            }
            rcc_gp_timer_update_irq(s);
        } else {
            s->count += ticks;
        }
    }
    s->epoch_ns = now;
}

static void rcc_gp_timer_arm(RCCTimer *s)
{
    timer_del(s->expiry);
    if (rcc_gp_timer_clock_enabled(s) && (s->control & 1)) {
        uint64_t remaining = UINT64_C(0x100000000) - s->count;
        uint64_t ns = DIV_ROUND_UP(remaining * NANOSECONDS_PER_SECOND -
                                  s->phase, rcc_gp_timer_frequency(s));

        timer_mod(s->expiry, s->epoch_ns + ns);
    }
}

static void rcc_gp_timer_expired(void *opaque)
{
    RCCTimer *s = opaque;

    rcc_gp_timer_sync(s);
    rcc_gp_timer_arm(s);
}

static void rcc_gp_timer_reset(RCCTimer *s)
{
    timer_del(s->expiry);
    s->epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->count = s->phase = 0;
    s->config = s->control = s->load = s->match = 0;
    s->status = s->enabled = s->wakeup = 0;
    s->posted = 4;
    rcc_gp_timer_update_irq(s);
}

static MemTxResult rcc_gp_timer_read(void *opaque, hwaddr offset,
                                    uint64_t *value, unsigned size,
                                    MemTxAttrs attrs)
{
    RCCTimer *s = opaque;

    if (size != 4 || !rcc_gp_timer_clock_enabled(s)) {
        goto unsupported;
    }
    rcc_gp_timer_sync(s);
    switch (offset) {
    case 0x10:
        *value = s->config;
        break;
    case 0x20:
    case 0x48: /* Posted writes complete synchronously. */
        *value = 0;
        break;
    case 0x24:
        *value = s->status;
        break;
    case 0x28:
        *value = s->status & s->enabled;
        break;
    case 0x2c:
    case 0x30:
        *value = s->enabled;
        break;
    case 0x34:
        *value = s->wakeup;
        break;
    case 0x38:
        *value = s->control;
        break;
    case 0x3c:
        *value = s->count;
        break;
    case 0x40:
        *value = s->load;
        break;
    case 0x44:
        *value = UINT32_MAX;
        break;
    case 0x4c:
        *value = s->match;
        break;
    case 0x54:
        *value = s->posted;
        break;
    default:
        goto unsupported;
    }
    return MEMTX_OK;
unsupported:
    *value = 0;
    qemu_log_mask(LOG_UNIMP, "mh2p-rcc: unsupported TIMER%u read 0x%"
                  HWADDR_PRIx " size=%u clock=0x%x\n",
                  s->number, offset, size, s->clock_control);
    return MEMTX_ERROR;
}

static MemTxResult rcc_gp_timer_write(void *opaque, hwaddr offset,
                                     uint64_t value, unsigned size,
                                     MemTxAttrs attrs)
{
    RCCTimer *s = opaque;

    if (size != 4 || !rcc_gp_timer_clock_enabled(s)) {
        goto unsupported;
    }
    rcc_gp_timer_sync(s);
    if (offset == 0x10 && !(value & ~0xf)) {
        if (value & 1) {
            rcc_gp_timer_reset(s);
        } else {
            s->config = value;
        }
        return MEMTX_OK;
    }
    if (offset == 0x20 && !value) {
        return MEMTX_OK;
    }
    if (offset >= 0x24 && offset <= 0x34 && !(value & ~7)) {
        switch (offset) {
        case 0x24:
            s->status |= value;
            break;
        case 0x28:
            s->status &= ~value;
            break;
        case 0x2c:
            s->enabled |= value;
            break;
        case 0x30:
            s->enabled &= ~value;
            break;
        case 0x34:
            s->wakeup = value;
            break;
        }
        rcc_gp_timer_update_irq(s);
        return MEMTX_OK;
    }
    if (offset == 0x38 && !(value & ~0x3f)) {
        s->control = value;
        rcc_gp_timer_arm(s);
        return MEMTX_OK;
    }
    if (offset == 0x3c || offset == 0x40 || offset == 0x44) {
        if (offset == 0x40) {
            s->load = value;
        } else {
            s->count = offset == 0x44 ? s->load : value;
            s->phase = 0;
        }
        rcc_gp_timer_arm(s);
        return MEMTX_OK;
    }
    if (offset == 0x4c) {
        s->match = value;
        return MEMTX_OK;
    }
    if (offset == 0x54 && (value == 0 || value == 4)) {
        s->posted = value;
        return MEMTX_OK;
    }
unsupported:
    qemu_log_mask(LOG_UNIMP, "mh2p-rcc: unsupported TIMER%u write 0x%"
                  HWADDR_PRIx " = 0x%" PRIx64 " size=%u\n",
                  s->number, offset, value, size);
    return MEMTX_ERROR;
}

static const MemoryRegionOps rcc_gp_timer_ops = {
    .read_with_attrs = rcc_gp_timer_read,
    .write_with_attrs = rcc_gp_timer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static uint64_t rcc_mpu_wdt_period(MH2PRCCMachineState *s)
{
    return (s->mpu_wdt_sampled_prescaler + 1) *
           (s->mpu_dpll_mode == 7 ? 1ULL : 50ULL);
}

static void rcc_mpu_wdt_sync(MH2PRCCMachineState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if ((s->mpu_wdt_control & 1) && s->mpu_wdt_remaining) {
        uint64_t elapsed = now - s->mpu_wdt_epoch + s->mpu_wdt_phase;
        uint64_t period = rcc_mpu_wdt_period(s);
        uint64_t ticks = elapsed / period;

        s->mpu_wdt_phase = elapsed % period;
        s->mpu_wdt_remaining -= MIN(ticks, s->mpu_wdt_remaining);
    }
    s->mpu_wdt_epoch = now;
    if (s->mpu_wdt_control & 1) {
        uint64_t count = s->mpu_wdt_remaining ?
                         s->mpu_wdt_remaining - 1 : 0;

        if ((s->mpu_wdt_control & BIT(8)) && !s->mpu_wdt_warned &&
            count <= s->mpu_wdt_warning) {
            s->mpu_wdt_status |= 2;
            s->mpu_wdt_warned = true;
        }
        if (!s->mpu_wdt_remaining && !s->mpu_wdt_expired) {
            s->mpu_wdt_status |= 1;
            s->mpu_wdt_expired = true;
        }
        if (s->mpu_wdt_expired && (s->mpu_wdt_control & BIT(3)) &&
            !s->mpu_wdt_reset_requested) {
            s->mpu_wdt_reset_requested = true;
            qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        }
    }
    qemu_set_irq(s->crossbar.inputs[5],
                 (s->mpu_wdt_status & 2) && (s->mpu_wdt_control & BIT(8)));
    qemu_set_irq(s->crossbar.inputs[139],
                 (s->mpu_wdt_status & 1) && (s->mpu_wdt_control & BIT(1)));
}

static void rcc_mpu_wdt_arm(MH2PRCCMachineState *s)
{
    uint64_t ticks = s->mpu_wdt_remaining;

    timer_del(s->mpu_watchdog_timer);
    if (!(s->mpu_wdt_control & 1) || !ticks) {
        return;
    }
    if ((s->mpu_wdt_control & BIT(8)) && !s->mpu_wdt_warned &&
        ticks > (uint64_t)s->mpu_wdt_warning + 1) {
        ticks -= (uint64_t)s->mpu_wdt_warning + 1;
    }
    timer_mod(s->mpu_watchdog_timer, s->mpu_wdt_epoch +
              ticks * rcc_mpu_wdt_period(s) - s->mpu_wdt_phase);
}

static void rcc_mpu_wdt_expired(void *opaque)
{
    MH2PRCCMachineState *s = opaque;

    rcc_mpu_wdt_sync(s);
    rcc_mpu_wdt_arm(s);
}

static MemTxResult rcc_mpu_wdt_read(void *opaque, hwaddr offset,
                                   uint64_t *value, unsigned size,
                                   MemTxAttrs attrs)
{
    MH2PRCCMachineState *s = opaque;

    rcc_mpu_wdt_sync(s);
    switch (offset) {
    case 0x00:
        *value = s->mpu_wdt_load;
        return MEMTX_OK;
    case 0x04:
        *value = s->mpu_wdt_remaining ? s->mpu_wdt_remaining - 1 : 0;
        return MEMTX_OK;
    case 0x08:
        *value = s->mpu_wdt_warning;
        return MEMTX_OK;
    case 0x0c:
        *value = s->mpu_wdt_prescaler;
        return MEMTX_OK;
    case 0x10:
        *value = s->mpu_wdt_control;
        return MEMTX_OK;
    case 0x14:
        *value = s->mpu_wdt_status;
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP, "mh2p-rcc: unsupported MPU watchdog read "
                  "offset=0x%" HWADDR_PRIx "\n", offset);
    return MEMTX_ERROR;
}

static MemTxResult rcc_mpu_wdt_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size,
                                    MemTxAttrs attrs)
{
    MH2PRCCMachineState *s = opaque;

    rcc_mpu_wdt_sync(s);
    switch (offset) {
    case 0x00:
        s->mpu_wdt_load = value;
        s->mpu_wdt_remaining = value + 1;
        s->mpu_wdt_sampled_prescaler = s->mpu_wdt_prescaler;
        s->mpu_wdt_phase = 0;
        s->mpu_wdt_warned = s->mpu_wdt_expired = false;
        s->mpu_wdt_reset_requested = false;
        break;
    case 0x08:
        s->mpu_wdt_warning = value;
        break;
    case 0x0c:
        s->mpu_wdt_prescaler = value & 0x3ff;
        break;
    case 0x10:
        s->mpu_wdt_control = value & 0x10b;
        break;
    case 0x14:
        s->mpu_wdt_status &= ~value;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "mh2p-rcc: unsupported MPU watchdog write "
                      "offset=0x%" HWADDR_PRIx " value=0x%" PRIx64 "\n",
                      offset, value);
        return MEMTX_ERROR;
    }
    rcc_mpu_wdt_sync(s);
    rcc_mpu_wdt_arm(s);
    return MEMTX_OK;
}

static const MemoryRegionOps rcc_mpu_wdt_ops = {
    .read_with_attrs = rcc_mpu_wdt_read,
    .write_with_attrs = rcc_mpu_wdt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static bool rcc_pipe3_can_lock(MH2PRCCMachineState *s)
{
    uint32_t cfg1 = s->pipe3_regs[RCC_PIPE3_CFG1];
    uint32_t cfg2 = s->pipe3_regs[RCC_PIPE3_CFG2];
    uint64_t multiplier = ((uint64_t)(cfg1 >> 9) << 18) |
                          s->pipe3_regs[RCC_PIPE3_CFG4];
    uint64_t numerator = 20000000 * multiplier;
    uint64_t denominator = (((cfg1 >> 1) & 0xff) + 1) * (1ULL << 18);
    unsigned range = (cfg2 >> 1) & 7;

    if ((cfg2 & 1) || ((cfg2 >> 9) & 3) > 1) {
        return false;
    }
    return (range == 2 && numerator >= 750000000ULL * denominator &&
            numerator <= 1500000000ULL * denominator) ||
           (range == 4 && numerator >= 1250000000ULL * denominator &&
            numerator <= 2500000000ULL * denominator);
}

static bool rcc_pcie_dpll_valid(MH2PRCCMachineState *s)
{
    uint32_t select = s->pcie_dpll_select;
    unsigned m = (select >> 8) & 0xfff;
    unsigned n = (select & 0xff) + 1;
    unsigned sd = select >> 24;
    uint64_t frequency = (select & BIT(21)) ? 1250000000 : 1500000000;

    /* Only the two nominal DCO profiles are qualified with SYS_CLKIN1=20 MHz. */
    return m >= 2 && 20000000ULL * m == frequency * n &&
           sd == DIV_ROUND_UP(frequency, 250000000) &&
           s->pcie_dpll_divider != 0;
}

static bool rcc_pcie_apll_can_lock(MH2PRCCMachineState *s, uint32_t mode)
{
    uint64_t m = (s->pcie_dpll_select >> 8) & 0xfff;
    unsigned n = (s->pcie_dpll_select & 0xff) + 1;

    if ((s->pcie_clkctrl[2] & 3) != 2) {
        return false;
    }
    if (mode & BIT(7)) {
        return s->pcie_external_reference && s->pcie_acs_control == BIT(17);
    }
    return s->pcie_dpll_mode == 7 &&
           20000000 * m == 100000000ULL * n * s->pcie_dpll_divider;
}

static uint32_t rcc_pcie_apll_outputs(MH2PRCCMachineState *s)
{
    return (s->pcie_apll_mode & 3) == 1 &&
           rcc_pcie_apll_can_lock(s, s->pcie_apll_mode) ?
           s->pcie_clkctrl[2] & 0x600 : 0;
}

static void rcc_pcie_external_reference(void *opaque, int n, int level)
{
    MH2PRCCMachineState *s = opaque;

    s->pcie_external_reference = level;
}

static MemTxResult rcc_pcie_pll_read(void *opaque, hwaddr offset,
                                    uint64_t *value, unsigned size,
                                    MemTxAttrs attrs)
{
    MH2PRCCMachineState *s = opaque;

    switch (offset) {
    case 0:
        *value = s->pcie_dpll_mode;
        return MEMTX_OK;
    case 4:
        *value = (s->pcie_dpll_initialized ? BIT(4) : 0) |
                 (s->pcie_dpll_mode == 7 ? 1 : s->pcie_dpll_mode << 1);
        return MEMTX_OK;
    case 8:
        *value = 0; /* Automatic low-power transitions are not supported. */
        return MEMTX_OK;
    case 0xc:
        *value = s->pcie_dpll_select;
        return MEMTX_OK;
    case 0x10:
        *value = s->pcie_dpll_divider |
                 (s->pcie_dpll_mode == 7 ? 0x600 : 0);
        return MEMTX_OK;
    case 0x1c:
        *value = s->pcie_apll_mode;
        return MEMTX_OK;
    case 0x20:
        *value = (s->pcie_apll_mode & 3) == 1 &&
                 rcc_pcie_apll_can_lock(s, s->pcie_apll_mode);
        return MEMTX_OK;
    case 0x28:
        *value = rcc_pcie_apll_outputs(s);
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP, "mh2p-rcc: unsupported PCIe PLL read at 0x%"
                  HWADDR_PRIx "\n", offset + 0x4a008200);
    *value = 0;
    return MEMTX_ERROR;
}

static MemTxResult rcc_pcie_pll_write(void *opaque, hwaddr offset,
                                     uint64_t value, unsigned size,
                                     MemTxAttrs attrs)
{
    MH2PRCCMachineState *s = opaque;

    switch (offset) {
    case 0:
        if (value == 1 || value == 5 ||
            (value == 7 && rcc_pcie_dpll_valid(s))) {
            s->pcie_dpll_mode = value;
            s->pcie_dpll_initialized |= value == 7;
            return MEMTX_OK;
        }
        break;
    case 8:
        if (value == 0) {
            return MEMTX_OK;
        }
        break;
    case 0xc:
        if (!(value & ~0xff2fffffU) && s->pcie_dpll_mode != 7) {
            s->pcie_dpll_select = value;
            return MEMTX_OK;
        }
        break;
    case 0x10:
        if (!(value & ~0x67f) && (value & 0x7f) &&
            s->pcie_dpll_mode != 7) {
            s->pcie_dpll_divider = value & 0x7f;
            return MEMTX_OK;
        }
        break;
    case 0x1c:
        if (!(value & ~0x183) &&
            ((!(value & 3) &&
              (!(s->pcie_apll_mode & 3) ||
               !rcc_pcie_apll_can_lock(s, s->pcie_apll_mode))) ||
             ((value & 3) == 1 && rcc_pcie_apll_can_lock(s, value)))) {
            s->pcie_apll_mode = value;
            return MEMTX_OK;
        }
        break;
    }
    qemu_log_mask(LOG_UNIMP, "mh2p-rcc: unsupported PCIe PLL write at 0x%"
                  HWADDR_PRIx " value=0x%" PRIx64 "\n",
                  offset + 0x4a008200, value);
    return MEMTX_ERROR;
}

static const MemoryRegionOps rcc_pcie_pll_ops = {
    .read_with_attrs = rcc_pcie_pll_read,
    .write_with_attrs = rcc_pcie_pll_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static MemTxResult rcc_pcie_rx_read(void *opaque, hwaddr offset,
                                   uint64_t *value, unsigned size,
                                   MemTxAttrs attrs)
{
    MH2PRCCMachineState *s = opaque;

    for (unsigned i = 0; i < ARRAY_SIZE(rcc_pcie_rx_registers); i++) {
        if (s->pcie_clkctrl[1] && offset == rcc_pcie_rx_registers[i].offset) {
            *value = s->pcie_rx_regs[i];
            return MEMTX_OK;
        }
    }
    qemu_log_mask(LOG_UNIMP, "mh2p-rcc: unsupported/clock-gated PCIe RX read "
                  "at 0x%" HWADDR_PRIx "\n", offset + 0x4a094000);
    *value = 0;
    return MEMTX_ERROR;
}

static MemTxResult rcc_pcie_rx_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size,
                                    MemTxAttrs attrs)
{
    MH2PRCCMachineState *s = opaque;

    for (unsigned i = 0; i < ARRAY_SIZE(rcc_pcie_rx_registers); i++) {
        uint32_t mask = rcc_pcie_rx_registers[i].mask;

        if (s->pcie_clkctrl[1] && offset == rcc_pcie_rx_registers[i].offset &&
            (value & ~mask) == (rcc_pcie_rx_registers[i].reset & ~mask)) {
            s->pcie_rx_regs[i] = value;
            return MEMTX_OK;
        }
    }
    qemu_log_mask(LOG_UNIMP, "mh2p-rcc: unsupported/clock-gated PCIe RX write "
                  "at 0x%" HWADDR_PRIx " value=0x%" PRIx64 "\n",
                  offset + 0x4a094000, value);
    return MEMTX_ERROR;
}

static const MemoryRegionOps rcc_pcie_rx_ops = {
    .read_with_attrs = rcc_pcie_rx_read,
    .write_with_attrs = rcc_pcie_rx_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void rcc_pcie_controller_reset(RCCPCIEController *s)
{
    static const uint32_t bar_masks[] = {
        0x1fffff, 0x1ffff, 0x1fffff, 0x1ffff, 0x1fff, 0x1ffff,
    };

    s->type = 4;
    s->command = 0;
    s->identity = 0x8888104c;
    s->pci_command = 0x00100000;
    s->class_code = 1;
    s->link_cap = 0x00433c22;
    s->cache_line = 0;
    s->interrupt_line = 0xff;
    s->subsystem = 0x00010000;
    for (unsigned i = 0; i < ARRAY_SIZE(rcc_pcie_capabilities); i++) {
        s->capabilities[i] = rcc_pcie_capabilities[i].reset;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(s->bar_mask); i++) {
        s->bar[i] = 8;
        s->bar_mask[i] = bar_masks[i];
    }
    s->viewport = 0;
    memset(s->inbound, 0, sizeof(s->inbound));
    memset(s->outbound, 0, sizeof(s->outbound));
}

static bool rcc_pcie_endpoint_ready(void *opaque)
{
    MH2PRCCMachineState *s = opaque;

    return (s->pcie_clkctrl[2] & 3) == 2 &&
           !(s->pcie_reset_control & 1) && s->pcie.type == 0 &&
           s->pcie.command == 1 && rcc_pcie_apll_outputs(s) == 0x600 &&
           s->pcie_phy_power == 0x0500c000;
}

static MemTxResult rcc_pcie_endpoint_config(void *opaque, unsigned reg,
                                            unsigned size, uint32_t *value,
                                            bool write)
{
    MH2PRCCMachineState *ms = opaque;
    RCCPCIEController *s = &ms->pcie;
    unsigned offset = reg & ~3U, shift = (reg & 3) * 8;
    uint32_t data, mask = MAKE_64BIT_MASK(shift, size * 8);

    switch (offset) {
    case 0:
        data = s->identity;
        break;
    case 4:
        data = s->pci_command;
        break;
    case 8:
        data = s->class_code;
        break;
    case 0xc:
        data = s->cache_line;
        break;
    case 0x10 ... 0x24:
        data = s->bar[(offset - 0x10) / 4];
        break;
    case 0x28:
    case 0x30: /* No expansion ROM is installed. */
    case 0x38:
        data = 0;
        break;
    case 0x2c:
        data = s->subsystem;
        break;
    case 0x34:
        data = 0x40;
        break;
    case 0x3c:
        data = 0x100 | s->interrupt_line;
        break;
    default:
        for (unsigned i = 0; i < ARRAY_SIZE(rcc_pcie_capabilities); i++) {
            if (offset != rcc_pcie_capabilities[i].offset) {
                continue;
            }
            data = s->capabilities[i];
            if (write) {
                uint32_t bits = (*value << shift) & mask;
                uint32_t writable = rcc_pcie_capabilities[i].writable & mask;

                /* Only D0 is functional; do not pretend to enter D1/D3. */
                if ((offset == 0x44 && (bits & 3)) ||
                    (offset == 0x50 && (bits & 0x00700000))) {
                    goto unsupported;
                }
                s->capabilities[i] = (data & ~writable) | (bits & writable);
                s->capabilities[i] &= ~(bits & rcc_pcie_capabilities[i].w1c);
            } else {
                if (offset == 0x80 && rcc_pcie_endpoint_ready(ms) &&
                    mh2p_pcie_config_peer(ms->pcie_transport)) {
                    data |= BIT(29);
                }
                *value = (data & mask) >> shift;
            }
            return MEMTX_OK;
        }
        if (offset == 0x7c) {
            if (!write) {
                *value = (s->link_cap & mask) >> shift;
            }
            return MEMTX_OK;
        }
        goto unsupported;
    }
    if (!write) {
        *value = (data & mask) >> shift;
        return MEMTX_OK;
    }
    data = (data & ~mask) | ((*value << shift) & mask);
    switch (offset) {
    case 4:
        s->pci_command = (s->pci_command & ~0x547U) | (data & 0x547);
        s->pci_command &= ~(data & mask & 0xf9000000);
        break;
    case 0xc:
        s->cache_line = data & 0xff;
        break;
    case 0x10 ... 0x24:
    {
        unsigned bar = (offset - 0x10) / 4;

        s->bar[bar] = (s->bar_mask[bar] & 1) ?
            (data & ~s->bar_mask[bar] & ~0xfU) | (s->bar[bar] & 0xf) : 0;
        break;
    }
    case 0x3c:
        s->interrupt_line = data & 0xff;
        break;
    }
    return MEMTX_OK;
unsupported:
    qemu_log_mask(LOG_UNIMP, "mh2p-rcc: unsupported external PCIe config "
                  "%s at 0x%x size %u\n", write ? "write" : "read", reg, size);
    return MEMTX_ERROR;
}

static MemTxResult rcc_pcie_outbound_access(MH2PRCCMachineState *ms,
                                          hwaddr offset, uint32_t *value,
                                          unsigned size, bool write)
{
    RCCPCIEController *s = &ms->pcie;
    uint64_t translated = 0;
    unsigned matches = 0;

    if (!ms->pcie_outbound_transport || !(s->pci_command & 4) ||
        !rcc_pcie_endpoint_ready(ms) ||
        !mh2p_pcie_config_peer(ms->pcie_transport)) {
        goto unsupported;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(s->outbound); i++) {
        RCCPCIERegion *region = &s->outbound[i];

        if (region->control2 != BIT(31) || region->control1 ||
            offset < region->base || offset + size - 1 > region->limit) {
            continue;
        }
        translated = (uint64_t)region->target + offset - region->base;
        matches++;
    }
    if (matches != 1 || translated + size > 1ULL << 32) {
        goto unsupported;
    }
    return mh2p_pcie_memory_access(
        ms->pcie_outbound_transport, true, translated, size, value, write);
unsupported:
    qemu_log_mask(LOG_UNIMP, "mh2p-rcc: unsupported/unmapped outbound PCIe "
                  "%s at 0x%" HWADDR_PRIx " size %u\n",
                  write ? "write" : "read", offset + 0x20000000, size);
    return MEMTX_ERROR;
}

static MemTxResult rcc_pcie_outbound_read(void *opaque, hwaddr offset,
                                        uint64_t *value, unsigned size,
                                        MemTxAttrs attrs)
{
    uint32_t data = 0;
    MemTxResult result = rcc_pcie_outbound_access(
        opaque, offset, &data, size, false);

    *value = data;
    return result;
}

static MemTxResult rcc_pcie_outbound_write(void *opaque, hwaddr offset,
                                         uint64_t value, unsigned size,
                                         MemTxAttrs attrs)
{
    uint32_t data = value;

    return rcc_pcie_outbound_access(opaque, offset, &data, size, true);
}

static const MemoryRegionOps rcc_pcie_outbound_ops = {
    .read_with_attrs = rcc_pcie_outbound_read,
    .write_with_attrs = rcc_pcie_outbound_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static MemTxResult rcc_pcie_controller_read(void *opaque, hwaddr offset,
                                          uint64_t *value, unsigned size,
                                          MemTxAttrs attrs);
static MemTxResult rcc_pcie_controller_write(void *opaque, hwaddr offset,
                                           uint64_t value, unsigned size,
                                           MemTxAttrs attrs);

static MemTxResult rcc_pcie_endpoint_memory(void *opaque, unsigned address,
                                          unsigned size, uint32_t *value,
                                          bool write)
{
    MH2PRCCMachineState *ms = opaque;
    RCCPCIEController *s = &ms->pcie;
    unsigned selected = ARRAY_SIZE(s->bar);
    uint64_t translated = 0;
    unsigned regions = 0;
    uint8_t bytes[4];
    MemTxResult result;

    if (!(s->pci_command & 2) || (s->capabilities[1] & 3)) {
        goto unsupported;
    }
    for (unsigned bar = 0; bar < ARRAY_SIZE(s->bar); bar++) {
        uint32_t base = s->bar[bar] & ~0xfU;
        uint64_t length = (uint64_t)s->bar_mask[bar] + 1;

        if (!s->bar_mask[bar] || address < base ||
            (uint64_t)address - base + size > length) {
            continue;
        }
        if (selected != ARRAY_SIZE(s->bar)) {
            goto unsupported;
        }
        selected = bar;
    }
    if (selected == ARRAY_SIZE(s->bar)) {
        goto unsupported;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(s->inbound); i++) {
        if ((s->inbound[i].control2 & 0xc0000700) !=
            (0xc0000000 | (selected << 8)) ||
            s->inbound[i].control1 != 0) {
            continue;
        }
        translated = (uint64_t)s->inbound[i].target +
                     address - (s->bar[selected] & ~0xfU);
        regions++;
    }
    if (regions != 1) {
        goto unsupported;
    }
    if (translated >= 0x51000000 && translated + size <= 0x51004000 &&
        size == 4) {
        if (write) {
            return rcc_pcie_controller_write(
                ms, translated - 0x51000000, *value, size,
                MEMTXATTRS_UNSPECIFIED);
        } else {
            uint64_t data = 0;

            result = rcc_pcie_controller_read(
                ms, translated - 0x51000000, &data, size,
                MEMTXATTRS_UNSPECIFIED);
            *value = data;
            return result;
        }
    }
    /* Route only RCC RAM and its real BAR1 mailbox, never arbitrary devices. */
    if (!((translated >= RCC_RAM_BASE &&
           translated + size <= RCC_RAM_BASE + MACHINE(ms)->ram_size) ||
          (translated >= 0x4a0f4000 && translated + size <= 0x4a0f5000 &&
           size == 4))) {
        goto unsupported;
    }
    if (write) {
        stl_le_p(bytes, *value);
        result = address_space_write(&address_space_memory, translated,
                                     MEMTXATTRS_UNSPECIFIED, bytes, size);
    } else {
        result = address_space_read(&address_space_memory, translated,
                                    MEMTXATTRS_UNSPECIFIED, bytes, size);
        if (result == MEMTX_OK) {
            *value = size == 4 ? ldl_le_p(bytes) :
                     size == 2 ? lduw_le_p(bytes) : bytes[0];
        }
    }
    return result;
unsupported:
    qemu_log_mask(LOG_UNIMP, "mh2p-rcc: unsupported/unmapped PCIe memory "
                  "%s at 0x%x size %u\n", write ? "write" : "read",
                  address, size);
    return MEMTX_ERROR;
}

static MemTxResult rcc_pcie_controller_read(void *opaque, hwaddr offset,
                                           uint64_t *value, unsigned size,
                                           MemTxAttrs attrs)
{
    MH2PRCCMachineState *ms = opaque;
    RCCPCIEController *s = &ms->pcie;
    unsigned region = s->viewport & 3;
    RCCPCIERegion *atu = (s->viewport & BIT(31)) ?
        &s->inbound[region] : &s->outbound[region];

    if ((ms->pcie_clkctrl[2] & 3) != 2) {
        goto unsupported;
    }
    switch (offset) {
    case 0x2100:
        *value = s->type;
        return MEMTX_OK;
    case 0x2104:
        *value = s->command;
        return MEMTX_OK;
    case 0x210c:
        *value = rcc_pcie_endpoint_ready(ms) &&
                 mh2p_pcie_config_peer(ms->pcie_transport) ? BIT(16) : 0;
        return MEMTX_OK;
    }
    if ((ms->pcie_reset_control & 1) || s->type != 0) {
        goto unsupported;
    }
    if (offset >= 0x10 && offset <= 0x24) {
        *value = s->bar[(offset - 0x10) / 4];
        return MEMTX_OK;
    }
    if (offset >= 0x1010 && offset <= 0x1024) {
        *value = s->bar_mask[(offset - 0x1010) / 4];
        return MEMTX_OK;
    }
    switch (offset) {
    case 0:
        *value = s->identity;
        return MEMTX_OK;
    case 4:
        *value = s->pci_command;
        return MEMTX_OK;
    case 8:
        *value = s->class_code;
        return MEMTX_OK;
    case 0x2c:
        *value = s->subsystem;
        return MEMTX_OK;
    case 0x7c:
        *value = s->link_cap;
        return MEMTX_OK;
    case 0x900:
        *value = s->viewport;
        return MEMTX_OK;
    case 0x904:
        *value = atu->control1;
        return MEMTX_OK;
    case 0x908:
        *value = atu->control2;
        return MEMTX_OK;
    case 0x90c:
        *value = atu->base;
        return MEMTX_OK;
    case 0x910:
        *value = 0;
        return MEMTX_OK;
    case 0x914:
        *value = atu->limit;
        return MEMTX_OK;
    case 0x918:
        *value = atu->target;
        return MEMTX_OK;
    case 0x91c:
        *value = 0; /* Only 32-bit local targets are supported. */
        return MEMTX_OK;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(rcc_pcie_capabilities); i++) {
        if (offset == rcc_pcie_capabilities[i].offset) {
            uint32_t data = 0;
            MemTxResult result = rcc_pcie_endpoint_config(
                ms, offset, size, &data, false);

            *value = data;
            return result;
        }
    }
unsupported:
    qemu_log_mask(LOG_UNIMP, "mh2p-rcc: unsupported/reset/clock-gated PCIe "
                  "controller read at 0x%" HWADDR_PRIx "\n",
                  offset + 0x51000000);
    *value = 0;
    return MEMTX_ERROR;
}

static MemTxResult rcc_pcie_controller_write(void *opaque, hwaddr offset,
                                            uint64_t value, unsigned size,
                                            MemTxAttrs attrs)
{
    MH2PRCCMachineState *ms = opaque;
    RCCPCIEController *s = &ms->pcie;
    unsigned region = s->viewport & 3;
    RCCPCIERegion *atu = (s->viewport & BIT(31)) ?
        &s->inbound[region] : &s->outbound[region];

    if (offset >= 0x900 && offset <= 0x91c) {
        trace_mh2p_rcc_pcie_iatu(s->viewport, offset, value);
    }
    if ((ms->pcie_clkctrl[2] & 3) != 2) {
        goto unsupported;
    }
    if (offset == 0x2100 && value == 0 && !s->command) {
        s->type = value;
        return MEMTX_OK;
    }
    if (offset == 0x2104 && value <= 1 && s->type == 0 &&
        !(ms->pcie_reset_control & 1) &&
        (!value || (rcc_pcie_apll_outputs(ms) == 0x600 &&
                    ms->pcie_phy_power == 0x0500c000))) {
        s->command = value;
        return MEMTX_OK;
    }
    if ((ms->pcie_reset_control & 1) || s->type != 0 ||
        (s->command && offset != 0x2c &&
         !(offset >= 0x900 && offset <= 0x91c))) {
        goto unsupported;
    }
    if (offset >= 0x10 && offset <= 0x24 && value == 0) {
        s->bar[(offset - 0x10) / 4] = value;
        return MEMTX_OK;
    }
    if (offset >= 0x1010 && offset <= 0x1024 &&
        (value == 0 || ((value & 1) && value <= UINT32_MAX &&
                        is_power_of_2(value + 1)))) {
        s->bar_mask[(offset - 0x1010) / 4] = value;
        return MEMTX_OK;
    }
    switch (offset) {
    case 0:
        s->identity = value;
        return MEMTX_OK;
    case 4:
        if (!(value & ~0x00100002U)) {
            s->pci_command = (value & 2) | 0x00100000;
            return MEMTX_OK;
        }
        break;
    case 8:
        s->class_code = value;
        return MEMTX_OK;
    case 0x2c:
        s->subsystem = value;
        return MEMTX_OK;
    case 0x7c:
        if ((value & ~0x3ffU) == (s->link_cap & ~0x3ffU) &&
            ((value & 0x3f0) == 0x10 || (value & 0x3f0) == 0x20) &&
            ((value & 0xf) == 1 || (value & 0xf) == 2)) {
            s->link_cap = value;
            return MEMTX_OK;
        }
        break;
    case 0x900:
        if (!(value & ~0x80000003U)) {
            s->viewport = value;
            return MEMTX_OK;
        }
        break;
    case 0x904:
        if (value == 0) {
            atu->control1 = value;
            return MEMTX_OK;
        }
        break;
    case 0x908:
        if ((s->viewport & BIT(31)) ?
            (!(value & ~0xc0000700U) && ((value >> 8) & 7) < 6 &&
             (!(value & BIT(31)) || (value & BIT(30)))) :
            !(value & ~0x80000000U)) {
            atu->control2 = value;
            return MEMTX_OK;
        }
        break;
    case 0x90c:
        if (!(value & 0xfff)) {
            atu->base = value;
            return MEMTX_OK;
        }
        break;
    case 0x910:
        if (!value) {
            return MEMTX_OK;
        }
        break;
    case 0x914:
        atu->limit = value;
        return MEMTX_OK;
    case 0x918:
        if (!(value & 0xfff)) {
            atu->target = value;
            return MEMTX_OK;
        }
        break;
    case 0x91c:
        if (!value) {
            return MEMTX_OK;
        }
        break;
    }
unsupported:
    qemu_log_mask(LOG_UNIMP, "mh2p-rcc: unsupported/reset/clock-gated PCIe "
                  "controller write at 0x%" HWADDR_PRIx " value=0x%" PRIx64
                  "\n", offset + 0x51000000, value);
    return MEMTX_ERROR;
}

static const MemoryRegionOps rcc_pcie_controller_ops = {
    .read_with_attrs = rcc_pcie_controller_read,
    .write_with_attrs = rcc_pcie_controller_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static MemTxResult rcc_unknown_read(void *opaque, hwaddr offset,
                                   uint64_t *value, unsigned size,
                                   MemTxAttrs attrs)
{
    MH2PRCCMachineState *s = opaque;

    for (unsigned i = 0;
         s->power_clocks && i < ARRAY_SIZE(rcc_bypass_dplls); i++) {
        hwaddr reg = offset + 0x40000000 - rcc_bypass_dplls[i].base;

        if (size == 4 && (reg == 0 || reg == 4 || reg == 8)) {
            *value = reg == 0 ? s->bypass_dpll_mode[i] :
                     reg == 4 ? s->bypass_dpll_mode[i] << 1 : 0;
            return MEMTX_OK;
        }
    }
    for (unsigned i = 0;
         s->power_clocks && i < ARRAY_SIZE(rcc_disabled_clkctrl); i++) {
        if (size == 4 && offset + 0x40000000 ==
            rcc_disabled_clkctrl[i].address) {
            *value = s->disabled_clkctrl[i];
            return MEMTX_OK;
        }
    }
    for (unsigned i = 0;
         s->power_clocks && i < ARRAY_SIZE(rcc_mcasp_gblctl); i++) {
        if (size == 4 && offset + 0x40000000 == rcc_mcasp_gblctl[i]) {
            /* No serial clocks or audio execution: all sections stay reset. */
            *value = 0;
            return MEMTX_OK;
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(s->dcan); i++) {
        if (size == 4 && offset + 0x40000000 == rcc_dcan_clocks[i]) {
            *value = s->dcan[i].clock_control |
                     (s->dcan[i].clock_control & 2 ? 0 : 0x30000);
            return MEMTX_OK;
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(rcc_gpio_clocks); i++) {
        if (size == 4 && offset + 0x40000000 == rcc_gpio_clocks[i]) {
            *value = s->gpio_clkctrl[i] |
                     (s->gpio_clkctrl[i] & 1 ? 0 : 0x30000);
            return MEMTX_OK;
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(rcc_gp_timers); i++) {
        if (size == 4 && offset + 0x40000000 == rcc_gp_timers[i].clock) {
            *value = s->gp_timer[i].clock_control |
                     (rcc_gp_timer_clock_enabled(&s->gp_timer[i]) ?
                      0 : 0x30000);
            return MEMTX_OK;
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(rcc_pcie_clkctrl); i++) {
        if (size == 4 && offset + 0x40000000 == rcc_pcie_clkctrl[i].address) {
            *value = s->pcie_clkctrl[i] |
                     ((s->pcie_clkctrl[i] & 3) ? 0 : 0x30000);
            return MEMTX_OK;
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(rcc_peripheral_clkctrl); i++) {
        if (size == 4 && offset + 0x40000000 == rcc_peripheral_clkctrl[i]) {
            *value = s->peripheral_clkctrl[i] |
                     (s->peripheral_clkctrl[i] ? 0 : 0x30000);
            return MEMTX_OK;
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(rcc_peripheral_context); i++) {
        if (size == 4 && offset + 0x40000000 ==
            rcc_peripheral_context[i].address) {
            *value = s->peripheral_context[i];
            return MEMTX_OK;
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(rcc_pads); i++) {
        if (size == 4 && offset + 0x40000000 == rcc_pads[i].address) {
            *value = s->pad_config[i];
            return MEMTX_OK;
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(rcc_ddr_registers); i++) {
        if (size == 4 && offset + 0x40000000 ==
            rcc_ddr_registers[i].address) {
            *value = s->ddr_regs[i];
            return MEMTX_OK;
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(rcc_pipe3_registers); i++) {
        if (size == 4 && offset + 0x40000000 ==
            rcc_pipe3_registers[i].address) {
            *value = s->pipe3_regs[i];
            return MEMTX_OK;
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(rcc_usb_clkctrl); i++) {
        if (size == 4 && offset + 0x40000000 == rcc_usb_clkctrl[i]) {
            /*
             * IDLEST reports the clock domain, not USB controller readiness.
             * With no USB activity, enabled HW_AUTO is immediately functional.
             */
            *value = s->usb_clkctrl[i] |
                     ((s->usb_clkctrl[i] & 3) ? 0 : (3 << 16));
            return MEMTX_OK;
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(rcc_usb_phy_clkctrl); i++) {
        if (size == 4 && offset + 0x40000000 == rcc_usb_phy_clkctrl[i]) {
            *value = s->usb_phy_clkctrl[i];
            return MEMTX_OK;
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(rcc_usb_phy_ana_config1); i++) {
        if (size == 4 && offset + 0x40000000 == rcc_usb_phy_ana_config1[i]) {
            *value = s->usb_phy_ana_config1[i];
            return MEMTX_OK;
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(rcc_mlb_registers); i++) {
        if (size == 4 && offset + 0x40000000 ==
            rcc_mlb_registers[i].address) {
            *value = s->mlb_regs[i];
            return MEMTX_OK;
        }
    }
    switch (size == 4 ? offset + 0x40000000 : 0) {
    case 0x4ae0c204:
        *value = 0x0b9bc02f; /* DRA72x ES1.0, TI JTAG IDCODE. */
        return MEMTX_OK;
    case 0x4ae04030:
        *value = (uint32_t)(3 + muldiv64(
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), 32768,
            NANOSECONDS_PER_SECOND));
        return MEMTX_OK;
    case 0x4a002544:
        *value = s->mmr_lock2;
        return MEMTX_OK;
    case 0x4a002554:
        *value = s->control_io1;
        return MEMTX_OK;
    case 0x4a008730:
    case 0x4a008770:
    case 0x4a008778:
    case 0x4a008780:
        /* Read-only HW_AUTO clock in the direct-start, active L3 domain. */
        *value = 1;
        return MEMTX_OK;
    case 0x4a005520:
        if (s->ipu1_native) {
            *value = s->ipu1_clkctrl |
                     (rcc_ipu1_clock_active(s) ? 0 : 0x70000);
            return MEMTX_OK;
        }
        /* Direct IPU execution has a fixed, already-running diagnostic clock. */
        *value = s->ipu1_clkctrl | (s->ipu1_firmware ? 0 : 0x70000);
        return MEMTX_OK;
    case 0x4a005500:
        if (s->ipu1_native) {
            *value = s->ipu1_clkstctrl |
                     (rcc_ipu1_clock_active(s) ? BIT(8) : 0);
            return MEMTX_OK;
        }
        *value = s->ipu1_clkstctrl | (s->ipu1_firmware ? BIT(8) : 0);
        return MEMTX_OK;
    case 0x4a005540:
        *value = rcc_ipu_per_clock_status(s);
        return MEMTX_OK;
    case 0x4ae06510:
    case 0x4ae06514:
        if (!s->ipu1_native) {
            break;
        }
        *value = offset + 0x40000000 == 0x4ae06510 ?
                 s->ipu1_reset_control : s->ipu1_reset_status;
        return MEMTX_OK;
    case 0x4ae06910:
    case 0x4ae06914:
        if (!s->ipu2_native) {
            break;
        }
        *value = offset + 0x40000000 == 0x4ae06910 ?
                 s->ipu2_reset_control : s->ipu2_reset_status;
        return MEMTX_OK;
    case 0x4a008900:
        if (!s->ipu2_native) {
            break;
        }
        *value = s->ipu2_clkstctrl |
                 (s->ipu2_clock_running ? BIT(8) : 0);
        return MEMTX_OK;
    case 0x4a008920:
        if (!s->ipu2_native) {
            break;
        }
        *value = s->ipu2_clkctrl | (s->ipu2_clock_running ? 0 : 0x70000);
        return MEMTX_OK;
    case 0x4a002698:
        *value = s->mlb_l3_pressure;
        return MEMTX_OK;
    case 0x4a0026d0:
        *value = s->mlb_clk_bg_ctrl;
        return MEMTX_OK;
    case 0x4a0026c8:
    case 0x4a0026cc:
        *value = s->mlb_io_ctrl[(offset + 0x40000000 - 0x4a0026c8) / 4];
        return MEMTX_OK;
    case 0x4a002798:
        *value = s->mlb_dll;
        return MEMTX_OK;
    case 0x4a00279c:
        *value = s->mlb_clock;
        return MEMTX_OK;
    case 0x4842c4c0:
    case 0x4842c4c4:
    case 0x4842c4c8:
    case 0x4842c4cc:
        *value = s->mlb_mdat[(offset + 0x40000000 - 0x4842c4c0) / 4];
        return MEMTX_OK;
    case 0x4842c4d0:
    case 0x4842c4d4:
    case 0x4842c4d8:
    case 0x4842c4dc:
        *value = s->mlb_mdwe[(offset + 0x40000000 - 0x4842c4d0) / 4];
        return MEMTX_OK;
    case 0x4842c4e0:
        *value = s->mlb_mctl;
        return MEMTX_OK;
    case 0x4842c4e4:
        *value = s->mlb_madr;
        return MEMTX_OK;
    case 0x4ae07310:
        *value = s->pcie_reset_control;
        return MEMTX_OK;
    case 0x4ae07314:
        *value = s->pcie_reset_status;
        return MEMTX_OK;
    case 0x4a003c40:
        *value = s->pcie_phy_power;
        return MEMTX_OK;
    case 0x4a003c34:
        *value = s->pcie_pcs_delay;
        return MEMTX_OK;
    case 0x4a003c14:
        *value = s->pcie_acs_control;
        return MEMTX_OK;
    case 0x4a0093a0:
        *value = s->pcie_clkstctrl;
        return MEMTX_OK;
    case 0x4a009358:
        /* No MLB transactions are modeled, so the module stays in standby. */
        *value = s->mlb_clkctrl | BIT(18) |
                 (s->mlb_clkctrl ? 0 : 0x30000);
        return MEMTX_OK;
    case 0x4a008c00:
        *value = s->atl_clkctrl | (s->atl_clkctrl ? 0 : 0x30000);
        return MEMTX_OK;
    case 0x4a008c20:
        *value = s->atl_clkstctrl |
                 ((s->atl_clkctrl || s->atl_clkstctrl == 2) ? BIT(8) : 0) |
                 (s->atl_clkctrl ? BIT(9) : 0);
        return MEMTX_OK;
    case 0x4a08481c:
    case 0x4a084824:
        qemu_log_mask(LOG_UNIMP, "mh2p-rcc: reserved PIPE3 TX read at 0x%"
                      HWADDR_PRIx " (zero)\n", offset + 0x40000000);
        *value = 0;
        return MEMTX_OK;
    case 0x4a080014:
    case 0x4a090014:
        *value = 1; /* Synchronous OCP2SCP reset completion. */
        return MEMTX_OK;
    case 0x4a084c04:
        *value = 1 | ((s->pipe3_active_valid &&
                       !(s->pipe3_regs[RCC_PIPE3_CFG2] & 1)) ? 2 : 0);
        return MEMTX_OK;
    case 0x4a084c08:
        *value = 0; /* Supported PLL_GO requests complete synchronously. */
        return MEMTX_OK;
    case 0x4a0086c0:
        *value = s->usb_60m_clkctrl;
        return MEMTX_OK;
    case 0x4a002e74:
        *value = s->usb2_phy_power;
        return MEMTX_OK;
    case 0x4a002e00:
        if (!(s->pbias_control & (BIT(27) | BIT(25)))) {
            *value = s->pbias_control;
            return MEMTX_OK;
        }
        /* Active voltage/error status needs a connected supply model. */
        break;
    case 0x4ae06110:
        /* CM_CLKSEL_SYS: board fixture uses a 20 MHz SYS_CLKIN1. */
        *value = 2;
        return MEMTX_OK;
    case 0x4a005160:
    case 0x4a005164:
    case 0x4a005168:
        if (size != 4) {
            break;
        }
        switch (offset + 0x40000000) {
        case 0x4a005160:
            *value = s->mpu_dpll_mode;
            break;
        case 0x4a005164:
            *value = BIT(4) | (s->mpu_dpll_mode == 7 ?
                              1 : s->mpu_dpll_mode << 1);
            break;
        default:
            *value = 0; /* Automatic clock gating is not modeled. */
            break;
        }
        return MEMTX_OK;
    case 0x4a00516c:
        *value = (500 << 8) | 9;
        return MEMTX_OK;
    case 0x4a005170:
        *value = 1;
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP, "mh2p-rcc: unsupported read at 0x%" HWADDR_PRIx
                  " size=%u\n", offset + 0x40000000, size);
    *value = 0;
    return MEMTX_ERROR;
}

static MemTxResult rcc_unknown_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size,
                                    MemTxAttrs attrs)
{
    MH2PRCCMachineState *s = opaque;

    if (size == 4 && offset + 0x40000000 == 0x4a002554 &&
        !(value & 0x110000) && (value & 3) != 3 &&
        ((value >> 4) & 3) != 3) {
        s->control_io1 = value & 0x3333;
        return MEMTX_OK;
    }
    for (unsigned i = 0;
         s->power_clocks && i < ARRAY_SIZE(rcc_bypass_dplls); i++) {
        hwaddr reg = offset + 0x40000000 - rcc_bypass_dplls[i].base;

        if (size == 4 && reg == 0 &&
            (value == 5 || (value == 6 && rcc_bypass_dplls[i].fast_relock))) {
            s->bypass_dpll_mode[i] = value;
            return MEMTX_OK;
        }
        if (size == 4 && reg == 8 && value == 0) {
            return MEMTX_OK;
        }
    }
    for (unsigned i = 0;
         s->power_clocks && i < ARRAY_SIZE(rcc_disabled_clkctrl); i++) {
        uint32_t reset = rcc_disabled_clkctrl[i].reset;
        uint32_t status = reset & 0x70000;
        uint64_t control = value & ~(uint64_t)status;

        if (size == 4 && offset + 0x40000000 ==
            rcc_disabled_clkctrl[i].address &&
            (control == 0 || control == (reset & ~status))) {
            /* Only the disabled reset clock selections are modeled. */
            s->disabled_clkctrl[i] = control | status;
            return MEMTX_OK;
        }
    }
    for (unsigned i = 0;
         s->power_clocks && i < ARRAY_SIZE(rcc_mcasp_gblctl); i++) {
        if (size == 4 && offset + 0x40000000 == rcc_mcasp_gblctl[i] &&
            value == 0) {
            /* Only asserting every section reset is supported. */
            return MEMTX_OK;
        }
    }
    if (size == 4 && offset + 0x40000000 == 0x4a005160 &&
        (value == 5 || value == 6 || value == 7)) {
        uint64_t old_period = rcc_mpu_wdt_period(s);

        /* Synchronous bypass/relock of the fixed 20 MHz * 500 / 10 handoff. */
        rcc_mpu_wdt_sync(s);
        s->mpu_dpll_mode = value;
        s->mpu_wdt_phase = muldiv64(s->mpu_wdt_phase,
                                   rcc_mpu_wdt_period(s), old_period);
        rcc_mpu_wdt_arm(s);
        return MEMTX_OK;
    }
    if (size == 4 && offset + 0x40000000 == 0x4a005168 && value == 0) {
        return MEMTX_OK;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(rcc_ddr_registers); i++) {
        if (size == 4 && offset + 0x40000000 ==
            rcc_ddr_registers[i].address) {
            uint32_t mask = rcc_ddr_registers[i].mask;

            if (i < 4) {
                for (unsigned shift = 5; shift < 32; shift += 8) {
                    if (((mask >> shift) & 7) &&
                        ((value >> shift) & 7) > 4) {
                        qemu_log_mask(LOG_UNIMP,
                                      "mh2p-rcc: unsupported DDR impedance "
                                      "at 0x%" HWADDR_PRIx
                                      " value=0x%" PRIx64 "\n",
                                      offset + 0x40000000, value);
                        return MEMTX_ERROR;
                    }
                }
            }
            if ((value & ~mask) != (rcc_ddr_registers[i].reset & ~mask)) {
                qemu_log_mask(LOG_UNIMP,
                              "mh2p-rcc: ignoring read-only DDR control bits "
                              "at 0x%" HWADDR_PRIx " value=0x%" PRIx64 "\n",
                              offset + 0x40000000, value);
            }
            s->ddr_regs[i] = (value & mask) |
                            (rcc_ddr_registers[i].reset & ~mask);
            return MEMTX_OK;
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(s->dcan); i++) {
        uint32_t mask = i ? 0x30002 : 0x1030002;

        if (size == 4 && offset + 0x40000000 == rcc_dcan_clocks[i] &&
            !(value & ~mask)) {
            if ((value & BIT(24)) && !(s->dcan[i].control & 1)) {
                qemu_log_mask(LOG_UNIMP,
                              "mh2p-rcc: unsupported active DCAN SYS_CLK2\n");
                return MEMTX_ERROR;
            }
            s->dcan[i].clock_control = value & 0x1000002;
            memory_region_set_enabled(&s->dcan[i].registers, value & 2);
            rcc_dcan_interrupts(&s->dcan[i]);
            rcc_dcan_schedule(&s->dcan[i]);
            return MEMTX_OK;
        }
    }
    if (size == 4 && offset + 0x40000000 == 0x4a002544 &&
        (value == 0xfdf45530 || value == 0xf757fdc0)) {
        s->mmr_lock2 = value;
        return MEMTX_OK;
    }
    if (size == 4 &&
        (offset + 0x40000000 == 0x4a008778 ||
         offset + 0x40000000 == 0x4a008780) && value == 1) {
        /* Existing synchronous EDMA fixture has fixed enabled TC clocks. */
        return MEMTX_OK;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(rcc_gpio_clocks); i++) {
        if (size == 4 && offset + 0x40000000 == rcc_gpio_clocks[i] &&
            !(value & ~0x30101U)) {
            s->gpio_clkctrl[i] = value & 0x101;
            memory_region_set_enabled(
                sysbus_mmio_get_region(SYS_BUS_DEVICE(s->gpio[i]), 0),
                value & 1);
            return MEMTX_OK;
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(rcc_gp_timers); i++) {
        if (size == 4 && offset + 0x40000000 == rcc_gp_timers[i].clock &&
            s->gp_timer[i].number != 12 &&
            !(value & ~0x01030002U)) {
            rcc_gp_timer_sync(&s->gp_timer[i]);
            s->gp_timer[i].clock_control = value & 0x01000002;
            rcc_gp_timer_arm(&s->gp_timer[i]);
            rcc_ipu_per_clock_status(s);
            return MEMTX_OK;
        }
    }
    if (size == 4 && s->ipu1_native &&
        offset + 0x40000000 == 0x4ae06510 && !(value & ~7U)) {
        s->ipu1_reset_control = value;
        rcc_ipu1_control(s);
        return MEMTX_OK;
    }
    if (size == 4 && s->ipu2_native) {
        switch (offset + 0x40000000) {
        case 0x4ae06910:
            if (value & ~7U) {
                break;
            }
            s->ipu2_reset_control = value;
            rcc_ipu2_control(s);
            return MEMTX_OK;
        case 0x4ae06914:
            if (value & ~0x7fU) {
                break;
            }
            s->ipu2_reset_status &= ~value;
            return MEMTX_OK;
        case 0x4a008900:
            if (value & ~0x103U) {
                break;
            }
            s->ipu2_clkstctrl = value & 3;
            rcc_ipu2_control(s);
            return MEMTX_OK;
        case 0x4a008920:
            if (value & ~0x70001U) {
                break;
            }
            s->ipu2_clkctrl = value & 1;
            rcc_ipu2_control(s);
            return MEMTX_OK;
        }
    }
    if (size == 4 && s->ipu1_native &&
        offset + 0x40000000 == 0x4ae06514 && !(value & ~0x7fU)) {
        s->ipu1_reset_status &= ~value;
        return MEMTX_OK;
    }
    if (size == 4 && s->ipu1_native &&
        offset + 0x40000000 == 0x4a005520 &&
        !(value & ~0x1070001U) &&
        (!(value & 1) || (value & BIT(24)))) {
        s->ipu1_clkctrl = value & 0x1000001;
        rcc_ipu1_control(s);
        return MEMTX_OK;
    }
    if (size == 4 && s->ipu1_native &&
        offset + 0x40000000 == 0x4a005500 && !(value & ~0x103U)) {
        s->ipu1_clkstctrl = value & 3;
        rcc_ipu1_control(s);
        return MEMTX_OK;
    }
    if (size == 4 && offset + 0x40000000 == 0x4a005520 &&
        (value == 1 || value == 0x1000001) && s->ipu1_firmware) {
        s->ipu1_clkctrl = value;
        return MEMTX_OK;
    }
    if (size == 4 && offset + 0x40000000 == 0x4a005500 &&
        (value == 0 || value == 2) && s->ipu1_firmware) {
        /* Only modes keeping the fixed diagnostic CPU clock awake. */
        s->ipu1_clkstctrl = value;
        return MEMTX_OK;
    }
    if (size == 4 && offset + 0x40000000 == 0x4a005540 &&
        !(value & ~0x77f03U)) {
        rcc_ipu_per_clock_status(s);
        s->ipu_per_clkstctrl = value & 3;
        rcc_ipu_per_clock_status(s);
        return MEMTX_OK;
    }
    if (size == 4 && offset + 0x40000000 == 0x4ae07310 &&
        (value == 3 || (value == 2 && (s->pcie_clkctrl[2] & 3) == 2))) {
        if (value & 1) {
            rcc_pcie_controller_reset(&s->pcie);
        }
        if ((s->pcie_reset_control & 1) && !(value & 1)) {
            s->pcie_reset_status |= 1;
        }
        s->pcie_reset_control = value;
        return MEMTX_OK;
    }
    if (size == 4 && offset + 0x40000000 == 0x4ae07314 &&
        !(value & ~1)) {
        s->pcie_reset_status &= ~value;
        return MEMTX_OK;
    }
    if (size == 4 && offset + 0x40000000 == 0x4a003c40 &&
        !(value & ~0xffffc000U) && ((value >> 14) & 0xff) <= 3 &&
        (!(value & 0xc000) || (value >> 22) == 20)) {
        s->pcie_phy_power = value;
        return MEMTX_OK;
    }
    if (size == 4 && offset + 0x40000000 == 0x4a003c34 &&
        !(value & ~0xff0000U)) {
        s->pcie_pcs_delay = value;
        return MEMTX_OK;
    }
    if (size == 4 && offset + 0x40000000 == 0x4a003c14 &&
        (value == 0 || value == 0x20000)) {
        s->pcie_acs_control = value;
        return MEMTX_OK;
    }
    if (size == 4 && offset + 0x40000000 == 0x4a0093a0 &&
        !(value & ~3) && value != 1) {
        s->pcie_clkstctrl = value;
        return MEMTX_OK;
    }
    if (size == 4 && offset + 0x40000000 == 0x4a009358 &&
        !(value & ~0x70003U) && ((value & 3) == 0 || (value & 3) == 2)) {
        s->mlb_clkctrl = value & 3;
        return MEMTX_OK;
    }
    if (size == 4 && offset + 0x40000000 == 0x4a002698 &&
        !(value & ~7U)) {
        s->mlb_l3_pressure = value;
        return MEMTX_OK;
    }
    if (size == 4 && offset + 0x40000000 == 0x4a0026d0) {
        if (value & ~0x100ffU) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "mh2p-rcc: ignoring reserved MLB clock/bandgap "
                          "bits 0x%" PRIx64 "\n", value & ~0x100ffU);
        }
        s->mlb_clk_bg_ctrl = value & 0x100ff;
        return MEMTX_OK;
    }
    if (size == 4 && (offset + 0x40000000 == 0x4a0026c8 ||
                      offset + 0x40000000 == 0x4a0026cc)) {
        if (value & ~0x3f3f78U) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "mh2p-rcc: ignoring reserved MLB signal/data "
                          "IO bits 0x%" PRIx64 "\n", value & ~0x3f3f78U);
        }
        s->mlb_io_ctrl[(offset + 0x40000000 - 0x4a0026c8) / 4] =
            value & 0x3f3f78;
        return MEMTX_OK;
    }
    if (size == 4 && (offset + 0x40000000 == 0x4a002798 ||
                      offset + 0x40000000 == 0x4a00279c)) {
        /* DRA72x documents both lock fields as RW; no lock is inferred. */
        bool dll = offset + 0x40000000 == 0x4a002798;
        uint32_t mask = dll ? 0x7ff : 1;

        if (value & ~mask) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "mh2p-rcc: ignoring reserved MLB %s bits "
                          "0x%" PRIx64 "\n", dll ? "DLL" : "clock",
                          value & ~mask);
        }
        if (dll) {
            s->mlb_dll = value & mask;
        } else {
            s->mlb_clock = value & mask;
        }
        return MEMTX_OK;
    }
    if (size == 4 && offset + 0x40000000 >= 0x4842c4c0 &&
        offset + 0x40000000 <= 0x4842c4dc && !(offset & 3)) {
        unsigned word = (offset + 0x40000000 - 0x4842c4c0) / 4;

        if (word < 4) {
            s->mlb_mdat[word] = value;
        } else {
            s->mlb_mdwe[word - 4] = value;
        }
        return MEMTX_OK;
    }
    if (size == 4 && (offset + 0x40000000 == 0x4842c4e0 ||
                     offset + 0x40000000 == 0x4842c4e4)) {
        bool control = offset + 0x40000000 == 0x4842c4e0;
        uint32_t mask = control ? 1 : 0x800000ffU;

        if (value & ~mask) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "mh2p-rcc: ignoring reserved MLB memory %s bits "
                          "0x%" PRIx64 "\n", control ? "control" : "address",
                          value & ~mask);
        }
        if (control) {
            s->mlb_mctl &= value & 1;
        } else {
            uint32_t *entry = s->mlb_channel_table[value & 0xff];

            s->mlb_madr = value & mask;
            for (unsigned i = 0; i < 4; i++) {
                if (value & BIT(31)) {
                    entry[i] = (entry[i] & ~s->mlb_mdwe[i]) |
                               (s->mlb_mdat[i] & s->mlb_mdwe[i]);
                } else {
                    s->mlb_mdat[i] = entry[i];
                }
            }
            /* A completed internal RAM access does not imply a link or DMA. */
            s->mlb_mctl = 1;
        }
        return MEMTX_OK;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(rcc_mlb_registers); i++) {
        if (size == 4 && offset + 0x40000000 ==
            rcc_mlb_registers[i].address) {
            uint32_t rw = rcc_mlb_registers[i].rw;
            uint32_t w0c = rcc_mlb_registers[i].w0c;
            uint32_t w1c = rcc_mlb_registers[i].w1c;

            if (value & ~(rw | w0c | w1c)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "mh2p-rcc: ignoring reserved/read-only MLB "
                              "register 0x%" HWADDR_PRIx " bits 0x%" PRIx64 "\n",
                              offset + 0x40000000,
                              value & ~(rw | w0c | w1c));
            }
            if (offset + 0x40000000 == 0x4842c400 &&
                (value & 1) && !(s->mlb_regs[i] & 1)) {
                qemu_log_mask(LOG_UNIMP,
                              "mh2p-rcc: MLB link/DMA transfers are not "
                              "implemented; no external lock or completion\n");
            }
            s->mlb_regs[i] = (value & rw) |
                            (s->mlb_regs[i] & value & w0c) |
                            (s->mlb_regs[i] & ~value & w1c);
            return MEMTX_OK;
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(rcc_pcie_clkctrl); i++) {
        if (size == 4 && offset + 0x40000000 == rcc_pcie_clkctrl[i].address &&
            !(value & ~(rcc_pcie_clkctrl[i].mask | 0x30000))) {
            s->pcie_clkctrl[i] = value & rcc_pcie_clkctrl[i].mask;
            return MEMTX_OK;
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(rcc_peripheral_clkctrl); i++) {
        if (size == 4 && offset + 0x40000000 == rcc_peripheral_clkctrl[i] &&
            !(value & ~0x30002)) {
            for (unsigned j = 0; j < ARRAY_SIZE(s->i2c); j++) {
                if (rcc_peripheral_clkctrl[i] == rcc_i2cs[j].clock &&
                    !dra7_i2c_set_clock(s->i2c[j], value & 2)) {
                    goto unsupported;
                }
            }
            for (unsigned j = 0; j < ARRAY_SIZE(rcc_uarts); j++) {
                RCCUART *uart = &s->uart[j];

                if (rcc_peripheral_clkctrl[i] != rcc_uarts[j].clock) {
                    continue;
                }
                if (!(value & 2) && (uart->core->serial.lsr & 0x60) != 0x60) {
                    goto unsupported; /* Cannot pause an active transmitter. */
                }
                rcc_uart_set_clock(uart, value & 2);
            }
            for (unsigned j = 0; j < ARRAY_SIZE(s->spi); j++) {
                if (rcc_peripheral_clkctrl[i] == rcc_spis[j].clock) {
                    dra7_mcspi_set_clock(s->spi[j], value & 2);
                }
            }
            s->peripheral_clkctrl[i] = value & 2;
            rcc_ipu_per_clock_status(s);
            return MEMTX_OK;
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(rcc_peripheral_context); i++) {
        if (size == 4 && offset + 0x40000000 ==
            rcc_peripheral_context[i].address) {
            if (value & ~rcc_peripheral_context[i].mask) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "mh2p-rcc: ignoring read-only context bits at "
                              "0x%" HWADDR_PRIx " value=0x%" PRIx64 "\n",
                              rcc_peripheral_context[i].address,
                              value & ~rcc_peripheral_context[i].mask);
            }
            s->peripheral_context[i] &= ~value;
            return MEMTX_OK;
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(rcc_pads); i++) {
        uint32_t mask = rcc_pads[i].write_mask ?
                        rcc_pads[i].write_mask : 0x010f01ff;

        if (size == 4 && offset + 0x40000000 == rcc_pads[i].address) {
            /* MUXMODE is RW even when no signal is assigned to its encoding. */
            if (!(rcc_pads[i].mux_modes & BIT(value & mask & 0xf))) {
                qemu_log_mask(LOG_UNIMP,
                              "mh2p-rcc: reserved pad mux mode at "
                              "0x%" HWADDR_PRIx " value=0x%" PRIx64 "\n",
                              rcc_pads[i].address, value);
            }
            if (value & ~mask) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "mh2p-rcc: ignoring read-only pad bits at "
                              "0x%" HWADDR_PRIx " value=0x%" PRIx64 "\n",
                              rcc_pads[i].address, value & ~mask);
            }
            s->pad_config[i] = value & mask;
            return MEMTX_OK;
        }
    }
    if (size == 4 && offset + 0x40000000 == 0x4a008c00 &&
        !(value & ~0x30002)) {
        s->atl_clkctrl = value & 2;
        return MEMTX_OK;
    }
    if (size == 4 && offset + 0x40000000 == 0x4a008c20 &&
        !(value & ~0x303) && (value & 3) != 1) {
        s->atl_clkstctrl = value & 3;
        return MEMTX_OK;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(rcc_pipe3_registers); i++) {
        uint32_t mask = rcc_pipe3_registers[i].mask;

        if (size == 4 && offset + 0x40000000 ==
            rcc_pipe3_registers[i].address &&
            (value & ~mask) == (rcc_pipe3_registers[i].reset & ~mask)) {
            if ((i == RCC_SCP_SYSCONFIG || i == RCC_SCP3_SYSCONFIG) &&
                (value & 2)) {
                s->pipe3_regs[i] = 0x11;
                s->pipe3_regs[i + 1] = 1;
            } else {
                s->pipe3_regs[i] = value;
            }
            return MEMTX_OK;
        }
    }
    if (size == 4 && (offset + 0x40000000 == 0x4a08481c ||
                      offset + 0x40000000 == 0x4a084824)) {
        qemu_log_mask(LOG_UNIMP, "mh2p-rcc: reserved PIPE3 TX write at 0x%"
                      HWADDR_PRIx " = 0x%" PRIx64 " (ignored)\n",
                      offset + 0x40000000, value);
        return MEMTX_OK;
    }
    if (size == 4 && offset + 0x40000000 == 0x4a084c08 &&
        (value == 0 || (value == 1 && rcc_pipe3_can_lock(s)))) {
        if (value == 1) {
            s->pipe3_active_valid = true;
        }
        return MEMTX_OK;
    }
    if (size == 4 && offset + 0x40000000 == 0x4a0086c0 &&
        !(value & ~BIT(8))) {
        s->usb_60m_clkctrl = value;
        return MEMTX_OK;
    }
    if (size == 4 && offset + 0x40000000 == 0x4a002e74 &&
        !(value & ~BIT(28))) {
        s->usb2_phy_power = value;
        return MEMTX_OK;
    }
    if (size == 4 && offset + 0x40000000 == 0x4a002e00) {
        const uint32_t writable = BIT(27) | BIT(26) | BIT(25) | BIT(21);

        if (value & ~writable) {
            qemu_log_mask(LOG_UNIMP,
                          "mh2p-rcc: ignoring read-only PBIAS bits "
                          "value=0x%" PRIx64 "\n", value);
        }
        s->pbias_control = value & writable;
        return MEMTX_OK;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(rcc_usb_clkctrl); i++) {
        if (size == 4 && offset + 0x40000000 == rcc_usb_clkctrl[i] &&
            !(value & ~0x101)) {
            s->usb_clkctrl[i] = value;
            if (i < ARRAY_SIZE(s->usb)) {
                memory_region_set_enabled(
                    sysbus_mmio_get_region(SYS_BUS_DEVICE(s->usb[i]), 0),
                    value & 1);
            }
            return MEMTX_OK;
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(rcc_usb_phy_clkctrl); i++) {
        if (size == 4 && offset + 0x40000000 == rcc_usb_phy_clkctrl[i] &&
            !(value & ~BIT(8))) {
            s->usb_phy_clkctrl[i] = value;
            return MEMTX_OK;
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(rcc_usb_phy_ana_config1); i++) {
        if (size == 4 && offset + 0x40000000 == rcc_usb_phy_ana_config1[i] &&
            !(value & ~BIT(31))) {
            s->usb_phy_ana_config1[i] = value;
            return MEMTX_OK;
        }
    }
unsupported:
    qemu_log_mask(LOG_UNIMP, "mh2p-rcc: unsupported write at 0x%" HWADDR_PRIx
                  " size=%u value=0x%" PRIx64 " cpu=%d pc=0x%" PRIx64 "\n",
                  offset + 0x40000000, size, value,
                  current_cpu ? current_cpu->cpu_index : -1,
                  current_cpu ?
                  CPU_GET_CLASS(current_cpu)->get_pc(current_cpu) : 0);
    return MEMTX_ERROR;
}

static const MemoryRegionOps rcc_unknown_ops = {
    .read_with_attrs = rcc_unknown_read,
    .write_with_attrs = rcc_unknown_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void rcc_reset(void *opaque)
{
    MH2PRCCMachineState *s = opaque;
    MemTxAttrs secure = { .secure = true };
    MemTxResult result;

    cpu_reset(CPU(s->cpu));
    arm_emulate_firmware_reset(CPU(s->cpu), 1);
    s->cpu->env.cp15.mvbar = RCC_MONITOR_BASE;
    arm_rebuild_hflags(&s->cpu->env);
    s->l2actlr = 0;
    s->pbias_control = BIT(21);
    for (unsigned i = 0; i < ARRAY_SIZE(rcc_ddr_registers); i++) {
        s->ddr_regs[i] = rcc_ddr_registers[i].reset;
    }
    rcc_gpmc_reset(s, s->entry != 0x40300000 && s->entry != 0x40340000);
    memset(s->spinlocks, 0, sizeof(s->spinlocks));
    for (unsigned i = 0; i < ARRAY_SIZE(s->spi); i++) {
        device_cold_reset(s->spi[i]);
        dra7_mcspi_set_clock(s->spi[i], false);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(s->i2c); i++) {
        device_cold_reset(s->i2c[i]);
        dra7_i2c_set_clock(s->i2c[i], false);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(s->dcan); i++) {
        memset(s->dcan[i].messages, 0, sizeof(s->dcan[i].messages));
        rcc_dcan_reset(&s->dcan[i]);
        /* The direct-start fixture permits the initial bootloader CTL read. */
        s->dcan[i].clock_control = 2;
        memory_region_set_enabled(&s->dcan[i].registers, true);
    }
    /*
     * This monitor owns no secure interrupt handlers. Hand all GIC interrupts
     * to nonsecure QNX without enabling them or changing guest priorities.
     */
    for (unsigned i = 0; i < (RCC_EXTERNAL_IRQS + 32) / 32; i++) {
        address_space_stl_le(&address_space_memory, RCC_GIC_BASE + 0x1080 +
                             i * 4, UINT32_MAX, secure, &result);
        assert(result == MEMTX_OK);
    }
    address_space_stl_le(&address_space_memory, RCC_GIC_BASE + 0x2004, 0xff,
                         secure, &result);
    assert(result == MEMTX_OK);
    s->iodelay_lock = 0xaaab;
    memset(s->pad_delay, 0, sizeof(s->pad_delay));
    memset(s->edma_map, 0, sizeof(s->edma_map));
    s->control_io1 = 0x3300;
    timer_del(s->gmac_mdio_timer);
    s->gmac_mdio_control = s->gmac_mdio_raw = 0;
    s->gmac_mdio_active = -1;
    s->gmac_mdio_next = 0;
    memset(s->gmac_mdio_user, 0, sizeof(s->gmac_mdio_user));
    memset(s->gmac_mdio_phy, 0, sizeof(s->gmac_mdio_phy));
    rcc_gmac_dma_reset(s);
    rcc_gmac_ale_reset(s);
    for (unsigned i = 0; i < ARRAY_SIZE(s->gmac_config); i++) {
        s->gmac_config[i].value = rcc_gmac_config[i].reset;
    }
    memset(s->edma_access, 0, sizeof(s->edma_access));
    memset(s->edma_qmap, 0, sizeof(s->edma_qmap));
    memset(s->edma_param, 0, sizeof(s->edma_param));
    memset(s->edma_ier, 0, sizeof(s->edma_ier));
    memset(s->edma_qeer, 0, sizeof(s->edma_qeer));
    memset(s->edma_ipr, 0, sizeof(s->edma_ipr));
    s->edma_epoch++;
    memset(s->edma_queue, 0, sizeof(s->edma_queue));
    s->edma_qemr = 0;
    rcc_edma_update_irq(s);
    memset(s->usb_clkctrl, 0, sizeof(s->usb_clkctrl));
    memset(s->usb_phy_clkctrl, 0, sizeof(s->usb_phy_clkctrl));
    memset(s->usb_phy_ana_config1, 0, sizeof(s->usb_phy_ana_config1));
    s->usb_60m_clkctrl = 0;
    s->usb2_phy_power = BIT(28);
    s->atl_clkctrl = 0;
    s->mlb_clkctrl = 0;
    s->mlb_l3_pressure = 0;
    s->mlb_clk_bg_ctrl = 1;
    s->mlb_io_ctrl[0] = s->mlb_io_ctrl[1] = 0x30;
    s->mlb_dll = s->mlb_clock = 0;
    memset(s->mlb_mdat, 0, sizeof(s->mlb_mdat));
    memset(s->mlb_mdwe, 0, sizeof(s->mlb_mdwe));
    memset(s->mlb_channel_table, 0, sizeof(s->mlb_channel_table));
    s->mlb_mctl = s->mlb_madr = 0;
    for (unsigned i = 0; i < ARRAY_SIZE(rcc_mlb_registers); i++) {
        s->mlb_regs[i] = rcc_mlb_registers[i].reset;
    }
    s->ipu1_clkctrl = s->ipu1_firmware ? 1 : 0;
    s->ipu1_clkstctrl = 3;
    s->ipu1_reset_control = s->ipu1_reset_asserted = 7;
    s->ipu1_reset_status = 0;
    s->ipu1_clock_running = false;
    if (s->ipu1_native && s->ipu1) {
        rcc_ipu1_control(s);
    }
    s->ipu2_clkctrl = 0;
    s->ipu2_clkstctrl = 3;
    s->ipu2_reset_control = s->ipu2_reset_asserted = 7;
    s->ipu2_reset_status = 0;
    s->ipu2_clock_running = false;
    if (s->ipu2) {
        rcc_ipu2_control(s);
    }
    s->ipu_per_clkstctrl = 3;
    s->ipu_per_clock_active = false;
    s->mmr_lock2 = 0xf757fdc0; /* Preserve unlocked direct-start handoff. */
    s->atl_clkstctrl = 3;
    memset(s->pcie_clkctrl, 0, sizeof(s->pcie_clkctrl));
    s->pcie_clkstctrl = 3;
    s->pcie_dpll_mode = 5;
    s->mpu_dpll_mode = 7;
    timer_del(s->mpu_watchdog_timer);
    s->mpu_wdt_load = s->mpu_wdt_warning = s->mpu_wdt_prescaler = 0;
    s->mpu_wdt_sampled_prescaler = s->mpu_wdt_control = 0;
    s->mpu_wdt_remaining = s->mpu_wdt_phase = 0;
    s->mpu_wdt_epoch = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->mpu_wdt_warned = s->mpu_wdt_expired = false;
    s->mpu_wdt_reset_requested = false;
    qemu_set_irq(s->crossbar.inputs[5], 0);
    qemu_set_irq(s->crossbar.inputs[139], 0);
    for (unsigned i = 0; i < ARRAY_SIZE(rcc_bypass_dplls); i++) {
        s->bypass_dpll_mode[i] = 5;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(rcc_disabled_clkctrl); i++) {
        s->disabled_clkctrl[i] = rcc_disabled_clkctrl[i].reset;
    }
    s->pcie_dpll_select = 0x04000000;
    s->pcie_dpll_divider = 1;
    s->pcie_dpll_initialized = false;
    s->pcie_apll_mode = 0;
    s->pcie_acs_control = 0;
    s->pcie_phy_power = 0;
    s->pcie_pcs_delay = 0;
    s->pcie_reset_control = 3;
    s->pcie_reset_status = 0;
    rcc_pcie_controller_reset(&s->pcie);
    for (unsigned i = 0; i < ARRAY_SIZE(rcc_pcie_rx_registers); i++) {
        s->pcie_rx_regs[i] = rcc_pcie_rx_registers[i].reset;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(rcc_peripheral_clkctrl); i++) {
        s->peripheral_clkctrl[i] = 0;
        for (unsigned j = 0; j < ARRAY_SIZE(rcc_uarts); j++) {
            if (rcc_peripheral_clkctrl[i] == rcc_uarts[j].clock) {
                s->peripheral_clkctrl[i] = 2;
            }
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(rcc_peripheral_context); i++) {
        /* This full board reset also resets the peripheral contexts. */
        s->peripheral_context[i] = rcc_peripheral_context[i].mask;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(rcc_pads); i++) {
        s->pad_config[i] = rcc_pads[i].reset;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(s->gpio); i++) {
        /* Preserve the direct-start bootloader's enabled GPIO fixture. */
        s->gpio_clkctrl[i] = 1;
        memory_region_set_enabled(
            sysbus_mmio_get_region(SYS_BUS_DEVICE(s->gpio[i]), 0), true);
    }
    memset(s->atl_config, 0, sizeof(s->atl_config));
    s->timer2_epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    for (unsigned i = 0; i < ARRAY_SIZE(s->gp_timer); i++) {
        s->gp_timer[i].clock_control = s->gp_timer[i].number == 12 ? 1 : 0;
        rcc_gp_timer_reset(&s->gp_timer[i]);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(rcc_pipe3_registers); i++) {
        s->pipe3_regs[i] = rcc_pipe3_registers[i].reset;
    }
    s->pipe3_active_valid = false;
    rcc_crossbar_reset(&s->crossbar);
    for (unsigned i = 0; i < ARRAY_SIZE(s->dsp_crossbar); i++) {
        rcc_crossbar_reset(&s->dsp_crossbar[i]);
    }
    rcc_crossbar_reset(&s->ipu_crossbar);
    if (s->ipu2_native) {
        rcc_crossbar_reset(&s->ipu2_crossbar);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(s->usb); i++) {
        memory_region_set_enabled(
            sysbus_mmio_get_region(SYS_BUS_DEVICE(s->usb[i]), 0), false);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(s->uart); i++) {
        s->uart[i].mdr1 = 7;
        s->uart[i].sysconfig = 0;
        rcc_uart_set_clock(&s->uart[i], true);
    }
    s->watchdog_stop = 0;
    s->dma_sysconfig = 0;
    s->dma_gcr = 0x10010;
    memset(s->dma_irqenable, 0, sizeof(s->dma_irqenable));
    memset(s->dma_channel, 0, sizeof(s->dma_channel));
    memset(s->dma_index, 0, sizeof(s->dma_index));
    cpu_set_pc(CPU(s->cpu), s->entry);
}

static void rcc_init_done(Notifier *notifier, void *opaque)
{
    MH2PRCCMachineState *s = container_of(notifier, MH2PRCCMachineState,
                                         init_done);

    /* Apply bootloader handoff state after the sysbus devices reset. */
    qemu_register_reset(rcc_reset, s);
}

static void rcc_init(MachineState *ms)
{
    MH2PRCCMachineState *s = (MH2PRCCMachineState *)ms;
    MemoryRegion *sysmem = get_system_memory();
    DeviceState *gic, *mailbox, *ipu_mmu;
    DriveInfo *nor_drive = drive_get(IF_PFLASH, 0, 0);
    PFlashCFI02 *nor;
    g_autofree char *image = NULL;
    g_autofree char *ipl = NULL;
    g_autoptr(GError) error = NULL;
    size_t size;
    uint32_t entry, load, startup_size;
    static const hwaddr gpio_base[] = {
        0x4ae10000, 0x48055000, 0x48057000, 0x48059000,
        0x4805b000, 0x4805d000, 0x48051000, 0x48053000,
    };
    uint32_t monitor[] = {
        0xeafffffe, /* Unexpected monitor vectors stop at their entry. */
        0xeafffffe,
        0xea00000c, /* SMC vector: b monitor + 0x40 */
        0xeafffffe,
        0xeafffffe,
        0xeafffffe,
        0xeafffffe,
        0xeafffffe,
        0, 0, 0, 0, 0, 0, 0, 0,
        0xe35c0f41, /* cmp ip, #0x104: TI L2ACTLR_SET */
        0x1a000004, /* bne unknown */
        0xee2f0f10, /* mcr p15, 1, r0, c15, c0, 0 */
        0xf57ff04f, /* dsb sy */
        0xf57ff06f, /* isb sy */
        0xe3a00000, /* mov r0, #0 */
        0xe1b0f00e, /* movs pc, lr */
        0xe3e00000, /* unknown: mvn r0, #0 (TI unknown-service error) */
        0xe1b0f00e, /* movs pc, lr */
    };

    if (!ms->kernel_filename ||
        !g_file_get_contents(ms->kernel_filename, &image, &size, &error)) {
        error_report("mh2p-rcc requires -kernel <original RCC QNX IFS>: %s",
                     error ? error->message : "no image specified");
        exit(EXIT_FAILURE);
    }
    if (ms->ram_size != 256 * MiB) {
        error_report("mh2p-rcc requires the RCC2P board's 256 MiB RAM");
        exit(EXIT_FAILURE);
    }
    if (size < 0x108 || size > 32 * MiB) {
        error_report("mh2p-rcc: unsupported startup image size %zu", size);
        exit(EXIT_FAILURE);
    }
    entry = ldl_le_p(image + 20);
    load = ldl_le_p(image + 28);
    startup_size = ldl_le_p(image + 40);
    if (ldl_le_p(image + 8) != 0x00ff7eeb ||
        lduw_le_p(image + 12) != 1 ||
        lduw_le_p(image + 16) != 0x100 ||
        lduw_le_p(image + 18) != 40 ||
        load != RCC_IMAGE_BASE + 8 ||
        ldl_le_p(image + 44) != size - 8 ||
        startup_size < 0x100 || startup_size > size - 8 ||
        (entry & 3) ||
        entry < load ||
        entry >= (uint64_t)load + startup_size) {
        error_report("mh2p-rcc: unsupported QNX startup header or load address "
                     "(size=%zu)", size);
        error_report("magic=%08x machine=%u load=%08x entry=%08x",
                     ldl_le_p(image + 8), lduw_le_p(image + 18),
                     ldl_le_p(image + 28), ldl_le_p(image + 20));
        exit(EXIT_FAILURE);
    }
    s->entry = entry;
    if (ms->firmware) {
        size_t ipl_size;

        if (!g_file_get_contents(ms->firmware, &ipl, &ipl_size, &error)) {
            error_report("mh2p-rcc: cannot read IPL: %s", error->message);
            exit(EXIT_FAILURE);
        }
        if (ipl_size != 128 * KiB ||
            !((ldl_le_p(ipl) == 0xfa001061 &&
               ldl_le_p(ipl + 4) == 0xeafffffd) ||
              (ldl_le_p(ipl) == 0xe59f8078 &&
               ldl_le_p(ipl + 4) == 0xe5880000))) {
            error_report("mh2p-rcc: unsupported RCC IPL size or entry");
            exit(EXIT_FAILURE);
        }
        s->entry = ldl_le_p(ipl) == 0xe59f8078 ? 0x40300000 : 0x40340000;
        rom_add_blob_fixed("mh2p-rcc-ipl", ipl, ipl_size, s->entry);
    }
    memory_region_add_subregion(sysmem, RCC_RAM_BASE, ms->ram);
    /* Explicit 64 MiB AMD CFI fixture, not a recovered board chip identity. */
    nor = pflash_cfi02_register(0x08000000, "mh2p-rcc-nor", 64 * MiB,
                                nor_drive ?
                                blk_by_legacy_dinfo(nor_drive) : NULL,
                                128 * KiB, 1, 2,
                                0x0001, 0x227e, 0x2223, 0x2201,
                                0x555, 0x2aa, 0);
    if (!nor_drive) {
        MemoryRegion *memory = sysbus_mmio_get_region(SYS_BUS_DEVICE(nor), 0);

        memset(memory_region_get_ram_ptr(memory), 0xff, 64 * MiB);
    }
    memory_region_del_subregion(
        sysmem, sysbus_mmio_get_region(SYS_BUS_DEVICE(nor), 0));
    memory_region_init_alias(&s->nor_window, OBJECT(ms), "mh2p-rcc-nor-window",
                            sysbus_mmio_get_region(SYS_BUS_DEVICE(nor), 0),
                            0, 64 * MiB);
    memory_region_add_subregion(sysmem, 0x08000000, &s->nor_window);
    memory_region_init_io(&s->gpmc, OBJECT(ms), &rcc_gpmc_ops, s,
                          "mh2p-rcc-gpmc", 0x1000);
    memory_region_add_subregion(sysmem, 0x50000000, &s->gpmc);
    memory_region_init_ram(&s->ocmc_ram, NULL, "mh2p-rcc-ocmc-ram1",
                           512 * KiB, &error_fatal);
    memory_region_add_subregion(sysmem, 0x40300000, &s->ocmc_ram);
    memory_region_init_io(&s->peripherals, OBJECT(ms), &rcc_unknown_ops, s,
                          "mh2p-rcc-unimplemented", 0x40000000);
    memory_region_add_subregion_overlap(sysmem, 0x40000000,
                                        &s->peripherals, -1);
    memory_region_init_rom(&s->monitor, NULL, "mh2p-rcc-monitor",
                           0x1000, &error_fatal);
    memory_region_add_subregion(sysmem, RCC_MONITOR_BASE, &s->monitor);
    for (unsigned i = 0; i < ARRAY_SIZE(monitor); i++) {
        monitor[i] = cpu_to_le32(monitor[i]);
    }
    rom_add_blob_fixed("mh2p-rcc-monitor-code", monitor, sizeof(monitor),
                       RCC_MONITOR_BASE);
    memory_region_init_io(&s->watchdog, OBJECT(ms), &rcc_watchdog_ops, s,
                          "mh2p-rcc-watchdog-stop", 0x80);
    memory_region_add_subregion(sysmem, 0x4ae14000, &s->watchdog);
    memory_region_init_io(&s->dma, OBJECT(ms), &rcc_dma_ops, s,
                          "mh2p-rcc-sdma-config", 0x1000);
    memory_region_add_subregion(sysmem, 0x4a056000, &s->dma);
    memory_region_init_io(&s->iodelay, OBJECT(ms), &rcc_iodelay_ops, s,
                          "mh2p-rcc-iodelay", 0x1000);
    memory_region_add_subregion(sysmem, 0x4844a000, &s->iodelay);
    memory_region_init_io(&s->edma, OBJECT(ms), &rcc_edma_ops, s,
                          "mh2p-rcc-edma-config", 0x10000);
    memory_region_add_subregion(sysmem, 0x43300000, &s->edma);
    for (unsigned i = 0; i < ARRAY_SIZE(rcc_gmac_resets); i++) {
        g_autofree char *name = g_strdup_printf("mh2p-rcc-gmac-reset%u", i);

        memory_region_init_io(&s->gmac_reset[i], OBJECT(ms),
                              i == 1 ? &rcc_gmac_dma_reset_ops :
                              i == 4 ? &rcc_gmac_wrapper_reset_ops :
                                       &rcc_gmac_reset_ops,
                              i < 2 || i == 4 ? s : NULL, name, 4);
        memory_region_add_subregion_overlap(sysmem, rcc_gmac_resets[i],
                                            &s->gmac_reset[i], 1);
    }
    memory_region_init_ram(&s->gmac_cppi_ram, NULL,
                           "mh2p-rcc-gmac-cppi-ram", 0x2000, &error_fatal);
    memory_region_add_subregion(sysmem, 0x48486000, &s->gmac_cppi_ram);
    memory_region_init_io(&s->gmac_dma, OBJECT(ms), &rcc_gmac_dma_ops, s,
                          "mh2p-rcc-gmac-dma", 0x300);
    memory_region_add_subregion_overlap(sysmem, 0x48484800, &s->gmac_dma, 0);
    memory_region_init_io(&s->gmac_ale, OBJECT(ms), &rcc_gmac_ale_ops, s,
                          "mh2p-rcc-gmac-ale", 0x4c);
    memory_region_add_subregion(sysmem, 0x48484d00, &s->gmac_ale);
    s->gmac_mdio_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                    rcc_gmac_mdio_complete, s);
    memory_region_init_io(&s->gmac_mdio, OBJECT(ms), &rcc_gmac_mdio_ops, s,
                          "mh2p-rcc-gmac-mdio", 0x90);
    memory_region_add_subregion(sysmem, 0x48485000, &s->gmac_mdio);
    for (unsigned i = 0; i < ARRAY_SIZE(s->gmac_config); i++) {
        g_autofree char *name = g_strdup_printf("mh2p-rcc-gmac-config%u", i);
        RCCGMACConfig *r = &s->gmac_config[i];

        r->value = rcc_gmac_config[i].reset;
        r->mask = rcc_gmac_config[i].mask;
        memory_region_init_io(&r->registers, OBJECT(ms), &rcc_gmac_config_ops,
                              r, name, 4);
        memory_region_add_subregion(sysmem, rcc_gmac_config[i].address,
                                    &r->registers);
    }
    memory_region_init_io(&s->atl, OBJECT(ms), &rcc_atl_ops, s,
                          "mh2p-rcc-atl-config", 0x1000);
    memory_region_add_subregion(sysmem, 0x4843c000, &s->atl);
    memory_region_init_io(&s->timer2, OBJECT(ms), &rcc_timer2_ops, s,
                          "mh2p-rcc-timer2-handoff", 0x1000);
    memory_region_add_subregion(sysmem, 0x48032000, &s->timer2);
    memory_region_init_io(&s->spinlock, OBJECT(ms), &rcc_spinlock_ops, s,
                          "mh2p-rcc-spinlock", 0x1000);
    memory_region_add_subregion(sysmem, 0x4a0f6000, &s->spinlock);
    for (unsigned i = 0; i < ARRAY_SIZE(s->dcan); i++) {
        g_autofree char *name = g_strdup_printf("mh2p-rcc-dcan%u", i + 1);

        s->dcan[i].transmit_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                                rcc_dcan_transmit, &s->dcan[i]);
        s->dcan[i].bus_client.info = &rcc_dcan_bus_info;
        if (s->canbus[i]) {
            can_bus_insert_client(s->canbus[i], &s->dcan[i].bus_client);
        }
        memory_region_init_io(&s->dcan[i].registers, OBJECT(ms), &rcc_dcan_ops,
                              &s->dcan[i], name, 0x200);
        memory_region_add_subregion(sysmem, i ? 0x48480000 : 0x4ae3c000,
                                    &s->dcan[i].registers);
    }
    memory_region_init_io(&s->pcie_pll, OBJECT(ms), &rcc_pcie_pll_ops, s,
                          "mh2p-rcc-pcie-pll", 0x2c);
    memory_region_add_subregion(sysmem, 0x4a008200, &s->pcie_pll);
    memory_region_init_io(&s->pcie_rx, OBJECT(ms), &rcc_pcie_rx_ops, s,
                          "mh2p-rcc-pcie-rx-config", 0x40);
    memory_region_add_subregion(sysmem, 0x4a094000, &s->pcie_rx);
    memory_region_init_io(&s->pcie.registers, OBJECT(ms),
                          &rcc_pcie_controller_ops, s,
                          "mh2p-rcc-pcie-controller-config", 0x4000);
    memory_region_add_subregion(sysmem, 0x51000000, &s->pcie.registers);
    s->cpu = ARM_CPU(object_new(ms->cpu_type));
    object_property_add_child(OBJECT(ms), "cpu", OBJECT(s->cpu));
    qdev_prop_set_bit(DEVICE(s->cpu), "has_el3", true);
    qdev_prop_set_bit(DEVICE(s->cpu), "has_el2", false);
    qdev_prop_set_bit(DEVICE(s->cpu), "l2-axi-error-irq", true);
    qdev_prop_set_uint64(DEVICE(s->cpu), "reset-cbar", RCC_GIC_BASE);
    qdev_prop_set_uint64(DEVICE(s->cpu), "cntfrq", 20000000);
    qdev_realize(DEVICE(s->cpu), NULL, &error_fatal);
    define_arm_cp_regs(s->cpu, rcc_cpregs);
    object_unref(OBJECT(s->cpu));
    gic = qdev_new(TYPE_A15MPCORE_PRIV);
    object_property_add_child(OBJECT(ms), "interrupt-controller", OBJECT(gic));
    qdev_prop_set_uint32(gic, "num-cpu", 1);
    qdev_prop_set_uint32(gic, "num-irq", RCC_EXTERNAL_IRQS + 32);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(gic), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(gic), 0, RCC_GIC_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(gic), 0,
                       qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(gic), 1,
                       qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_FIQ));
    qdev_connect_gpio_out_named(DEVICE(s->cpu), "l2-axi-error", 0,
                               qdev_get_gpio_in(gic, 3));
    s->crossbar.selector_count = RCC_XBAR_SELECTORS;
    s->crossbar.mmr_lock = &s->mmr_lock2;
    s->crossbar.mpu = true;
    for (unsigned i = 0; i < ARRAY_SIZE(s->crossbar.outputs); i++) {
        s->crossbar.outputs[i] =
            qdev_get_gpio_in(gic, rcc_crossbar_destination(i));
    }
    s->crossbar.inputs = qemu_allocate_irqs(rcc_crossbar_input, &s->crossbar,
                                           RCC_XBAR_INPUTS);
    for (unsigned i = 0; i < ARRAY_SIZE(s->spi); i++) {
        DeviceState *spi = qdev_new(TYPE_DRA7_MCSPI);
        g_autofree char *name = g_strdup_printf("spi%u", rcc_spis[i].number);

        object_property_add_child(OBJECT(ms), name, OBJECT(spi));
        qdev_prop_set_string(spi, "bus-name", name);
        s->spi[i] = spi;
        sysbus_realize_and_unref(SYS_BUS_DEVICE(spi), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(spi), 0, rcc_spis[i].base);
        sysbus_connect_irq(SYS_BUS_DEVICE(spi), 0,
                           s->crossbar.inputs[rcc_spis[i].irq]);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(s->dcan); i++) {
        for (unsigned line = 0; line < 2; line++) {
            s->dcan[i].irq[line] = s->crossbar.inputs[222 + i * 3 + line];
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(s->i2c); i++) {
        DeviceState *i2c = qdev_new(TYPE_DRA7_I2C);
        g_autofree char *name = g_strdup_printf("i2c%u", i + 1);

        object_property_add_child(OBJECT(ms), name, OBJECT(i2c));
        qdev_prop_set_string(i2c, "bus-name", name);
        s->i2c[i] = i2c;
        sysbus_realize_and_unref(SYS_BUS_DEVICE(i2c), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(i2c), 0, rcc_i2cs[i].base);
        sysbus_connect_irq(SYS_BUS_DEVICE(i2c), 0,
                           s->crossbar.inputs[rcc_i2cs[i].irq]);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(s->gp_timer); i++) {
        RCCTimer *timer = &s->gp_timer[i];
        g_autofree char *name = g_strdup_printf("mh2p-rcc-timer%u",
                                               rcc_gp_timers[i].number);

        timer->number = rcc_gp_timers[i].number;
        timer->irq = s->crossbar.inputs[rcc_gp_timers[i].irq];
        timer->expiry = timer_new_ns(QEMU_CLOCK_VIRTUAL, rcc_gp_timer_expired,
                                     timer);
        memory_region_init_io(&timer->registers, OBJECT(ms), &rcc_gp_timer_ops,
                              timer, name, 0x1000);
        memory_region_add_subregion(sysmem, rcc_gp_timers[i].base,
                                    &timer->registers);
    }
    memory_region_init_io(&s->crossbar.registers, OBJECT(ms),
                          &rcc_crossbar_ops, &s->crossbar,
                          "mh2p-rcc-mpu-crossbar", 0x130);
    memory_region_add_subregion(sysmem, 0x4a002a48, &s->crossbar.registers);
    s->mpu_watchdog_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                       rcc_mpu_wdt_expired, s);
    memory_region_init_io(&s->mpu_watchdog, OBJECT(ms), &rcc_mpu_wdt_ops, s,
                          "mh2p-rcc-mpu-watchdog", 0x18);
    memory_region_add_subregion(sysmem, 0x482a0000, &s->mpu_watchdog);
    for (unsigned i = 0; i < ARRAY_SIZE(s->dsp_crossbar); i++) {
        RCCCrossbar *crossbar = &s->dsp_crossbar[i];
        g_autofree char *name = g_strdup_printf("mh2p-rcc-dsp%u-crossbar",
                                               i + 1);

        crossbar->selector_count = 64;
        crossbar->mmr_lock = &s->mmr_lock2;
        memory_region_init_io(&crossbar->registers, OBJECT(ms),
                              &rcc_crossbar_ops, crossbar, name, 0x80);
        memory_region_add_subregion(sysmem, 0x4a002948 + i * 0x80,
                                    &crossbar->registers);
    }
    s->ipu_crossbar.selector_count = ARRAY_SIZE(rcc_ipu1_crossbar_defaults);
    s->ipu_crossbar.defaults = rcc_ipu1_crossbar_defaults;
    s->ipu_crossbar.mmr_lock = &s->mmr_lock2;
    s->crossbar.peer = &s->ipu_crossbar;
    memory_region_init_io(&s->ipu_crossbar.registers, OBJECT(ms),
                          &rcc_crossbar_ops, &s->ipu_crossbar,
                          "mh2p-rcc-ipu1-crossbar", 0x74);
    memory_region_add_subregion(sysmem, 0x4a0027e0,
                                &s->ipu_crossbar.registers);
    if (s->ipu2_native) {
        s->ipu2_crossbar.selector_count = ARRAY_SIZE(rcc_ipu1_crossbar_defaults);
        s->ipu2_crossbar.defaults = rcc_ipu1_crossbar_defaults;
        s->ipu2_crossbar.mmr_lock = &s->mmr_lock2;
        s->ipu_crossbar.peer = &s->ipu2_crossbar;
        memory_region_init_io(&s->ipu2_crossbar.registers, OBJECT(ms),
                              &rcc_crossbar_ops, &s->ipu2_crossbar,
                              "mh2p-rcc-ipu2-crossbar", 0x74);
        memory_region_add_subregion(sysmem, 0x4a002854,
                                    &s->ipu2_crossbar.registers);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(rcc_uarts); i++) {
        RCCUART *uart = &s->uart[i];

        uart->base = rcc_uarts[i].base;
        uart->irq = s->crossbar.inputs[rcc_uarts[i].irq];
        uart->clock_enabled = true;
        uart->core = serial_mm_init(sysmem, rcc_uarts[i].base, 2,
                                   qemu_allocate_irq(rcc_uart_irq, uart, 0),
                                   3000000, serial_hd(rcc_uarts[i].serial),
                                   DEVICE_LITTLE_ENDIAN);
        memory_region_init_io(&uart->registers, OBJECT(ms), &rcc_uart_ops,
                              uart, "mh2p-rcc-uart", 0x100);
        memory_region_add_subregion_overlap(sysmem, rcc_uarts[i].base,
                                            &uart->registers, 1);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(gpio_base); i++) {
        static const unsigned irq1[] = { 24, 25, 26, 27, 28, 29, 30, 116 };
        static const unsigned irq2[] = {
            138, 139, 140, 141, 142, 143, 347, 348,
        };
        DeviceState *gpio = qdev_new(TYPE_DRA7_GPIO);
        g_autofree char *name = g_strdup_printf("gpio%u", i + 1);

        object_property_add_child(OBJECT(ms), name, OBJECT(gpio));
        s->gpio[i] = gpio;
        sysbus_realize_and_unref(SYS_BUS_DEVICE(gpio), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(gpio), 0, gpio_base[i]);
        sysbus_connect_irq(SYS_BUS_DEVICE(gpio), 0,
                           s->crossbar.inputs[irq1[i]]);
        sysbus_connect_irq(SYS_BUS_DEVICE(gpio), 1,
                           s->crossbar.inputs[irq2[i]]);
        if (i == 6) {
            DeviceState *sideband =
                mh2p_pcie_sideband_create(OBJECT(ms), false);

            if (sideband) {
                sysbus_connect_irq(SYS_BUS_DEVICE(sideband), 0,
                    qemu_allocate_irq(rcc_pcie_external_reference, s, 0));
                sysbus_connect_irq(SYS_BUS_DEVICE(sideband), 1,
                                    qdev_get_gpio_in(gpio, 27));
            }
        }
    }
    mailbox = qdev_new(TYPE_OMAP4_MAILBOX);
    object_property_add_child(OBJECT(ms), "mailbox1", OBJECT(mailbox));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(mailbox), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(mailbox), 0, 0x4a0f4000);
    sysbus_connect_irq(SYS_BUS_DEVICE(mailbox), 0, s->crossbar.inputs[21]);
    sysbus_connect_irq(SYS_BUS_DEVICE(mailbox), 1, s->crossbar.inputs[135]);
    sysbus_connect_irq(SYS_BUS_DEVICE(mailbox), 2, s->crossbar.inputs[134]);
    for (unsigned i = 0; i < 2; i++) {
        g_autofree char *name = g_strdup_printf("mailbox%u", i + 5);

        mailbox = qdev_new(TYPE_OMAP4_MAILBOX);
        object_property_add_child(OBJECT(ms), name, OBJECT(mailbox));
        qdev_prop_set_bit(mailbox, "dra7", true);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(mailbox), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(mailbox), 0, 0x48840000 + i * 0x2000);
        for (unsigned user = 0; user < 4; user++) {
            sysbus_connect_irq(SYS_BUS_DEVICE(mailbox), user,
                               s->crossbar.inputs[249 + i * 4 + user]);
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(s->usb); i++) {
        DeviceState *usb = qdev_new(TYPE_USB_DWC3);
        g_autofree char *name = g_strdup_printf("usb%u", i + 1);

        object_property_add_child(OBJECT(ms), name, OBJECT(usb));
        qdev_prop_set_bit(usb, "usb2-utmi", true);
        qdev_prop_set_uint32(usb, "intrs", 1);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(usb), &error_fatal);
        s->usb[i] = usb;
        sysbus_mmio_map(SYS_BUS_DEVICE(usb), 0, 0x48890000 + i * 0x40000);
        memory_region_set_enabled(
            sysbus_mmio_get_region(SYS_BUS_DEVICE(usb), 0), false);
        qdev_connect_gpio_out_named(DEVICE(&USB_DWC3(usb)->sysbus_xhci),
                                    SYSBUS_DEVICE_GPIO_IRQ, 0,
                                    s->crossbar.inputs[71 + i * 2]);
    }
    rom_add_blob_fixed("mh2p-rcc-ifs", image, size, RCC_IMAGE_BASE);
    s->pcie_transport = mh2p_pcie_config_create(
        OBJECT(ms), "pcie-config", s->pcie_fd, rcc_pcie_endpoint_ready,
        rcc_pcie_endpoint_config, rcc_pcie_endpoint_memory, s);
    s->pcie_outbound_transport = mh2p_pcie_config_create(
        OBJECT(ms), "pcie-outbound", s->pcie_outbound_fd,
        NULL, NULL, NULL, NULL);
    memory_region_init_io(&s->pcie.outbound_window, OBJECT(ms),
                          &rcc_pcie_outbound_ops, s, "rcc-pcie-outbound",
                          0x10000000);
    memory_region_add_subregion(sysmem, 0x20000000,
                                &s->pcie.outbound_window);
    s->init_done.notify = rcc_init_done;
    ipu_mmu = qdev_new(TYPE_MH2P_IPU_MMU);
    object_property_add_child(OBJECT(ms), "ipu1-mmu", OBJECT(ipu_mmu));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(ipu_mmu), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(ipu_mmu), 0, 0x58882000);
    sysbus_connect_irq(SYS_BUS_DEVICE(ipu_mmu), 0, s->crossbar.inputs[395]);
    if (s->ipu1_native && s->ipu1_firmware) {
        error_report("experimental-ipu1-native cannot preload "
                     "diagnostic firmware");
        exit(EXIT_FAILURE);
    }
    if (s->ipu1_firmware || s->ipu1_native) {
        DeviceState *secondary = NULL;
        DeviceState *ipu = mh2p_rcc_ipu1_create(OBJECT(ms), s->ipu1_firmware,
                                              ipu_mmu,
                                              s->ipu1_smp || s->ipu1_native ?
                                              &secondary : NULL, &s->ipu1, 1);

        sysbus_connect_irq(SYS_BUS_DEVICE(ipu_mmu), 1,
                            qdev_get_gpio_in(ipu, 0));
        if (secondary) {
            sysbus_connect_irq(SYS_BUS_DEVICE(ipu_mmu), 2,
                                qdev_get_gpio_in(secondary, 0));
        }
        for (unsigned i = 0; i < 57; i++) {
            s->ipu_crossbar.outputs[i] = qdev_get_gpio_in(ipu, i + 7);
            if (secondary) {
                s->ipu_crossbar.secondary_outputs[i] =
                    qdev_get_gpio_in(secondary, i + 7);
            }
        }
    } else if (s->ipu1_smp) {
        error_report("experimental-ipu1-smp requires experimental-ipu1-firmware");
        exit(EXIT_FAILURE);
    }
    if (s->ipu2_native) {
        DeviceState *secondary = NULL;
        DeviceState *mmu = qdev_new(TYPE_MH2P_IPU_MMU);
        DeviceState *ipu;

        object_property_add_child(OBJECT(ms), "ipu2-mmu", OBJECT(mmu));
        sysbus_realize_and_unref(SYS_BUS_DEVICE(mmu), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(mmu), 0, 0x55082000);
        sysbus_connect_irq(SYS_BUS_DEVICE(mmu), 0, s->crossbar.inputs[396]);
        ipu = mh2p_rcc_ipu1_create(OBJECT(ms), NULL, mmu, &secondary,
                                  &s->ipu2, 2);
        sysbus_connect_irq(SYS_BUS_DEVICE(mmu), 1, qdev_get_gpio_in(ipu, 0));
        sysbus_connect_irq(SYS_BUS_DEVICE(mmu), 2,
                           qdev_get_gpio_in(secondary, 0));
        for (unsigned i = 0; i < 57; i++) {
            s->ipu2_crossbar.outputs[i] = qdev_get_gpio_in(ipu, i + 7);
            s->ipu2_crossbar.secondary_outputs[i] =
                qdev_get_gpio_in(secondary, i + 7);
        }
    }
    qemu_add_machine_init_done_notifier(&s->init_done);
}

static char *rcc_get_pcie_fd(Object *obj, Error **errp)
{
    return g_strdup(((MH2PRCCMachineState *)obj)->pcie_fd);
}

static void rcc_set_pcie_fd(Object *obj, const char *value, Error **errp)
{
    MH2PRCCMachineState *s = (MH2PRCCMachineState *)obj;

    g_free(s->pcie_fd);
    s->pcie_fd = g_strdup(value);
}

static char *rcc_get_pcie_outbound_fd(Object *obj, Error **errp)
{
    return g_strdup(((MH2PRCCMachineState *)obj)->pcie_outbound_fd);
}

static void rcc_set_pcie_outbound_fd(Object *obj, const char *value,
                                   Error **errp)
{
    MH2PRCCMachineState *s = (MH2PRCCMachineState *)obj;

    g_free(s->pcie_outbound_fd);
    s->pcie_outbound_fd = g_strdup(value);
}

static char *rcc_get_ipu1_firmware(Object *obj, Error **errp)
{
    return g_strdup(((MH2PRCCMachineState *)obj)->ipu1_firmware);
}

static void rcc_set_ipu1_firmware(Object *obj, const char *value, Error **errp)
{
    MH2PRCCMachineState *s = (MH2PRCCMachineState *)obj;

    g_free(s->ipu1_firmware);
    s->ipu1_firmware = g_strdup(value);
}

static bool rcc_get_ipu1_smp(Object *obj, Error **errp)
{
    return ((MH2PRCCMachineState *)obj)->ipu1_smp;
}

static void rcc_set_ipu1_smp(Object *obj, bool value, Error **errp)
{
    ((MH2PRCCMachineState *)obj)->ipu1_smp = value;
}

static bool rcc_get_ipu1_native(Object *obj, Error **errp)
{
    return ((MH2PRCCMachineState *)obj)->ipu1_native;
}

static void rcc_set_ipu1_native(Object *obj, bool value, Error **errp)
{
    ((MH2PRCCMachineState *)obj)->ipu1_native = value;
}

static bool rcc_get_ipu2_native(Object *obj, Error **errp)
{
    return ((MH2PRCCMachineState *)obj)->ipu2_native;
}

static void rcc_set_ipu2_native(Object *obj, bool value, Error **errp)
{
    ((MH2PRCCMachineState *)obj)->ipu2_native = value;
}

static bool rcc_get_power_clocks(Object *obj, Error **errp)
{
    return ((MH2PRCCMachineState *)obj)->power_clocks;
}

static void rcc_set_power_clocks(Object *obj, bool value, Error **errp)
{
    ((MH2PRCCMachineState *)obj)->power_clocks = value;
}

static void rcc_machine_init(MachineClass *mc)
{
    static const char * const cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-a15"), NULL,
    };

    mc->desc = "MH2P RCC DRA72x (experimental QNX direct start)";
    mc->init = rcc_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a15");
    mc->valid_cpu_types = cpu_types;
    mc->default_cpus = mc->min_cpus = mc->max_cpus = 1;
    mc->default_ram_size = 256 * MiB;
    mc->default_ram_id = "mh2p-rcc.ram";
    for (unsigned i = 0; i < 2; i++) {
        g_autofree char *name = g_strdup_printf("canbus%u", i);

        object_class_property_add_link(OBJECT_CLASS(mc), name, TYPE_CAN_BUS,
            offsetof(MH2PRCCMachineState, canbus) + i * sizeof(CanBusState *),
            object_property_allow_set_link, 0);
    }
    object_class_property_add_str(OBJECT_CLASS(mc), "pcie-fd",
                                  rcc_get_pcie_fd, rcc_set_pcie_fd);
    object_class_property_add_str(OBJECT_CLASS(mc), "pcie-outbound-fd",
                                  rcc_get_pcie_outbound_fd,
                                  rcc_set_pcie_outbound_fd);
    object_class_property_add_str(OBJECT_CLASS(mc), "experimental-ipu1-firmware",
                                  rcc_get_ipu1_firmware,
                                  rcc_set_ipu1_firmware);
    object_class_property_add_bool(OBJECT_CLASS(mc), "experimental-ipu1-smp",
                                   rcc_get_ipu1_smp, rcc_set_ipu1_smp);
    object_class_property_add_bool(OBJECT_CLASS(mc), "experimental-ipu1-native",
                                   rcc_get_ipu1_native, rcc_set_ipu1_native);
    object_class_property_add_bool(OBJECT_CLASS(mc), "experimental-ipu2-native",
                                   rcc_get_ipu2_native, rcc_set_ipu2_native);
    object_class_property_add_bool(OBJECT_CLASS(mc), "experimental-power-clocks",
                                   rcc_get_power_clocks, rcc_set_power_clocks);
}

DEFINE_MACHINE_EXTENDED("mh2p-rcc", MACHINE, MH2PRCCMachineState,
                        rcc_machine_init, false, false, NULL)

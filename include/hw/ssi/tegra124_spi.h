#ifndef HW_SSI_TEGRA124_SPI_H
#define HW_SSI_TEGRA124_SPI_H

#include "qom/object.h"
#include "hw/core/sysbus.h"
#include "hw/ssi/ssi.h"

#define TEGRA124_SPI_IOSIZE 0x200
#define TEGRA124_SPI_FIFO_DEPTH 64
#define TEGRA124_SPI_NUM_CS 4

#define TYPE_TEGRA124_SPI "tegra124-spi"
OBJECT_DECLARE_SIMPLE_TYPE(Tegra124SPIState, TEGRA124_SPI)

struct Tegra124SPIState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    SSIBus *bus;
    qemu_irq irq;
    qemu_irq cs_lines[TEGRA124_SPI_NUM_CS];

    uint32_t command1;
    uint32_t command2;
    uint32_t timing1;
    uint32_t timing2;
    uint32_t trans_status;
    uint32_t fifo_errors;
    uint32_t dma_ctl;
    uint32_t dma_blk;
    uint32_t intr_mask;
    uint32_t misc;
    uint32_t timing3;
    uint32_t tx_fifo[TEGRA124_SPI_FIFO_DEPTH];
    uint32_t rx_fifo[TEGRA124_SPI_FIFO_DEPTH];
    uint32_t tx_head, tx_count;
    uint32_t rx_head, rx_count;
    int32_t active_cs;
    bool reset_asserted;
};

#endif

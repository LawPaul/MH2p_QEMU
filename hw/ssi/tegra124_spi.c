/*
 * NVIDIA Tegra124 SPI (SLINK successor) controller, PIO mode.
 *
 * The MH2P factory driver (spi-tegra124.so) runs SPI1 and SPI5 with "nodma":
 * it flushes both FIFOs, clears TRANS_STATUS.RDY, fills TX_FIFO, programs
 * DMA_BLK, enables the RX interrupt in DMA_CTL and sets COMMAND1.PIO. Its
 * interrupt handler acknowledges RDY, drains RX_FIFO and restarts the next
 * FIFO-sized chunk. APB DMA is not modelled.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/ssi/tegra124_spi.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "trace.h"

#define COMMAND1            0x000
#define COMMAND2            0x004
#define TIMING1             0x008
#define TIMING2             0x00c
#define TRANS_STATUS        0x010
#define FIFO_STATUS         0x014
#define TX_DATA             0x018
#define RX_DATA             0x01c
#define DMA_CTL             0x020
#define DMA_BLK             0x024
#define TX_FIFO             0x108
#define RX_FIFO             0x188
#define INTR_MASK           0x18c
#define MISC                0x194
#define TIMING3             0x198

#define CMD1_PIO            BIT(31)
#define CMD1_CS_SEL_SHIFT   26
#define CMD1_CS_SW_HW       BIT(21)
#define CMD1_CS_SW_VAL      BIT(20)
#define CMD1_RX_EN          BIT(12)
#define CMD1_TX_EN          BIT(11)
#define CMD1_LSBI_FE        BIT(7)
#define CMD1_LSBY_FE        BIT(6)
#define CMD1_PACKED         BIT(5)
#define CMD1_BIT_LEN_MASK   0x1f

#define TRANS_RDY           BIT(30)
#define TRANS_BLK_CNT_MASK  0xffff

#define FIFO_RX_EMPTY       BIT(0)
#define FIFO_RX_FULL        BIT(1)
#define FIFO_TX_EMPTY       BIT(2)
#define FIFO_TX_FULL        BIT(3)
#define FIFO_ERRORS         0x1f0
#define FIFO_TX_FLUSH       BIT(14)
#define FIFO_RX_FLUSH       BIT(15)
#define FIFO_CS_INACTIVE    BIT(31)

#define DMA_IE_TX           BIT(28)
#define DMA_IE_RX           BIT(29)

static void tegra124_spi_update_irq(Tegra124SPIState *s)
{
    qemu_set_irq(s->irq, !s->reset_asserted &&
                 (s->trans_status & TRANS_RDY) &&
                 (s->dma_ctl & (DMA_IE_TX | DMA_IE_RX)));
}

static void tegra124_spi_update_cs(Tegra124SPIState *s, bool transfer)
{
    int cs = -1;

    if (!s->reset_asserted &&
        (transfer || ((s->command1 & CMD1_CS_SW_HW) &&
                      !(s->command1 & CMD1_CS_SW_VAL)))) {
        cs = (s->command1 >> CMD1_CS_SEL_SHIFT) & 3;
    }
    if (cs == s->active_cs) {
        return;
    }
    if (s->active_cs >= 0) {
        qemu_set_irq(s->cs_lines[s->active_cs], 1);
    }
    s->active_cs = cs;
    if (cs >= 0) {
        qemu_set_irq(s->cs_lines[cs], 0);
    }
}

static uint32_t tegra124_spi_fifo_status(Tegra124SPIState *s)
{
    uint32_t value = s->fifo_errors;

    value |= (TEGRA124_SPI_FIFO_DEPTH - s->tx_count) << 16;
    value |= s->rx_count << 23;
    if (!s->tx_count) {
        value |= FIFO_TX_EMPTY;
    } else if (s->tx_count == TEGRA124_SPI_FIFO_DEPTH) {
        value |= FIFO_TX_FULL;
    }
    if (!s->rx_count) {
        value |= FIFO_RX_EMPTY;
    } else if (s->rx_count == TEGRA124_SPI_FIFO_DEPTH) {
        value |= FIFO_RX_FULL;
    }
    if (s->active_cs < 0) {
        value |= FIFO_CS_INACTIVE;
    }
    return value;
}

static uint32_t tegra124_spi_tx_pop(Tegra124SPIState *s, bool *underflow)
{
    uint32_t value;

    if (!s->tx_count) {
        *underflow = true;
        return 0;
    }
    value = s->tx_fifo[s->tx_head];
    s->tx_head = (s->tx_head + 1) % TEGRA124_SPI_FIFO_DEPTH;
    s->tx_count--;
    return value;
}

static void tegra124_spi_rx_push(Tegra124SPIState *s, uint32_t value)
{
    if (s->rx_count == TEGRA124_SPI_FIFO_DEPTH) {
        s->fifo_errors |= BIT(5);
        return;
    }
    s->rx_fifo[(s->rx_head + s->rx_count) % TEGRA124_SPI_FIFO_DEPTH] = value;
    s->rx_count++;
}

static uint8_t tegra124_spi_byte(Tegra124SPIState *s, uint8_t out)
{
    uint8_t in;

    if (s->command1 & CMD1_LSBI_FE) {
        out = revbit8(out);
    }
    in = ssi_transfer(s->bus, out);
    return s->command1 & CMD1_LSBI_FE ? revbit8(in) : in;
}

/* Exchange one word of 'bytes' bytes, most significant byte first. */
static uint32_t tegra124_spi_word(Tegra124SPIState *s, uint32_t out,
                                  unsigned bytes)
{
    uint32_t in = 0;

    for (unsigned i = 0; i < bytes; i++) {
        unsigned shift = s->command1 & CMD1_LSBY_FE ?
                         8 * i : 8 * (bytes - 1 - i);
        uint8_t byte = s->command1 & CMD1_TX_EN ? out >> shift : 0xff;

        in |= (uint32_t)tegra124_spi_byte(s, byte) << shift;
    }
    return in;
}

static void tegra124_spi_transfer(Tegra124SPIState *s)
{
    unsigned bits = (s->command1 & CMD1_BIT_LEN_MASK) + 1;
    unsigned bytes = DIV_ROUND_UP(bits, 8);
    uint32_t blocks = (s->dma_blk & 0xffff) + 1;
    bool underflow = false;

    tegra124_spi_update_cs(s, true);
    if ((s->command1 & CMD1_PACKED) && bytes < 4) {
        unsigned per_word = 4 / bytes;

        for (uint32_t done = 0; done < blocks;) {
            uint32_t out = s->command1 & CMD1_TX_EN ?
                           tegra124_spi_tx_pop(s, &underflow) : ~0U;
            uint32_t in = 0;

            for (unsigned i = 0; i < per_word && done < blocks; i++, done++) {
                unsigned shift = 8 * bytes * i;
                uint32_t mask = MAKE_64BIT_MASK(0, 8 * bytes);

                in |= (tegra124_spi_word(s, (out >> shift) & mask, bytes) &
                       mask) << shift;
            }
            if (s->command1 & CMD1_RX_EN) {
                tegra124_spi_rx_push(s, in);
            }
        }
    } else {
        for (uint32_t done = 0; done < blocks; done++) {
            uint32_t out = s->command1 & CMD1_TX_EN ?
                           tegra124_spi_tx_pop(s, &underflow) : ~0U;
            uint32_t in = tegra124_spi_word(s, out, bytes);

            if (s->command1 & CMD1_RX_EN) {
                tegra124_spi_rx_push(s, in & MAKE_64BIT_MASK(0, bits));
            }
        }
    }
    if (underflow) {
        s->fifo_errors |= BIT(6);
    }
    trace_tegra124_spi_transfer(blocks, bits, s->command1, s->rx_count);
    s->command1 &= ~CMD1_PIO;
    s->trans_status = TRANS_RDY | (blocks & TRANS_BLK_CNT_MASK);
    tegra124_spi_update_cs(s, false);
    tegra124_spi_update_irq(s);
}

static uint64_t tegra124_spi_read(void *opaque, hwaddr offset, unsigned size)
{
    Tegra124SPIState *s = opaque;
    uint32_t value = 0;

    switch (offset) {
    case COMMAND1:
        value = s->command1;
        break;
    case COMMAND2:
        value = s->command2;
        break;
    case TIMING1:
        value = s->timing1;
        break;
    case TIMING2:
        value = s->timing2;
        break;
    case TRANS_STATUS:
        value = s->trans_status;
        break;
    case FIFO_STATUS:
        value = tegra124_spi_fifo_status(s);
        break;
    case TX_DATA:
    case RX_DATA:
        break;
    case DMA_CTL:
        value = s->dma_ctl;
        break;
    case DMA_BLK:
        value = s->dma_blk;
        break;
    case RX_FIFO:
        if (!s->rx_count) {
            s->fifo_errors |= BIT(4);
            break;
        }
        value = s->rx_fifo[s->rx_head];
        s->rx_head = (s->rx_head + 1) % TEGRA124_SPI_FIFO_DEPTH;
        s->rx_count--;
        break;
    case INTR_MASK:
        value = s->intr_mask;
        break;
    case MISC:
        value = s->misc;
        break;
    case TIMING3:
        value = s->timing3;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "tegra124-spi: unimplemented read 0x%"
                      HWADDR_PRIx "\n", offset);
        break;
    }
    trace_tegra124_spi_read(offset, value);
    return value;
}

static void tegra124_spi_write(void *opaque, hwaddr offset, uint64_t value64,
                               unsigned size)
{
    Tegra124SPIState *s = opaque;
    uint32_t value = value64;

    trace_tegra124_spi_write(offset, value);
    if (s->reset_asserted) {
        return;
    }
    switch (offset) {
    case COMMAND1: {
        bool start = (value & CMD1_PIO) && !(s->command1 & CMD1_PIO);

        s->command1 = value;
        tegra124_spi_update_cs(s, false);
        if (start) {
            tegra124_spi_transfer(s);
        }
        break;
    }
    case COMMAND2:
        s->command2 = value;
        break;
    case TIMING1:
        s->timing1 = value;
        break;
    case TIMING2:
        s->timing2 = value;
        break;
    case TRANS_STATUS:
        if (value & TRANS_RDY) {
            s->trans_status &= ~TRANS_RDY;
        }
        break;
    case FIFO_STATUS:
        s->fifo_errors &= ~(value & FIFO_ERRORS);
        if (value & FIFO_TX_FLUSH) {
            s->tx_head = s->tx_count = 0;
        }
        if (value & FIFO_RX_FLUSH) {
            s->rx_head = s->rx_count = 0;
        }
        break;
    case TX_DATA:
    case RX_DATA:
        break;
    case DMA_CTL:
        s->dma_ctl = value;
        if (value & BIT(31)) {
            qemu_log_mask(LOG_UNIMP, "tegra124-spi: DMA mode\n");
        }
        break;
    case DMA_BLK:
        s->dma_blk = value & 0xffff;
        break;
    case TX_FIFO:
        if (s->tx_count == TEGRA124_SPI_FIFO_DEPTH) {
            s->fifo_errors |= BIT(7);
            break;
        }
        s->tx_fifo[(s->tx_head + s->tx_count) % TEGRA124_SPI_FIFO_DEPTH] =
            value;
        s->tx_count++;
        break;
    case INTR_MASK:
        s->intr_mask = value;
        break;
    case MISC:
        s->misc = value;
        break;
    case TIMING3:
        s->timing3 = value;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "tegra124-spi: unimplemented write 0x%"
                      HWADDR_PRIx " = 0x%08x\n", offset, value);
        break;
    }
    tegra124_spi_update_irq(s);
}

static const MemoryRegionOps tegra124_spi_ops = {
    .read = tegra124_spi_read,
    .write = tegra124_spi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void tegra124_spi_reset(DeviceState *dev)
{
    Tegra124SPIState *s = TEGRA124_SPI(dev);

    s->command1 = 0x40000007;
    s->command2 = 0;
    s->timing1 = 0;
    s->timing2 = 0;
    s->trans_status = 0;
    s->fifo_errors = 0;
    s->dma_ctl = 0;
    s->dma_blk = 0;
    s->intr_mask = 0;
    s->misc = 0;
    s->timing3 = 0;
    s->tx_head = s->tx_count = 0;
    s->rx_head = s->rx_count = 0;
    tegra124_spi_update_cs(s, false);
    tegra124_spi_update_irq(s);
}

static void tegra124_spi_reset_input(void *opaque, int n, int level)
{
    Tegra124SPIState *s = opaque;

    if (level && !s->reset_asserted) {
        tegra124_spi_reset(DEVICE(s));
    }
    s->reset_asserted = level;
    tegra124_spi_update_cs(s, false);
    tegra124_spi_update_irq(s);
}

static void tegra124_spi_realize(DeviceState *dev, Error **errp)
{
    Tegra124SPIState *s = TEGRA124_SPI(dev);

    s->bus = ssi_create_bus(dev, "spi");
    memory_region_init_io(&s->iomem, OBJECT(dev), &tegra124_spi_ops, s,
                          TYPE_TEGRA124_SPI, TEGRA124_SPI_IOSIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    for (unsigned i = 0; i < TEGRA124_SPI_NUM_CS; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->cs_lines[i]);
    }
    qdev_init_gpio_in_named(dev, tegra124_spi_reset_input, "reset", 1);
    s->active_cs = -1;
}

static const VMStateDescription tegra124_spi_vmstate = {
    .name = TYPE_TEGRA124_SPI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(command1, Tegra124SPIState),
        VMSTATE_UINT32(command2, Tegra124SPIState),
        VMSTATE_UINT32(timing1, Tegra124SPIState),
        VMSTATE_UINT32(timing2, Tegra124SPIState),
        VMSTATE_UINT32(trans_status, Tegra124SPIState),
        VMSTATE_UINT32(fifo_errors, Tegra124SPIState),
        VMSTATE_UINT32(dma_ctl, Tegra124SPIState),
        VMSTATE_UINT32(dma_blk, Tegra124SPIState),
        VMSTATE_UINT32(intr_mask, Tegra124SPIState),
        VMSTATE_UINT32(misc, Tegra124SPIState),
        VMSTATE_UINT32(timing3, Tegra124SPIState),
        VMSTATE_UINT32_ARRAY(tx_fifo, Tegra124SPIState,
                             TEGRA124_SPI_FIFO_DEPTH),
        VMSTATE_UINT32_ARRAY(rx_fifo, Tegra124SPIState,
                             TEGRA124_SPI_FIFO_DEPTH),
        VMSTATE_UINT32(tx_head, Tegra124SPIState),
        VMSTATE_UINT32(tx_count, Tegra124SPIState),
        VMSTATE_UINT32(rx_head, Tegra124SPIState),
        VMSTATE_UINT32(rx_count, Tegra124SPIState),
        VMSTATE_INT32(active_cs, Tegra124SPIState),
        VMSTATE_BOOL(reset_asserted, Tegra124SPIState),
        VMSTATE_END_OF_LIST()
    },
};

static void tegra124_spi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = tegra124_spi_realize;
    device_class_set_legacy_reset(dc, tegra124_spi_reset);
    dc->vmsd = &tegra124_spi_vmstate;
    dc->desc = "NVIDIA Tegra124 SPI controller";
}

static const TypeInfo tegra124_spi_info = {
    .name = TYPE_TEGRA124_SPI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Tegra124SPIState),
    .class_init = tegra124_spi_class_init,
};

static void tegra124_spi_register(void)
{
    type_register_static(&tegra124_spi_info);
}

type_init(tegra124_spi_register)

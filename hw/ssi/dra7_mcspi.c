/*
 * DRA7 McSPI: synchronous PIO master transfers and native FIFO/IRQ state.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/fifo8.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/ssi/ssi.h"
#include "hw/ssi/dra7_mcspi.h"
#include "migration/vmstate.h"

OBJECT_DECLARE_SIMPLE_TYPE(DRA7McSPIState, DRA7_MCSPI)

#define FFER BIT(28)
#define FFEW BIT(27)
#define FORCE BIT(20)
#define DMA (BIT(15) | BIT(14))

typedef struct DRA7McSPIChannel {
    uint32_t config, control, last_tx;
    unsigned words;
    bool eot;
    Fifo8 tx, rx;
} DRA7McSPIChannel;

struct DRA7McSPIState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    SSIBus *bus;
    char *bus_name;
    qemu_irq irq;
    bool clock_enabled;
    uint32_t sysconfig, module, status, irq_enable, level, system_test;
    DRA7McSPIChannel channel[4];
};

static unsigned dra7_mcspi_word_bytes(DRA7McSPIChannel *c)
{
    unsigned bits = ((c->config >> 7) & 31) + 1;

    return bits <= 8 ? 1 : bits <= 16 ? 2 : 4;
}

static unsigned dra7_mcspi_capacity(DRA7McSPIChannel *c, bool receive)
{
    uint32_t fifo = c->config & (FFER | FFEW);

    if (!(fifo & (receive ? FFER : FFEW))) {
        return dra7_mcspi_word_bytes(c);
    }
    return fifo == (FFER | FFEW) ? 64 : 128;
}

static void dra7_mcspi_irq(DRA7McSPIState *s)
{
    qemu_set_irq(s->irq, s->clock_enabled && !!(s->status & s->irq_enable));
}

static void dra7_mcspi_events(DRA7McSPIState *s, unsigned index)
{
    DRA7McSPIChannel *c = &s->channel[index];
    unsigned tx_space = dra7_mcspi_capacity(c, false) - fifo8_num_used(&c->tx);
    unsigned rx_used = fifo8_num_used(&c->rx);
    unsigned mode = (c->config >> 12) & 3;

    if (c->control & 1) {
        if (mode != 1 && tx_space >= ((c->config & FFEW) ?
                                     (s->level & 0xff) + 1 :
                                     dra7_mcspi_word_bytes(c))) {
            s->status |= BIT(index * 4);
        }
        if (mode != 2 && rx_used >= ((c->config & FFER) ?
                                    ((s->level >> 8) & 0xff) + 1 :
                                    dra7_mcspi_word_bytes(c))) {
            s->status |= BIT(index * 4 + 2);
        }
    }
    dra7_mcspi_irq(s);
}

static void dra7_mcspi_select(DRA7McSPIState *s, unsigned index, bool active)
{
    for (unsigned i = 0; i < ARRAY_SIZE(s->channel); i++) {
        DeviceState *dev = ssi_get_cs(s->bus, i);

        if (dev && SSI_PERIPHERAL_GET_CLASS(dev)->cs_polarity != SSI_CS_NONE) {
            bool polarity = s->channel[i].config & BIT(6);

            qemu_set_irq(qdev_get_gpio_in_named(dev, SSI_GPIO_CS, 0),
                         i == index && active ? !polarity : polarity);
        }
    }
}

static bool dra7_mcspi_pump(DRA7McSPIState *s, unsigned index)
{
    DRA7McSPIChannel *c = &s->channel[index];
    unsigned bytes = dra7_mcspi_word_bytes(c);
    unsigned bits = ((c->config >> 7) & 31) + 1;
    bool receive = ((c->config >> 12) & 3) != 2;
    unsigned limit = (c->config & (FFER | FFEW)) ? s->level >> 16 : 0;

    if (!(c->control & 1) || (s->module & 4) || !s->clock_enabled) {
        return true;
    }
    while (fifo8_num_used(&c->tx) >= bytes && (!limit || c->words < limit)) {
        uint32_t tx = 0, rx;

        /*
         * No guessed peer or zero-filled response on an unattached bus.
         * RX overflow and DMA need their actual engines, not a ready fixture.
         */
        if (!ssi_get_cs(s->bus, index) ||
            (receive && fifo8_num_used(&c->rx) + bytes >
             dra7_mcspi_capacity(c, true))) {
            return false;
        }
        for (unsigned i = 0; i < bytes; i++) {
            tx |= (uint32_t)fifo8_pop(&c->tx) << (8 * i);
        }
        c->eot = false;
        dra7_mcspi_select(s, index, true);
        rx = ssi_transfer(s->bus, tx & MAKE_64BIT_MASK(0, bits));
        if (receive) {
            rx &= MAKE_64BIT_MASK(0, bits);
            for (unsigned i = 0; i < bytes; i++) {
                fifo8_push(&c->rx, rx >> (8 * i));
            }
        }
        c->words++;
        c->eot = true;
        if (limit && c->words == limit) {
            s->status |= BIT(17);
        }
        dra7_mcspi_select(s, index, (s->module & 1) && (c->config & FORCE));
    }
    dra7_mcspi_events(s, index);
    return true;
}

static void dra7_mcspi_soft_reset(DRA7McSPIState *s)
{
    s->module = 4;
    s->status = s->irq_enable = s->level = 0;
    s->system_test = 0;
    for (unsigned i = 0; i < ARRAY_SIZE(s->channel); i++) {
        DRA7McSPIChannel *c = &s->channel[i];

        c->config = 0x60000;
        c->control = c->last_tx = c->words = 0;
        c->eot = false;
        fifo8_reset(&c->tx);
        fifo8_reset(&c->rx);
    }
    dra7_mcspi_select(s, 0, false);
    dra7_mcspi_irq(s);
}

static void dra7_mcspi_reset(DeviceState *dev)
{
    DRA7McSPIState *s = DRA7_MCSPI(dev);

    s->sysconfig = 0x15;
    dra7_mcspi_soft_reset(s);
}

void dra7_mcspi_set_clock(DeviceState *dev, bool enabled)
{
    DRA7McSPIState *s = DRA7_MCSPI(dev);

    s->clock_enabled = enabled;
    memory_region_set_enabled(&s->mmio, enabled);
    dra7_mcspi_irq(s);
}

static uint32_t dra7_mcspi_channel_status(DRA7McSPIChannel *c)
{
    unsigned tx = fifo8_num_used(&c->tx), rx = fifo8_num_used(&c->rx);
    uint32_t status = c->eot ? BIT(2) : 0;

    if (!(c->control & 1)) {
        return status;
    }
    status |= (rx >= dra7_mcspi_word_bytes(c) ? BIT(0) : 0) |
              (tx < dra7_mcspi_capacity(c, false) ? BIT(1) : 0);
    if (c->config & FFEW) {
        status |= !tx ? BIT(3) : 0;
        status |= tx == dra7_mcspi_capacity(c, false) ? BIT(4) : 0;
    }
    if (c->config & FFER) {
        status |= !rx ? BIT(5) : 0;
        status |= rx == dra7_mcspi_capacity(c, true) ? BIT(6) : 0;
    }
    return status;
}

static MemTxResult dra7_mcspi_read(void *opaque, hwaddr offset,
                                   uint64_t *value, unsigned size,
                                   MemTxAttrs attrs)
{
    DRA7McSPIState *s = opaque;

    if (offset == 0x1a0 && (s->module & BIT(8))) {
        for (unsigned i = 0; i < ARRAY_SIZE(s->channel); i++) {
            if ((s->channel[i].control & 1) &&
                (s->channel[i].config & FFER)) {
                offset = 0x13c + i * 0x14;
                break;
            }
        }
    }
    if (offset >= 0x12c && offset < 0x17c) {
        unsigned index = (offset - 0x12c) / 0x14;
        unsigned reg = (offset - 0x12c) % 0x14;
        DRA7McSPIChannel *c = &s->channel[index];

        if (reg == 0x10) {
            unsigned bytes = dra7_mcspi_word_bytes(c);

            if (size < bytes || fifo8_num_used(&c->rx) < bytes) {
                goto unsupported;
            }
            *value = 0;
            for (unsigned i = 0; i < bytes; i++) {
                *value |= (uint32_t)fifo8_pop(&c->rx) << (8 * i);
            }
            dra7_mcspi_events(s, index);
            return MEMTX_OK;
        }
        if (size != 4) {
            goto unsupported;
        }
        switch (reg) {
        case 0: *value = c->config; return MEMTX_OK;
        case 4: *value = dra7_mcspi_channel_status(c); return MEMTX_OK;
        case 8: *value = c->control; return MEMTX_OK;
        case 0xc: *value = c->last_tx; return MEMTX_OK;
        default: goto unsupported;
        }
    }
    if (size != 4) {
        goto unsupported;
    }
    switch (offset) {
    case 0x110: *value = s->sysconfig; return MEMTX_OK;
    case 0x114: *value = 1; return MEMTX_OK;
    case 0x118: *value = s->status; return MEMTX_OK;
    case 0x11c: *value = s->irq_enable; return MEMTX_OK;
    case 0x120:
        *value = 0;
        return MEMTX_OK;
    case 0x124:
        *value = s->system_test;
        return MEMTX_OK;
    case 0x128: *value = s->module; return MEMTX_OK;
    case 0x17c:
        *value = s->level;
        for (unsigned i = 0; i < ARRAY_SIZE(s->channel); i++) {
            DRA7McSPIChannel *c = &s->channel[i];

            if ((c->control & 1) && (c->config & (FFER | FFEW))) {
                *value = (s->level & 0xffff) | ((c->words & 0xffff) << 16);
                break;
            }
        }
        return MEMTX_OK;
    }
unsupported:
    qemu_log_mask(LOG_UNIMP, "dra7-mcspi: unsupported read 0x%"
                  HWADDR_PRIx " size=%u\n", offset, size);
    *value = 0;
    return MEMTX_ERROR;
}

static MemTxResult dra7_mcspi_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size,
                                    MemTxAttrs attrs)
{
    DRA7McSPIState *s = opaque;

    if (offset == 0x180 && (s->module & BIT(8))) {
        for (unsigned i = 0; i < ARRAY_SIZE(s->channel); i++) {
            if ((s->channel[i].control & 1) &&
                (s->channel[i].config & FFEW)) {
                offset = 0x138 + i * 0x14;
                break;
            }
        }
    }
    if (offset >= 0x12c && offset < 0x17c) {
        unsigned index = (offset - 0x12c) / 0x14;
        unsigned reg = (offset - 0x12c) % 0x14;
        DRA7McSPIChannel *c = &s->channel[index];

        if (reg == 0xc) {
            unsigned bytes = dra7_mcspi_word_bytes(c);

            if (size < bytes || fifo8_num_used(&c->tx) + bytes >
                dra7_mcspi_capacity(c, false) ||
                ((c->control & 1) && !(s->module & 4) &&
                 (!ssi_get_cs(s->bus, index) ||
                  (((c->config >> 12) & 3) != 2 &&
                   fifo8_num_used(&c->rx) + bytes >
                   dra7_mcspi_capacity(c, true))))) {
                goto unsupported;
            }
            for (unsigned i = 0; i < bytes; i++) {
                fifo8_push(&c->tx, value >> (8 * i));
            }
            c->last_tx = value;
            if (!dra7_mcspi_pump(s, index)) {
                goto unsupported;
            }
            return MEMTX_OK;
        }
        if (size != 4) {
            goto unsupported;
        }
        if (reg == 0 && !(value & ~0x3fffffffU)) {
            if (c->control & 1) {
                if (((value ^ c->config) & ~FORCE) ||
                    (s->module & 5) != 1) {
                    goto unsupported;
                }
                c->config = value;
                dra7_mcspi_select(s, index, value & FORCE);
                return MEMTX_OK;
            }
            if ((value ^ c->config) & (FFER | FFEW)) {
                fifo8_reset(&c->tx);
                fifo8_reset(&c->rx);
            }
            c->config = value;
            return MEMTX_OK;
        }
        if (reg == 8 && !(value & ~0xff01U)) {
            if (value & 1) {
                unsigned mode = (c->config >> 12) & 3;

                if (((c->config >> 7) & 31) < 3 ||
                    (c->config & (DMA | BIT(23))) || mode == 3 ||
                    (s->module & (BIT(7) | BIT(1))) ||
                    (!(s->module & 4) && mode == 1) ||
                    ((s->module & 4) && index)) {
                    goto unsupported;
                }
                for (unsigned i = 0; i < ARRAY_SIZE(s->channel); i++) {
                    if (i != index && (s->channel[i].control & 1) &&
                        ((s->module & 1) ||
                         ((c->config & (FFER | FFEW)) &&
                          (s->channel[i].config & (FFER | FFEW))))) {
                        goto unsupported;
                    }
                }
                if (!(c->control & 1)) {
                    c->words = 0;
                    c->eot = false;
                    if (c->config & (FFER | FFEW)) {
                        fifo8_reset(&c->tx);
                        fifo8_reset(&c->rx);
                    } else if (!(s->module & 4) &&
                               !fifo8_is_empty(&c->tx) &&
                               !ssi_get_cs(s->bus, index)) {
                        goto unsupported;
                    }
                }
            }
            c->control = value;
            if (!(value & 1)) {
                dra7_mcspi_select(s, index, false);
            }
            if (!dra7_mcspi_pump(s, index)) {
                goto unsupported;
            }
            dra7_mcspi_events(s, index);
            return MEMTX_OK;
        }
        goto unsupported;
    }
    if (size != 4) {
        goto unsupported;
    }
    switch (offset) {
    case 0x110:
        if ((value & ~0x31fU) || (value & 0x18) == 0x18) {
            goto unsupported;
        }
        s->sysconfig = value & ~2U;
        if (value & 2) {
            dra7_mcspi_soft_reset(s);
        }
        return MEMTX_OK;
    case 0x118:
        if (value & ~0x3777fU) {
            goto unsupported;
        }
        s->status &= ~value;
        dra7_mcspi_irq(s);
        return MEMTX_OK;
    case 0x11c:
        if (value & ~0x3777fU) {
            goto unsupported;
        }
        s->irq_enable = value;
        dra7_mcspi_irq(s);
        return MEMTX_OK;
    case 0x120:
        if (value) {
            goto unsupported;
        }
        return MEMTX_OK;
    case 0x124:
        /* Direction configuration is dormant while SYSTEST is unsupported. */
        if (value & ~0x700U) {
            goto unsupported;
        }
        s->system_test = value;
        return MEMTX_OK;
    case 0x128:
        if ((value & ~0x1f7U) || (value & 0x70) > 0x40) {
            goto unsupported;
        }
        for (unsigned i = 0; i < ARRAY_SIZE(s->channel); i++) {
            if (s->channel[i].control & 1) {
                goto unsupported;
            }
        }
        s->module = value;
        return MEMTX_OK;
    case 0x17c:
        for (unsigned i = 0; i < ARRAY_SIZE(s->channel); i++) {
            if (s->channel[i].control & 1) {
                goto unsupported;
            }
        }
        s->level = value;
        return MEMTX_OK;
    }
unsupported:
    qemu_log_mask(LOG_UNIMP, "dra7-mcspi: unsupported write 0x%"
                  HWADDR_PRIx " size=%u value=0x%" PRIx64 "\n",
                  offset, size, value);
    return MEMTX_ERROR;
}

static const MemoryRegionOps dra7_mcspi_ops = {
    .read_with_attrs = dra7_mcspi_read,
    .write_with_attrs = dra7_mcspi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static const VMStateDescription vmstate_dra7_mcspi = {
    .name = TYPE_DRA7_MCSPI,
    .unmigratable = true,
};

static void dra7_mcspi_init(Object *obj)
{
    DRA7McSPIState *s = DRA7_MCSPI(obj);

    memory_region_init_io(&s->mmio, obj, &dra7_mcspi_ops, s,
                          TYPE_DRA7_MCSPI, 0x200);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    for (unsigned i = 0; i < ARRAY_SIZE(s->channel); i++) {
        fifo8_create(&s->channel[i].tx, 128);
        fifo8_create(&s->channel[i].rx, 128);
    }
    s->clock_enabled = true;
}

static void dra7_mcspi_realize(DeviceState *dev, Error **errp)
{
    DRA7McSPIState *s = DRA7_MCSPI(dev);

    s->bus = ssi_create_bus(dev, s->bus_name);
}

static void dra7_mcspi_finalize(Object *obj)
{
    DRA7McSPIState *s = DRA7_MCSPI(obj);

    for (unsigned i = 0; i < ARRAY_SIZE(s->channel); i++) {
        fifo8_destroy(&s->channel[i].tx);
        fifo8_destroy(&s->channel[i].rx);
    }
}

static const Property dra7_mcspi_properties[] = {
    DEFINE_PROP_STRING("bus-name", DRA7McSPIState, bus_name),
};

static void dra7_mcspi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = dra7_mcspi_realize;
    dc->vmsd = &vmstate_dra7_mcspi;
    device_class_set_props(dc, dra7_mcspi_properties);
    device_class_set_legacy_reset(dc, dra7_mcspi_reset);
}

static const TypeInfo dra7_mcspi_info = {
    .name = TYPE_DRA7_MCSPI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DRA7McSPIState),
    .instance_init = dra7_mcspi_init,
    .instance_finalize = dra7_mcspi_finalize,
    .class_init = dra7_mcspi_class_init,
};

static void dra7_mcspi_register_types(void)
{
    type_register_static(&dra7_mcspi_info);
}

type_init(dra7_mcspi_register_types)

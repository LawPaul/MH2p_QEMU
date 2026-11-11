/*
 * DRA7 HS I2C: synchronous standard/fast-mode 7-bit master and 16-byte FIFOs.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/fifo8.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/i2c/i2c.h"
#include "hw/i2c/dra7_i2c.h"
#include "migration/vmstate.h"

OBJECT_DECLARE_SIMPLE_TYPE(DRA7I2CState, DRA7_I2C)

#define I2C_EN BIT(15)
#define MST BIT(10)
#define TRX BIT(9)
#define STP BIT(1)
#define STT BIT(0)
#define NACK BIT(1)
#define ARDY BIT(2)
#define RRDY BIT(3)
#define XRDY BIT(4)
#define BF BIT(8)
#define BB BIT(12)
#define RDR BIT(13)
#define XDR BIT(14)
#define EVENTS 0x6fff
/* DRA72x TRM SPRUIC2D Table 24-42, excluding reserved bits. */
#define WAKE_EVENTS 0x6f6f

struct DRA7I2CState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    I2CBus *bus;
    char *bus_name;
    qemu_irq irq;
    Fifo8 tx, rx;
    uint16_t sysconfig, status, enabled, wake_enabled, buffer, count, control;
    uint16_t own[4], address, prescaler, scl_low, scl_high, system_test;
    bool clock_enabled, active;
};

static void dra7_i2c_update(DRA7I2CState *s)
{
    qemu_set_irq(s->irq, s->clock_enabled && !!(s->status & s->enabled));
}

static void dra7_i2c_end(DRA7I2CState *s)
{
    i2c_end_transfer(s->bus);
    s->control &= ~(STP | MST);
    s->status |= BF;
}

static void dra7_i2c_pump(DRA7I2CState *s)
{
    s->status &= ~(RRDY | RDR | XRDY | XDR);
    if (s->active) {
        while (s->count) {
            if (s->control & TRX) {
                if (fifo8_is_empty(&s->tx)) {
                    break;
                }
                if (i2c_send(s->bus, fifo8_pop(&s->tx)) < 0) {
                    s->status |= NACK;
                    s->active = false;
                    dra7_i2c_end(s);
                    break;
                }
            } else {
                if (fifo8_is_full(&s->rx)) {
                    break;
                }
                fifo8_push(&s->rx, i2c_recv(s->bus));
                if (s->count == 1) {
                    i2c_nack(s->bus);
                }
            }
            s->count--;
        }
        if (s->active && !s->count) {
            s->active = false;
            s->status |= ARDY;
            if (s->control & STP) {
                dra7_i2c_end(s);
            }
        }
    }
    if (!fifo8_is_empty(&s->rx)) {
        s->status |= fifo8_num_used(&s->rx) > ((s->buffer >> 8) & 0x3f) ?
                     RRDY : RDR;
    }
    if (s->active && (s->control & TRX)) {
        s->status |= s->count > (s->buffer & 0x3f) ? XRDY : XDR;
    }
    dra7_i2c_update(s);
}

static void dra7_i2c_reset(DeviceState *dev)
{
    DRA7I2CState *s = DRA7_I2C(dev);

    i2c_end_transfer(s->bus);
    fifo8_reset(&s->tx);
    fifo8_reset(&s->rx);
    s->sysconfig = 1;
    s->status = s->enabled = s->wake_enabled = 0;
    s->buffer = s->count = s->control = 0;
    s->address = s->prescaler = s->scl_low = s->scl_high = 0;
    s->system_test = 0;
    memset(s->own, 0, sizeof(s->own));
    s->active = false;
    dra7_i2c_update(s);
}

bool dra7_i2c_set_clock(DeviceState *dev, bool enabled)
{
    DRA7I2CState *s = DRA7_I2C(dev);

    if (!enabled && i2c_bus_busy(s->bus)) {
        qemu_log_mask(LOG_UNIMP, "dra7-i2c: cannot gate an active I2C bus\n");
        return false;
    }
    s->clock_enabled = enabled;
    memory_region_set_enabled(&s->mmio, enabled);
    dra7_i2c_update(s);
    return true;
}

static MemTxResult dra7_i2c_read(void *opaque, hwaddr offset,
                                uint64_t *value, unsigned size,
                                MemTxAttrs attrs)
{
    DRA7I2CState *s = opaque;

    if (offset == 0x9c && (size == 1 || size == 2 || size == 4)) {
        if (fifo8_is_empty(&s->rx)) {
            goto unsupported;
        }
        *value = fifo8_pop(&s->rx);
        if (size > 1 && !fifo8_is_empty(&s->rx)) {
            *value |= (uint32_t)fifo8_pop(&s->rx) << 8;
        }
        dra7_i2c_pump(s);
        return MEMTX_OK;
    }
    if (size != 2 && size != 4) {
        goto unsupported;
    }
    switch (offset) {
    case 0x10: *value = s->sysconfig; break;
    case 0x24:
        *value = s->status | (i2c_bus_busy(s->bus) ? BB : 0);
        break;
    case 0x28: *value = s->status & s->enabled; break;
    case 0x2c:
    case 0x30: *value = s->enabled; break;
    case 0x34: *value = s->wake_enabled; break;
    case 0x38:
    case 0x3c:
    case 0x40:
    case 0x44:
    case 0x48:
    case 0x4c: *value = 0; break;
    case 0x90: *value = 1; break;
    case 0x94: *value = s->buffer; break;
    case 0x98: *value = s->count; break;
    case 0xa4: *value = s->control; break;
    case 0xa8: *value = s->own[0]; break;
    case 0xac: *value = s->address; break;
    case 0xb0: *value = s->prescaler; break;
    case 0xb4: *value = s->scl_low; break;
    case 0xb8: *value = s->scl_high; break;
    case 0xbc:
        if (i2c_bus_busy(s->bus)) {
            goto unsupported; /* Bit-level line sampling is not modeled. */
        }
        *value = 0x1e0 | s->system_test;
        break;
    case 0xc0:
        *value = 0x4000 | fifo8_num_used(&s->rx) << 8 | fifo8_num_used(&s->tx);
        break;
    case 0xc4:
    case 0xc8:
    case 0xcc: *value = s->own[(offset - 0xc0) / 4]; break;
    default: goto unsupported;
    }
    return MEMTX_OK;
unsupported:
    qemu_log_mask(LOG_UNIMP, "dra7-i2c: unsupported read 0x%"
                  HWADDR_PRIx " size=%u\n", offset, size);
    *value = 0;
    return MEMTX_ERROR;
}

static MemTxResult dra7_i2c_write(void *opaque, hwaddr offset,
                                 uint64_t value, unsigned size,
                                 MemTxAttrs attrs)
{
    DRA7I2CState *s = opaque;

    if (offset == 0x9c && (size == 1 || size == 2 || size == 4)) {
        unsigned bytes = size == 1 ? 1 : 2;

        if (fifo8_num_free(&s->tx) < bytes || !(s->control & I2C_EN)) {
            goto unsupported;
        }
        if (s->active && !(s->control & TRX)) {
            goto unsupported;
        }
        fifo8_push(&s->tx, value);
        if (bytes == 2) {
            fifo8_push(&s->tx, value >> 8);
        }
        dra7_i2c_pump(s);
        return MEMTX_OK;
    }
    if (size != 2 && size != 4) {
        goto unsupported;
    }
    value &= UINT16_MAX; /* These are 16-bit registers on a 32-bit interface. */
    switch (offset) {
    case 0x10:
        /* Wakeup remains dormant: I2C_WE sources are all disabled. */
        if (value & ~0x31fU) {
            goto unsupported;
        }
        if (value & 2) {
            dra7_i2c_reset(DEVICE(s));
        } else {
            s->sysconfig = value;
        }
        break;
    case 0x20:
        if (value) {
            goto unsupported;
        }
        break;
    case 0x24:
    case 0x28:
    case 0x2c:
    case 0x30:
        value &= EVENTS;
        if (offset == 0x24) {
            s->status |= value;
        } else if (offset == 0x28) {
            s->status &= ~value;
            /* Acknowledge cannot discard an ongoing FIFO-ready condition. */
            dra7_i2c_pump(s);
        } else if (offset == 0x2c) {
            s->enabled |= value;
        } else {
            s->enabled &= ~value;
        }
        break;
    case 0x34:
        if (value & ~WAKE_EVENTS) {
            goto unsupported;
        }
        /* Wake-event routing is not wired; retain configuration readback. */
        s->wake_enabled = value;
        break;
    case 0x38:
    case 0x3c:
    case 0x40:
    case 0x44:
    case 0x48:
    case 0x4c:
        if (value) {
            goto unsupported;
        }
        break;
    case 0x94:
        if ((value & 0x8080) || (value & 0x3f) > 15 ||
            ((value >> 8) & 0x3f) > 15) {
            goto unsupported;
        }
        s->buffer = value & 0x3f3f;
        if (value & BIT(6)) {
            fifo8_reset(&s->tx);
        }
        if (value & BIT(14)) {
            fifo8_reset(&s->rx);
        }
        dra7_i2c_pump(s);
        break;
    case 0x98:
        if (s->active) {
            goto unsupported;
        }
        s->count = value;
        break;
    case 0xa4:
        if (value & ~0x8603U) {
            goto unsupported;
        }
        if (s->active && (value & I2C_EN) &&
            (value & ~STP) != (s->control & ~STP)) {
            goto unsupported;
        }
        if ((value & STT) && (!(value & I2C_EN) || !(value & MST) ||
                              !s->count || s->active || s->address > 0x7f)) {
            goto unsupported;
        }
        if (!(value & I2C_EN)) {
            i2c_end_transfer(s->bus);
            s->status = 0;
            s->active = false;
            fifo8_reset(&s->tx);
            fifo8_reset(&s->rx);
        }
        s->control = value & ~STT;
        if (value & STT) {
            s->status &= ~(ARDY | NACK | BF);
            if (i2c_start_transfer(s->bus, s->address, !(value & TRX))) {
                s->status |= NACK;
                dra7_i2c_end(s);
            } else {
                s->active = true;
            }
        } else if ((value & STP) && !s->active && i2c_bus_busy(s->bus)) {
            dra7_i2c_end(s);
        }
        dra7_i2c_pump(s);
        break;
    case 0xa8:
        if (value & ~0xe3ffU) {
            goto unsupported;
        }
        s->own[0] = value;
        break;
    case 0xac:
        if (value > 0x3ff || s->active) {
            goto unsupported;
        }
        s->address = value;
        break;
    case 0xb0:
    case 0xb4:
    case 0xb8:
        if (value > 0xff || s->active) {
            goto unsupported;
        }
        if (offset == 0xb0) {
            s->prescaler = value;
        } else if (offset == 0xb4) {
            s->scl_low = value;
        } else {
            s->scl_high = value;
        }
        break;
    case 0xc4:
    case 0xc8:
    case 0xcc:
        if (value > 0x3ff) {
            goto unsupported;
        }
        s->own[(offset - 0xc0) / 4] = value;
        break;
    case 0xbc:
        if (value & ~0x41e0U) {
            goto unsupported; /* Loopback and bit-level test modes are absent. */
        }
        s->system_test = value & BIT(14);
        break;
    default: goto unsupported;
    }
    dra7_i2c_update(s);
    return MEMTX_OK;
unsupported:
    qemu_log_mask(LOG_UNIMP, "dra7-i2c: unsupported write 0x%"
                  HWADDR_PRIx " size=%u value=0x%" PRIx64 "\n",
                  offset, size, value);
    return MEMTX_ERROR;
}

static const MemoryRegionOps dra7_i2c_ops = {
    .read_with_attrs = dra7_i2c_read,
    .write_with_attrs = dra7_i2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static const VMStateDescription vmstate_dra7_i2c = {
    .name = TYPE_DRA7_I2C,
    .unmigratable = true,
};

static void dra7_i2c_init(Object *obj)
{
    DRA7I2CState *s = DRA7_I2C(obj);

    memory_region_init_io(&s->mmio, obj, &dra7_i2c_ops, s, TYPE_DRA7_I2C, 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    fifo8_create(&s->tx, 16);
    fifo8_create(&s->rx, 16);
    s->clock_enabled = true;
}

static void dra7_i2c_realize(DeviceState *dev, Error **errp)
{
    DRA7I2CState *s = DRA7_I2C(dev);

    s->bus = i2c_init_bus(dev, s->bus_name);
}

static const Property dra7_i2c_properties[] = {
    DEFINE_PROP_STRING("bus-name", DRA7I2CState, bus_name),
};

static void dra7_i2c_finalize(Object *obj)
{
    DRA7I2CState *s = DRA7_I2C(obj);

    fifo8_destroy(&s->tx);
    fifo8_destroy(&s->rx);
}

static void dra7_i2c_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, dra7_i2c_reset);
    device_class_set_props(dc, dra7_i2c_properties);
    dc->realize = dra7_i2c_realize;
    dc->vmsd = &vmstate_dra7_i2c;
}

static const TypeInfo dra7_i2c_info = {
    .name = TYPE_DRA7_I2C,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DRA7I2CState),
    .instance_init = dra7_i2c_init,
    .instance_finalize = dra7_i2c_finalize,
    .class_init = dra7_i2c_class_init,
};

static void dra7_i2c_register_types(void)
{
    type_register_static(&dra7_i2c_info);
}

type_init(dra7_i2c_register_types)

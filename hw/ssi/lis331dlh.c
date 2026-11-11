/*
 * LIS331DLH four-wire SPI and normal/low-power stationary acceleration model.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/ssi/ssi.h"
#include "migration/vmstate.h"

#define TYPE_LIS331DLH "lis331dlh"
OBJECT_DECLARE_SIMPLE_TYPE(LIS331DLHState, LIS331DLH)

struct LIS331DLHState {
    SSIPeripheral parent_obj;
    uint8_t registers[64];
    uint8_t command, address, read_bytes;
    bool command_pending;
    int64_t next_sample;
    int32_t acceleration[3];
};

static int64_t lis331dlh_period(LIS331DLHState *s)
{
    static const unsigned normal_rate[] = { 50, 100, 400, 1000 };
    static const int64_t low_power_period[] = {
        0, 0, 2000000000, 1000000000, 500000000, 200000000, 100000000,
    };
    unsigned mode = s->registers[0x20] >> 5;

    if (mode == 1) {
        return NANOSECONDS_PER_SECOND /
               normal_rate[(s->registers[0x20] >> 3) & 3];
    }
    return mode < ARRAY_SIZE(low_power_period) ? low_power_period[mode] : 0;
}

static void lis331dlh_status(LIS331DLHState *s)
{
    uint8_t status = s->registers[0x27] & 0x77;
    unsigned enabled = s->registers[0x20] & 7;

    if (enabled && (status & enabled) == enabled) {
        status |= 8;
    }
    if (status & 0x70) {
        status |= 0x80;
    }
    s->registers[0x27] = status;
}

static void lis331dlh_sample(LIS331DLHState *s, bool missed)
{
    static const unsigned sensitivity[] = { 1000, 2000, 0, 3900 };
    unsigned scale = (s->registers[0x23] >> 4) & 3;

    for (unsigned axis = 0; axis < 3; axis++) {
        unsigned address = 0x28 + 2 * axis;
        int64_t sample;
        uint16_t output;

        if (!(s->registers[0x20] & BIT(axis))) {
            continue;
        }
        /*
         * BDU protects each axis until both bytes have been read. Samples
         * blocked by BDU do not overwrite registers or raise overrun.
         */
        if ((s->registers[0x23] & 0x80) &&
            (s->registers[0x27] & BIT(axis))) {
            continue;
        }
        if ((s->registers[0x27] & BIT(axis)) ||
            (missed && !(s->registers[0x23] & 0x80))) {
            s->registers[0x27] |= BIT(axis + 4);
        }
        sample = (int64_t)s->acceleration[axis] * 1000 / sensitivity[scale];
        output = (uint16_t)(MAX(-2048, MIN(2047, sample)) * 16);
        s->registers[address] = s->registers[0x23] & 0x40 ?
                                output >> 8 : output;
        s->registers[address + 1] = s->registers[0x23] & 0x40 ?
                                    output : output >> 8;
        s->registers[0x27] |= BIT(axis);
        s->read_bytes &= ~(3 << (2 * axis));
    }
    lis331dlh_status(s);
}

static void lis331dlh_sync(LIS331DLHState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t period = lis331dlh_period(s);

    if (period && now >= s->next_sample) {
        int64_t elapsed = (now - s->next_sample) / period;

        lis331dlh_sample(s, elapsed != 0);
        s->next_sample += (elapsed + 1) * period;
    }
}

static uint8_t lis331dlh_read(LIS331DLHState *s)
{
    unsigned address = s->address;
    uint8_t value;

    lis331dlh_sync(s);
    switch (address) {
    case 0x0f:
    case 0x20 ... 0x24:
    case 0x26 ... 0x27:
    case 0x30 ... 0x37:
        return s->registers[address];
    case 0x25:
        return 0; /* No high-pass state when the filter is disabled. */
    case 0x28 ... 0x2d: {
        unsigned axis = (address - 0x28) / 2;
        unsigned pair = 3 << (axis * 2);

        value = s->registers[address];
        s->read_bytes |= BIT(address - 0x28);
        if ((s->read_bytes & pair) == pair) {
            s->registers[0x27] &= ~(BIT(axis) | BIT(axis + 4));
            lis331dlh_status(s);
        }
        return value;
    }
    default:
        qemu_log_mask(LOG_UNIMP, "lis331dlh: unsupported register read 0x%x\n",
                      address);
        return 0xff;
    }
}

static void lis331dlh_write(LIS331DLHState *s, uint8_t value)
{
    unsigned address = s->address;

    lis331dlh_sync(s);
    switch (address) {
    case 0x20:
        if ((value >> 5) == 7) {
            goto unsupported;
        }
        s->registers[address] = value;
        s->next_sample = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                         lis331dlh_period(s);
        return;
    case 0x21:
        if (value & 0x7f) {
            goto unsupported; /* High-pass filtering is not implemented. */
        }
        /* BOOT reloads internal trim, not application configuration. */
        s->registers[address] = 0;
        return;
    case 0x22:
    case 0x24:
    case 0x30:
    case 0x34:
        if (value) {
            goto unsupported; /* Interrupt/sleep-to-wake engines are absent. */
        }
        s->registers[address] = value;
        return;
    case 0x23:
        if ((value & ~0xf0) || (value & 0x30) == 0x20) {
            goto unsupported; /* Self-test and three-wire SPI are absent. */
        }
        s->registers[address] = value;
        return;
    case 0x26:
        s->registers[address] = value;
        return;
    case 0x32 ... 0x33:
    case 0x36 ... 0x37:
        s->registers[address] = value & 0x7f;
        return;
    default:
        goto unsupported;
    }

unsupported:
    qemu_log_mask(LOG_UNIMP, "lis331dlh: unsupported register write "
                  "0x%x = 0x%x\n", address, value);
}

static uint32_t lis331dlh_transfer(SSIPeripheral *dev, uint32_t value)
{
    LIS331DLHState *s = LIS331DLH(dev);
    uint8_t result = 0;

    if (value > 0xff) {
        qemu_log_mask(LOG_UNIMP, "lis331dlh: SPI transfer wider than a byte\n");
        return 0xff;
    }
    if (s->command_pending) {
        s->command = value;
        s->address = value & 0x3f;
        s->command_pending = false;
        return 0;
    }
    if (s->command & 0x80) {
        result = lis331dlh_read(s);
    } else {
        lis331dlh_write(s, value);
    }
    if (s->command & 0x40) {
        s->address = (s->address + 1) & 0x3f;
    }
    return result;
}

static int lis331dlh_select(SSIPeripheral *dev, bool deselected)
{
    LIS331DLHState *s = LIS331DLH(dev);

    if (deselected) {
        s->command_pending = true;
    }
    return 0;
}

static void lis331dlh_reset(DeviceState *dev)
{
    LIS331DLHState *s = LIS331DLH(dev);

    memset(s->registers, 0, sizeof(s->registers));
    s->registers[0x0f] = 0x32;
    s->registers[0x20] = 7;
    s->command_pending = true;
    s->command = s->address = s->read_bytes = 0;
    s->next_sample = 0;
}

static void lis331dlh_realize(SSIPeripheral *dev, Error **errp)
{
    LIS331DLHState *s = LIS331DLH(dev);

    for (unsigned i = 0; i < 3; i++) {
        if (s->acceleration[i] < -8000 || s->acceleration[i] > 8000) {
            error_setg(errp,
                       "lis331dlh: acceleration must be within +/-8000 mg");
            return;
        }
    }
}

static const Property lis331dlh_properties[] = {
    DEFINE_PROP_INT32("x-mg", LIS331DLHState, acceleration[0], 0),
    DEFINE_PROP_INT32("y-mg", LIS331DLHState, acceleration[1], 0),
    DEFINE_PROP_INT32("z-mg", LIS331DLHState, acceleration[2], 1000),
};

static const VMStateDescription vmstate_lis331dlh = {
    .name = TYPE_LIS331DLH,
    .unmigratable = true,
};

static void lis331dlh_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SSIPeripheralClass *sc = SSI_PERIPHERAL_CLASS(klass);

    sc->realize = lis331dlh_realize;
    sc->transfer = lis331dlh_transfer;
    sc->set_cs = lis331dlh_select;
    sc->cs_polarity = SSI_CS_LOW;
    dc->vmsd = &vmstate_lis331dlh;
    device_class_set_props(dc, lis331dlh_properties);
    device_class_set_legacy_reset(dc, lis331dlh_reset);
}

static const TypeInfo lis331dlh_info = {
    .name = TYPE_LIS331DLH,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(LIS331DLHState),
    .class_init = lis331dlh_class_init,
};

static void lis331dlh_register_types(void)
{
    type_register_static(&lis331dlh_info);
}

type_init(lis331dlh_register_types)

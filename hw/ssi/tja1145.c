/*
 * NXP TJA1145 SPI control plane, with fixed valid supplies.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "hw/core/qdev-properties.h"
#include "hw/ssi/ssi.h"
#include "migration/vmstate.h"
#include "net/can_emu.h"

#define TYPE_TJA1145 "tja1145"
OBJECT_DECLARE_SIMPLE_TYPE(TJA1145State, TJA1145)

struct TJA1145State {
    SSIPeripheral parent_obj;
    uint8_t registers[128];
    uint8_t command, data[3];
    unsigned length;
    CanBusState *canbus;
    CanBusClientState bus_client;
    QEMUTimer *silence_timer;
    bool bus_silent, experimental_frame_wakeup;
};

static uint8_t tja1145_events(TJA1145State *s)
{
    return (!!s->registers[0x61]) |
           (!!s->registers[0x63] << 2) |
           (!!s->registers[0x64] << 3);
}

static uint8_t tja1145_read(TJA1145State *s, unsigned address)
{
    switch (address) {
    case 0x22:
        /* PNCOK is unset: no configured partial-network wake-up filter. */
        return 0x40 | (s->bus_silent ? 8 : 0) |
                      ((s->registers[1] == 7 &&
                       (s->registers[0x20] & 3) &&
                       (s->registers[0x20] & 3) != 3) ? 0x80 : 0);
    case 0x60:
        return tja1145_events(s);
    case 0x7e:
        return 0x70;
    default:
        return address < ARRAY_SIZE(s->registers) ?
               s->registers[address] : 0;
    }
}

static bool tja1145_locked(TJA1145State *s, unsigned address)
{
    unsigned group;

    if (address >= 6 && address <= 9) {
        group = 0;
    } else if (address >= 0x10 && address <= 0x5f) {
        group = address >> 4;
    } else if (address >= 0x68 && address <= 0x6f) {
        group = 6;
    } else {
        return false;
    }
    return s->registers[0xa] & BIT(group);
}

static void tja1145_write(TJA1145State *s, unsigned address, uint8_t value)
{
    if (tja1145_locked(s, address)) {
        return;
    }
    switch (address) {
    case 1:
        value &= 7;
        if (value != 1 && value != 4 && value != 7) {
            goto unsupported;
        }
        if (value == 1 && (tja1145_events(s) ||
                          !(s->registers[0x23] & 1 ||
                            s->registers[0x4c] & 3))) {
            value = 4;
        }
        if (value == 7) {
            s->registers[3] &= ~0x20;
        }
        s->registers[1] = value;
        break;
    case 4: s->registers[address] = value & 6; break;
    case 6 ... 9: s->registers[address] = value; break;
    case 0xa: s->registers[address] = value & 0x7f; break;
    case 0x20:
        if (value & 0x30) {
            goto unsupported;
        }
        s->registers[address] = value & 3;
        break;
    case 0x23: s->registers[address] = value & 0x13; break;
    case 0x4c: s->registers[address] = value & 3; break;
    case 0x61: s->registers[address] &= ~(value & 0x16); break;
    case 0x63: s->registers[address] &= ~(value & 0x33); break;
    case 0x64: s->registers[address] &= ~(value & 3); break;
    case 0x26 ... 0x2f:
    case 0x68 ... 0x6f:
        goto unsupported;
    default:
        /* Writes to reserved and read-only addresses have no effect. */
        break;
    }
    return;

unsupported:
    qemu_log_mask(LOG_UNIMP, "tja1145: unsupported register write "
                  "0x%x = 0x%x\n", address, value);
    s->registers[0x61] |= 2;
}

static int tja1145_select(SSIPeripheral *dev, bool deselected)
{
    TJA1145State *s = TJA1145(dev);

    if (deselected && s->length) {
        if (!(s->command & 1)) {
            if (s->length < 2 || s->length > 4) {
                s->registers[0x61] |= 2;
            } else {
                for (unsigned i = 1; i < s->length; i++) {
                    tja1145_write(s, (s->command >> 1) + i - 1,
                                 s->data[i - 1]);
                }
            }
        }
        s->length = 0;
    }
    return 0;
}

static uint32_t tja1145_transfer(SSIPeripheral *dev, uint32_t value)
{
    TJA1145State *s = TJA1145(dev);
    uint8_t result = 0;

    if (!s->length) {
        s->command = value;
    } else if (s->length <= 3) {
        result = tja1145_read(s, (s->command >> 1) + s->length - 1);
        s->data[s->length - 1] = value;
    } else if (s->command & 1) {
        /* Read transactions reflect SDI from clock 33 onward. */
        result = value;
    }
    if (s->length < 5) {
        s->length++;
    }
    return result;
}

static void tja1145_reset(DeviceState *dev)
{
    TJA1145State *s = TJA1145(dev);

    memset(s->registers, 0, sizeof(s->registers));
    s->registers[1] = 4;
    s->registers[3] = 0x20;
    s->registers[0x26] = 5;
    s->registers[0x61] = 0x10;
    s->registers[0x2f] = 0x40;
    memset(s->registers + 0x68, 0xff, 8);
    s->length = 0;
    s->bus_silent = true;
    timer_del(s->silence_timer);
}

static void tja1145_silence(void *opaque)
{
    TJA1145State *s = opaque;

    s->bus_silent = true;
    if (s->registers[0x23] & 0x10) {
        s->registers[0x63] |= 0x10;
    }
}

static bool tja1145_can_receive(CanBusClientState *client)
{
    return true;
}

static ssize_t tja1145_receive(CanBusClientState *client,
                               const qemu_can_frame *frames, size_t count)
{
    TJA1145State *s = container_of(client, TJA1145State, bus_client);

    for (size_t i = 0; i < count; i++) {
        if (frames[i].can_dlc > 8 || frames[i].flags ||
            (frames[i].can_id & QEMU_CAN_ERR_FLAG)) {
            qemu_log_mask(LOG_UNIMP, "tja1145: unsupported CAN bus frame\n");
            continue;
        }
        s->bus_silent = false;
        timer_mod(s->silence_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  NANOSECONDS_PER_SECOND);
        /*
         * Explicit frame-level experiment: a decoded frame stands for a
         * qualifying physical wake pattern. Pulse filtering is not modeled.
         */
        if (s->experimental_frame_wakeup && !(s->registers[0x20] & 3) &&
            (s->registers[0x23] & 1)) {
            s->registers[0x63] |= 1;
            if (s->registers[1] == 1) {
                s->registers[1] = 4;
            }
        }
    }
    return 0; /* A transceiver alone cannot acknowledge a CAN transmission. */
}

static CanBusClientInfo tja1145_bus_info = {
    .can_receive = tja1145_can_receive,
    .receive = tja1145_receive,
};

static void tja1145_realize(SSIPeripheral *dev, Error **errp)
{
    TJA1145State *s = TJA1145(dev);

    s->silence_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, tja1145_silence, s);
    s->bus_client.info = &tja1145_bus_info;
    if (s->canbus) {
        can_bus_insert_client(s->canbus, &s->bus_client);
    }
}

static void tja1145_unrealize(DeviceState *dev)
{
    TJA1145State *s = TJA1145(dev);

    can_bus_remove_client(&s->bus_client);
    timer_free(s->silence_timer);
}

static const Property tja1145_properties[] = {
    DEFINE_PROP_LINK("canbus", TJA1145State, canbus, TYPE_CAN_BUS,
                     CanBusState *),
    DEFINE_PROP_BOOL("experimental-frame-wakeup", TJA1145State,
                     experimental_frame_wakeup, false),
};

static const VMStateDescription vmstate_tja1145 = {
    .name = TYPE_TJA1145,
    .unmigratable = true,
};

static void tja1145_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SSIPeripheralClass *sc = SSI_PERIPHERAL_CLASS(klass);

    sc->realize = tja1145_realize;
    sc->transfer = tja1145_transfer;
    sc->set_cs = tja1145_select;
    sc->cs_polarity = SSI_CS_LOW;
    dc->vmsd = &vmstate_tja1145;
    dc->unrealize = tja1145_unrealize;
    device_class_set_props(dc, tja1145_properties);
    device_class_set_legacy_reset(dc, tja1145_reset);
}

static const TypeInfo tja1145_info = {
    .name = TYPE_TJA1145,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(TJA1145State),
    .class_init = tja1145_class_init,
};

static void tja1145_register_types(void)
{
    type_register_static(&tja1145_info);
}

type_init(tja1145_register_types)

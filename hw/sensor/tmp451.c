/*
 * TI TMP451 remote/local temperature sensor (LM90 register family).
 *
 * The Tegra K1 thermal monitor (NVIDIA devg-nvtmon on MH2p, lm90 on Linux)
 * reads the configuration and status registers, selects the extended
 * (-64..191 C) range and programs the alert/therm limits.  The die and
 * board temperatures are fixed, settable with the "local-temp" and
 * "remote-temp" properties (milli-degrees C).
 *
 * Register map (TMP451 datasheet, SBOS686): reads 0x03..0x08 are written
 * at 0x09..0x0e; 0x0f is the one-shot trigger.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_TMP451 "tmp451"
OBJECT_DECLARE_SIMPLE_TYPE(TMP451State, TMP451)

enum {
    R_LOCAL_TEMP      = 0x00,
    R_REMOTE_TEMP_HI  = 0x01,
    R_STATUS          = 0x02,
    R_CONFIG          = 0x03,
    R_CONV_RATE       = 0x04,
    R_LOCAL_HIGH      = 0x05,
    R_LOCAL_LOW       = 0x06,
    R_REMOTE_HIGH_HI  = 0x07,
    R_REMOTE_LOW_HI   = 0x08,
    W_CONFIG          = 0x09,
    W_LOCAL_LOW       = 0x0e,
    W_ONE_SHOT        = 0x0f,
    R_REMOTE_TEMP_LO  = 0x10,
    R_LOCAL_TEMP_LO   = 0x15,
    R_MANUFACTURER_ID = 0xfe,
};

#define CONFIG_RANGE    (1u << 2)    /* extended range: offset by 64 C */

struct TMP451State {
    I2CSlave parent_obj;

    uint8_t regs[256];
    uint8_t pointer;
    bool have_pointer;

    int32_t local_mc;
    int32_t remote_mc;
};

/* Temperature byte pair (integer, fraction in bits 7:4) for the range. */
static void tmp451_encode(TMP451State *s, int32_t mc, uint8_t *hi, uint8_t *lo)
{
    bool ext = s->regs[R_CONFIG] & CONFIG_RANGE;
    int32_t min = ext ? -64000 : 0, max = ext ? 191937 : 127937;
    int32_t v = MIN(MAX(mc, min), max) + (ext ? 64000 : 0);

    *hi = v / 1000;
    *lo = ((v % 1000) * 16 / 1000) << 4;
}

static uint8_t tmp451_read(TMP451State *s, uint8_t reg)
{
    uint8_t hi, lo;

    switch (reg) {
    case R_LOCAL_TEMP:
    case R_LOCAL_TEMP_LO:
        tmp451_encode(s, s->local_mc, &hi, &lo);
        return reg == R_LOCAL_TEMP ? hi : lo;
    case R_REMOTE_TEMP_HI:
    case R_REMOTE_TEMP_LO:
        tmp451_encode(s, s->remote_mc, &hi, &lo);
        return reg == R_REMOTE_TEMP_HI ? hi : lo;
    case R_STATUS:
        return 0;    /* not busy, no open diode, no limit exceeded */
    default:
        return s->regs[reg];
    }
}

static void tmp451_write(TMP451State *s, uint8_t reg, uint8_t value)
{
    if (reg >= W_CONFIG && reg <= W_LOCAL_LOW) {
        s->regs[reg - (W_CONFIG - R_CONFIG)] = value;
        return;
    }
    switch (reg) {
    case W_ONE_SHOT:
        return;
    case R_LOCAL_TEMP ... R_STATUS:
    case R_REMOTE_TEMP_LO:
    case R_LOCAL_TEMP_LO:
    case R_MANUFACTURER_ID:
        qemu_log_mask(LOG_GUEST_ERROR, "tmp451: write to read-only 0x%02x\n",
                      reg);
        return;
    default:
        s->regs[reg] = value;
    }
}

static int tmp451_event(I2CSlave *i2c, enum i2c_event event)
{
    TMP451State *s = TMP451(i2c);

    if (event == I2C_START_SEND) {
        s->have_pointer = false;
    }
    return 0;
}

static uint8_t tmp451_recv(I2CSlave *i2c)
{
    TMP451State *s = TMP451(i2c);

    return tmp451_read(s, s->pointer++);
}

static int tmp451_send(I2CSlave *i2c, uint8_t data)
{
    TMP451State *s = TMP451(i2c);

    if (!s->have_pointer) {
        s->pointer = data;
        s->have_pointer = true;
    } else {
        tmp451_write(s, s->pointer++, data);
    }
    return 0;
}

static void tmp451_reset(DeviceState *dev)
{
    TMP451State *s = TMP451(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[R_CONV_RATE] = 0x08;           /* 16 conversions/s */
    s->regs[R_LOCAL_HIGH] = 0x55;          /* 85 C */
    s->regs[R_REMOTE_HIGH_HI] = 0x55;
    s->regs[0x19] = 0x6c;                  /* remote THERM limit 108 C */
    s->regs[0x20] = 0x6c;                  /* local THERM limit */
    s->regs[0x21] = 0x0a;                  /* THERM hysteresis */
    s->regs[0x22] = 0x01;                  /* consecutive alert */
    s->regs[0x24] = 0x00;                  /* digital filter off */
    s->regs[R_MANUFACTURER_ID] = 0x55;     /* Texas Instruments */
    s->pointer = 0;
    s->have_pointer = false;
}

static const VMStateDescription vmstate_tmp451 = {
    .name = TYPE_TMP451,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, TMP451State, 256),
        VMSTATE_UINT8(pointer, TMP451State),
        VMSTATE_BOOL(have_pointer, TMP451State),
        VMSTATE_I2C_SLAVE(parent_obj, TMP451State),
        VMSTATE_END_OF_LIST()
    }
};

static const Property tmp451_properties[] = {
    DEFINE_PROP_INT32("local-temp", TMP451State, local_mc, 42000),
    DEFINE_PROP_INT32("remote-temp", TMP451State, remote_mc, 45000),
};

static void tmp451_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    dc->desc = "TI TMP451 temperature sensor";
    device_class_set_legacy_reset(dc, tmp451_reset);
    device_class_set_props(dc, tmp451_properties);
    dc->vmsd = &vmstate_tmp451;
    k->event = tmp451_event;
    k->recv = tmp451_recv;
    k->send = tmp451_send;
}

static const TypeInfo tmp451_info = {
    .name          = TYPE_TMP451,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(TMP451State),
    .class_init    = tmp451_class_init,
};

static void tmp451_register_types(void)
{
    type_register_static(&tmp451_info);
}

type_init(tmp451_register_types)

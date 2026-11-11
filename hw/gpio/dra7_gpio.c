/*
 * DRA7 / OMAP4-style GPIO, without pad muxing, wakeup or debounce timing.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/gpio/dra7_gpio.h"
#include "migration/vmstate.h"
#include "qom/object.h"

OBJECT_DECLARE_SIMPLE_TYPE(DRA7GPIOState, DRA7_GPIO)

struct DRA7GPIOState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq irq[2];
    qemu_irq pins[32];
    uint32_t sysconfig;
    uint32_t ctrl;
    uint32_t oe;
    uint32_t input;
    uint32_t output;
    uint32_t status[2];
    uint32_t enable[2];
    uint32_t low;
    uint32_t high;
    uint32_t rising;
    uint32_t falling;
};

static uint32_t dra7_gpio_pending(DRA7GPIOState *s, unsigned index)
{
    uint32_t levels = (~s->input & s->low) | (s->input & s->high);

    return s->ctrl & 1 ? 0 : s->status[index] | levels;
}

static void dra7_gpio_update(DRA7GPIOState *s)
{
    for (unsigned i = 0; i < 2; i++) {
        qemu_set_irq(s->irq[i], !!(dra7_gpio_pending(s, i) & s->enable[i]));
    }
    for (unsigned i = 0; i < 32; i++) {
        qemu_set_irq(s->pins[i],
                     !(s->ctrl & 1) && !(s->oe & BIT(i)) &&
                     (s->output & BIT(i)));
    }
}

static void dra7_gpio_input(void *opaque, int line, int level)
{
    DRA7GPIOState *s = opaque;
    uint32_t old = s->input;
    uint32_t edges;

    s->input = deposit32(s->input, line, 1, !!level);
    edges = (~old & s->input & s->rising) | (old & ~s->input & s->falling);
    if (!(s->ctrl & 1)) {
        s->status[0] |= edges;
        s->status[1] |= edges;
    }
    dra7_gpio_update(s);
}

static void dra7_gpio_reset(DeviceState *dev)
{
    DRA7GPIOState *s = DRA7_GPIO(dev);

    s->sysconfig = 0;
    s->ctrl = 0;
    s->oe = UINT32_MAX;
    s->output = 0;
    s->status[0] = s->status[1] = 0;
    s->enable[0] = s->enable[1] = 0;
    s->low = s->high = s->rising = s->falling = 0;
    dra7_gpio_update(s);
}

static MemTxResult dra7_gpio_read(void *opaque, hwaddr offset,
                                 uint64_t *value, unsigned size,
                                 MemTxAttrs attrs)
{
    DRA7GPIOState *s = opaque;

    switch (offset) {
    case 0x10:
        *value = s->sysconfig;
        break;
    case 0x24: case 0x28:
        *value = dra7_gpio_pending(s, (offset - 0x24) / 4);
        break;
    case 0x2c: case 0x30:
        *value = dra7_gpio_pending(s, (offset - 0x2c) / 4) &
                 s->enable[(offset - 0x2c) / 4];
        break;
    case 0x34: case 0x38:
        *value = s->enable[(offset - 0x34) / 4];
        break;
    case 0x3c: case 0x40:
        *value = s->enable[(offset - 0x3c) / 4];
        break;
    case 0x114:
        *value = 1; /* Supported resets complete synchronously. */
        break;
    case 0x130:
        *value = s->ctrl;
        break;
    case 0x134:
        *value = s->oe;
        break;
    case 0x138:
        *value = s->input;
        break;
    case 0x13c:
        *value = s->output;
        break;
    case 0x140:
        *value = s->low;
        break;
    case 0x144:
        *value = s->high;
        break;
    case 0x148:
        *value = s->rising;
        break;
    case 0x14c:
        *value = s->falling;
        break;
    case 0x150:
        *value = 0; /* Debounce is disabled; enabling it is unsupported. */
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "dra7-gpio: unsupported read 0x%"
                      HWADDR_PRIx "\n", offset);
        *value = 0;
        return MEMTX_ERROR;
    }
    return MEMTX_OK;
}

static MemTxResult dra7_gpio_write(void *opaque, hwaddr offset,
                                  uint64_t value, unsigned size,
                                  MemTxAttrs attrs)
{
    DRA7GPIOState *s = opaque;

    switch (offset) {
    case 0x10:
        if (value & ~0x1b) {
            goto unsupported;
        }
        if (value & 2) {
            dra7_gpio_reset(DEVICE(s));
        }
        s->sysconfig = value & ~2;
        break;
    case 0x24: case 0x28:
        s->status[(offset - 0x24) / 4] |= value;
        break;
    case 0x2c: case 0x30:
        s->status[(offset - 0x2c) / 4] &= ~value;
        break;
    case 0x34: case 0x38:
        s->enable[(offset - 0x34) / 4] |= value;
        break;
    case 0x3c: case 0x40:
        s->enable[(offset - 0x3c) / 4] &= ~value;
        break;
    case 0x130:
        if (value & ~7) {
            goto unsupported;
        }
        s->ctrl = value;
        break;
    case 0x134:
        s->oe = value;
        break;
    case 0x13c:
        s->output = value;
        break;
    case 0x140:
        s->low = value;
        break;
    case 0x144:
        s->high = value;
        break;
    case 0x148:
        s->rising = value;
        break;
    case 0x14c:
        s->falling = value;
        break;
    case 0x190:
        s->output &= ~value;
        break;
    case 0x194:
        s->output |= value;
        break;
    case 0x150:
        if (value) {
            goto unsupported;
        }
        break;
    default:
        goto unsupported;
    }
    dra7_gpio_update(s);
    return MEMTX_OK;

unsupported:
    qemu_log_mask(LOG_UNIMP, "dra7-gpio: unsupported write 0x%"
                  HWADDR_PRIx " = 0x%" PRIx64 "\n", offset, value);
    return MEMTX_ERROR;
}

static const MemoryRegionOps dra7_gpio_ops = {
    .read_with_attrs = dra7_gpio_read,
    .write_with_attrs = dra7_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static int dra7_gpio_post_load(void *opaque, int version_id)
{
    dra7_gpio_update(opaque);
    return 0;
}

static const VMStateDescription vmstate_dra7_gpio = {
    .name = TYPE_DRA7_GPIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = dra7_gpio_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(sysconfig, DRA7GPIOState),
        VMSTATE_UINT32(ctrl, DRA7GPIOState),
        VMSTATE_UINT32(oe, DRA7GPIOState),
        VMSTATE_UINT32(input, DRA7GPIOState),
        VMSTATE_UINT32(output, DRA7GPIOState),
        VMSTATE_UINT32_ARRAY(status, DRA7GPIOState, 2),
        VMSTATE_UINT32_ARRAY(enable, DRA7GPIOState, 2),
        VMSTATE_UINT32(low, DRA7GPIOState),
        VMSTATE_UINT32(high, DRA7GPIOState),
        VMSTATE_UINT32(rising, DRA7GPIOState),
        VMSTATE_UINT32(falling, DRA7GPIOState),
        VMSTATE_END_OF_LIST()
    },
};

static void dra7_gpio_init(Object *obj)
{
    DRA7GPIOState *s = DRA7_GPIO(obj);

    memory_region_init_io(&s->mmio, obj, &dra7_gpio_ops, s, TYPE_DRA7_GPIO,
                          0x200);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    for (unsigned i = 0; i < 2; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq[i]);
    }
    qdev_init_gpio_in(DEVICE(obj), dra7_gpio_input, 32);
    qdev_init_gpio_out_named(DEVICE(obj), s->pins, "pins", 32);
}

static void dra7_gpio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, dra7_gpio_reset);
    dc->vmsd = &vmstate_dra7_gpio;
}

static const TypeInfo dra7_gpio_info = {
    .name = TYPE_DRA7_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DRA7GPIOState),
    .instance_init = dra7_gpio_init,
    .class_init = dra7_gpio_class_init,
};

static void dra7_gpio_register_types(void)
{
    type_register_static(&dra7_gpio_info);
}

type_init(dra7_gpio_register_types)

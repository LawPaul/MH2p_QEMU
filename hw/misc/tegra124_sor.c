/*
 * Experimental T124 SOR configuration, without link or frame completion.
 * Register fields follow NVIDIA's public T124 SOR definitions.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/tegra124_sor.h"

struct Tegra124SorState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t regs[0x80];
    bool experimental_configuration;
    bool reset_asserted;
    bool clock_enabled;
};

static uint32_t sor_config_mask(unsigned index)
{
    static const uint32_t masks[] = {
        [0x13] = 0x7f, /* Clock selection and link speed. */
        [0x17] = 0x0f000ff5,
        [0x18] = 0x03f01f00,
        [0x19] = 0x03c60001,
        [0x1a] = 0x2000,
        [0x5c ... 0x5d] = UINT32_MAX,
        [0x60 ... 0x61] = UINT32_MAX, /* DP_SPARE0/1 */
    };

    return index < ARRAY_SIZE(masks) ? masks[index] : 0;
}

static bool sor_accessible(Tegra124SorState *s)
{
    return s->experimental_configuration && !s->reset_asserted &&
           s->clock_enabled;
}

static MemTxResult sor_read(void *opaque, hwaddr addr, uint64_t *value,
                            unsigned size, MemTxAttrs attrs)
{
    Tegra124SorState *s = opaque;

    if (sor_accessible(s) &&
        (sor_config_mask(addr / 4) || addr == 0x54)) {
        *value = s->regs[addr / 4];
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP, "tegra124-sor: unsupported read at 0x%"
                  HWADDR_PRIx " (enabled=%d reset=%d clock=%d)\n", addr,
                  s->experimental_configuration, s->reset_asserted,
                  s->clock_enabled);
    return MEMTX_ERROR;
}

static MemTxResult sor_write(void *opaque, hwaddr addr, uint64_t value,
                             unsigned size, MemTxAttrs attrs)
{
    Tegra124SorState *s = opaque;
    uint32_t mask = sor_config_mask(addr / 4);

    if (sor_accessible(s) && mask && !(value & ~mask)) {
        s->regs[addr / 4] = value;
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP, "tegra124-sor: unsupported write at 0x%"
                  HWADDR_PRIx " = 0x%" PRIx64
                  " (enabled=%d reset=%d clock=%d)\n", addr, value,
                  s->experimental_configuration, s->reset_asserted,
                  s->clock_enabled);
    return MEMTX_ERROR;
}

static const MemoryRegionOps sor_ops = {
    .read_with_attrs = sor_read,
    .write_with_attrs = sor_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void sor_reset(DeviceState *dev)
{
    Tegra124SorState *s = TEGRA124_SOR(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void sor_reset_line(void *opaque, int n, int level)
{
    Tegra124SorState *s = opaque;

    if (level && !s->reset_asserted) {
        sor_reset(DEVICE(s));
    }
    s->reset_asserted = level;
}

static void sor_clock_line(void *opaque, int n, int level)
{
    Tegra124SorState *s = opaque;

    s->clock_enabled = level;
}

static void sor_init(Object *obj)
{
    Tegra124SorState *s = TEGRA124_SOR(obj);

    memory_region_init_io(&s->iomem, obj, &sor_ops, s, TYPE_TEGRA124_SOR,
                          0x40000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    qdev_init_gpio_in_named(DEVICE(obj), sor_reset_line, "reset", 1);
    qdev_init_gpio_in_named(DEVICE(obj), sor_clock_line, "clock-enable", 1);
}

static const Property sor_properties[] = {
    DEFINE_PROP_BOOL("experimental-configuration", Tegra124SorState,
                     experimental_configuration, false),
};

static void sor_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, sor_properties);
    device_class_set_legacy_reset(dc, sor_reset);
}

static const TypeInfo sor_info = {
    .name = TYPE_TEGRA124_SOR,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Tegra124SorState),
    .instance_init = sor_init,
    .class_init = sor_class_init,
};

static void sor_register_types(void)
{
    type_register_static(&sor_info);
}

type_init(sor_register_types)

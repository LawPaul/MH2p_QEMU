/*
 * Tegra K1 (T124) register blocks that a direct-start machine needs but
 * whose analogue side is not modelled:
 *
 *  - EMC (external memory controller, 0x7001b000): timing registers are
 *    plain storage (the DRAM itself is ideal), TIMING_CONTROL updates
 *    complete immediately, EMC_STATUS reports idle and INTSTATUS reports a
 *    completed clock change.  Thermal DRAM derating (EMC_REFRESH) and DVFS
 *    frequency switches therefore succeed without timing effects.
 *
 *  - SOC_THERM (0x700e2000): the on-die sensors (CPU0-3, MEM0-1, GPU, PLLX)
 *    report valid captures and a fixed die temperature; throttling and
 *    interrupt configuration is storage, and no alarm ever fires.
 *
 *  - CL-DVFS / DFLL (0x70110000): the CPU closed-loop clock source.  Its
 *    configuration, I2C PMIC control and voltage LUT are storage; the
 *    frequency monitor always has a fresh sample equal to the request and
 *    the I2C voltage interface is never busy.
 *
 * Register offsets follow the T124 TRM and the Linux tegra124 drivers
 * (tegra124-emc.c, soctherm.c, clk-dfll.c).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TEGRA124_REGBLOCK_SIZE 0x1000

typedef struct Tegra124RegBlockState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t regs[TEGRA124_REGBLOCK_SIZE / 4];
    int32_t temp_mc;                     /* SOC_THERM die temperature */
} Tegra124RegBlockState;

typedef struct Tegra124RegBlockClass {
    SysBusDeviceClass parent_class;
    const char *name;
    uint64_t (*read)(Tegra124RegBlockState *s, hwaddr offset);
    void (*write)(Tegra124RegBlockState *s, hwaddr offset, uint32_t value);
    void (*reset)(Tegra124RegBlockState *s);
} Tegra124RegBlockClass;

#define TYPE_TEGRA124_REGBLOCK "tegra124-regblock"
OBJECT_DECLARE_TYPE(Tegra124RegBlockState, Tegra124RegBlockClass,
                    TEGRA124_REGBLOCK)

#define TYPE_TEGRA124_EMC          "tegra124-emc"
#define TYPE_TEGRA124_SOCTHERM     "tegra124-soctherm"
#define TYPE_TEGRA124_DFLL         "tegra124-dfll"

/* ---- EMC ---------------------------------------------------------------- */

#define EMC_INTSTATUS           0x000
#define EMC_INTSTATUS_CLKCHANGE_COMPLETE (1u << 4)
#define EMC_TIMING_CONTROL      0x028
#define EMC_STATUS              0x2b4

static uint64_t emc_read(Tegra124RegBlockState *s, hwaddr offset)
{
    switch (offset) {
    case EMC_INTSTATUS:
        return s->regs[offset / 4] | EMC_INTSTATUS_CLKCHANGE_COMPLETE;
    case EMC_TIMING_CONTROL:
        return 0;                        /* shadow update already applied */
    case EMC_STATUS:
        return 0;                        /* no update stalled, no MRR busy */
    default:
        return s->regs[offset / 4];
    }
}

static void emc_write(Tegra124RegBlockState *s, hwaddr offset, uint32_t value)
{
    switch (offset) {
    case EMC_INTSTATUS:                  /* write one to clear */
        s->regs[offset / 4] &= ~value;
        break;
    case EMC_TIMING_CONTROL:
    case EMC_STATUS:
        break;
    default:
        s->regs[offset / 4] = value;
    }
}

/* ---- SOC_THERM ---------------------------------------------------------- */

#define SOCTHERM_SENSOR_BASE    0x0c0
#define SOCTHERM_SENSOR_STRIDE  0x020
#define SOCTHERM_SENSORS        8
#define SENSOR_CONFIG0          0x00
#define SENSOR_CONFIG0_STOP     (1u << 0)
#define SENSOR_STATUS0          0x0c
#define SENSOR_STATUS1          0x10
#define SENSOR_STATUS_VALID     (1u << 31)
#define SOCTHERM_INTR_STATUS    0x084
#define SOCTHERM_TS_TEMP1       0x1c8    /* [31:16] CPU, [15:0] GPU */
#define SOCTHERM_TS_TEMP2       0x1cc    /* [31:16] MEM, [15:0] PLLX */

/* SOC_THERM temperature field: [15:8] degrees, bit 7 = +0.5, bit 0 = sign */
static uint32_t soctherm_encode(int32_t mc)
{
    uint32_t v = (uint32_t)(ABS(mc) / 1000) << 8;

    if (ABS(mc) % 1000 >= 500) {
        v |= 0x80;
    }
    return v | (mc < 0 ? 1 : 0);
}

static uint64_t soctherm_read(Tegra124RegBlockState *s, hwaddr offset)
{
    uint32_t t = soctherm_encode(s->temp_mc);

    if (offset >= SOCTHERM_SENSOR_BASE &&
        offset < SOCTHERM_SENSOR_BASE + SOCTHERM_SENSORS * SOCTHERM_SENSOR_STRIDE) {
        hwaddr base = offset - (offset - SOCTHERM_SENSOR_BASE) %
                      SOCTHERM_SENSOR_STRIDE;
        bool running = !(s->regs[(base + SENSOR_CONFIG0) / 4] &
                         SENSOR_CONFIG0_STOP);

        switch (offset - base) {
        case SENSOR_STATUS0:             /* raw TSOSC capture */
            return running ? SENSOR_STATUS_VALID | 0x0d80 : 0;
        case SENSOR_STATUS1:             /* calibrated temperature */
            return running ? SENSOR_STATUS_VALID | t : 0;
        }
    }
    switch (offset) {
    case SOCTHERM_TS_TEMP1:
    case SOCTHERM_TS_TEMP2:
        return (t << 16) | t;
    case SOCTHERM_INTR_STATUS:
        return 0;                        /* no threshold ever crossed */
    default:
        return s->regs[offset / 4];
    }
}

static void soctherm_write(Tegra124RegBlockState *s, hwaddr offset,
                           uint32_t value)
{
    switch (offset) {
    case SOCTHERM_TS_TEMP1:
    case SOCTHERM_TS_TEMP2:
    case SOCTHERM_INTR_STATUS:
        break;
    default:
        s->regs[offset / 4] = value;
    }
}

/* ---- CL-DVFS (DFLL) ----------------------------------------------------- */

#define DFLL_FREQ_REQ           0x014
#define DFLL_FREQ_REQ_FREQ_MASK 0x7f
#define DFLL_MONITOR_DATA       0x02c
#define DFLL_MONITOR_DATA_NEW   (1u << 16)
#define DFLL_I2C_STS            0x048
#define DFLL_INTR_STS           0x05c

static uint64_t dfll_read(Tegra124RegBlockState *s, hwaddr offset)
{
    switch (offset) {
    case DFLL_MONITOR_DATA:
        return DFLL_MONITOR_DATA_NEW |
               (s->regs[DFLL_FREQ_REQ / 4] & DFLL_FREQ_REQ_FREQ_MASK);
    case DFLL_I2C_STS:
        return 0;                        /* no I2C request pending */
    case DFLL_INTR_STS:
        return 0;
    default:
        return s->regs[offset / 4];
    }
}

static void dfll_write(Tegra124RegBlockState *s, hwaddr offset, uint32_t value)
{
    switch (offset) {
    case DFLL_MONITOR_DATA:
    case DFLL_I2C_STS:
    case DFLL_INTR_STS:
        break;
    default:
        s->regs[offset / 4] = value;
    }
}

/* ---- common ------------------------------------------------------------- */

static uint64_t regblock_read(void *opaque, hwaddr offset, unsigned size)
{
    Tegra124RegBlockState *s = opaque;

    return TEGRA124_REGBLOCK_GET_CLASS(s)->read(s, offset & ~3);
}

static void regblock_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    Tegra124RegBlockState *s = opaque;

    TEGRA124_REGBLOCK_GET_CLASS(s)->write(s, offset & ~3, value);
}

static const MemoryRegionOps regblock_ops = {
    .read = regblock_read,
    .write = regblock_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void regblock_reset(DeviceState *dev)
{
    Tegra124RegBlockState *s = TEGRA124_REGBLOCK(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void regblock_init(Object *obj)
{
    Tegra124RegBlockState *s = TEGRA124_REGBLOCK(obj);
    Tegra124RegBlockClass *c = TEGRA124_REGBLOCK_GET_CLASS(obj);

    memory_region_init_io(&s->iomem, obj, &regblock_ops, s, c->name,
                          TEGRA124_REGBLOCK_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const VMStateDescription vmstate_regblock = {
    .name = TYPE_TEGRA124_REGBLOCK,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, Tegra124RegBlockState,
                             TEGRA124_REGBLOCK_SIZE / 4),
        VMSTATE_END_OF_LIST()
    }
};

static const Property soctherm_properties[] = {
    DEFINE_PROP_INT32("temperature", Tegra124RegBlockState, temp_mc, 42000),
};

static void regblock_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, regblock_reset);
    dc->vmsd = &vmstate_regblock;
}

static void emc_class_init(ObjectClass *klass, const void *data)
{
    Tegra124RegBlockClass *c = TEGRA124_REGBLOCK_CLASS(klass);

    DEVICE_CLASS(klass)->desc = "Tegra124 EMC (ideal DRAM)";
    c->name = "tegra124-emc";
    c->read = emc_read;
    c->write = emc_write;
}

static void soctherm_class_init(ObjectClass *klass, const void *data)
{
    Tegra124RegBlockClass *c = TEGRA124_REGBLOCK_CLASS(klass);

    DEVICE_CLASS(klass)->desc = "Tegra124 SOC_THERM (fixed temperature)";
    device_class_set_props(DEVICE_CLASS(klass), soctherm_properties);
    c->name = "tegra124-soctherm";
    c->read = soctherm_read;
    c->write = soctherm_write;
}

static void dfll_class_init(ObjectClass *klass, const void *data)
{
    Tegra124RegBlockClass *c = TEGRA124_REGBLOCK_CLASS(klass);

    DEVICE_CLASS(klass)->desc = "Tegra124 CL-DVFS (DFLL)";
    c->name = "tegra124-dfll";
    c->read = dfll_read;
    c->write = dfll_write;
}

static const TypeInfo regblock_types[] = {
    {
        .name          = TYPE_TEGRA124_REGBLOCK,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Tegra124RegBlockState),
        .instance_init = regblock_init,
        .class_size    = sizeof(Tegra124RegBlockClass),
        .class_init    = regblock_class_init,
        .abstract      = true,
    }, {
        .name          = TYPE_TEGRA124_EMC,
        .parent        = TYPE_TEGRA124_REGBLOCK,
        .class_init    = emc_class_init,
    }, {
        .name          = TYPE_TEGRA124_SOCTHERM,
        .parent        = TYPE_TEGRA124_REGBLOCK,
        .class_init    = soctherm_class_init,
    }, {
        .name          = TYPE_TEGRA124_DFLL,
        .parent        = TYPE_TEGRA124_REGBLOCK,
        .class_init    = dfll_class_init,
    },
};

DEFINE_TYPES(regblock_types)

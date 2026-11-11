/*
 * T124 HDMI configuration subset. No HDCP or physical-link completion.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/bitops.h"
#include "hw/misc/tegra124_hdmi.h"

struct Tegra124HdmiState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t regs[0x100];
    uint32_t active_state[2];
    unsigned wait_frames;
    bool powered;
    bool reset_asserted;
};

static uint32_t hdmi_config_mask(unsigned index)
{
    static const uint32_t masks[] = {
        [0x02] = 0xf, [0x03] = 0x7fff,
        [0x1e] = 0x300, /* Audio infoframes remain disabled. */
        [0x23] = 0x311, [0x25] = 0xffffff,
        [0x26] = UINT32_MAX, [0x27] = 0xffffff,
        [0x28] = UINT32_MAX, [0x29] = 0xffffff,
        [0x2a] = 0x11111,
        [0x44] = 0x5f1f157f, [0x45 ... 0x46] = 0x83ff03ff,
        [0x47] = 0x111, [0x49] = 0xffffff,
        [0x4c ... 0x4d] = UINT32_MAX, [0x4f] = UINT32_MAX,
        [0x57] = 0x3f0fff3f, [0x58] = 0x70f01f00,
        [0x59] = 0xff0000, [0x5a] = 0x7f2ffbff,
        [0x5b] = 0x7f0e82f8, [0x5e] = 3, [0x5f] = 0xfff0,
        [0x60 ... 0x6f] = 0xffe0b3ff,
        [0x7e] = UINT32_MAX, [0x95] = 0xffc0,
        [0x97] = 3, [0x98] = UINT32_MAX, [0x99] = 0xf0f0f0f,
        [0xd1] = UINT32_MAX, [0xd2] = 0x837fffff, [0xd3] = 0xfff,
    };

    return index < ARRAY_SIZE(masks) ? masks[index] : 0;
}

static void hdmi_frame(void *opaque, int head, int level)
{
    Tegra124HdmiState *s = opaque;

    if (!s->reset_asserted && level && head == (s->regs[0x97] & 1) &&
        s->wait_frames && !--s->wait_frames) {
        s->regs[0x55] &= ~BIT(31);
        s->regs[0x5f] &= ~BIT(28);
        s->powered = s->regs[0x55] & 1;
    }
}

static MemTxResult hdmi_read(void *opaque, hwaddr addr, uint64_t *value,
                             unsigned size, MemTxAttrs attrs)
{
    Tegra124HdmiState *s = opaque;

    if (!s->reset_asserted && (hdmi_config_mask(addr / 4) ||
                             addr == 4 || addr == 0x154)) {
        *value = s->regs[addr / 4];
        return MEMTX_OK;
    }
    if (!s->reset_asserted && addr == 0x158) {
        *value = 0x800000 | ((s->active_state[0] & 3) << 8) |
                 ((s->active_state[0] & 8) << 7);
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP, "tegra124-hdmi: unsupported read at 0x%"
                  HWADDR_PRIx "%s\n", addr,
                  s->reset_asserted ? " during reset" : "");
    return MEMTX_ERROR;
}

static MemTxResult hdmi_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned size, MemTxAttrs attrs)
{
    Tegra124HdmiState *s = opaque;
    uint32_t mask = hdmi_config_mask(addr / 4);

    if (s->reset_asserted) {
        goto unsupported;
    }
    if (addr == 0x154 && !(value & ~0x80000001U)) {
        if (s->wait_frames) {
            if (value != (s->regs[0x55] & ~BIT(31))) {
                goto unsupported;
            }
            return MEMTX_OK; /* Clearing the request cannot clear busy. */
        }
        if (value & BIT(31)) {
            unsigned pc = value & 1 ? 0 : (s->regs[0x5f] >> 8) & 15;
            uint32_t instruction = s->regs[0x60 + pc];

            /* Terminating VSYNC wait: driven outputs, no PLL reset. */
            if ((instruction & 0xff7ffc00U) != 0xa000 ||
                !(instruction & 0x3ff)) {
                goto unsupported;
            }
            s->wait_frames = instruction & 0x3ff;
            s->regs[0x5f] = (s->regs[0x5f] & ~0xf0000U) |
                            (pc << 16) | BIT(28);
        }
        s->regs[0x55] = value;
        return MEMTX_OK;
    }
    if (addr == 4 && value <= 1) {
        if (s->regs[1] && !value) {
            uint32_t state = s->regs[2], owner = s->regs[3] & 15;

            if ((state & 3) != 0 && (state & 3) != 2) {
                goto unsupported;
            }
            if ((state & 8) && (!s->powered ||
                owner != (s->regs[0x97] & 1) + 1 ||
                ((s->regs[3] >> 8) & 15) != 1)) {
                goto unsupported;
            }
            s->active_state[0] = state;
            s->active_state[1] = s->regs[3];
        }
        s->regs[1] = value;
        return MEMTX_OK;
    }
    if (mask && !(value & ~mask) && !s->wait_frames) {
        s->regs[addr / 4] = value;
        return MEMTX_OK;
    }
unsupported:
    qemu_log_mask(LOG_UNIMP, "tegra124-hdmi: unsupported write at 0x%"
                  HWADDR_PRIx " = 0x%" PRIx64 "%s\n", addr, value,
                  s->reset_asserted ? " during reset" : "");
    return MEMTX_ERROR;
}

static const MemoryRegionOps hdmi_ops = {
    .read_with_attrs = hdmi_read,
    .write_with_attrs = hdmi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void hdmi_reset(DeviceState *dev)
{
    Tegra124HdmiState *s = TEGRA124_HDMI(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->active_state, 0, sizeof(s->active_state));
    s->wait_frames = 0;
    s->powered = false;
    s->regs[3] = 0x151;
    s->regs[0x1e] = 0x200;
    s->regs[0x23] = 0x200;
    s->regs[0x2a] = 0x10000;
    s->regs[0x44] = 0x20038;
    s->regs[0x45] = 0x819a028a;
    s->regs[0x46] = 0x82000210;
    s->regs[0x49] = 1;
    s->regs[0x4f] = 0x10000;
    s->regs[0x57] = 0x200333f;
    s->regs[0x58] = 0x301200;
    s->regs[0x5a] = s->regs[0x5b] = 0x1c800;
    s->regs[0x5f] = 0x8800;
    for (unsigned i = 0x60; i <= 0x6f; i++) {
        s->regs[i] = 0x1008000;
    }
    s->regs[0x7e] = 0x20202020;
    s->regs[0x95] = 0x1900;
    s->regs[0x99] = 0x8080808;
    s->regs[0xd2] = 0x34bb;
}

static void hdmi_reset_line(void *opaque, int n, int level)
{
    Tegra124HdmiState *s = opaque;

    if (level && !s->reset_asserted) {
        hdmi_reset(DEVICE(s));
    }
    s->reset_asserted = level;
}

static void hdmi_init(Object *obj)
{
    Tegra124HdmiState *s = TEGRA124_HDMI(obj);

    memory_region_init_io(&s->iomem, obj, &hdmi_ops, s,
                          TYPE_TEGRA124_HDMI, 0x40000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    qdev_init_gpio_in_named(DEVICE(obj), hdmi_reset_line, "reset", 1);
    qdev_init_gpio_in_named(DEVICE(obj), hdmi_frame, "frame", 2);
}

static void hdmi_class_init(ObjectClass *klass, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(klass), hdmi_reset);
}

static const TypeInfo hdmi_info = {
    .name = TYPE_TEGRA124_HDMI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Tegra124HdmiState),
    .instance_init = hdmi_init,
    .class_init = hdmi_class_init,
};

static void hdmi_register_types(void)
{
    type_register_static(&hdmi_info);
}

type_init(hdmi_register_types)

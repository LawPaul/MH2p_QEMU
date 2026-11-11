/*
 * Explicit development-only QEMU pointer export, not MH2P touch hardware.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "chardev/char-fe.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "system/reset.h"
#include "migration/vmstate.h"
#include "ui/console.h"
#include "ui/input.h"

#define TYPE_MH2P_DIAGNOSTIC_POINTER "mh2p-diagnostic-pointer"
OBJECT_DECLARE_SIMPLE_TYPE(Mh2pDiagnosticPointer, MH2P_DIAGNOSTIC_POINTER)

struct Mh2pDiagnosticPointer {
    DeviceState parent_obj;
    CharFrontend chr;
    QemuInputHandlerState *handler;
    uint32_t width;
    uint32_t height;
    unsigned x;
    unsigned y;
    bool down;
    bool pending;
    bool motion;
    bool moved;
};

static void pointer_sync(DeviceState *dev);

static void pointer_event(DeviceState *dev, QemuConsole *src,
                          QemuInputEvent *event)
{
    Mh2pDiagnosticPointer *s = MH2P_DIAGNOSTIC_POINTER(dev);

    if (event->type == INPUT_EVENT_KIND_ABS) {
        int value = CLAMP(event->abs.value, INPUT_EVENT_ABS_MIN,
                          INPUT_EVENT_ABS_MAX);
        unsigned old_x = s->x, old_y = s->y;

        if (event->abs.axis == INPUT_AXIS_X) {
            s->x = qemu_input_scale_axis(value, INPUT_EVENT_ABS_MIN,
                                        INPUT_EVENT_ABS_MAX, 0, s->width - 1);
        } else if (event->abs.axis == INPUT_AXIS_Y) {
            s->y = qemu_input_scale_axis(value, INPUT_EVENT_ABS_MIN,
                                        INPUT_EVENT_ABS_MAX, 0, s->height - 1);
        }
        if (s->motion && s->down && (s->x != old_x || s->y != old_y)) {
            s->moved = true;
        }
    } else if (event->type == INPUT_EVENT_KIND_BTN &&
               event->btn.button == INPUT_BUTTON_LEFT &&
               event->btn.down != s->down) {
        if (s->pending) {
            pointer_sync(dev);
        }
        s->down = event->btn.down;
        s->pending = true;
    }
}

static void pointer_export(Mh2pDiagnosticPointer *s, const char *action)
{
    char line[80];
    int length;

    length = snprintf(line, sizeof(line), "%s %u %u\n", action, s->x, s->y);
    if (qemu_chr_fe_write(&s->chr, (const uint8_t *)line, length) != length) {
        error_report("mh2p-diagnostic-pointer: input export failed");
    }
}

static void pointer_sync(DeviceState *dev)
{
    Mh2pDiagnosticPointer *s = MH2P_DIAGNOSTIC_POINTER(dev);

    if (s->pending) {
        pointer_export(s, s->down ? "press" : "release");
    } else if (s->motion && s->down && s->moved) {
        pointer_export(s, "move");
    }
    s->pending = s->moved = false;
}

static const QemuInputHandler pointer_handler = {
    .name = "MH2P diagnostic pointer (not native HID)",
    .mask = INPUT_EVENT_MASK_ABS | INPUT_EVENT_MASK_BTN,
    .event = pointer_event,
    .sync = pointer_sync,
};

static void pointer_reset(void *opaque);

static void pointer_realize(DeviceState *dev, Error **errp)
{
    Mh2pDiagnosticPointer *s = MH2P_DIAGNOSTIC_POINTER(dev);

    if (!qemu_chr_fe_backend_connected(&s->chr) ||
        !s->width || s->width > 4096 || !s->height || s->height > 4096) {
        error_setg(errp, "diagnostic pointer requires a chardev and "
                   "width/height in 1..4096");
        return;
    }
    s->handler = qemu_input_handler_register(dev, &pointer_handler);
    qemu_input_handler_activate(s->handler);
    qemu_register_reset(pointer_reset, s);
}

static void pointer_unrealize(DeviceState *dev)
{
    Mh2pDiagnosticPointer *s = MH2P_DIAGNOSTIC_POINTER(dev);

    qemu_input_handler_unregister(s->handler);
    qemu_unregister_reset(pointer_reset, s);
    qemu_chr_fe_deinit(&s->chr, false);
}

static void pointer_reset(void *opaque)
{
    Mh2pDiagnosticPointer *s = opaque;

    if (s->down) {
        pointer_export(s, "cancel");
    }
    s->down = s->pending = s->moved = false;
    s->x = s->y = 0;
}

static const VMStateDescription pointer_vmstate = {
    .name = TYPE_MH2P_DIAGNOSTIC_POINTER,
    .unmigratable = true,
};

static const Property pointer_properties[] = {
    DEFINE_PROP_CHR("chardev", Mh2pDiagnosticPointer, chr),
    DEFINE_PROP_UINT32("width", Mh2pDiagnosticPointer, width, 1920),
    DEFINE_PROP_UINT32("height", Mh2pDiagnosticPointer, height, 720),
    DEFINE_PROP_BOOL("motion", Mh2pDiagnosticPointer, motion, false),
};

static void pointer_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = pointer_realize;
    dc->unrealize = pointer_unrealize;
    dc->desc = "Development-only pointer export; no native touch emulation";
    dc->vmsd = &pointer_vmstate;
    device_class_set_props(dc, pointer_properties);
}

static const TypeInfo pointer_type = {
    .name = TYPE_MH2P_DIAGNOSTIC_POINTER,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(Mh2pDiagnosticPointer),
    .class_init = pointer_class_init,
};

static void pointer_register_types(void)
{
    type_register_static(&pointer_type);
}

type_init(pointer_register_types)

/*
 * Classical CAN diagnostic peer over a character stream.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "chardev/char-fe.h"
#include "net/can_host.h"

#define TYPE_CAN_HOST_CHARDEV "can-host-chardev"
OBJECT_DECLARE_SIMPLE_TYPE(CanHostChardev, CAN_HOST_CHARDEV)

struct CanHostChardev {
    CanHostState parent;
    CharFrontend chr;
    char *chardev;
    uint8_t input[16], output[1024];
    unsigned input_used, output_used;
    guint watch;
    bool opened, failed;
};

static void can_chardev_flush(CanHostChardev *s);

static gboolean can_chardev_writable(GIOChannel *channel,
                                     GIOCondition condition, void *opaque)
{
    CanHostChardev *s = opaque;

    s->watch = 0;
    if (condition & G_IO_HUP) {
        s->opened = false;
    } else {
        can_chardev_flush(s);
    }
    return G_SOURCE_REMOVE;
}

static void can_chardev_flush(CanHostChardev *s)
{
    while (s->output_used) {
        int written = qemu_chr_fe_write(&s->chr, s->output, s->output_used);

        if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            error_report("CAN chardev '%s': write failed: %s",
                         s->chardev, strerror(errno));
            s->failed = true;
            return;
        }
        if (written <= 0) {
            if (!s->watch) {
                s->watch = qemu_chr_fe_add_watch(&s->chr, G_IO_OUT | G_IO_HUP,
                                                can_chardev_writable, s);
                if (!s->watch) {
                    error_report("CAN chardev '%s': cannot wait for output",
                                 s->chardev);
                    s->failed = true;
                }
            }
            return;
        }
        s->output_used -= written;
        memmove(s->output, s->output + written, s->output_used);
    }
}

static bool can_chardev_can_receive(CanBusClientState *client)
{
    CanHostChardev *s = container_of(client, CanHostChardev, parent.bus_client);

    return s->opened && !s->failed;
}

static ssize_t can_chardev_receive(CanBusClientState *client,
                                   const qemu_can_frame *frames, size_t count)
{
    CanHostChardev *s = container_of(client, CanHostChardev, parent.bus_client);
    size_t accepted = 0;

    for (size_t i = 0; i < count; i++) {
        uint8_t *wire = s->output + s->output_used;

        if (frames[i].can_dlc > 8 || frames[i].flags ||
            (frames[i].can_id & QEMU_CAN_ERR_FLAG)) {
            error_report("CAN chardev '%s': only classical CAN is supported",
                         s->chardev);
            continue;
        }
        if (sizeof(s->output) - s->output_used < 16) {
            error_report("CAN chardev '%s': output queue full", s->chardev);
            break;
        }
        memset(wire, 0, 16);
        stl_le_p(wire, frames[i].can_id);
        wire[4] = frames[i].can_dlc;
        if (!(frames[i].can_id & QEMU_CAN_RTR_FLAG)) {
            memcpy(wire + 8, frames[i].data, frames[i].can_dlc);
        }
        s->output_used += 16;
        can_chardev_flush(s);
        if (s->failed) {
            break;
        }
        accepted++;
    }
    return accepted;
}

static CanBusClientInfo can_chardev_bus_info = {
    .can_receive = can_chardev_can_receive,
    .receive = can_chardev_receive,
};

static int can_chardev_can_read(void *opaque)
{
    CanHostChardev *s = opaque;

    return s->failed ? 0 : sizeof(s->input) - s->input_used;
}

static void can_chardev_read(void *opaque, const uint8_t *data, int size)
{
    CanHostChardev *s = opaque;
    qemu_can_frame frame = { 0 };

    memcpy(s->input + s->input_used, data, size);
    s->input_used += size;
    if (s->input_used != sizeof(s->input)) {
        return;
    }
    s->input_used = 0;
    frame.can_id = ldl_le_p(s->input);
    frame.can_dlc = s->input[4];
    if (frame.can_dlc > 8 || s->input[5] || s->input[6] || s->input[7] ||
        (frame.can_id & QEMU_CAN_ERR_FLAG) ||
        (!(frame.can_id & QEMU_CAN_EFF_FLAG) &&
         (frame.can_id & QEMU_CAN_EFF_MASK) > QEMU_CAN_SFF_MASK)) {
        error_report("CAN chardev '%s': invalid 16-byte classical CAN record",
                     s->chardev);
        return;
    }
    memcpy(frame.data, s->input + 8, frame.can_dlc);
    can_bus_client_send(&s->parent.bus_client, &frame, 1);
}

static void can_chardev_event(void *opaque, QEMUChrEvent event)
{
    CanHostChardev *s = opaque;

    if (event != CHR_EVENT_OPENED && event != CHR_EVENT_CLOSED) {
        return;
    }
    if (s->watch) {
        g_source_remove(s->watch);
        s->watch = 0;
    }
    if (event == CHR_EVENT_CLOSED && (s->input_used || s->output_used)) {
        warn_report("CAN chardev '%s': disconnect discarded %u input and "
                    "%u output bytes", s->chardev,
                    s->input_used, s->output_used);
    }
    s->input_used = s->output_used = 0;
    s->failed = false;
    s->opened = event == CHR_EVENT_OPENED;
}

static void can_chardev_connect(CanHostState *host, Error **errp)
{
    CanHostChardev *s = CAN_HOST_CHARDEV(host);
    Chardev *chr = s->chardev ? qemu_chr_find(s->chardev) : NULL;

    if (!chr) {
        error_setg(errp, "CAN chardev '%s' not found",
                   s->chardev ? s->chardev : "(unset)");
        return;
    }
    if (!qemu_chr_fe_init(&s->chr, chr, errp)) {
        return;
    }
    host->bus_client.info = &can_chardev_bus_info;
    qemu_chr_fe_set_handlers(&s->chr, can_chardev_can_read, can_chardev_read,
                             can_chardev_event, NULL, s, NULL, true);
}

static void can_chardev_disconnect(CanHostState *host)
{
    CanHostChardev *s = CAN_HOST_CHARDEV(host);

    if (s->watch) {
        g_source_remove(s->watch);
        s->watch = 0;
    }
    qemu_chr_fe_deinit(&s->chr, false);
}

static char *can_chardev_get(Object *obj, Error **errp)
{
    return g_strdup(CAN_HOST_CHARDEV(obj)->chardev);
}

static void can_chardev_set(Object *obj, const char *value, Error **errp)
{
    CanHostChardev *s = CAN_HOST_CHARDEV(obj);

    if (qemu_chr_fe_backend_connected(&s->chr)) {
        error_setg(errp, "CAN chardev is already configured");
        return;
    }
    g_free(s->chardev);
    s->chardev = g_strdup(value);
}

static void can_chardev_finalize(Object *obj)
{
    g_free(CAN_HOST_CHARDEV(obj)->chardev);
}

static void can_chardev_class_init(ObjectClass *klass, const void *data)
{
    CanHostClass *chc = CAN_HOST_CLASS(klass);

    object_class_property_add_str(klass, "chardev", can_chardev_get,
                                  can_chardev_set);
    chc->connect = can_chardev_connect;
    chc->disconnect = can_chardev_disconnect;
}

static const TypeInfo can_chardev_info = {
    .parent = TYPE_CAN_HOST,
    .name = TYPE_CAN_HOST_CHARDEV,
    .instance_size = sizeof(CanHostChardev),
    .instance_finalize = can_chardev_finalize,
    .class_init = can_chardev_class_init,
};

static void can_chardev_register_types(void)
{
    type_register_static(&can_chardev_info);
}

type_init(can_chardev_register_types);

/*
 * Local MH2P PCIe sideband experiment. No link-up or PCI transactions.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "chardev/char-fe.h"
#include "chardev/char-socket.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/misc/mh2p_pcie_sideband.h"
#include "migration/vmstate.h"

#define TYPE_MH2P_PCIE_SIDEBAND "mh2p-pcie-sideband"
OBJECT_DECLARE_SIMPLE_TYPE(MH2PPCIeSideband, MH2P_PCIE_SIDEBAND)

#define FRAME_SIZE 16
#define QUEUE_SIZE (FRAME_SIZE * 256)
#define WIRE_MAGIC 0x5348324d /* "M2HS", little endian */

struct MH2PPCIeSideband {
    SysBusDevice parent;
    CharFrontend chr;
    bool transmitter, clock, reset_n, failed;
    qemu_irq outputs[2];
    uint8_t rx[FRAME_SIZE], tx[QUEUE_SIZE];
    unsigned rx_count, tx_count, tx_head;
    guint watch;
};

static void sideband_clear(MH2PPCIeSideband *s)
{
    if (s->watch) {
        g_source_remove(s->watch);
        s->watch = 0;
    }
    s->rx_count = s->tx_count = s->tx_head = 0;
    if (!s->transmitter) {
        s->clock = s->reset_n = false;
        qemu_set_irq(s->outputs[0], 0);
        qemu_set_irq(s->outputs[1], 0);
    }
}

static void sideband_fail(MH2PPCIeSideband *s, const char *reason)
{
    error_report("mh2p-pcie-sideband: %s; disconnecting", reason);
    s->failed = true;
    sideband_clear(s);
    qemu_chr_fe_disconnect(&s->chr);
}

static gboolean sideband_writable(void *unused, GIOCondition condition,
                                  void *opaque);

static void sideband_drain(MH2PPCIeSideband *s)
{
    while (s->tx_count && qemu_chr_fe_backend_open(&s->chr) && !s->failed) {
        unsigned length = MIN(s->tx_count, QUEUE_SIZE - s->tx_head);
        int written = qemu_chr_fe_write(&s->chr, s->tx + s->tx_head, length);

        if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            sideband_fail(s, "socket write failed");
            return;
        }
        if (written <= 0) {
            break;
        }
        s->tx_head = (s->tx_head + written) % QUEUE_SIZE;
        s->tx_count -= written;
    }
    if (s->tx_count && !s->watch && !s->failed) {
        s->watch = qemu_chr_fe_add_watch(&s->chr,
                                        G_IO_OUT | G_IO_HUP | G_IO_ERR,
                                        sideband_writable, s);
        if (!s->watch) {
            sideband_fail(s, "backend cannot resume a partial write");
        }
    }
}

static gboolean sideband_writable(void *unused, GIOCondition condition,
                                  void *opaque)
{
    MH2PPCIeSideband *s = opaque;

    s->watch = 0;
    if (condition & (G_IO_HUP | G_IO_ERR)) {
        sideband_fail(s, "socket closed during partial write");
    } else {
        sideband_drain(s);
    }
    return G_SOURCE_REMOVE;
}

static void sideband_send(MH2PPCIeSideband *s)
{
    uint8_t frame[FRAME_SIZE];

    if (!qemu_chr_fe_backend_open(&s->chr) || s->failed) {
        return;
    }
    if (s->tx_count > QUEUE_SIZE - FRAME_SIZE) {
        sideband_fail(s, "reset-edge queue exhausted");
        return;
    }
    stl_le_p(frame, WIRE_MAGIC);
    stl_le_p(frame + 4, 1);
    stl_le_p(frame + 8, s->clock ? 100000000 : 0);
    stl_le_p(frame + 12, s->reset_n);
    for (unsigned i = 0; i < FRAME_SIZE; i++) {
        s->tx[(s->tx_head + s->tx_count + i) % QUEUE_SIZE] = frame[i];
    }
    s->tx_count += FRAME_SIZE;
    sideband_drain(s);
}

void mh2p_pcie_sideband_set(DeviceState *dev, bool clock, bool reset_n)
{
    MH2PPCIeSideband *s;

    if (!dev) {
        return;
    }
    s = MH2P_PCIE_SIDEBAND(dev);
    assert(s->transmitter);
    if (s->clock != clock || s->reset_n != reset_n) {
        s->clock = clock;
        s->reset_n = reset_n;
        sideband_send(s);
    }
}

static int sideband_can_read(void *opaque)
{
    MH2PPCIeSideband *s = opaque;

    return s->failed ? 0 : FRAME_SIZE - s->rx_count;
}

static void sideband_read(void *opaque, const uint8_t *data, int size)
{
    MH2PPCIeSideband *s = opaque;
    uint32_t hz, reset_n;

    if (s->transmitter) {
        sideband_fail(s, "unexpected reverse-direction data");
        return;
    }
    memcpy(s->rx + s->rx_count, data, size);
    s->rx_count += size;
    if (s->rx_count != FRAME_SIZE) {
        return;
    }
    hz = ldl_le_p(s->rx + 8);
    reset_n = ldl_le_p(s->rx + 12);
    if (ldl_le_p(s->rx) != WIRE_MAGIC || ldl_le_p(s->rx + 4) != 1 ||
        (hz && hz != 100000000) || reset_n > 1) {
        sideband_fail(s, "invalid version-1 clock/reset frame");
        return;
    }
    s->rx_count = 0;
    s->clock = hz != 0;
    s->reset_n = reset_n;
    qemu_set_irq(s->outputs[0], s->clock);
    qemu_set_irq(s->outputs[1], s->reset_n);
}

static void sideband_event(void *opaque, QEMUChrEvent event)
{
    MH2PPCIeSideband *s = opaque;

    if (event == CHR_EVENT_CLOSED) {
        sideband_clear(s);
    } else if (event == CHR_EVENT_OPENED) {
        sideband_clear(s);
        s->failed = false;
        if (s->transmitter) {
            sideband_send(s);
        }
    }
}

static void sideband_realize(DeviceState *dev, Error **errp)
{
    MH2PPCIeSideband *s = MH2P_PCIE_SIDEBAND(dev);

    if (!qemu_chr_fe_backend_connected(&s->chr) ||
        !object_dynamic_cast(OBJECT(qemu_chr_fe_get_driver(&s->chr)),
                             TYPE_CHARDEV_SOCKET)) {
        error_setg(errp, "mh2p-pcie-sideband requires a socket chardev");
        return;
    }
    qemu_chr_fe_set_handlers(&s->chr, sideband_can_read, sideband_read,
                            sideband_event, NULL, s, NULL, true);
}

static void sideband_unrealize(DeviceState *dev)
{
    MH2PPCIeSideband *s = MH2P_PCIE_SIDEBAND(dev);

    sideband_clear(s);
    qemu_chr_fe_deinit(&s->chr, false);
}

static void sideband_init(Object *obj)
{
    MH2PPCIeSideband *s = MH2P_PCIE_SIDEBAND(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->outputs[0]);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->outputs[1]);
}

static const Property sideband_properties[] = {
    DEFINE_PROP_CHR("chardev", MH2PPCIeSideband, chr),
    DEFINE_PROP_BOOL("transmitter", MH2PPCIeSideband, transmitter, false),
};

static const VMStateDescription sideband_vmstate = {
    .name = TYPE_MH2P_PCIE_SIDEBAND,
    .unmigratable = 1,
};

static void sideband_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = sideband_realize;
    dc->unrealize = sideband_unrealize;
    dc->user_creatable = false;
    dc->vmsd = &sideband_vmstate;
    device_class_set_props(dc, sideband_properties);
    /* Receiver reset must preserve externally driven wire levels. */
}

static const TypeInfo sideband_info = {
    .name = TYPE_MH2P_PCIE_SIDEBAND,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MH2PPCIeSideband),
    .instance_init = sideband_init,
    .class_init = sideband_class_init,
};

static void sideband_register_types(void)
{
    type_register_static(&sideband_info);
}

type_init(sideband_register_types)

DeviceState *mh2p_pcie_sideband_create(Object *parent, bool transmitter)
{
    Chardev *chr = qemu_chr_find("mh2p-pcie-sideband");
    DeviceState *dev;

    if (!chr) {
        return NULL;
    }
    dev = qdev_new(TYPE_MH2P_PCIE_SIDEBAND);
    object_property_add_child(parent, "pcie-sideband", OBJECT(dev));
    qdev_prop_set_bit(dev, "transmitter", transmitter);
    qdev_prop_set_chr(dev, "chardev", chr);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    return dev;
}

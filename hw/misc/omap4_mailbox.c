/*
 * OMAP4 mailbox1 and DRA7 mailbox: four-word FIFOs and per-user interrupts.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/misc/omap4_mailbox.h"
#include "migration/vmstate.h"
#include "qom/object.h"

OBJECT_DECLARE_SIMPLE_TYPE(OMAP4MailboxState, OMAP4_MAILBOX)

struct OMAP4MailboxState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq irq[4];
    bool dra7;
    uint32_t sysconfig;
    uint32_t words[48];
    uint8_t head[12];
    uint8_t count[12];
    uint32_t status[4];
    uint32_t enable[4];
};

static unsigned mailbox_fifos(OMAP4MailboxState *s)
{
    return s->dra7 ? 12 : 8;
}

static unsigned mailbox_users(OMAP4MailboxState *s)
{
    return s->dra7 ? 4 : 3;
}

static uint32_t mailbox_conditions(OMAP4MailboxState *s)
{
    uint32_t bits = 0;

    for (unsigned i = 0; i < mailbox_fifos(s); i++) {
        if (s->count[i]) {
            bits |= BIT(2 * i);
        }
        if (s->count[i] < 4) {
            bits |= BIT(2 * i + 1);
        }
    }
    return bits;
}

static void mailbox_update(OMAP4MailboxState *s)
{
    uint32_t bits = mailbox_conditions(s);

    for (unsigned i = 0; i < mailbox_users(s); i++) {
        s->status[i] |= bits;
        qemu_set_irq(s->irq[i], !!(s->status[i] & s->enable[i]));
    }
}

static void mailbox_reset(DeviceState *dev)
{
    OMAP4MailboxState *s = OMAP4_MAILBOX(dev);

    s->sysconfig = s->dra7 ? 8 : 0;
    memset(s->words, 0, sizeof(s->words));
    memset(s->head, 0, sizeof(s->head));
    memset(s->count, 0, sizeof(s->count));
    memset(s->status, 0, sizeof(s->status));
    memset(s->enable, 0, sizeof(s->enable));
    mailbox_update(s);
}

static MemTxResult mailbox_read(void *opaque, hwaddr offset,
                               uint64_t *value, unsigned size, MemTxAttrs attrs)
{
    OMAP4MailboxState *s = opaque;
    unsigned index;

    if (offset == 0x10) {
        *value = s->sysconfig;
        return MEMTX_OK;
    }
    if (offset >= 0x40 && offset < 0x40 + 4 * mailbox_fifos(s)) {
        index = (offset - 0x40) / 4;
        if (!s->count[index]) {
            *value = 0;
            return MEMTX_OK;
        }
        *value = s->words[index * 4 + s->head[index]];
        s->head[index] = (s->head[index] + 1) % 4;
        s->count[index]--;
        mailbox_update(s);
        return MEMTX_OK;
    }
    if (offset >= 0x80 && offset < 0x80 + 4 * mailbox_fifos(s)) {
        *value = s->count[(offset - 0x80) / 4] == 4;
        return MEMTX_OK;
    }
    if (offset >= 0xc0 && offset < 0xc0 + 4 * mailbox_fifos(s)) {
        *value = s->count[(offset - 0xc0) / 4];
        return MEMTX_OK;
    }
    if (offset >= 0x100 && offset < 0x100 + 0x10 * mailbox_users(s)) {
        index = (offset - 0x100) / 0x10;
        switch (offset & 0xf) {
        case 0:
            *value = s->status[index];
            return MEMTX_OK;
        case 4:
            *value = s->status[index] & s->enable[index];
            return MEMTX_OK;
        case 8: case 12:
            *value = s->enable[index];
            return MEMTX_OK;
        }
    }

    qemu_log_mask(LOG_UNIMP, "omap4-mailbox: unsupported read 0x%"
                  HWADDR_PRIx "\n", offset);
    *value = 0;
    return MEMTX_ERROR;
}

static MemTxResult mailbox_write(void *opaque, hwaddr offset,
                                uint64_t value, unsigned size, MemTxAttrs attrs)
{
    OMAP4MailboxState *s = opaque;
    unsigned index;

    if (offset == 0x10) {
        if ((value & ~0xd) || (value & 0xc) == 0xc) {
            goto unsupported;
        }
        if (value & 1) {
            mailbox_reset(DEVICE(s));
        }
        s->sysconfig = value & ~1;
    } else if (offset >= 0x40 && offset < 0x40 + 4 * mailbox_fifos(s)) {
        unsigned tail;

        index = (offset - 0x40) / 4;
        if (s->count[index] == 4) {
            /* SPRUI30H 19.3.6: a full FIFO discards the new message. */
            qemu_log_mask(LOG_GUEST_ERROR, "omap4-mailbox: discarded full "
                          "FIFO %u write 0x%" PRIx64 "\n", index, value);
            return MEMTX_OK;
        }
        tail = (s->head[index] + s->count[index]) % 4;
        s->words[index * 4 + tail] = value;
        s->count[index]++;
    } else if (offset >= 0x100 && offset < 0x100 + 0x10 * mailbox_users(s)) {
        index = (offset - 0x100) / 0x10;
        switch (offset & 0xf) {
        case 0:
            s->status[index] |= value & MAKE_64BIT_MASK(0, 2 * mailbox_fifos(s));
            break;
        case 4:
            s->status[index] &= ~value;
            break;
        case 8:
            s->enable[index] |= value & MAKE_64BIT_MASK(0, 2 * mailbox_fifos(s));
            break;
        case 12:
            s->enable[index] &= ~value;
            break;
        default:
            goto unsupported;
        }
    } else {
        goto unsupported;
    }
    mailbox_update(s);
    return MEMTX_OK;

unsupported:
    qemu_log_mask(LOG_UNIMP, "omap4-mailbox: unsupported write 0x%"
                  HWADDR_PRIx " = 0x%" PRIx64 "\n", offset, value);
    return MEMTX_ERROR;
}

static const MemoryRegionOps mailbox_ops = {
    .read_with_attrs = mailbox_read,
    .write_with_attrs = mailbox_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static int mailbox_post_load(void *opaque, int version_id)
{
    OMAP4MailboxState *s = opaque;

    if (version_id < 2 && s->dra7) {
        return -EINVAL;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(s->count); i++) {
        if (s->count[i] > 4 || s->head[i] >= 4) {
            return -EINVAL;
        }
        if (i >= mailbox_fifos(s) && (s->count[i] || s->head[i])) {
            return -EINVAL;
        }
    }
    mailbox_update(s);
    return 0;
}

static const VMStateDescription vmstate_mailbox = {
    .name = TYPE_OMAP4_MAILBOX,
    .version_id = 2,
    .minimum_version_id = 1,
    .post_load = mailbox_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(sysconfig, OMAP4MailboxState),
        VMSTATE_UINT32_SUB_ARRAY(words, OMAP4MailboxState, 0, 32),
        VMSTATE_UINT8_SUB_ARRAY(head, OMAP4MailboxState, 0, 8),
        VMSTATE_UINT8_SUB_ARRAY(count, OMAP4MailboxState, 0, 8),
        VMSTATE_UINT32_SUB_ARRAY(status, OMAP4MailboxState, 0, 3),
        VMSTATE_UINT32_SUB_ARRAY(enable, OMAP4MailboxState, 0, 3),
        VMSTATE_SUB_ARRAY(words, OMAP4MailboxState, 32, 16, 2,
                         vmstate_info_uint32, uint32_t),
        VMSTATE_SUB_ARRAY(head, OMAP4MailboxState, 8, 4, 2,
                         vmstate_info_uint8, uint8_t),
        VMSTATE_SUB_ARRAY(count, OMAP4MailboxState, 8, 4, 2,
                         vmstate_info_uint8, uint8_t),
        VMSTATE_SUB_ARRAY(status, OMAP4MailboxState, 3, 1, 2,
                         vmstate_info_uint32, uint32_t),
        VMSTATE_SUB_ARRAY(enable, OMAP4MailboxState, 3, 1, 2,
                         vmstate_info_uint32, uint32_t),
        VMSTATE_END_OF_LIST()
    },
};

static void mailbox_init(Object *obj)
{
    OMAP4MailboxState *s = OMAP4_MAILBOX(obj);

    memory_region_init_io(&s->mmio, obj, &mailbox_ops, s, TYPE_OMAP4_MAILBOX,
                          0x200);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    for (unsigned i = 0; i < ARRAY_SIZE(s->irq); i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq[i]);
    }
}

static const Property mailbox_properties[] = {
    DEFINE_PROP_BOOL("dra7", OMAP4MailboxState, dra7, false),
};

static void mailbox_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, mailbox_reset);
    dc->vmsd = &vmstate_mailbox;
    device_class_set_props(dc, mailbox_properties);
}

static const TypeInfo mailbox_info = {
    .name = TYPE_OMAP4_MAILBOX,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OMAP4MailboxState),
    .instance_init = mailbox_init,
    .class_init = mailbox_class_init,
};

static void mailbox_register_types(void)
{
    type_register_static(&mailbox_info);
}

type_init(mailbox_register_types)

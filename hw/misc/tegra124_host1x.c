/*
 * T124 HOST1X synchronization and bounded display-command DMA subset.
 * Register layout: Linux hw_host1x04_sync.h and host1x04_info.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/tegra124_host1x.h"
#include "system/address-spaces.h"

#define SYNC_OFFSET 0x2100
#define NUM_SYNCPTS 192
#define NUM_BANKS (NUM_SYNCPTS / 32)
#define NUM_BASES 64
#define NUM_MLOCKS 16
#define NUM_CHANNELS 12

typedef struct Tegra124Host1xStream {
    uint32_t next;
    uint32_t mask;
    uint32_t gather;
    uint32_t gather_end;
    unsigned code;
    unsigned method;
    unsigned count;
    bool active;
} Tegra124Host1xStream;

typedef struct Tegra124Host1xChannel {
    uint32_t start;
    uint32_t put;
    uint32_t get;
    uint32_t end;
    uint32_t control;
    unsigned class;
    bool faulted;
    bool waiting;
    bool delaying;
    int64_t delay_deadline;
    unsigned wait_id;
    uint32_t wait_threshold;
    Tegra124Host1xStream stream[5];
} Tegra124Host1xChannel;

struct Tegra124Host1xState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    MemoryRegion *dma_mr;
    AddressSpace dma_as;
    qemu_irq irq;
    QEMUBH *resume_bh;
    QEMUTimer *delay_timer;
    bool reset_asserted;
    uint32_t syncpt[NUM_SYNCPTS];
    uint32_t threshold[NUM_SYNCPTS];
    uint32_t enabled[NUM_BANKS];
    uint32_t pending[NUM_BANKS];
    uint32_t base[NUM_BASES];
    uint32_t mlock[NUM_MLOCKS];
    uint32_t fifo_setup[NUM_CHANNELS];
    Tegra124Host1xChannel channel[NUM_CHANNELS];
    uint32_t usec_clk;
    uint32_t ctxsw_timeout;
    uint32_t busy_timeout;
    uint32_t intmask;
    uint32_t intc0mask;
    uint32_t hintmask;
    uint32_t hintmask_ext;
};

static void host1x_update(Tegra124Host1xState *s)
{
    bool level = false;

    for (unsigned bank = 0; bank < NUM_BANKS; bank++) {
        for (unsigned bit = 0; bit < 32; bit++) {
            unsigned id = bank * 32 + bit;

            if ((s->enabled[bank] & BIT(bit)) &&
                (int32_t)(s->syncpt[id] - s->threshold[id]) >= 0) {
                s->pending[bank] |= BIT(bit);
            }
        }
        level |= !!(s->pending[bank] & s->enabled[bank]);
    }
    qemu_set_irq(s->irq, level);
}

bool tegra124_host1x_syncpt_incr(Tegra124Host1xState *s, unsigned id)
{
    if (s->reset_asserted || id >= NUM_SYNCPTS) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "tegra124-host1x: invalid syncpoint increment %u%s\n",
                      id, s->reset_asserted ? " during reset" : "");
        return false;
    }
    s->syncpt[id]++;
    host1x_update(s);
    qemu_bh_schedule(s->resume_bh);
    return true;
}

static bool host1x_fetch(Tegra124Host1xState *s, uint32_t address,
                         uint32_t *value)
{
    MemTxResult result;

    *value = address_space_ldl_le(&s->dma_as, address,
                                 MEMTXATTRS_UNSPECIFIED, &result);
    if (result != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "tegra124-host1x: DMA read failed at 0x%x\n", address);
        return false;
    }
    return true;
}

static void host1x_schedule_delay(Tegra124Host1xState *s)
{
    int64_t next = INT64_MAX;

    for (unsigned i = 0; i < NUM_CHANNELS; i++) {
        Tegra124Host1xChannel *ch = &s->channel[i];

        if (ch->delaying && !ch->faulted && !(ch->control & 1)) {
            next = MIN(next, ch->delay_deadline);
        }
    }
    if (next == INT64_MAX) {
        timer_del(s->delay_timer);
    } else {
        timer_mod(s->delay_timer, next);
    }
}

static bool host1x_method(Tegra124Host1xState *s, Tegra124Host1xChannel *ch,
                          unsigned method, uint32_t value)
{
    MemTxResult result;

    if (ch->class == 0x5d &&
        (method == 0 || method == 0x10 || method == 0x11)) {
        address_space_stl_le(&address_space_memory, 0x54340000 + method * 4,
                             value, MEMTXATTRS_UNSPECIFIED, &result);
        return result == MEMTX_OK;
    }
    if (ch->class == 0x70 || ch->class == 0x71) {
        hwaddr base = ch->class == 0x70 ? 0x54200000 : 0x54240000;

        address_space_stl_le(&address_space_memory, base + method * 4,
                             value, MEMTXATTRS_UNSPECIFIED, &result);
        if (result != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "tegra124-host1x: failed channel=%td class=0x%x "
                          "method=0x%x value=0x%x result=%d\n",
                          ch - s->channel, ch->class, method, value, result);
        }
        return result == MEMTX_OK;
    }
    if (ch->class == 1 && method == 0 && value < NUM_SYNCPTS) {
        return tegra124_host1x_syncpt_incr(s, value);
    }
    if (ch->class == 1 && method == 0x10 && value <= 0xfffff) {
        ch->delaying = value != 0;
        ch->delay_deadline = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                             (int64_t)value * 1000;
        host1x_schedule_delay(s);
        return true;
    }
    if (ch->class == 1 && method == 8) {
        unsigned id = value >> 24;

        if (id < NUM_SYNCPTS && s->syncpt[id] < 0x1000000) {
            ch->wait_id = id;
            ch->wait_threshold = value & 0xffffff;
            ch->waiting = s->syncpt[id] < ch->wait_threshold;
            return true;
        }
        qemu_log_mask(LOG_UNIMP,
                      "tegra124-host1x: unsupported invalid/wrapped wait "
                      "id=%u threshold=0x%x current=0x%x\n", id,
                      value & 0xffffff, id < NUM_SYNCPTS ? s->syncpt[id] : 0);
        return false;
    }
    qemu_log_mask(LOG_UNIMP,
                  "tegra124-host1x: unsupported class=0x%x "
                  "method=0x%x value=0x%x\n", ch->class, method, value);
    return false;
}

static bool host1x_stream(Tegra124Host1xState *s, Tegra124Host1xChannel *ch,
                          uint32_t *get, uint32_t end, unsigned depth,
                          unsigned *budget)
{
    Tegra124Host1xStream *f = &ch->stream[depth];

    while (*get != end) {
        uint32_t limit = !depth && *get > end ? ch->end : end;

        if (*get >= limit || (!depth && *get < ch->start)) {
            return false;
        }
        if (!f->active) {
            uint32_t op;

            if (!*budget || !host1x_fetch(s, *get, &op)) {
                return false;
            }
            (*budget)--;
            f->next = *get + 4;
            f->code = op >> 28;
            f->method = (op >> 16) & 0xfff;
            f->count = op & 0xffff;
            f->mask = 0;
            switch (f->code) {
            case 0:
                ch->class = (op >> 6) & 0x3ff;
                f->mask = op & 0x3f;
                f->count = ctpop32(f->mask);
                break;
            case 1:
            case 2:
                break;
            case 3:
                f->mask = f->count;
                f->count = ctpop32(f->mask);
                break;
            case 4:
                if (!host1x_method(s, ch, f->method, f->count)) {
                    return false;
                }
                f->count = 0;
                break;
            case 5:
                if (depth || (op << 4) < ch->start ||
                    (op << 4) >= ch->end) {
                    return false;
                }
                f->next = op << 4;
                f->count = 0;
                break;
            case 6:
                if ((op & 0xffffc000) != 0x60000000 || depth >= 4 ||
                    f->next >= limit || !*budget ||
                    !host1x_fetch(s, f->next, &f->gather) ||
                    (f->gather & 3) ||
                    f->count > (UINT32_MAX - f->gather) / 4) {
                    return false;
                }
                (*budget)--;
                f->gather_end = f->gather + f->count * 4;
                f->next += 4;
                f->count = 0;
                break;
            case 14: {
                unsigned lock = op & 0xff, subop = (op >> 24) & 0xf;
                uint32_t owner = 1U | ((ch - s->channel) << 8);

                if ((op & 0xffff00) || lock >= NUM_MLOCKS || subop > 1 ||
                    (s->mlock[lock] && s->mlock[lock] != owner) ||
                    (subop && s->mlock[lock] != owner)) {
                    qemu_log_mask(LOG_UNIMP,
                                  "tegra124-host1x: unsupported/contended "
                                  "channel lock opcode 0x%x\n", op);
                    return false;
                }
                s->mlock[lock] = subop ? 0 : owner;
                f->count = 0;
                break;
            }
            default:
                qemu_log_mask(LOG_UNIMP,
                              "tegra124-host1x: unsupported opcode 0x%x "
                              "at 0x%x\n", op, *get);
                return false;
            }
            f->active = true;
        }
        if (f->code == 6) {
            if (!host1x_stream(s, ch, &f->gather, f->gather_end,
                              depth + 1, budget)) {
                return false;
            }
        }
        if (ch->waiting || ch->delaying) {
            return true;
        }
        while (f->count) {
            unsigned reg = f->method;
            uint32_t value;

            if (f->next >= limit || !*budget ||
                !host1x_fetch(s, f->next, &value)) {
                return false;
            }
            (*budget)--;
            if (f->mask) {
                reg += ctz32(f->mask);
                f->mask &= f->mask - 1;
            }
            if (reg >= 0x1000 || !host1x_method(s, ch, reg, value)) {
                return false;
            }
            if (f->code == 1) {
                f->method++;
            }
            f->next += 4;
            f->count--;
            if (ch->waiting || ch->delaying) {
                return true;
            }
        }
        *get = f->next;
        f->active = false;
    }
    return true;
}

static bool host1x_channel_run(Tegra124Host1xState *s,
                               Tegra124Host1xChannel *ch)
{
    unsigned budget = 65536;

    if (ch->control & 1) {
        return true;
    }
    if (ch->delaying && !ch->faulted) {
        if (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) < ch->delay_deadline) {
            return true;
        }
        ch->delaying = false;
    }
    if (ch->waiting && !ch->faulted) {
        if (s->syncpt[ch->wait_id] >= 0x1000000) {
            qemu_log_mask(LOG_UNIMP,
                          "tegra124-host1x: wrapped pending wait id=%u\n",
                          ch->wait_id);
            ch->faulted = true;
        } else if (s->syncpt[ch->wait_id] < ch->wait_threshold) {
            return true;
        }
        ch->waiting = false;
    }
    if (!ch->faulted && ch->get >= ch->start &&
        ch->get <= ch->end && ch->put >= ch->start &&
        ch->put <= ch->end &&
        host1x_stream(s, ch, &ch->get, ch->put, 0, &budget)) {
        return true;
    }
    ch->faulted = true;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "tegra124-host1x: channel DMA fault get=0x%x put=0x%x; "
                  "reset required\n", ch->get, ch->put);
    return false;
}

static void host1x_resume(void *opaque)
{
    Tegra124Host1xState *s = opaque;

    if (!s->reset_asserted) {
        for (unsigned i = 0; i < NUM_CHANNELS; i++) {
            if ((s->channel[i].waiting || s->channel[i].delaying) &&
                !s->channel[i].faulted) {
                host1x_channel_run(s, &s->channel[i]);
            }
        }
    }
    host1x_schedule_delay(s);
}

static MemTxResult host1x_read(void *opaque, hwaddr offset, uint64_t *value,
                               unsigned size, MemTxAttrs attrs)
{
    Tegra124Host1xState *s = opaque;
    hwaddr reg = offset - SYNC_OFFSET;

    if (s->reset_asserted) {
        qemu_log_mask(LOG_GUEST_ERROR, "tegra124-host1x: read during reset\n");
        return MEMTX_ERROR;
    }
    if (offset / 0x4000 < NUM_CHANNELS && offset % 0x4000 < 0x100) {
        Tegra124Host1xChannel *ch = &s->channel[offset / 0x4000];

        switch (offset % 0x4000) {
        case 0x14:
            *value = ch->start;
            return MEMTX_OK;
        case 0x18:
            *value = ch->put;
            return MEMTX_OK;
        case 0x1c:
            *value = ch->get;
            return MEMTX_OK;
        case 0x20:
            *value = ch->end;
            return MEMTX_OK;
        case 0x24:
            *value = ch->control;
            return MEMTX_OK;
        }
    }
    switch (reg) {
    case 0x0:
    case 0x20:
    case 0x28: /* No HOST1X general-error sources are generated yet. */
        *value = 0;
        return MEMTX_OK;
    case 0x4:
        *value = s->intmask;
        return MEMTX_OK;
    case 0x8:
        *value = s->intc0mask;
        return MEMTX_OK;
    case 0x24:
        *value = s->hintmask;
        return MEMTX_OK;
    case 0x2c:
        *value = s->hintmask_ext;
        return MEMTX_OK;
    case 0xf80 ... 0x127c:
        *value = s->syncpt[(reg - 0xf80) / 4];
        return MEMTX_OK;
    case 0x1380 ... 0x167c:
        *value = s->threshold[(reg - 0x1380) / 4];
        return MEMTX_OK;
    case 0xe80 ... 0xe94:
        *value = s->pending[(reg - 0xe80) / 4];
        return MEMTX_OK;
    case 0xf20 ... 0xf34:
        *value = s->enabled[(reg - 0xf20) / 4];
        return MEMTX_OK;
    case 0xea0 ... 0xeb4: /* No CPU1 interrupt destination is implemented. */
    case 0xf40 ... 0xf54:
    case 0xf00 ... 0xf14:
    case 0xf60 ... 0xf74:
        *value = 0;
        return MEMTX_OK;
    case 0x600 ... 0x6fc:
        *value = s->base[(reg - 0x600) / 4];
        return MEMTX_OK;
    case 0x2c0 ... 0x2fc: {
        unsigned id = (reg - 0x2c0) / 4;

        *value = !!s->mlock[id];
        if (!s->mlock[id]) {
            s->mlock[id] = 2;
        }
        return MEMTX_OK;
    }
    case 0x340 ... 0x37c:
        *value = s->mlock[(reg - 0x340) / 4];
        return MEMTX_OK;
    case 0xc00 ... 0xc2c:
        *value = s->fifo_setup[(reg - 0xc00) / 4];
        return MEMTX_OK;
    case 0x1a4:
        *value = s->usec_clk;
        return MEMTX_OK;
    case 0x1a8:
        *value = s->ctxsw_timeout;
        return MEMTX_OK;
    case 0x1bc:
        *value = s->busy_timeout;
        return MEMTX_OK;
    default:
        qemu_log_mask(LOG_UNIMP, "tegra124-host1x: unsupported read at 0x%"
                      HWADDR_PRIx "\n", offset);
        return MEMTX_DECODE_ERROR;
    }
}

static MemTxResult host1x_write(void *opaque, hwaddr offset, uint64_t value,
                                unsigned size, MemTxAttrs attrs)
{
    Tegra124Host1xState *s = opaque;
    hwaddr reg = offset - SYNC_OFFSET;

    if (s->reset_asserted) {
        qemu_log_mask(LOG_GUEST_ERROR, "tegra124-host1x: write during reset\n");
        return MEMTX_ERROR;
    }
    if (offset / 0x4000 < NUM_CHANNELS && offset % 0x4000 < 0x100) {
        Tegra124Host1xChannel *ch = &s->channel[offset / 0x4000];
        uint32_t pointer = value & ~3U;

        switch (offset % 0x4000) {
        case 0x14:
        case 0x20:
            if (!(ch->control & 1)) {
                goto unsupported;
            }
            if (offset % 0x4000 == 0x14) {
                ch->start = pointer;
            } else {
                ch->end = pointer;
            }
            return MEMTX_OK;
        case 0x18:
            ch->put = pointer;
            return host1x_channel_run(s, ch) ? MEMTX_OK : MEMTX_ERROR;
        case 0x24:
            if (value != 0 && value != 1 && value != 7) {
                goto unsupported;
            }
            if (value == 7) {
                ch->get = ch->put;
                ch->class = 0;
                ch->faulted = false;
                ch->waiting = false;
                ch->delaying = false;
                memset(ch->stream, 0, sizeof(ch->stream));
            }
            ch->control = value;
            host1x_schedule_delay(s);
            return host1x_channel_run(s, ch) ? MEMTX_OK : MEMTX_ERROR;
        }
    }
    switch (reg) {
    case 0x20:
    case 0x28: /* W1C: no pending general errors. */
        return MEMTX_OK;
    case 0x4:
        s->intmask = value;
        return MEMTX_OK;
    case 0x8:
        s->intc0mask = value;
        return MEMTX_OK;
    case 0x24:
        s->hintmask = value;
        return MEMTX_OK;
    case 0x2c:
        s->hintmask_ext = value;
        return MEMTX_OK;
    case 0xf80 ... 0x127c:
        s->syncpt[(reg - 0xf80) / 4] = value;
        host1x_update(s);
        qemu_bh_schedule(s->resume_bh);
        return MEMTX_OK;
    case 0x1380 ... 0x167c:
        s->threshold[(reg - 0x1380) / 4] = value;
        host1x_update(s);
        return MEMTX_OK;
    case 0xe80 ... 0xe94:
        s->pending[(reg - 0xe80) / 4] &= ~value;
        host1x_update(s);
        return MEMTX_OK;
    case 0xea0 ... 0xeb4:
        return MEMTX_OK;
    case 0xf00 ... 0xf14:
        s->enabled[(reg - 0xf00) / 4] &= ~value;
        host1x_update(s);
        return MEMTX_OK;
    case 0xf20 ... 0xf34:
        s->enabled[(reg - 0xf20) / 4] |= value;
        host1x_update(s);
        return MEMTX_OK;
    case 0xf40 ... 0xf54:
        if (value) {
            break;
        }
        return MEMTX_OK;
    case 0xf60 ... 0xf74: {
        unsigned bank = (reg - 0xf60) / 4;

        for (unsigned bit = 0; bit < 32; bit++) {
            if (value & BIT(bit)) {
                tegra124_host1x_syncpt_incr(s, bank * 32 + bit);
            }
        }
        return MEMTX_OK;
    }
    case 0x600 ... 0x6fc:
        s->base[(reg - 0x600) / 4] = value;
        return MEMTX_OK;
    case 0x2c0 ... 0x2fc:
        if (value || (s->mlock[(reg - 0x2c0) / 4] & 1)) {
            break;
        }
        s->mlock[(reg - 0x2c0) / 4] = 0;
        return MEMTX_OK;
    case 0xc00 ... 0xc2c:
        s->fifo_setup[(reg - 0xc00) / 4] = value & 0x03ff03ff;
        return MEMTX_OK;
    case 0x1a4:
        s->usec_clk = value;
        return MEMTX_OK;
    case 0x1a8:
        s->ctxsw_timeout = value;
        return MEMTX_OK;
    case 0x1bc:
        s->busy_timeout = value;
        return MEMTX_OK;
    default:
        break;
    }
unsupported:
    qemu_log_mask(LOG_UNIMP, "tegra124-host1x: unsupported write at 0x%"
                  HWADDR_PRIx " value 0x%" PRIx64 "\n", offset, value);
    return MEMTX_DECODE_ERROR;
}

static const MemoryRegionOps host1x_ops = {
    .read_with_attrs = host1x_read,
    .write_with_attrs = host1x_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void host1x_reset(DeviceState *dev)
{
    Tegra124Host1xState *s = TEGRA124_HOST1X(dev);

    qemu_bh_cancel(s->resume_bh);
    timer_del(s->delay_timer);
    memset(s->syncpt, 0, sizeof(s->syncpt));
    memset(s->threshold, 0, sizeof(s->threshold));
    memset(s->enabled, 0, sizeof(s->enabled));
    memset(s->pending, 0, sizeof(s->pending));
    memset(s->base, 0, sizeof(s->base));
    memset(s->mlock, 0, sizeof(s->mlock));
    memset(s->fifo_setup, 0, sizeof(s->fifo_setup));
    memset(s->channel, 0, sizeof(s->channel));
    for (unsigned i = 0; i < NUM_CHANNELS; i++) {
        s->channel[i].control = 1;
    }
    s->usec_clk = 0;
    s->ctxsw_timeout = 0;
    s->busy_timeout = 0;
    s->intmask = 0;
    s->intc0mask = 0;
    s->hintmask = 0;
    s->hintmask_ext = 0;
    qemu_set_irq(s->irq, 0);
}

static void host1x_reset_line(void *opaque, int n, int level)
{
    Tegra124Host1xState *s = opaque;

    if (level && !s->reset_asserted) {
        host1x_reset(DEVICE(s));
    }
    s->reset_asserted = level;
}

static void host1x_init(Object *obj)
{
    Tegra124Host1xState *s = TEGRA124_HOST1X(obj);

    memory_region_init_io(&s->iomem, obj, &host1x_ops, s,
                          TYPE_TEGRA124_HOST1X, 0x34000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    qdev_init_gpio_in_named(DEVICE(obj), host1x_reset_line, "reset", 1);
}

static void host1x_realize(DeviceState *dev, Error **errp)
{
    Tegra124Host1xState *s = TEGRA124_HOST1X(dev);

    if (!s->dma_mr) {
        error_setg(errp, "tegra124-host1x requires a DMA address space");
        return;
    }
    address_space_init(&s->dma_as, s->dma_mr, "tegra124-host1x-dma");
    s->resume_bh = qemu_bh_new(host1x_resume, s);
    s->delay_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, host1x_resume, s);
}

static void host1x_unrealize(DeviceState *dev)
{
    Tegra124Host1xState *s = TEGRA124_HOST1X(dev);

    qemu_bh_delete(s->resume_bh);
    timer_free(s->delay_timer);
    address_space_destroy(&s->dma_as);
}

static const Property host1x_properties[] = {
    DEFINE_PROP_LINK("dma", Tegra124Host1xState, dma_mr, TYPE_MEMORY_REGION,
                     MemoryRegion *),
};

static void host1x_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = host1x_realize;
    dc->unrealize = host1x_unrealize;
    device_class_set_props(dc, host1x_properties);
    device_class_set_legacy_reset(dc, host1x_reset);
}

static const TypeInfo host1x_info = {
    .name = TYPE_TEGRA124_HOST1X,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Tegra124Host1xState),
    .instance_init = host1x_init,
    .class_init = host1x_class_init,
};

static void host1x_register_types(void)
{
    type_register_static(&host1x_info);
}

type_init(host1x_register_types)

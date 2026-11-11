/*
 * T124 display DMA registers, bounded RGB scanout and clocked frame events.
 * Register indices, masks and defaults follow NVIDIA's public T12x ardisplay.h.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/irq.h"
#include "hw/misc/tegra124_dc.h"
#include "hw/misc/tegra124_host1x.h"
#include "ui/console.h"
#include "system/ramblock.h"

enum {
    DC_ASSEMBLY,
    DC_ARM,
    DC_ACTIVE,
    DC_BANKS,
};

struct Tegra124DCState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    MemoryRegion *dma_mr[3];
    AddressSpace dma_as[3];
    Tegra124Host1xState *host1x;
    Clock *clock;
    QEMUTimer *frame_timer;
    QemuConsole *console;
    qemu_irq irq;
    qemu_irq frame;
    uint32_t interrupt_status;
    uint32_t pending_activation;
    uint8_t sync_queue[4][32];
    unsigned sync_count[4];
    bool reset_asserted;
    bool experimental_window_scanout;
    uint32_t sync_control[4];
    uint32_t interrupt_control[4];
    uint32_t state_access;
    uint32_t window_header;
    uint32_t activation_control;
    uint32_t vsync_syncpt;
    uint32_t general[DC_BANKS][0x500];
    uint32_t window[DC_BANKS][3][0x220];
};

static bool dc_window_register(Tegra124DCState *s, unsigned index,
                                uint32_t *mask)
{
    static const uint32_t masks[] = {
        [0x001 ... 0x010] = UINT32_MAX,
        [0x011] = 0xff, [0x012] = 0x3ff,
        [0x013 ... 0x014] = 0x7ff, [0x015 ... 0x016] = 0x3ff,
        [0x017 ... 0x018] = 0x7ff, [0x019 ... 0x028] = 0xff,
        [0x029 ... 0x038] = 0x3ff,
        [0x101 ... 0x102] = 7, [0x103] = 0x7f,
        [0x104 ... 0x105] = 0x1fff1fff, [0x106] = 0x1fff7fff,
        [0x107 ... 0x108] = 0xffff, [0x109 ... 0x10a] = UINT32_MAX,
        [0x10e] = 0x70707,
        [0x116] = 0xfffffff, [0x117 ... 0x118] = 0x3377,
        [0x119] = 0xffff,
        [0x200 ... 0x209] = UINT32_MAX,
        [0x20b] = 0x73, [0x20c] = 0x7f,
        [0x20d ... 0x212] = 3,
    };

    if (index == 0x700) {
        *mask = s->experimental_window_scanout ? BIT(30) : 0;
        return true;
    }
    if (index >= 0x600 && index - 0x600 < ARRAY_SIZE(masks) &&
        masks[index - 0x600]) {
        *mask = masks[index - 0x600];
        return true;
    }
    return false;
}

static bool dc_general_register(unsigned index, uint32_t *mask)
{
    static const uint32_t masks[] = {
        [0x036] = 0x50155,
        [0x302] = 0x55555555, [0x303] = 0x55150005,
        [0x304] = 0x550515, [0x305] = 0x555,
        [0x306] = 0x55555555, [0x307] = 0x55150005,
        [0x308] = 0x550515, [0x309] = 0x555,
        [0x30a] = UINT32_MAX, [0x30b] = 0xff3f000f,
        [0x30c] = 0xff0f3f, [0x30d] = 0xfff,
        [0x314] = 0x77777777, [0x315] = 0x77777777,
        [0x316] = 0x77, [0x317] = 0x77770777,
        [0x318] = 0x770777, [0x319] = 0x7777, [0x31a] = 0x777777,
        [0x400] = 0x51d1500, [0x401] = 0x50000,
        [0x402] = 0x62010000, [0x403] = 0x3f7f7f7f,
        [0x404] = 0x3f3f3f3f, [0x405] = 0x1fff,
        [0x406 ... 0x40a] = 0x1fff1fff,
        [0x40b] = 0xfd8, [0x40c ... 0x40f] = 0x1fff1fff,
        [0x410] = 0xfd8, [0x411 ... 0x414] = 0x1fff1fff,
        [0x415] = 0xfd8, [0x416 ... 0x419] = 0x1fff1fff,
        [0x41a] = 0x1fff0fd0, [0x41b ... 0x41d] = 0x1fff1fff,
        [0x41e] = 0x1fff0fd0, [0x41f ... 0x421] = 0x1fff1fff,
        [0x422] = 0x1fff0110, [0x423] = 0x1fff1fff,
        [0x424] = 0x1fff0110, [0x425] = 0x1fff1fff,
        [0x426 ... 0x427] = 0x1fff1ff3,
        [0x428] = 0x1fff00d3, [0x429] = 0xffff,
        [0x42a ... 0x42d] = UINT32_MAX,
        [0x42e] = 0xfff, [0x42f] = 0x30f, [0x430] = 0xf1733cf,
        [0x431] = 0xff00ff, [0x432] = 0x1f, [0x433] = 0xff,
        [0x434] = 0x1f, [0x436 ... 0x439] = UINT32_MAX,
        [0x480] = 0x1f000f, [0x481 ... 0x483] = UINT32_MAX,
        [0x4c1] = 3, [0x4e4] = UINT32_MAX,
    };

    if (index == 0x032) {
        *mask = 0x20; /* STOP or continuous scanout. */
        return true;
    }
    if (index == 0x031 || index == 0x300 ||
        index == 0x4c2 || index == 0x4e5 || index == 0x4ef) {
        /* Optional command, CRC, dimming, interlace and CSC disabled. */
        *mask = 0;
        return true;
    }
    if (index < ARRAY_SIZE(masks) && masks[index]) {
        *mask = masks[index];
        return true;
    }
    return false;
}

static void dc_update_irq(Tegra124DCState *s)
{
    qemu_set_irq(s->irq, !!(s->interrupt_status & s->interrupt_control[0]));
}

static int64_t dc_frame_period(Tegra124DCState *s, const uint32_t *r)
{
    static const unsigned pixel_dividers[] = {
        2, 3, 4, 6, 8, 12, 16, 18, 24, 32, 36, 48, 26,
    };
    unsigned h = 0, v = 0, divider = (r[0x42e] >> 8) & 15;
    uint64_t hz = clock_get_hz(s->clock);

    if (!hz || divider >= ARRAY_SIZE(pixel_dividers) ||
        !(r[0x409] & 0x1fff) || !(r[0x409] >> 16) ||
        (r[0x409] & 0x1fff) > 4096 || (r[0x409] >> 16) > 4096) {
        return 0;
    }
    for (unsigned i = 0x407; i <= 0x40a; i++) {
        h += r[i] & 0x1fff;
        v += (r[i] >> 16) & 0x1fff;
    }
    return muldiv64((uint64_t)h * v * ((r[0x42e] & 255) + 2) *
                    pixel_dividers[divider], NANOSECONDS_PER_SECOND, hz * 4);
}

static void dc_schedule(Tegra124DCState *s)
{
    int64_t period = dc_frame_period(s, s->general[DC_ACTIVE]);

    timer_del(s->frame_timer);
    if (!s->reset_asserted && s->general[DC_ACTIVE][0x032] && period > 0) {
        timer_mod(s->frame_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + period);
    }
}

static void dc_clock_update(void *opaque, ClockEvent event)
{
    dc_schedule(opaque);
}

static void dc_activate(Tegra124DCState *s, uint32_t value)
{
    if (value & BIT(0)) {
        memcpy(s->general[DC_ACTIVE], s->general[DC_ARM],
               sizeof(s->general[DC_ACTIVE]));
    }
    for (unsigned i = 0; i < 3; i++) {
        if (value & BIT(1 + i)) {
            memcpy(s->window[DC_ACTIVE][i], s->window[DC_ARM][i],
                   sizeof(s->window[DC_ACTIVE][i]));
        }
    }
}

static bool dc_window_valid(const uint32_t *r, const uint32_t *w)
{
    unsigned width = r[0x409] & 0x1fff, height = r[0x409] >> 16;
    unsigned pitch = w[0x10a] & 0xffff;
    unsigned kind = w[0x20b] & 3, block = (w[0x20b] >> 4) & 7;
    bool blend = (w[0x116] & 0x0fffff00) == 0x00ffff00 &&
                 w[0x117] == 0x262 && w[0x118] == 0x224 &&
                 w[0x119] == 0xff00;

    return width && height && width <= 4096 && height <= 4096 &&
           w[0x100] == BIT(30) &&
           (w[0x103] == 12 || w[0x103] == 13) &&
           !w[0x101] && !w[0x102] && !w[0x104] &&
           w[0x105] == r[0x409] &&
           w[0x106] == ((height << 16) | (width * 4)) &&
           !w[0x107] && !w[0x108] && w[0x109] == 0x10001000 &&
           !w[0x206] && !w[0x208] &&
           pitch >= width * 4 && pitch <= 16384 && !(pitch & 3) &&
           !(w[0x200] & 3) &&
           (kind == 0 || (kind == 2 && block <= 5 &&
                         !(pitch & 63) && !(w[0x200] & 511))) &&
           (w[0x116] == 0x1000000 || blend);
}

static bool dc_scanout_valid(Tegra124DCState *s, uint32_t activation)
{
    const uint32_t *r = s->general[activation & 1 ? DC_ARM : DC_ACTIVE];
    unsigned enabled = 0;
    unsigned depths[2];

    for (unsigned i = 0; i < 3; i++) {
        const uint32_t *w =
            s->window[activation & BIT(i + 1) ? DC_ARM : DC_ACTIVE][i];
        bool invalid = false;

        if (!r[0x032] || !w[0x100]) {
            continue;
        }
        enabled++;
        if (!dc_window_valid(r, w) || enabled > 2) {
            invalid = true;
        } else if (enabled == 2 &&
                   (depths[0] == (w[0x116] & 255) ||
                    (w[0x116] & BIT(24)))) {
            invalid = true;
        }
        if (enabled <= 2) {
            depths[enabled - 1] = w[0x116] & 255;
        }
        if (invalid) {
            for (unsigned j = 0; j < 3; j++) {
                const uint32_t *layer =
                    s->window[activation & BIT(j + 1) ? DC_ARM : DC_ACTIVE][j];

                qemu_log_mask(LOG_UNIMP,
                              "tegra124-dc: layer=%u valid=%u "
                              "options=%08x format=%08x position=%08x "
                              "size=%08x prescaled=%08x pitch=%08x "
                              "blend=%08x match=%08x nomatch=%08x "
                              "alpha=%08x address=%08x kind=%08x\n",
                              j, dc_window_valid(r, layer), layer[0x100],
                              layer[0x103], layer[0x104], layer[0x105],
                              layer[0x106], layer[0x10a], layer[0x116],
                              layer[0x117], layer[0x118], layer[0x119],
                              layer[0x200], layer[0x20b]);
            }
            qemu_log_mask(LOG_UNIMP,
                          "tegra124-dc: unsupported window configuration "
                          "activation=0x%x enabled=%u window=%u "
                          "display=0x%x size=0x%x "
                          "w100=%08x w101=%08x w102=%08x w103=%08x "
                          "w104=%08x w105=%08x w106=%08x w107=%08x "
                          "w108=%08x w109=%08x w10a=%08x "
                          "w116=%08x w117=%08x w118=%08x w119=%08x "
                          "w200=%08x w206=%08x w208=%08x w20b=%08x\n",
                          activation, enabled, i, r[0x032], r[0x409],
                          w[0x100], w[0x101], w[0x102], w[0x103],
                          w[0x104], w[0x105], w[0x106], w[0x107],
                          w[0x108], w[0x109], w[0x10a], w[0x116],
                          w[0x117], w[0x118], w[0x119], w[0x200],
                          w[0x206], w[0x208], w[0x20b]);
            return false;
        }
    }
    if (enabled == 2) {
        for (unsigned i = 0; i < 3; i++) {
            const uint32_t *w =
                s->window[activation & BIT(i + 1) ? DC_ARM : DC_ACTIVE][i];

            if (w[0x100] && (w[0x116] & BIT(24))) {
                qemu_log_mask(LOG_UNIMP,
                              "tegra124-dc: layered blend bypass unsupported\n");
                return false;
            }
        }
    }
    return true;
}

static size_t dc_pixel_offset(unsigned x, unsigned y, unsigned pitch,
                              unsigned block_height)
{
    unsigned byte = x * 4;

    if (!block_height) {
        return (size_t)y * pitch + byte;
    }
    /* 64-byte by 8-row GOBs, stacked into block_height-GOB columns. */
    return (size_t)(y / (8 * block_height)) * pitch * 8 * block_height +
           (byte / 64) * 512 * block_height +
           ((y / 8) % block_height) * 512 +
           ((byte % 64) / 32) * 256 + ((y % 8) / 2) * 64 +
           ((byte % 32) / 16) * 32 + (y % 2) * 16 + byte % 16;
}

static inline void dc_draw_pixels(uint32_t *row, const uint8_t *pixels,
                                  unsigned count, bool bgra, bool blend,
                                  const uint32_t *backdrop, bool over_window)
{
    unsigned x = 0;

    if (over_window) {
        for (; x < count; x++) {
            uint32_t p = ldl_le_p(pixels + x * 4);
            uint32_t rgb = bgra ? p & 0xffffff : bswap32(p) >> 8;
            uint32_t dst = row[x];
            unsigned inverse = 255 - (p >> 24);
            uint32_t out = 0;

            for (unsigned shift = 0; shift < 24; shift += 8) {
                unsigned value = ((rgb >> shift) & 255) +
                    ((((dst >> shift) & 255) * inverse + 127) / 255);

                out |= MIN(value, 255) << shift;
            }
            row[x] = out;
        }
        return;
    }
#if !HOST_BIG_ENDIAN
    typedef uint32_t PixelVector __attribute__((vector_size(16)));

    for (; count - x >= 4; x += 4) {
        PixelVector p, rgb;

        memcpy(&p, pixels + x * 4, sizeof(p));
        rgb = bgra ? p & 0x00ffffff :
              ((p & 255) << 16) | (p & 0xff00) | ((p >> 16) & 255);
        if (blend) {
            /* Alpha 255 adds no backdrop; zero pixels select backdrop[0]. */
            PixelVector dst = {
                backdrop[p[0] >> 24], backdrop[p[1] >> 24],
                backdrop[p[2] >> 24], backdrop[p[3] >> 24],
            };
            PixelVector rb = (dst & 0x00ff00ff) + (rgb & 0x00ff00ff);
            PixelVector overflow = rb & 0x01000100;
            PixelVector g = ((dst >> 8) & 255) + ((rgb >> 8) & 255);

            rb = (rb | (overflow - (overflow >> 8))) & 0x00ff00ff;
            rgb = rb | (((g | (PixelVector)(g > 255)) & 255) << 8);
        }
        memcpy(row + x, &rgb, sizeof(rgb));
    }
#endif
    if (!blend) {
        for (; x < count; x++) {
            uint32_t p = ldl_le_p(pixels + x * 4);

            row[x] = bgra ? p & 0xffffff : bswap32(p) >> 8;
        }
        return;
    }
    for (; x < count; x++) {
        uint32_t p = ldl_le_p(pixels + x * 4);
        uint32_t rgb = bgra ? p & 0xffffff : bswap32(p) >> 8;

        if (p < 0xff000000) {
            uint32_t dst, rb, g, overflow;

            if (!p) {
                row[x] = backdrop[0];
                continue;
            }
            dst = backdrop[p >> 24];
            rb = (dst & 0x00ff00ff) + (rgb & 0x00ff00ff);
            /* Expand each lane's ninth bit to a saturating byte mask. */
            overflow = rb & 0x01000100;
            rb = (rb | (overflow - (overflow >> 8))) & 0x00ff00ff;
            g = ((dst >> 8) & 255) + ((rgb >> 8) & 255);
            rgb = rb | (MIN(g, 255) << 8);
        }
        row[x] = rgb;
    }
}

/*
 * Window pixels, either mapped page by page from guest RAM (the window DMA
 * path is an IOMMU, so one mapping never spans a translation page) or a
 * linear copy read through the DMA address space.
 */
typedef struct DCPixels {
    AddressSpace *as;
    const uint8_t *linear;
    uint8_t **pages;
    hwaddr *lengths;
    unsigned count;
    unsigned base;
} DCPixels;

static void dc_pixels_release(DCPixels *p)
{
    for (unsigned i = 0; i < p->count; i++) {
        address_space_unmap(p->as, p->pages[i] + (i ? 0 : p->base),
                            p->lengths[i], false, 0);
    }
    g_free(p->pages);
    g_free(p->lengths);
    p->pages = NULL;
    p->lengths = NULL;
    p->count = 0;
}

static bool dc_pixels_map(DCPixels *p, hwaddr addr, size_t bytes)
{
    unsigned pages = ((addr & 4095) + bytes + 4095) >> 12;

    p->base = addr & 4095;
    p->pages = g_new(uint8_t *, pages);
    p->lengths = g_new(hwaddr, pages);
    for (hwaddr page = addr & ~(hwaddr)4095; p->count < pages; page += 4096) {
        hwaddr start = MAX(page, addr);
        hwaddr length = MIN(page + 4096, addr + bytes) - start;
        hwaddr mapped = length;
        uint8_t *host = address_space_map(p->as, start, &mapped, false,
                                          MEMTXATTRS_UNSPECIFIED);

        /* Bounce buffers are single and partial; use the copy instead. */
        if (host && (mapped != length ||
                     qemu_ram_addr_from_host(host) == RAM_ADDR_INVALID)) {
            address_space_unmap(p->as, host, mapped, false, 0);
            host = NULL;
        }
        if (!host) {
            dc_pixels_release(p);
            return false;
        }
        /* Index every entry by its page, including the unaligned first one. */
        p->pages[p->count] = host - (start - page);
        p->lengths[p->count++] = mapped;
    }
    return true;
}

/* Pixels at offset, and how many bytes stay contiguous from there. */
static inline const uint8_t *dc_pixels_at(const DCPixels *p, size_t offset,
                                          size_t *contiguous)
{
    size_t a = p->base + offset;

    if (p->linear) {
        *contiguous = SIZE_MAX;
        return p->linear + offset;
    }
    *contiguous = 4096 - (a & 4095);
    return p->pages[a >> 12] + (a & 4095);
}

static bool dc_draw_window(Tegra124DCState *s, unsigned window,
                           DisplaySurface *surface, uint32_t background,
                           bool over_window)
{
    const uint32_t *w = s->window[DC_ACTIVE][window];
    unsigned width = w[0x105] & 0x1fff, height = w[0x105] >> 16;
    unsigned pitch = w[0x10a] & 0xffff;
    unsigned block_height = (w[0x20b] & 3) == 2 ?
                            1U << ((w[0x20b] >> 4) & 7) : 0;
    size_t bytes = (size_t)pitch *
                   (block_height ? ROUND_UP(height, block_height * 8) : height);
    bool bgra = w[0x103] == 12;
    /* Premultiplied color over black is already the final RGB value. */
    bool blend = background && !(w[0x116] & BIT(24));
    uint32_t backdrop[256];
    g_autofree uint8_t *copy = NULL;
    DCPixels pixels = { .as = &s->dma_as[window] };
    size_t contiguous;

    if ((uint64_t)w[0x200] + bytes > (1ULL << 32)) {
        goto fault;
    }
    if (!dc_pixels_map(&pixels, w[0x200], bytes)) {
        copy = g_malloc(bytes);
        if (address_space_read(&s->dma_as[window], w[0x200],
                               MEMTXATTRS_UNSPECIFIED, copy,
                               bytes) != MEMTX_OK) {
            goto fault;
        }
        pixels.linear = copy;
    }
    if (blend) {
        /* Validation allows one full-screen window over a uniform backdrop. */
        for (unsigned alpha = 0; alpha < ARRAY_SIZE(backdrop); alpha++) {
            uint32_t rgb = 0;

            for (unsigned shift = 0; shift < 24; shift += 8) {
                unsigned channel = ((background >> shift) & 255) *
                                   (255 - alpha);

                rgb |= ((channel + 127) / 255) << shift;
            }
            backdrop[alpha] = rgb;
        }
    }
    if (!block_height) {
        for (unsigned y = 0; y < height; y++) {
            uint32_t *row = (uint32_t *)(surface_data(surface) +
                                        y * surface_stride(surface));

            /* Pitch and base are 4-byte aligned: no pixel straddles a page. */
            for (unsigned x = 0; x < width;) {
                const uint8_t *src = dc_pixels_at(&pixels,
                                                  (size_t)y * pitch + x * 4,
                                                  &contiguous);
                unsigned count = MIN(width - x, contiguous / 4);

                dc_draw_pixels(row + x, src, count, bgra, blend, backdrop,
                               over_window);
                x += count;
            }
        }
    } else {
        for (unsigned y = 0; y < height; y += 8) {
            size_t offset = dc_pixel_offset(0, y, pitch, block_height);

            /* Consume each page-contained 16x8 GOB before moving columns. */
            unsigned x = 0;

            for (; width - x >= 16;
                 x += 16, offset += 512 * block_height) {
                const uint8_t *gob = dc_pixels_at(&pixels, offset,
                                                  &contiguous);

                for (unsigned gy = 0; gy < MIN(8, height - y); gy++) {
                    uint32_t *row = (uint32_t *)(surface_data(surface) +
                        (y + gy) * surface_stride(surface)) + x;
                    const uint8_t *src = gob + (gy / 2) * 64 + (gy % 2) * 16;
                    dc_draw_pixels(row, src, 4, bgra, blend, backdrop,
                                   over_window);
                    dc_draw_pixels(row + 4, src + 32, 4, bgra, blend,
                                   backdrop, over_window);
                    dc_draw_pixels(row + 8, src + 256, 4, bgra, blend,
                                   backdrop, over_window);
                    dc_draw_pixels(row + 12, src + 288, 4, bgra, blend,
                                   backdrop, over_window);
                }
            }
            for (unsigned gy = 0; gy < MIN(8, height - y); gy++) {
                uint32_t *row = (uint32_t *)(surface_data(surface) +
                    (y + gy) * surface_stride(surface));

                for (unsigned tail = x; tail < width; tail++) {
                    dc_draw_pixels(row + tail,
                                   dc_pixels_at(&pixels,
                                                dc_pixel_offset(tail, y + gy,
                                                                pitch,
                                                                block_height),
                                                &contiguous),
                                   1, bgra, blend, backdrop, over_window);
                }
            }
        }
    }
    dc_pixels_release(&pixels);
    return true;
fault:
    qemu_log_mask(LOG_GUEST_ERROR,
                  "tegra124-dc: window %u DMA fault at 0x%x, size %zu\n",
                  window, w[0x200], bytes);
    return false;
}

static void dc_frame(void *opaque)
{
    Tegra124DCState *s = opaque;
    uint32_t *r = s->general[DC_ACTIVE];
    unsigned width = r[0x409] & 0x1fff, height = r[0x409] >> 16;
    uint32_t color = r[0x4e4];
    DisplaySurface *surface;
    unsigned windows[2], window_count = 0;

    if (!dc_scanout_valid(s, 0)) {
        return;
    }
    qemu_console_resize(s->console, width, height);
    surface = qemu_console_surface(s->console);
    color = ((color & 255) << 16) | (color & 0xff00) | ((color >> 16) & 255);
    for (unsigned window = 0; window < 3; window++) {
        if (s->window[DC_ACTIVE][window][0x100]) {
            if (window_count == ARRAY_SIZE(windows)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "tegra124-dc: too many active windows\n");
                return;
            }
            windows[window_count++] = window;
        }
    }
    if (window_count == 2 &&
        (s->window[DC_ACTIVE][windows[0]][0x116] & 255) <
        (s->window[DC_ACTIVE][windows[1]][0x116] & 255)) {
        unsigned tmp = windows[0];

        windows[0] = windows[1];
        windows[1] = tmp;
    }
    /* A validated window covers the entire output, including transparent pixels. */
    if (!window_count || !dc_draw_window(s, windows[0], surface, color, false)) {
        for (unsigned y = 0; y < height; y++) {
            uint32_t *row = (uint32_t *)(surface_data(surface) +
                                        y * surface_stride(surface));
            for (unsigned x = 0; x < width; x++) {
                row[x] = color;
            }
        }
        if (window_count) {
            return;
        }
    }
    if (window_count == 2 &&
        !dc_draw_window(s, windows[1], surface, color, true)) {
        return;
    }
    qemu_console_update_full(s->console);
    qemu_irq_pulse(s->frame);
    dc_activate(s, s->pending_activation);
    s->pending_activation = 0;
    s->interrupt_status |= (BIT(1) | BIT(2)) & s->interrupt_control[1];
    dc_update_irq(s);
    if (s->vsync_syncpt & BIT(8)) {
        tegra124_host1x_syncpt_incr(s->host1x, s->vsync_syncpt & 255);
    }
    for (unsigned queue = 0; queue < 4; queue++) {
        unsigned count = s->sync_count[queue];

        s->sync_count[queue] = 0;
        for (unsigned i = 0; i < count; i++) {
            tegra124_host1x_syncpt_incr(s->host1x, s->sync_queue[queue][i]);
        }
    }
    dc_schedule(s);
}

static bool dc_latch(Tegra124DCState *s, uint32_t value)
{
    uint32_t activation = (s->pending_activation | value) & 15;

    if (value & BIT(8)) {
        memcpy(s->general[DC_ARM], s->general[DC_ASSEMBLY],
               sizeof(s->general[DC_ARM]));
    }
    for (unsigned i = 0; i < 3; i++) {
        if (value & BIT(9 + i)) {
            memcpy(s->window[DC_ARM][i], s->window[DC_ASSEMBLY][i],
                   sizeof(s->window[DC_ARM][i]));
        }
    }
    if (!dc_scanout_valid(s, activation)) {
        return false;
    }
    if ((value & BIT(0)) && s->general[DC_ARM][0x032] &&
        (!dc_frame_period(s, s->general[DC_ARM]) || s->activation_control)) {
        qemu_log_mask(LOG_UNIMP, "tegra124-dc: invalid scanout timing/clock\n");
        return false;
    }
    if (s->general[DC_ACTIVE][0x032]) {
        s->pending_activation |= value & 15;
    } else {
        dc_activate(s, value & 15);
        dc_schedule(s);
    }
    return true;
}

static MemTxResult dc_read(void *opaque, hwaddr offset, uint64_t *value,
                           unsigned size, MemTxAttrs attrs)
{
    Tegra124DCState *s = opaque;
    uint32_t mask;
    unsigned bank = s->state_access & BIT(0) ? DC_ACTIVE : DC_ASSEMBLY;

    if (s->reset_asserted) {
        qemu_log_mask(LOG_GUEST_ERROR, "tegra124-dc: read during reset\n");
        return MEMTX_ERROR;
    }
    if (dc_general_register(offset / 4, &mask)) {
        *value = s->general[bank][offset / 4];
        return MEMTX_OK;
    }
    if (dc_window_register(s, offset / 4, &mask) &&
        is_power_of_2(s->window_header)) {
        *value = s->window[bank][ctz32(s->window_header) - 4]
                          [offset / 4 - 0x600];
        return MEMTX_OK;
    }
    switch (offset / 4) {
    case 0x001:
    case 0x009:
    case 0x011:
    case 0x019:
        *value = s->sync_control[(offset / 4) / 8];
        return MEMTX_OK;
    case 0x002:
    case 0x00a:
    case 0x012:
    case 0x01a: /* No increment errors are latched. */
        *value = 0;
        return MEMTX_OK;
    case 0x037:
        *value = s->interrupt_status;
        return MEMTX_OK;
    case 0x041:
        *value = s->pending_activation;
        return MEMTX_OK;
    case 0x038 ... 0x03b:
        *value = s->interrupt_control[offset / 4 - 0x038];
        return MEMTX_OK;
    case 0x040:
        *value = s->state_access;
        return MEMTX_OK;
    case 0x042:
        *value = s->window_header;
        return MEMTX_OK;
    case 0x043:
        *value = s->activation_control;
        return MEMTX_OK;
    case 0x028:
        *value = s->vsync_syncpt;
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP, "tegra124-dc: unsupported read at 0x%"
                  HWADDR_PRIx "\n", s->iomem.addr + offset);
    return MEMTX_DECODE_ERROR;
}

static MemTxResult dc_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size, MemTxAttrs attrs)
{
    Tegra124DCState *s = opaque;
    uint32_t mask;
    unsigned bank = s->state_access & BIT(2) ? DC_ACTIVE : DC_ASSEMBLY;

    if (s->reset_asserted) {
        qemu_log_mask(LOG_GUEST_ERROR, "tegra124-dc: write during reset\n");
        return MEMTX_ERROR;
    }
    if (dc_general_register(offset / 4, &mask)) {
        if (value & ~mask) {
            goto unsupported;
        }
        if (bank == DC_ACTIVE && (s->general[DC_ACTIVE][0x032] ||
                                 (offset / 4 == 0x032 && value))) {
            goto unsupported;
        }
        s->general[bank][offset / 4] = value;
        return MEMTX_OK;
    }
    if (dc_window_register(s, offset / 4, &mask)) {
        if ((value & ~mask) || !s->window_header ||
            (bank == DC_ACTIVE && s->general[DC_ACTIVE][0x032])) {
            goto unsupported;
        }
        for (unsigned i = 0; i < 3; i++) {
            if (s->window_header & BIT(4 + i)) {
                s->window[bank][i][offset / 4 - 0x600] = value;
            }
        }
        return MEMTX_OK;
    }
    switch (offset / 4) {
    case 0x000:
    case 0x008:
    case 0x010:
    case 0x018:
        if (((value >> 8) != 0 && (value >> 8) != 3 && (value >> 8) != 5) ||
            (value & 255) >= 192) {
            break;
        }
        if ((value >> 8) && s->general[DC_ACTIVE][0x032]) {
            unsigned queue = offset / 32;

            if (s->sync_count[queue] == ARRAY_SIZE(s->sync_queue[queue])) {
                break;
            }
            s->sync_queue[queue][s->sync_count[queue]++] = value & 255;
            return MEMTX_OK;
        }
        if ((value >> 8) == 5) {
            break;
        }
        return tegra124_host1x_syncpt_incr(s->host1x, value & 0xff) ?
            MEMTX_OK : MEMTX_ERROR;
    case 0x001:
    case 0x009:
    case 0x011:
    case 0x019:
        if (value & ~0x101U) {
            break;
        }
        if (value & 1) {
            s->sync_count[(offset / 4) / 8] = 0;
        }
        s->sync_control[(offset / 4) / 8] = value & 0x100;
        return MEMTX_OK;
    case 0x037:
        s->interrupt_status &= ~value;
        dc_update_irq(s);
        return MEMTX_OK;
    case 0x038 ... 0x03b:
        mask = offset / 4 < 0x03a ? 0x3c1f7bf : 0x3c1f7be;
        s->interrupt_control[offset / 4 - 0x038] = value & mask;
        dc_update_irq(s);
        return MEMTX_OK;
    case 0x040:
        if (value & ~5U) {
            break;
        }
        s->state_access = value;
        return MEMTX_OK;
    case 0x042:
        if (value & ~0x70U) {
            break;
        }
        s->window_header = value;
        return MEMTX_OK;
    case 0x041:
        if (value & ~0xf0fU) {
            break;
        }
        return dc_latch(s, value) ? MEMTX_OK : MEMTX_ERROR;
    case 0x043: /* H/V selection changes require stopped scanout. */
        if ((value & ~0x55U) ||
            (s->general[DC_ACTIVE][0x032] &&
             value != s->activation_control)) {
            break;
        }
        s->activation_control = value;
        return MEMTX_OK;
    case 0x028:
        if ((value & ~0x1ffU) || (value & 0xff) >= 192) {
            break;
        }
        s->vsync_syncpt = value;
        return MEMTX_OK;
    }
unsupported:
    qemu_log_mask(LOG_UNIMP, "tegra124-dc: unsupported write at 0x%"
                  HWADDR_PRIx " = 0x%" PRIx64 "\n",
                  s->iomem.addr + offset, value);
    return MEMTX_ERROR;
}

static const MemoryRegionOps dc_ops = {
    .read_with_attrs = dc_read,
    .write_with_attrs = dc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void dc_reset(DeviceState *dev)
{
    Tegra124DCState *s = TEGRA124_DC(dev);

    timer_del(s->frame_timer);
    if (s->console) {
        DisplaySurface *surface;

        qemu_console_resize(s->console, 4, 4);
        surface = qemu_console_surface(s->console);
        memset(surface_data(surface), 0, surface_stride(surface) * 4);
        qemu_console_update_full(s->console);
    }
    s->interrupt_status = 0;
    s->pending_activation = 0;
    memset(s->sync_count, 0, sizeof(s->sync_count));
    dc_update_irq(s);
    memset(s->sync_control, 0, sizeof(s->sync_control));
    memset(s->interrupt_control, 0, sizeof(s->interrupt_control));
    s->interrupt_control[1] = 1;
    s->interrupt_control[3] = 0x400000;
    s->state_access = 0;
    s->window_header = 0;
    s->activation_control = 0;
    s->vsync_syncpt = 0;
    memset(s->general, 0, sizeof(s->general));
    for (unsigned bank = 0; bank < DC_BANKS; bank++) {
        s->general[bank][0x304] = 0x510104;
        s->general[bank][0x305] = 0x555;
        s->general[bank][0x409] = 0x30003;
        s->general[bank][0x42e] = 6;
        for (unsigned i = 0x481; i <= 0x483; i++) {
            s->general[bank][i] = 0xcf401f1f;
        }
        s->general[bank][0x4c1] = 2;
        s->general[bank][0x4c2] = 0x4000;
    }
    memset(s->window, 0, sizeof(s->window));
    for (unsigned bank = 0; bank < DC_BANKS; bank++) {
        for (unsigned i = 0; i < 3; i++) {
            s->window[bank][i][0x103] = 0xc;
            s->window[bank][i][0x116] = 0x1000000;
            s->window[bank][i][0x20b] = 0x10;
        }
    }
}

static void dc_reset_line(void *opaque, int n, int level)
{
    Tegra124DCState *s = opaque;

    if (level && !s->reset_asserted) {
        dc_reset(DEVICE(s));
    }
    s->reset_asserted = level;
}

static void dc_init(Object *obj)
{
    Tegra124DCState *s = TEGRA124_DC(obj);

    memory_region_init_io(&s->iomem, obj, &dc_ops, s,
                          TYPE_TEGRA124_DC, 0x40000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    qdev_init_gpio_out_named(DEVICE(obj), &s->frame, "frame", 1);
    s->frame_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, dc_frame, s);
    s->clock = qdev_init_clock_in(DEVICE(obj), "dc", dc_clock_update, s,
                                 ClockUpdate);
    qdev_init_gpio_in_named(DEVICE(obj), dc_reset_line, "reset", 1);
}

static void dc_realize(DeviceState *dev, Error **errp)
{
    static const GraphicHwOps ops = {};
    Tegra124DCState *s = TEGRA124_DC(dev);

    if (!s->host1x || !s->dma_mr[0] || !s->dma_mr[1] || !s->dma_mr[2]) {
        error_setg(errp, "tegra124-dc requires HOST1X and window DMA links");
        return;
    }
    for (unsigned i = 0; i < 3; i++) {
        address_space_init(&s->dma_as[i], s->dma_mr[i], "tegra124-dc-dma");
    }
    s->console = qemu_graphic_console_create(dev, 0, &ops, NULL);
}

static void dc_unrealize(DeviceState *dev)
{
    Tegra124DCState *s = TEGRA124_DC(dev);

    timer_del(s->frame_timer);
    for (unsigned i = 0; i < 3; i++) {
        address_space_destroy(&s->dma_as[i]);
    }
    qemu_graphic_console_close(s->console);
    s->console = NULL;
}

static void dc_finalize(Object *obj)
{
    timer_free(TEGRA124_DC(obj)->frame_timer);
}

static const Property dc_properties[] = {
    DEFINE_PROP_BOOL("experimental-window-scanout", Tegra124DCState,
                     experimental_window_scanout, false),
    DEFINE_PROP_LINK("dma-a", Tegra124DCState, dma_mr[0], TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_LINK("dma-b", Tegra124DCState, dma_mr[1], TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_LINK("dma-c", Tegra124DCState, dma_mr[2], TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_LINK("host1x", Tegra124DCState, host1x, TYPE_TEGRA124_HOST1X,
                     Tegra124Host1xState *),
};

static void dc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = dc_realize;
    dc->unrealize = dc_unrealize;
    device_class_set_props(dc, dc_properties);
    device_class_set_legacy_reset(dc, dc_reset);
}

static const TypeInfo dc_info = {
    .name = TYPE_TEGRA124_DC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Tegra124DCState),
    .instance_init = dc_init,
    .instance_finalize = dc_finalize,
    .class_init = dc_class_init,
};

static void dc_register_types(void)
{
    type_register_static(&dc_info);
}

type_init(dc_register_types)

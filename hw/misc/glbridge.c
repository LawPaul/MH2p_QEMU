/*
 * Experimental GLES forwarding transport, extracted from mhi2_harman.c.
 * This optional host service is not a Tegra hardware device.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include <fcntl.h>
#include "qemu/units.h"
#include "qemu/module.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "chardev/char-fe.h"
#include "chardev/char-fd.h"
#include "io/channel-file.h"
#include "qemu/timer.h"
#include <poll.h>
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/misc/glbridge.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/ramblock.h"
#include "trace.h"
#include "glbridge-surface.h"

#define TYPE_GLBRIDGE "experimental-glbridge"
OBJECT_DECLARE_SIMPLE_TYPE(GLBridgeState, GLBRIDGE)

#define GL_BULK_BYTES (1 * MiB)
#define GL_MAX_RECORD (16 * MiB + 20)
#define GL_TX_BYTES (2 * GL_MAX_RECORD)

struct GLBridgeState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    MemoryRegion bulk;
    CharFrontend chr;
    uint8_t tx[4096];
    uint8_t *bulk_tx, *rx;
    unsigned rx_count;
    bool locked;
    uint8_t pending[GL_TX_BYTES];
    unsigned tx_head, tx_count;
    guint watch;
    uint32_t reply_wait_ms;
    bool nonblocking_in;
    uint32_t surface_status;
    unsigned fallback_reports;
    bool profile;
    int64_t profile_start;
    uint64_t profile_ns[2][17];
    uint64_t profile_count[2][17];
    GHashTable *images;
    uint64_t image_bytes;
    uint8_t *scratch;
    size_t scratch_bytes;
    uint8_t *native_scratch;
    size_t native_scratch_bytes;
    bool pipes_sized;
    uint8_t *direct_dest;
    size_t direct_cap, direct_got;
    uint64_t wait_ns, swizzle_ns;
};

/*
 * native, when set, is the guest-layout copy the pixels were swizzled from,
 * so an unchanged refresh is one memcmp instead of a swizzle.
 */
typedef struct GLImage {
    uint32_t bytes;
    uint32_t native_bytes;
    uint32_t params[5];
    uint8_t *native;
    uint8_t pixels[];
} GLImage;

#define GL_SURFACE_MAGIC 0x31414d44
#define GL_SURFACE_HEADER 40
#define GL_SURFACE_OK 0
#define GL_SURFACE_REJECTED 1
#define GL_SURFACE_FAILED 2
#define GL_SURFACE_UNCHANGED 3
#define GL_REFRESH_MAGIC 0x31534452
#define GL_REFRESH_MAX_HEADER 64
#define GL_IMAGE_CACHE_BYTES (256 * MiB)

static gboolean gl_writable(void *unused, GIOCondition condition, void *opaque);
static void gl_image_free(gpointer data);

static void gl_drain(GLBridgeState *s)
{
    unsigned budget = 256 * KiB;

    while (s->tx_count && budget && qemu_chr_fe_backend_open(&s->chr)) {
        unsigned n = MIN(budget, MIN(s->tx_count, GL_TX_BYTES - s->tx_head));
        int written = qemu_chr_fe_write(&s->chr, s->pending + s->tx_head, n);

        if (written <= 0) {
            break;
        }
        s->tx_head = (s->tx_head + written) % GL_TX_BYTES;
        s->tx_count -= written;
        budget -= written;
    }
    if (s->tx_count && !s->watch && qemu_chr_fe_backend_open(&s->chr)) {
        s->watch = qemu_chr_fe_add_watch(&s->chr, G_IO_OUT | G_IO_HUP,
                                        gl_writable, s);
    }
}

static gboolean gl_writable(void *unused, GIOCondition condition, void *opaque)
{
    GLBridgeState *s = opaque;

    s->watch = 0;
    if (!(condition & G_IO_HUP)) {
        gl_drain(s);
    }
    return G_SOURCE_REMOVE;
}

static void gl_queue(GLBridgeState *s, const uint8_t *data, unsigned length)
{
    unsigned tail, first;

    if (length > GL_TX_BYTES - s->tx_count) {
        error_report("glbridge: TX exceeds reserved record capacity");
        return;
    }
    tail = (s->tx_head + s->tx_count) % GL_TX_BYTES;
    first = MIN(length, GL_TX_BYTES - tail);
    memcpy(s->pending + tail, data, first);
    memcpy(s->pending, data + first, length - first);
    s->tx_count += length;
    gl_drain(s);
}

static void gl_clear_tx(GLBridgeState *s)
{
    if (s->watch) {
        g_source_remove(s->watch);
        s->watch = 0;
    }
    s->tx_head = s->tx_count = 0;
}

static void gl_event(void *opaque, QEMUChrEvent event)
{
    GLBridgeState *s = opaque;

    if (event == CHR_EVENT_OPENED) {
        gl_drain(s);
    } else if (event == CHR_EVENT_CLOSED) {
        gl_clear_tx(s);
        s->rx_count = 0;
        s->locked = false;
    }
}

static int gl_can_receive(void *opaque)
{
    GLBridgeState *s = opaque;

    return GL_BULK_BYTES - s->rx_count;
}

static void gl_receive(void *opaque, const uint8_t *buf, int size)
{
    GLBridgeState *s = opaque;

    memcpy(s->rx + s->rx_count, buf, size);
    s->rx_count += size;
}

static int gl_channel_fd(QIOChannel *ioc)
{
    return ioc && object_dynamic_cast(OBJECT(ioc), TYPE_QIO_CHANNEL_FILE) ?
           QIO_CHANNEL_FILE(ioc)->fd : -1;
}

/*
 * A real Tegra GPU answers queries in microseconds. Without this, every
 * synchronous GLES query costs the guest at least one 1 ms usleep() tick plus
 * the main-loop round trip, and frames with hundreds of queries exceed the
 * displaymanager's 2 s watchdog. Waiting here on the vCPU thread for the host
 * renderer's reply costs no guest time under icount.
 */
static void gl_wait_reply(GLBridgeState *s)
{
    Chardev *chr = qemu_chr_fe_get_driver(&s->chr);
    FDChardev *fd_chr;
    int in, out;
    int64_t deadline;

    if (!s->reply_wait_ms || s->rx_count || s->direct_got || !chr ||
        !object_dynamic_cast(OBJECT(chr), TYPE_CHARDEV_FD) ||
        !qemu_chr_fe_backend_open(&s->chr)) {
        return;
    }
    fd_chr = FD_CHARDEV(chr);
    in = gl_channel_fd(fd_chr->ioc_in);
    out = gl_channel_fd(fd_chr->ioc_out);
    if (in < 0 || out < 0) {
        return;
    }
    /*
     * The main loop may have polled this pipe readable before we drained it
     * here; a blocking fd would then hang its dispatch read forever.
     */
    if (!s->nonblocking_in) {
        if (!qio_channel_set_blocking(fd_chr->ioc_in, false, NULL)) {
            return;
        }
        s->nonblocking_in = true;
    }
    if (!s->pipes_sized) {
        /* The 64 KiB default costs a wakeup per 64 KiB of every frame. */
#ifdef F_SETPIPE_SZ
        fcntl(in, F_SETPIPE_SZ, 1 * MiB);
        fcntl(out, F_SETPIPE_SZ, 1 * MiB);
#endif
        s->pipes_sized = true;
    }
    deadline = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + s->reply_wait_ms;
    while (!s->rx_count && !s->direct_got) {
        int64_t remaining = deadline - qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
        struct pollfd pfd = {
            .fd = s->tx_count ? out : in,
            .events = s->tx_count ? POLLOUT : POLLIN,
        };
        ssize_t n;

        if (remaining <= 0 || poll(&pfd, 1, remaining) <= 0) {
            break;
        }
        if (s->tx_count) {
            unsigned chunk = MIN(s->tx_count, GL_TX_BYTES - s->tx_head);

            n = write(out, s->pending + s->tx_head, chunk);
            if (n > 0) {
                s->tx_head = (s->tx_head + n) % GL_TX_BYTES;
                s->tx_count -= n;
            }
        } else {
            if (s->direct_dest) {
                n = read(in, s->direct_dest, s->direct_cap);
                if (n > 0) {
                    s->direct_got = n;
                }
            } else {
                n = read(in, s->rx, GL_BULK_BYTES);
                if (n > 0) {
                    s->rx_count = n;
                }
            }
        }
        if (n < 0 && errno != EAGAIN && errno != EINTR) {
            break;
        }
    }
}

static bool gl_sync_capable(GLBridgeState *s)
{
    Chardev *chr = qemu_chr_fe_get_driver(&s->chr);

    return s->reply_wait_ms && chr &&
           object_dynamic_cast(OBJECT(chr), TYPE_CHARDEV_FD) &&
           qemu_chr_fe_backend_open(&s->chr);
}

static uint32_t gl_le32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/*
 * Common descriptor in the bulk TX window (little-endian u32):
 *   magic, bytes, width, height, pitch, layout, block_shift, flags,
 *   extent, runs, <extra command bytes>, runs x {phys_lo, phys_hi, length}.
 * The runs are the guest-physical pages of the mapped native surface.
 */
static bool gl_surface_parse(const uint8_t *d, uint32_t length,
                             uint32_t magic, uint32_t extra, GLSurface *g)
{
    uint32_t rows, row_bytes, total = 0;

    if (length < GL_SURFACE_HEADER + extra || gl_le32(d) != magic) {
        return false;
    }
    g->bytes = gl_le32(d + 4);
    g->width = gl_le32(d + 8);
    g->height = gl_le32(d + 12);
    g->pitch = gl_le32(d + 16);
    g->layout = gl_le32(d + 20);
    g->shift = gl_le32(d + 24);
    g->flags = gl_le32(d + 28);
    g->extent = gl_le32(d + 32);
    g->runs = gl_le32(d + 36);
    g->run_table = d + GL_SURFACE_HEADER + extra;
    row_bytes = g->width * 4;
    rows = g->height;
    if (!g->width || g->width > 2048 || !g->height || g->height > 2048 ||
        g->bytes != row_bytes * g->height || g->pitch < row_bytes ||
        g->pitch > 16384 || !g->runs ||
        g->runs > (length - GL_SURFACE_HEADER - extra) / 12 ||
        length != GL_SURFACE_HEADER + extra + g->runs * 12) {
        return false;
    }
    if (g->layout == 3) {
        if (g->shift > 5 || g->pitch % 64) {
            return false;
        }
        rows = (rows + (8u << g->shift) - 1) & ~((8u << g->shift) - 1);
    } else if (g->layout != 1) {
        return false;
    }
    if (g->extent != rows * g->pitch) {
        return false;
    }
    for (uint32_t i = 0; i < g->runs; i++) {
        uint32_t run = gl_le32(g->run_table + i * 12 + 8);

        if (!run || run > g->extent - total) {
            return false;
        }
        total += run;
    }
    return total == g->extent;
}

static bool gl_surface_pages(const GLSurface *g, uint8_t *native, bool write)
{
    for (uint32_t i = 0, at = 0; i < g->runs; i++) {
        const uint8_t *r = g->run_table + i * 12;
        hwaddr phys = gl_le32(r) | (hwaddr)gl_le32(r + 4) << 32;
        uint32_t run = gl_le32(r + 8);
        MemTxResult result = write ?
            address_space_write(&address_space_memory, phys,
                                MEMTXATTRS_UNSPECIFIED, native + at, run) :
            address_space_read(&address_space_memory, phys,
                               MEMTXATTRS_UNSPECIFIED, native + at, run);

        if (result != MEMTX_OK) {
            return false;
        }
        at += run;
    }
    return true;
}

/*
 * Map the native surface runs in place when they are host-contiguous guest
 * RAM, so callers can swizzle straight into or out of guest memory instead
 * of going through a bounce buffer. Returns NULL if that is not possible.
 */
static uint8_t *gl_surface_map(const GLSurface *g, bool write)
{
    uint8_t *base = NULL;
    uint32_t i, at = 0;

    for (i = 0; i < g->runs; i++) {
        const uint8_t *r = g->run_table + i * 12;
        hwaddr phys = gl_le32(r) | (hwaddr)gl_le32(r + 4) << 32;
        hwaddr len = gl_le32(r + 8);
        uint8_t *p = address_space_map(&address_space_memory, phys, &len,
                                       write, MEMTXATTRS_UNSPECIFIED);

        /* Bounce buffers would drop untouched padding on write: RAM only. */
        if (p && len == gl_le32(r + 8) && (!i || p == base + at) &&
            qemu_ram_addr_from_host(p) != RAM_ADDR_INVALID) {
            base = i ? base : p;
            at += len;
            continue;
        }
        if (p) {
            address_space_unmap(&address_space_memory, p, len, write, 0);
        }
        break;
    }
    if (i == g->runs) {
        return base;
    }
    for (uint32_t j = 0, off = 0; j < i; j++) {
        uint32_t run = gl_le32(g->run_table + j * 12 + 8);

        address_space_unmap(&address_space_memory, base + off, run, write, 0);
        off += run;
    }
    return NULL;
}

static void gl_surface_unmap(const GLSurface *g, uint8_t *base, bool write)
{
    for (uint32_t i = 0, at = 0; i < g->runs; i++) {
        uint32_t run = gl_le32(g->run_table + i * 12 + 8);

        address_space_unmap(&address_space_memory, base + at, run, write,
                            write ? run : 0);
        at += run;
    }
}

static void gl_surface_unmap_pages(const GLSurface *g, uint8_t **pages,
                                   uint32_t count, bool written)
{
    for (uint32_t i = 0; i < count; i++) {
        uint32_t length = MIN(4096, g->extent - i * 4096);

        address_space_unmap(&address_space_memory, pages[i], length, true,
                            written ? length : 0);
    }
    g_free(pages);
}

/* A native GOB never crosses a logical 4 KiB page boundary. */
static uint8_t **gl_surface_map_pages(const GLSurface *g)
{
    uint8_t **pages = g_new(uint8_t *, DIV_ROUND_UP(g->extent, 4096));
    /* Aliased runs need the bounce path's last-run-wins write ordering. */
    g_autoptr(GHashTable) seen = g_hash_table_new(g_direct_hash, g_direct_equal);
    uint32_t count = 0, offset = 0;

    for (uint32_t i = 0; i < g->runs; i++) {
        const uint8_t *r = g->run_table + i * 12;
        hwaddr phys = gl_le32(r) | (hwaddr)gl_le32(r + 4) << 32;
        uint32_t run = gl_le32(r + 8);

        if ((phys & 4095) || (offset & 4095) ||
            (i + 1 < g->runs && (run & 4095))) {
            goto fail;
        }
        for (uint32_t at = 0; at < run;) {
            hwaddr length = MIN(4096, run - at), mapped = length;
            uint8_t *p = address_space_map(&address_space_memory, phys + at,
                                           &mapped, true,
                                           MEMTXATTRS_UNSPECIFIED);

            if (!p || mapped != length || ((uintptr_t)p & 4095) ||
                qemu_ram_addr_from_host(p) == RAM_ADDR_INVALID ||
                !g_hash_table_add(seen, p)) {
                if (p) {
                    address_space_unmap(&address_space_memory, p, mapped,
                                        true, 0);
                }
                goto fail;
            }
            pages[count++] = p;
            at += length;
        }
        offset += run;
    }
    return pages;
fail:
    gl_surface_unmap_pages(g, pages, count, false);
    return NULL;
}

/*
 * Write the next reply payload (a linear RGBA8 readback) straight into the
 * guest's native Screen surface pages, applying the row flip (flags bit 0)
 * and Tegra block-linear layout here. This replaces two full-frame copies
 * that the guest shim would otherwise execute under emulation per swap.
 */
static GLImage *gl_image_take(GLBridgeState *s, uint64_t key)
{
    gpointer stored_key, stored_image;

    if (g_hash_table_steal_extended(s->images, &key, &stored_key, &stored_image)) {
        GLImage *image = stored_image;

        s->image_bytes -= image->bytes + image->native_bytes;
        g_free(stored_key);
        return image;
    }
    return NULL;
}

static void gl_image_store(GLBridgeState *s, uint64_t key, GLImage *image)
{
    gl_image_free(gl_image_take(s, key));
    if (s->image_bytes + image->bytes + image->native_bytes >
        GL_IMAGE_CACHE_BYTES) {
        /* Dropping entries only costs one extra upload per image. */
        g_hash_table_remove_all(s->images);
        s->image_bytes = 0;
    }
    g_hash_table_insert(s->images, g_memdup2(&key, sizeof(key)), image);
    s->image_bytes += image->bytes + image->native_bytes;
    trace_glbridge_image_cache(image->bytes, image->native_bytes,
                              s->image_bytes, g_hash_table_size(s->images));
}

/* Reused bounce buffer: a fresh 5 MiB malloc per frame is mostly page faults. */
static uint8_t *gl_native_scratch(GLBridgeState *s, size_t bytes)
{
    if (s->native_scratch_bytes < bytes) {
        g_free(s->native_scratch);
        s->native_scratch = g_malloc(bytes);
        s->native_scratch_bytes = bytes;
    }
    return s->native_scratch;
}

static uint8_t *gl_pixel_scratch(GLBridgeState *s, size_t bytes)
{
    if (s->scratch_bytes < bytes) {
        g_free(s->scratch);
        s->scratch = g_malloc(bytes);
        s->scratch_bytes = bytes;
    }
    return s->scratch;
}

static void gl_image_free(gpointer data)
{
    GLImage *image = data;

    if (!image) {
        return;
    }
    g_free(image->native);
    g_free(image);
}

static bool gl_image_native_matches(const GLImage *image, const GLSurface *g)
{
    return image->native && image->native_bytes == g->extent &&
           image->bytes == g->bytes && image->params[0] == g->width &&
           image->params[1] == g->height && image->params[2] == g->pitch &&
           image->params[3] == g->layout && image->params[4] == g->shift;
}

static void gl_image_set_native(GLImage *image, const GLSurface *g,
                                const uint8_t *native)
{
    if (image->native_bytes != g->extent) {
        g_free(image->native);
        image->native = g_malloc(g->extent);
        image->native_bytes = g->extent;
    }
    memcpy(image->native, native, g->extent);
    image->params[0] = g->width;
    image->params[1] = g->height;
    image->params[2] = g->pitch;
    image->params[3] = g->layout;
    image->params[4] = g->shift;
}

/*
 * Flags: bit 0 flips rows; bit 2 means the reply starts with a 4-byte status
 * word (pixmap readback); bit 3 adds extra key_lo, key_hi words and records
 * the pixels as the host texture contents for later refresh comparisons.
 */
static uint32_t gl_surface_write(GLBridgeState *s, uint32_t length)
{
    const uint8_t *d = s->bulk_tx;
    uint8_t *native = NULL;
    GLImage *image = NULL;
    uint32_t received = 0, prefix, flags, total, status;
    uint8_t head[4] = { 0 }, *mapped, *pixels;
    int64_t deadline;
    uint64_t key = 0;
    GLSurface g;

    if (length < GL_SURFACE_HEADER) {
        return GL_SURFACE_REJECTED;
    }
    flags = gl_le32(d + 28);
    prefix = flags & 4 ? 4 : 0;
    if ((flags & ~0xdu) ||
        !gl_surface_parse(d, length, GL_SURFACE_MAGIC, flags & 8 ? 8 : 0, &g) ||
        !gl_sync_capable(s)) {
        return GL_SURFACE_REJECTED;
    }
    if (flags & 8) {
        key = gl_le32(d + GL_SURFACE_HEADER) |
              (uint64_t)gl_le32(d + GL_SURFACE_HEADER + 4) << 32;
    }

    /* Accepted: from here the reply payload is consumed whatever happens. */
    total = prefix + g.bytes;
    if (flags & 8) {
        image = g_malloc0(sizeof(*image) + g.bytes);
        image->bytes = g.bytes;
        pixels = image->pixels;
    } else {
        pixels = gl_pixel_scratch(s, g.bytes);
    }
    deadline = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 10000;
    while (received < total) {
        uint32_t chunk;
        int64_t t0 = s->profile ? get_clock() : 0;

        if (!s->rx_count && received >= prefix) {
            /* Pixel payload: read straight into its buffer, no rx bounce. */
            s->direct_dest = pixels + received - prefix;
            s->direct_cap = total - received;
            s->direct_got = 0;
            gl_wait_reply(s);
            s->direct_dest = NULL;
            received += s->direct_got;
            s->direct_got = 0;
            if (s->profile) {
                s->wait_ns += get_clock() - t0;
            }
            if (received >= total) {
                break;
            }
        } else if (!s->rx_count) {
            gl_wait_reply(s);
            if (s->profile) {
                s->wait_ns += get_clock() - t0;
            }
        }
        if (!s->rx_count) {
            if (qemu_clock_get_ms(QEMU_CLOCK_REALTIME) > deadline ||
                !gl_sync_capable(s)) {
                g_free(image);
                return GL_SURFACE_FAILED;
            }
            continue;
        }
        chunk = MIN(s->rx_count, total - received);
        if (received < prefix) {
            chunk = MIN(chunk, prefix - received);
            memcpy(head + received, s->rx, chunk);
        } else {
            memcpy(pixels + received - prefix, s->rx, chunk);
        }
        received += chunk;
        s->rx_count -= chunk;
        memmove(s->rx, s->rx + chunk, s->rx_count);
    }
    qemu_chr_fe_accept_input(&s->chr);
    status = gl_le32(head);
    if (prefix && status) {
        g_free(image);
        return GL_SURFACE_FAILED;
    }

    mapped = gl_surface_map(&g, true);
    if (mapped) {
        int64_t t1 = s->profile ? get_clock() : 0;

        trace_glbridge_surface_mapping(g.runs, 1);
        gl_surface_swizzle(&g, pixels, mapped, true, g.flags & 1);
        gl_surface_unmap(&g, mapped, true);
        if (s->profile) {
            s->swizzle_ns += get_clock() - t1;
        }
    } else {
        uint8_t **pages = gl_surface_map_pages(&g);
        int64_t t1 = s->profile ? get_clock() : 0;

        trace_glbridge_surface_mapping(g.runs, pages ? 2 : 0);
        if (pages) {
            gl_surface_swizzle_mapped(&g, pixels, NULL, pages, true,
                                      g.flags & 1);
            gl_surface_unmap_pages(&g, pages, DIV_ROUND_UP(g.extent, 4096),
                                   true);
        } else {
            /* Preserve padding when irregular runs need the bounce path. */
            native = gl_native_scratch(s, g.extent);
            if (!gl_surface_pages(&g, native, false)) {
                g_free(image);
                return GL_SURFACE_FAILED;
            }
            gl_surface_swizzle(&g, pixels, native, true, g.flags & 1);
            if (!gl_surface_pages(&g, native, true)) {
                g_free(image);
                return GL_SURFACE_FAILED;
            }
        }
        if (s->profile) {
            s->swizzle_ns += get_clock() - t1;
        }
    }
    if (flags & 8) {
        gl_image_store(s, key, image);
    }
    return GL_SURFACE_OK;
}

/*
 * Refresh a host texture from a native Screen pixmap. The guest shim used to
 * read the whole surface, compare it with its cached copy and re-upload it
 * on every draw that sampled it. Here the device reads and de-swizzles the
 * guest pages, compares against its own copy per (pid, image) key, and only
 * if the pixels changed (or flags bit 1 forces it) queues the guest-built
 * upload record header followed by the pixels.
 *
 * Extra command bytes: key_lo, key_hi, header_bytes, header[header_bytes].
 */
static uint32_t gl_surface_refresh(GLBridgeState *s, uint32_t length)
{
    const uint8_t *d = s->bulk_tx;
    GLImage *image, *cached;
    uint8_t *mapped;
    uint8_t *src;
    uint32_t header;
    uint64_t key;
    GLSurface g;

    if (length < GL_SURFACE_HEADER + 12) {
        return GL_SURFACE_REJECTED;
    }
    header = gl_le32(d + GL_SURFACE_HEADER + 8);
    if (!header || header > GL_REFRESH_MAX_HEADER || header % 4 ||
        !gl_surface_parse(d, length, GL_REFRESH_MAGIC, 12 + header, &g) ||
        g.bytes > GL_MAX_RECORD - header ||
        header + g.bytes > GL_TX_BYTES - s->tx_count) {
        return GL_SURFACE_REJECTED;
    }
    key = gl_le32(d + GL_SURFACE_HEADER) |
          (uint64_t)gl_le32(d + GL_SURFACE_HEADER + 4) << 32;
    gl_pixel_scratch(s, g.bytes);
    mapped = gl_surface_map(&g, false);
    cached = g_hash_table_lookup(s->images, &key);
    if (mapped) {
        src = mapped;
    } else {
        src = gl_native_scratch(s, g.extent);
        if (!gl_surface_pages(&g, src, false)) {
            return GL_SURFACE_REJECTED;
        }
    }
    if (!(g.flags & 2) && cached && gl_image_native_matches(cached, &g) &&
        !memcmp(src, cached->native, g.extent)) {
        if (mapped) {
            gl_surface_unmap(&g, mapped, false);
        }
        return GL_SURFACE_UNCHANGED;
    }
    gl_surface_swizzle(&g, s->scratch, src, false, false);
    if (!(g.flags & 2) && cached && cached->bytes == g.bytes &&
        !memcmp(cached->pixels, s->scratch, g.bytes)) {
        gl_image_take(s, key);
        gl_image_set_native(cached, &g, src);
        gl_image_store(s, key, cached);
        if (mapped) {
            gl_surface_unmap(&g, mapped, false);
        }
        return GL_SURFACE_UNCHANGED;
    }
    if (cached && cached->bytes == g.bytes) {
        image = gl_image_take(s, key);
        trace_glbridge_image_reuse(g.bytes);
    } else {
        image = g_malloc0(sizeof(*image) + g.bytes);
        image->bytes = g.bytes;
    }
    memcpy(image->pixels, s->scratch, g.bytes);
    gl_image_set_native(image, &g, src);
    if (mapped) {
        gl_surface_unmap(&g, mapped, false);
    }
    gl_queue(s, d + GL_SURFACE_HEADER + 12, header);
    gl_queue(s, image->pixels, g.bytes);
    gl_image_store(s, key, image);
    return GL_SURFACE_OK;
}

static uint64_t gl_read_register(void *opaque, hwaddr addr, unsigned size)
{
    GLBridgeState *s = opaque;

    if (addr == 0) {
        return 0x474c4252;
    }
    if (addr == 16) {
        /* Reserve a whole record without sleeping under QEMU's global lock. */
        bool held = s->locked || s->tx_count > GL_TX_BYTES - GL_MAX_RECORD;
        if (!held) {
            s->locked = true;
        }
        return held;
    }
    if (addr == 8) {
        gl_wait_reply(s);
        return MIN(s->rx_count, 4096);
    }
    if (addr == 24) {
        return GL_BULK_BYTES;
    }
    if (addr == 32) {
        gl_wait_reply(s);
        return s->rx_count;
    }
    if (addr == 44) {
        return s->surface_status;
    }
    if (addr >= 0x2000 && addr - 0x2000 + size <= s->rx_count) {
        uint64_t value = 0;
        for (unsigned i = 0; i < size; i++) {
            value |= (uint64_t)s->rx[addr - 0x2000 + i] << (8 * i);
        }
        return value;
    }
    if (addr == 12 && s->rx_count) {
        uint8_t value = s->rx[0];
        memmove(s->rx, s->rx + 1, --s->rx_count);
        qemu_chr_fe_accept_input(&s->chr);
        return value;
    }
    if (addr != 12 && addr < 0x2000) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "glbridge: invalid read at 0x%" HWADDR_PRIx "\n", addr);
    }
    return 0;
}

static void gl_write_register(void *opaque, hwaddr addr, uint64_t value,
                              unsigned size)
{
    GLBridgeState *s = opaque;

    if (addr == 16) {
        s->locked = false;
    } else if (((addr == 20 && value <= 4096) || addr == 36) &&
               value <= s->rx_count) {
        s->rx_count -= value;
        memmove(s->rx, s->rx + value, s->rx_count);
        qemu_chr_fe_accept_input(&s->chr);
    } else if (addr >= 0x1000 && addr + size <= 0x2000) {
        for (unsigned i = 0; i < size; i++) {
            s->tx[addr - 0x1000 + i] = value >> (i * 8);
        }
    } else if (addr == 4 && value <= sizeof(s->tx)) {
        gl_queue(s, s->tx, value);
    } else if (addr == 28 && value <= GL_BULK_BYTES) {
        gl_queue(s, s->bulk_tx, value);
    } else if (addr == 40 && value <= GL_BULK_BYTES) {
        s->surface_status = gl_surface_write(s, value);
        trace_glbridge_surface_write(value, s->surface_status);
    } else if (addr == 52) {
        /* Guest adapters report why they fell back to in-guest copies. */
        trace_glbridge_surface_fallback(value);
        if (s->fallback_reports < 256) {
            s->fallback_reports++;
            info_report("glbridge: guest surface fallback 0x%" PRIx64, value);
        }
    } else if (addr == 48 && value <= GL_BULK_BYTES) {
        s->surface_status = gl_surface_refresh(s, value);
        trace_glbridge_surface_refresh(value, s->surface_status);
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "glbridge: invalid write at 0x%" HWADDR_PRIx "\n", addr);
    }
}

/*
 * GLBRIDGE_PROFILE=1: every 10 host seconds, report host time spent inside
 * register accesses (these run on the vCPU thread and stall the guest).
 */
#define GL_PROFILE_SLOTS 17

static void gl_profile(GLBridgeState *s, hwaddr addr, bool write, int64_t start)
{
    int64_t now = get_clock();
    unsigned slot = addr < 64 ? addr / 4 : 16;

    s->profile_ns[write][slot] += now - start;
    s->profile_count[write][slot]++;
    if (!s->profile_start) {
        s->profile_start = now;
    } else if (now - s->profile_start >= 10 * NANOSECONDS_PER_SECOND) {
        GString *line = g_string_new("glbridge profile:");
        int64_t total = 0;

        for (unsigned w = 0; w < 2; w++) {
            for (unsigned i = 0; i < GL_PROFILE_SLOTS; i++) {
                if (s->profile_count[w][i]) {
                    total += s->profile_ns[w][i];
                    g_string_append_printf(line, " %c%u=%" PRIu64 "/%.1fms",
                                           w ? 'w' : 'r', i < 16 ? i * 4 : 0x1000,
                                           s->profile_count[w][i],
                                           s->profile_ns[w][i] / 1e6);
                }
            }
        }
        info_report("%s total=%.1f%% of %.1fs w40-wait=%.1fms w40-swizzle=%.1fms",
                    line->str, 100.0 * total / (now - s->profile_start),
                    (now - s->profile_start) / 1e9,
                    s->wait_ns / 1e6, s->swizzle_ns / 1e6);
        s->wait_ns = s->swizzle_ns = 0;
        g_string_free(line, true);
        memset(s->profile_ns, 0, sizeof(s->profile_ns));
        memset(s->profile_count, 0, sizeof(s->profile_count));
        s->profile_start = now;
    }
}

static uint64_t gl_read(void *opaque, hwaddr addr, unsigned size)
{
    GLBridgeState *s = opaque;
    int64_t start;
    uint64_t value;

    if (!s->profile) {
        return gl_read_register(opaque, addr, size);
    }
    start = get_clock();
    value = gl_read_register(opaque, addr, size);
    gl_profile(s, addr, false, start);
    return value;
}

static void gl_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    GLBridgeState *s = opaque;
    int64_t start;

    if (!s->profile) {
        gl_write_register(opaque, addr, value, size);
        return;
    }
    start = get_clock();
    gl_write_register(opaque, addr, value, size);
    gl_profile(s, addr, true, start);
}

static const MemoryRegionOps gl_ops = {
    .read = gl_read,
    .write = gl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void gl_reset(DeviceState *dev)
{
    GLBridgeState *s = GLBRIDGE(dev);

    s->locked = false;
    s->rx_count = 0;
    s->surface_status = GL_SURFACE_REJECTED;
    g_hash_table_remove_all(s->images);
    s->image_bytes = 0;
    gl_clear_tx(s);
}

static void gl_realize(DeviceState *dev, Error **errp)
{
    GLBridgeState *s = GLBRIDGE(dev);

    if (!qemu_chr_fe_backend_connected(&s->chr)) {
        error_setg(errp, "glbridge requires a chardev");
        return;
    }
    if (!memory_region_init_ram(&s->bulk, OBJECT(s), "glbridge-bulk",
                                2 * GL_BULK_BYTES, errp)) {
        return;
    }
    s->bulk_tx = memory_region_get_ram_ptr(&s->bulk);
    s->rx = s->bulk_tx + GL_BULK_BYTES;
    s->images = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                      g_free, gl_image_free);
    s->profile = g_strcmp0(g_getenv("GLBRIDGE_PROFILE"), "1") == 0;
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->bulk);
    qemu_chr_fe_set_handlers(&s->chr, gl_can_receive, gl_receive,
                            gl_event, NULL, s, NULL, true);
}

static void gl_unrealize(DeviceState *dev)
{
    GLBridgeState *s = GLBRIDGE(dev);

    gl_clear_tx(s);
    g_hash_table_destroy(s->images);
    g_free(s->scratch);
    g_free(s->native_scratch);
    qemu_chr_fe_deinit(&s->chr, false);
}

static void gl_init(Object *obj)
{
    GLBridgeState *s = GLBRIDGE(obj);

    memory_region_init_io(&s->mmio, obj, &gl_ops, s, "glbridge", 0x3000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);
}

static const Property gl_properties[] = {
    DEFINE_PROP_CHR("chardev", GLBridgeState, chr),
    DEFINE_PROP_UINT32("reply-wait-ms", GLBridgeState, reply_wait_ms, 250),
};

static void gl_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = gl_realize;
    dc->unrealize = gl_unrealize;
    dc->user_creatable = false;
    device_class_set_legacy_reset(dc, gl_reset);
    device_class_set_props(dc, gl_properties);
}

static const TypeInfo gl_info = {
    .name = TYPE_GLBRIDGE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GLBridgeState),
    .instance_init = gl_init,
    .class_init = gl_class_init,
};

static void gl_register_types(void)
{
    type_register_static(&gl_info);
}

type_init(gl_register_types)

void glbridge_create(Object *parent)
{
    Chardev *chr = qemu_chr_find("glbridge");
    DeviceState *dev;

    if (!chr) {
        return;
    }
    dev = qdev_new(TYPE_GLBRIDGE);
    object_property_add_child(parent, "glbridge", OBJECT(dev));
    qdev_prop_set_chr(dev, "chardev", chr);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0x5f000000);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 1, 0x5e000000);
}

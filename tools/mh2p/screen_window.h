/*
 * Opt-in P2865 Screen window adapter for the local Mesa bridge.
 * Native surface ABI verified against the shipped libraries, without an SDK.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef MH2P_SCREEN_WINDOW_H
#define MH2P_SCREEN_WINDOW_H

extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int dlclose(void *);
extern char *dlerror(void);
extern void *memset(void *, int, unsigned long);
extern int memcmp(const void *, const void *, unsigned long);
extern int strcmp(const char *, const char *);

struct ScreenSurface {
    u32 width, height, format, layout, pitch, memory, offset;
    u32 rest[11];
};

/*
 * Guest-physical page runs of a native surface ({phys_lo, phys_hi, length}
 * triples). Discovering them maps the whole surface, which costs a page-table
 * update and TLB invalidation per page in both directions, so they are cached
 * while `memory` (a retained handle on the same storage) keeps the pages from
 * being freed and reused.
 */
struct ScreenSurfaceRuns {
    u32 key, memory, offset, extent, count;
    u32 *runs;
};

struct ScreenImageState {
    struct {
        u32 texture;
        struct ScreenImage *image;
    } textures[256];
    struct {
        u32 framebuffer, texture;
    } fbos[128];
    u32 unit, bound[32][2], draw_fbo, read_fbo;
    i32 error;
};
static struct ScreenImageState *current_screen_image_state(void);

#include "screen_surface.h"

struct ScreenWindow {
    struct ScreenWindow *next;
    struct ScreenImage *pixmap_image;
    u32 id, traced_posts;
    int pbuffer;
    int shared_libraries;
    void *window, *screen, *rm, *nvutil;
    u32 device;
    i32 width, height, count;
    unsigned acquire_failures;
    void *buffers[8];
    struct ScreenSurfaceRuns dma_runs[9];
    unsigned dma_next;
    unsigned char *pixels, *rows;
    int (*get)(void *, int, int *);
    int (*getp)(void *, int, void **);
    int (*getbp)(void *, int, void **);
    int (*post)(void *, void *, int, const int *, int);
    int (*open)(u32 *, u32);
    void (*close)(u32);
    u32 (*size)(u32);
    int (*acquire)(struct ScreenSurface *, void *);
    void (*release)(struct ScreenSurface *, void *);
    void (*write)(struct ScreenSurface *, int, int, int, int, const void *);
    void (*read)(struct ScreenSurface *, int, int, int, int, void *);
};

static struct ScreenWindow *screen_window;
#define screen_gl_error (current_screen_image_state()->error)
static struct {
    void *screen, *rm, *nvutil;
    unsigned users;
} screen_libraries;

static struct ScreenWindow *native_window_find(void *surface)
{
    for (struct ScreenWindow *w = screen_window; w; w = w->next) {
        if (surface == w) {
            return w;
        }
    }
    return 0;
}

static void screen_log(const char *message)
{
    const char prefix[] = "glbridge-screen: ";

    write(2, prefix, sizeof(prefix) - 1);
    write(2, message, strlen(message));
    write(2, "\n", 1);
}

static void screen_log_value(const char *label, u32 value)
{
    char hex[11] = "0x00000000\n";
    for (unsigned i = 0; i < 8; i++)
        hex[9-i] = "0123456789abcdef"[(value >> (4*i)) & 15];
    write(2, "glbridge-screen: ", 17);
    write(2, label, strlen(label));
    write(2, hex, sizeof(hex));
}

static int screen_failure(const char *message, i32 error)
{
    egl_error = error;
    screen_log(message);
    return 0;
}

static void screen_gl_failure(const char *message, i32 error)
{
    if (!screen_gl_error) {
        screen_gl_error = error;
    }
    screen_log(message);
}

void glShaderBinary(i32 count, const u32 *shaders, u32 format,
                    const void *binary, i32 length)
{
    screen_gl_failure("native shader binaries are unsupported",
                      count < 0 || length < 0 ? 0x501 : 0x500);
}

void glProgramBinaryOES(u32 program, u32 format, const void *binary, i32 length)
{
    if (length < 0 || length > 4 * 1024 * 1024) {
        screen_gl_failure("invalid program binary size", 0x501);
        return;
    }
    if (format != 0x8e21) {
        screen_gl_failure("unsupported program binary format", 0x500);
        return;
    }
    if (!binary || !length) {
        screen_gl_failure("empty program binary", 0x501);
        return;
    }
    u32 header[] = {program, format, length}, error;
    rec(144, sizeof(header) + length);
    PART(header, sizeof(header));
    PART(binary, length);
    if (recv_all(&error, sizeof(error))) {
        screen_gl_failure("program binary transport failed", 0x502);
    } else if (error) {
        screen_gl_failure("original program upload failed", error);
    }
}

void glProgramBinary(u32 program, u32 format, const void *binary, i32 length)
{
    glProgramBinaryOES(program, format, binary, length);
}

static int screen_libraries_release(void)
{
    int ok = 1;
    if (screen_libraries.users && --screen_libraries.users) {
        return 1;
    }
    if (screen_libraries.nvutil && dlclose(screen_libraries.nvutil)) {
        ok = screen_failure("closing native surface utility failed", 0x3002);
        const char *error = dlerror();
        if (error) {
            screen_log(error);
        }
    }
    if (screen_libraries.rm && dlclose(screen_libraries.rm)) {
        ok = screen_failure("closing RM failed", 0x3002);
        const char *error = dlerror();
        if (error) {
            screen_log(error);
        }
    }
    if (screen_libraries.screen && dlclose(screen_libraries.screen)) {
        ok = screen_failure("closing Screen failed", 0x3002);
    }
    return ok;
}

static int screen_libraries_acquire(void)
{
    if (screen_libraries.users) {
        screen_libraries.users++;
        return 1;
    }
    screen_libraries.screen = dlopen("libscreen.so.1", 2);
    screen_libraries.rm = dlopen("libnvrm.so", 2);
    screen_libraries.nvutil = dlopen("libqnx-nvutil.so.1", 2);
    if (!screen_libraries.screen || !screen_libraries.rm ||
        !screen_libraries.nvutil) {
        screen_libraries_release();
        return screen_failure("loading native Screen/RM/surface utility failed",
                              0x3003);
    }
    screen_libraries.users = 1;
    return 1;
}

static void screen_surface_runs_clear(struct ScreenSurfaceRuns *cache,
                                      void (*memory_free)(u32))
{
    if (cache->memory && memory_free) {
        memory_free(cache->memory);
    }
    free(cache->runs);
    memset(cache, 0, sizeof(*cache));
}

static void screen_window_runs_clear(struct ScreenWindow *w)
{
    void (*memory_free)(u32) = 0;

    for (unsigned i = 0; i < 9; i++) {
        if (w->dma_runs[i].memory && !memory_free && screen_libraries.rm) {
            memory_free = dlsym(screen_libraries.rm, "NvRmMemHandleFree");
        }
        screen_surface_runs_clear(&w->dma_runs[i], memory_free);
    }
}

static int screen_release(struct ScreenWindow *w)
{
    int ok = 1;
    screen_window_runs_clear(w);
    if (w->device) {
        w->close(w->device);
    }
    free(w->pixels);
    free(w->rows);
    if (w->shared_libraries) {
        ok = screen_libraries_release();
    }
    free(w);
    return ok;
}

static int screen_fast_publish(const struct ScreenSurface *s)
{
    const char *publish = getenv("GLBRIDGE_FAST_PUBLISH");
    const char *fast = getenv("GLBRIDGE_FAST_SURFACES");
    const char *verify = getenv("GLBRIDGE_VERIFY_SURFACES");

    return publish && !strcmp(publish, "1") &&
           fast && !strcmp(fast, "1") &&
           (!verify || strcmp(verify, "1")) && screen_surface_extent(s);
}

static int screen_surface_transfer_rows(struct ScreenSurface *s, void *pixels,
                                   int writing, int inverted,
                                   void (*read_surface)(struct ScreenSurface *,
                                       int, int, int, int, void *),
                                   void (*write_surface)(struct ScreenSurface *,
                                       int, int, int, int, const void *))
{
    const char *fast = getenv("GLBRIDGE_FAST_SURFACES");
    if (!fast || strcmp(fast, "1") || !screen_surface_extent(s)) {
        if (inverted) {
            return screen_failure("inverted transfer requires mapped surface",
                                  0x3009);
        }
        if (writing) {
            write_surface(s, 0, 0, s->width, s->height, pixels);
        } else {
            read_surface(s, 0, 0, s->width, s->height, pixels);
        }
        return 1;
    }
    int (*map)(u32, u32, u32, u32, void **) =
        dlsym(screen_libraries.rm, "NvRmMemMap");
    void (*unmap)(u32, void *, u32) =
        dlsym(screen_libraries.rm, "NvRmMemUnmap");
    u32 (*size_of)(u32) = dlsym(screen_libraries.rm, "NvRmMemGetSize");
    if (!map || !unmap || !size_of) {
        return screen_failure("missing native surface mapping API", 0x3003);
    }
    unsigned size = size_of(s->memory);
    void *mapped = 0;
    if (s->offset > size || screen_surface_extent(s) > size - s->offset ||
        map(s->memory, 0, size, 3, &mapped) || !mapped) {
        return screen_failure("native surface mapping failed", 0x3003);
    }
    int ok = screen_surface_copy_rows(s, mapped, size, pixels, writing, inverted);
    __sync_synchronize();
    unmap(s->memory, mapped, size);
    if (!ok) {
        return screen_failure("native surface copy bounds failed", 0x3003);
    }
    const char *verify = getenv("GLBRIDGE_VERIFY_SURFACES");
    if (verify && !strcmp(verify, "1")) {
        unsigned bytes = s->width * s->height * 4;
        void *reference = malloc(bytes);
        if (!reference) {
            return screen_failure("surface reference allocation failed", 0x3003);
        }
        read_surface(s, 0, 0, s->width, s->height, reference);
        for (unsigned y = 0; y < s->height; y++) {
            unsigned row_bytes = s->width * 4;
            unsigned linear_y = inverted ? s->height - y - 1 : y;

            if (memcmp((unsigned char *)reference + y * row_bytes,
                       (unsigned char *)pixels + linear_y * row_bytes,
                       row_bytes)) {
                ok = 0;
                break;
            }
        }
        free(reference);
        if (!ok) {
            return screen_failure("original surface reference mismatch", 0x3002);
        }
        screen_log("original surface reference verified");
    }
    return 1;
}

extern int mem_offset64(const void *, int, unsigned, long long *, unsigned *);

#define SCREEN_SURFACE_WRITE 0x31414d44
#define SCREEN_SURFACE_REFRESH 0x31534452

/*
 * Describe the native surface's guest-physical pages so the bridge device can
 * access them directly (see glbridge.c): the common header, `extra` command
 * words, then {phys_lo, phys_hi, length} runs. Returns a malloc'd
 * descriptor, or 0 to use the in-guest copy path.
 */
/* Report the first few fallback reasons to the bridge trace (register 52). */
static void screen_surface_dma_fallback(u32 reason)
{
    static unsigned reports;

    if (bridge && reports < 16) {
        reports++;
        *(volatile u32 *)(bridge + 52) = reason;
    }
}

static u32 *screen_surface_dma_descriptor(const struct ScreenSurface *s,
                                          struct ScreenSurfaceRuns *cache,
                                          u32 magic, u32 flags,
                                          const u32 *extra, unsigned words,
                                          unsigned *length)
{
    const char *dma = getenv("GLBRIDGE_SURFACE_DMA");
    unsigned extent = screen_surface_extent(s);

    if (!bulk || !extent || (dma && !strcmp(dma, "0"))) {
        screen_surface_dma_fallback((magic & 0xff00) | 1 | (s->layout << 4));
        return 0;
    }
    unsigned max_runs = extent / 4096 + 2;
    unsigned header = 10 + words, runs = 0;
    u32 *d;

    if (cache && cache->runs && cache->offset == s->offset &&
        cache->extent == extent && cache->count <= max_runs) {
        runs = cache->count;
        if ((header + runs * 3) * 4 > bulk_size) {
            screen_surface_dma_fallback((magic & 0xff00) | 3 | (extent << 16));
            return 0;
        }
        d = malloc((header + runs * 3) * 4);
        if (!d) {
            return 0;
        }
        memcpy(d + header, cache->runs, runs * 12);
        goto described;
    }
    int (*map)(u32, u32, u32, u32, void **) =
        dlsym(screen_libraries.rm, "NvRmMemMap");
    void (*unmap)(u32, void *, u32) =
        dlsym(screen_libraries.rm, "NvRmMemUnmap");
    u32 (*size_of)(u32) = dlsym(screen_libraries.rm, "NvRmMemGetSize");
    if (!map || !unmap || !size_of) {
        screen_surface_dma_fallback((magic & 0xff00) | 2);
        return 0;
    }
    unsigned size = size_of(s->memory);
    void *mapped = 0;
    if (s->offset > size || extent > size - s->offset ||
        (header + max_runs * 3) * 4 > bulk_size) {
        screen_surface_dma_fallback((magic & 0xff00) | 3 | (extent << 16));
        return 0;
    }
    d = malloc((header + max_runs * 3) * 4);
    if (!d) {
        return 0;
    }
    if (map(s->memory, 0, size, 3, &mapped) || !mapped) {
        screen_surface_dma_fallback((magic & 0xff00) | 5);
        free(d);
        return 0;
    }
    unsigned char *at = (unsigned char *)mapped + s->offset;
    unsigned remaining = extent;
    while (remaining) {
        long long phys;
        unsigned contig = 0;

        if (mem_offset64(at, -1, remaining, &phys, &contig) || !contig) {
            screen_surface_dma_fallback((magic & 0xff00) | 6 |
                                        ((extent - remaining) << 16));
            break;
        }
        if (contig > remaining) {
            contig = remaining;
        }
        u32 *last = runs ? d + header + (runs - 1) * 3 : 0;
        if (last && ((unsigned long long)last[1] << 32 | last[0]) + last[2] ==
                        (unsigned long long)phys) {
            last[2] += contig;
        } else if (runs < max_runs) {
            last = d + header + runs * 3;
            last[0] = (u32)phys;
            last[1] = (u32)((unsigned long long)phys >> 32);
            last[2] = contig;
            runs++;
        } else {
            screen_surface_dma_fallback((magic & 0xff00) | 7);
            break;
        }
        at += contig;
        remaining -= contig;
    }
    unmap(s->memory, mapped, size);
    if (remaining) {
        free(d);
        return 0;
    }
    if (cache) {
        free(cache->runs);
        cache->runs = malloc(runs * 12);
        if (cache->runs) {
            memcpy(cache->runs, d + header, runs * 12);
            cache->offset = s->offset;
            cache->extent = extent;
            cache->count = runs;
        }
    }
described:
    d[0] = magic;
    d[1] = s->width * s->height * 4;
    d[2] = s->width;
    d[3] = s->height;
    d[4] = s->pitch;
    d[5] = s->layout;
    d[6] = s->rest[1];
    d[7] = flags;
    d[8] = extent;
    d[9] = runs;
    if (words) {
        memcpy(d + 10, extra, words * 4);
    }
    *length = (header + runs * 3) * 4;
    return d;
}

static int screen_surface_transfer(struct ScreenSurface *s, void *pixels,
                                   int writing,
                                   void (*read_surface)(struct ScreenSurface *,
                                       int, int, int, int, void *),
                                   void (*write_surface)(struct ScreenSurface *,
                                       int, int, int, int, const void *))
{
    return screen_surface_transfer_rows(s, pixels, writing, 0,
                                        read_surface, write_surface);
}

static void *native_window_create(void *window)
{
    struct ScreenWindow *w;
    int dimensions[2], format, usage;
    u32 device = 0;

    if (!window) {
        screen_failure("null native Screen window", 0x300b);
        return 0;
    }
    for (struct ScreenWindow *entry = screen_window; entry; entry = entry->next) {
        if (entry->window == window) {
            screen_failure("Screen window already has an EGL surface", 0x3003);
            return 0;
        }
    }
    w = malloc(sizeof(*w));
    if (!w) {
        screen_failure("window allocation failed", 0x3003);
        return 0;
    }
    memset(w, 0, sizeof(*w));
    w->window = window;
    if (!screen_libraries_acquire()) {
        goto fail;
    }
    w->screen = screen_libraries.screen;
    w->rm = screen_libraries.rm;
    w->nvutil = screen_libraries.nvutil;
    w->shared_libraries = 1;
#define SCREEN_SYMBOL(lib, field, name) do { \
    w->field = dlsym(w->lib, name); \
    if (!w->field) { \
        screen_failure("missing " name, 0x3003); \
        goto fail; \
    } \
} while (0)
    SCREEN_SYMBOL(screen, get, "screen_get_window_property_iv");
    SCREEN_SYMBOL(screen, getp, "screen_get_window_property_pv");
    SCREEN_SYMBOL(screen, getbp, "screen_get_buffer_property_pv");
    SCREEN_SYMBOL(screen, post, "screen_post_window");
    SCREEN_SYMBOL(rm, open, "NvRmOpen");
    SCREEN_SYMBOL(rm, close, "NvRmClose");
    SCREEN_SYMBOL(rm, size, "NvRmMemGetSize");
    SCREEN_SYMBOL(rm, write, "NvRmSurfaceWrite");
    SCREEN_SYMBOL(rm, read, "NvRmSurfaceRead");
    SCREEN_SYMBOL(nvutil, acquire, "qnxnv_acquire_image_surfaces");
    SCREEN_SYMBOL(nvutil, release, "qnxnv_release_image_surfaces");
#undef SCREEN_SYMBOL
    if (w->get(window, 5, dimensions) ||
        w->get(window, 14, &format) ||
        w->get(window, 48, &usage) || w->get(window, 4, &w->count)) {
        screen_failure("querying Screen window failed", 0x300b);
        goto fail;
    }
    w->width = dimensions[0];
    w->height = dimensions[1];
    if (w->width <= 0 || w->width > 2048 ||
        w->height <= 0 || w->height > 2048 ||
        format != 8 || !(usage & 32) || w->count < 1 || w->count > 8) {
        screen_log_value("window width=", w->width);
        screen_log_value("window height=", w->height);
        screen_log_value("window format=", format);
        screen_log_value("window usage=", usage);
        screen_log_value("window buffer count=", w->count);
        screen_failure("unsupported Screen format, buffers or window size",
                       0x3009);
        goto fail;
    }
    if (w->open(&device, 0)) {
        screen_failure("NvRmOpen failed", 0x3003);
        goto fail;
    }
    w->device = device;
    w->pixels = malloc((unsigned)w->width * w->height * 4);
    w->rows = malloc((unsigned)w->width * w->height * 4);
    if (!w->pixels || !w->rows) {
        screen_failure("readback allocation failed", 0x3003);
        goto fail;
    }
    w->next = screen_window;
    screen_window = w;
    return w;
fail:
    screen_release(w);
    return 0;
}

static int native_window_size(void *surface, i32 *width, i32 *height)
{
    struct ScreenWindow *w = native_window_find(surface);

    if (!w) {
        return screen_failure("invalid EGL window surface", 0x300d);
    }
    *width = w->width;
    *height = w->height;
    return 1;
}

/*
 * Find or allocate the run cache for a Screen window buffer. Buffers are
 * identified by their RM id; each cached entry retains its own handle so the
 * id and pages cannot be recycled while cached. Returns 0 (no caching) if the
 * RM id API is unavailable.
 */
static struct ScreenSurfaceRuns *screen_window_runs(struct ScreenWindow *w,
                                                    const struct ScreenSurface *s)
{
    unsigned slots = w->count < 8 ? w->count + 1 : 9;
    u32 key, memory = 0;

    if (!bulk || !screen_libraries.rm) {
        return 0;
    }
    u32 (*id_of)(u32) = dlsym(screen_libraries.rm, "NvRmMemGetId");
    if (!id_of || !(key = id_of(s->memory))) {
        return 0;
    }
    for (unsigned i = 0; i < slots; i++) {
        if (w->dma_runs[i].memory && w->dma_runs[i].key == key) {
            return &w->dma_runs[i];
        }
    }
    int (*from_id)(u32, u32 *) = dlsym(screen_libraries.rm,
                                       "NvRmMemHandleFromId");
    void (*memory_free)(u32) = dlsym(screen_libraries.rm, "NvRmMemHandleFree");
    if (!from_id || !memory_free) {
        return 0;
    }
    struct ScreenSurfaceRuns *cache = &w->dma_runs[w->dma_next++ % slots];
    screen_surface_runs_clear(cache, memory_free);
    if (from_id(key, &memory) || !memory) {
        return 0;
    }
    cache->key = key;
    cache->memory = memory;
    return cache;
}

static int screen_trace_publication(struct ScreenWindow *w,
                                    struct ScreenSurface *s, int bottom_up)
{
    u32 histogram[16] = {0}, nonzero[4] = {0}, hash = 2166136261u;
    u32 (*id_of)(u32) = dlsym(w->rm, "NvRmMemGetId");
    u32 memory_id;

    if (!id_of || !(memory_id = id_of(s->memory))) {
        return screen_failure("publication trace memory identity unavailable",
                              0x3002);
    }

    memset(w->rows, 0xa5, s->width * s->height * 4);
    w->read(s, 0, 0, s->width, s->height, w->rows);
    for (unsigned y = 0; y < s->height; y++) {
        const unsigned char *native = w->rows + y * s->width * 4;
        const unsigned char *mesa =
            w->pixels + (bottom_up ? s->height - y - 1 : y) * s->width * 4;

        if (memcmp(native, mesa, s->width * 4)) {
            return screen_failure("publication trace native/Mesa mismatch",
                                  0x3002);
        }
        for (unsigned x = 0; x < s->width; x++) {
            histogram[native[x * 4] & 15]++;
            for (unsigned channel = 0; channel < 4; channel++) {
                unsigned char value = native[x * 4 + channel];
                nonzero[channel] += value != 0;
                hash = (hash ^ value) * 16777619u;
            }
        }
    }
    screen_log("publication trace begin (original native readback verified)");
    screen_log_value("surface id=", w->id);
    screen_log_value("sample=", ++w->traced_posts);
    screen_log_value("width=", s->width);
    screen_log_value("height=", s->height);
    screen_log_value("memory=", s->memory);
    screen_log_value("memory id=", memory_id);
    screen_log_value("offset=", s->offset);
    screen_log_value("native rgba fnv1a=", hash);
    for (unsigned channel = 0; channel < 4; channel++) {
        screen_log_value("channel=", channel);
        screen_log_value("nonzero pixels=", nonzero[channel]);
    }
    for (unsigned value = 0; value < 16; value++) {
        screen_log_value("first-byte low nibble=", value);
        screen_log_value("pixels=", histogram[value]);
    }
    screen_log("publication trace end");
    return 1;
}

static int screen_preserve_gl_errors(void)
{
    i32 error;
    do {
        rec(105, 0);
        if (recv_all(&error, sizeof(error))) {
            return 0;
        }
        if (error && !screen_gl_error) {
            screen_gl_error = error;
        }
    } while (error);
    return 1;
}

static int native_window_swap(void *surface)
{
    struct ScreenWindow *w = native_window_find(surface);
    struct ScreenSurface s = {0};
    void *image = 0;
    void *buffer = 0;
    int buffer_count = 0, render_count = 0, result;
    i32 framebuffer = 0x8ca6, state[16], error;
    i32 request[6], rect[4];
    unsigned bytes;

    if (!w) {
        return screen_failure("invalid swap surface", 0x300d);
    }
    if (w->pbuffer) {
        return 1;
    }
    result = w->get(w->window, 4, &buffer_count);
    if (result || buffer_count != w->count) {
        screen_log_value("Screen buffer-count query result=", result);
        screen_log_value("Screen buffer count=", buffer_count);
        return screen_failure("acquiring Screen render buffer failed", 0x300b);
    }
    result = w->getp(w->window, 37, w->buffers);
    if (result) {
        screen_log_value("Screen render-buffer query result=", result);
        return screen_failure("acquiring Screen render buffer failed", 0x300b);
    }
    result = w->get(w->window, 53, &render_count);
    if (result || render_count < 1 || render_count > w->count) {
        screen_log_value("Screen render-count query result=", result);
        screen_log_value("Screen render count=", render_count);
        return screen_failure("acquiring Screen render buffer failed", 0x300b);
    }
    int image_error = 0;
    for (int i = 0; i < render_count; i++) {
        void *candidate = 0;
        if (!w->buffers[i]) {
            if (w->acquire_failures < 8) {
                screen_log_value("Screen render-buffer index=", i);
                screen_log_value("Screen render buffer missing=", i);
            }
            w->acquire_failures++;
            continue;
        }
        result = w->getbp(w->buffers[i], 113, &candidate);
        if (result || !candidate) {
            if (w->acquire_failures < 8) {
                screen_log_value("Screen render-buffer index=", i);
                screen_log_value("Screen buffer image query result=", result);
                screen_log_value("Screen buffer pointer=",
                                 (u32)(unsigned long)w->buffers[i]);
                screen_log_value("Screen native image pointer=",
                                 (u32)(unsigned long)candidate);
            }
            w->acquire_failures++;
            continue;
        }
        result = w->acquire(&s, candidate);
        if (result) {
            if (w->acquire_failures < 8) {
                screen_log_value("Screen render-buffer index=", i);
                screen_log_value("native image acquire result=", result);
                screen_log_value("Screen buffer pointer=",
                                 (u32)(unsigned long)w->buffers[i]);
                screen_log_value("Screen native image pointer=",
                                 (u32)(unsigned long)candidate);
            }
            w->acquire_failures++;
            image_error = 1;
            continue;
        }
        buffer = w->buffers[i];
        image = candidate;
        break;
    }
    if (!buffer) {
        return screen_failure(image_error ? "acquiring native image failed" :
                              "acquiring Screen render buffer failed",
                              image_error ? 0x3003 : 0x300b);
    }
    if (s.width != (u32)w->width || s.height != (u32)w->height ||
        s.format != 0x20105321 || (s.layout != 1 && s.layout != 3) ||
        s.pitch < s.width * 4 || s.pitch > 16384 ||
        s.offset > w->size(s.memory) ||
        s.height > (w->size(s.memory) - s.offset) / s.pitch) {
        w->release(&s, image);
        return screen_failure("unsupported native surface", 0x3009);
    }
    emit_iv(104, &framebuffer, 1);
    if (recv_all(state, sizeof(state)) || state[0]) {
        w->release(&s, image);
        return screen_failure("swap requires the default framebuffer", 0x3002);
    }
    bytes = s.width * s.height * 4;
    request[0] = request[1] = 0;
    request[2] = w->width;
    request[3] = w->height;
    request[4] = 0x1908;
    request[5] = 0x1401;
    if (!screen_preserve_gl_errors()) {
        w->release(&s, image);
        return screen_failure("GL error query transport failed", 0x3002);
    }
    unsigned dma_length = 0;
    int direct = 0;
    const char *trace = getenv("GLBRIDGE_TRACE_PUBLISH");
    int tracing = trace && !strcmp(trace, "1") && w->traced_posts < 32;
    u32 *dma = !tracing && screen_fast_publish(&s) ?
               screen_surface_dma_descriptor(&s, screen_window_runs(w, &s),
                                             SCREEN_SURFACE_WRITE, 1, 0, 0,
                                             &dma_length) : 0;
    emit_iv(120, request, 6);
    if (dma) {
        u32 status;

        memcpy(bulk, dma, dma_length);
        free(dma);
        __sync_synchronize();
        *(volatile u32 *)(bridge + 40) = dma_length;
        status = *(volatile u32 *)(bridge + 44);
        if (status == 2) {
            unlock_record();
            w->release(&s, image);
            return screen_failure("direct surface readback failed", 0x3002);
        }
        if (!status) {
            unlock_record();
            direct = 1;
        }
    }
    if (!direct && recv_all(w->pixels, bytes)) {
        w->release(&s, image);
        return screen_failure("Mesa readback transport failed", 0x3002);
    }
    rec(105, 0);
    if (recv_all(&error, sizeof(error)) || error) {
        w->release(&s, image);
        return screen_failure("Mesa readback failed", 0x3002);
    }
    if (direct) {
        /* The bridge device already wrote the flipped image into place. */
    } else if (screen_fast_publish(&s)) {
        /* Flip Mesa's bottom-up rows while writing the native image once. */
        if (!screen_surface_transfer_rows(&s, w->pixels, 1, 1,
                                          w->read, w->write)) {
            w->release(&s, image);
            return 0;
        }
    } else {
        for (unsigned y = 0; y < s.height; y++) {
            memcpy(w->rows + y * s.width * 4,
                   w->pixels + (s.height - y - 1) * s.width * 4, s.width * 4);
        }
        if (!screen_surface_transfer(&s, w->rows, 1, w->read, w->write) ||
            !screen_surface_transfer(&s, w->pixels, 0, w->read, w->write)) {
            w->release(&s, image);
            return 0;
        }
        if (memcmp(w->pixels, w->rows, bytes)) {
            w->release(&s, image);
            return screen_failure("native surface write/readback mismatch",
                                  0x3002);
        }
    }
    if (tracing && !screen_trace_publication(w, &s, screen_fast_publish(&s))) {
        w->release(&s, image);
        return 0;
    }
    w->release(&s, image);
    __sync_synchronize();
    rect[0] = rect[1] = 0;
    rect[2] = w->width;
    rect[3] = w->height;
    result = w->post(w->window, buffer, 1, rect, 0);
    return !result || screen_failure("screen_post_window failed", 0x3002);
}

static int native_window_destroy(void *surface)
{
    struct ScreenWindow **link = &screen_window;
    struct ScreenWindow *w = native_window_find(surface);

    if (!w) {
        return screen_failure("invalid destroy surface", 0x300d);
    }
    while (*link != w) {
        link = &(*link)->next;
    }
    *link = w->next;
    return screen_release(w);
}

static void native_register_api(u32 api)
{
    screen_failure("native NVIDIA client API registration is unsupported",
                   0x300c);
}

#endif

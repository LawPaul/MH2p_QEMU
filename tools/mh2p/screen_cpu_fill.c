/*
 * Opt-in P2865 diagnostic Screen blitter: fills and unscaled opaque copies.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
typedef unsigned u32;
extern void *malloc(unsigned);
extern void free(void *);
extern void *memset(void *, int, unsigned);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern char *dlerror(void);
extern int write(int, const void *, unsigned);
extern int snprintf(char *, unsigned, const char *, ...);
extern char *getenv(const char *);
extern int strcmp(const char *, const char *);
extern void *memcpy(void *, const void *, unsigned);

struct ScreenSurface {
    u32 width, height, format, layout, pitch, memory, offset;
    u32 rest[11];
};
#include "screen_surface.h"

struct context {
    void *rm, *glsi;
    u32 device;
    int (*open)(u32 *, u32);
    void (*close)(u32);
    int (*map)(u32, u32, u32, u32, void **);
    void (*unmap)(u32, void *, u32);
    u32 (*size)(u32);
    u32 (*id)(u32);
    void (*read)(struct ScreenSurface *, int, int, int, int, void *);
    void (*write)(struct ScreenSurface *, int, int, int, int, const void *);
    int (*count)(u32 *, u32);
    int (*acquire)(struct ScreenSurface *, void *);
    void (*release)(struct ScreenSurface *, void *);
};

struct handle {
    struct context *ctx;
    void *image, *mapped;
    struct ScreenSurface surface;
    unsigned bytes;
};

static void log_text(const char *text)
{
    unsigned length = 0;
    while (text[length]) {
        length++;
    }
    write(2, text, length);
}

static int failure(const char *message)
{
    log_text("mh2p-cpu: ");
    log_text(message);
    log_text("\n");
    return 48;
}

static int initialize(void **out)
{
    struct context *c = malloc(sizeof(*c));
    if (!c) {
        return failure("context allocation failed");
    }
    memset(c, 0, sizeof(*c));
    c->rm = dlopen("libnvrm.so", 2);
    c->glsi = dlopen("libnvidia-glsi.so.352.00", 2);
    if (!c->rm || !c->glsi) {
        failure(dlerror());
        free(c);
        return 48;
    }
#define RESOLVE(lib, field, name) \
    c->field = dlsym(c->lib, name); \
    if (!c->field) { failure(name); free(c); return 48; }
    RESOLVE(rm, open, "NvRmOpen");
    RESOLVE(rm, close, "NvRmClose");
    RESOLVE(rm, map, "NvRmMemMap");
    RESOLVE(rm, unmap, "NvRmMemUnmap");
    RESOLVE(rm, size, "NvRmMemGetSize");
    RESOLVE(rm, id, "NvRmMemGetId");
    RESOLVE(rm, read, "NvRmSurfaceRead");
    RESOLVE(rm, write, "NvRmSurfaceWrite");
    RESOLVE(glsi, count, "qnxnv_get_surface_count");
    RESOLVE(glsi, acquire, "qnxnv_acquire_image_surfaces");
    RESOLVE(glsi, release, "qnxnv_release_image_surfaces");
#undef RESOLVE
    if (c->open(&c->device, 0)) {
        free(c);
        return failure("NvRmOpen failed");
    }
    *out = c;
    log_text("mh2p-cpu: CPU fill context initialized\n");
    return 0;
}

static void finalize(struct context *c)
{
    c->close(c->device);
    free(c);
}

static void *wrap(struct context *c, u32 *image)
{
    struct handle *h;
    u32 count = 0;
    char line[200];
    if (!image || c->count(&count, image[2]) || count != 1) {
        failure("only single-plane images are supported");
        return 0;
    }
    h = malloc(sizeof(*h));
    if (!h) {
        failure("image allocation failed");
        return 0;
    }
    memset(h, 0, sizeof(*h));
    h->ctx = c;
    h->image = image;
    if (c->acquire(&h->surface, image)) {
        failure("surface acquisition failed");
        free(h);
        return 0;
    }
    struct ScreenSurface *s = &h->surface;
    snprintf(line, sizeof(line),
             "mh2p-cpu: image format=%u surface=%ux%u format=%x "
             "layout=%u pitch=%u memory=%x offset=%u\n",
             image[2], s->width, s->height, s->format, s->layout,
             s->pitch, s->memory, s->offset);
    log_text(line);
    if (!s->width || !s->height || s->width > 4096 || s->height > 4096 ||
        (s->layout != 1 && s->layout != 3) ||
        (s->format != 0x20105321 && s->format != 0x2010d121) ||
        s->pitch < s->width * 4 ||
        s->pitch > 65536 || s->offset > c->size(s->memory) ||
        s->height > (c->size(s->memory) - s->offset) / s->pitch) {
        failure("unsupported surface geometry, layout or format");
        c->release(s, image);
        free(h);
        return 0;
    }
    h->bytes = s->pitch * s->height;
    return h;
}

static int unwrap(struct context *c, struct handle *h)
{
    if (!h || h->ctx != c) {
        return failure("invalid image ownership");
    }
    c->release(&h->surface, h->image);
    free(h);
    return 0;
}

static int fill(struct context *c, struct handle *h, const int *rect, u32 color)
{
    char line[160];
    if (!h || h->ctx != c || !rect) {
        return failure("invalid fill arguments");
    }
    snprintf(line, sizeof(line), "mh2p-cpu: fill %d,%d %dx%d color=%08x\n",
             rect[0], rect[1], rect[2], rect[3], color);
    log_text(line);
    if (rect[0] < 0 || rect[1] < 0 || rect[2] < 0 || rect[3] < 0 ||
        (u32)rect[0] > h->surface.width ||
        (u32)rect[1] > h->surface.height ||
        (u32)rect[2] > h->surface.width - rect[0] ||
        (u32)rect[3] > h->surface.height - rect[1]) {
        return failure("fill rectangle outside surface");
    }
    unsigned count = (unsigned)rect[2] * rect[3];
    if (!count) {
        return 0;
    }
    const char *fast = getenv("GLBRIDGE_FAST_SURFACES");
    const char *verify = getenv("GLBRIDGE_VERIFY_SURFACES");
    const char *publish = getenv("GLBRIDGE_FAST_PUBLISH");
    unsigned size = c->size(h->surface.memory);
    void *mapped = 0;
    int use_mapping = fast && !strcmp(fast, "1") &&
        rect[0] == 0 && rect[1] == 0 &&
        (u32)rect[2] == h->surface.width &&
        (u32)rect[3] == h->surface.height &&
        screen_surface_extent(&h->surface);
    int fast_publish = use_mapping && publish && !strcmp(publish, "1") &&
        !(verify && !strcmp(verify, "1"));
    u32 *pixels = malloc((fast_publish ? h->surface.width : count) * 4);
    if (!pixels) {
        return failure("fill allocation failed");
    }
    if (use_mapping &&
        (h->surface.offset > size ||
         screen_surface_extent(&h->surface) > size - h->surface.offset ||
         c->map(h->surface.memory, 0, size, 3, &mapped) || !mapped)) {
        free(pixels);
        return failure("native fill mapping failed");
    }
    int result = 0;
    if (fast_publish) {
        for (unsigned i = 0; i < h->surface.width; i++) {
            pixels[i] = color;
        }
        if (!screen_surface_copy_strided(&h->surface, mapped, size, pixels,
                                          1, 0, 0)) {
            result = failure("native fill copy bounds failed");
        }
        __sync_synchronize();
        c->unmap(h->surface.memory, mapped, size);
        free(pixels);
        if (!result) {
            log_text("mh2p-cpu: mapped fill published without readback\n");
        }
        return result;
    }
    /* Diagnostic only: prove writes by changing every pixel before clearing. */
    for (unsigned pass = 0; pass < 2; pass++) {
        u32 expected = pass ? color : color ^ 0x00ffffff;
        for (unsigned i = 0; i < count; i++) {
            pixels[i] = expected;
        }
        if (use_mapping) {
            if (!screen_surface_copy(&h->surface, mapped, size, pixels, 1)) {
                result = failure("native fill copy bounds failed");
                break;
            }
            __sync_synchronize();
        } else {
            c->write(&h->surface, rect[0], rect[1], rect[2], rect[3], pixels);
        }
        memset(pixels, ~expected, count * 4);
        if (use_mapping && !(verify && !strcmp(verify, "1"))) {
            if (!screen_surface_copy(&h->surface, mapped, size, pixels, 0)) {
                result = failure("native fill readback bounds failed");
                break;
            }
        } else {
            c->read(&h->surface, rect[0], rect[1], rect[2], rect[3], pixels);
        }
        for (unsigned i = 0; i < count; i++) {
            if (pixels[i] != expected) {
                result = failure("native fill readback mismatch");
                break;
            }
        }
        if (result) {
            break;
        }
    }
    if (mapped) {
        c->unmap(h->surface.memory, mapped, size);
    }
    free(pixels);
    if (result) {
        return result;
    }
    __sync_synchronize();
    if (use_mapping) {
        log_text("mh2p-cpu: mapped fill and both readback passes verified\n");
    }
    log_text("mh2p-cpu: native fill readback verified\n");
    return 0;
}

/* The module ABI is source-first, unlike the public screen_blit() API. */
static int blit(struct context *c, struct handle *source,
                struct handle *destination, const int *parameters)
{
    char line[256];
    if (!c || !destination || !source || !parameters ||
        destination->ctx != c || source->ctx != c) {
        return failure("invalid copy arguments or ownership");
    }
    snprintf(line, sizeof(line),
             "mh2p-cpu: copy dst=%d,%d %dx%d src=%d,%d %dx%d "
             "operation=%d mode=%d alpha=%d quality=%d transform=%d,%d,%d\n",
             parameters[4], parameters[5], parameters[6], parameters[7],
             parameters[0], parameters[1], parameters[2], parameters[3],
             parameters[8], parameters[9], parameters[10], parameters[11],
             parameters[12], parameters[13], parameters[14]);
    log_text(line);
    if (parameters[8] != 0 || parameters[9] != 0 ||
        parameters[10] != 255 || parameters[11] != 0 ||
        parameters[12] != 0 || parameters[13] != 0 || parameters[14] != 0) {
        return failure("copy blending, filtering or transform unsupported");
    }
    if (parameters[0] < 0 || parameters[1] < 0 ||
        parameters[4] < 0 || parameters[5] < 0 ||
        parameters[2] <= 0 || parameters[3] <= 0 ||
        parameters[2] != parameters[6] || parameters[3] != parameters[7] ||
        (u32)parameters[0] > source->surface.width ||
        (u32)parameters[1] > source->surface.height ||
        (u32)parameters[2] > source->surface.width - parameters[0] ||
        (u32)parameters[3] > source->surface.height - parameters[1] ||
        (u32)parameters[4] > destination->surface.width ||
        (u32)parameters[5] > destination->surface.height ||
        (u32)parameters[6] > destination->surface.width - parameters[4] ||
        (u32)parameters[7] > destination->surface.height - parameters[5]) {
        return failure("copy rectangle outside surface or scaling unsupported");
    }
    unsigned bytes = (unsigned)parameters[2] * parameters[3] * 4;
    const char *fast = getenv("GLBRIDGE_FAST_SURFACES");
    const char *publish = getenv("GLBRIDGE_FAST_PUBLISH");
    const char *verify = getenv("GLBRIDGE_VERIFY_SURFACES");
    int fast_publish = fast && !strcmp(fast, "1") &&
        publish && !strcmp(publish, "1") &&
        !(verify && !strcmp(verify, "1"));
    unsigned char *pixels = malloc(bytes);
    unsigned char *readback = fast_publish ? 0 : malloc(bytes);
    if (!pixels || (!fast_publish && !readback)) {
        free(readback);
        free(pixels);
        return failure("copy allocation failed");
    }
    c->read(&source->surface, parameters[0], parameters[1],
            parameters[2], parameters[3], pixels);
    const char *trace = getenv("GLBRIDGE_TRACE_PUBLISH");
    if (trace && !strcmp(trace, "1")) {
        u32 hash = 2166136261u, nonzero = 0, region_eight = 0;
        if (!c->id || !c->id(source->surface.memory)) {
            free(readback);
            free(pixels);
            return failure("capture trace memory identity unavailable");
        }
        for (unsigned i = 0; i < bytes; i++) {
            hash = (hash ^ pixels[i]) * 16777619u;
            nonzero += pixels[i] != 0;
            if (!(i & 3)) {
                region_eight += (pixels[i] & 15) == 8;
            }
        }
        snprintf(line, sizeof(line),
                 "mh2p-cpu: capture source memory=%x id=%x offset=%u format=%x "
                 "bytes=%u nonzero_bytes=%u first_byte_region8=%u fnv1a=%08x\n",
                 source->surface.memory, c->id(source->surface.memory),
                 source->surface.offset,
                 source->surface.format, bytes, nonzero, region_eight, hash);
        log_text(line);
    }
    if (source->surface.format != destination->surface.format) {
        for (unsigned i = 0; i < bytes; i += 4) {
            unsigned char red = pixels[i];
            pixels[i] = pixels[i + 2];
            pixels[i + 2] = red;
        }
    }
    c->write(&destination->surface, parameters[4], parameters[5],
             parameters[6], parameters[7], pixels);
    if (fast_publish) {
        __sync_synchronize();
        free(pixels);
        log_text("mh2p-cpu: native copy published without readback\n");
        return 0;
    }
    memset(readback, 0, bytes);
    c->read(&destination->surface, parameters[4], parameters[5],
            parameters[6], parameters[7], readback);
    int result = 0;
    for (unsigned i = 0; i < bytes; i++) {
        if (pixels[i] != readback[i]) {
            result = failure("native copy readback mismatch");
            break;
        }
    }
    free(readback);
    free(pixels);
    return result;
}

static int finish(struct context *c)
{
    if (!c) {
        return failure("invalid completion context");
    }
    return 0;
}

static int prefx(void)
{
    return 0;
}

void win_blit_module_getfuncs(void **functions)
{
    functions[3] = initialize;
    functions[4] = finalize;
    functions[5] = wrap;
    functions[6] = unwrap;
    functions[7] = fill;
    functions[8] = blit;
    functions[9] = finish;
    functions[10] = finish;
    functions[11] = prefx;
}

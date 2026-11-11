/* Firmware-free native allocation lifetime and image transfer tests.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef uint32_t u32;
typedef int32_t i32;
static i32 egl_error;
static u32 g_id, screen_egl_mutex;
static struct { u32 egl_context; void *egl_draw; } record = {1, NULL};
#define current_record() (&record)
#define SCREEN_EGL_RETURN(type, expression) return (expression)
static unsigned opcode, action, part, commands[7], fault;
static unsigned acquired, released, retained, freed, opens, closes;
static unsigned reads, maps, unmaps;
static unsigned char native_pixels[16], host_pixels[16];
enum {
    BAD_PIXMAP = 1, BAD_BUFFER, BAD_ACQUIRE, BAD_FORMAT, BAD_BOUNDS,
    BAD_RETAIN, BAD_SYMBOL, BAD_LIBRARY, BAD_OPEN,
    BAD_TRANSPORT, BAD_HOST, BAD_READBACK,
};

static void rec(unsigned op, unsigned bytes)
{
    assert(op == 145 && (bytes == 20 || bytes == 36));
    opcode = op;
    part = 0;
}
static void emit_iv(unsigned op, const i32 *values, unsigned count)
{
    assert(op == 146 && count == 5 && values[0] == 4);
    opcode = op;
    commands[4]++;
}
static void record_part(const void *data, unsigned bytes)
{
    if (!part++) {
        assert(bytes == 20);
        const u32 *header = data;
        action = header[0];
        assert(action <= 6 && action != 4 && header[1] &&
               header[3] == 2 && header[4] == 2);
        commands[action]++;
    } else {
        assert((action == 0 || action == 2) && bytes == 16);
        memcpy(host_pixels, data, 16);
    }
}
#define PART(data, bytes) record_part(data, bytes)
static int recv_all(void *data, unsigned bytes)
{
    assert(bytes == (opcode == 146 ? 20 : 4));
    if (fault == BAD_TRANSPORT) return -1;
    *(u32 *)data = fault == BAD_HOST ? 0x502 : 0;
    if (opcode == 146) memcpy((char *)data + 4, host_pixels, 16);
    return 0;
}
static unsigned char *bulk;
static unsigned bulk_size;
static volatile unsigned char *bridge;
static unsigned unlocks, offsets_valid;
static void unlock_record(void) { unlocks++; }
static int mem_offset64(const void *address, int fd, unsigned length,
                        long long *physical, unsigned *contiguous)
{
    if (!offsets_valid) return -1;
    *physical = 0x80000000LL + ((const unsigned char *)address - native_pixels);
    *contiguous = length;
    return 0;
}
static unsigned record_header(void *r, u32 op, u32 total, u32 *h)
{
    u32 frame[] = {119, 4, getpid(), 140, 4, 0, op, total};
    memcpy(h, frame, sizeof(frame));
    return sizeof(frame);
}
#define dlopen mock_dlopen
#define dlclose mock_dlclose
#define dlsym mock_dlsym
#include "screen_window.h"
static struct ScreenImageState image_states[2];
static unsigned image_state_queries;
static struct ScreenImageState *current_screen_image_state(void)
{
    image_state_queries++;
    return &image_states[record.egl_context == 2];
}
static int screen_display(void *d)
{
    return d == (void *)1 || screen_failure("invalid test display", 0x3008);
}
static int screen_config(void *d, void *c)
{
    return screen_display(d) && c == (void *)1;
}
static void *screen_create_surface(void *d, void *c, void *w,
                                   const i32 *attributes, int pbuffer)
{
    struct ScreenWindow *surface = calloc(1, sizeof(*surface));
    assert(surface && pbuffer);
    surface->width = attributes[1];
    surface->height = attributes[3];
    surface->pbuffer = 1;
    surface->next = screen_window;
    screen_window = surface;
    return surface;
}
static int pixmap(void *p, int property, void **result)
{
    assert(p == (void *)10 && property == 37);
    *result = (void *)11;
    return fault == BAD_PIXMAP;
}
static int buffer(void *b, int property, void **result)
{
    assert(b == (void *)11 && property == 113);
    *result = (void *)12;
    return fault == BAD_BUFFER;
}
static int acquire(struct ScreenSurface *s, void *image)
{
    assert(image == (void *)12);
    if (fault == BAD_ACQUIRE) return -1;
    acquired++;
    *s = (struct ScreenSurface) {
        .width = 2, .height = 2, .pitch = 8, .memory = 1, .layout = 3,
        .format = fault == BAD_FORMAT ? 0 : 0x20105321,
        .offset = fault == BAD_BOUNDS ? 1 : 0,
    };
    return 0;
}
static void release(struct ScreenSurface *s, void *image)
{
    assert(s->memory == 1 && image == (void *)12);
    released++;
}
static int device_open(u32 *device, u32 index)
{
    *device = 8;
    if (fault == BAD_OPEN) return -1;
    opens++;
    return 0;
}
static void device_close(u32 device) { assert(device == 8); closes++; }
static u32 size(u32 memory) { assert(memory == 1 || memory == 2); return 16; }
static int surface_map(u32 memory, u32 offset, u32 bytes, u32 flags, void **out)
{
    assert(memory == 2 && !offset && bytes == 16 && flags == 3);
    maps++;
    *out = native_pixels;
    return 0;
}
static void surface_unmap(u32 memory, void *pointer, u32 bytes)
{
    assert(memory == 2 && pointer == native_pixels && bytes == 16);
    unmaps++;
}
static u32 id(u32 memory) { assert(memory == 1); return 42; }
static int retain(u32 identity, u32 *memory)
{
    assert(identity == 42);
    if (fault == BAD_RETAIN) return -1;
    *memory = 2;
    retained++;
    return 0;
}
static void memory_free(u32 memory) { assert(memory == 2); freed++; }
static void surface_read(struct ScreenSurface *s, int x, int y,
                          int w, int h, void *data)
{
    assert(s->memory == 2 && retained > freed && acquired == released);
    reads++;
    memcpy(data, native_pixels, 16);
    if (fault == BAD_READBACK) ((unsigned char *)data)[0] ^= 1;
}
static void surface_write(struct ScreenSurface *s, int x, int y,
                           int w, int h, const void *data)
{
    assert(s->memory == 2 && retained > freed);
    memcpy(native_pixels, data, 16);
}
static int window_dimensions[2] = {463, 477};
static int window_format = 8, window_usage = 32, window_count = 2;
static int window_get(void *window, int property, int *value)
{
    assert(window == (void *)15);
    switch (property) {
    case 5:
        value[0] = window_dimensions[0];
        value[1] = window_dimensions[1];
        return 0;
    case 14: *value = window_format; return 0;
    case 48: *value = window_usage; return 0;
    case 4: *value = window_count; return 0;
    default: assert(!"unexpected window property"); return -1;
    }
}
static int window_getp(void *window, int property, void **value)
{
    assert(!"window buffers should not be queried during surface creation");
    return -1;
}
static int window_post(void *window, void *buffer, int count,
                       const int *rect, int flags)
{
    assert(!"surface creation should not post a window");
    return -1;
}
#include "screen_image.h"

void *mock_dlopen(const char *name, int mode)
{
    if (!strcmp(name, "libscreen.so.1")) return (void *)1;
    if (!strcmp(name, "libnvrm.so")) return (void *)2;
    assert(!strcmp(name, "libqnx-nvutil.so.1"));
    return fault == BAD_LIBRARY ? NULL : (void *)3;
}
int mock_dlclose(void *handle)
{
    assert(handle == (void *)1 || handle == (void *)2 || handle == (void *)3);
    return 0;
}
void *mock_dlsym(void *handle, const char *name)
{
    if (!strcmp(name, "qnxnv_acquire_image_surfaces") ||
        !strcmp(name, "qnxnv_release_image_surfaces")) {
        assert(handle == (void *)3);
    }
    if (fault == BAD_SYMBOL) return NULL;
#define SYMBOL(symbol, function) if (!strcmp(name, symbol)) return (void *)function
    SYMBOL("screen_get_pixmap_property_pv", pixmap);
    SYMBOL("screen_get_window_property_iv", window_get);
    SYMBOL("screen_get_window_property_pv", window_getp);
    SYMBOL("screen_post_window", window_post);
    SYMBOL("screen_get_buffer_property_pv", buffer);
    SYMBOL("qnxnv_acquire_image_surfaces", acquire);
    SYMBOL("qnxnv_release_image_surfaces", release);
    SYMBOL("NvRmOpen", device_open);
    SYMBOL("NvRmClose", device_close);
    SYMBOL("NvRmMemGetSize", size);
    SYMBOL("NvRmMemMap", surface_map);
    SYMBOL("NvRmMemUnmap", surface_unmap);
    SYMBOL("NvRmMemGetId", id);
    SYMBOL("NvRmMemHandleFromId", retain);
    SYMBOL("NvRmMemHandleFree", memory_free);
    SYMBOL("NvRmSurfaceRead", surface_read);
    SYMBOL("NvRmSurfaceWrite", surface_write);
#undef SYMBOL
    assert(!"unexpected symbol");
    return NULL;
}

static void test_texture_lookup(void)
{
    struct ScreenImage first = {0}, last = {0}, other = {0};
    image_states[0].textures[0].texture = 17;
    image_states[0].textures[0].image = &first;
    image_states[0].textures[255].texture = 0xffffffff;
    image_states[0].textures[255].image = &last;
    image_states[1].textures[255].texture = 17;
    image_states[1].textures[255].image = &other;
    image_state_queries = 0;
    assert(!screen_texture_image(0) && !image_state_queries);
    assert(screen_texture_image(17) == &first);
    assert(screen_texture_image(0xffffffff) == &last);
    assert(!screen_texture_image(23));
    image_states[0].textures[0].texture = 0;
    assert(!screen_texture_image(17));
    record.egl_context = 2;
    assert(screen_texture_image(17) == &other);
    record.egl_context = 1;
    memset(image_states, 0, sizeof(image_states));
    for (unsigned i = 0; i < 256; i++) {
        image_states[0].textures[i].texture = 1000 + i;
        image_states[0].textures[i].image = i & 1 ? &first : &last;
    }
    for (unsigned i = 0; i < 256; i++) {
        assert(screen_texture_image(1000 + i) == (i & 1 ? &first : &last));
        image_states[0].textures[i].texture = 0;
        assert(!screen_texture_image(1000 + i));
    }
    memset(image_states, 0, sizeof(image_states));
}

int main(void)
{
    test_texture_lookup();
    void *d = (void *)1, *p = (void *)10;
    assert(!eglCreateImageKHR(NULL, NULL, 0x30b0, p, NULL));
    assert(!eglCreateImageKHR(d, d, 0x30b0, p, NULL));
    assert(!eglCreateImageKHR(d, NULL, 0x30b1, p, NULL));
    i32 attributes[] = {0x30d2, 2, 0x3038};
    assert(!eglCreateImageKHR(d, NULL, 0x30b0, p, attributes));
    for (fault = BAD_PIXMAP; fault <= BAD_OPEN; fault++) {
        assert(!eglCreateImageKHR(d, NULL, 0x30b0, p, NULL));
        assert(!screen_images_live() && !screen_image_api.users);
        assert(!screen_libraries.users && acquired == released);
        assert(retained == freed && opens == closes);
    }
    fault = 0;
    struct ScreenImage *image = eglCreateImageKHR(d, NULL, 0x30b0, p, NULL);
    assert(image && screen_images_live() && acquired == released);
    assert(retained == freed + 1 && image->surface.memory == 2);
    assert(!screen_image_bind(0xde1, image));
    native_texture_bind(0xde1, 21);
    assert(screen_image_bind(0xde1, image));
    assert(commands[0] == 1 && commands[1] == 1);
    record.egl_context = 2;
    assert(!screen_image_bound[0][0] && !screen_image_textures[0].texture);
    native_texture_bind(0xde1, 21);
    screen_gl_error = 0;
    assert(!screen_image_bind(0xde1, image) && screen_gl_error == 0x502);
    assert(commands[0] == 1 && commands[1] == 1 && image->refs == 1);
    record.egl_context = 1;
    assert(screen_image_bound[0][0] == 21 && screen_texture_image(21) == image);
    native_texture_bind(0x8d65, 22);
    glEGLImageTargetTexture2DOES(0x8d65, image);
    assert(image->refs == 2 && commands[0] == 1);
    assert(!screen_image_storage(0xde1));
    assert(screen_image_storage(0x806f));
    native_pixels[3] = 42;
    assert(screen_image_before_draw(1));
    assert(commands[2] == 1 && host_pixels[3] == 42);
    assert(screen_image_before_draw(1) && commands[2] == 1);
    screen_image_draw_fbo = screen_image_read_fbo = 9;
    assert(screen_image_attachment(0x8d40, 0x8ce0, 0xde1, 21, 0));
    assert(!screen_image_renderbuffer(0x8d40, 0x8ce0));
    assert(screen_image_renderbuffer(0x8d40, 0x8d00));
    native_pixels[5] = 99;
    assert(screen_image_before_draw(0) && image->dirty);
    assert(commands[2] == 2 && host_pixels[5] == 99);
    memset(host_pixels, 123, 16);
    assert(screen_image_before_draw(1) && commands[2] == 2);
    for (fault = BAD_TRANSPORT; fault <= BAD_READBACK; fault++) {
        screen_gl_error = 0;
        assert(!screen_image_finish() && image->dirty && screen_gl_error);
    }
    fault = 0;
    assert(screen_image_finish() && !image->dirty);
    assert(!memcmp(native_pixels, host_pixels, 16));
    unsigned old_reads = reads;
    setenv("GLBRIDGE_FAST_PUBLISH", "1", 1);
    setenv("GLBRIDGE_FAST_SURFACES", "1", 1);
    image->surface.layout = 1;
    for (unsigned i = 0; i < 16; i++) {
        host_pixels[i] = i * 7;
    }
    image->dirty = 1;
    assert(screen_image_finish() && !image->dirty);
    assert(!memcmp(native_pixels, host_pixels, 16));
    assert(!memcmp(image->cached, host_pixels, 16));
    assert(reads == old_reads && maps == 1 && unmaps == maps);
    setenv("GLBRIDGE_VERIFY_SURFACES", "1", 1);
    image->dirty = 1;
    fault = BAD_READBACK;
    assert(!screen_image_finish() && image->dirty);
    fault = 0;
    assert(screen_image_finish() && !image->dirty);
    assert(!memcmp(native_pixels, host_pixels, 16));
    assert(!memcmp(image->cached, host_pixels, 16));
    assert(reads == old_reads + 3 && maps == unmaps);
    unsetenv("GLBRIDGE_VERIFY_SURFACES");

    /* Device-side pixmap refresh: compare and upload without guest copies. */
    static u32 bulk_window[256], registers[0x3000 / 4];
    bulk = (unsigned char *)bulk_window;
    bulk_size = sizeof(bulk_window);
    bridge = (unsigned char *)registers;
    offsets_valid = 1;
    unsigned old_commands = commands[2], old_unlocks = unlocks;
    unsigned old_maps = maps;
    old_reads = reads;
    image->resync = 1;
    opcode = 145;
    registers[11] = 0;
    assert(screen_image_refresh(image) == 1 && !image->resync);
    assert(registers[12] == (26 + 3) * 4);
    assert(bulk_window[0] == 0x31534452 && bulk_window[1] == 16 &&
           bulk_window[5] == 1 && bulk_window[7] == 2 &&
           bulk_window[8] == 16 && bulk_window[9] == 1);
    assert(bulk_window[10] == (u32)getpid() && bulk_window[11] == image->id &&
           bulk_window[12] == 52);
    assert(bulk_window[13] == 119 && bulk_window[19] == 145 &&
           bulk_window[20] == 20 + 16);
    assert(bulk_window[21] == 2 && bulk_window[22] == image->id &&
           bulk_window[24] == 2 && bulk_window[25] == 2);
    assert(bulk_window[26] == 0x80000000 && bulk_window[27] == 0 &&
           bulk_window[28] == 16);
    assert(commands[2] == old_commands && unlocks == old_unlocks);
    assert(reads == old_reads && maps == unmaps);
    assert(maps == old_maps + 1);
    registers[11] = 3;
    assert(screen_image_refresh(image) == 1 && bulk_window[7] == 0);
    assert(unlocks == old_unlocks + 1 && commands[2] == old_commands);
    /* The pixmap's physical runs are cached for its retained storage. */
    assert(maps == old_maps + 1 && bulk_window[26] == 0x80000000);
    registers[11] = 1;
    native_pixels[1] ^= 0xff;
    assert(screen_image_refresh(image) == 1 && unlocks == old_unlocks + 2);
    assert(commands[2] == old_commands + 1 && image->resync);
    registers[11] = 2;
    assert(screen_image_refresh(image) == 1 && commands[2] == old_commands + 1);

    /* Device-side render readback placement records the host contents. */
    unsigned old_syncs = commands[4];
    old_unlocks = unlocks;
    old_maps = maps;
    registers[11] = 0;
    image->dirty = 1;
    image->resync = 1;
    assert(screen_image_finish() && !image->dirty && !image->resync);
    assert(maps == old_maps);
    assert(image->stale && commands[4] == old_syncs + 1);
    assert(unlocks == old_unlocks + 1 && registers[10] == (10 + 2 + 3) * 4);
    assert(bulk_window[0] == 0x31414d44 && bulk_window[7] == 12 &&
           bulk_window[10] == (u32)getpid() && bulk_window[11] == image->id &&
           bulk_window[12] == 0x80000000 && bulk_window[14] == 16);
    /* A stale guest comparison copy forces the fallback upload. */
    registers[11] = 1;
    old_commands = commands[2];
    assert(screen_image_refresh(image) == 1 && !image->stale);
    assert(commands[2] == old_commands + 1);
    /* A rejected placement falls back to the guest readback copy. */
    image->dirty = 1;
    for (unsigned i = 0; i < 16; i++) {
        host_pixels[i] = 200 - i;
    }
    assert(screen_image_finish() && !image->dirty && image->resync);
    assert(!memcmp(native_pixels, host_pixels, 16));
    assert(unlocks == old_unlocks + 2);
    registers[11] = 2;
    image->dirty = 1;
    assert(!screen_image_finish() && image->dirty);
    assert(unlocks == old_unlocks + 3);
    registers[11] = 1;
    assert(screen_image_finish() && !image->dirty);
    bulk = NULL;
    bridge = NULL;
    offsets_valid = 0;
    unsetenv("GLBRIDGE_FAST_PUBLISH");
    unsetenv("GLBRIDGE_FAST_SURFACES");
    image->surface.layout = 3;
    assert(eglDestroyImageKHR(d, image) && image->refs == 2);
    assert(!eglDestroyImageKHR(d, image));
    assert(!screen_image_bind(0xde1, image));
    u32 textures[] = {21, 22}, framebuffer = 9;
    assert(!screen_image_delete(0, 2, textures, 0));
    assert(screen_image_delete(2, 1, &framebuffer, 1));
    assert(!screen_image_draw_fbo && !screen_image_read_fbo);
    assert(screen_image_delete(0, 1, textures, 1) && image->refs == 1);
    image->dirty = 1;
    host_pixels[1] = 17;
    assert(screen_image_delete(0, 1, textures + 1, 1));
    assert(!screen_images_live() && native_pixels[1] == 17);
    assert(commands[3] == 1 && retained == freed && opens == closes);
    assert(!screen_image_api.users && !screen_libraries.users);
    image = eglCreateImageKHR(d, NULL, 0x30b0, p, NULL);
    assert(image);
    native_texture_bind(0xde1, 21);
    assert(screen_image_bind(0xde1, image));
    assert(screen_image_delete(0, 1, textures, 1));
    record.egl_context = 0;
    assert(!eglDestroyImageKHR(d, image) && screen_images_live());
    record.egl_context = 1;
    assert(eglDestroyImageKHR(d, image));
    assert(retained == freed && opens == closes);
    struct ScreenWindow *drawable = screen_create_pixmap_surface(d, d, p, NULL);
    assert(drawable && drawable->pixmap_image);
    assert(!screen_create_pixmap_surface(d, d, p, NULL));
    record.egl_draw = drawable;
    assert(screen_image_surface_current(drawable) && commands[5] == 1);
    assert(screen_image_before_draw(0) && drawable->pixmap_image->dirty);
    memset(host_pixels, 87, 16);
    assert(screen_image_finish() && commands[6] == 1 && native_pixels[0] == 87);
    assert(screen_image_surface_unbind(drawable));
    record.egl_draw = NULL;
    record.egl_context = 0;
    assert(screen_image_surface_release(drawable));
    assert(native_window_destroy(drawable));
    assert(!screen_images_live() && retained == freed && opens == closes);
    screen_image_unit = 7;
    screen_image_reset();
    assert(!screen_image_unit && !screen_image_bound[0][0]);
    setenv("GLBRIDGE_W", "1920", 1);
    setenv("GLBRIDGE_H", "720", 1);
    struct ScreenWindow *window = native_window_create((void *)15);
    assert(window && window->width == 463 && window->height == 477);
    assert(native_window_destroy(window) && opens == closes);
    window_dimensions[0] = 1920;
    window_dimensions[1] = 720;
    window = native_window_create((void *)15);
    assert(window && window->width == 1920 && window->height == 720);
    assert(native_window_destroy(window) && opens == closes);
    window_dimensions[0] = 2048;
    window_dimensions[1] = 2048;
    window = native_window_create((void *)15);
    assert(window && window->width == 2048 && window->height == 2048);
    assert(native_window_destroy(window) && opens == closes);
    for (unsigned axis = 0; axis < 2; axis++) {
        for (unsigned invalid = 0; invalid < 2; invalid++) {
            window_dimensions[0] = 463;
            window_dimensions[1] = 477;
            window_dimensions[axis] = invalid ? 2049 : 0;
            assert(!native_window_create((void *)15));
            assert(!screen_libraries.users && opens == closes);
        }
    }
    window_dimensions[0] = 463;
    window_dimensions[1] = 477;
    window_format = 0;
    assert(!native_window_create((void *)15));
    window_format = 8;
    window_usage = 0;
    assert(!native_window_create((void *)15));
    window_usage = 32;
    window_count = 9;
    assert(!native_window_create((void *)15));
    assert(!screen_libraries.users && opens == closes);
    return 0;
}

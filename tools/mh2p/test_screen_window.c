/*
 * Firmware-free tests of the opt-in Screen publication boundary.
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
static unsigned opcode, fault, posts, acquired, released, closed, libraries_closed;
static unsigned program_bytes;
static u32 program_header[3];
static unsigned char native_pixels[16];
static int buffers[2], image;
static int linear_surface;
static unsigned reads, writes;
static unsigned readback_started, stale_error_returned;

enum {
    BAD_COUNT = 1, BAD_BUFFERS, BAD_IMAGE, BAD_ACQUIRE, BAD_FORMAT,
    BAD_BOUNDS, BAD_FBO, BAD_TRANSPORT, BAD_GL, BAD_READBACK, BAD_POST,
    BAD_PROGRAM, BAD_PROGRAM_TRANSPORT,
    BAD_MAP, BAD_MAP_SYMBOL, BAD_MAP_SIZE, BAD_STALE_GL, BAD_ACQUIRE_FIRST,
};

static int surface_map(u32, u32, u32, u32, void **);
static void surface_unmap(u32, void *, u32);
static u32 allocation_size(u32);
static u32 memory_id(u32);
static int memory_retain(u32, u32 *);
static void memory_free(u32);
static u32 memory_key = 0x77;
static unsigned retained, freed_handles;
static unsigned rm_symbol_lookups, size_queries;
static unsigned long last_allocation;

static void *tracked_malloc(unsigned long bytes)
{
    last_allocation = bytes;
    return malloc(bytes);
}

static void emit_iv(unsigned op, const i32 *values, unsigned count)
{
    opcode = op;
    if (op == 104) {
        readback_started = 0;
        stale_error_returned = 0;
    } else if (op == 120) {
        readback_started = 1;
    }
}

static void rec(unsigned op, unsigned bytes)
{
    opcode = op;
    program_bytes = 0;
}

static void record_part(const void *data, unsigned bytes)
{
    assert(opcode == 144);
    if (!program_bytes) {
        assert(bytes == sizeof(program_header));
        memcpy(program_header, data, bytes);
    } else {
        assert(program_bytes == 12 && bytes == program_header[2]);
    }
    program_bytes += bytes;
}

#define PART(data, bytes) record_part(data, bytes)

static int recv_all(void *data, unsigned bytes)
{
    memset(data, 0, bytes);
    if (opcode == 144) {
        assert(bytes == 4 && program_bytes == 12 + program_header[2]);
        if (fault == BAD_PROGRAM_TRANSPORT) {
            return -1;
        }
        *(u32 *)data = fault == BAD_PROGRAM ? 0x502 : 0;
    } else if (opcode == 104) {
        assert(bytes == 64);
        *(i32 *)data = fault == BAD_FBO;
    } else if (opcode == 105) {
        assert(bytes == 4);
        *(i32 *)data = 0;
        if (fault == BAD_GL && readback_started) {
            *(i32 *)data = 0x502;
        } else if (fault == BAD_STALE_GL && !readback_started &&
                   !stale_error_returned) {
            *(i32 *)data = 0x502;
            stale_error_returned = 1;
        }
    } else {
        assert(opcode == 120 && bytes == 16);
        if (fault == BAD_TRANSPORT) {
            return -1;
        }
        for (unsigned i = 0; i < bytes; i++) {
            ((unsigned char *)data)[i] = i + 1;
        }
    }
    return 0;
}

static unsigned char *bulk;
static unsigned bulk_size;
static volatile unsigned char *bridge;
static unsigned unlocked;

static void unlock_record(void)
{
    unlocked++;
}

static int mem_offset64(const void *address, int fd, unsigned length,
                        long long *physical, unsigned *contiguous)
{
    unsigned offset = (const unsigned char *)address - native_pixels;

    assert(fd == -1 && offset < sizeof(native_pixels));
    *physical = 0x180000000LL + offset;
    /* Split the mapping to exercise run coalescing. */
    *contiguous = length > 4 ? 4 : length;
    return 0;
}
#define dlclose mock_dlclose
#define dlsym mock_dlsym
#define malloc tracked_malloc
#include "screen_window.h"
static struct ScreenImageState image_state;
static struct ScreenImageState *current_screen_image_state(void)
{
    return &image_state;
}
#undef dlclose
#undef dlsym
#undef malloc

void *mock_dlsym(void *handle, const char *name)
{
    if (handle == buffers + 1) {
        rm_symbol_lookups++;
        if (fault == BAD_MAP_SYMBOL) {
            return NULL;
        }
        if (!strcmp(name, "NvRmMemMap")) return surface_map;
        if (!strcmp(name, "NvRmMemUnmap")) return surface_unmap;
        if (!strcmp(name, "NvRmMemGetSize")) return allocation_size;
        if (!strcmp(name, "NvRmMemGetId")) return memory_id;
        if (!strcmp(name, "NvRmMemHandleFromId")) return memory_retain;
        if (!strcmp(name, "NvRmMemHandleFree")) return memory_free;
        assert(0);
    }
    assert(handle == buffers);
    assert(!strcmp(name, "screen_get_window_property_iv"));
    return NULL;
}

int mock_dlclose(void *handle)
{
    assert(handle == buffers || handle == buffers + 1 || handle == &image);
    libraries_closed++;
    return 0;
}

static int get(void *window, int property, int *value)
{
    assert(window == buffers && (property == 4 || property == 53));
    *value = fault == BAD_COUNT ? 9 : 2;
    return 0;
}

static int getp(void *window, int property, void **value)
{
    assert(window == buffers && property == 37);
    value[0] = &buffers[posts % 2];
    value[1] = &buffers[(posts + 1) % 2];
    return fault == BAD_BUFFERS;
}

static int getbp(void *buffer, int property, void **value)
{
    assert((buffer == &buffers[0] || buffer == &buffers[1]) &&
           property == 113);
    *value = buffer;
    return fault == BAD_IMAGE;
}

static int acquire(struct ScreenSurface *surface, void *native_image)
{
    assert(native_image == &buffers[0] || native_image == &buffers[1]);
    if (fault == BAD_ACQUIRE ||
        (fault == BAD_ACQUIRE_FIRST &&
         native_image == &buffers[posts % 2])) {
        return -1;
    }
    acquired++;
    *surface = (struct ScreenSurface) {
        .width = 2, .height = 2, .pitch = 8, .memory = 1,
        .format = fault == BAD_FORMAT ? 0 : 0x20105321,
        .layout = linear_surface ? 1 : 3,
        .offset = fault == BAD_BOUNDS ? 17 : 0,
    };
    return 0;
}

static void release(struct ScreenSurface *surface, void *native_image)
{
    assert((native_image == &buffers[0] || native_image == &buffers[1]) &&
           released < acquired);
    released++;
}

static u32 allocation_size(u32 memory)
{
    size_queries++;
    assert(memory == 1);
    return fault == BAD_MAP_SIZE ? 15 : 16;
}

static unsigned mapped_count, unmapped_count;

static u32 memory_id(u32 memory)
{
    assert(memory == 1);
    return memory_key;
}

static int memory_retain(u32 id, u32 *memory)
{
    assert(id == memory_key && id);
    *memory = 0x100 + ++retained;
    return 0;
}

static void memory_free(u32 memory)
{
    assert(memory > 0x100 && memory <= 0x100 + retained);
    freed_handles++;
}

static int surface_map(u32 memory, u32 offset, u32 bytes, u32 flags, void **out)
{
    assert(memory == 1 && !offset && bytes == 16 && flags == 3);
    if (fault == BAD_MAP) {
        return -1;
    }
    mapped_count++;
    *out = native_pixels;
    return 0;
}

static void surface_unmap(u32 memory, void *pointer, u32 bytes)
{
    assert(memory == 1 && pointer == native_pixels && bytes == 16);
    unmapped_count++;
}

static void surface_write(struct ScreenSurface *surface, int x, int y,
                          int width, int height, const void *data)
{
    assert(x == 0 && y == 0 && width == 2 && height == 2);
    writes++;
    memcpy(native_pixels, data, sizeof(native_pixels));
}

static void surface_read(struct ScreenSurface *surface, int x, int y,
                         int width, int height, void *data)
{
    reads++;
    memcpy(data, native_pixels, sizeof(native_pixels));
    if (fault == BAD_READBACK) {
        ((unsigned char *)data)[0] ^= 1;
    }
}

static int post(void *window, void *buffer, int count, const int *rect,
                int flags)
{
    assert(window == buffers &&
           buffer == &buffers[(posts + (fault == BAD_ACQUIRE_FIRST)) % 2]);
    assert(count == 1 && !flags && !rect[0] && !rect[1]);
    assert(rect[2] == 2 && rect[3] == 2 && acquired == released);
    if (fault == BAD_POST) {
        return -1;
    }
    posts++;
    return 0;
}

static void close_device(u32 device)
{
    assert(device == 1);
    closed++;
}

static void fast_surface_tests(void)
{
    struct ScreenSurface s = {
        .width = 2, .height = 2, .pitch = 8, .memory = 1,
        .format = 0x20105321, .layout = 1,
    };
    unsigned char pixels[16], output[16];
    memset(pixels, 0x39, sizeof(pixels));
    screen_libraries.rm = buffers + 1;
    fault = 0;
    mapped_count = unmapped_count = 0;
    setenv("GLBRIDGE_FAST_SURFACES", "1", 1);
    assert(screen_surface_transfer(&s, pixels, 1, surface_read, surface_write));
    assert(!memcmp(native_pixels, pixels, sizeof(pixels)));
    setenv("GLBRIDGE_VERIFY_SURFACES", "1", 1);
    assert(screen_surface_transfer(&s, output, 0, surface_read, surface_write));
    assert(!memcmp(output, pixels, sizeof(pixels)));
    fault = BAD_READBACK;
    assert(!screen_surface_transfer(&s, output, 0, surface_read, surface_write));
    for (fault = BAD_MAP; fault <= BAD_MAP_SIZE; fault++) {
        assert(!screen_surface_transfer(&s, output, 0, surface_read,
                                        surface_write));
    }
    assert(mapped_count == unmapped_count && mapped_count == 3);
    fault = 0;
    s.layout = 2;
    assert(screen_surface_transfer(&s, output, 0, surface_read, surface_write));
    assert(mapped_count == 3);
    unsetenv("GLBRIDGE_FAST_SURFACES");
    unsetenv("GLBRIDGE_VERIFY_SURFACES");
    s.layout = 1;
    assert(!screen_surface_transfer_rows(&s, pixels, 1, 1,
                                         surface_read, surface_write));
    for (unsigned i = 0; i < sizeof(pixels); i++) {
        pixels[i] = i;
    }
    setenv("GLBRIDGE_FAST_SURFACES", "1", 1);
    setenv("GLBRIDGE_VERIFY_SURFACES", "1", 1);
    assert(screen_surface_transfer_rows(&s, pixels, 1, 1,
                                        surface_read, surface_write));
    assert(!memcmp(native_pixels, pixels + 8, 8));
    assert(!memcmp(native_pixels + 8, pixels, 8));
    assert(screen_surface_transfer_rows(&s, output, 0, 1,
                                        surface_read, surface_write));
    assert(!memcmp(output, pixels, sizeof(pixels)));
    assert(mapped_count == unmapped_count);
    unsetenv("GLBRIDGE_FAST_SURFACES");
    unsetenv("GLBRIDGE_VERIFY_SURFACES");
    screen_libraries.rm = NULL;
}

int main(void)
{
    struct ScreenWindow *w = calloc(1, sizeof(*w));
    i32 width, height;
    static const unsigned char expected[] = {
        9, 10, 11, 12, 13, 14, 15, 16, 1, 2, 3, 4, 5, 6, 7, 8,
    };

    assert(w);
    assert(!native_window_create(NULL));
    *w = (struct ScreenWindow) {
        .window = buffers, .width = 2, .height = 2, .count = 2,
        .rm = buffers + 1,
        .device = 1, .pixels = malloc(16), .rows = malloc(16),
        .get = get, .getp = getp, .getbp = getbp, .post = post,
        .acquire = acquire, .release = release, .size = allocation_size,
        .write = surface_write, .read = surface_read, .close = close_device,
    };
    assert(w->pixels && w->rows);
    screen_window = w;
    assert(!native_window_create(buffers));
    assert(native_window_size(w, &width, &height));
    assert(width == 2 && height == 2);
    assert(!native_window_size(NULL, &width, &height));
    assert(!native_window_swap(NULL));
    assert(!native_window_swap((void *)1) && posts == 0);
    for (unsigned i = 0; i < 2; i++) {
        assert(native_window_swap(w));
        assert(!memcmp(native_pixels, expected, sizeof(expected)));
        assert(posts == i + 1 && acquired == released);
    }
    for (fault = BAD_COUNT; fault <= BAD_POST; fault++) {
        egl_error = 0x3000;
        assert(!native_window_swap(w));
        assert(egl_error != 0x3000 && posts == 2 && acquired == released);
    }
    fault = 0;
    assert(native_window_swap(w) && posts == 3);
    unsigned old_reads = reads, old_writes = writes, old_maps = mapped_count;
    setenv("GLBRIDGE_FAST_PUBLISH", "1", 1);
    setenv("GLBRIDGE_FAST_SURFACES", "1", 1);
    screen_libraries.rm = buffers + 1;
    linear_surface = 1;
    assert(native_window_swap(w) && posts == 4);
    assert(!memcmp(native_pixels, expected, sizeof(expected)));
    assert(reads == old_reads && writes == old_writes);
    assert(mapped_count == old_maps + 1 && mapped_count == unmapped_count);
    setenv("GLBRIDGE_VERIFY_SURFACES", "1", 1);
    assert(native_window_swap(w) && posts == 5);
    assert(!memcmp(native_pixels, expected, sizeof(expected)));
    assert(reads == old_reads + 2 && mapped_count == old_maps + 3);
    fault = BAD_READBACK;
    assert(!native_window_swap(w) && posts == 5);
    assert(acquired == released && mapped_count == unmapped_count);
    unsetenv("GLBRIDGE_VERIFY_SURFACES");
    for (fault = BAD_MAP; fault <= BAD_MAP_SIZE; fault++) {
        assert(!native_window_swap(w) && posts == 5);
        assert(acquired == released && mapped_count == unmapped_count);
    }
    fault = 0;
    linear_surface = 0;
    old_reads = reads;
    assert(native_window_swap(w) && posts == 6);
    assert(reads == old_reads + 1);

    /* Direct device placement of the readback into the native surface. */
    static u32 bulk_window[1024], registers[0x3000 / 4];
    bulk = (unsigned char *)bulk_window;
    bulk_size = sizeof(bulk_window);
    bridge = (unsigned char *)registers;
    linear_surface = 1;
    memset(native_pixels, 0x5a, sizeof(native_pixels));
    old_reads = reads;
    old_writes = writes;
    old_maps = mapped_count;
    registers[11] = 0;
    assert(native_window_swap(w) && posts == 7 && unlocked == 1);
    assert(registers[10] == 52);
    assert(bulk_window[0] == 0x31414d44 && bulk_window[1] == 16 &&
           bulk_window[2] == 2 && bulk_window[3] == 2 &&
           bulk_window[4] == 8 && bulk_window[5] == 1 &&
           bulk_window[7] == 1 && bulk_window[8] == 16 &&
           bulk_window[9] == 1);
    assert(bulk_window[10] == 0x80000000 && bulk_window[11] == 1 &&
           bulk_window[12] == 16);
    for (unsigned i = 0; i < sizeof(native_pixels); i++) {
        assert(native_pixels[i] == 0x5a);
    }
    assert(reads == old_reads && writes == old_writes);
    assert(mapped_count == old_maps + 1 && mapped_count == unmapped_count);
    assert(retained == 1 && !freed_handles);
    struct ScreenSurface cached_surface = {
        .width = 2, .height = 2, .pitch = 8, .layout = 1, .memory = 1,
        .format = 0x20105321,
    };
    unsigned cached_length, old_lookups = rm_symbol_lookups;
    unsigned old_size_queries = size_queries;
    u32 *cached_descriptor = screen_surface_dma_descriptor(
        &cached_surface, &w->dma_runs[0], SCREEN_SURFACE_WRITE, 1,
        NULL, 0, &cached_length);
    assert(cached_descriptor && cached_length == registers[10]);
    assert(last_allocation == cached_length);
    assert(!memcmp(cached_descriptor, bulk_window, cached_length));
    assert(rm_symbol_lookups == old_lookups && size_queries == old_size_queries);
    free(cached_descriptor);
    bulk_size = cached_length - 4;
    assert(!screen_surface_dma_descriptor(
        &cached_surface, &w->dma_runs[0], SCREEN_SURFACE_WRITE, 1,
        NULL, 0, &cached_length));
    bulk_size = sizeof(bulk_window);
    cached_surface.offset = 4;
    assert(!screen_surface_dma_descriptor(
        &cached_surface, &w->dma_runs[0], SCREEN_SURFACE_WRITE, 1,
        NULL, 0, &cached_length));
    cached_surface.offset = 0;
    cached_surface.height = 3;
    assert(!screen_surface_dma_descriptor(
        &cached_surface, &w->dma_runs[0], SCREEN_SURFACE_WRITE, 1,
        NULL, 0, &cached_length));
    assert(rm_symbol_lookups == old_lookups + 6 &&
           size_queries == old_size_queries + 2);
    registers[11] = 1;
    assert(native_window_swap(w) && posts == 8 && unlocked == 1);
    assert(!memcmp(native_pixels, expected, sizeof(expected)));
    /* Cached runs: only the rejected-path in-guest copy maps the surface. */
    assert(mapped_count == old_maps + 2 && retained == 1);
    registers[11] = 2;
    egl_error = 0x3000;
    old_lookups = rm_symbol_lookups;
    assert(!native_window_swap(w) && posts == 8 && unlocked == 2);
    assert(egl_error == 0x3002 && acquired == released);
    assert(rm_symbol_lookups == old_lookups + 1);
    registers[10] = 0;
    setenv("GLBRIDGE_SURFACE_DMA", "0", 1);
    assert(native_window_swap(w) && posts == 9 && !registers[10]);
    unsetenv("GLBRIDGE_SURFACE_DMA");
    assert(mapped_count == unmapped_count);
    /* A different buffer gets its own retained entry; unknown ids skip it. */
    registers[11] = 0;
    old_maps = mapped_count;
    memory_key = 0x78;
    assert(native_window_swap(w) && posts == 10 && retained == 2);
    assert(mapped_count == old_maps + 1 && bulk_window[10] == 0x80000000);
    memory_key = 0x77;
    assert(native_window_swap(w) && posts == 11 && retained == 2);
    assert(mapped_count == old_maps + 1);
    memory_key = 0;
    assert(native_window_swap(w) && posts == 12 && retained == 2);
    assert(mapped_count == old_maps + 2 && mapped_count == unmapped_count);
    memory_key = 0x77;
    assert(!freed_handles);
    setenv("GLBRIDGE_TRACE_PUBLISH", "0", 1);
    registers[10] = 0;
    assert(native_window_swap(w) && posts == 13 && registers[10]);
    assert(!w->traced_posts);
    setenv("GLBRIDGE_TRACE_PUBLISH", "1", 1);
    registers[10] = 0;
    old_reads = reads;
    assert(native_window_swap(w) && posts == 14 && !registers[10]);
    assert(w->traced_posts == 1 && reads == old_reads + 1);
    assert(!memcmp(native_pixels, expected, sizeof(expected)));
    struct ScreenSurface traced = {
        .width = 2, .height = 2, .memory = 1,
    };
    fault = BAD_READBACK;
    assert(!screen_trace_publication(w, &traced, 1));
    assert(w->traced_posts == 1);
    fault = 0;
    w->traced_posts = 32;
    assert(native_window_swap(w) && posts == 15 && registers[10]);
    assert(w->traced_posts == 32);
    bulk = NULL;
    bulk_size = 0;
    bridge = NULL;
    unsetenv("GLBRIDGE_FAST_PUBLISH");
    unsetenv("GLBRIDGE_FAST_SURFACES");
    w->traced_posts = 0;
    assert(native_window_swap(w) && posts == 16 && w->traced_posts == 1);
    unsetenv("GLBRIDGE_TRACE_PUBLISH");
    fault = BAD_STALE_GL;
    screen_gl_error = 0;
    assert(native_window_swap(w) && posts == 17 && acquired == released);
    assert(screen_gl_error == 0x502);
    fault = BAD_ACQUIRE_FIRST;
    screen_gl_error = 0;
    assert(native_window_swap(w) && posts == 18 && acquired == released);
    fault = 0;
    screen_libraries.rm = NULL;
    assert(!native_window_destroy(NULL));
    assert(!native_window_destroy((void *)1) && screen_window == w);
    struct ScreenWindow *second = calloc(1, sizeof(*second));
    assert(second);
    second->width = 4;
    second->height = 3;
    second->next = w;
    w->screen = second->screen = buffers;
    w->rm = second->rm = buffers + 1;
    w->nvutil = second->nvutil = &image;
    w->shared_libraries = second->shared_libraries = 1;
    screen_libraries.screen = w->screen;
    screen_libraries.rm = w->rm;
    screen_libraries.nvutil = w->nvutil;
    screen_libraries.users = 2;
    screen_window = second;
    assert(native_window_size(w, &width, &height) && width == 2 && height == 2);
    assert(native_window_destroy(w) && screen_window == second && closed == 1);
    assert(freed_handles == retained);
    assert(libraries_closed == 0 && screen_libraries.users == 1);
    assert(!native_window_create(buffers));
    assert(screen_window == second && screen_libraries.users == 1 &&
           libraries_closed == 0);
    assert(native_window_size(second, &width, &height) &&
           width == 4 && height == 3);
    assert(native_window_destroy(second) && !screen_window);
    assert(libraries_closed == 3 && screen_libraries.users == 0);
    w = calloc(1, sizeof(*w));
    second = calloc(1, sizeof(*second));
    assert(w && second);
    w->screen = second->screen = buffers;
    w->rm = second->rm = buffers + 1;
    w->nvutil = second->nvutil = &image;
    w->shared_libraries = second->shared_libraries = 1;
    second->next = w;
    screen_window = second;
    screen_libraries.users = 2;
    assert(native_window_destroy(second) && screen_window == w);
    assert(libraries_closed == 3 && screen_libraries.users == 1);
    assert(native_window_destroy(w) && !screen_window);
    assert(libraries_closed == 6 && screen_libraries.users == 0);
    native_register_api(0);
    for (unsigned negative = 0; negative < 2; negative++) {
        screen_gl_error = 0;
        egl_error = 0x3000;
        glShaderBinary(negative ? -1 : 2, NULL, 0x8e21, NULL, 0);
        assert(screen_gl_error == (negative ? 0x501 : 0x500));
        assert(egl_error == 0x3000);
        screen_gl_error = 0;
        glProgramBinaryOES(1, 0x8e21, NULL, negative ? -1 : 0);
        assert(screen_gl_error == 0x501);
        assert(egl_error == 0x3000);
    }
    u32 binary[] = {10, 0, 0, 0};
    for (unsigned i = 0; i < 3; i++) {
        fault = i == 1 ? BAD_PROGRAM : i == 2 ? BAD_PROGRAM_TRANSPORT : 0;
        screen_gl_error = 0;
        egl_error = 0x3000;
        glProgramBinary(42, 0x8e21, binary, sizeof(binary));
        assert(screen_gl_error == (i ? 0x502 : 0) && egl_error == 0x3000);
        assert(program_header[0] == 42 && program_header[1] == 0x8e21 &&
               program_header[2] == sizeof(binary));
    }
    fast_surface_tests();
    return 0;
}

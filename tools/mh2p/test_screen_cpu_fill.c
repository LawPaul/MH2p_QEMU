/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static unsigned allocated_size, allocations, fail_allocation;
static void *probe_malloc(unsigned size)
{
    allocated_size = size;
    if (++allocations == fail_allocation) {
        return NULL;
    }
    return malloc(size);
}
static void *probe_memset(void *p, int value, unsigned size)
{ return memset(p, value, size); }
static void *probe_memcpy(void *a, const void *b, unsigned size)
{ return memcpy(a, b, size); }
#define malloc probe_malloc
#define memset probe_memset
#define memcpy probe_memcpy
#include "screen_cpu_fill.c"
#undef malloc
#undef memset
#undef memcpy

static unsigned char storage[65536], linear[65536];
static unsigned maps, unmaps, reads, writes;
static int bad_map, bad_read;

static int map_surface(u32 memory, u32 offset, u32 size, u32 flags, void **out)
{
    assert(memory == 1 && offset == 0 && size == sizeof(storage) && flags == 3);
    maps++;
    *out = bad_map ? NULL : storage;
    return bad_map;
}

static void unmap_surface(u32 memory, void *mapped, u32 size)
{
    assert(memory == 1 && mapped == storage && size == sizeof(storage));
    unmaps++;
}

static u32 surface_size(u32 memory)
{
    assert(memory == 1);
    return sizeof(storage);
}

static void read_surface(struct ScreenSurface *s, int x, int y, int w, int h,
                         void *pixels)
{
    reads++;
    assert(screen_surface_copy(s, storage, sizeof(storage), linear, 0));
    for (int row = 0; row < h; row++) {
        memcpy((char *)pixels + row * w * 4,
               linear + ((y + row) * s->width + x) * 4, w * 4);
    }
    if (bad_read) {
        ((unsigned char *)pixels)[0] ^= 1;
    }
}

static void write_surface(struct ScreenSurface *s, int x, int y, int w, int h,
                          const void *pixels)
{
    writes++;
    assert(screen_surface_copy(s, storage, sizeof(storage), linear, 0));
    for (int row = 0; row < h; row++) {
        memcpy(linear + ((y + row) * s->width + x) * 4,
               (const char *)pixels + row * w * 4, w * 4);
    }
    assert(screen_surface_copy(s, storage, sizeof(storage), linear, 1));
}

static unsigned char copy_storage[2][8 * 8 * 4];
static unsigned copy_reads, copy_writes;
static unsigned wrapped_format, released;

static u32 copy_id(u32 memory)
{
    assert(memory < 2);
    return 0x100 + memory;
}

static int image_count(u32 *count, u32 format)
{
    assert(format == 8);
    *count = 1;
    return 0;
}

static int acquire_image(struct ScreenSurface *surface, void *image)
{
    assert(image);
    *surface = (struct ScreenSurface) {
        .width = 8, .height = 8, .format = wrapped_format,
        .layout = 1, .pitch = 32, .memory = 1,
    };
    return 0;
}

static void release_image(struct ScreenSurface *surface, void *image)
{
    assert(surface && image);
    released++;
}

static void copy_read(struct ScreenSurface *s, int x, int y, int w, int h,
                      void *pixels)
{
    assert(s->memory < 2);
    copy_reads++;
    for (int row = 0; row < h; row++) {
        memcpy((char *)pixels + row * w * 4,
               copy_storage[s->memory] + ((y + row) * s->width + x) * 4,
               w * 4);
    }
    if (bad_read && s->memory == 1) {
        ((unsigned char *)pixels)[0] ^= 1;
    }
}

static void copy_write(struct ScreenSurface *s, int x, int y, int w, int h,
                       const void *pixels)
{
    assert(s->memory < 2);
    copy_writes++;
    for (int row = 0; row < h; row++) {
        memcpy(copy_storage[s->memory] + ((y + row) * s->width + x) * 4,
               (const char *)pixels + row * w * 4, w * 4);
    }
}

static void check_capture_copy(unsigned publish)
{
    setenv("GLBRIDGE_FAST_SURFACES", publish ? "1" : "0", 1);
    setenv("GLBRIDGE_FAST_PUBLISH", publish ? "1" : "0", 1);
    unsetenv("GLBRIDGE_VERIFY_SURFACES");
    released = 0;
    void *functions[12] = {0};
    win_blit_module_getfuncs(functions);
    int (*copy)(struct context *, struct handle *, struct handle *,
                const int *) = functions[8];
    struct context c = {
        .read = copy_read, .write = copy_write, .count = image_count,
        .acquire = acquire_image, .release = release_image, .size = surface_size,
        .id = copy_id,
    };
    u32 image[3] = {0, 0, 8};
    wrapped_format = 0x2010d121;
    struct handle *wrapped = wrap(&c, image);
    assert(wrapped && wrapped->surface.format == wrapped_format);
    assert(unwrap(&c, wrapped) == 0 && released == 1);
    wrapped_format = 0;
    assert(!wrap(&c, image) && released == 2);
    struct handle source = {
        .ctx = &c,
        .surface = {.width = 8, .height = 8, .format = 0x20105321},
    };
    struct handle destination = source;
    destination.surface.memory = 1;
    int parameters[15] = {2, 1, 4, 3, 1, 2, 4, 3, 0, 0, 255};
    const int full[15] = {0, 0, 8, 8, 0, 0, 8, 8, 0, 0, 255};
    const unsigned formats[] = {0x20105321, 0x2010d121};
    for (unsigned from = 0; from < 2; from++) {
        for (unsigned to = 0; to < 2; to++) {
            source.surface.format = formats[from];
            destination.surface.format = formats[to];
            memset(copy_storage[1], 0xa5, sizeof(copy_storage[1]));
            for (unsigned i = 0; i < sizeof(copy_storage[0]); i++) {
                copy_storage[0][i] = i * 73 + i / 17;
            }
            unsigned char original[sizeof(copy_storage[0])];
            memcpy(original, copy_storage[0], sizeof(original));
            unsigned reads_before = copy_reads, writes_before = copy_writes;
            assert(copy(&c, &source, &destination, parameters) == 0);
            assert(copy_reads == reads_before + (publish ? 1 : 2));
            assert(copy_writes == writes_before + 1);
            assert(!memcmp(copy_storage[0], original, sizeof(original)));
            for (unsigned y = 0; y < 8; y++) {
                for (unsigned x = 0; x < 8; x++) {
                    for (unsigned channel = 0; channel < 4; channel++) {
                        unsigned char expected = 0xa5;
                        if (x >= 1 && x < 5 && y >= 2 && y < 5) {
                            unsigned sc = from != to && !(channel & 1) ?
                                2 - channel : channel;
                            expected = copy_storage[0][
                                ((y - 1) * 8 + x + 1) * 4 + sc];
                        }
                        assert(copy_storage[1][(y * 8 + x) * 4 + channel] ==
                               expected);
                    }
                }
            }
            assert(copy(&c, &source, &destination, full) == 0);
            assert(!memcmp(copy_storage[0], original, sizeof(original)));
            for (unsigned i = 0; i < sizeof(copy_storage[1]); i++) {
                unsigned sc = from != to && !(i & 1) ? i ^ 2 : i;
                assert(copy_storage[1][i] == copy_storage[0][sc]);
            }
        }
        setenv("GLBRIDGE_TRACE_PUBLISH", "1", 1);
        assert(copy(&c, &source, &destination, full) == 0);
        c.id = NULL;
        unsigned writes_before = copy_writes;
        assert(copy(&c, &source, &destination, full) == 48);
        assert(copy_writes == writes_before);
        c.id = copy_id;
        unsetenv("GLBRIDGE_TRACE_PUBLISH");
    }
    source.surface.format = destination.surface.format;
    memcpy(copy_storage[1], copy_storage[0], sizeof(copy_storage[0]));
    assert(copy(&c, &source, &source, parameters) == 0);
    for (unsigned y = 0; y < 3; y++) {
        assert(!memcmp(copy_storage[0] + ((y + 2) * 8 + 1) * 4,
                       copy_storage[1] + ((y + 1) * 8 + 2) * 4, 16));
    }
    for (unsigned field = 8; field < 15; field++) {
        int saved = parameters[field];
        parameters[field] = field == 10 ? 254 : 1;
        unsigned writes_before = copy_writes, reads_before = copy_reads;
        assert(copy(&c, &source, &destination, parameters) == 48);
        assert(copy_writes == writes_before && copy_reads == reads_before);
        parameters[field] = saved;
    }
    for (unsigned field = 0; field < 8; field++) {
        int saved = parameters[field];
        parameters[field] = -1;
        assert(copy(&c, &source, &destination, parameters) == 48);
        parameters[field] = 9;
        assert(copy(&c, &source, &destination, parameters) == 48);
        parameters[field] = saved;
    }
    assert(copy(&c, NULL, &destination, parameters) == 48);
    destination.ctx = NULL;
    assert(copy(&c, &source, &destination, parameters) == 48);
    destination.ctx = &c;
    for (unsigned flags = 0; flags < 8; flags++) {
        setenv("GLBRIDGE_FAST_SURFACES", flags & 1 ? "1" : "0", 1);
        setenv("GLBRIDGE_FAST_PUBLISH", flags & 2 ? "1" : "0", 1);
        setenv("GLBRIDGE_VERIFY_SURFACES", flags & 4 ? "1" : "0", 1);
        unsigned reads_before = copy_reads, writes_before = copy_writes;
        unsigned allocations_before = allocations;
        assert(copy(&c, &source, &destination, full) == 0);
        assert(copy_reads == reads_before + (flags == 3 ? 1 : 2));
        assert(allocations == allocations_before + (flags == 3 ? 1 : 2));
        assert(copy_writes == writes_before + 1);
        assert(!memcmp(copy_storage[0], copy_storage[1],
                       sizeof(copy_storage[0])));
    }
    bad_read = 1;
    assert(copy(&c, &source, &destination, parameters) == 48);
    bad_read = 0;
    for (unsigned fast = 0; fast < 2; fast++) {
        setenv("GLBRIDGE_FAST_SURFACES", "1", 1);
        setenv("GLBRIDGE_FAST_PUBLISH", "1", 1);
        setenv("GLBRIDGE_VERIFY_SURFACES", fast ? "0" : "1", 1);
        for (unsigned fail = 1; fail <= (fast ? 1 : 2); fail++) {
            fail_allocation = allocations + fail;
            unsigned reads_before = copy_reads, writes_before = copy_writes;
            assert(copy(&c, &source, &destination, full) == 48);
            assert(copy_reads == reads_before && copy_writes == writes_before);
            fail_allocation = 0;
        }
    }
}

int main(void)
{
    struct context c = {
        .map = map_surface, .unmap = unmap_surface, .size = surface_size,
        .read = read_surface, .write = write_surface,
    };
    struct handle h = {
        .ctx = &c,
        .surface = {
            .width = 33, .height = 17, .format = 0x20105321,
            .layout = 3, .pitch = 192, .memory = 1, .offset = 128,
            .rest = {0xdb, 2},
        },
    };
    int full[] = {0, 0, 33, 17}, partial[] = {3, 5, 7, 9};
    setenv("GLBRIDGE_FAST_SURFACES", "1", 1);
    memset(storage, 0xa5, sizeof(storage));
    assert(fill(&c, &h, full, 0x7f315ba2) == 0);
    assert(maps == 1 && unmaps == 1 && reads == 0 && writes == 0);
    assert(screen_surface_copy(&h.surface, storage, sizeof(storage), linear, 0));
    for (unsigned i = 0; i < 33 * 17; i++) {
        u32 value;
        memcpy(&value, linear + i * 4, 4);
        assert(value == 0x7f315ba2);
    }
    for (unsigned i = 0; i < 128; i++) {
        assert(storage[i] == 0xa5);
    }
    setenv("GLBRIDGE_FAST_PUBLISH", "1", 1);
    setenv("GLBRIDGE_VERIFY_SURFACES", "1", 1);
    assert(fill(&c, &h, full, 0x12345678) == 0);
    assert(allocated_size == 33 * 17 * 4);
    assert(maps == 2 && unmaps == 2 && reads == 2 && writes == 0);
    assert(fill(&c, &h, partial, 0xabcdef01) == 0);
    assert(maps == 2 && reads == 4 && writes == 2);
    bad_read = 1;
    assert(fill(&c, &h, full, 0) == 48);
    assert(maps == 3 && unmaps == 3);
    bad_read = 0;
    bad_map = 1;
    assert(fill(&c, &h, full, 0) == 48);
    assert(maps == 4 && unmaps == 3);
    bad_map = 0;
    unsetenv("GLBRIDGE_FAST_SURFACES");
    assert(fill(&c, &h, full, 0) == 0);
    assert(maps == 4 && unmaps == 3);
    int invalid[] = {-1, 0, 1, 1};
    assert(fill(&c, &h, invalid, 0) == 48);
    setenv("GLBRIDGE_FAST_SURFACES", "1", 1);
    unsetenv("GLBRIDGE_VERIFY_SURFACES");
    unsigned old_maps = maps, old_reads = reads, old_writes = writes;
    assert(fill(&c, &h, partial, 0xabcdef01) == 0);
    assert(maps == old_maps && reads == old_reads + 2 &&
           writes == old_writes + 2);
    old_reads = reads;
    old_writes = writes;
    assert(fill(&c, &h, full, 0x12345678) == 0);
    assert(allocated_size == h.surface.width * 4);
    assert(maps == old_maps + 1 && reads == old_reads && writes == old_writes);
    bad_map = 1;
    assert(fill(&c, &h, full, 0) == 48);
    bad_map = 0;
    assert(maps == old_maps + 2 && unmaps == maps - 2);
    const unsigned widths[] = {1, 3, 4, 15, 16, 17, 31, 32, 33};
    for (unsigned publish = 0; publish < 2; publish++) {
        setenv("GLBRIDGE_FAST_PUBLISH", publish ? "1" : "0", 1);
        for (unsigned mode = 0; mode <= 6; mode++) {
            h.surface.layout = mode ? 3 : 1;
            h.surface.rest[1] = mode ? mode - 1 : 0;
            for (unsigned w = 0; w < sizeof(widths) / sizeof(widths[0]); w++) {
                h.surface.width = widths[w];
                h.surface.pitch = (widths[w] * 4 + 63) & ~63u;
                full[2] = widths[w];
                for (unsigned alignment = 0; alignment < 4; alignment++) {
                    unsigned char touched[sizeof(storage)] = {0};
                    h.surface.offset = 128 + alignment;
                    memset(storage, 0xa5, sizeof(storage));
                    assert(fill(&c, &h, full, 0x7f315ba2) == 0);
                    assert(allocated_size == h.surface.width * 4 *
                           (publish ? 1 : h.surface.height));
                    for (unsigned y = 0; y < h.surface.height; y++) {
                        for (unsigned x = 0; x < h.surface.width; x++) {
                            unsigned at = h.surface.offset +
                                screen_surface_address(&h.surface, x * 4, y);
                            u32 value;
                            memcpy(&value, storage + at, 4);
                            assert(value == 0x7f315ba2);
                            memset(touched + at, 1, 4);
                        }
                    }
                    for (unsigned i = 0; i < sizeof(storage); i++) {
                        assert(touched[i] || storage[i] == 0xa5);
                    }
                }
            }
        }
    }
    check_capture_copy(0);
    check_capture_copy(1);
    return 0;
}

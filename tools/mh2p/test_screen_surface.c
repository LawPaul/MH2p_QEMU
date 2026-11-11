/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdlib.h>
#include <string.h>

struct ScreenSurface {
    unsigned width, height, format, layout, pitch, memory, offset, rest[11];
};
#include "screen_surface.h"

static unsigned reference(const struct ScreenSurface *s, unsigned x, unsigned y)
{
    if (s->layout == 1) {
        return y * s->pitch + x;
    }
    unsigned height = 1u << s->rest[1];
    unsigned gob = (y >> (3 + s->rest[1])) * (s->pitch >> 6) * height;
    gob += (x >> 6) * height + ((y >> 3) & (height - 1));
    return gob * 512 + ((x & 32) << 3) + ((y & 6) << 5) +
           ((x & 16) << 1) + ((y & 1) << 4) + (x & 15);
}

static void exercise(unsigned width, unsigned height, unsigned shift,
                     unsigned layout, unsigned offset, unsigned linear_offset,
                     int inverted)
{
    struct ScreenSurface s = {
        .width = width, .height = height, .format = 0x20105321,
        .layout = layout, .pitch = ((width * 4 + 63) & ~63u) +
                                  (layout == 1 ? shift : 0),
        .offset = offset, .rest = {0xdb, shift},
    };
    unsigned size = s.offset + screen_surface_extent(&s) + 17;
    unsigned bytes = width * height * 4;
    unsigned char *native = malloc(size), *expected = malloc(size);
    unsigned char *input = malloc(bytes + 3), *output = malloc(bytes + 3);
    assert(native && expected && input && output);
    unsigned char *pixels = input + linear_offset;
    unsigned char *readback = output + linear_offset;
    memset(native, 0xa5, size);
    memset(expected, 0xa5, size);
    for (unsigned i = 0; i < bytes; i++) {
        pixels[i] = (i * 73 + i / 257) & 255;
    }
    for (unsigned y = 0; y < height; y++) {
        for (unsigned x = 0; x < width * 4; x++) {
            unsigned at = s.offset + reference(&s, x, y);
            assert(at < size - 17);
            unsigned linear_y = inverted ? height - y - 1 : y;
            expected[at] = pixels[linear_y * width * 4 + x];
        }
    }
    assert(screen_surface_copy_rows(&s, native, size, pixels, 1, inverted));
    assert(!memcmp(native, expected, size));
    assert(screen_surface_copy_rows(&s, expected, size, readback, 0, inverted));
    assert(!memcmp(pixels, readback, bytes));
    memset(native, 0xa5, size);
    assert(!screen_surface_copy_rows(&s, native, s.offset +
                                     screen_surface_extent(&s) - 1, pixels, 1,
                                     inverted));
    for (unsigned i = 0; i < size; i++) {
        assert(native[i] == 0xa5);
    }
    free(output);
    free(input);
    free(expected);
    free(native);
}

int main(void)
{
    const unsigned dimensions[][2] = {
        {1, 1}, {3, 7}, {4, 5}, {15, 8}, {16, 8}, {17, 9}, {19, 17},
        {31, 16}, {32, 16}, {33, 65}, {65, 257}, {1920, 720},
    };
    for (unsigned i = 0; i < sizeof(dimensions) / sizeof(dimensions[0]); i++) {
        for (unsigned alignment = 0; alignment < 4; alignment++) {
            for (unsigned linear = 0; linear < 4; linear++) {
                for (int inverted = 0; inverted <= 1; inverted++) {
                    for (unsigned padding = 0; padding < 4; padding++) {
                        exercise(dimensions[i][0], dimensions[i][1], padding, 1,
                                 123 + alignment, linear, inverted);
                    }
                    for (unsigned shift = 0; shift <= 5; shift++) {
                        exercise(dimensions[i][0], dimensions[i][1], shift, 3,
                                 123 + alignment, linear, inverted);
                    }
                }
            }
        }
    }
    struct ScreenSurface s = {
        .width = 10, .height = 10, .format = 0x20105321,
        .layout = 3, .pitch = 64, .rest = {0xfe, 0},
    };
    assert(screen_surface_extent(&s) == 1024);
    s.rest[1] = 6;
    assert(!screen_surface_extent(&s));
    s.rest[1] = 0;
    s.rest[0] = 0;
    assert(!screen_surface_extent(&s));
    s.rest[0] = 0xdb;
    s.rest[2] = 1;
    assert(!screen_surface_extent(&s));
    s.rest[2] = 0;
    s.offset = ~0u;
    unsigned char storage[1024], pixels[400];
    memset(storage, 0xa5, sizeof(storage));
    assert(!screen_surface_copy(&s, storage, sizeof(storage), pixels, 1));
    s.offset = 0;
    s.pitch = 63;
    assert(!screen_surface_extent(&s));
    s.pitch = 64;
    s.format = 0x2010d121;
    assert(screen_surface_extent(&s) == 1024);
    for (unsigned i = 0; i < sizeof(pixels); i++) {
        pixels[i] = i * 73;
    }
    assert(screen_surface_copy(&s, storage, sizeof(storage), pixels, 1));
    unsigned char bgra_readback[sizeof(pixels)];
    assert(screen_surface_copy(&s, storage, sizeof(storage), bgra_readback, 0));
    assert(!memcmp(pixels, bgra_readback, sizeof(pixels)));
    s.format = 0;
    assert(!screen_surface_extent(&s));
    s.format = 0x20105321;
    s.width = 0;
    assert(!screen_surface_extent(&s));
    s.width = 2049;
    assert(!screen_surface_extent(&s));
    s.width = 10;
    s.height = 2049;
    assert(!screen_surface_extent(&s));
    return 0;
}

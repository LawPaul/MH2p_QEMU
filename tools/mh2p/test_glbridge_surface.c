/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#include "../../hw/misc/glbridge-surface.h"

static uint32_t reference(const GLSurface *g, unsigned x, unsigned y)
{
    if (g->layout == 1) {
        return y * g->pitch + x;
    }
    unsigned block = y / (8u << g->shift);
    unsigned gob = (y / 8) % (1u << g->shift);
    unsigned column = x / 64;
    unsigned byte = (x % 64) / 32 * 256 + (y % 8) / 2 * 64 +
                    (x % 32) / 16 * 32 + (y % 2) * 16 + x % 16;

    return block * g->pitch * (8u << g->shift) +
           column * (512u << g->shift) + gob * 512 + byte;
}

static GLSurface surface(unsigned width, unsigned height, unsigned shift,
                         unsigned layout, unsigned padding)
{
    GLSurface g = {
        .width = width, .height = height, .shift = shift, .layout = layout,
        .pitch = ((width * 4 + 63) & ~63u) + padding,
        .bytes = width * height * 4,
    };
    unsigned rows = layout == 1 ? height :
        (height + (8u << shift) - 1) & ~((8u << shift) - 1);

    g.extent = rows * g.pitch;
    return g;
}

static void exercise(GLSurface g, unsigned alignment, unsigned linear_alignment,
                     bool inverted)
{
    unsigned size = g.extent + 32;
    uint8_t *native = malloc(size), *expected = malloc(size);
    uint8_t *input = malloc(g.bytes + 32), *output = malloc(g.bytes + 32);

    assert(native && expected && input && output);
    memset(native, 0xa5, size);
    memset(expected, 0xa5, size);
    memset(input, 0xcc, g.bytes + 32);
    memset(output, 0xcc, g.bytes + 32);
    for (unsigned i = 0; i < g.bytes; i++) {
        input[linear_alignment + i] = (i * 73 + i / 257) & 255;
    }
    for (unsigned y = 0; y < g.height; y++) {
        unsigned linear_y = inverted ? g.height - y - 1 : y;

        for (unsigned x = 0; x < g.width * 4; x++) {
            unsigned at = reference(&g, x, y);

            assert(at < g.extent);
            expected[alignment + at] =
                input[linear_alignment + linear_y * g.width * 4 + x];
        }
    }
    gl_surface_swizzle(&g, input + linear_alignment, native + alignment,
                       true, inverted);
    assert(!memcmp(native, expected, size));
    gl_surface_swizzle(&g, output + linear_alignment, expected + alignment,
                       false, inverted);
    assert(!memcmp(input, output, g.bytes + 32));
    unsigned count = (g.extent + 4095) / 4096;
    uint8_t **pages = malloc(count * sizeof(*pages));

    assert(pages);
    for (unsigned i = 0; i < count; i++) {
        unsigned length = MIN(4096, g.extent - i * 4096);

        pages[i] = malloc(length + 32);
        assert(pages[i]);
        memset(pages[i], 0xa5, length + 32);
        pages[i] += alignment;
    }
    gl_surface_swizzle_mapped(&g, input + linear_alignment, NULL, pages,
                              true, inverted);
    for (unsigned i = 0; i < count; i++) {
        unsigned length = MIN(4096, g.extent - i * 4096);

        assert(!memcmp(pages[i], expected + alignment + i * 4096, length));
        for (unsigned j = 0; j < alignment; j++) {
            assert(pages[i][(int)j - (int)alignment] == 0xa5);
        }
        for (unsigned j = length; j < length + 32 - alignment; j++) {
            assert(pages[i][j] == 0xa5);
        }
    }
    memset(output, 0xcc, g.bytes + 32);
    gl_surface_swizzle_mapped(&g, output + linear_alignment, NULL, pages,
                              false, inverted);
    assert(!memcmp(input, output, g.bytes + 32));
    for (unsigned i = 0; i < count; i++) {
        free(pages[i] - alignment);
    }
    free(pages);
    free(output);
    free(input);
    free(expected);
    free(native);
}

static double seconds(void)
{
    struct timespec now;

    assert(!clock_gettime(CLOCK_MONOTONIC, &now));
    return now.tv_sec + now.tv_nsec * 1e-9;
}

static void benchmark(void)
{
    for (unsigned shift = 0; shift <= 5; shift++) {
        GLSurface g = surface(1920, 720, shift, 3, 0);
        uint8_t *native = malloc(g.extent), *linear = malloc(g.bytes);

        assert(native && linear);
        memset(native, 0xa5, g.extent);
        memset(linear, 0x5a, g.bytes);
        for (unsigned writing = 0; writing <= 1; writing++) {
            double best = 1e9;

            for (unsigned sample = 0; sample < 5; sample++) {
                double start = seconds();

                for (unsigned i = 0; i < 100; i++) {
                    gl_surface_swizzle(&g, linear, native, writing, true);
                }
                double elapsed = seconds() - start;
                best = elapsed < best ? elapsed : best;
            }
            printf("shift=%u direction=%s ms_per_frame=%.6f\n", shift,
                   writing ? "write" : "read", best * 10);
        }
        free(linear);
        free(native);
    }
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--benchmark")) {
        benchmark();
        return 0;
    }
    assert(argc == 1);
    const unsigned dimensions[][2] = {
        {1, 1}, {3, 7}, {4, 5}, {15, 8}, {16, 8}, {17, 9},
        {19, 17}, {31, 16}, {32, 16}, {33, 65}, {65, 257}, {1920, 720},
    };
    for (unsigned i = 0; i < sizeof(dimensions) / sizeof(dimensions[0]); i++) {
        for (unsigned alignment = 0; alignment < 4; alignment++) {
            for (unsigned linear_alignment = 0; linear_alignment < 4;
                 linear_alignment++) {
                for (unsigned inverted = 0; inverted <= 1; inverted++) {
                    for (unsigned shift = 0; shift <= 5; shift++) {
                        exercise(surface(dimensions[i][0], dimensions[i][1],
                                         shift, 3, 64), alignment,
                                 linear_alignment, inverted);
                    }
                    exercise(surface(dimensions[i][0], dimensions[i][1],
                                     0, 1, 7), alignment, linear_alignment,
                             inverted);
                }
            }
        }
    }
    return 0;
}

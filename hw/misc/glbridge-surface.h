/*
 * RGBA8 surface conversion for the experimental GL bridge.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_GLBRIDGE_SURFACE_H
#define HW_MISC_GLBRIDGE_SURFACE_H

#include "qemu/compiler.h"

typedef struct GLSurface {
    uint32_t bytes, width, height, pitch, layout, shift, flags, extent, runs;
    const uint8_t *run_table;
} GLSurface;

static uint32_t gl_surface_address(uint32_t layout, uint32_t pitch,
                                   uint32_t shift, uint32_t x, uint32_t y)
{
    if (layout == 1) {
        return y * pitch + x;
    }
    return (y >> (3 + shift)) * pitch * (8u << shift) +
           (x >> 6) * (512u << shift) +
           ((y >> 3) & ((1u << shift) - 1)) * 512 +
           ((x & 32) << 3) + ((y & 6) << 5) +
           ((x & 16) << 1) + ((y & 1) << 4) + (x & 15);
}

static inline uint8_t *gl_surface_pointer(uint8_t *native,
                                         uint8_t *const *pages, uint32_t offset)
{
    return pages ? pages[offset >> 12] + (offset & 4095) : native + offset;
}

/* Specialize the contiguous path without page-table tests in its inner loop. */
static inline QEMU_ALWAYS_INLINE
void gl_surface_swizzle_mapped(const GLSurface *g, uint8_t *linear,
                               uint8_t *native, uint8_t *const *pages,
                               bool to_native, bool inverted)
{
    uint32_t row_bytes = g->width * 4;

    if (g->layout == 1) {
        for (uint32_t y = 0; y < g->height; y++) {
            uint8_t *row = linear +
                (inverted ? g->height - y - 1 : y) * row_bytes;
            uint32_t offset = y * g->pitch;

            for (uint32_t x = 0; x < row_bytes;) {
                uint32_t count = pages ?
                    MIN(row_bytes - x, 4096 - ((offset + x) & 4095)) :
                    row_bytes - x;
                uint8_t *at = gl_surface_pointer(native, pages, offset + x);

                memcpy(to_native ? at : row + x,
                       to_native ? row + x : at, count);
                x += count;
            }
        }
        return;
    }
    if (to_native) {
        /* Finish a native GOB before moving to the next dirty cache lines. */
        for (uint32_t y = 0; y < g->height; y += 8) {
            uint32_t rows = MIN(8, g->height - y);
            uint32_t gob_bytes = 512u << g->shift;
            uint32_t offset =
                gl_surface_address(3, g->pitch, g->shift, 0, y);

            for (uint32_t x = 0; x + 64 <= row_bytes;
                 x += 64, offset += gob_bytes) {
                uint8_t *base = gl_surface_pointer(native, pages, offset);

                for (uint32_t j = 0; j < rows; j++) {
                    uint32_t linear_y =
                        inverted ? g->height - y - j - 1 : y + j;
                    uint8_t *src = linear + linear_y * row_bytes + x;
                    uint8_t *at = base + ((j & 6) << 5) + ((j & 1) << 4);

                    memcpy(at, src, 16);
                    memcpy(at + 32, src + 16, 16);
                    memcpy(at + 256, src + 32, 16);
                    memcpy(at + 288, src + 48, 16);
                }
            }
        }
    } else {
        /* Reading remains row-major so the linear destination is contiguous. */
        for (uint32_t y = 0; y < g->height; y++) {
            uint8_t *row = linear +
                (inverted ? g->height - y - 1 : y) * row_bytes;
            uint32_t offset =
                gl_surface_address(3, g->pitch, g->shift, 0, y);
            uint32_t gob_bytes = 512u << g->shift;

            for (uint32_t x = 0; x + 64 <= row_bytes;
                 x += 64, offset += gob_bytes) {
                uint8_t *base = gl_surface_pointer(native, pages, offset);

                memcpy(row + x, base, 16);
                memcpy(row + x + 16, base + 32, 16);
                memcpy(row + x + 32, base + 256, 16);
                memcpy(row + x + 48, base + 288, 16);
            }
        }
    }
    for (uint32_t y = 0; y < g->height; y++) {
        uint8_t *row = linear + (inverted ? g->height - y - 1 : y) * row_bytes;

        for (uint32_t x = row_bytes & ~63u; x < row_bytes;) {
            uint32_t count = MIN(16 - (x & 15), row_bytes - x);
            uint8_t *at = gl_surface_pointer(native, pages,
                gl_surface_address(g->layout, g->pitch, g->shift, x, y));

            memcpy(to_native ? at : row + x, to_native ? row + x : at, count);
            x += count;
        }
    }
}

static void gl_surface_swizzle(const GLSurface *g, uint8_t *linear,
                               uint8_t *native, bool to_native, bool inverted)
{
    gl_surface_swizzle_mapped(g, linear, native, NULL, to_native, inverted);
}
#endif

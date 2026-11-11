/* Packed RGBA8/BGRA8 copies for the diagnostic bridge, not a native GPU engine.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef MH2P_SCREEN_SURFACE_H
#define MH2P_SCREEN_SURFACE_H

static unsigned screen_surface_extent(const struct ScreenSurface *s)
{
    unsigned rows = s->height;

    if (!s->width || s->width > 2048 || !rows || rows > 2048 ||
        (s->format != 0x20105321 && s->format != 0x2010d121) ||
        s->pitch < s->width * 4 ||
        s->pitch > 16384 || s->rest[2] == 1) {
        return 0;
    }
    if (s->layout == 3) {
        if ((s->rest[0] != 0xdb && s->rest[0] != 0xfe) ||
            s->rest[1] > 5 || s->pitch % 64) {
            return 0;
        }
        unsigned block_rows = 8u << s->rest[1];
        rows = (rows + block_rows - 1) & ~(block_rows - 1);
    } else if (s->layout != 1) {
        return 0;
    }
    return rows * s->pitch;
}

static unsigned screen_surface_address(const struct ScreenSurface *s,
                                       unsigned x, unsigned y)
{
    if (s->layout == 1) {
        return y * s->pitch + x;
    }
    unsigned shift = s->rest[1];
    return (y >> (3 + shift)) * s->pitch * (8u << shift) +
           (x >> 6) * (512u << shift) +
           ((y >> 3) & ((1u << shift) - 1)) * 512 +
           ((x & 32) << 3) + ((y & 6) << 5) +
           ((x & 16) << 1) + ((y & 1) << 4) + (x & 15);
}

static int screen_surface_copy_strided(const struct ScreenSurface *s,
                                      void *mapped, unsigned size, void *linear,
                                      int writing, int inverted,
                                      unsigned linear_stride)
{
    unsigned extent = screen_surface_extent(s);
    if (!extent || s->offset > size || extent > size - s->offset) {
        return 0;
    }
    unsigned char *native = (unsigned char *)mapped + s->offset;
    unsigned char *pixels = linear;
    unsigned row_bytes = s->width * 4;
    int aligned = !(((unsigned long)native | (unsigned long)pixels |
                     s->pitch | linear_stride) & 3);
    for (unsigned y = 0; y < s->height; y++) {
        unsigned x = 0;
        unsigned linear_y = inverted ? s->height - y - 1 : y;

        if (aligned && s->layout == 3) {
            unsigned address = screen_surface_address(s, 0, y);
            unsigned char *row = pixels + linear_y * linear_stride;
            unsigned stride = 512u << s->rest[1];

            /* A 64-byte row segment occupies four 16-byte GOB fragments. */
            for (; row_bytes - x >= 64; x += 64, address += stride, row += 64) {
                unsigned char *at = native + address;

                if (writing) {
                    __builtin_memcpy(__builtin_assume_aligned(at, 4),
                                     __builtin_assume_aligned(row, 4), 16);
                    __builtin_memcpy(__builtin_assume_aligned(at + 32, 4),
                                     __builtin_assume_aligned(row + 16, 4), 16);
                    __builtin_memcpy(__builtin_assume_aligned(at + 256, 4),
                                     __builtin_assume_aligned(row + 32, 4), 16);
                    __builtin_memcpy(__builtin_assume_aligned(at + 288, 4),
                                     __builtin_assume_aligned(row + 48, 4), 16);
                } else {
                    __builtin_memcpy(__builtin_assume_aligned(row, 4),
                                     __builtin_assume_aligned(at, 4), 16);
                    __builtin_memcpy(__builtin_assume_aligned(row + 16, 4),
                                     __builtin_assume_aligned(at + 32, 4), 16);
                    __builtin_memcpy(__builtin_assume_aligned(row + 32, 4),
                                     __builtin_assume_aligned(at + 256, 4), 16);
                    __builtin_memcpy(__builtin_assume_aligned(row + 48, 4),
                                     __builtin_assume_aligned(at + 288, 4), 16);
                }
            }
        }
        for (; x < row_bytes;) {
            unsigned count = s->layout == 1 ? row_bytes : 16;
            if (count > row_bytes - x) {
                count = row_bytes - x;
            }
            unsigned address = screen_surface_address(s, x, y);
            unsigned char *at = native + address;
            unsigned char *row = pixels + linear_y * linear_stride + x;
            unsigned char *destination = writing ? at : row;
            unsigned char *source = writing ? row : at;
            if (aligned && count == 16) {
                /* Inline fixed-size GOB chunks in the freestanding build. */
                __builtin_memcpy(__builtin_assume_aligned(destination, 4),
                                 __builtin_assume_aligned(source, 4), 16);
            } else {
                memcpy(destination, source, count);
            }
            x += count;
        }
    }
    return 1;
}

static inline int screen_surface_copy_rows(const struct ScreenSurface *s,
                                          void *mapped, unsigned size,
                                          void *linear, int writing,
                                          int inverted)
{
    return screen_surface_copy_strided(s, mapped, size, linear, writing,
                                       inverted, s->width * 4);
}

static inline int screen_surface_copy(const struct ScreenSurface *s, void *mapped,
                               unsigned size, void *linear, int writing)
{
    return screen_surface_copy_rows(s, mapped, size, linear, writing, 0);
}
#endif

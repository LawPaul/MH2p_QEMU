/* ASTC records carry original compressed blocks, decoded by the actual GL driver.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef GLHOST_ASTC_TEXTURES_H
#define GLHOST_ASTC_TEXTURES_H

#include "../astc_format.h"

static uint32_t astc_native(void)
{
    const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
    const char *name = "GL_KHR_texture_compression_astc_ldr";
    const char *match = extensions ? strstr(extensions, name) : NULL;

    return match && (match == extensions || match[-1] == ' ') &&
           (!match[strlen(name)] || match[strlen(name)] == ' ');
}

#ifdef GLHOST_ASTC_SOFTWARE
/* Opt-in (MHI2_ASTC_SOFTWARE=1): decode original ASTC blocks on the CPU when the host GL
 * has no ASTC (ANGLE/D3D11), and upload RGBA8 / SRGB8_ALPHA8 instead. */
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_SRGB8_ALPHA8
#define GL_SRGB8_ALPHA8 0x8C43
#endif
#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_UNPACK_SKIP_ROWS 0x0CF3
#define GL_UNPACK_SKIP_PIXELS 0x0CF4
#endif
#ifndef GL_PIXEL_UNPACK_BUFFER
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#define GL_PIXEL_UNPACK_BUFFER_BINDING 0x88EF
#endif
int astc_soft_decode(unsigned block_x, unsigned block_y, int srgb, unsigned width,
                     unsigned height, const uint8_t *blocks, size_t bytes, uint8_t *rgba);

static int astc_software(void)
{
    static int state = -1;
    if (state < 0) {
        const char *e = getenv("MHI2_ASTC_SOFTWARE");
        state = e && *e == '1' && !astc_native();
        if (state) {
            fprintf(stderr, "glhost: ASTC decoded on the CPU (host GL has no ASTC)\n");
        }
    }
    return state;
}
#else
static int astc_software(void) { return 0; }
#endif

static uint32_t astc_supported(void)
{
    return astc_native() || astc_software();
}

static int astc_record_valid(uint32_t op, uint32_t len, const uint8_t *p)
{
    if (len < 32) {
        return 0;
    }
    unsigned expected = op == 142 ?
        astc_image_bytes(u32(p, 8), u32(p, 12), u32(p, 16)) :
        astc_image_bytes(u32(p, 24), u32(p, 16), u32(p, 20));
    if (!expected) {
        return 0;
    }
    if (op == 142) {
        return u32(p, 24) == expected && u32(p, 28) <= 1 &&
               len - 32 == (u32(p, 28) ? expected : 0);
    }
    return u32(p, 28) == expected && len - 32 == expected;
}

#ifdef GLHOST_ASTC_SOFTWARE
static void astc_upload_rgba(uint32_t op, const uint8_t *p)
{
    static const unsigned char dims[14][2] = {
        {4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6},
        {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12},
    };
    uint32_t format = op == 142 ? u32(p, 8) : u32(p, 24);
    uint32_t width = op == 142 ? u32(p, 12) : u32(p, 16);
    uint32_t height = op == 142 ? u32(p, 16) : u32(p, 20);
    int index = astc_format_index(format), srgb = format >= 0x93d0;
    const uint8_t *data = op == 142 ? (u32(p, 28) ? p + 32 : NULL) : p + 32;
    uint8_t *rgba = NULL;

    if (index < 0) {
        return;
    }
    if (data) {
        rgba = (uint8_t *)malloc((size_t)width * height * 4);
        if (!rgba || astc_soft_decode(dims[index][0], dims[index][1], srgb, width, height,
                                      data, astc_image_bytes(format, width, height), rgba)) {
            fprintf(stderr, "glhost: ASTC CPU decode failed (format 0x%x %ux%u)\n",
                    format, width, height);
            free(rgba);
            return;
        }
    }
    /* The guest's unpack state applies to its compressed data, not to this RGBA copy. */
    GLint align, row, rows, pixels, buffer;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &rows);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &pixels);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &buffer);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    if (op == 142) {
        glTexImage2D(u32(p, 0), i32(p, 4), srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8,
                     width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    } else {
        glTexSubImage2D(u32(p, 0), i32(p, 4), i32(p, 8), i32(p, 12), width, height,
                        GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, align);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, row);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, rows);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, pixels);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer);
    free(rgba);
}
#endif

static void astc_upload(uint32_t op, const uint8_t *p)
{
#ifdef GLHOST_ASTC_SOFTWARE
    if (astc_software()) {
        astc_upload_rgba(op, p);
        return;
    }
#endif
    if (op == 142) {
        glCompressedTexImage2D(u32(p, 0), i32(p, 4), u32(p, 8),
                               i32(p, 12), i32(p, 16), i32(p, 20),
                               i32(p, 24), u32(p, 28) ? p + 32 : NULL);
    } else {
        glCompressedTexSubImage2D(u32(p, 0), i32(p, 4), i32(p, 8), i32(p, 12),
                                  i32(p, 16), i32(p, 20), u32(p, 24),
                                  i32(p, 28), p + 32);
    }
}

#endif

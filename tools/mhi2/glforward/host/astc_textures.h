/* ASTC records carry original compressed blocks, decoded by the actual GL driver.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef GLHOST_ASTC_TEXTURES_H
#define GLHOST_ASTC_TEXTURES_H

#include "../astc_format.h"

static uint32_t astc_supported(void)
{
    const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
    const char *name = "GL_KHR_texture_compression_astc_ldr";
    const char *match = extensions ? strstr(extensions, name) : NULL;

    return match && (match == extensions || match[-1] == ' ') &&
           (!match[strlen(name)] || match[strlen(name)] == ' ');
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

static void astc_upload(uint32_t op, const uint8_t *p)
{
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

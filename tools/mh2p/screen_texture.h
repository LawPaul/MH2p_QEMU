/* Real ASTC uploads for the experimental Screen bridge.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef MH2P_SCREEN_TEXTURE_H
#define MH2P_SCREEN_TEXTURE_H

#include "../mhi2/glforward/astc_format.h"

static int screen_astc_supported(void)
{
    static u32 context, supported;

    if (!current_record()->egl_context) {
        screen_gl_failure("texture capability query needs a current context",
                          0x502);
        return 0;
    }
    if (context != current_screen_context()->id) {
        rec(141, 0);
        if (recv_all(&supported, sizeof(supported))) {
            screen_gl_failure("texture capability transport failed", 0x502);
            return 0;
        }
        context = current_screen_context()->id;
    }
    return supported != 0;
}

const char *glGetStringi(u32 name, u32 index)
{
    if (name != 0x1f03) {
        screen_gl_failure("unsupported indexed string query", 0x500);
        return 0;
    }
    if (index || !screen_astc_supported()) {
        screen_gl_failure("invalid extension index", 0x501);
        return 0;
    }
    return "GL_KHR_texture_compression_astc_ldr";
}

static int screen_compressed_input(u32 format, i32 width, i32 height,
                                    i32 bytes)
{
    if (astc_format_index(format) < 0 || !screen_astc_supported()) {
        screen_gl_failure("unsupported compressed texture format", 0x500);
        return 0;
    }
    unsigned expected = astc_image_bytes(format, width, height);
    if (!expected || bytes < 0 || (unsigned)bytes != expected) {
        screen_gl_failure("invalid compressed texture size", 0x501);
        return 0;
    }
    if (current_screen_context()->version >= 3) {
        i32 binding = 0x88ef, result[16];
        emit_iv(104, &binding, 1);
        if (recv_all(result, sizeof(result)) || result[0]) {
            screen_gl_failure("compressed pixel-buffer imports are unsupported",
                              0x502);
            return 0;
        }
    }
    return 1;
}

void glCompressedTexImage2D(u32 target, i32 level, u32 format, i32 width,
                            i32 height, i32 border, i32 bytes, const void *data)
{
    if (!screen_image_storage(target) ||
        !screen_compressed_input(format, width, height, bytes)) {
        return;
    }
    u32 header[] = {target, level, format, width, height, border, bytes, !!data};
    rec(142, sizeof(header) + (data ? bytes : 0));
    PART(header, sizeof(header));
    if (data) {
        PART(data, bytes);
    }
}

void glCompressedTexSubImage2D(u32 target, i32 level, i32 x, i32 y, i32 width,
                               i32 height, u32 format, i32 bytes,
                               const void *data)
{
    if (!screen_image_storage(target) ||
        !screen_compressed_input(format, width, height, bytes)) {
        return;
    }
    if (!data) {
        screen_gl_failure("null compressed subimage", 0x501);
        return;
    }
    u32 header[] = {target, level, x, y, width, height, format, bytes};
    rec(143, sizeof(header) + bytes);
    PART(header, sizeof(header));
    PART(data, bytes);
}

#endif

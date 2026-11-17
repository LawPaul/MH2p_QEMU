/* Optional CPU ASTC LDR decode for hosts whose GL lacks GL_KHR_texture_compression_astc_ldr
 * (e.g. ANGLE on Direct3D 11, Intel Gen12). Built only with -DGLHOST_ASTC_SOFTWARE and linked
 * against ARM astc-encoder (Apache-2.0); build steps in README.Windows.md.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>
#include <stddef.h>
#include <mutex>

#include "astcenc.h"

extern "C" int astc_soft_decode(unsigned block_x, unsigned block_y, int srgb,
                                unsigned width, unsigned height,
                                const uint8_t *blocks, size_t bytes, uint8_t *rgba)
{
    static std::mutex lock;
    static astcenc_context *contexts[2][13][13];
    if (block_x < 4 || block_x > 12 || block_y < 4 || block_y > 12) {
        return -1;
    }
    std::lock_guard<std::mutex> guard(lock);
    astcenc_context *&context = contexts[srgb ? 1 : 0][block_x][block_y];
    if (!context) {
        astcenc_config config;
        if (astcenc_config_init(srgb ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR,
                                block_x, block_y, 1, ASTCENC_PRE_FASTEST,
                                ASTCENC_FLG_DECOMPRESS_ONLY, &config) != ASTCENC_SUCCESS ||
            astcenc_context_alloc(&config, 1, &context) != ASTCENC_SUCCESS) {
            context = nullptr;
            return -1;
        }
    }
    void *slices[1] = {rgba};
    astcenc_image image;
    image.dim_x = width;
    image.dim_y = height;
    image.dim_z = 1;
    image.data_type = ASTCENC_TYPE_U8;
    image.data = slices;
    const astcenc_swizzle swizzle = {ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    astcenc_error error = astcenc_decompress_image(context, blocks, bytes, &image, &swizzle, 0);
    astcenc_decompress_reset(context);
    return error == ASTCENC_SUCCESS ? 0 : -1;
}

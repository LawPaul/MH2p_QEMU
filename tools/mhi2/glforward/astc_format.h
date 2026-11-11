/* Bounded ASTC LDR transport geometry. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef GLBRIDGE_ASTC_FORMAT_H
#define GLBRIDGE_ASTC_FORMAT_H

static int astc_format_index(unsigned format)
{
    if (format >= 0x93b0 && format <= 0x93bd) {
        return format - 0x93b0;
    }
    if (format >= 0x93d0 && format <= 0x93dd) {
        return format - 0x93d0;
    }
    return -1;
}

static unsigned astc_image_bytes(unsigned format, unsigned width, unsigned height)
{
    static const unsigned char blocks[14][2] = {
        {4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6},
        {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12},
    };
    int index = astc_format_index(format);

    if (index < 0 || !width || width > 2048 || !height || height > 2048) {
        return 0;
    }
    return ((width + blocks[index][0] - 1) / blocks[index][0]) *
           ((height + blocks[index][1] - 1) / blocks[index][1]) * 16;
}

#endif

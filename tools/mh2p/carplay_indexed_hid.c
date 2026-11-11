/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Laboratory ABI wrappers only; not a firmware installation payload. */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

struct Screen {
    u16 width, height;
    u32 maximum;
};

struct Contact {
    u16 x, y;
    u8 down, reserved;
};

extern void *malloc(u32 size);
extern void free(void *pointer);
typedef int (*Descriptor)(u8 **output, u32 *length,
                          const struct Screen *screen);
typedef int (*Report)(u8 **output, u32 *length,
                      const struct Contact *contacts, u32 count,
                      u32 maximum, u8 release);

/* Preserve the Thumb bit when calling the identity-checked original image. */
static Descriptor const original_descriptor = (Descriptor)0x810bc775u;
static Report const original_report = (Report)0x810bca05u;

int indexed_descriptor(u8 **output, u32 *length, const struct Screen *screen)
{
    /* Index 0..3, before the original touch and coordinate fields. */
    static const u8 index_field[] = {
        0x05, 0x0d, 0x09, 0x38, 0x15, 0x00, 0x25, 0x03,
        0x75, 0x08, 0x95, 0x01, 0x81, 0x02,
    };
    u8 *original, *result;
    u32 size, position = 0;
    int status;

    if (!output || !length || !screen ||
        screen->maximum < 1 || screen->maximum > 4) {
        return -50;
    }
    status = original_descriptor(&original, &size, screen);
    if (status) {
        return status;
    }
    if (size != 7 + 55 * screen->maximum) {
        free(original);
        return -50;
    }
    result = malloc(size + sizeof(index_field) * screen->maximum);
    if (!result) {
        free(original);
        return -108;
    }
    for (u32 i = 0; i < 6; i++) {
        result[position++] = original[i];
    }
    for (u32 slot = 0; slot < screen->maximum; slot++) {
        const u8 *finger = original + 6 + 55 * slot;
        for (u32 i = 0; i < 6; i++) {
            result[position++] = finger[i];
        }
        for (u32 i = 0; i < sizeof(index_field); i++) {
            result[position++] = index_field[i];
        }
        for (u32 i = 6; i < 55; i++) {
            result[position++] = finger[i];
        }
    }
    result[position++] = original[size - 1];
    free(original);
    *output = result;
    *length = position;
    return 0;
}

int indexed_report(u8 **output, u32 *length,
                   const struct Contact *contacts, u32 count,
                   u32 maximum, u8 release)
{
    u8 *original, *result;
    u32 size;
    int status;

    if (!output || !length || (!contacts && count) ||
        maximum < 1 || maximum > 4) {
        return -50;
    }
    status = original_report(&original, &size, contacts, count, maximum, release);
    if (status) {
        return status;
    }
    if (size != 5 * maximum) {
        free(original);
        return -50;
    }
    result = malloc(6 * maximum);
    if (!result) {
        free(original);
        return -108;
    }
    for (u32 slot = 0; slot < maximum; slot++) {
        result[6 * slot] = slot;
        for (u32 i = 0; i < 5; i++) {
            result[6 * slot + 1 + i] = original[5 * slot + i];
        }
    }
    free(original);
    *output = result;
    *length = 6 * maximum;
    return 0;
}

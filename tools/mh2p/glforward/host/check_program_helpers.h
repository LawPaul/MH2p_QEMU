/* Shared checks for exact program caches extracted from user firmware.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef CHECK_PROGRAM_HELPERS_H
#define CHECK_PROGRAM_HELPERS_H
#include <assert.h>

static GLuint load_program(const char *directory, const char *key, unsigned id)
{
    char path[4096];
    uint8_t header[24];
    snprintf(path, sizeof(path), "%s/%s.program", directory, key);
    FILE *file = fopen(path, "rb");
    assert(file && fread(header, 1, sizeof(header), file) == sizeof(header));
    unsigned size = u32(header, 8);
    assert(size && size <= 4 * 1024 * 1024);
    uint8_t *binary = malloc(size);
    assert(binary && fread(binary, 1, size, file) == size && !fclose(file));
    gmap[id] = glCreateProgram();
    assert(cached_program(id, 0x8e21, binary, size) == GL_NO_ERROR);
    free(binary);
    glUseProgram(M(id));
    return M(id);
}

static GLint uniform(GLuint program, const char *name)
{
    GLint location = glGetUniformLocation(program, name);
    assert(location >= 0);
    return location;
}
#endif

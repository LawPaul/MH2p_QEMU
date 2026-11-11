/* Verified original GLSL and explicit native-instruction transcriptions.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef GLHOST_PROGRAM_CACHE_H
#define GLHOST_PROGRAM_CACHE_H

static uint32_t cached_program(uint32_t id, uint32_t format,
                                const uint8_t *binary, uint32_t bytes)
{
    const char *directory = getenv("MHI2_SHADER_CACHE");
    uint64_t hash = UINT64_C(14695981039346656037);
    char path[4096];
    uint8_t *record = NULL;
    GLuint shaders[2] = {0}, program = 0;
    uint32_t error = GL_INVALID_OPERATION;
    GLint active, compiled = 0, linked = 0;
    FILE *file = NULL;

    if (format != 0x8e21) {
        return GL_INVALID_ENUM;
    }
    if (!glIsProgram(M(id))) {
        fprintf(stderr, "glhost: binary target is not a program: %u\n", id);
        return GL_INVALID_OPERATION;
    }
    glGetIntegerv(GL_CURRENT_PROGRAM, &active);
    if ((GLuint)active == M(id)) {
        fprintf(stderr, "glhost: active program binary reload is unsupported\n");
        return GL_INVALID_OPERATION;
    }
    for (unsigned i = 0; i < bytes; i++) {
        hash = (hash ^ binary[i]) * UINT64_C(1099511628211);
    }
    if (!directory) {
        fprintf(stderr, "glhost: no original program cache configured\n");
        return GL_INVALID_OPERATION;
    }
    int n = snprintf(path, sizeof(path), "%s/%016llx-%u.program", directory,
                     (unsigned long long)hash, bytes);
    if (n < 0 || n >= sizeof(path)) {
        fprintf(stderr, "glhost: program cache path exceeds limit\n");
        return GL_INVALID_OPERATION;
    }
    file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "glhost: original program unavailable %016llx-%u: %s\n",
                (unsigned long long)hash, bytes, strerror(errno));
        return GL_INVALID_OPERATION;
    }
    if (fseek(file, 0, SEEK_END)) {
        goto invalid;
    }
    long size = ftell(file);
    if (size < 24 || size > 6 * 1024 * 1024 + 24 ||
        fseek(file, 0, SEEK_SET)) {
        goto invalid;
    }
    record = malloc(size);
    if (!record) {
        error = GL_OUT_OF_MEMORY;
        goto invalid;
    }
    if (fread(record, 1, size, file) != size ||
        memcmp(record, "MH2PGLSL", 8)) {
        goto invalid;
    }
    uint32_t stored = u32(record, 8), vs_size = u32(record, 12);
    uint32_t fs_size = u32(record, 16), mode = u32(record, 20);
    if (stored != bytes || stored > 4 * 1024 * 1024 ||
        !vs_size || vs_size > 1024 * 1024 ||
        !fs_size || fs_size > 1024 * 1024 || mode > 5 ||
        (uint64_t)24 + stored + vs_size + fs_size != size ||
        memcmp(record + 24, binary, bytes)) {
        goto invalid;
    }
    const char *sources[] = {(char *)record + 24 + stored,
                            (char *)record + 24 + stored + vs_size};
    GLint lengths[] = {vs_size, fs_size};
    if (memchr(sources[0], 0, vs_size) || memchr(sources[1], 0, fs_size)) {
        goto invalid;
    }
    program = glCreateProgram();
    if (!program) {
        error = GL_OUT_OF_MEMORY;
        goto invalid;
    }
    for (unsigned i = 0; i < 2; i++) {
        shaders[i] = glCreateShader(i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
        if (!shaders[i]) {
            error = GL_OUT_OF_MEMORY;
            goto invalid;
        }
        glShaderSource(shaders[i], 1, sources + i, lengths + i);
        glCompileShader(shaders[i]);
        glGetShaderiv(shaders[i], GL_COMPILE_STATUS, &compiled);
        check_shader("original program shader", shaders[i], 0);
        if (!compiled) {
            goto invalid;
        }
        glAttachShader(program, shaders[i]);
    }
    glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    check_shader("original program link", program, 1);
    if (!linked) {
        goto invalid;
    }
    glDeleteProgram(M(id));
    gmap[id] = program;
    blend->program[id] = mode;
    blend->fragment[id] = 0;
    program = 0;
    error = GL_NO_ERROR;
    fprintf(stderr, "glhost: linked original program %016llx-%u id=%u\n",
            (unsigned long long)hash, bytes, id);
    goto done;
invalid:
    fprintf(stderr, "glhost: rejected original program cache %016llx-%u\n",
            (unsigned long long)hash, bytes);
done:
    if (program) {
        glDeleteProgram(program);
    }
    for (unsigned i = 0; i < 2; i++) {
        if (shaders[i]) {
            glDeleteShader(shaders[i]);
        }
    }
    free(record);
    fclose(file);
    return error;
}

#endif

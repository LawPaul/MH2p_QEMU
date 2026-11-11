/* Check exact-identity program loading, real linking/pixels and closed failures.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define main glhost_main
#include "glhost.c"
#undef main
#include <assert.h>

static void put_cache(const char *path, const unsigned char *binary, unsigned size,
                      const char *vertex, const char *fragment)
{
    FILE *f = fopen(path, "wb");
    uint32_t lengths[] = {size, strlen(vertex), strlen(fragment), 0};
    assert(f);
    assert(fwrite("MH2PGLSL", 1, 8, f) == 8);
    assert(fwrite(lengths, 1, 16, f) == 16);
    assert(fwrite(binary, 1, size, f) == size);
    assert(fwrite(vertex, 1, strlen(vertex), f) == strlen(vertex));
    assert(fwrite(fragment, 1, strlen(fragment), f) == strlen(fragment));
    assert(!fclose(f));
}

int main(void)
{
    char directory[] = "/tmp/mh2p-program-test-XXXXXX", path[256];
    const unsigned char binary[] = {10, 0, 0, 0, 1, 2, 3, 4};
    const char *vertex =
        "attribute vec2 p; void main(){gl_Position=vec4(p,0.,1.);}";
    const char *fragment =
        "precision mediump float; uniform vec4 color;"
        "void main(){gl_FragColor=color;}";
    uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned i = 0; i < sizeof(binary); i++) {
        hash = (hash ^ binary[i]) * UINT64_C(1099511628211);
    }
    assert(mkdtemp(directory));
    snprintf(path, sizeof(path), "%s/%016llx-%zu.program",
             directory, (unsigned long long)hash, sizeof(binary));
    setenv("MHI2_SHADER_CACHE", directory, 1);
    ensure_window(8, 8);
    select_client(100);
    gmap[5] = glCreateProgram();
    assert(cached_program(5, 0x8e21, binary, sizeof(binary)) == GL_INVALID_OPERATION);
    assert(cached_program(5, 0xffff, binary, sizeof(binary)) == GL_INVALID_ENUM);
    assert(cached_program(6, 0x8e21, binary, sizeof(binary)) == GL_INVALID_OPERATION);
    put_cache(path, binary, sizeof(binary), vertex, fragment);
    assert(cached_program(5, 0x8e21, binary, sizeof(binary)) == GL_NO_ERROR);
    GLint linked;
    glGetProgramiv(M(5), GL_LINK_STATUS, &linked);
    assert(linked);
    glUseProgram(M(5));
    assert(cached_program(5, 0x8e21, binary, sizeof(binary)) == GL_INVALID_OPERATION);
    const float vertices[] = {-1, -1, 3, -1, -1, 3};
    GLint attribute = glGetAttribLocation(M(5), "p");
    GLint uniform = glGetUniformLocation(M(5), "color");
    assert(attribute >= 0 && uniform >= 0);
    glVertexAttribPointer(attribute, 2, GL_FLOAT, GL_FALSE, 0, vertices);
    glEnableVertexAttribArray(attribute);
    glUniform4f(uniform, 1, 0, 0, 1);
    glViewport(0, 0, 8, 8);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    unsigned char pixels[8 * 8 * 4];
    glReadPixels(0, 0, 8, 8, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    assert(glGetError() == GL_NO_ERROR);
    for (unsigned i = 0; i < sizeof(pixels); i += 4) {
        assert(pixels[i] == 255 && !pixels[i + 1] &&
               !pixels[i + 2] && pixels[i + 3] == 255);
    }
    glUseProgram(0);
    FILE *file = fopen(path, "r+b");
    assert(file && !fseek(file, 24, SEEK_SET));
    assert(fputc(11, file) != EOF && !fclose(file));
    assert(cached_program(5, 0x8e21, binary, sizeof(binary)) == GL_INVALID_OPERATION);
    gmap[6] = glCreateProgram();
    put_cache(path, binary, sizeof(binary), vertex, "not a shader");
    assert(cached_program(6, 0x8e21, binary, sizeof(binary)) == GL_INVALID_OPERATION);
    glGetProgramiv(M(6), GL_LINK_STATUS, &linked);
    assert(!linked);
    uint32_t request[] = {5, 0x8e21, 8, 10, 0x04030201};
    assert(valid_record(144, sizeof(request), (uint8_t *)request));
    assert(!valid_record(144, 12, (uint8_t *)request));
    request[2] = UINT32_MAX;
    assert(!valid_record(144, sizeof(request), (uint8_t *)request));
    glDeleteProgram(M(5));
    glDeleteProgram(M(6));
    assert(!unlink(path) && !rmdir(directory));
    puts("PASS: exact program identity, real GLSL linking/reflection/pixels, "
         "missing/corrupt cache and compiler failures");
    return 0;
}

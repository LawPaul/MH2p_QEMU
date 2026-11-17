/* Firmware-free ASTC decoding and compressed wire-boundary checks.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define main glhost_main
#include "glhost.c"
#undef main
#include <assert.h>

static GLuint shader(GLenum type, const char *source)
{
    GLuint id = glCreateShader(type);
    glShaderSource(id, 1, &source, NULL);
    glCompileShader(id);
    GLint ok;
    glGetShaderiv(id, GL_COMPILE_STATUS, &ok);
    assert(ok);
    return id;
}

static void pixels(unsigned red, unsigned blue)
{
    unsigned char p[8 * 8 * 4];
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glReadPixels(0, 0, 8, 8, GL_RGBA, GL_UNSIGNED_BYTE, p);
    assert(glGetError() == GL_NO_ERROR);
    for (unsigned i = 0; i < sizeof(p); i += 4) {
        assert(p[i] == red && !p[i + 1] && p[i + 2] == blue && p[i + 3] == 255);
    }
}

int main(void)
{
    ensure_window(8, 8);
    select_client(100);
    assert(astc_supported());
    assert(valid_record(141, 0, NULL) && !valid_record(141, 4, NULL));
    GLuint program = glCreateProgram();
    GLuint vertex = shader(GL_VERTEX_SHADER,
        "attribute vec2 p; void main(){gl_Position=vec4(p,0.,1.);}");
    GLuint fragment = shader(GL_FRAGMENT_SHADER,
        "precision mediump float; uniform sampler2D t;"
        "void main(){gl_FragColor=texture2D(t,vec2(.5));}");
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glBindAttribLocation(program, 0, "p");
    glLinkProgram(program);
    GLint linked;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    assert(linked);
    glUseProgram(program);
    const float vertices[] = {-1, -1, 3, -1, -1, 3};
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
    glEnableVertexAttribArray(0);
    glViewport(0, 0, 8, 8);
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    for (unsigned i = 0; i < 28; i++) {
        unsigned format = i < 14 ? 0x93b0 + i : 0x93d0 + i - 14;
        uint32_t image[] = {GL_TEXTURE_2D, 0, format, 4, 4, 0, 16, 1,
                           0xfffffdfc, 0xffffffff, 0x0000ffff, 0xffff0000};
        uint32_t sub[] = {GL_TEXTURE_2D, 0, 0, 0, 4, 4, format, 16,
                         0xfffffdfc, 0xffffffff, 0, 0xffffffff};
        assert(valid_record(142, sizeof(image), (uint8_t *)image));
        assert(valid_record(143, sizeof(sub), (uint8_t *)sub));
        astc_upload(142, (uint8_t *)image);
        pixels(255, 0);
        astc_upload(143, (uint8_t *)sub);
        pixels(0, 255);
        assert(!valid_record(142, 31, (uint8_t *)image));
        assert(!valid_record(143, sizeof(sub) - 1, (uint8_t *)sub));
        image[7] = 2;
        assert(!valid_record(142, sizeof(image), (uint8_t *)image));
        image[7] = 0;
        assert(valid_record(142, 32, (uint8_t *)image));
        assert(!valid_record(142, sizeof(image), (uint8_t *)image));
        image[3] = UINT32_MAX;
        assert(!valid_record(142, 32, (uint8_t *)image));
    }
    assert(astc_image_bytes(0x93b0, 2048, 2048) == 4194304);
    assert(astc_image_bytes(0x93b7, 352, 352) == 30976);
    assert(!astc_image_bytes(0x93be, 4, 4));
    assert(!astc_image_bytes(0x93b0, 0, 4));
    assert(!astc_image_bytes(0x93b0, 2049, 4));
    glDeleteTextures(1, &texture);
    glDeleteProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    puts("PASS: all 28 ASTC LDR formats, original block decoding, subimage "
         "updates, every sampled pixel, bounds and malformed records");
    return 0;
}

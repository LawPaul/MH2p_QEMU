/* Check P2865 NV transcriptions against pixel-level compositor arithmetic.
 * Requires the cache extracted from the user's original firmware.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define main glhost_main
#include "glhost.c"
#undef main
#include <GLES2/gl2ext.h>
#include <math.h>
#include "check_program_helpers.h"

static void attribute(GLuint program, const char *name, const float *values)
{
    GLint location = glGetAttribLocation(program, name);
    assert(location >= 0);
    glVertexAttribPointer(location, 2, GL_FLOAT, GL_FALSE, 0, values);
    glEnableVertexAttribArray(location);
}

static void draw(uint8_t *pixels)
{
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glReadPixels(0, 0, 8, 8, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    assert(glGetError() == GL_NO_ERROR);
}

static void pixel(const uint8_t *p, unsigned r, unsigned g, unsigned b, unsigned a)
{
    unsigned expected[] = {r, g, b, a};
    for (unsigned i = 0; i < 4; i++) {
        assert(abs((int)p[i] - (int)expected[i]) <= 1);
    }
}

static unsigned check_debug(const char *directory, const float *unit)
{
    GLuint program = load_program(directory, "ff564d5a6715eaa3-10085", 4);
    static const struct {
        const char *name;
        GLenum type;
        GLint size;
    } uniforms[] = {
        {"uDigits[0]", GL_INT, 10}, {"uDisplayable", GL_INT, 1},
        {"uOpacity", GL_FLOAT, 1}, {"uSize", GL_FLOAT_VEC4, 1},
        {"uSource", GL_FLOAT_VEC4, 1}, {"uTarget", GL_FLOAT_VEC4, 1},
        {"uTex", GL_SAMPLER_EXTERNAL_OES, 1},
    };
    GLint count;
    glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &count);
    assert(count == 7 && !blend->program[4]);
    for (unsigned n = 0; n < 7; n++) {
        bool found = false;
        for (GLint i = 0; i < count; i++) {
            char name[128];
            GLint size;
            GLenum type;
            glGetActiveUniform(program, i, sizeof(name), NULL, &size, &type, name);
            if (!strcmp(name, uniforms[n].name)) {
                assert(type == uniforms[n].type && size == uniforms[n].size);
                found = true;
            }
        }
        assert(found);
    }
    attribute(program, "position", unit);
    glGetProgramiv(program, GL_ACTIVE_ATTRIBUTES, &count);
    assert(count == 1);
    char name[128];
    GLenum type;
    glGetActiveAttrib(program, 0, sizeof(name), NULL, &count, &type, name);
    assert(!strcmp(name, "position") && type == GL_FLOAT_VEC2 && count == 1);

    enum { WIDTH = 160, HEIGHT = 112 };
    uint8_t pixels[WIDTH * HEIGHT * 4];
    GLuint framebuffer, output;
    glGenTextures(1, &output);
    glBindTexture(GL_TEXTURE_2D, output);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, WIDTH, HEIGHT, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, output, 0);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    glViewport(0, 0, WIDTH, HEIGHT);
    glUniform4f(uniform(program, "uSize"), 4, 4, WIDTH, HEIGHT);
    glUniform4f(uniform(program, "uTarget"), 16, 8, 128, 96);
    glUniform1i(uniform(program, "uTex"), 0);
    const GLint masks[] = {
        0x1fffff, 1, 0x100000, 0x155555, 0xaaaaa,
        0x104104, 0x49249, -1, INT32_MIN + 1, 0x40000100,
    };
    const GLint ids[] = {-1, 0, 1, 2, 9, 10, 11, 16, 98, 99,
                         100, 101, 109, 110, 123, 999, 1000, 9876};
    const float opacity[] = {0, .25, 1};
    glUniform1iv(uniform(program, "uDigits[0]"), 10, masks);
    unsigned checked = 0, white = 0;
    for (unsigned crop = 0; crop < 2; crop++) {
        glUniform4f(uniform(program, "uSource"), crop ? .5 : 0,
                     crop ? .25 : 0, crop ? 3 : 4, crop ? 3 : 4);
        for (unsigned n = 0; n < sizeof(ids) / sizeof(ids[0]); n++) {
            glUniform1i(uniform(program, "uDisplayable"), ids[n]);
            for (unsigned alpha = 0; alpha < 3; alpha++) {
                glUniform1f(uniform(program, "uOpacity"), opacity[alpha]);
                glClearColor(0, 0, 0, 0);
                glClear(GL_COLOR_BUFFER_BIT);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                glReadPixels(0, 0, WIDTH, HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
                assert(glGetError() == GL_NO_ERROR);
                for (unsigned y = 0; y < HEIGHT; y++) {
                    for (unsigned x = 0; x < WIDTH; x++) {
                        const uint8_t *p = pixels + (y * WIDTH + x) * 4;
                        if (x < 16 || x >= 144 || y < 8 || y >= 104) {
                            pixel(p, 0, 0, 0, 0);
                        } else {
                            float u = (x + .5f - 16) / 128;
                            float v = (HEIGHT - y - .5f - 8) / 96;
                            if (crop) {
                                u = (u * 3 + .5f) / 4;
                                v = (v * 3 + .25f) / 4;
                            }
                            bool ink = false;
                            int cursor = 10;
                            for (int place = 100; place >= 1; place /= 10) {
                                if (ids[n] > place) {
                                    float col = (u * 128 - cursor) / 7;
                                    float row = (v * 96 - (96 - 59)) / 7;
                                    if (col >= 0 && col < 3 && row >= 0 && row < 7) {
                                        unsigned bit = (int)row * 3 + (int)col;
                                        ink |= (masks[(ids[n] / place) % 10] &
                                                (1 << bit)) != 0;
                                    }
                                    cursor += 28;
                                }
                            }
                            if (ink) {
                                pixel(p, 255, 255, 255, 255);
                                white++;
                            } else {
                                pixel(p, 32 + (unsigned)(u * 4) * 48,
                                      16 + (unsigned)(v * 4) * 64, 64,
                                      192 * opacity[alpha]);
                            }
                        }
                        checked++;
                    }
                }
            }
        }
    }
    assert(white > 0 && checked == 1935360);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &output);
    return checked;
}

static unsigned texel(float coordinate)
{
    return coordinate <= 0 ? 0 : coordinate >= 1 ? 3 : (unsigned)(coordinate * 4);
}

static unsigned check_animation(const char *directory, const float *clip)
{
    GLuint program = load_program(directory, "243e48c5d2f8a7a4-10117", 5);
    static const char *names[] = {
        "mask", "maskAngle", "maskOffset", "opacity", "tex",
        "uAngle", "uResolution", "uSource", "uTarget",
    };
    static const GLenum types[] = {
        GL_SAMPLER_2D, GL_FLOAT, GL_FLOAT_VEC2, GL_FLOAT, GL_SAMPLER_2D,
        GL_FLOAT, GL_FLOAT_VEC2, GL_FLOAT_VEC4, GL_FLOAT_VEC4,
    };
    GLint count;
    glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &count);
    assert(count == 9 && !blend->program[5]);
    for (unsigned n = 0; n < 9; n++) {
        bool found = false;
        for (GLint i = 0; i < count; i++) {
            char name[128];
            GLint size;
            GLenum type;
            glGetActiveUniform(program, i, sizeof(name), NULL, &size, &type, name);
            if (!strcmp(name, names[n])) {
                assert(type == types[n] && size == 1);
                found = true;
            }
        }
        assert(found);
    }
    attribute(program, "position", clip);
    GLuint textures[2];
    uint8_t input[2][4 * 4 * 4], pixels[8 * 8 * 4];
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            unsigned at = (y * 4 + x) * 4;
            const uint8_t color[] = {32 + 48 * x, 16 + 64 * y, 64, 224 - 16 * y};
            const uint8_t mask[] = {x == 3 ? 255 : 32 * (x + y), 0, 0, 255};
            memcpy(input[0] + at, color, 4);
            memcpy(input[1] + at, mask, 4);
        }
    }
    glGenTextures(2, textures);
    for (unsigned i = 0; i < 2; i++) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, textures[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, input[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glUniform1i(uniform(program, "tex"), 0);
    glUniform1i(uniform(program, "mask"), 1);
    glUniform2f(uniform(program, "uResolution"), 8, 8);
    glUniform4f(uniform(program, "uSource"), .14, .1, .6, .7);
    glUniform2f(uniform(program, "maskOffset"), 1.25, -.75);
    glViewport(0, 0, 8, 8);
    const float angles[] = {0, .61, 1.57079632679};
    unsigned checked = 0;
    for (unsigned shape = 0; shape < 4; shape++) {
        float width = 4 + (shape & 1), height = 4 + (shape >> 1);
        glUniform4f(uniform(program, "uTarget"), 3.17, 4.11, width, height);
        for (unsigned a = 0; a < 3; a++) {
            float c = cosf(angles[a]), s = sinf(angles[a]);
            glUniform1f(uniform(program, "uAngle"), angles[a]);
            for (unsigned m = 0; m < 2; m++) {
                float mc = cosf(m * -.73f), ms = sinf(m * -.73f);
                glUniform1f(uniform(program, "maskAngle"), m * -.73f);
                for (unsigned alpha = 0; alpha < 3; alpha++) {
                    glUniform1f(uniform(program, "opacity"), alpha * .5f);
                    draw(pixels);
                    for (unsigned y = 0; y < 8; y++) {
                        for (unsigned x = 0; x < 8; x++) {
                            float rx = (2 * (x + .5f - 3.17f) + (shape >> 1)) / width;
                            float ry = (2 * (y + .5f - 4.11f) + (shape & 1)) / height;
                            float px = rx * c + ry * s, py = -rx * s + ry * c;
                            const uint8_t *p = pixels + (y * 8 + x) * 4;
                            unsigned expected[4] = {0};
                            if (fabsf(px) < 1 && fabsf(py) < 1) {
                                float u = (px + 1) * .3f + .14f;
                                float v = (py + 1) * .35f + .1f;
                                float du = u - 1.25f / 8, dv = v + .75f / 8;
                                unsigned source = (texel(v) * 4 + texel(u)) * 4;
                                unsigned mask = (texel(du * ms + dv * mc) * 4 +
                                                 texel(du * mc - dv * ms)) * 4;
                                float opacity = ((int)input[0][source + 3] -
                                                 (int)input[1][mask]) * (alpha * .5f);
                                expected[0] = input[0][source];
                                expected[1] = input[0][source + 1];
                                expected[2] = input[0][source + 2];
                                expected[3] = fmaxf(opacity, 0) + .5f;
                            }
                            for (unsigned channel = 0; channel < 4; channel++) {
                                if (abs((int)p[channel] - (int)expected[channel]) > 1) {
                                    fprintf(stderr, "animation shape=%u angle=%u mask=%u "
                                            "opacity=%u pixel=%u,%u channel=%u: %u != %u\n",
                                            shape, a, m, alpha, x, y, channel,
                                            p[channel], expected[channel]);
                                    assert(false);
                                }
                            }
                            checked++;
                        }
                    }
                }
            }
        }
    }
    assert(checked == 4608);
    glDeleteTextures(2, textures);
    glActiveTexture(GL_TEXTURE0);
    return checked;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    setenv("MHI2_SHADER_CACHE", argv[1], 1);
    ensure_window(8, 8);
    select_client(100);
    glViewport(0, 0, 8, 8);
    const float unit[] = {0, 0, 1, 0, 0, 1, 1, 1};
    const float clip[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    uint8_t pixels[8 * 8 * 4], texels[4 * 4 * 4];
    GLuint program = load_program(argv[1], "f4398fef864f13ae-7703", 1);
    attribute(program, "position", unit);
    glUniform2f(uniform(program, "uStart"), 2, 1);
    glUniform2f(uniform(program, "uEnd"), 6, 5);
    glUniform2f(uniform(program, "uDisplaySize"), 8, 8);
    glUniform4f(uniform(program, "uColor"), .25, .5, .75, .5);
    draw(pixels);
    for (unsigned y = 0; y < 8; y++) {
        for (unsigned x = 0; x < 8; x++) {
            bool inside = x >= 2 && x < 6 && y >= 3 && y < 7;
            pixel(pixels + (y * 8 + x) * 4, inside ? 64 : 0,
                  inside ? 128 : 0, inside ? 191 : 0, inside ? 128 : 0);
        }
    }
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            unsigned at = (y * 4 + x) * 4;
            texels[at] = 32 + x * 48;
            texels[at + 1] = 16 + y * 64;
            texels[at + 2] = 64;
            texels[at + 3] = 192;
        }
    }
    GLuint texture, external;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, texels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    program = load_program(argv[1], "e8aedc531a654175-7335", 2);
    attribute(program, "attrVertex", clip);
    attribute(program, "attrTex", unit);
    glUniform1i(uniform(program, "texture1"), 0);
    draw(pixels);
    for (unsigned y = 0; y < 8; y++) {
        for (unsigned x = 0; x < 8; x++) {
            pixel(pixels + (y * 8 + x) * 4, 32 + (x / 2) * 48,
                  16 + (y / 2) * 64, 64, 255);
        }
    }
    PFNEGLCREATEIMAGEKHRPROC create_image =
        (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    PFNEGLDESTROYIMAGEKHRPROC destroy_image =
        (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_texture =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
            eglGetProcAddress("glEGLImageTargetTexture2DOES");
    assert(create_image && destroy_image && image_texture);
    EGLint attributes[] = {EGL_GL_TEXTURE_LEVEL_KHR, 0,
                            EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    glFinish();
    EGLImageKHR image = create_image(display, eglGetCurrentContext(),
                                    EGL_GL_TEXTURE_2D_KHR,
                                    (EGLClientBuffer)(uintptr_t)texture,
                                    attributes);
    assert(image != EGL_NO_IMAGE_KHR && eglGetError() == EGL_SUCCESS);
    glGenTextures(1, &external);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, external);
    image_texture(GL_TEXTURE_EXTERNAL_OES, image);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    program = load_program(argv[1], "cbf0db5cc34deab4-8288", 3);
    attribute(program, "position", unit);
    glUniform4f(uniform(program, "uSize"), 4, 4, 8, 8);
    glUniform4f(uniform(program, "uSource"), 1, 0, 2, 4);
    glUniform4f(uniform(program, "uTarget"), 2, 1, 4, 6);
    glUniform1f(uniform(program, "uOpacity"), .5);
    glUniform1i(uniform(program, "uTex"), 0);
    draw(pixels);
    for (unsigned y = 0; y < 8; y++) {
        for (unsigned x = 0; x < 8; x++) {
            if (x >= 2 && x < 6 && y >= 1 && y < 7) {
                unsigned sx = 1 + (x - 2) / 2;
                unsigned sy = (13 - 2 * y) / 3;
                pixel(pixels + (y * 8 + x) * 4, 16 + sx * 24,
                      8 + sy * 32, 32, 96);
            } else {
                pixel(pixels + (y * 8 + x) * 4, 0, 0, 0, 0);
            }
        }
    }
    unsigned debug_pixels = check_debug(argv[1], unit);
    unsigned animation_pixels = check_animation(argv[1], clip);
    assert(destroy_image(display, image));
    glDeleteTextures(1, &external);
    glDeleteTextures(1, &texture);
    glUseProgram(0);
    for (unsigned id = 1; id <= 5; id++) {
        glDeleteProgram(M(id));
    }
    assert(glGetError() == GL_NO_ERROR);
    printf("PASS: five original compositor identities, reflection, rectangle/Y mapping, "
           "external EGLImages, debug integer masks/strict digit thresholds/alpha "
           "and animation rotation/masks/odd-size alignment (%u pixels)\n",
           192 + debug_pixels + animation_pixels);
    return 0;
}

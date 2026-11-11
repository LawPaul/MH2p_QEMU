/* Pixel-level checks of the exact P2865 EAL instruction transcriptions.
 * Requires a cache extracted from the user's original firmware.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define main glhost_main
#include "glhost.c"
#undef main
#include "check_program_helpers.h"

enum {
    FONT = 1, MULTICOLOR = 2, LAYER = 4, FAST = 8, ALPHA = 16,
    PREMULTIPLIED = 32, EXTERNAL = 64, SIMPLE = 128, QUICK = 256, MULTI = 512,
};
static const struct {
    const char *name, *key;
    unsigned flags;
} cases[] = {
    {"font", "e75a42bd0efd5143-8292", FONT},
    {"multicolor font", "7871e00126f31465-8590", FONT | MULTICOLOR},
    {"textured", "3841567283ca7906-8407", 0},
    {"multi texture", "c684928c5755d767-8418", MULTI},
    {"alpha texture", "470695e64a1acd48-8456", ALPHA},
    {"premultiplied texture", "1771c55aa9df3f25-8435", PREMULTIPLIED},
    {"external texture", "cac48f3da5282a93-8359", EXTERNAL},
    {"layer texture", "4bc28d6e11f1945e-8626", LAYER},
    {"alpha layer", "db2fed955b511008-8675", LAYER | ALPHA},
    {"premultiplied layer", "2005cdd7a45aed02-8626", LAYER},
    {"fast layer", "8fafaa969bedd115-8403", LAYER | FAST},
    {"external fast layer", "d645e46d50de41ae-8355", LAYER | FAST | EXTERNAL},
    {"simple", "82b2237306d187f6-6971", SIMPLE},
    {"quick node", "779e465667331d18-7337", QUICK},
};

static GLint typed_uniform(GLuint program, const char *name, GLenum expected)
{
    GLint count;
    glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &count);
    for (GLint i = 0; i < count; i++) {
        char reflected[128];
        GLint size;
        GLenum type;
        glGetActiveUniform(program, i, sizeof(reflected), NULL, &size, &type, reflected);
        if (!strcmp(name, reflected)) {
            assert(size == 1 && type == expected);
            return uniform(program, name);
        }
    }
    fprintf(stderr, "Missing reflected uniform: %s\n", name);
    abort();
}

static void attribute(GLuint program, const char *name, unsigned components,
                       const float *values)
{
    GLint location = glGetAttribLocation(program, name), count;
    assert(location >= 0);
    glGetProgramiv(program, GL_ACTIVE_ATTRIBUTES, &count);
    bool found = false;
    for (GLint i = 0; i < count; i++) {
        char reflected[128];
        GLint size;
        GLenum type;
        glGetActiveAttrib(program, i, sizeof(reflected), NULL, &size, &type, reflected);
        if (!strcmp(name, reflected)) {
            assert(size == 1 && type == GL_FLOAT_VEC2 + components - 2);
            found = true;
        }
    }
    assert(found);
    glVertexAttribPointer(location, components, GL_FLOAT, GL_FALSE, 0, values);
    glEnableVertexAttribArray(location);
}

static void expected(unsigned flags, unsigned pass, unsigned x, unsigned y,
                      const uint8_t *texels, float result[4])
{
    float u = (x + .5f) / 4, v = (y + .5f) / 4;
    float vertex[] = {.2f + .6f * u, .4f + .4f * v, .6f, .25f + .5f * u};
    float font[] = {.8f, .6f, .4f, .7f}, modulation[] = {.5f, .25f, .75f, .6f};
    float opacity = pass == 2 ? .8f : .45f;
    if (flags & SIMPLE) {
        for (unsigned i = 0; i < 4; i++) result[i] = 1;
        return;
    }
    if (flags & QUICK) {
        memcpy(result, vertex, sizeof(vertex));
        return;
    }
    unsigned sx = x, sy = y;
    if (flags & LAYER) {
        sx = pass == 2 ? 2 - x / 2 : 1 + x / 2;
        sy = 1 + y / 2;
    }
    float sample[4];
    for (unsigned i = 0; i < 4; i++) sample[i] = texels[(sy * 4 + sx) * 4 + i] / 255.f;
    if (flags & FONT) {
        if (flags & MULTICOLOR) {
            for (unsigned i = 0; i < 4; i++) font[i] *= vertex[i];
        }
        float coverage = sample[0] * font[3] * opacity;
        for (unsigned i = 0; i < 3; i++) result[i] = font[i] * coverage;
        result[3] = coverage;
    } else {
        float alpha = modulation[3] * ((flags & FAST) ? 1 : opacity);
        for (unsigned i = 0; i < 3; i++) {
            result[i] = modulation[i] * ((flags & ALPHA) ? 1 : sample[i]);
            if (flags & PREMULTIPLIED) result[i] *= alpha;
        }
        result[3] = sample[3] * alpha;
    }
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    setenv("MHI2_SHADER_CACHE", argv[1], 1);
    ensure_window(8, 8);
    select_client(100);
    glViewport(0, 0, 8, 8);
    uint8_t texels[64], pixels[256];
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            unsigned at = (y * 4 + x) * 4;
            texels[at] = 32 + x * 48;
            texels[at + 1] = 16 + y * 60;
            texels[at + 2] = 80 + x * 12 + y * 8;
            texels[at + 3] = 48 + y * 32 + x * 8;
        }
    }
    GLuint texture, external;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    PFNEGLCREATEIMAGEKHRPROC create_image = (void *)eglGetProcAddress("eglCreateImageKHR");
    PFNEGLDESTROYIMAGEKHRPROC destroy_image = (void *)eglGetProcAddress("eglDestroyImageKHR");
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_texture =
        (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    assert(create_image && destroy_image && image_texture);
    const EGLint attributes[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    EGLImageKHR image = create_image(display, eglGetCurrentContext(),
                                     EGL_GL_TEXTURE_2D_KHR,
                                     (EGLClientBuffer)(uintptr_t)texture, attributes);
    assert(image != EGL_NO_IMAGE_KHR);
    glGenTextures(1, &external);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, external);
    image_texture(GL_TEXTURE_EXTERNAL_OES, image);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    const float positions[] = {-1,-1,.25, 1,-1,.25, -1,1,.25, 1,1,.25};
    const float flat[] = {-1,-1, 1,-1, -1,1, 1,1};
    const float uv[] = {0,0, 1,0, 0,1, 1,1};
    const float colors[] = {.2,.4,.6,.25, .8,.4,.6,.75,
                            .2,.8,.6,.25, .8,.8,.6,.75};
    const float projection[] = {.5,0,0,0, 0,.5,0,0, .5,0,1,.5, -.125,-.25,0,.875};
    const float world[] = {.5,0,0,0, 0,.5,0,0, 8,16,32,64, 0,-.25,0,1};
    const float background[] = {32/255.f, 64/255.f, 96/255.f, 128/255.f};
    GLint max_attributes;
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &max_attributes);
    unsigned checked = 0;
    for (unsigned n = 0; n < sizeof(cases) / sizeof(cases[0]); n++) {
        unsigned flags = cases[n].flags, id = n + 1;
        GLuint program = load_program(argv[1], cases[n].key, id);
        assert(!blend->program[id]);
        for (GLint i = 0; i < max_attributes; i++) glDisableVertexAttribArray(i);
        bool simple = flags & (SIMPLE | QUICK);
        attribute(program, "kzPosition", simple ? 2 : 3, simple ? flat : positions);
        glUniformMatrix4fv(typed_uniform(program, simple ? "kzWorldMatrix" :
                                        "kzProjectionCameraWorldMatrix", GL_FLOAT_MAT4),
                           1, GL_FALSE, simple ? world : projection);
        if (!simple) {
            attribute(program, "kzTextureCoordinate0", 2, uv);
            glBindTexture(GL_TEXTURE_2D, flags & EXTERNAL ? 0 : texture);
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, flags & EXTERNAL ? external : 0);
            glUniform1i(typed_uniform(program, flags & (FONT | LAYER | MULTI) ?
                                     "LayerRenderTexture" : "Texture",
                                     flags & EXTERNAL ? GL_SAMPLER_EXTERNAL_OES :
                                     GL_SAMPLER_2D), 0);
            if (flags & FONT) {
                glUniform4f(typed_uniform(program, "ealFontColor", GL_FLOAT_VEC4),
                             .8,.6,.4,.7);
            } else {
                glUniform4f(typed_uniform(program, "ealModulateColor", GL_FLOAT_VEC4),
                             .5,.25,.75,.6);
            }
        }
        if (flags & (MULTICOLOR | QUICK)) attribute(program, "kzColor0", 4, colors);
        for (unsigned pass = 0; pass < 3; pass++) {
            if (!simple && !(flags & FAST)) {
                glUniform1f(typed_uniform(program, flags & LAYER ? "LayerRenderOpacity" :
                                         "BlendIntensity", GL_FLOAT), pass == 2 ? .8 : .45);
            }
            if (flags & LAYER) {
                glUniform4f(typed_uniform(program, "ealAtlasCoords", GL_FLOAT_VEC4),
                             pass == 2 ? .75 : .25, .25, pass == 2 ? -.5 : .5, .5);
            }
            if (pass) glEnable(GL_BLEND); else glDisable(GL_BLEND);
            glBlendEquation(GL_FUNC_ADD);
            glBlendFuncSeparate(pass == 2 ? GL_SRC_ALPHA : GL_ONE,
                                 GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            glClearColor(.125,.25,.375,.5);
            glClear(GL_COLOR_BUFFER_BIT);
            struct BlendState saved;
            begin_binary_blend(blend->program[id], &saved);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            end_binary_blend(blend->program[id], &saved);
            assert(glIsEnabled(GL_BLEND) == (pass != 0));
            glReadPixels(0, 0, 8, 8, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
            assert(glGetError() == GL_NO_ERROR);
            for (unsigned y = 0; y < 8; y++) {
                for (unsigned x = 0; x < 8; x++) {
                    float want[4];
                    memcpy(want, background, sizeof(want));
                    if (x >= 2 && x < 6 && y >= 1 && y < 5) {
                        expected(flags, pass, x - 2, y - 1, texels, want);
                        if (pass) {
                            float alpha = want[3];
                            for (unsigned c = 0; c < 4; c++) {
                                want[c] = want[c] * (pass == 2 && c < 3 ? alpha : 1) +
                                          background[c] * (1 - alpha);
                            }
                        }
                    }
                    for (unsigned c = 0; c < 4; c++) {
                        int byte = (want[c] > 1 ? 1 : want[c]) * 255 + .5f;
                        int actual = pixels[(y * 8 + x) * 4 + c];
                        if (abs(actual - byte) > 1) {
                            fprintf(stderr, "%s pass %u (%u,%u) channel %u: %d != %d\n",
                                    cases[n].name, pass, x, y, c, actual, byte);
                            abort();
                        }
                    }
                    checked++;
                }
            }
        }
        glUseProgram(0);
        glDeleteProgram(program);
    }
    assert(destroy_image(display, image));
    glDeleteTextures(1, &external);
    glDeleteTextures(1, &texture);
    assert(glGetError() == GL_NO_ERROR && checked == 2688);
    printf("PASS: 14 exact EAL programs, reflected inputs, matrix/atlas transforms, "
           "red-channel fonts, alpha/premultiplication, external images and "
           "application blending (%u pixels)\n", checked);
    return 0;
}

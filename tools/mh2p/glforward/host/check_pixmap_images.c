/* Real EGLImage sibling storage, native transfers and state preservation.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define main glhost_main
#include "glhost.c"
#undef main
#include <assert.h>

static void check_fast_readback_binding(void)
{
    GLuint texture, framebuffer, vertex, pack;
    GLint binding;
    uint8_t pixels[128 * 128 * 4];

    assert(strstr((const char *)glGetString(GL_VERSION), "OpenGL ES 3."));
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 128, 128, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, texture, 0);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    glClearColor(1, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glGenBuffers(1, &vertex);
    glBindBuffer(GL_ARRAY_BUFFER, vertex);
    glBufferData(GL_ARRAY_BUFFER, 16, NULL, GL_STATIC_DRAW);
    glBindBuffer(0x88eb, 0);
    assert(glGetError() == GL_NO_ERROR);

    fast_read_pixels(0, 0, 128, 128, pixels);
    assert(glGetError() == GL_NO_ERROR);
    glGetIntegerv(0x88ed, &binding);
    assert(binding == 0);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &binding);
    assert(binding == vertex);
    for (unsigned i = 0; i < sizeof(pixels); i += 4) {
        assert(pixels[i] == 255 && !pixels[i + 1] &&
               pixels[i + 2] == 255 && pixels[i + 3] == 255);
    }
    glGenBuffers(1, &pack);
    glBindBuffer(0x88eb, pack);
    glBufferData(0x88eb, 16, NULL, GL_STATIC_DRAW);
    fast_read_pixels(0, 0, 128, 128, pixels);
    assert(glGetError() == GL_NO_ERROR);
    glGetIntegerv(0x88ed, &binding);
    assert(binding == pack);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &binding);
    assert(binding == vertex);
    glBindBuffer(0x88eb, 0);
    fast_read_pixels(0, 0, 1, 1, pixels);
    assert(glGetError() == GL_NO_ERROR);
    assert(pixels[0] == 255 && !pixels[1] && pixels[2] == 255 && pixels[3] == 255);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &texture);
    glDeleteBuffers(1, &vertex);
    glDeleteBuffers(1, &pack);
}

int main(void)
{
    ensure_window(4, 4);
    select_client(100);
    printf("Framebuffer origin control: %s\n",
           strstr((const char *)glGetString(GL_EXTENSIONS),
                  "GL_MESA_framebuffer_flip_y") ? "available" : "unavailable");
    struct Client *client = get_client(100);
    uint32_t packet[21] = {0, 7, 0, 4, 4};
    uint8_t *source = (uint8_t *)(packet + 5), pixels[64];
    for (unsigned i = 0; i < 64; i++) source[i] = i;
    assert(pixmap_record(145, sizeof(packet), (uint8_t *)packet));
    assert(!pixmap_record(145, sizeof(packet) - 1, (uint8_t *)packet));
    GLuint textures[3], pbo, framebuffer;
    glGenTextures(3, textures);
    glGenBuffers(1, &pbo);
    glBindBuffer(0x88ec, pbo);
    glBufferData(0x88ec, 256, NULL, GL_STATIC_DRAW);
    glBindTexture(GL_TEXTURE_2D, textures[0]);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
    glPixelStorei(0x0cf2, 7);
    glPixelStorei(0x0cf3, 2);
    glPixelStorei(0x0cf4, 1);
    glEnable(0);
    assert(pixmap_control(client, (uint8_t *)packet) == GL_NO_ERROR);
    assert(client_gl_error(client) == GL_INVALID_ENUM);
    assert(client_gl_error(client) == GL_NO_ERROR);
    GLint value;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &value);
    assert(value == textures[0]);
    glGetIntegerv(0x88ef, &value);
    assert(value == pbo);
    glGetIntegerv(0x0cf2, &value);
    assert(value == 7);
    glGetIntegerv(0x0cf3, &value);
    assert(value == 2);
    glGetIntegerv(0x0cf4, &value);
    assert(value == 1);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &value);
    assert(value == 8);
    assert(pixmap_control(client, (uint8_t *)packet) == GL_INVALID_OPERATION);
    packet[0] = 1;
    packet[2] = GL_TEXTURE_2D;
    assert(pixmap_control(client, (uint8_t *)packet) == GL_NO_ERROR);
    glBindTexture(GL_TEXTURE_2D, textures[1]);
    assert(pixmap_control(client, (uint8_t *)packet) == GL_NO_ERROR);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, textures[2]);
    packet[2] = GL_TEXTURE_EXTERNAL_OES;
    assert(pixmap_control(client, (uint8_t *)packet) == GL_NO_ERROR);

    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glClear(GL_COLOR_BUFFER_BIT);
    packet[2] = GL_TEXTURE_2D;
    assert(pixmap_control(client, (uint8_t *)packet) == GL_NO_ERROR);
    assert(client_gl_error(client) == GL_INVALID_FRAMEBUFFER_OPERATION);
    assert(client_gl_error(client) == GL_NO_ERROR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, textures[0], 0);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    glClearColor(1, .5, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindBuffer(0x88eb, pbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 8);
    glPixelStorei(0x0d02, 7);
    glPixelStorei(0x0d03, 2);
    glPixelStorei(0x0d04, 1);
    packet[0] = 4;
    packet[2] = 0;
    assert(pixmap_record(146, 20, (uint8_t *)packet));
    assert(pixmap_read(client, (uint8_t *)packet, pixels) == GL_NO_ERROR);
    for (unsigned i = 0; i < 64; i += 4) {
        assert(pixels[i] == 255 && pixels[i + 1] == 128 &&
               !pixels[i + 2] && pixels[i + 3] == 255);
    }
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &value);
    assert(value == framebuffer);
    glGetIntegerv(0x88ed, &value);
    assert(value == pbo);
    glGetIntegerv(0x0d02, &value);
    assert(value == 7);
    glGetIntegerv(0x0d03, &value);
    assert(value == 2);
    glGetIntegerv(0x0d04, &value);
    assert(value == 1);
    glGetIntegerv(GL_PACK_ALIGNMENT, &value);
    assert(value == 8);
    packet[0] = 2;
    assert(pixmap_control(client, (uint8_t *)packet) == GL_NO_ERROR);
    glBindBuffer(0x88eb, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(0x0d02, 0);
    glPixelStorei(0x0d03, 0);
    glPixelStorei(0x0d04, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, textures[1], 0);
    glReadPixels(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    assert(!memcmp(pixels, source, sizeof(pixels)));
    assert(pixmap_framebuffer_origin(client, GL_FRAMEBUFFER, true) == GL_NO_ERROR);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 3, 4, 1);
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    packet[0] = 4;
    assert(pixmap_read(client, (uint8_t *)packet, pixels) == GL_NO_ERROR);
    for (unsigned i = 0; i < 16; i += 4) {
        assert(pixels[i] == 255 && !pixels[i + 1] &&
               !pixels[i + 2] && pixels[i + 3] == 255);
    }
    assert(!memcmp(pixels + 16, source + 16, 48));
    assert(pixmap_framebuffer_origin(client, GL_FRAMEBUFFER, false) == GL_NO_ERROR);
    packet[0] = 2;
    assert(pixmap_control(client, (uint8_t *)packet) == GL_NO_ERROR);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, 1, 1);
    packet[0] = 5;
    assert(pixmap_record(145, 20, (uint8_t *)packet));
    assert(pixmap_control(client, (uint8_t *)packet) == GL_NO_ERROR);
    assert(glIsEnabled(GL_SCISSOR_TEST));
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &value);
    assert(value == framebuffer);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glReadPixels(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    for (unsigned y = 0; y < 4; y++) {
        assert(!memcmp(pixels + y * 16, source + (3 - y) * 16, 16));
    }
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 3, 2, 1);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    packet[0] = 6;
    assert(pixmap_control(client, (uint8_t *)packet) == GL_NO_ERROR);
    assert(glIsEnabled(GL_SCISSOR_TEST));
    packet[0] = 4;
    assert(pixmap_read(client, (uint8_t *)packet, pixels) == GL_NO_ERROR);
    for (unsigned i = 0; i < 8; i += 4) {
        assert(!pixels[i] && !pixels[i + 1] &&
               pixels[i + 2] == 255 && pixels[i + 3] == 255);
    }
    assert(!memcmp(pixels + 8, source + 8, 56));
    packet[0] = 2;
    assert(pixmap_control(client, (uint8_t *)packet) == GL_NO_ERROR);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    select_client(200);
    packet[0] = 0;
    memset(source, 123, 64);
    assert(pixmap_control(get_client(200), (uint8_t *)packet) == GL_NO_ERROR);
    packet[0] = 3;
    assert(pixmap_control(get_client(200), (uint8_t *)packet) == GL_NO_ERROR);
    assert(!get_client(200)->images);
    select_client(100);
    packet[0] = 4;
    assert(pixmap_read(client, (uint8_t *)packet, pixels) == GL_NO_ERROR);
    for (unsigned i = 0; i < 64; i++) assert(pixels[i] == i);
    packet[0] = 3;
    assert(pixmap_control(client, (uint8_t *)packet) == GL_NO_ERROR);
    assert(!client->images);
    assert(pixmap_control(client, (uint8_t *)packet) == GL_INVALID_OPERATION);
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(3, textures);
    glDeleteBuffers(1, &pbo);
    assert(glGetError() == GL_NO_ERROR);
    check_fast_readback_binding();
    puts("PASS: real pixmap siblings, upload/render/readback, pixel-store/PBO "
         "preservation, process isolation and cleanup");
    return 0;
}

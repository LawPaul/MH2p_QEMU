/* Real Mesa storage shared by imported native pixmap texture siblings.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef GLHOST_PIXMAP_IMAGES_H
#define GLHOST_PIXMAP_IMAGES_H
#include <GLES2/gl2ext.h>

static void client_save_errors(struct Client *client)
{
    GLenum error;
    while ((error = glGetError()) != GL_NO_ERROR) {
        if (error < 0x500 || error > 0x507) {
            fprintf(stderr, "glhost: unexpected GL error %x\n", error);
            exit(1);
        }
        client->saved_errors |= 1u << (error - 0x500);
    }
}

static GLenum client_gl_error(struct Client *client)
{
    client_save_errors(client);
    for (unsigned i = 0; i < 8; i++) {
        if (client->saved_errors & (1u << i)) {
            client->saved_errors &= ~(1u << i);
            return 0x500 + i;
        }
    }
    return GL_NO_ERROR;
}

struct PixmapImage {
    struct PixmapImage *next;
    uint32_t id, width, height;
    GLuint texture;
    EGLImageKHR image;
};

static GLenum pixmap_framebuffer_origin(struct Client *client, GLenum target,
                                        bool native)
{
    client_save_errors(client);
    void (*parameter)(GLenum, GLenum, GLint) =
        (void *)eglGetProcAddress("glFramebufferParameteriMESA");
    if (!parameter || !strstr((const char *)glGetString(GL_EXTENSIONS),
                              "GL_MESA_framebuffer_flip_y")) {
        fprintf(stderr, "glhost: native framebuffer origin control unavailable\n");
        return GL_INVALID_OPERATION;
    }
    parameter(target, 0x8bbb, native);
    return glGetError();
}

static struct PixmapImage *pixmap_image(struct Client *client, uint32_t id)
{
    for (struct PixmapImage *image = client->images; image; image = image->next) {
        if (image->id == id) {
            return image;
        }
    }
    return NULL;
}

/* A pixel pack buffer lets the driver blit into cached memory; a direct
 * glReadPixels into client memory is roughly twice as slow on Mesa iris. */
static struct {EGLContext context;GLuint buffer;size_t size;} pack_buffers[16];
static void *(*map_range)(GLenum,GLintptr,GLsizeiptr,GLbitfield);
static GLboolean (*unmap_buffer)(GLenum);
static void fast_read_pixels(GLint x,GLint y,GLsizei w,GLsizei h,void *dst)
{
    size_t size=(size_t)w*h*4;
    EGLContext context=eglGetCurrentContext();
    const char *version=(const char *)glGetString(GL_VERSION);
    GLint bound=0;
    unsigned slot=0;
    if(!map_range){
        map_range=(void *(*)(GLenum,GLintptr,GLsizeiptr,GLbitfield))eglGetProcAddress("glMapBufferRange");
        unmap_buffer=(GLboolean (*)(GLenum))eglGetProcAddress("glUnmapBuffer");
    }
    if(size<65536||!map_range||!unmap_buffer||!version||!strstr(version,"OpenGL ES 3.")){
        glReadPixels(x,y,w,h,GL_RGBA,GL_UNSIGNED_BYTE,dst);
        return;
    }
    for(;slot<16&&pack_buffers[slot].context&&pack_buffers[slot].context!=context;slot++);
    if(slot==16){glReadPixels(x,y,w,h,GL_RGBA,GL_UNSIGNED_BYTE,dst);return;}
    glGetIntegerv(0x88ed,&bound);
    if(!pack_buffers[slot].buffer){
        pack_buffers[slot].context=context;
        glGenBuffers(1,&pack_buffers[slot].buffer);
    }
    glBindBuffer(0x88eb,pack_buffers[slot].buffer);
    if(pack_buffers[slot].size<size){
        glBufferData(0x88eb,size,NULL,0x88e1);
        pack_buffers[slot].size=size;
    }
    glReadPixels(x,y,w,h,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    void *mapped=map_range(0x88eb,0,size,1);
    if(mapped){memcpy(dst,mapped,size);unmap_buffer(0x88eb);}
    glBindBuffer(0x88eb,bound);
    if(!mapped)glReadPixels(x,y,w,h,GL_RGBA,GL_UNSIGNED_BYTE,dst);
}

static bool pixmap_record(uint32_t op, uint32_t len, const uint8_t *p)
{
    if (len < 20 || !u32(p, 4)) {
        return false;
    }
    uint32_t action = u32(p, 0), w = u32(p, 12), h = u32(p, 16);
    if (!w || w > 2048 || !h || h > 2048) {
        return false;
    }
    if (op == 146) {
        return len == 20 && action == 4 && !u32(p, 8);
    }
    if (action == 5 || action == 6) {
        return len == 20 && !u32(p, 8);
    }
    return action <= 3 &&
           len == 20 + ((action == 0 || action == 2) ? w * h * 4 : 0) &&
           (action == 1 ? (u32(p, 8) == GL_TEXTURE_2D ||
                           u32(p, 8) == GL_TEXTURE_EXTERNAL_OES) :
            !u32(p, 8));
}

/* Internal transfers must not inherit the application's pixel-store/PBO state. */
static void pixmap_transfer_state(bool unpack, bool restore, GLint state[5])
{
    const GLenum names[2][4] = {
        {GL_PACK_ALIGNMENT, 0x0d02, 0x0d03, 0x0d04},
        {GL_UNPACK_ALIGNMENT, 0x0cf2, 0x0cf3, 0x0cf4},
    };
    bool es3 = strstr((const char *)glGetString(GL_VERSION), "OpenGL ES 3.") != NULL;
    for (unsigned i = 0; i < (es3 ? 4 : 1); i++) {
        if (!restore) {
            glGetIntegerv(names[unpack][i], &state[i]);
        }
        glPixelStorei(names[unpack][i], restore ? state[i] : i ? 0 : 1);
    }
    if (es3) {
        if (!restore) {
            glGetIntegerv(unpack ? 0x88ef : 0x88ed, &state[4]);
        }
        glBindBuffer(unpack ? 0x88ec : 0x88eb, restore ? state[4] : 0);
    }
}

static GLenum pixmap_surface_transfer(struct Client *client,
                                      struct PixmapImage *image, bool publish)
{
    if ((client->managed && (!client->draw ||
         client->draw->width != image->width ||
         client->draw->height != image->height)) ||
        !strstr((const char *)glGetString(GL_VERSION), "OpenGL ES 3.")) {
        return GL_INVALID_OPERATION;
    }
    void (*blit)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint,
                  GLbitfield, GLenum) = (void *)eglGetProcAddress("glBlitFramebuffer");
    if (!blit) return GL_INVALID_OPERATION;
    GLint draw, read;
    GLuint framebuffer;
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &draw);
    glGetIntegerv(0x8caa, &read);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, image->texture, 0);
    glBindFramebuffer(publish ? 0x8ca8 : 0x8ca9, 0);
    GLenum error = GL_INVALID_FRAMEBUFFER_OPERATION;
    if (glCheckFramebufferStatus(0x8ca8) == GL_FRAMEBUFFER_COMPLETE &&
        glCheckFramebufferStatus(0x8ca9) == GL_FRAMEBUFFER_COMPLETE) {
        glDisable(GL_SCISSOR_TEST);
        blit(0, 0, image->width, image->height,
             0, image->height, image->width, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        error = glGetError();
    }
    if (scissor) glEnable(GL_SCISSOR_TEST);
    glBindFramebuffer(0x8ca9, draw);
    glBindFramebuffer(0x8ca8, read);
    glDeleteFramebuffers(1, &framebuffer);
    return error;
}

static uint32_t pixmap_control(struct Client *client, const uint8_t *p)
{
    client_save_errors(client);
    uint32_t action = u32(p, 0), id = u32(p, 4), target = u32(p, 8);
    uint32_t width = u32(p, 12), height = u32(p, 16);
    struct PixmapImage *image = pixmap_image(client, id);
    PFNEGLCREATEIMAGEKHRPROC create_image =
        (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    PFNEGLDESTROYIMAGEKHRPROC destroy_image =
        (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_texture =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
            eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!create_image || !destroy_image || !image_texture ||
        (action == 0 ? image != NULL : image == NULL) ||
        (image && (image->width != width || image->height != height))) {
        return GL_INVALID_OPERATION;
    }
    if (action == 5 || action == 6) {
        return pixmap_surface_transfer(client, image, action == 6);
    }
    if (action == 1) {
        GLint bound = 0;
        glGetIntegerv(target == GL_TEXTURE_2D ? GL_TEXTURE_BINDING_2D :
                      GL_TEXTURE_BINDING_EXTERNAL_OES, &bound);
        if (!bound) {
            return GL_INVALID_OPERATION;
        }
        image_texture(target, image->image);
        return glGetError();
    }
    if (action == 3) {
        if (!destroy_image(display, image->image)) {
            fprintf(stderr, "glhost: destroying pixmap EGLImage failed: %x\n",
                    eglGetError());
            return GL_INVALID_OPERATION;
        }
        struct PixmapImage **link = &client->images;
        while (*link != image) {
            link = &(*link)->next;
        }
        *link = image->next;
        glDeleteTextures(1, &image->texture);
        free(image);
        return glGetError();
    }
    if (action == 0) {
        unsigned count = 0;
        for (struct PixmapImage *entry = client->images; entry; entry = entry->next) {
            count++;
        }
        if (count >= 64 || !(image = calloc(1, sizeof(*image)))) {
            return GL_OUT_OF_MEMORY;
        }
        image->id = id;
        image->width = width;
        image->height = height;
        glGenTextures(1, &image->texture);
    }
    GLint bound, state[5];
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    pixmap_transfer_state(true, false, state);
    glBindTexture(GL_TEXTURE_2D, image->texture);
    if (action == 0) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, p + 20);
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height,
                        GL_RGBA, GL_UNSIGNED_BYTE, p + 20);
    }
    uint32_t error = glGetError();
    if (!error && action == 0) {
        const EGLint attributes[] = {
            EGL_GL_TEXTURE_LEVEL_KHR, 0, EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE,
        };
        image->image = create_image(display, eglGetCurrentContext(),
                                    EGL_GL_TEXTURE_2D_KHR,
                                    (EGLClientBuffer)(uintptr_t)image->texture,
                                    attributes);
        if (image->image == EGL_NO_IMAGE_KHR) {
            fprintf(stderr, "glhost: creating pixmap EGLImage failed: %x\n",
                    eglGetError());
            error = GL_INVALID_OPERATION;
        }
    }
    glBindTexture(GL_TEXTURE_2D, bound);
    pixmap_transfer_state(true, true, state);
    if (action == 0) {
        if (error) {
            glDeleteTextures(1, &image->texture);
            free(image);
        } else {
            image->next = client->images;
            client->images = image;
        }
    }
    return error;
}

static uint32_t pixmap_read(struct Client *client, const uint8_t *p, void *pixels)
{
    client_save_errors(client);
    struct PixmapImage *image = pixmap_image(client, u32(p, 4));
    if (!image || image->width != u32(p, 12) || image->height != u32(p, 16)) {
        return GL_INVALID_OPERATION;
    }
    GLuint framebuffer;
    GLint draw, read, state[5];
    bool es3 = strstr((const char *)glGetString(GL_VERSION), "OpenGL ES 3.") != NULL;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &draw);
    if (es3) {
        glGetIntegerv(0x8caa, &read);
    }
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, image->texture, 0);
    uint32_t error = GL_INVALID_FRAMEBUFFER_OPERATION;
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        pixmap_transfer_state(false, false, state);
        fast_read_pixels(0, 0, image->width, image->height, pixels);
        error = glGetError();
        pixmap_transfer_state(false, true, state);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, draw);
    if (es3) {
        glBindFramebuffer(0x8ca8, read);
    }
    glDeleteFramebuffers(1, &framebuffer);
    if (!error && getenv("MHI2_GL_DUMP_CLIENTS")) {
        size_t bytes = (size_t)image->width * image->height * 3;
        uint8_t *rgb = malloc(bytes);
        if (!rgb) {
            fprintf(stderr, "glhost: pixmap diagnostic allocation failed\n");
            return error;
        }
        for (size_t i = 0; i < bytes / 3; i++) {
            memcpy(rgb + i * 3, (uint8_t *)pixels + i * 4, 3);
        }
        char path[4096], temporary[4096];
        int length = snprintf(path, sizeof(path), "%s.pixmap-%u.ppm",
                               frame_path(), client->pid);
        int temp_length = snprintf(temporary, sizeof(temporary), "%s.tmp", path);
        if (length < 0 || (size_t)length >= sizeof(path) ||
            temp_length < 0 || (size_t)temp_length >= sizeof(temporary)) {
            fprintf(stderr, "glhost: pixmap diagnostic path too long\n");
        } else {
            FILE *file = fopen(temporary, "wb");
            bool ok = file && fprintf(file, "P6\n%u %u\n255\n",
                                      image->width, image->height) > 0;
            if (ok) ok = fwrite(rgb, 1, bytes, file) == bytes;
            if (file && fclose(file)) ok = false;
            if (!ok || rename(temporary, path)) {
                fprintf(stderr, "glhost: writing pixmap diagnostic failed\n");
            }
        }
        free(rgb);
    }
    return error;
}
#endif

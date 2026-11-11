/* Bounded native Screen pixmap ownership and real Mesa image siblings.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef MH2P_SCREEN_IMAGE_H
#define MH2P_SCREEN_IMAGE_H

static struct {
    unsigned users;
    u32 device;
    int (*pixmap)(void *, int, void **);
    int (*buffer)(void *, int, void **);
    int (*acquire)(struct ScreenSurface *, void *);
    void (*release)(struct ScreenSurface *, void *);
    int (*open)(u32 *, u32);
    void (*close)(u32);
    u32 (*size)(u32);
    u32 (*id)(u32);
    int (*retain)(u32, u32 *);
    void (*free)(u32);
    void (*read)(struct ScreenSurface *, int, int, int, int, void *);
    void (*write)(struct ScreenSurface *, int, int, int, int, const void *);
} screen_image_api;

struct ScreenImage {
    struct ScreenImage *next;
    struct ScreenWindow *drawable;
    struct ScreenSurface surface;
    u32 id;
    unsigned long context;
    unsigned refs, bytes;
    int public, hosted, dirty, published, resync, stale;
    unsigned char *pixels, *cached, *reply;
    /* surface.memory is already retained for the image's lifetime. */
    struct ScreenSurfaceRuns dma_runs;
};
static struct ScreenImage *screen_images;
#define screen_image_textures (current_screen_image_state()->textures)
#define screen_image_fbos (current_screen_image_state()->fbos)
#define screen_image_unit (current_screen_image_state()->unit)
#define screen_image_bound (current_screen_image_state()->bound)
#define screen_image_draw_fbo (current_screen_image_state()->draw_fbo)
#define screen_image_read_fbo (current_screen_image_state()->read_fbo)
static int screen_image_sync(void);

static int screen_image_api_release(void)
{
    if (--screen_image_api.users) {
        return 1;
    }
    screen_image_api.close(screen_image_api.device);
    return screen_libraries_release();
}

static int screen_image_api_acquire(void)
{
    if (screen_image_api.users) {
        screen_image_api.users++;
        return 1;
    }
    if (!screen_libraries_acquire()) {
        return 0;
    }
#define IMAGE_SYMBOL(lib, field, symbol) do { \
    screen_image_api.field = dlsym(screen_libraries.lib, symbol); \
    if (!screen_image_api.field) { \
        screen_libraries_release(); \
        return screen_failure("missing " symbol, 0x3003); \
    } \
} while (0)
    IMAGE_SYMBOL(screen, pixmap, "screen_get_pixmap_property_pv");
    IMAGE_SYMBOL(screen, buffer, "screen_get_buffer_property_pv");
    IMAGE_SYMBOL(nvutil, acquire, "qnxnv_acquire_image_surfaces");
    IMAGE_SYMBOL(nvutil, release, "qnxnv_release_image_surfaces");
    IMAGE_SYMBOL(rm, open, "NvRmOpen");
    IMAGE_SYMBOL(rm, close, "NvRmClose");
    IMAGE_SYMBOL(rm, size, "NvRmMemGetSize");
    IMAGE_SYMBOL(rm, id, "NvRmMemGetId");
    IMAGE_SYMBOL(rm, retain, "NvRmMemHandleFromId");
    IMAGE_SYMBOL(rm, free, "NvRmMemHandleFree");
    IMAGE_SYMBOL(rm, read, "NvRmSurfaceRead");
    IMAGE_SYMBOL(rm, write, "NvRmSurfaceWrite");
#undef IMAGE_SYMBOL
    if (screen_image_api.open(&screen_image_api.device, 0)) {
        screen_libraries_release();
        return screen_failure("pixmap NvRmOpen failed", 0x3003);
    }
    screen_image_api.users = 1;
    return 1;
}

static int screen_image_command(struct ScreenImage *image, u32 action, u32 target)
{
    unsigned long context = (unsigned long)current_record()->egl_context;
    if (!context || (image->hosted && image->context != context)) {
        screen_gl_failure("native image belongs to a different context", 0x502);
        return 0;
    }
    u32 header[] = {action, image->id, target,
                    image->surface.width, image->surface.height};
    unsigned bytes = (action == 0 || action == 2) ? image->bytes : 0;
    u32 error;
    rec(145, sizeof(header) + bytes);
    PART(header, sizeof(header));
    if (bytes) {
        PART(image->pixels, bytes);
    }
    if (recv_all(&error, 4)) {
        screen_gl_failure("pixmap transport failed", 0x502);
        return 0;
    }
    if (error) {
        screen_gl_failure("host pixmap operation failed", error);
        return 0;
    }
    image->context = context;
    return 1;
}

static int screen_image_drop(struct ScreenImage *image)
{
    if (image->hosted && !current_record()->egl_context) {
        return screen_failure("pixmap cleanup requires its current context", 0x3002);
    }
    if (image->dirty && !screen_image_sync()) return 0;
    if (image->hosted && !screen_image_command(image, 3, 0)) {
        return screen_failure("host pixmap cleanup failed", 0x3002);
    }
    struct ScreenImage **link = &screen_images;
    while (*link != image) {
        link = &(*link)->next;
    }
    *link = image->next;
    screen_image_api.free(image->surface.memory);
    free(image->dma_runs.runs);
    free(image->pixels);
    free(image->cached);
    free(image->reply);
    free(image);
    return screen_image_api_release();
}

static void *screen_image_create(void *display, void *context, u32 target,
                                  void *pixmap, const i32 *attributes)
{
    if (!screen_display(display)) {
        return 0;
    }
    if (context || target != 0x30b0 || !pixmap) {
        screen_failure("unsupported native pixmap import", 0x300c);
        return 0;
    }
    if (attributes) {
        unsigned i;
        for (i = 0; i < 32 && attributes[i] != 0x3038; i += 2) {
            if (attributes[i] != 0x30d2 ||
                (attributes[i + 1] != 0 && attributes[i + 1] != 1)) {
                screen_failure("unsupported pixmap image attribute", 0x3004);
                return 0;
            }
        }
        if (i == 32) {
            screen_failure("unterminated pixmap image attributes", 0x3004);
            return 0;
        }
    }
    if (screen_image_api.users >= 64 || !screen_image_api_acquire()) {
        screen_failure("native pixmap resource limit/allocation failure", 0x3003);
        return 0;
    }
    struct ScreenSurface surface = {0};
    void *buffer = 0, *native = 0;
    u32 memory = 0;
    int acquired = 0;
    struct ScreenImage *image = 0;
    if (screen_image_api.pixmap(pixmap, 37, &buffer) || !buffer ||
        screen_image_api.buffer(buffer, 113, &native) || !native ||
        screen_image_api.acquire(&surface, native)) {
        screen_failure("acquiring native pixmap buffer failed", 0x300c);
        goto fail;
    }
    acquired = 1;
    if (!surface.width || surface.width > 2048 ||
        !surface.height || surface.height > 2048 ||
        surface.width * surface.height * 4 > 16 * 1024 * 1024 - 20 ||
        surface.format != 0x20105321 ||
        (surface.layout != 1 && surface.layout != 3) ||
        surface.pitch < surface.width * 4 || surface.pitch > 16384 ||
        surface.offset > screen_image_api.size(surface.memory) ||
        surface.height > (screen_image_api.size(surface.memory) -
                          surface.offset) / surface.pitch) {
        screen_failure("unsupported native pixmap surface", 0x3009);
        goto fail;
    }
    if (screen_image_api.retain(screen_image_api.id(surface.memory), &memory)) {
        screen_failure("retaining native pixmap storage failed", 0x3003);
        goto fail;
    }
    image = malloc(sizeof(*image));
    if (!image) {
        screen_failure("pixmap allocation failed", 0x3003);
        goto fail;
    }
    memset(image, 0, sizeof(*image));
    image->surface = surface;
    image->surface.memory = memory;
    image->bytes = surface.width * surface.height * 4;
    image->pixels = malloc(image->bytes);
    image->cached = malloc(image->bytes);
    image->reply = malloc(image->bytes + 4);
    if (!image->pixels || !image->cached || !image->reply) {
        screen_failure("pixmap pixel allocation failed", 0x3003);
        goto fail;
    }
    image->id = __sync_add_and_fetch(&g_id, 1);
    image->public = 1;
    image->next = screen_images;
    screen_images = image;
    screen_image_api.release(&surface, native);
    return image;
fail:
    if (acquired) screen_image_api.release(&surface, native);
    if (memory) screen_image_api.free(memory);
    if (image) {
        free(image->pixels);
        free(image->cached);
        free(image->reply);
        free(image);
    }
    screen_image_api_release();
    return 0;
}

static struct ScreenImage *screen_texture_image(u32 texture)
{
    if (!texture) {
        return 0;
    }
    for (unsigned i = 0; i < 256; i++) {
        if (screen_image_textures[i].texture == texture) {
            return screen_image_textures[i].image;
        }
    }
    return 0;
}

/*
 * Let the bridge device read, compare and upload the native pixmap itself.
 * Returns 1 when done, 0 on failure and -1 to use the in-guest copy path.
 */
static int screen_image_device_refresh(struct ScreenImage *image)
{
    const char *fast = getenv("GLBRIDGE_FAST_SURFACES");
    const char *verify = getenv("GLBRIDGE_VERIFY_SURFACES");
    unsigned long context = (unsigned long)current_record()->egl_context;
    struct ScreenSurface *s = &image->surface;

    if (!fast || strcmp(fast, "1") || (verify && !strcmp(verify, "1")) ||
        !context || (image->hosted && image->context != context)) {
        return -1;
    }
    /* key_lo, key_hi, header bytes, record framing, action header */
    u32 extra[3 + 8 + 5];
    unsigned prefix = record_header(current_record(), 145,
                                    20 + image->bytes, extra + 3);
    u32 *action = extra + 3 + prefix / 4;
    action[0] = image->hosted ? 2 : 0;
    action[1] = image->id;
    action[2] = 0;
    action[3] = s->width;
    action[4] = s->height;
    extra[0] = getpid();
    extra[1] = image->id;
    extra[2] = prefix + 20;
    unsigned length = 0;
    u32 *d = screen_surface_dma_descriptor(s, &image->dma_runs,
                                           SCREEN_SURFACE_REFRESH,
                                           image->hosted && !image->resync ?
                                           0 : 2, extra,
                                           3 + extra[2] / 4, &length);
    if (!d) {
        return -1;
    }
    while (*(volatile u32 *)(bridge + 16)) {
        usleep(1000);
    }
    memcpy(bulk, d, length);
    free(d);
    __sync_synchronize();
    *(volatile u32 *)(bridge + 48) = length;
    u32 status = *(volatile u32 *)(bridge + 44), error;
    if (status == 0) {
        if (recv_all(&error, 4)) {
            screen_gl_failure("pixmap transport failed", 0x502);
            return 0;
        }
        if (error) {
            screen_gl_failure("host pixmap operation failed", error);
            return 0;
        }
        image->context = context;
        image->hosted = 1;
        image->resync = 0;
        return 1;
    }
    unlock_record();
    return status == 3 && image->hosted ? 1 : -1;
}

static int screen_image_refresh(struct ScreenImage *image)
{
    if (image->dirty) return 1;
    int direct = screen_image_device_refresh(image);
    if (direct >= 0) return direct;
    struct ScreenSurface *s = &image->surface;
    if (!screen_surface_transfer(s, image->pixels, 0,
                                 screen_image_api.read, screen_image_api.write)) {
        return 0;
    }
    if (!image->hosted || image->stale ||
        memcmp(image->pixels, image->cached, image->bytes)) {
        if (!screen_image_command(image, image->hosted ? 2 : 0, 0)) return 0;
        image->hosted = 1;
        image->stale = 0;
        /* The device's comparison copy no longer matches the host texture. */
        image->resync = 1;
        memcpy(image->cached, image->pixels, image->bytes);
    }
    return 1;
}

static int screen_image_sync(void)
{
    for (struct ScreenImage *image = screen_images; image; image = image->next) {
        if (!image->dirty ||
            image->context != (unsigned long)current_record()->egl_context)
            continue;
        struct ScreenSurface *s = &image->surface;
        if (image->drawable &&
            (current_record()->egl_draw != image->drawable ||
             !screen_image_command(image, 6, 0))) {
            screen_gl_failure("native pixmap drawable readback failed", 0x502);
            return 0;
        }
        i32 header[] = {4, image->id, 0, s->width, s->height};
        int fast = screen_fast_publish(s);
        u32 key[] = {getpid(), image->id}, dma_length = 0, status = 1;
        /* Status-prefixed reply, recorded as the host texture contents. */
        u32 *dma = fast ? screen_surface_dma_descriptor(s, &image->dma_runs,
                                                        SCREEN_SURFACE_WRITE,
                                                        4 | 8, key, 2,
                                                        &dma_length) : 0;
        emit_iv(146, header, 5);
        if (dma) {
            memcpy(bulk, dma, dma_length);
            free(dma);
            __sync_synchronize();
            *(volatile u32 *)(bridge + 40) = dma_length;
            status = *(volatile u32 *)(bridge + 44);
            if (status != 1) {
                unlock_record();
            }
            if (status == 2) {
                screen_gl_failure("pixmap render readback failed", 0x502);
                return 0;
            }
        }
        if (!status) {
            /* The device placed the pixels and now holds the comparison copy. */
            image->stale = 1;
            image->resync = 0;
            image->dirty = 0;
            if (!image->published) {
                screen_log("native pixmap render placed by bridge device");
                image->published = 1;
            }
            continue;
        }
        if (recv_all(image->reply, image->bytes + 4) || *(u32 *)image->reply) {
            screen_gl_failure("pixmap render readback failed", 0x502);
            return 0;
        }
        if (!screen_surface_transfer(s, image->reply + 4, 1,
                                     screen_image_api.read, screen_image_api.write)) {
            return 0;
        }
        if (!fast) {
            if (!screen_surface_transfer(s, image->pixels, 0,
                                          screen_image_api.read,
                                          screen_image_api.write)) {
                return 0;
            }
            if (memcmp(image->pixels, image->reply + 4, image->bytes)) {
                screen_gl_failure("native pixmap write/readback mismatch", 0x502);
                return 0;
            }
        }
        memcpy(image->cached, image->reply + 4, image->bytes);
        image->stale = 0;
        /* The device's comparison copy no longer matches the host texture. */
        image->resync = 1;
        image->dirty = 0;
        if (!image->published) {
            screen_log(fast ? "native pixmap render published without readback" :
                             "native pixmap render write/readback verified");
            image->published = 1;
        }
    }
    return 1;
}

static int screen_image_destroy(void *display, void *handle)
{
    if (!screen_display(display)) return 0;
    for (struct ScreenImage *image = screen_images; image; image = image->next) {
        if (image == handle && image->public) {
            if (!image->refs) return screen_image_drop(image);
            image->public = 0;
            return 1;
        }
    }
    return screen_failure("invalid Screen EGLImage", 0x300c);
}

static int screen_image_bind(u32 target, void *handle)
{
    struct ScreenImage *image;
    if ((target != 0xde1 && target != 0x8d65) ||
        screen_image_unit >= 32 || !current_record()->egl_context) {
        screen_gl_failure("unsupported image texture target/context/unit", 0x502);
        return 0;
    }
    for (image = screen_images; image; image = image->next) {
        if (image == handle && image->public) break;
    }
    u32 texture = screen_image_bound[screen_image_unit][target == 0x8d65];
    if (!image || !texture) {
        screen_gl_failure("invalid image or unbound texture", 0x502);
        return 0;
    }
    unsigned slot;
    for (slot = 0; slot < 256; slot++) {
        if (screen_image_textures[slot].texture == texture) break;
    }
    if (slot == 256) {
        for (slot = 0; slot < 256 && screen_image_textures[slot].texture; slot++) {}
    }
    if (slot == 256) {
        screen_gl_failure("image texture binding limit", 0x505);
        return 0;
    }
    struct ScreenImage *old = screen_image_textures[slot].image;
    if (old && old != image) {
        screen_gl_failure("rebinding an existing image texture is unsupported", 0x502);
        return 0;
    }
    if (!screen_image_refresh(image) || !screen_image_command(image, 1, target)) return 0;
    if (!old) image->refs++;
    screen_image_textures[slot].texture = texture;
    screen_image_textures[slot].image = image;
    return 1;
}

void *eglCreateImageKHR(void *d, void *c, u32 target, void *buffer, const i32 *a)
{
    SCREEN_EGL_RETURN(void *, screen_image_create(d, c, target, buffer, a));
}

u32 eglDestroyImageKHR(void *d, void *image)
{
    SCREEN_EGL_RETURN(u32, screen_image_destroy(d, image));
}

void glEGLImageTargetTexture2DOES(u32 target, void *image)
{
    /* The helper's integer result is consumed here; this GL entry point is void. */
    while (!__sync_bool_compare_and_swap(&screen_egl_mutex, 0, 1)) usleep(1000);
    screen_image_bind(target, image);
    __atomic_store_n(&screen_egl_mutex, 0, __ATOMIC_RELEASE);
}

static int screen_image_texture_bind(u32 target, u32 texture)
{
    if (target != 0xde1 && target != 0x8d65) return 1;
    if (screen_image_unit < 32) {
        screen_image_bound[screen_image_unit][target == 0x8d65] = texture;
    }
    struct ScreenImage *image = screen_texture_image(texture);
    return !image || screen_image_refresh(image);
}

static int screen_image_draw(int refresh)
{
    for (unsigned i = 0; refresh && i < 32; i++) {
        for (unsigned t = 0; t < 2; t++) {
            struct ScreenImage *image = screen_texture_image(screen_image_bound[i][t]);
            if (image && !screen_image_refresh(image)) return 0;
        }
    }
    struct ScreenWindow *window = native_window_find(current_record()->egl_draw);
    if (!screen_image_draw_fbo && window && window->pixmap_image) {
        struct ScreenImage *image = window->pixmap_image;
        if (!image->dirty && (!screen_image_refresh(image) ||
                             !screen_image_command(image, 5, 0))) return 0;
        image->dirty = 1;
    }
    for (unsigned i = 0; i < 128; i++) {
        if (screen_image_draw_fbo &&
            screen_image_fbos[i].framebuffer == screen_image_draw_fbo) {
            struct ScreenImage *image = screen_texture_image(screen_image_fbos[i].texture);
            if (image) {
                if (!screen_image_refresh(image)) return 0;
                image->dirty = 1;
            }
        }
    }
    return 1;
}

static void *screen_create_pixmap_surface(void *display, void *config,
                                          void *pixmap, const i32 *attributes)
{
    if (!screen_config(display, config)) return 0;
    if (attributes) {
        unsigned i;
        for (i = 0; i < 64 && attributes[i] != 0x3038; i += 2) {
            if (attributes[i] != 0x3086 || attributes[i + 1] != 0x3085) {
                screen_failure("unsupported pixmap surface attribute", 0x3004);
                return 0;
            }
        }
        if (i == 64) {
            screen_failure("unterminated pixmap surface attributes", 0x3004);
            return 0;
        }
    }
    for (struct ScreenWindow *w = screen_window; w; w = w->next) {
        if (w->pixmap_image && w->window == pixmap) {
            screen_failure("pixmap already has an EGL surface", 0x3003);
            return 0;
        }
    }
    struct ScreenImage *image = screen_image_create(display, 0, 0x30b0, pixmap, 0);
    if (!image) return 0;
    i32 size[] = {0x3057, image->surface.width, 0x3056, image->surface.height, 0x3038};
    struct ScreenWindow *w = screen_create_surface(display, config, 0, size, 1);
    if (!w) {
        screen_image_drop(image);
        return 0;
    }
    image->public = 0;
    image->refs = 1;
    image->drawable = w;
    w->pixmap_image = image;
    w->window = pixmap;
    return w;
}

static int screen_image_surface_current(struct ScreenWindow *window)
{
    return !window || !window->pixmap_image ||
           (screen_image_refresh(window->pixmap_image) &&
            screen_image_command(window->pixmap_image, 5, 0));
}

static int screen_image_surface_unbind(struct ScreenWindow *window)
{
    if (window && window->pixmap_image && window->pixmap_image->hosted) {
        if (!screen_image_command(window->pixmap_image, 3, 0)) return 0;
        window->pixmap_image->hosted = 0;
    }
    return 1;
}

static int screen_image_surface_release(struct ScreenWindow *window)
{
    if (!window->pixmap_image) return 1;
    struct ScreenImage *image = window->pixmap_image;
    window->pixmap_image = 0;
    return screen_image_drop(image);
}

static int screen_image_attachment(u32 target, u32 attachment, u32 type,
                                    u32 texture, i32 level)
{
    u32 framebuffer = target == 0x8ca8 ? screen_image_read_fbo : screen_image_draw_fbo;
    struct ScreenImage *image = screen_texture_image(texture);
    if (image && (!framebuffer || attachment != 0x8ce0 ||
                  type != 0xde1 || level)) {
        screen_gl_failure("unsupported native image framebuffer attachment", 0x502);
        return 0;
    }
    if (attachment != 0x8ce0) return 1;
    unsigned slot;
    for (slot = 0; slot < 128; slot++) {
        if (screen_image_fbos[slot].framebuffer == framebuffer) break;
    }
    if (slot == 128 && image) {
        for (slot = 0; slot < 128 && screen_image_fbos[slot].framebuffer; slot++) {}
    }
    if (slot == 128 && image) {
        screen_gl_failure("native image framebuffer limit", 0x505);
        return 0;
    }
    if (slot < 128) {
        screen_image_fbos[slot].framebuffer = image ? framebuffer : 0;
        screen_image_fbos[slot].texture = image ? texture : 0;
    }
    return 1;
}

static int screen_image_renderbuffer(u32 target, u32 attachment)
{
    u32 framebuffer = target == 0x8ca8 ? screen_image_read_fbo : screen_image_draw_fbo;
    for (unsigned i = 0; framebuffer && attachment == 0x8ce0 && i < 128; i++) {
        if (screen_image_fbos[i].framebuffer == framebuffer) {
            screen_gl_failure("detach native image before renderbuffer attachment", 0x502);
            return 0;
        }
    }
    return 1;
}

static int screen_image_delete(u32 kind, i32 count, const u32 *ids, int apply)
{
    for (i32 n = 0; ids && n < count; n++) {
        if (kind == 2) {
            if (!apply) continue;
            for (unsigned i = 0; i < 128; i++) {
                if (screen_image_fbos[i].framebuffer == ids[n]) {
                    screen_image_fbos[i].framebuffer = 0;
                }
            }
            if (screen_image_draw_fbo == ids[n]) screen_image_draw_fbo = 0;
            if (screen_image_read_fbo == ids[n]) screen_image_read_fbo = 0;
        } else if (kind == 0 && ids[n]) {
            for (unsigned i = 0; i < 128; i++) {
                if (screen_image_fbos[i].framebuffer &&
                    screen_image_fbos[i].texture == ids[n]) {
                    screen_gl_failure("detach native image framebuffer before texture deletion",
                                      0x502);
                    return 0;
                }
            }
            if (!apply) continue;
            for (unsigned i = 0; i < 256; i++) {
                if (screen_image_textures[i].texture == ids[n]) {
                    struct ScreenImage *image = screen_image_textures[i].image;
                    screen_image_textures[i].texture = 0;
                    screen_image_textures[i].image = 0;
                    if (!--image->refs && !image->public && !screen_image_drop(image)) return 0;
                }
            }
            for (unsigned i = 0; i < 32; i++) {
                for (unsigned j = 0; j < 2; j++) {
                    if (screen_image_bound[i][j] == ids[n]) screen_image_bound[i][j] = 0;
                }
            }
        }
    }
    return 1;
}

static int screen_image_storage(u32 target)
{
    if (screen_image_unit < 32 && (target == 0xde1 || target == 0x8d65) &&
        screen_texture_image(screen_image_bound[screen_image_unit][target == 0x8d65])) {
        screen_gl_failure("direct imported image texture uploads are unsupported", 0x502);
        return 0;
    }
    return 1;
}

#define SCREEN_IMAGE_LOCK() \
    while (!__sync_bool_compare_and_swap(&screen_egl_mutex, 0, 1)) usleep(1000)
#define SCREEN_IMAGE_UNLOCK() __atomic_store_n(&screen_egl_mutex, 0, __ATOMIC_RELEASE)

static void native_texture_bind(u32 target, u32 texture)
{
    SCREEN_IMAGE_LOCK();
    screen_image_texture_bind(target, texture);
    SCREEN_IMAGE_UNLOCK();
}

static int screen_images_live(void)
{
    return screen_images != 0;
}

static void screen_image_reset(void)
{
    memset(screen_image_textures, 0, sizeof(screen_image_textures));
    memset(screen_image_fbos, 0, sizeof(screen_image_fbos));
    memset(screen_image_bound, 0, sizeof(screen_image_bound));
    screen_image_unit = screen_image_draw_fbo = screen_image_read_fbo = 0;
}

static int screen_image_before_draw(int refresh)
{
    SCREEN_EGL_RETURN(int, screen_image_draw(refresh));
}

static int screen_image_finish(void)
{
    SCREEN_EGL_RETURN(int, screen_image_sync());
}
#endif

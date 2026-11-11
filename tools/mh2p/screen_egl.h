/*
 * Bounded Screen EGL lifecycle backed by real, independent Mesa surfaces.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef MH2P_SCREEN_EGL_H
#define MH2P_SCREEN_EGL_H

static struct ScreenContext {
    u32 id, owner, version;
    struct Record graphics;
    struct ScreenImageState images;
} screen_contexts[8];

static struct ScreenContext *screen_context_find(void *handle)
{
    for (unsigned i = 0; i < 8; i++) {
        if (handle == &screen_contexts[i] && screen_contexts[i].id)
            return &screen_contexts[i];
    }
    return 0;
}

static struct ScreenContext *current_screen_context(void)
{
    return screen_context_find(current_record()->egl_context);
}

static struct Record *screen_graphics_record(void)
{
    static struct Record unbound;
    struct ScreenContext *context = current_screen_context();
    return context ? &context->graphics : &unbound;
}

static struct ScreenImageState *current_screen_image_state(void)
{
    static struct ScreenImageState unbound;
    struct ScreenContext *context = current_screen_context();
    return context ? &context->images : &unbound;
}
static int screen_initialized;
static volatile unsigned screen_egl_mutex;

#define SCREEN_EGL_RETURN(type, expression) do { \
    while (!__sync_bool_compare_and_swap(&screen_egl_mutex, 0, 1)) { \
        usleep(1000); \
    } \
    type result = (expression); \
    __atomic_store_n(&screen_egl_mutex, 0, __ATOMIC_RELEASE); \
    return result; \
} while (0)

static int screen_control(u32 action, u32 id, u32 a, u32 b, u32 thread)
{
    i32 request[] = {action, id, a, b, thread, 0}, error;

    emit_iv(139, request, 6);
    if (recv_all(&error, sizeof(error))) {
        return screen_failure("EGL control transport failed", 0x3002);
    }
    return error == 0x3000 ||
           screen_failure("host EGL operation rejected", error);
}

static int screen_display(void *display)
{
    if (display != (void *)1) {
        return screen_failure("invalid EGL display", 0x3008);
    }
    return screen_initialized ||
           screen_failure("EGL display is not initialized", 0x3001);
}

static int screen_config(void *display, void *config)
{
    return screen_display(display) &&
           (config == (void *)1 ||
            screen_failure("invalid EGL config", 0x3005));
}

static int screen_query_context(void *display, void *context, i32 attribute,
                                i32 *value)
{
    if (!screen_display(display)) {
        return 0;
    }
    struct ScreenContext *c = screen_context_find(context);
    if (!c) {
        return screen_failure("invalid query context", 0x3006);
    }
    if (!value) {
        return screen_failure("null context query output", 0x300c);
    }
    switch (attribute) {
    case 0x3028:
        *value = 1;
        break;
    case 0x3097:
        *value = 0x30a0;
        break;
    case 0x3098:
        *value = c->version;
        break;
    case 0x3086:
        *value = 0x3084;
        break;
    default:
        return screen_failure("unsupported context query", 0x3004);
    }
    return 1;
}

static int screen_query_surface(void *display, void *surface, i32 attribute,
                                i32 *value)
{
    i32 width, height;

    if (!screen_display(display) ||
        !native_window_size(surface, &width, &height)) {
        return 0;
    }
    if (!value) {
        return screen_failure("null surface query output", 0x300c);
    }
    switch (attribute) {
    case 0x3057:
        *value = width;
        break;
    case 0x3056:
        *value = height;
        break;
    case 0x3086:
        *value = native_window_find(surface)->pixmap_image ? 0x3085 : 0x3084;
        break;
    default:
        return screen_failure("unsupported surface query", 0x3004);
    }
    return 1;
}

static int screen_initialize(void *display, i32 *major, i32 *minor)
{
    const char *width = getenv("GLBRIDGE_W"), *height = getenv("GLBRIDGE_H");
    i32 dimensions[] = {width ? atoi(width) : 800, height ? atoi(height) : 480};

    if (display != (void *)1) {
        return screen_failure("invalid EGL display", 0x3008);
    }
    if (dimensions[0] < 1 || dimensions[0] > 2048 ||
        dimensions[1] < 1 || dimensions[1] > 2048) {
        return screen_failure("unsupported bridge dimensions", 0x3009);
    }
    if (!screen_initialized) {
        emit_iv(1, dimensions, 2);
        if (!screen_control(0, 0, 0, 0, 0)) {
            return 0;
        }
        screen_initialized = 1;
    }
    if (major) {
        *major = 1;
    }
    if (minor) {
        *minor = 4;
    }
    return 1;
}

static void *screen_create_context(void *display, void *config, void *share,
                                   const i32 *attributes)
{
    u32 id;
    i32 version = 1;

    if (!screen_config(display, config)) {
        return 0;
    }
    if (share) {
        screen_failure("shared contexts are unsupported", 0x3009);
        return 0;
    }
    if (attributes) {
        unsigned i;
        for (i = 0; i < 64 && attributes[i] != 0x3038; i += 2) {
            if (attributes[i] != 0x3098) {
                screen_failure("unsupported context attribute", 0x3004);
                return 0;
            }
            version = attributes[i + 1];
        }
        if (i == 64) {
            screen_failure("unterminated context attributes", 0x3004);
            return 0;
        }
    }
    if (version != 2 && version != 3) {
        screen_failure("only ES2 and ES3 contexts are supported", 0x3009);
        return 0;
    }
    struct ScreenContext *context = 0;
    for (unsigned i = 0; i < 8; i++) {
        if (!screen_contexts[i].id) {
            context = &screen_contexts[i];
            break;
        }
    }
    if (!context) {
        screen_failure("EGL context limit reached", 0x3003);
        return 0;
    }
    id = __sync_add_and_fetch(&g_id, 1);
    if (!screen_control(1, id, version, 0, 0)) {
        return 0;
    }
    context->id = id;
    context->version = version;
    return context;
}

static void *screen_create_surface(void *display, void *config, void *window,
                                   const i32 *attributes, int pbuffer)
{
    struct ScreenWindow *w;
    i32 width = 0, height = 0;

    if (!screen_config(display, config)) {
        return 0;
    }
    if (attributes) {
        unsigned i;
        for (i = 0; i < 64 && attributes[i] != 0x3038; i += 2) {
            if (pbuffer && attributes[i] == 0x3057) {
                width = attributes[i + 1];
            } else if (pbuffer && attributes[i] == 0x3056) {
                height = attributes[i + 1];
            } else if (attributes[i] != 0x3086 ||
                       attributes[i + 1] != 0x3084) {
                screen_failure("unsupported surface attribute", 0x3004);
                return 0;
            }
        }
        if (i == 64) {
            screen_failure("unterminated surface attributes", 0x3004);
            return 0;
        }
    }
    if (pbuffer) {
        if (width < 1 || width > 2048 || height < 1 || height > 2048) {
            screen_failure("unsupported pbuffer dimensions", 0x300c);
            return 0;
        }
        w = malloc(sizeof(*w));
        if (!w) {
            screen_failure("pbuffer allocation failed", 0x3003);
            return 0;
        }
        memset(w, 0, sizeof(*w));
        w->width = width;
        w->height = height;
        w->pbuffer = 1;
        w->next = screen_window;
        screen_window = w;
    } else {
        w = native_window_create(window);
        if (!w) {
            return 0;
        }
    }
    w->id = __sync_add_and_fetch(&g_id, 1);
    if (!screen_control(3, w->id, w->width, w->height, 0)) {
        native_window_destroy(w);
        return 0;
    }
    return w;
}

static int screen_make_current(void *display, void *draw, void *read,
                                void *context)
{
    struct Record *r = current_record();
    struct ScreenWindow *d = native_window_find(draw);
    struct ScreenWindow *s = native_window_find(read);
    struct ScreenContext *c = screen_context_find(context);
    struct ScreenContext *old_c = current_screen_context();

    if (!screen_display(display)) {
        return 0;
    }
    if (context && !c) {
        return screen_failure("invalid EGL context", 0x3006);
    }
    if ((draw && !d) || (read && !s)) {
        return screen_failure("invalid current EGL surface", 0x300d);
    }
    if ((!context && (draw || read)) || (!draw != !read)) {
        return screen_failure("incompatible EGL binding", 0x3009);
    }
    if (((d && d->pixmap_image) || (s && s->pixmap_image)) && d != s) {
        return screen_failure("pixmap draw/read surfaces must match", 0x3009);
    }
    if (!context && !r->egl_context) {
        return 1;
    }
    if (c && c->owner && c->owner != r->egl_thread) {
        return screen_failure("EGL context belongs to another thread", 0x3002);
    }
    struct ScreenWindow *old = native_window_find(r->egl_draw);
    struct ScreenWindow *old_read = native_window_find(r->egl_read);
    void *old_context = r->egl_context;
    if (r->egl_context && (!screen_image_sync() ||
        ((old != d || old_context != context) &&
         !screen_image_surface_unbind(old)))) {
        return screen_failure("flushing previous native surface failed", 0x3002);
    }
    if (!screen_control(5, c ? c->id : 0,
                        d ? d->id : 0, s ? s->id : 0, r->egl_thread)) {
        return 0;
    }
    r->egl_context = context;
    r->egl_draw = draw;
    r->egl_read = read;
    if (old_c) old_c->owner = 0;
    if (c) c->owner = r->egl_thread;
    if (context && !screen_image_surface_current(d)) {
        if (!screen_image_surface_unbind(d)) {
            screen_log("failed pixmap binding cleanup failed");
        }
        if (screen_control(5, old_c ? old_c->id : 0,
                           old ? old->id : 0, old_read ? old_read->id : 0,
                           r->egl_thread)) {
            r->egl_context = old_context;
            r->egl_draw = old;
            r->egl_read = old_read;
            if (c) c->owner = 0;
            if (old_c) old_c->owner = r->egl_thread;
        }
        return screen_failure("initializing native pixmap drawable failed", 0x3002);
    }
    return 1;
}

static int screen_destroy_surface(void *display, void *surface)
{
    struct ScreenWindow *w = native_window_find(surface);

    if (!screen_display(display)) {
        return 0;
    }
    if (!w) {
        return screen_failure("invalid EGL surface", 0x300d);
    }
    if (!screen_control(4, w->id, 0, 0, 0)) return 0;
    int released = screen_image_surface_release(w);
    int destroyed = native_window_destroy(w);
    return released && destroyed;
}

static int screen_destroy_context(void *display, void *context)
{
    struct ScreenContext *c = screen_context_find(context);
    if (!screen_display(display)) {
        return 0;
    }
    if (!c) {
        return screen_failure("invalid EGL context", 0x3006);
    }
    if (c->owner) {
        return screen_failure("release current context before destroying", 0x3002);
    }
    if (screen_images_live()) {
        return screen_failure("release native images before destroying context", 0x3002);
    }
    if (!screen_control(2, c->id, 0, 0, 0)) {
        return 0;
    }
    memset(c, 0, sizeof(*c));
    return 1;
}

static int screen_swap(void *display, void *surface)
{
    struct Record *r = current_record();

    if (!screen_display(display)) {
        return 0;
    }
    if (!r->egl_context || r->egl_draw != surface || r->egl_read != surface) {
        return screen_failure("swap requires its current draw/read surface",
                              0x3009);
    }
    if (!native_window_swap(surface)) {
        return 0;
    }
    rec(5, 0);
    return 1;
}

static int screen_terminate(void *display)
{
    if (!screen_display(display)) {
        return 0;
    }
    int contexts = 0;
    for (unsigned i = 0; i < 8; i++) contexts |= screen_contexts[i].id != 0;
    if (contexts || screen_window || screen_images_live()) {
        return screen_failure("destroy EGL objects before terminating", 0x3002);
    }
    screen_initialized = 0;
    return 1;
}

static void release_screen_record(struct Record *r)
{
    if (r->egl_context) {
        if (pthread_setspecific(record_key, r)) {
            abort();
        }
        while (!__sync_bool_compare_and_swap(&screen_egl_mutex, 0, 1)) {
            usleep(1000);
        }
        screen_make_current((void *)1, 0, 0, 0);
        __atomic_store_n(&screen_egl_mutex, 0, __ATOMIC_RELEASE);
        if (pthread_setspecific(record_key, 0)) {
            abort();
        }
    }
}

#endif

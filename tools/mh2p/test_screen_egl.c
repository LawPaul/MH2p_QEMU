/* Firmware-free independent context lifecycle and guest-side state checks.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef uint32_t u32;
typedef int32_t i32;
struct Record {
    u32 array_buffer, element_buffer, egl_thread;
    void *egl_context, *egl_draw, *egl_read;
};
struct ScreenImageState { u32 texture, framebuffer; i32 error; };
struct ScreenWindow {
    struct ScreenWindow *next;
    void *pixmap_image;
    i32 width, height;
    u32 id;
    int pbuffer;
};
static struct ScreenWindow *screen_window;
static struct Record records[2] = {{.egl_thread = 11}, {.egl_thread = 12}};
static unsigned thread_index, controls, syncs, fail_binding;
static unsigned record_key;
static int pthread_setspecific(unsigned key, const void *value)
{
    (void)key;
    (void)value;
    return 0;
}
static u32 g_id;
static i32 egl_error, host_error = 0x3000;
static struct Record *current_record(void) { return &records[thread_index]; }
static int screen_failure(const char *message, i32 error)
{
    (void)message;
    egl_error = error;
    return 0;
}
static void screen_log(const char *message) { (void)message; }
static void emit_iv(unsigned op, const i32 *request, unsigned count)
{
    assert((op == 1 && count == 2) || (op == 139 && count == 6));
    (void)request;
    controls++;
}
static int recv_all(void *data, unsigned size)
{
    assert(size == sizeof(host_error));
    memcpy(data, &host_error, size);
    return 0;
}
static struct ScreenWindow *native_window_find(void *handle)
{
    for (struct ScreenWindow *w = screen_window; w; w = w->next)
        if (w == handle) return w;
    return NULL;
}
static int native_window_size(void *handle, i32 *width, i32 *height)
{
    struct ScreenWindow *w = native_window_find(handle);
    if (!w) return 0;
    *width = w->width;
    *height = w->height;
    return 1;
}
static void *native_window_create(void *window) { return window; }
static int native_window_destroy(void *window)
{
    struct ScreenWindow **link = &screen_window;
    while (*link && *link != window) link = &(*link)->next;
    if (!*link) return 0;
    struct ScreenWindow *old = *link;
    *link = old->next;
    free(old);
    return 1;
}
static int native_window_swap(void *window) { return !!window; }
static int screen_image_sync(void) { syncs++; return 1; }
static int screen_image_surface_unbind(struct ScreenWindow *w) { (void)w; return 1; }
static int screen_image_surface_current(struct ScreenWindow *w)
{
    (void)w;
    return !fail_binding;
}
static int screen_image_surface_release(struct ScreenWindow *w) { (void)w; return 1; }
static int screen_images_live(void) { return 0; }
static void rec(unsigned op, unsigned size) { (void)op; (void)size; }
#include "screen_egl.h"

int main(void)
{
    void *display = (void *)1;
    i32 es2[] = {0x3098, 2, 0x3038};
    i32 es3[] = {0x3098, 3, 0x3038};
    i32 dimensions[] = {0x3057, 4, 0x3056, 4, 0x3038};
    assert(screen_initialize(display, NULL, NULL));
    void *first = screen_create_context(display, display, NULL, es2);
    void *second = screen_create_context(display, display, NULL, es3);
    assert(first && second && first != second);
    assert(!screen_create_context(display, display, first, es2));
    assert(egl_error == 0x3009);
    i32 version;
    assert(screen_query_context(display, first, 0x3098, &version) && version == 2);
    assert(screen_query_context(display, second, 0x3098, &version) && version == 3);
    void *surface = screen_create_surface(display, display, NULL, dimensions, 1);
    assert(surface);
    assert(screen_make_current(display, surface, surface, first));
    screen_graphics_record()->array_buffer = 123;
    current_screen_image_state()->texture = 42;
    current_screen_image_state()->error = 0x501;
    assert(screen_make_current(display, surface, surface, second));
    assert(!screen_graphics_record()->array_buffer);
    assert(!current_screen_image_state()->texture);
    assert(!current_screen_image_state()->error);
    screen_graphics_record()->array_buffer = 456;
    current_screen_image_state()->texture = 71;
    current_screen_image_state()->error = 0x502;
    assert(screen_make_current(display, surface, surface, first));
    assert(screen_graphics_record()->array_buffer == 123);
    assert(current_screen_image_state()->texture == 42);
    assert(current_screen_image_state()->error == 0x501);
    thread_index = 1;
    unsigned before = controls;
    assert(!screen_make_current(display, surface, surface, first));
    assert(egl_error == 0x3002 && controls == before);
    assert(screen_make_current(display, NULL, NULL, second));
    assert(screen_graphics_record()->array_buffer == 456);
    assert(current_screen_image_state()->texture == 71);
    assert(!screen_destroy_context(display, second));
    assert(screen_make_current(display, NULL, NULL, NULL));
    thread_index = 0;
    host_error = 0x3002;
    assert(!screen_make_current(display, surface, surface, second));
    assert(current_screen_context() == first);
    assert(screen_graphics_record()->array_buffer == 123);
    host_error = 0x3000;
    fail_binding = 1;
    assert(!screen_make_current(display, surface, surface, second));
    assert(current_screen_context() == first);
    assert(screen_context_find(first)->owner == 11);
    assert(!screen_context_find(second)->owner);
    fail_binding = 0;
    assert(screen_make_current(display, NULL, NULL, NULL));
    assert(screen_destroy_context(display, first));
    assert(!screen_query_context(display, first, 0x3098, &version));
    void *replacement = screen_create_context(display, display, NULL, es2);
    assert(replacement);
    assert(screen_make_current(display, surface, surface, replacement));
    assert(!screen_graphics_record()->array_buffer);
    assert(!current_screen_image_state()->texture);
    assert(!current_screen_image_state()->error);
    assert(screen_make_current(display, NULL, NULL, NULL));
    void *contexts[6];
    for (unsigned i = 0; i < 6; i++) {
        contexts[i] = screen_create_context(display, display, NULL, es2);
        assert(contexts[i]);
    }
    assert(!screen_create_context(display, display, NULL, es2));
    assert(egl_error == 0x3003);
    assert(!screen_terminate(display));
    for (unsigned i = 0; i < 6; i++)
        assert(screen_destroy_context(display, contexts[i]));
    assert(screen_destroy_context(display, replacement));
    assert(screen_destroy_context(display, second));
    assert(screen_destroy_surface(display, surface));
    assert(screen_terminate(display) && syncs);
    return 0;
}

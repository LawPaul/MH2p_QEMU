/* Firmware-free Mesa checks for the opt-in EGL surface lifecycle. */
#define main glhost_main
#include "glhost.c"
#undef main
#include <assert.h>

static uint32_t control(uint32_t action, uint32_t id, uint32_t a, uint32_t b,
                        uint32_t thread)
{
    uint32_t request[] = {action, id, a, b, thread, 0};
    assert(valid_record(139, sizeof(request), (uint8_t *)request));
    return client_egl_control(100, (uint8_t *)request);
}

static void bind_surface(unsigned id, unsigned thread)
{
    assert(control(5, 42, id, id, thread) == EGL_SUCCESS);
    request_thread = thread;
    select_client(100);
}

static void pixel(unsigned r, unsigned g, unsigned b)
{
    unsigned char p[4];
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
    assert(glGetError() == GL_NO_ERROR);
    assert(p[0] == r && p[1] == g && p[2] == b && p[3] == 255);
}

int main(void)
{
    ensure_window(8, 8);
    assert(control(3, 1, 8, 8, 0) == EGL_NOT_INITIALIZED);
    assert(control(0, 0, 0, 0, 0) == EGL_SUCCESS);
    assert(control(1, 42, 1, 0, 0) == EGL_BAD_MATCH);
    assert(control(1, 42, 2, 0, 0) == EGL_SUCCESS);
    assert(control(1, 42, 2, 0, 0) == EGL_BAD_ALLOC);
    assert(control(1, 43, 2, 42, 0) == EGL_BAD_MATCH);
    assert(control(3, 1, 8, 8, 0) == EGL_SUCCESS);
    assert(control(3, 1, 8, 8, 0) == EGL_BAD_SURFACE);
    assert(control(3, 2, 2049, 8, 0) == EGL_BAD_PARAMETER);
    assert(control(3, 2, 4, 6, 0) == EGL_SUCCESS);
    for (unsigned i = 3; i <= 8; i++) {
        assert(control(3, i, 2, 2, 0) == EGL_SUCCESS);
    }
    assert(control(3, 9, 2, 2, 0) == EGL_BAD_ALLOC);
    assert(control(1, 43, 2, 0, 0) == EGL_SUCCESS);
    bind_surface(1, 11);
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport(1, 2, 3, 4);
    assert(control(4, 1, 0, 0, 0) == EGL_BAD_ACCESS);
    assert(control(2, 42, 0, 0, 0) == EGL_BAD_ACCESS);
    assert(control(5, 42, 2, 2, 12) == EGL_BAD_ACCESS);
    assert(control(5, 42, 999, 999, 11) == EGL_BAD_SURFACE);
    bind_surface(2, 11);
    assert(win_w == 4 && win_h == 6);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    pixel(0, 0, 255);
    bind_surface(1, 11);
    pixel(255, 0, 0);
    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    assert(viewport[0] == 1 && viewport[1] == 2 &&
           viewport[2] == 3 && viewport[3] == 4);
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    gmap[900] = texture;
    blend->active_program = 55;
    current_client->saved_errors = 1u << (GL_INVALID_ENUM - 0x500);
    assert(control(5, 43, 2, 2, 11) == EGL_SUCCESS);
    select_client(100);
    assert(!glIsTexture(texture));
    assert(!gmap[900] && !blend->active_program);
    assert(client_gl_error(current_client) == GL_NO_ERROR);
    GLint separate_viewport[4];
    glGetIntegerv(GL_VIEWPORT, separate_viewport);
    assert(separate_viewport[0] == 0 && separate_viewport[1] == 0);
    glViewport(0, 0, 2, 2);
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    bind_surface(1, 11);
    assert(glIsTexture(texture));
    assert(gmap[900] == texture && blend->active_program == 55);
    assert(client_gl_error(current_client) == GL_INVALID_ENUM);
    assert(client_gl_error(current_client) == GL_NO_ERROR);
    glGetIntegerv(GL_VIEWPORT, separate_viewport);
    assert(separate_viewport[0] == 1 && separate_viewport[1] == 2 &&
           separate_viewport[2] == 3 && separate_viewport[3] == 4);
    assert(control(5, 43, 2, 2, 12) == EGL_SUCCESS);
    assert(control(5, 42, 2, 2, 11) == EGL_BAD_ACCESS);
    assert(control(5, 43, 1, 1, 11) == EGL_BAD_ACCESS);
    request_thread = 12;
    select_client(100);
    pixel(0, 255, 0);
    glGetIntegerv(GL_VIEWPORT, separate_viewport);
    assert(separate_viewport[2] == 2 && separate_viewport[3] == 2);
    assert(control(4, 2, 0, 0, 0) == EGL_BAD_ACCESS);
    assert(control(2, 43, 0, 0, 0) == EGL_BAD_ACCESS);
    assert(control(5, 0, 0, 0, 12) == EGL_SUCCESS);
    assert(control(2, 43, 0, 0, 0) == EGL_SUCCESS);
    request_thread = 11;
    bind_surface(2, 11);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    assert(glIsTexture(texture));
    select_client(200);
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    select_client(100);
    pixel(0, 0, 255);
    assert(control(5, 0, 0, 0, 11) == EGL_SUCCESS);
    assert(control(5, 42, 1, 0, 12) == EGL_BAD_MATCH);
    bind_surface(1, 12);
    pixel(255, 0, 0);
    assert(glIsTexture(texture));
    assert(control(5, 42, 0, 0, 12) == EGL_SUCCESS);
    request_thread = 12;
    select_client(100);
    assert(eglGetCurrentSurface(EGL_DRAW) == EGL_NO_SURFACE);
    assert(eglGetCurrentContext() != EGL_NO_CONTEXT);
    assert(control(5, 0, 0, 0, 12) == EGL_SUCCESS);
    for (unsigned i = 1; i <= 8; i++) {
        assert(control(4, i, 0, 0, 0) == EGL_SUCCESS);
    }
    assert(control(4, 1, 0, 0, 0) == EGL_BAD_SURFACE);
    assert(control(2, 42, 0, 0, 0) == EGL_SUCCESS);
    for (unsigned i = 0; i < 8; i++) {
        assert(control(1, 100 + i, 2, 0, 0) == EGL_SUCCESS);
    }
    assert(control(1, 108, 2, 0, 0) == EGL_BAD_ALLOC);
    for (unsigned i = 0; i < 8; i++) {
        assert(control(2, 100 + i, 0, 0, 0) == EGL_SUCCESS);
    }
    assert(control(2, 42, 0, 0, 0) == EGL_BAD_CONTEXT);
    assert(control(1, 42, 2, 0, 0) == EGL_SUCCESS);
    assert(control(3, 1, 8, 8, 0) == EGL_SUCCESS);
    bind_surface(1, 11);
    assert(!glIsTexture(texture));
    assert(control(5, 0, 0, 0, 11) == EGL_SUCCESS);
    assert(control(4, 1, 0, 0, 0) == EGL_SUCCESS);
    assert(control(2, 42, 0, 0, 0) == EGL_SUCCESS);
    assert(control(1, 42, 3, 0, 0) == EGL_SUCCESS);
    assert(control(3, 1, 8, 8, 0) == EGL_SUCCESS);
    bind_surface(1, 11);
    EGLint version;
    assert(eglQueryContext(display, eglGetCurrentContext(),
                           EGL_CONTEXT_CLIENT_VERSION, &version));
    assert(version == 3);
    const char *source =
        "#version 300 es\n"
        "void main() { gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }\n";
    GLuint shader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint compiled;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    assert(compiled == GL_TRUE);
    glDeleteShader(shader);
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    pixel(0, 255, 0);
    assert(control(5, 0, 0, 0, 11) == EGL_SUCCESS);
    assert(control(4, 1, 0, 0, 0) == EGL_SUCCESS);
    assert(control(2, 42, 0, 0, 0) == EGL_SUCCESS);
    uint32_t malformed[] = {5, 0, 0, 0, 11, 1};
    assert(client_egl_control(100, (uint8_t *)malformed) == EGL_BAD_PARAMETER);
    assert(!valid_record(139, 20, (uint8_t *)malformed));
    malformed[0] = 6;
    assert(!valid_record(139, 24, (uint8_t *)malformed));
    malformed[0] = 0;
    assert(!valid_record(140, 4, (uint8_t *)malformed));
    puts("PASS: independent Mesa surfaces/contexts, isolated objects/errors/state, thread ownership, "
         "PID isolation, surfaceless binding and cleanup");
    return 0;
}

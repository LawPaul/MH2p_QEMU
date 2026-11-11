/* QNX pbuffer smoke client; no firmware graphics library is replaced. */
#include "graphics_smoke_abi.h"

static unsigned char pixels[64 * 32 * 4];

static void require(int condition, const char *message, unsigned size)
{
    if (!condition) {
        write(2, message, size);
        exit(1);
    }
}

#define REQUIRE(test, text) require((test), text "\n", sizeof(text "\n") - 1)

void smoke_main(unsigned *initial_stack) __attribute__((noreturn));

__attribute__((naked, noreturn)) void _start(void)
{
    __asm__ volatile("mov r0, sp\nb smoke_main");
}

void smoke_main(unsigned *initial_stack)
{
    const int config_attrs[] = { 0x3033, 1, 0x3040, 4, 0x3038 };
    const int surface_attrs[] = { 0x3057, 64, 0x3056, 32, 0x3038 };
    const int context_attrs[] = { 0x3098, 2, 0x3038 };
    void *display, *config = 0, *surface, *context;
    int major = 0, minor = 0, count = 0;

    /* QNX supplies argc/argv/envp on the initial stack; no vendor CRT. */
    environ = (char **)(initial_stack + initial_stack[0] + 2);
    write(1, "MH2P_EGL_SMOKE_START\n", sizeof("MH2P_EGL_SMOKE_START\n") - 1);
    display = eglGetDisplay(0);
    REQUIRE(display && eglInitialize(display, &major, &minor),
            "MH2P_EGL_SMOKE_FAIL initialize");
    REQUIRE(eglBindAPI(0x30a0), "MH2P_EGL_SMOKE_FAIL bind");
    REQUIRE(eglChooseConfig(display, config_attrs, &config, 1, &count) &&
            count == 1, "MH2P_EGL_SMOKE_FAIL config");
    surface = eglCreatePbufferSurface(display, config, surface_attrs);
    context = eglCreateContext(display, config, 0, context_attrs);
    REQUIRE(surface && context &&
            eglMakeCurrent(display, surface, surface, context),
            "MH2P_EGL_SMOKE_FAIL context");

    glViewport(0, 0, 64, 32);
    glClearColor(1, 0, 0, 1);
    glClear(0x4000);
    glEnable(0x0c11);
    glScissor(32, 0, 32, 32);
    glClearColor(0, 1, 0, 1);
    glClear(0x4000);
    glDisable(0x0c11);
    for (unsigned i = 0; i < sizeof(pixels); i++) {
        pixels[i] = 0x5a;
    }
    glReadPixels(0, 0, 64, 32, 0x1908, 0x1401, pixels);
    REQUIRE(glGetError() == 0, "MH2P_EGL_SMOKE_FAIL GL error");
    for (unsigned y = 0; y < 32; y++) {
        for (unsigned x = 0; x < 64; x++) {
            unsigned char *pixel = pixels + (y * 64 + x) * 4;
            REQUIRE(pixel[0] == (x < 32 ? 255 : 0) &&
                    pixel[1] == (x < 32 ? 0 : 255) &&
                    pixel[2] == 0 && pixel[3] == 255,
                    "MH2P_EGL_SMOKE_FAIL readback pixels");
        }
    }
    REQUIRE(eglSwapBuffers(display, surface), "MH2P_EGL_SMOKE_FAIL swap");
    /* Round trip after swap so the host has published the frame before PASS. */
    REQUIRE(glGetError() == 0, "MH2P_EGL_SMOKE_FAIL swap completion");
    REQUIRE(eglDestroySurface(display, surface) &&
            eglDestroyContext(display, context) && eglTerminate(display),
            "MH2P_EGL_SMOKE_FAIL cleanup");
    write(1, "MH2P_EGL_SMOKE_PASS 64x32 pixels=2048\n",
          sizeof("MH2P_EGL_SMOKE_PASS 64x32 pixels=2048\n") - 1);
    exit(0);
}

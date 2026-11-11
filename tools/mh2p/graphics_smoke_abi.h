/* Minimal ARM soft-float QNX/EGL ABI for SDK-free graphics diagnostics. */
#ifndef MH2P_GRAPHICS_SMOKE_ABI_H
#define MH2P_GRAPHICS_SMOKE_ABI_H

typedef unsigned int u32;
extern int write(int, const void *, unsigned);
extern void exit(int) __attribute__((noreturn));
extern char **environ;
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern char *dlerror(void);
extern void *eglGetDisplay(void *);
extern u32 eglInitialize(void *, int *, int *);
extern u32 eglBindAPI(u32);
extern u32 eglChooseConfig(void *, const int *, void **, int, int *);
extern void *eglCreatePbufferSurface(void *, void *, const int *);
extern void *eglCreateContext(void *, void *, void *, const int *);
extern u32 eglMakeCurrent(void *, void *, void *, void *);
extern u32 eglSwapBuffers(void *, void *);
extern u32 eglDestroySurface(void *, void *);
extern u32 eglDestroyContext(void *, void *);
extern u32 eglTerminate(void *);
extern void glViewport(int, int, int, int);
extern void glClearColor(float, float, float, float);
extern void glClear(u32);
extern void glEnable(u32);
extern void glDisable(u32);
extern void glScissor(int, int, int, int);
extern void glReadPixels(int, int, int, int, u32, u32, void *);
extern u32 glGetError(void);

#endif

#define _GNU_SOURCE
/*
 * glhost - Linux/macOS host renderer for the MIB2 GL-forward shim.
 *
 * Experimental QNX GLES/EGL forwarding over stdin/stdout, backed by a Mesa
 * surfaceless EGL pbuffer. Guest records use [u32 opcode][u32 length][payload].
 * Replies are written to stdout; diagnostics go to stderr. This is a local
 * bring-up prototype with incomplete APIs and context isolation.
 *
 * Payload layouts are paired with guest/gl_shim.c.
 *   1 eglInitialize{w,h} 2 glViewport 3 glClearColor 4 glClear 5 swap 6 glFinish
 *   7-14 shaders/programs  15-18 buffers  19-21 vertex attribs  22-30 uniforms
 *   31-32 draws  33-43 pipeline state  50-58 textures  100/101 sync getters
 */
#include <GLES2/gl2.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <errno.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "known_shaders.h"

#define DEFAULT_PORT 5005
#define DEFAULT_W    800
#define DEFAULT_H    480
#define MAXOBJ       (1 << 20)

static int win;
static EGLDisplay display;
static EGLSurface surface;
static EGLConfig config;
static int win_w, win_h;
static uint32_t current_pid;
static uint32_t request_thread;
static int default_w, default_h;
static uint32_t primary_display_pid;
static GLuint default_gmap[MAXOBJ];
static GLuint *gmap = default_gmap;        /* guest object id -> real GL object */

/* NVIDIA binaries bake the source-over operation into the fragment program.
 * GLES2 can express these three programs without framebuffer-fetch extensions:
 * emit premultiplied source, then blend ONE / ONE_MINUS_SRC_ALPHA. Road fill
 * keeps destination alpha. Scope the override to the draw, preserving guest
 * state and keeping shader/program metadata isolated between guest contexts. */
struct BlendMetadata {
    uint8_t shader[MAXOBJ], program[MAXOBJ];
    void *client_arrays[16];
    unsigned client_array_bytes[16];
    struct MenuTexture *menu;
    uint32_t fragment[MAXOBJ], active_program;
};
static struct BlendMetadata default_blend, *blend = &default_blend;

struct BlendState {
    GLboolean enabled;
    GLint src_rgb, dst_rgb, src_alpha, dst_alpha, equation_rgb, equation_alpha;
};

static void begin_binary_blend(unsigned mode, struct BlendState *saved)
{
    if (!mode) return;
    saved->enabled = glIsEnabled(GL_BLEND);
    glGetIntegerv(GL_BLEND_SRC_RGB, &saved->src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &saved->dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &saved->src_alpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &saved->dst_alpha);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &saved->equation_rgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &saved->equation_alpha);
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparate(mode == 5 ? GL_ZERO : (mode == 3 ? GL_SRC_ALPHA : GL_ONE),
                        mode >= 4 ? GL_ZERO : GL_ONE_MINUS_SRC_ALPHA,
                        (mode == 2 || mode == 5) ? GL_ZERO : (mode == 3 ? GL_SRC_ALPHA : GL_ONE),
                        mode == 2 ? GL_ONE : (mode >= 4 ? GL_ZERO : GL_ONE_MINUS_SRC_ALPHA));
}

static void end_binary_blend(unsigned mode, const struct BlendState *saved)
{
    if (!mode) return;
    glBlendFuncSeparate(saved->src_rgb, saved->dst_rgb, saved->src_alpha, saved->dst_alpha);
    glBlendEquationSeparate(saved->equation_rgb, saved->equation_alpha);
    if (!saved->enabled) glDisable(GL_BLEND);
}

/* Local GLSL extracted from paired source/binary Kanzi firmware resources. */
static char *load_firmware_shader(uint64_t hash, uint32_t size, unsigned *mode)
{
    const char *directory = getenv("MHI2_SHADER_CACHE");
    char path[4096];
    if (!directory) return NULL;
    int n = snprintf(path, sizeof(path), "%s/%016llx-%u.blend",
                     directory, (unsigned long long)hash, size);
    if (n < 0 || n >= sizeof(path)) return NULL;
    FILE *file = fopen(path, "r");
    if (!file) return NULL;
    int valid = fscanf(file, "%u", mode) == 1 && *mode <= 5;
    fclose(file);
    if (!valid) return NULL;
    snprintf(path, sizeof(path), "%s/%016llx-%u.glsl",
             directory, (unsigned long long)hash, size);
    file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END)) { fclose(file); return NULL; }
    long length = ftell(file);
    if (length <= 0 || length > 1024*1024) { fclose(file); return NULL; }
    rewind(file);
    char *source = malloc(length + 1);
    if (!source) { fclose(file); return NULL; }
    valid = fread(source, 1, length, file) == length;
    fclose(file);
    if (!valid) { free(source); return NULL; }
    source[length] = 0;
    return source;
}

static GLuint M(uint32_t id) { return id < MAXOBJ ? gmap[id] : 0; }

/* read a little-endian i32/u32/f32 from a payload at byte offset */
static int32_t i32(const uint8_t *p, int off) { int32_t v; memcpy(&v, p + off, 4); return v; }
static uint32_t u32(const uint8_t *p, int off) { uint32_t v; memcpy(&v, p + off, 4); return v; }

#include "encoder.h"
#include "video_decoder.h"
#include "menu_texture.h"

static unsigned char *display_rgba;

static const char *frame_path(void)
{ const char *p=getenv("MHI2_GL_FRAME");return p&&*p?p:"/tmp/glhost_frame.ppm"; }

static bool frame_dump_enabled(void)
{
    const char *disabled = getenv("MHI2_GL_NO_FRAME_DUMP");

    return !disabled || strcmp(disabled, "1");
}

static bool write_frame(const char *path, const uint8_t *rgba,
                         int width, int height, int stride, bool inverted)
{
    char temporary[4096];
    int length = snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    if (length < 0 || (size_t)length >= sizeof(temporary)) {
        fprintf(stderr, "glhost: frame path too long\n");
        return false;
    }
    uint8_t *row = malloc((size_t)width * 3);
    if (!row) {
        fprintf(stderr, "glhost: allocating frame row failed\n");
        return false;
    }
    FILE *file = fopen(temporary, "wb");
    if (!file) {
        fprintf(stderr, "glhost: opening frame %s: %s\n", temporary, strerror(errno));
        free(row);
        return false;
    }
    bool ok = fprintf(file, "P6\n%d %d\n255\n", width, height) > 0;
    for (int y = 0; ok && y < height; y++) {
        const uint8_t *source = rgba +
            (size_t)(inverted ? height - y - 1 : y) * stride;
        for (int x = 0; x < width; x++) {
            memcpy(row + x * 3, source + x * 4, 3);
        }
        ok = fwrite(row, 3, width, file) == (size_t)width;
    }
    if (!ok) {
        fprintf(stderr, "glhost: writing frame %s failed\n", temporary);
    }
    if (fclose(file)) {
        fprintf(stderr, "glhost: closing frame %s: %s\n", temporary, strerror(errno));
        ok = false;
    }
    if (ok && rename(temporary, path)) {
        fprintf(stderr, "glhost: publishing frame %s: %s\n", path, strerror(errno));
        ok = false;
    }
    if (!ok && unlink(temporary) && errno != ENOENT) {
        fprintf(stderr, "glhost: removing frame %s: %s\n", temporary, strerror(errno));
    }
    free(row);
    return ok;
}

static void dump_frame(const char *path)
{
    if (!win) return;
    size_t n = (size_t)win_w * win_h;
    unsigned char *rgba = malloc(n * 4);
    if (!rgba) return;
    glReadPixels(0, 0, win_w, win_h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    if (!strcmp(path,frame_path())) {
        display_rgba=realloc(display_rgba,n*4);
        if(display_rgba)memcpy(display_rgba,rgba,n*4);
        video_composite(rgba,win_w,win_h);
    }
    write_frame(path, rgba, win_w, win_h, win_w * 4, true);
    free(rgba);
}

/* Decoder callbacks only retain images. Publication occurs on the bridge thread. */
static void video_display_changed(void)
{
    const char *cluster_path=getenv("MHI2_CLUSTER_VIDEO_FRAME");
    if(cluster_path && *cluster_path){
        bool active=false;
        for(unsigned i=0;i<4;i++){
            struct VideoDecoder *v=&video_decoders[i];
            if(v->used&&v->target&&!v->hidden)active=true;
            if(!v->used||!v->target||v->hidden||!v->rgba||v->published_frames==v->frames)continue;
            /* source is validated by video_attributes, and defaults to the
             * whole frame. Publish the negotiated viewport at native size. */
            int *s=v->source;
            if (write_frame(cluster_path,
                            v->rgba + ((size_t)s[1] * v->width + s[0]) * 4,
                            s[2], s[3], v->width * 4, false))
                v->published_frames = v->frames;
        }
        if(!active)unlink(cluster_path);
    }
    if(!frame_dump_enabled()||!display_rgba||!win_w||!win_h)return;
    size_t bytes=(size_t)win_w*win_h*4;
    uint8_t *rgba=malloc(bytes);if(!rgba)return;
    memcpy(rgba,display_rgba,bytes);video_composite(rgba,win_w,win_h);
    write_frame(frame_path(), rgba, win_w, win_h, win_w * 4, true);
    free(rgba);
}

struct CommandStream {
    int fd;
    size_t start, end;
    uint8_t bytes[64 * 1024];
};

static int read_full(struct CommandStream *stream, void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n) {
        size_t available = stream->end - stream->start;
        if (available) {
            size_t count = available < n ? available : n;
            memcpy(p, stream->bytes + stream->start, count);
            stream->start += count;
            p += count;
            n -= count;
            continue;
        }
        /* Large pixel payloads bypass the staging buffer once drained. */
        bool direct = n >= sizeof(stream->bytes);
        ssize_t r = read(stream->fd, direct ? p : stream->bytes,
                         direct ? n : sizeof(stream->bytes));
        if (r == 0) return -1;
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (direct) {
            p += r;
            n -= (size_t)r;
        } else {
            stream->start = 0;
            stream->end = r;
        }
    }
    return 0;
}

static void ensure_window(int w, int h)
{
    if (win) return;
    if (w <= 0 || w > 8192) w = DEFAULT_W;
    if (h <= 0 || h > 8192) h = DEFAULT_H;

    display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, NULL, NULL);
    EGLint major, minor, count;
    const EGLint attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE};
    const EGLint pb[] = {EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE};
    const EGLint ca[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    if (!eglInitialize(display, &major, &minor) ||
        !eglChooseConfig(display, attrs, &config, 1, &count) || !count) {
        fprintf(stderr, "EGL initialization failed %x\n", eglGetError()); exit(1);
    }
    surface = eglCreatePbufferSurface(display, config, pb);
    EGLContext ctx = eglCreateContext(display, config, EGL_NO_CONTEXT, ca);
    if (!eglMakeCurrent(display, surface, surface, ctx)) {
        fprintf(stderr, "EGL context failed %x\n", eglGetError()); exit(1);
    }
    win = 1;
    win_w = w; win_h = h;
    default_w = w; default_h = h;
    fprintf(stderr, "glhost: window %dx%d, GLES = %s | %s\n",
            w, h, glGetString(GL_VERSION), glGetString(GL_RENDERER));
    glViewport(0, 0, w, h);
    glClearColor(0.05f, 0.05f, 0.10f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (frame_dump_enabled()) dump_frame(frame_path());
    eglSwapBuffers(display, surface);
}

struct ClientSurface {
    uint32_t id;
    EGLSurface surface;
    int width, height;
};

struct Client {
    uint32_t pid, context_id, owner;
    EGLContext context;
    EGLSurface surface;
    GLuint *objects;
    struct BlendMetadata *blend;
    bool managed;
    struct ClientSurface targets[8], *draw, *read;
    struct PixmapImage *images;
    uint32_t saved_errors;
    struct Client *contexts[7];
};

static struct Client *current_client;

static bool client_storage(struct Client *c)
{
    c->objects = calloc(MAXOBJ, sizeof(GLuint));
    c->blend = calloc(1, sizeof(struct BlendMetadata));
    if (!c->objects || !c->blend) {
        free(c->objects);
        free(c->blend);
        c->objects = NULL;
        c->blend = NULL;
        return false;
    }
    return true;
}

static struct Client *client_context_at(struct Client *c, unsigned i)
{
    return i ? c->contexts[i - 1] : c;
}

static struct Client *client_context(struct Client *c, uint32_t id)
{
    for (unsigned i = 0; c && id && i < 8; i++) {
        struct Client *context = client_context_at(c, i);
        if (context && context->context_id == id) return context;
    }
    return NULL;
}

static struct Client *get_client(uint32_t pid)
{
    static struct Client clients[32];
    static unsigned count;
    ensure_window(0,0);
    unsigned i;
    for (i=0;i<count;i++) if (clients[i].pid==pid) break;
    if (i==count) {
        if (count==32) return NULL;
        const EGLint ca[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
        const EGLint pb[]={EGL_WIDTH,default_w,EGL_HEIGHT,default_h,EGL_NONE};
        clients[i].pid=pid;
        clients[i].context=eglCreateContext(display,config,EGL_NO_CONTEXT,ca);
        clients[i].surface=eglCreatePbufferSurface(display,config,pb);
        if (!client_storage(&clients[i]) ||
            clients[i].context==EGL_NO_CONTEXT ||
            clients[i].surface==EGL_NO_SURFACE) {
            if (clients[i].context) eglDestroyContext(display,clients[i].context);
            if (clients[i].surface) eglDestroySurface(display,clients[i].surface);
            free(clients[i].objects);
            free(clients[i].blend);
            memset(&clients[i],0,sizeof(clients[i]));
            return NULL;
        }
        count++;
    }
    return &clients[i];
}

static struct Client *bound_client(uint32_t pid, uint32_t thread)
{
    struct Client *c = get_client(pid);
    if (!c || !c->managed) return c;
    for (unsigned i = 0; thread && i < 8; i++) {
        struct Client *context = client_context_at(c, i);
        if (context && context->owner == thread) return context;
    }
    return NULL;
}

/* Separate guest processes have separate object namespaces and GL state. */
static void select_client(uint32_t pid)
{
    struct Client *c = bound_client(pid, request_thread);
    if (!c || (c->managed && (!c->context_id || !c->owner ||
                              c->owner != request_thread))) {
        fprintf(stderr, "glhost: missing/current-thread graphics context: "
                "pid=%u thread=%u context=%u owner=%u\n",
                pid, request_thread, c ? c->context_id : 0, c ? c->owner : 0);
        exit(1);
    }
    if (pid == current_pid && c == current_client) return;
    surface = c->managed ? (c->draw ? c->draw->surface : EGL_NO_SURFACE) :
                          c->surface;
    EGLSurface read = c->managed ?
        (c->read ? c->read->surface : EGL_NO_SURFACE) : surface;
    if (!eglMakeCurrent(display,surface,read,c->context)) {
        fprintf(stderr,"glhost: restoring client context failed: %x\n",eglGetError());
        exit(1);
    }
    win_w = c->managed ? (c->draw ? c->draw->width : 0) : default_w;
    win_h = c->managed ? (c->draw ? c->draw->height : 0) : default_h;
    gmap=c->objects;
    blend=c->blend;
    current_pid=pid;
    current_client=c;
}

#include "pixmap_images.h"
#include "egl_surfaces.h"
#include "astc_textures.h"

#include "native_frames.h"


static void pump_events(void) {}

static void check_shader(const char *what, GLuint obj, int is_prog)
{
    GLint ok = 1;
    if (is_prog) glGetProgramiv(obj, GL_LINK_STATUS, &ok);
    else         glGetShaderiv(obj, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024]; GLsizei n = 0;
        if (is_prog) glGetProgramInfoLog(obj, sizeof(log), &n, log);
        else         glGetShaderInfoLog(obj, sizeof(log), &n, log);
        log[n < (GLsizei)sizeof(log) ? n : (GLsizei)sizeof(log) - 1] = 0;
        fprintf(stderr, "glhost: %s FAILED: %s\n", what, log);
    }
}

/* Reject a broken command boundary before it reaches Mesa or object tables. */
#include "program_cache.h"

static int valid_record(uint32_t op,uint32_t len,const uint8_t *p)
{
    if (op == 145 || op == 146) {
        return pixmap_record(op, len, p);
    }
    if (op == 62) {
        return len == 20 || (len == 24 && u32(p, 20) <= 1);
    }
    static const int sizes[120] = {
        [1]=9,
        [2]=17,
        [3]=17,
        [4]=5,
        [5]=1,
        [6]=1,
        [7]=9,
        [9]=5,
        [10]=5,
        [11]=9,
        [12]=5,
        [13]=5,
        [16]=9,
        [19]=25,
        [20]=5,
        [21]=5,
        [29]=9,
        [30]=9,
        [31]=13,
        [32]=17,
        [33]=5,
        [34]=5,
        [35]=9,
        [36]=5,
        [37]=5,
        [38]=5,
        [39]=5,
        [40]=5,
        [42]=5,
        [43]=9,
        [51]=9,
        [53]=13,
        [54]=5,
        [55]=9,
        [57]=5,
        [58]=13,
        [60]=9,
        [61]=9,
        [62]=21,
        [63]=17,
        [64]=17,
        [65]=17,
        [66]=17,
        [67]=5,
        [68]=5,
        [69]=13,
        [70]=13,
        [71]=17,
        [72]=5,
        [73]=9,
        [75]=9,
        [76]=17,
        [77]=17,
        [84]=21,
        [85]=9,
        [86]=17,
        [87]=9,
        [102]=9,
        [103]=9,
        [104]=5,
        [105]=1,
        [106]=5,
        [107]=5,
        [108]=9,
        [109]=5,
        [119]=5
    };
    if(op==139)return len==24 && u32(p,0)<=5;
    if(op==140)return len==4 && u32(p,0)!=0;
    if(op==141)return len==0;
    if(op==142 || op==143)return astc_record_valid(op,len,p);
    if(op==144)return len>12 && len<=4*1024*1024+12 &&
        u32(p,0)<MAXOBJ && u32(p,8)==len-12;
    if(op==134)return (len==12 || (len==16 && u32(p,12)<=1)) && u32(p,4)>0 && u32(p,4)<=2048 && u32(p,8)>0 && u32(p,8)<=2048;
    if(op==135)return len>8 && len<=VIDEO_MAX_PACKET+8 && u32(p,4)==len-8;
    if(op==136)return len==68;
    if(op==137)return len==4;
    if(op==131)return len==28 && i32(p,8)>0 && i32(p,8)<=2048 &&
        i32(p,12)>0 && i32(p,12)<=2048 && u32(p,16)==GL_RGBA && u32(p,20)==GL_UNSIGNED_BYTE;
    if(op==132)return len==12 && u32(p,4)>0 && u32(p,4)<=2048 && u32(p,8)>0 && u32(p,8)<=2048;
    if(op==133)return len==4;
    if(op==128)return len>=24 && u32(p,0)<16 && u32(p,20)==len-24;
    if(op==130)return len>=8 && u32(p,0)<=3 && (uint64_t)u32(p,4)*4==len-8;
    if(op==129)return len>=16 && u32(p,12)==len-16 && (uint64_t)u32(p,4)*(u32(p,8)==GL_UNSIGNED_BYTE?1:u32(p,8)==GL_UNSIGNED_SHORT?2:4)==len-16;
    if(op==127)return len>=24 && u32(p,4)>0 && u32(p,4)<=2048 && u32(p,8)>0 && u32(p,8)<=2048 &&
        !(u32(p,4)&1) && !(u32(p,8)&1) && u32(p,12)>0 && u32(p,12)<=60 &&
        u32(p,16)>=64000 && u32(p,16)<=20000000 && u32(p,20)<=1 && (uint64_t)u32(p,4)*u32(p,8)*4==len-24;
    if(op==138)return len==12 && u32(p,0)<=16*1024*1024 &&
        (u32(p,4)==GL_UNSIGNED_BYTE || u32(p,4)==GL_UNSIGNED_SHORT || u32(p,4)==GL_UNSIGNED_INT);
    if(op==122 || op==126)return len==4;
    if(op==123 || op==124)return len==12 && u32(p,8)<=4096;
    if(op==125)return len==4 && u32(p,0)==1;
    if(op==120)return len==24 && i32(p,8)>0 && i32(p,8)<=2048 &&
        i32(p,12)>0 && i32(p,12)<=2048 && u32(p,16)==GL_RGBA && u32(p,20)==GL_UNSIGNED_BYTE;
    if(op>=120)return 0;
    if(sizes[op] && len!=(unsigned)sizes[op]-1)return 0;
    if(op==7 || op==10 || op==12 || op==13)return u32(p,0)<MAXOBJ;
    if(op==11)return u32(p,0)<MAXOBJ && u32(p,4)<MAXOBJ;
    if(op==15 || op==50 || op==78 || op==79) {
        if(len<4 || (len-4)%4 || u32(p,0)!=(len-4)/4)return 0;
        for(unsigned i=4;i<len;i+=4)if(u32(p,i)>=MAXOBJ)return 0;
        return 1;
    }
    if(op==8 || op==100 || op==101)return len>=8 && u32(p,0)<MAXOBJ && u32(p,4)==len-8;
    if(op==14)return len>=12 && u32(p,8)==len-12;
    if(op==17)return len>=12 && (len==12 || u32(p,4)==len-12);
    if(op==18)return len>=12 && u32(p,8)==len-12;
    if(op>=22 && op<=25)return len>=8 && (uint64_t)u32(p,4)*(op-21)*4==len-8;
    if(op>=26 && op<=28)return len>=12 && (uint64_t)u32(p,4)*(op-24)*(op-24)*4==len-12;
    if(op>=80 && op<=83)return len>=8 && (uint64_t)u32(p,4)*(op-79)*4==len-8;
    if(op==52 || op==56)return len>=36 && u32(p,32)==len-36;
    if(op==110) {
        if(len<12 || (uint64_t)u32(p,0)*4+u32(p,8)!=len-12)return 0;
        for(unsigned i=0;i<u32(p,0);i++)if(u32(p,12+4*i)>=MAXOBJ)return 0;
        return 1;
    }
    return sizes[op]!=0;
}

static void serve(int cfd)
{
    struct CommandStream stream = {.fd = cfd};
    uint8_t *p = NULL;
    size_t cap = 0;
    unsigned long frames = 0;
    uint32_t request_pid = 0;
    const char *capture_path=getenv("MHI2_GL_CAPTURE");
    FILE *capture = capture_path && *capture_path ? fopen(capture_path, "wb") : NULL;
    bool trace = getenv("MHI2_GL_TRACE") != NULL;
    bool dump_clients = getenv("MHI2_GL_DUMP_CLIENTS") != NULL;
    const char *slow_env = getenv("MHI2_GL_SLOW_MS");
    double slow_ms = slow_env ? atof(slow_env) : 0;
    struct timespec started = {0};
    uint32_t last_op = 0, last_pid = 0;
    /* MHI2_GL_OPSTATS=1: per-opcode handling time, reported every 10 s. */
    bool opstats = getenv("MHI2_GL_OPSTATS") != NULL;
    static double op_ms[256];
    static unsigned long op_count[256];
    uint32_t stat_op = 0;
    struct timespec stat_start = {0}, stat_report = {0};

    for (;;) {
        uint32_t hdr[2];
        if (opstats && stat_start.tv_sec) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            op_ms[stat_op & 255] += (now.tv_sec - stat_start.tv_sec) * 1e3 +
                                    (now.tv_nsec - stat_start.tv_nsec) / 1e6;
            op_count[stat_op & 255]++;
            stat_start.tv_sec = 0;
            if (!stat_report.tv_sec) stat_report = now;
            if (now.tv_sec - stat_report.tv_sec >= 10) {
                fprintf(stderr, "glhost opstats:");
                for (unsigned i = 0; i < 256; i++) {
                    if (op_count[i] && (op_ms[i] >= 5 || i >= 100))
                        fprintf(stderr, " op%u=%lux/%.0fms", i, op_count[i], op_ms[i]);
                    op_count[i] = 0; op_ms[i] = 0;
                }
                fprintf(stderr, "\n");
                stat_report = now;
            }
        }
        if (slow_ms > 0 && started.tv_sec) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double ms = (now.tv_sec - started.tv_sec) * 1e3 +
                        (now.tv_nsec - started.tv_nsec) / 1e6;
            if (ms >= slow_ms)
                fprintf(stderr, "glhost: slow op=%u pid=%u %.1fms\n",
                        last_op, last_pid, ms);
            started.tv_sec = 0;
        }
        if (read_full(&stream, hdr, 8) < 0) break;
        uint32_t op = hdr[0], len = hdr[1];
        if (len > 64 * 1024 * 1024) break;
        if (trace) fprintf(stderr, "glhost op=%u len=%u\n", op, len);
        if (len > cap) {
            cap = len + 4096;
            p = realloc(p, cap);
            if (!p) { fprintf(stderr, "glhost: OOM %u\n", len); break; }
        }
        if (len && read_full(&stream, p, len) < 0) break;
        if (opstats) {
            clock_gettime(CLOCK_MONOTONIC, &stat_start);
            stat_op = op;
        }
        if (slow_ms > 0) {
            clock_gettime(CLOCK_MONOTONIC, &started);
            last_op = op;
            last_pid = request_pid;
        }
        if(capture){fwrite(hdr,1,8,capture);fwrite(p,1,len,capture);fflush(capture);}
        /* Pre-merge Audi shims used 128 for a 12-byte query. Porsche 128
         * carries at least 24 bytes. Retain both wire formats without ambiguity. */
        if(op==128 && len==12)op=138;
        if(!valid_record(op,len,p)){
            fprintf(stderr,"Invalid graphics record: op=%u len=%u\n",op,len);
            break;
        }
        pump_events();
        /* Initialize dimensions before client selection creates a context. */
        if (op == 1) {
            ensure_window(i32(p, 0), i32(p, 4));
        }
        /* Encoder traffic has no GL state. Polling must not flush another
         * client's renderer by repeatedly making the encoder owner's context
         * current. Owner selection takes effect lazily for graphics commands. */
        if(op!=1 && op!=119 && op!=122 && op!=126 && op!=127 &&
           op!=139 && op!=140 && !(op>=134&&op<=137))select_client(request_pid);

        switch (op) {
        case 145: {
            uint32_t error = pixmap_control(current_client, p);
            if (error) fprintf(stderr, "glhost: pixmap action %u image %u "
                               "failed: %x\n", u32(p, 0), u32(p, 4), error);
            if (write(STDOUT_FILENO, &error, 4) != 4) return;
            break;
        }
        case 146: {
            size_t bytes = (size_t)u32(p, 12) * u32(p, 16) * 4;
            uint32_t *reply = calloc(1, bytes + 4);
            if (!reply) {
                fprintf(stderr, "glhost: pixmap readback allocation failed\n");
                return;
            }
            reply[0] = pixmap_read(current_client, p, reply + 1);
            if (reply[0]) fprintf(stderr, "glhost: pixmap readback failed: %x\n", reply[0]);
            size_t sent = 0;
            while (sent < bytes + 4) {
                ssize_t n = write(STDOUT_FILENO, (uint8_t *)reply + sent,
                                  bytes + 4 - sent);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) break;
                sent += n;
            }
            free(reply);
            if (sent != bytes + 4) {
                fprintf(stderr, "glhost: pixmap readback reply transport failed\n");
                return;
            }
            break;
        }
        case 144: {
            uint32_t error = cached_program(u32(p, 0), u32(p, 4), p + 12,
                                             u32(p, 8));
            if (write(STDOUT_FILENO, &error, 4) != 4) return;
            break;
        }
        case 141: {
            uint32_t supported = astc_supported();
            if (write(STDOUT_FILENO, &supported, 4) != 4) return;
            break;
        }
        case 142: case 143:
            astc_upload(op, p);
            break;
        case 134: case 135: case 136: case 137: {
            uint32_t status=0;
            if(op==134){status=video_open(request_pid,p);
                if(!status)video_find(request_pid,u32(p,0))->target=len==16?u32(p,12):0;}
            if(op==135)status=video_decode(request_pid,p);
            if(op==136)status=video_attributes(request_pid,p);
            if(op==137)video_close(request_pid,u32(p,0));
            if(write(STDOUT_FILENO,&status,4)!=4)return;
            break;
        }
        case 119: request_pid=u32(p,0); request_thread=0; break;
        case 140: request_thread=u32(p,0); break;
        case 139: {
            uint32_t error = client_egl_control(request_pid, p);
            if (error != EGL_SUCCESS)
                fprintf(stderr,"glhost: EGL action %u failed: %x\n",u32(p,0),error);
            if(write(STDOUT_FILENO,&error,4)!=4)return;
            break;
        }
        case 125: primary_display_pid=current_pid;break;
        case 122: encoder_close(request_pid,u32(p,0));break;
        case 123: case 124: {
            uint32_t count=u32(p,8),header[3]={0};
            char name[4096]={0};GLsizei length=0;GLint size=0;GLenum type=0;
            if(op==123)glGetActiveAttrib(M(u32(p,0)),u32(p,4),count,&length,&size,&type,name);
            else glGetActiveUniform(M(u32(p,0)),u32(p,4),count,&length,&size,&type,name);
            header[0]=length;header[1]=size;header[2]=type;
            if(write(STDOUT_FILENO,header,12)!=12)return;
            unsigned done=0;while(done<count){ssize_t n=write(STDOUT_FILENO,name+done,count-done);
                if(n<0&&errno==EINTR)continue;if(n<=0)return;done+=n;}
            break;
        }
        case 127: {
            uint32_t status=encoder_submit(request_pid,p);
            if(write(STDOUT_FILENO,&status,4)!=4)return;
            break;
        }
        case 126: {
            uint8_t *data;uint32_t count=encoder_poll(request_pid,u32(p,0),&data);
            if(write(STDOUT_FILENO,&count,4)!=4){free(data);return;}
            size_t off=0;while(data&&off<count){ssize_t n=write(STDOUT_FILENO,data+off,count-off);
                if(n<0&&errno==EINTR)continue;if(n<=0){free(data);return;}off+=n;}
            free(data);break;
        }
        case 132: {
            uint32_t hit=native_frame_upload(u32(p,0),u32(p,4),u32(p,8));
            if(write(STDOUT_FILENO,&hit,4)!=4)return;
            break;
        }
        case 133: native_frame_remove(u32(p,0));break;
        case 120: case 131: {
            size_t size=(size_t)u32(p,8)*u32(p,12)*4;
            unsigned char *pixels=malloc(size);if(!pixels)return;
            GLint alignment;glGetIntegerv(GL_PACK_ALIGNMENT,&alignment);
            glPixelStorei(GL_PACK_ALIGNMENT,1);
            fast_read_pixels(i32(p,0),i32(p,4),i32(p,8),i32(p,12),pixels);
            glPixelStorei(GL_PACK_ALIGNMENT,alignment);
            size_t done=0;
            while(done<size){ssize_t n=write(STDOUT_FILENO,pixels+done,size-done);
                if(n<0&&errno==EINTR)continue;
                if(n<=0){free(pixels);return;}done+=n;}
            if(op==131)native_frame_store(u32(p,24),u32(p,8),u32(p,12),pixels);
            else free(pixels);
            break;
        }
        case 1:
            break;
        case 2:  ensure_window(0, 0); glViewport(i32(p,0), i32(p,4), i32(p,8), i32(p,12)); break;
        case 3:  ensure_window(0, 0); { float c[4]; memcpy(c, p, 16);
                 glClearColor(c[0], c[1], c[2], c[3]); } break;
        case 4:  ensure_window(0, 0); menu_texture_reset_draw(); glClear(u32(p, 0)); break;
        case 5:  ensure_window(0, 0);
                 menu_texture_reset_draw();
                 if(frame_dump_enabled() &&
                    (!primary_display_pid || current_pid==primary_display_pid))
                     dump_frame(frame_path());
                 if(dump_clients){char path[128];snprintf(path,sizeof(path),"/tmp/mhi2-client-%u.ppm",current_pid);dump_frame(path);}
                 eglSwapBuffers(display, surface);
                 if (frames < 3 || (frames % 120) == 0)
                     fprintf(stderr, "glhost: %lu frames swapped\n", frames + 1);
                 frames++; break;
        case 6:  ensure_window(0, 0); glFinish(); break;

        case 7: { /* glCreateShader {id, type} */
            uint32_t id = u32(p,0), type = u32(p,4);
            blend->shader[id]=0;
            gmap[id] = glCreateShader(type); } break;
        case 8: { /* glShaderSource {id, len, src} */
            uint32_t id = u32(p,0); GLint sl = (GLint)u32(p,4);
            const GLchar *src = (const GLchar *)(p + 8);
            char path[128];snprintf(path,sizeof(path),"/tmp/mhi2-source-%u.glsl",id);
            FILE *f=fopen(path,"wb");if(f){fwrite(src,1,sl,f);fclose(f);}
            blend->shader[id]=0;
            glShaderSource(M(id), 1, &src, &sl); } break;
        case 9:  glCompileShader(M(u32(p,0))); check_shader("compile", M(u32(p,0)), 0); break;
        case 10:
            blend->program[u32(p,0)]=0;blend->fragment[u32(p,0)]=0;
            gmap[u32(p,0)] = glCreateProgram(); break;
        case 11: {
            GLint type=0;glGetShaderiv(M(u32(p,4)),GL_SHADER_TYPE,&type);
            if(type==GL_FRAGMENT_SHADER)blend->fragment[u32(p,0)]=u32(p,4);
            glAttachShader(M(u32(p,0)), M(u32(p,4))); break;
        }
        case 12:
            glLinkProgram(M(u32(p,0))); check_shader("link", M(u32(p,0)), 1);
            blend->program[u32(p,0)]=blend->shader[blend->fragment[u32(p,0)]];break;
        case 13: {
            uint32_t id=u32(p,0);GLint active=0;
            glUseProgram(M(id));glGetIntegerv(GL_CURRENT_PROGRAM,&active);
            if((GLuint)active==M(id))blend->active_program=id;break;
        }
        case 14: { /* glBindAttribLocation {pid, index, namelen, name} */
            uint32_t pid = u32(p,0), idx = u32(p,4), nl = u32(p,8);
            char nm[256]; uint32_t k = nl < 255 ? nl : 255; memcpy(nm, p+12, k); nm[k]=0;
            glBindAttribLocation(M(pid), idx, nm); } break;

        case 15: { /* glGenBuffers {n, ids[n]} */
            uint32_t n = u32(p,0);
            for (uint32_t i = 0; i < n; i++) { GLuint b; glGenBuffers(1, &b); gmap[u32(p,4+i*4)] = b; }
            } break;
        case 16: glBindBuffer(u32(p,0), M(u32(p,4))); break;
        case 17: { /* glBufferData {target, size, usage, data} */
            uint32_t tgt = u32(p,0), sz = u32(p,4), usage = u32(p,8);
            const void *data = (len >= 12 + sz && sz) ? (p + 12) : NULL;
            glBufferData(tgt, sz, data, usage); } break;
        case 18: { /* glBufferSubData {target, offset, size, data} */
            uint32_t tgt = u32(p,0), off = u32(p,4), sz = u32(p,8);
            glBufferSubData(tgt, off, sz, (len >= 12 + sz && sz) ? (p + 12) : NULL); } break;

        case 19: { /* Guest pointers are offsets into a bound VBO only. */
            GLint binding = 0;
            glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &binding);
            if (!binding) {
                fprintf(stderr, "glhost: vertex array lacks transferred buffer\n");
                return;
            }
            glVertexAttribPointer(u32(p,0), i32(p,4), u32(p,8), u32(p,12) ? GL_TRUE : GL_FALSE,
                                  i32(p,16), (const void *)(uintptr_t)u32(p,20)); break;
        }
        case 138: { /* Maximum EBO index for copying guest client attributes. */
            uint32_t maximum = UINT32_MAX, count = u32(p,0), type = u32(p,4), offset = u32(p,8);
            unsigned width = type == GL_UNSIGNED_BYTE ? 1 : (type == GL_UNSIGNED_SHORT ? 2 : (type == GL_UNSIGNED_INT ? 4 : 0));
            GLint size = 0;
            glGetBufferParameteriv(GL_ELEMENT_ARRAY_BUFFER, GL_BUFFER_SIZE, &size);
            void *(*map)(GLenum, GLintptr, GLsizeiptr, GLbitfield) = (void *)eglGetProcAddress("glMapBufferRange");
            GLboolean (*unmap)(GLenum) = (void *)eglGetProcAddress("glUnmapBuffer");
            if (width && count && map && unmap && (uint64_t)offset+(uint64_t)count*width <= (uint32_t)size) {
                const uint8_t *indices = map(GL_ELEMENT_ARRAY_BUFFER, offset, (size_t)count*width, 1 /* GL_MAP_READ_BIT */);
                if (indices) {
                    maximum = 0;
                    for (uint32_t i = 0; i < count; i++) {
                        uint32_t value = 0;
                        memcpy(&value, indices+(size_t)i*width, width);
                        if (value > maximum) maximum = value;
                    }
                    unmap(GL_ELEMENT_ARRAY_BUFFER);
                }
            }
            fwrite(&maximum, 4, 1, stdout); fflush(stdout);
            break;
        }
        case 20: glEnableVertexAttribArray(u32(p,0)); break;
        case 21: glDisableVertexAttribArray(u32(p,0)); break;

        case 22: glUniform1fv(i32(p,0), i32(p,4), (const GLfloat *)(p+8)); break;
        case 23: glUniform2fv(i32(p,0), i32(p,4), (const GLfloat *)(p+8)); break;
        case 24: glUniform3fv(i32(p,0), i32(p,4), (const GLfloat *)(p+8)); break;
        case 25: glUniform4fv(i32(p,0), i32(p,4), (const GLfloat *)(p+8)); break;
        case 26: glUniformMatrix2fv(i32(p,0), i32(p,4), u32(p,8)?GL_TRUE:GL_FALSE, (const GLfloat *)(p+12)); break;
        case 27: glUniformMatrix3fv(i32(p,0), i32(p,4), u32(p,8)?GL_TRUE:GL_FALSE, (const GLfloat *)(p+12)); break;
        case 28: glUniformMatrix4fv(i32(p,0), i32(p,4), u32(p,8)?GL_TRUE:GL_FALSE, (const GLfloat *)(p+12)); break;
        case 29: { float v; memcpy(&v, p+4, 4); glUniform1f(i32(p,0), v); } break;
        case 30: glUniform1i(i32(p,0), i32(p,4)); break;

        case 128: {
            unsigned index=u32(p,0),bytes=u32(p,20);GLint binding;
            glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&binding);
            void *data=realloc(blend->client_arrays[index],bytes);
            if(!data && bytes)exit(1);blend->client_arrays[index]=data;
            blend->client_array_bytes[index]=bytes;
            memcpy(data,p+24,bytes);glBindBuffer(GL_ARRAY_BUFFER,0);
            glVertexAttribPointer(index,i32(p,4),u32(p,8),u32(p,12),i32(p,16),data);
            glBindBuffer(GL_ARRAY_BUFFER,binding);break;
        }
        case 31: case 32: case 129: {
            GLuint menu_binding=menu_texture_begin_draw(op,p,len);
            unsigned mode=blend->program[blend->active_program];
            struct BlendState saved;
            begin_binary_blend(mode,&saved);
            if(op==31)glDrawArrays(u32(p,0),i32(p,4),i32(p,8));
            else if(op==129){GLint binding;glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING,&binding);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,0);glDrawElements(u32(p,0),i32(p,4),u32(p,8),p+16);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,binding);
            }else glDrawElements(u32(p,0),i32(p,4),u32(p,8),(const void *)(uintptr_t)u32(p,12));
            end_binary_blend(mode,&saved);
            if(menu_binding)glBindTexture(GL_TEXTURE_2D,menu_binding);
            break;
        }

        case 33: glEnable(u32(p,0)); break;
        case 34: glDisable(u32(p,0)); break;
        case 35: glBlendFunc(u32(p,0), u32(p,4)); break;
        case 36: glDepthFunc(u32(p,0)); break;
        case 37: glCullFace(u32(p,0)); break;
        case 38: glFrontFace(u32(p,0)); break;
        case 39: glDepthMask(u32(p,0) ? GL_TRUE : GL_FALSE); break;
        case 40: { float d; memcpy(&d, p, 4); glClearDepthf(d); } break;
        case 42: { float w; memcpy(&w, p, 4); glLineWidth(w); } break;
        case 43: { float f[2]; memcpy(f, p, 8); glPolygonOffset(f[0], f[1]); } break;

        case 130: { /* Delete texture/buffer/framebuffer/renderbuffer names. */
            if (len < 8 || u32(p,4) > (len - 8) / 4) break;
            unsigned kind = u32(p,0), count = u32(p,4);
            if (kind > 3) break;
            for (unsigned i = 0; i < count; i++) {
                unsigned id = u32(p,8+4*i);
                if (!id || id >= MAXOBJ || !gmap[id]) continue;
                GLuint object = gmap[id];
                switch (kind) {
                case 0: menu_texture_deleted(object); glDeleteTextures(1, &object); break;
                case 1: glDeleteBuffers(1, &object); break;
                case 2: glDeleteFramebuffers(1, &object); break;
                case 3: glDeleteRenderbuffers(1, &object); break;
                }
                gmap[id] = 0;
            }
            break;
        }

        case 50: { uint32_t n = u32(p,0);
            for (uint32_t i = 0; i < n; i++) { GLuint t; glGenTextures(1, &t); gmap[u32(p,4+i*4)] = t; }
            } break;
        case 51: glBindTexture(u32(p,0), M(u32(p,4))); break;
        case 52: { /* glTexImage2D {target,level,ifmt,w,h,border,fmt,type,size,data} */
            uint32_t sz = u32(p,32);
            if (menu_texture_upload(52,p,len)) break;
            glTexImage2D(u32(p,0), i32(p,4), i32(p,8), i32(p,12), i32(p,16), i32(p,20),
                         u32(p,24), u32(p,28), (sz && len >= 36 + sz) ? (p + 36) : NULL); } break;
        case 53: glTexParameteri(u32(p,0), u32(p,4), i32(p,8)); break;
        case 54: glActiveTexture(u32(p,0)); break;
        case 55: glPixelStorei(u32(p,0), i32(p,4)); break;
        case 56: { /* glTexSubImage2D {target,level,x,y,w,h,fmt,type,size,data} */
            uint32_t sz = u32(p,32);
            if (menu_texture_upload(56,p,len)) break;
            glTexSubImage2D(u32(p,0), i32(p,4), i32(p,8), i32(p,12), i32(p,16), i32(p,20),
                            u32(p,24), u32(p,28), (sz && len >= 36 + sz) ? (p + 36) : NULL); } break;
        case 57: glGenerateMipmap(u32(p,0)); break;
        case 58: { float v; memcpy(&v, p+8, 4); glTexParameterf(u32(p,0), u32(p,4), v); } break;

        case 110: {
            static unsigned binary_id;
            char path[128];
            uint32_t count=u32(p,0), format=u32(p,4), size=u32(p,8);
            snprintf(path,sizeof(path),"/tmp/mhi2-shader-%u-%x.bin",binary_id++,format);
            FILE *f=fopen(path,"wb");
            if(f){fwrite(p+12+count*4,1,size,f);fclose(f);}
            fprintf(stderr,"shader binary %s count=%u size=%u\n",path,count,size);
            const uint8_t *binary = p+12+count*4;
            uint64_t hash = UINT64_C(14695981039346656037);
            for (unsigned i=0;i<size;i++) hash=(hash^binary[i])*UINT64_C(1099511628211);
            bool matched=false;
            const struct ShaderTranslation *translation = count == 1 ? find_shader(hash, size) : NULL;
            if (translation) {
                GLuint shader=M(u32(p,12));
                blend->shader[u32(p,12)]=translation->blend;
                glShaderSource(shader,1,&translation->source,NULL);
                glCompileShader(shader);check_shader("translated shader",shader,0);
                matched=true;
            }
            if (!matched && count == 1) {
                unsigned mode;
                char *source = load_firmware_shader(hash, size, &mode);
                if (source) {
                    GLuint shader = M(u32(p,12));
                    blend->shader[u32(p,12)] = mode;
                    const char *text = source;
                    glShaderSource(shader, 1, &text, NULL);
                    glCompileShader(shader);
                    check_shader("firmware source shader", shader, 0);
                    free(source);
                    matched = true;
                }
            }
            if (!matched) fprintf(stderr,"UNSUPPORTED NVIDIA shader hash=%016llx\n",(unsigned long long)hash);

            break;
        }
        case 104: {
            GLint out[16]={0};
            GLenum pname = u32(p,0);
            if (pname != GL_COMPRESSED_TEXTURE_FORMATS &&
                pname != GL_SHADER_BINARY_FORMATS &&
                pname != GL_NUM_COMPRESSED_TEXTURE_FORMATS &&
                pname != GL_NUM_SHADER_BINARY_FORMATS) {
                glGetIntegerv(pname,out);
            }
            if(write(STDOUT_FILENO,out,sizeof(out)) != sizeof(out))return;
            break;
        }
        case 106: {
            GLboolean out[16]={0};
            GLenum pname = u32(p,0);
            if (pname != GL_COMPRESSED_TEXTURE_FORMATS &&
                pname != GL_SHADER_BINARY_FORMATS &&
                pname != GL_NUM_COMPRESSED_TEXTURE_FORMATS &&
                pname != GL_NUM_SHADER_BINARY_FORMATS) {
                glGetBooleanv(pname,out);
            }
            if(write(STDOUT_FILENO,out,sizeof(out))!=sizeof(out))return;
            break;
        }
        case 107: {
            GLfloat out[16]={0};
            GLenum pname = u32(p,0);
            if (pname != GL_COMPRESSED_TEXTURE_FORMATS &&
                pname != GL_SHADER_BINARY_FORMATS &&
                pname != GL_NUM_COMPRESSED_TEXTURE_FORMATS &&
                pname != GL_NUM_SHADER_BINARY_FORMATS) {
                glGetFloatv(pname,out);
            }
            if(write(STDOUT_FILENO,out,sizeof(out))!=sizeof(out))return;
            break;
        }
        case 108: {
            GLint out[3]={0};glGetShaderPrecisionFormat(u32(p,0),u32(p,4),out,out+2);
            if(write(STDOUT_FILENO,out,sizeof(out))!=sizeof(out))return;
            break;
        }
        case 105: {
            int32_t out=client_gl_error(current_client);
            if(write(STDOUT_FILENO,&out,4)!=4)return;
            break;
        }
        case 102: case 103: {
            GLint out = 0;
            if (op == 102) glGetShaderiv(M(u32(p,0)),u32(p,4),&out);
            else glGetProgramiv(M(u32(p,0)),u32(p,4),&out);
            if (write(STDOUT_FILENO,&out,4) != 4) return;
            break;
        }
        case 60: glBindFramebuffer(u32(p,0),M(u32(p,4))); break;
        case 61: glBindRenderbuffer(u32(p,0),M(u32(p,4))); break;
        case 62:
            glFramebufferTexture2D(u32(p,0),u32(p,4),u32(p,8),
                                    M(u32(p,12)),i32(p,16));
            if (len == 24) {
                struct Client *client = current_client;
                GLenum error = pixmap_framebuffer_origin(client, u32(p,0),
                                                         u32(p,20));
                if (error) {
                    fprintf(stderr, "glhost: framebuffer origin failed: %x\n", error);
                    client->saved_errors |= 1u << (error - 0x500);
                }
            }
            break;
        case 63: glRenderbufferStorage(u32(p,0),u32(p,4),i32(p,8),i32(p,12)); break;
        case 64: glFramebufferRenderbuffer(u32(p,0),u32(p,4),u32(p,8),M(u32(p,12))); break;
        case 65: glScissor(i32(p,0),i32(p,4),i32(p,8),i32(p,12)); break;
        case 66: glColorMask(u32(p,0),u32(p,4),u32(p,8),u32(p,12)); break;
        case 67: glClearStencil(i32(p,0)); break;
        case 68: glStencilMask(u32(p,0)); break;
        case 69: glStencilFunc(u32(p,0),i32(p,4),u32(p,8)); break;
        case 70: glStencilOp(u32(p,0),u32(p,4),u32(p,8)); break;
        case 71: glBlendFuncSeparate(u32(p,0),u32(p,4),u32(p,8),u32(p,12)); break;
        case 72: glBlendEquation(u32(p,0)); break;
        case 73: glBlendEquationSeparate(u32(p,0),u32(p,4)); break;
        case 75: glStencilMaskSeparate(u32(p,0),u32(p,4)); break;
        case 76: glStencilFuncSeparate(u32(p,0),u32(p,4),i32(p,8),u32(p,12)); break;
        case 77: glStencilOpSeparate(u32(p,0),u32(p,4),u32(p,8),u32(p,12)); break;
        case 78: {unsigned count=u32(p,0);for(unsigned i=0;i<count;i++){GLuint o;glGenFramebuffers(1,&o);gmap[u32(p,4+4*i)]=o;}break;}
        case 79: {unsigned count=u32(p,0);for(unsigned i=0;i<count;i++){GLuint o;glGenRenderbuffers(1,&o);gmap[u32(p,4+4*i)]=o;}break;}
        case 109: {uint32_t out=glCheckFramebufferStatus(u32(p,0));if(write(STDOUT_FILENO,&out,4)!=4)return;break;}
        case 80: glUniform1iv(i32(p,0),i32(p,4),(const GLint *)(p+8));break;
        case 81: glUniform2iv(i32(p,0),i32(p,4),(const GLint *)(p+8));break;
        case 82: glUniform3iv(i32(p,0),i32(p,4),(const GLint *)(p+8));break;
        case 83: glUniform4iv(i32(p,0),i32(p,4),(const GLint *)(p+8));break;
        case 84: {float v[4];memcpy(v,p+4,16);glVertexAttrib4fv(u32(p,0),v);break;}
        case 85: {float v[2];memcpy(v,p,8);glDepthRangef(v[0],v[1]);break;}
        case 86: {float v[4];memcpy(v,p,16);glBlendColor(v[0],v[1],v[2],v[3]);break;}
        case 87: glHint(u32(p,0),u32(p,4));break;
        case 100: case 101: { /* sync getters {pid, namelen, name} -> i32 */
            uint32_t pid = u32(p,0), nl = u32(p,4);
            char nm[256]; uint32_t k = nl < 255 ? nl : 255; memcpy(nm, p+8, k); nm[k]=0;
            GLint loc = (op == 100) ? glGetAttribLocation(M(pid), nm)
                                    : glGetUniformLocation(M(pid), nm);
            int32_t out = loc;
            if (write(STDOUT_FILENO, &out, 4) != 4) { fprintf(stderr, "glhost: getter reply failed\n"); }
            } break;

        default: /* unknown: drained, stay in sync */ break;
        }
    }
    for(unsigned i=0;i<4;i++)encoder_stop(&encoders[i]);
    if(capture)fclose(capture);
    free(p);
    fprintf(stderr, "glhost: client disconnected after %lu frames\n", frames);
}

int main(int argc, char **argv)
{
    signal(SIGPIPE,SIG_IGN);
    menu_texture_init();
    if (argc == 2 && strcmp(argv[1], "--check-shaders") == 0) {
        ensure_window(DEFAULT_W, DEFAULT_H);
        int failed = 0;
        for (size_t i = 0; i < sizeof(shaders) / sizeof(shaders[0]); i++) {
            GLuint shader = glCreateShader(strstr(shaders[i].source, "gl_Position") ?
                                           GL_VERTEX_SHADER : GL_FRAGMENT_SHADER);
            glShaderSource(shader, 1, &shaders[i].source, NULL);
            glCompileShader(shader);
            GLint compiled = 0;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
            check_shader("self-check", shader, 0);
            failed += !compiled;
            glDeleteShader(shader);
        }
        fprintf(stderr, "glhost: %zu translated shaders checked, %d failed\n",
                sizeof(shaders) / sizeof(shaders[0]), failed);
        return failed != 0;
    }
    serve(STDIN_FILENO);
    return 0;
}

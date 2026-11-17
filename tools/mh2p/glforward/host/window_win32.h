/* Optional live window for glhost on Windows (MH2P_GL_WINDOW=1).
 *
 * glhost renders offscreen (EGL pbuffer); this shows each published display frame in a
 * plain Win32 window on its own thread (GDI StretchDIBits, no second GL context). With
 * MH2P_GL_INPUT=host:port (a QEMU QMP socket on the MMX), left-button drags become
 * absolute pointer events via QMP input-send-event, which the mh2p-rstp chardev turns
 * into native RSTP keypanel touch records for the original hidcontroller. The mouse
 * wheel and a few keys are forwarded the same way.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef GLHOST_WINDOW_WIN32_H
#define GLHOST_WINDOW_WIN32_H

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <pthread.h>

static struct {
    int enabled, started;
    pthread_mutex_t lock;
    uint8_t *bgra;          /* top-down BGRA copy of the latest frame */
    int width, height;
    HWND hwnd;
    SOCKET qmp;
    int down;
} window = {.lock = PTHREAD_MUTEX_INITIALIZER, .qmp = INVALID_SOCKET};

static void qmp_drain(void)
{
    char sink[4096];
    u_long pending = 0;
    while (window.qmp != INVALID_SOCKET &&
           !ioctlsocket(window.qmp, FIONREAD, &pending) && pending) {
        if (recv(window.qmp, sink, sizeof(sink), 0) <= 0) {
            break;
        }
    }
}

static void qmp_send(const char *json)
{
    if (window.qmp == INVALID_SOCKET) {
        return;
    }
    qmp_drain();
    if (send(window.qmp, json, (int)strlen(json), 0) < 0) {
        fprintf(stderr, "glhost: QMP input socket lost\n");
        closesocket(window.qmp);
        window.qmp = INVALID_SOCKET;
    }
}

static void qmp_connect(void)
{
    const char *spec = getenv("MH2P_GL_INPUT");
    char host[256];
    const char *colon = spec ? strrchr(spec, ':') : NULL;
    if (!colon || colon == spec || (size_t)(colon - spec) >= sizeof(host)) {
        return;
    }
    memcpy(host, spec, colon - spec);
    host[colon - spec] = 0;
    struct addrinfo hints = {0}, *ai = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    for (int attempt = 0; attempt < 100; attempt++) {
        if (!getaddrinfo(host, colon + 1, &hints, &ai)) {
            SOCKET s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (s != INVALID_SOCKET && !connect(s, ai->ai_addr, (int)ai->ai_addrlen)) {
                freeaddrinfo(ai);
                char greeting[4096];
                recv(s, greeting, sizeof(greeting), 0);   /* {"QMP": ...} */
                window.qmp = s;
                qmp_send("{\"execute\":\"qmp_capabilities\"}\n");
                fprintf(stderr, "glhost: window input -> QMP %s\n", spec);
                return;
            }
            if (s != INVALID_SOCKET) {
                closesocket(s);
            }
            freeaddrinfo(ai);
        }
        Sleep(200);
    }
    fprintf(stderr, "glhost: QMP input %s unreachable; window is view-only\n", spec);
}

/* Window client coordinates -> QEMU absolute axis range 0..0x7fff. */
static void send_pointer(int x, int y, const char *button)
{
    RECT r;
    GetClientRect(window.hwnd, &r);
    int w = r.right > 1 ? r.right : 1, h = r.bottom > 1 ? r.bottom : 1;
    x = x < 0 ? 0 : x >= w ? w - 1 : x;
    y = y < 0 ? 0 : y >= h ? h - 1 : y;
    long ax = (long)x * 0x7fff / (w - 1 ? w - 1 : 1);
    long ay = (long)y * 0x7fff / (h - 1 ? h - 1 : 1);
    char json[512];
    int n = snprintf(json, sizeof(json),
        "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":["
        "{\"type\":\"abs\",\"data\":{\"axis\":\"x\",\"value\":%ld}},"
        "{\"type\":\"abs\",\"data\":{\"axis\":\"y\",\"value\":%ld}}", ax, ay);
    if (button) {
        n += snprintf(json + n, sizeof(json) - n,
            ",{\"type\":\"btn\",\"data\":{\"down\":%s,\"button\":\"left\"}}", button);
    }
    snprintf(json + n, sizeof(json) - n, "]}}\n");
    qmp_send(json);
}

static void send_simple(const char *event)
{
    char json[256];
    snprintf(json, sizeof(json),
             "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[%s]}}\n", event);
    qmp_send(json);
}

static void send_key(const char *qcode, int down)
{
    char event[160];
    snprintf(event, sizeof(event),
             "{\"type\":\"key\",\"data\":{\"down\":%s,\"key\":{\"type\":\"qcode\","
             "\"data\":\"%s\"}}}", down ? "true" : "false", qcode);
    send_simple(event);
}

static const char *key_qcode(WPARAM vk)
{
    switch (vk) {
    case VK_ESCAPE: return "esc";
    case VK_BACK: return "backspace";
    case VK_RETURN: return "ret";
    case VK_HOME: return "home";
    case VK_UP: return "up";
    case VK_DOWN: return "down";
    case VK_LEFT: return "left";
    case VK_RIGHT: return "right";
    case VK_PRIOR: return "pgup";
    case VK_NEXT: return "pgdn";
    case VK_VOLUME_UP: case VK_ADD: return "volumeup";
    case VK_VOLUME_DOWN: case VK_SUBTRACT: return "volumedown";
    case VK_VOLUME_MUTE: return "audiomute";
    default: return NULL;
    }
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r;
        GetClientRect(hwnd, &r);
        pthread_mutex_lock(&window.lock);
        if (window.bgra) {
            BITMAPINFO bi = {0};
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = window.width;
            bi.bmiHeader.biHeight = -window.height;   /* top-down */
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(dc, HALFTONE);
            StretchDIBits(dc, 0, 0, r.right, r.bottom, 0, 0, window.width, window.height,
                          window.bgra, &bi, DIB_RGB_COLORS, SRCCOPY);
        } else {
            FillRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
        }
        pthread_mutex_unlock(&window.lock);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        window.down = 1;
        send_pointer(x, y, "true");
        return 0;
    case WM_MOUSEMOVE:
        if (window.down) {
            send_pointer(x, y, NULL);
        }
        return 0;
    case WM_LBUTTONUP:
        if (window.down) {
            window.down = 0;
            send_pointer(x, y, "false");
            ReleaseCapture();
        }
        return 0;
    case WM_MOUSEWHEEL:
        send_simple((short)HIWORD(wp) > 0 ?
            "{\"type\":\"btn\",\"data\":{\"down\":true,\"button\":\"wheel-up\"}},"
            "{\"type\":\"btn\",\"data\":{\"down\":false,\"button\":\"wheel-up\"}}" :
            "{\"type\":\"btn\",\"data\":{\"down\":true,\"button\":\"wheel-down\"}},"
            "{\"type\":\"btn\",\"data\":{\"down\":false,\"button\":\"wheel-down\"}}");
        return 0;
    case WM_KEYDOWN:
    case WM_KEYUP: {
        const char *q = key_qcode(wp);
        if (q && !(msg == WM_KEYDOWN && (lp & (1 << 30)))) {
            send_key(q, msg == WM_KEYDOWN);
        }
        return 0;
    }
    case WM_CLOSE:
        ShowWindow(hwnd, SW_MINIMIZE);   /* keep rendering; glhost exits with the bridge */
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static void *window_thread(void *unused)
{
    (void)unused;
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "glhost-window";
    RegisterClassA(&wc);
    int w = window.width, h = window.height;
    const char *scale = getenv("MH2P_GL_WINDOW_SCALE");
    double s = scale ? atof(scale) : 0;
    if (s <= 0) {
        /* Fit within ~90% of the primary screen width. */
        int sw = GetSystemMetrics(SM_CXSCREEN);
        s = w > sw * 9 / 10 ? (double)(sw * 9 / 10) / w : 1.0;
    }
    RECT r = {0, 0, (LONG)(w * s), (LONG)(h * s)};
    DWORD style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRect(&r, style, FALSE);
    window.hwnd = CreateWindowA("glhost-window", "MH2P HMI (glhost)", style,
                                CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                                r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    if (!window.hwnd) {
        fprintf(stderr, "glhost: CreateWindow failed (%lu)\n", GetLastError());
        return NULL;
    }
    ShowWindow(window.hwnd, SW_SHOWNOACTIVATE);
    fprintf(stderr, "glhost: live window %dx%d (scale %.2f)\n", w, h, s);
    qmp_connect();
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return NULL;
}

/* Called with each frame written to MH2P_GL_FRAME (RGBA, possibly bottom-up).
 * Returns 1 when the window is enabled. */
static int window_publish(const uint8_t *rgba, int width, int height, int stride,
                          bool inverted)
{
    if (!window.enabled) {
        const char *e = getenv("MH2P_GL_WINDOW");
        window.enabled = e && *e == '1' ? 1 : -1;
    }
    if (window.enabled < 0 || width <= 0 || height <= 0) {
        return window.enabled > 0;
    }
    pthread_mutex_lock(&window.lock);
    if (window.width != width || window.height != height || !window.bgra) {
        free(window.bgra);
        window.bgra = malloc((size_t)width * height * 4);
        window.width = width;
        window.height = height;
    }
    if (window.bgra) {
        for (int y = 0; y < height; y++) {
            const uint8_t *src = rgba + (size_t)(inverted ? height - 1 - y : y) * stride;
            uint8_t *dst = window.bgra + (size_t)y * width * 4;
            for (int x = 0; x < width; x++) {
                dst[x * 4 + 0] = src[x * 4 + 2];
                dst[x * 4 + 1] = src[x * 4 + 1];
                dst[x * 4 + 2] = src[x * 4 + 0];
                dst[x * 4 + 3] = 255;
            }
        }
    }
    pthread_mutex_unlock(&window.lock);
    if (!window.started) {
        window.started = 1;
        pthread_t t;
        if (pthread_create(&t, NULL, window_thread, NULL) == 0) {
            pthread_detach(t);
        }
    } else if (window.hwnd) {
        InvalidateRect(window.hwnd, NULL, FALSE);
    }
    return 1;
}

/* With the window showing frames, the MH2P_GL_FRAME file is only a snapshot: write it at
 * most every MH2P_GL_FRAME_INTERVAL_MS (default 1000; 0 = every frame). Writing a 4 MB PPM
 * per frame sits in the synchronous bridge path, so the guest waits for it. */
static int window_frame_file_due(void)
{
    static ULONGLONG last;
    static long interval = -1;
    if (interval < 0) {
        const char *e = getenv("MH2P_GL_FRAME_INTERVAL_MS");
        interval = e && *e ? atol(e) : 1000;
    }
    ULONGLONG now = GetTickCount64();
    if (interval && last && now - last < (ULONGLONG)interval) {
        return 0;
    }
    last = now;
    return 1;
}
#else
static int window_publish(const uint8_t *rgba, int width, int height, int stride,
                          bool inverted)
{
    (void)rgba; (void)width; (void)height; (void)stride; (void)inverted;
    return 0;
}

static int window_frame_file_due(void)
{
    return 1;
}
#endif

#endif

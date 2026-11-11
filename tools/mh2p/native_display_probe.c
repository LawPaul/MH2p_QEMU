/* Local P2865 diagnostic using the original QNX display libraries. */
#include "graphics_smoke_abi.h"

static void text(const char *s)
{
    unsigned n = 0;
    while (s[n]) {
        n++;
    }
    write(1, s, n);
}

static void number(const char *label, unsigned value)
{
    char hex[11] = "0x00000000\n";
    for (unsigned i = 0; i < 8; i++) {
        hex[9 - i] = "0123456789abcdef"[(value >> (4 * i)) & 15];
    }
    text(label);
    write(1, hex, sizeof(hex));
}

static void *library(const char *name)
{
    void *value = dlopen(name, 2);
    if (!value) {
        text(name);
        text(": ");
        text(dlerror());
        text("\n");
        exit(2);
    }
    return value;
}

static void *symbol(void *lib, const char *name)
{
    void *value = dlsym(lib, name);
    if (!value) {
        text(name);
        text(": ");
        text(dlerror());
        text("\n");
        exit(2);
    }
    return value;
}

static int equal(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void configure_hdmi(void)
{
    struct vector {
        void *base;
        unsigned length;
    };
    void *libc = library("libc.so.3");
    int (*open_control)(const char *, int) = symbol(libc, "open64");
    int (*request)(int, unsigned, int, int, const struct vector *,
                   const struct vector *, int *) = symbol(libc, "devctlv");
    int (*close_control)(int) = symbol(libc, "close");
    unsigned count = 1;
    /* Original G35 main-head record, without the second CLUSTER record. */
    unsigned config[0x1b8 / 4] = {
        [0] = 0, [1] = 1920, [2] = 720,
        [3] = 40, [4] = 2, [5] = 40, [6] = 4,
        [7] = 48, [8] = 3, [9] = 89600,
        [0x2c / 4] = 1,
        [0xd4 / 4] = 24,
        [0x1b4 / 4] = 1,
    };
    struct vector vectors[] = {
        { &count, sizeof(count) },
        { config, sizeof(config) },
    };
    int fd = open_control("/dev/nvdisp-ctrl", 2);
    number("single-head control fd=", fd);
    if (fd < 0) {
        exit(6);
    }
    /* The original ioctl wrapper explicitly accepts one or two records. */
    int status = request(fd, 0x80040503, 2, 0, vectors, 0, 0);
    number("single-head configure status=", status);
    int closed = close_control(fd);
    number("single-head close status=", closed);
    if (status || closed) {
        exit(6);
    }
}

void probe_main(unsigned *stack) __attribute__((noreturn));

__attribute__((naked, noreturn)) void _start(void)
{
    __asm__ volatile("mov r0, sp\nb probe_main");
}

void probe_main(unsigned *stack)
{
    char **argv = (char **)(stack + 1);
    environ = (char **)(stack + stack[0] + 2);
    int hdmi_only = stack[0] == 2 && equal(argv[1], "--hdmi-only");
    if (stack[0] != 1 && !hdmi_only) {
        text("Usage: mh2p-native-display-probe [--hdmi-only]\n");
        exit(1);
    }
    if (hdmi_only) {
        configure_hdmi();
    }
    void *nvdc = library("/lib/libnvdc.so");
    void *(*open_display)(int) = symbol(nvdc, "nvdcOpen");
    int (*num_heads)(void *) = symbol(nvdc, "nvdcQueryNumHeads");
    void (*close_display)(void *) = symbol(nvdc, "nvdcClose");
    void *display = open_display(-1);
    number("nvdcOpen=", (unsigned)display);
    if (!display) {
        exit(3);
    }
    int heads = num_heads(display);
    number("nvdcQueryNumHeads=", heads);
    close_display(display);
    if (heads <= 0 || heads > 2 || (hdmi_only && heads != 1)) {
        exit(3);
    }
    void *wfd = library("/lib/libWFDnvidia.so");
    int (*enumerate)(int *, int, const int *) =
        symbol(wfd, "wfdEnumerateDevices");
    unsigned (*create)(int, const int *) = symbol(wfd, "wfdCreateDevice");
    int (*destroy)(unsigned) = symbol(wfd, "wfdDestroyDevice");
    int ids[4] = { 0 };
    int count = enumerate(ids, 4, 0);
    number("wfdEnumerateDevices=", count);
    if (count <= 0 || count > 4 || (hdmi_only && count != 1)) {
        exit(4);
    }
    for (int i = 0; i < count; i++) {
        number("deviceId=", ids[i]);
    }
    unsigned device = create(ids[0], 0);
    number("wfdCreateDevice=", device);
    if (!device) {
        exit(5);
    }
    int status = destroy(device);
    number("wfdDestroyDevice=", status);
    if (status) {
        exit(5);
    }
    text("MH2P_NATIVE_DISPLAY_PASS\n");
    exit(0);
}

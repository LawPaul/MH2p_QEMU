/*
 * Opt-in UARTD tunnel to original HMI diagnosis, not native hardware input.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "graphics_smoke_abi.h"

static void fail(const char *message)
{
    unsigned size = 0;
    while (message[size]) {
        size++;
    }
    write(2, message, size);
    exit(1);
}

static void *symbol(void *library, const char *name)
{
    void *address = dlsym(library, name);
    if (!address) {
        fail("diagnosis UART: required native libc/socket symbol missing\n");
    }
    return address;
}

void relay_main(void) __attribute__((noreturn));
__attribute__((naked, noreturn)) void _start(void)
{
    __asm__ volatile("b relay_main");
}

void relay_main(void)
{
    void *libc = dlopen("libc.so.3", 2);
    void *net = dlopen("libsocket.so.3", 2);
    if (!libc || !net) {
        fail("diagnosis UART: loading native libc/socket failed\n");
    }
    void *(*map_fn)(void *, unsigned, int, int, unsigned long long) =
        symbol(libc, "mmap_device_memory");
    int (*sleep_fn)(unsigned) = symbol(libc, "usleep");
    int (*socket_fn)(int, int, int) = symbol(net, "socket");
    int (*connect_fn)(int, const void *, unsigned) = symbol(net, "connect");
    int (*send_fn)(int, const void *, unsigned, int) = symbol(net, "send");
    int (*recv_fn)(int, void *, unsigned, int) = symbol(net, "recv");
    int (*poll_fn)(void *, unsigned, int) = symbol(libc, "poll");
    void (*perror_fn)(const char *) = symbol(libc, "perror");
    void *mapping = map_fn(0, 4096, 0xb00, 0, 0x70006000ULL);
    if (mapping == (void *)-1) {
        perror_fn("diagnosis UART mapping");
        exit(1);
    }
    /* Volatile accesses target UART MMIO, not shared RAM. */
    volatile u32 *uart = (volatile u32 *)mapping + 0x300 / sizeof(u32);
    uart[1] = 0;
    uart[3] = 0x80;
    uart[0] = 1;
    uart[1] = 0;
    uart[3] = 3;
    uart[2] = 0xc7;
    uart[4] = 3;
    if (write(1, "DIAGNOSIS_UART_READY\n", 21) != 21) {
        fail("diagnosis UART: publishing readiness failed\n");
    }
    int fd = -1;
    for (;;) {
        char request[2048];
        unsigned length = 0;
        for (;;) {
            unsigned status = uart[5];
            if (status & 0x1e) {
                fail("diagnosis UART: receive overrun/framing/break error\n");
            }
            if (!(status & 1)) {
                if (sleep_fn(1000)) {
                    perror_fn("diagnosis UART request wait");
                    exit(1);
                }
                continue;
            }
            if (length == sizeof(request)) {
                fail("diagnosis UART: oversized request\n");
            }
            request[length++] = uart[0];
            if (request[length - 1] == '\n') {
                break;
            }
        }
        if (fd < 0) {
            const unsigned char address[16] = {
                16, 2, 0x05, 0xdd, 127, 0, 0, 1,
            };
            fd = socket_fn(2, 1, 0);
            if (fd < 0 || connect_fn(fd, address, sizeof(address))) {
                perror_fn("diagnosis UART original service connect");
                exit(1);
            }
        }
        for (unsigned sent = 0; sent < length;) {
            int count = send_fn(fd, request + sent, length - sent, 0);
            if (count <= 0) {
                perror_fn("diagnosis UART original service send");
                exit(1);
            }
            sent += count;
        }
        static char reply[65537];
        unsigned received = 0;
        while (!received || reply[received - 1] != '\n') {
            struct {
                int fd;
                short events, revents;
            } ready = {fd, 1, 0};
            if (received == sizeof(reply) ||
                poll_fn(&ready, 1, 5000) <= 0 || !(ready.revents & 1)) {
                fail("diagnosis UART: missing or oversized original reply\n");
            }
            int count = recv_fn(fd, reply + received,
                                sizeof(reply) - received, 0);
            if (count <= 0) {
                perror_fn("diagnosis UART original service recv");
                exit(1);
            }
            for (unsigned i = received; i + 1 < received + count; i++) {
                if (reply[i] == '\n') {
                    fail("diagnosis UART: unexpected extra original reply\n");
                }
            }
            received += count;
        }
        for (unsigned i = 0; i < received; i++) {
            unsigned waits = 0;
            while (!(uart[5] & 0x20)) {
                if (++waits > 10000 || sleep_fn(1000)) {
                    fail("diagnosis UART: transmit timeout\n");
                }
            }
            uart[0] = (unsigned char)reply[i];
        }
    }
}

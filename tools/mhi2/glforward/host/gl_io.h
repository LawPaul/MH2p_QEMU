/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Renderer transport I/O.  POSIX keeps plain read()/write() on the
 * stdin/stdout FIFOs.  Windows (ANGLE build) has no FIFOs: with
 * MHI2_GL_CONNECT=host:port the renderer connects to QEMU's
 * "-chardev socket,id=glbridge,...,server=on" and uses that socket for
 * both directions; otherwise it falls back to binary stdin/stdout.
 */
#ifndef GL_IO_H
#define GL_IO_H

#ifndef _WIN32
#include <unistd.h>
#define gl_io_read(fd, buf, n) read((fd), (buf), (n))
#define gl_io_write(fd, buf, n) write((fd), (buf), (n))
static inline void gl_io_init(void) { }
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static SOCKET gl_io_socket = INVALID_SOCKET;

static void gl_io_init(void)
{
    const char *spec = getenv("MHI2_GL_CONNECT");
    char host[256];
    const char *colon;
    struct addrinfo hints = {0}, *res = NULL;
    WSADATA wsa;

    _setmode(0, _O_BINARY);
    _setmode(1, _O_BINARY);
    if (!spec || !*spec) {
        return;
    }
    colon = strrchr(spec, ':');
    if (!colon || colon == spec || (size_t)(colon - spec) >= sizeof(host)) {
        fprintf(stderr, "glhost: MHI2_GL_CONNECT must be host:port\n");
        exit(1);
    }
    memcpy(host, spec, colon - spec);
    host[colon - spec] = 0;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) {
        fprintf(stderr, "glhost: WSAStartup failed\n");
        exit(1);
    }
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    for (int attempt = 0; attempt < 600; attempt++) {
        if (!getaddrinfo(host, colon + 1, &hints, &res)) {
            for (struct addrinfo *a = res; a; a = a->ai_next) {
                SOCKET s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
                if (s == INVALID_SOCKET) {
                    continue;
                }
                if (!connect(s, a->ai_addr, (int)a->ai_addrlen)) {
                    BOOL on = TRUE;
                    int size = 4 * 1024 * 1024;
                    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *)&on, sizeof(on));
                    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (char *)&size, sizeof(size));
                    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (char *)&size, sizeof(size));
                    gl_io_socket = s;
                    break;
                }
                closesocket(s);
            }
            freeaddrinfo(res);
        }
        if (gl_io_socket != INVALID_SOCKET) {
            fprintf(stderr, "glhost: connected to %s\n", spec);
            return;
        }
        Sleep(100);
    }
    fprintf(stderr, "glhost: cannot connect to %s\n", spec);
    exit(1);
}

/* POSIX rename() replaces atomically; MSVCRT's fails if the target exists. */
static int gl_io_rename(const char *from, const char *to)
{
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
}
#define rename gl_io_rename

static long gl_io_read(int fd, void *buf, size_t n)
{
    if (gl_io_socket != INVALID_SOCKET) {
        int r = recv(gl_io_socket, buf, n > 0x40000000 ? 0x40000000 : (int)n, 0);
        if (r < 0) {
            errno = EIO;
        }
        return r;
    }
    return _read(fd, buf, n > 0x40000000 ? 0x40000000 : (unsigned)n);
}

static long gl_io_write(int fd, const void *buf, size_t n)
{
    if (gl_io_socket != INVALID_SOCKET) {
        int r = send(gl_io_socket, buf, n > 0x40000000 ? 0x40000000 : (int)n, 0);
        if (r < 0) {
            errno = EIO;
        }
        return r;
    }
    return _write(fd, buf, n > 0x40000000 ? 0x40000000 : (unsigned)n);
}
#endif

#endif

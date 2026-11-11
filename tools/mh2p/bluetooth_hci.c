/*
 * Local H4 controller endpoint using the upstream BlueZ emulator.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

#include "emulator/btdev.h"
#include "src/shared/ecc.h"
#include "src/shared/mainloop.h"

#define MAX_CLIENTS 16
#define MAX_FRAME UINT16_MAX
#define MAX_OUTPUT (1024 * 1024)
#define OUTPUT_PAUSE (MAX_OUTPUT / 2)

struct client {
    int fd;
    struct btdev *dev;
    uint8_t input[MAX_FRAME];
    size_t received, expected;
    uint8_t output[MAX_OUTPUT];
    size_t output_start, output_end;
    bool failed, read_closed;
    uint8_t private_key[32];
    bool key_valid, le_meta_enabled;
    uint8_t le_event_mask[8];
};

static struct client *clients[MAX_CLIENTS];

static int watch_client(struct client *client)
{
    uint32_t events = 0;

    if (!client->read_closed &&
        client->output_end - client->output_start < OUTPUT_PAUSE) {
        events |= EPOLLIN;
    }
    if (client->output_start != client->output_end) {
        events |= EPOLLOUT;
    }
    if (mainloop_modify_fd(client->fd, events) < 0) {
        fprintf(stderr, "Cannot update H4 client events\n");
        client->failed = true;
        return -1;
    }
    return 0;
}

static bool flush_output(struct client *client)
{
    while (client->output_start < client->output_end) {
        ssize_t size = send(client->fd, client->output + client->output_start,
                            client->output_end - client->output_start,
                            MSG_DONTWAIT | MSG_NOSIGNAL);
        if (size < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            perror("Cannot send H4 controller response");
            client->failed = true;
            return false;
        }
        if (!size) {
            fprintf(stderr, "Zero-length H4 controller send\n");
            client->failed = true;
            return false;
        }
        client->output_start += size;
    }
    if (client->output_start == client->output_end) {
        client->output_start = client->output_end = 0;
    }
    return watch_client(client) == 0;
}

static void send_response(const struct iovec *iov, int count, void *opaque)
{
    struct client *client = opaque;
    size_t pending = client->output_end - client->output_start;
    size_t total = 0;
    int i;

    if (client->failed) {
        return;
    }
    for (i = 0; i < count; i++) {
        if (iov[i].iov_len > MAX_OUTPUT - pending - total) {
            fprintf(stderr, "H4 controller output capacity exceeded\n");
            client->failed = true;
            return;
        }
        total += iov[i].iov_len;
    }
    if (total > MAX_OUTPUT - client->output_end) {
        memmove(client->output, client->output + client->output_start, pending);
        client->output_start = 0;
        client->output_end = pending;
    }
    for (i = 0; i < count; i++) {
        memcpy(client->output + client->output_end, iov[i].iov_base,
               iov[i].iov_len);
        client->output_end += iov[i].iov_len;
    }
    flush_output(client);
}

static void le_crypto_event(struct client *client, uint8_t subevent,
                            uint8_t status, const uint8_t *value, size_t size)
{
    uint8_t header[] = { 4, 0x3e, size + 2, subevent, status };
    struct iovec iov[] = {
        { .iov_base = header, .iov_len = sizeof(header) },
        { .iov_base = (void *)value, .iov_len = size },
    };

    if (client->le_meta_enabled &&
        (client->le_event_mask[(subevent - 1) / 8] &
         (1 << ((subevent - 1) % 8)))) {
        send_response(iov, 2, client);
    }
}

static void controller_command(uint16_t opcode, const void *data, uint8_t len,
                               btdev_callback callback, void *opaque)
{
    struct client *client = opaque;
    const uint8_t *bytes = data;
    uint8_t value[64] = { 0 };
    uint8_t status = 0;

    switch (opcode) {
    case 0x0c03: /* Reset */
        if (len) {
            status = 0x12;
            btdev_command_complete(callback, &status, sizeof(status));
            return;
        }
        explicit_bzero(client->private_key, sizeof(client->private_key));
        client->key_valid = false;
        client->le_meta_enabled = false;
        memset(client->le_event_mask, 0, sizeof(client->le_event_mask));
        client->le_event_mask[0] = 0x1f;
        break;
    case 0x0c01: /* Set Event Mask */
    case 0x2001: /* LE Set Event Mask */
        if (len != 8) {
            status = 0x12; /* Invalid HCI Command Parameters */
            btdev_command_complete(callback, &status, sizeof(status));
            return;
        }
        if (opcode == 0x0c01) {
            client->le_meta_enabled = (bytes[7] & 0x20) != 0;
        } else {
            memcpy(client->le_event_mask, bytes, sizeof(client->le_event_mask));
        }
        break;
    case 0x2025: /* LE Read Local P-256 Public Key */
        if (len) {
            btdev_command_status(callback, 0x12);
            return;
        }
        btdev_command_status(callback, 0);
        explicit_bzero(client->private_key, sizeof(client->private_key));
        client->key_valid = ecc_make_key(value, client->private_key);
        if (!client->key_valid) {
            fprintf(stderr, "Cannot generate controller P-256 key\n");
            explicit_bzero(client->private_key, sizeof(client->private_key));
            explicit_bzero(value, sizeof(value));
            status = 0x03; /* Hardware Failure */
        }
        le_crypto_event(client, 0x08, status, value, sizeof(value));
        explicit_bzero(value, sizeof(value));
        return;
    case 0x2026: /* LE Generate DHKey */
        if (len != 64 || !ecc_valid_public_key(bytes)) {
            btdev_command_status(callback, 0x12);
            return;
        }
        if (!client->key_valid) {
            btdev_command_status(callback, 0x0c); /* Command Disallowed */
            return;
        }
        btdev_command_status(callback, 0);
        if (!ecdh_shared_secret(bytes, client->private_key, value)) {
            fprintf(stderr, "Cannot calculate controller P-256 shared secret\n");
            explicit_bzero(value, sizeof(value));
            status = 0x03;
        }
        le_crypto_event(client, 0x09, status, value, 32);
        explicit_bzero(value, sizeof(value));
        return;
    }
    btdev_command_default(callback);
}

static void destroy_client(void *opaque)
{
    struct client *client = opaque;
    int i;

    for (i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] == client) {
            clients[i] = NULL;
            break;
        }
    }
    btdev_destroy(client->dev);
    explicit_bzero(client->private_key, sizeof(client->private_key));
    close(client->fd);
    free(client);
}

static bool receive_bytes(struct client *client, const uint8_t *data,
                          size_t size)
{
    while (size) {
        size_t count = client->expected - client->received;

        if (count > size) {
            count = size;
        }
        memcpy(client->input + client->received, data, count);
        client->received += count;
        data += count;
        size -= count;
        if (client->received != client->expected) {
            continue;
        }
        if (client->received == 1) {
            switch (client->input[0]) {
            case 1:
                client->expected = 4;
                break;
            case 2:
            case 5:
                client->expected = 5;
                break;
            default:
                fprintf(stderr, "Unsupported H4 packet type 0x%02x\n",
                        client->input[0]);
                return false;
            }
            continue;
        }
        if ((client->input[0] == 1 && client->received == 4) ||
            (client->input[0] != 1 && client->received == 5)) {
            size_t payload = client->input[0] == 1 ? client->input[3] :
                client->input[3] | ((size_t)client->input[4] << 8);

            if (client->input[0] == 5) {
                payload &= 0x3fff;
            }
            if (payload > MAX_FRAME - client->received) {
                fprintf(stderr, "H4 packet exceeds controller frame capacity\n");
                return false;
            }
            client->expected += payload;
            if (payload) {
                continue;
            }
        }
        btdev_receive_h4(client->dev, client->input, client->received);
        if (client->failed) {
            return false;
        }
        client->received = 0;
        client->expected = 1;
    }
    return true;
}

static void client_event(int fd, uint32_t events, void *opaque)
{
    struct client *client = opaque;
    uint8_t data[4096];
    ssize_t size;

    if (client->failed || events & EPOLLERR) {
        mainloop_remove_fd(fd);
        return;
    }
    if (events & (EPOLLOUT | EPOLLHUP) && !flush_output(client)) {
        mainloop_remove_fd(fd);
        return;
    }
    if (client->read_closed) {
        if (client->output_start == client->output_end) {
            mainloop_remove_fd(fd);
        }
        return;
    }
    if (events & (EPOLLIN | EPOLLHUP)) {
        do {
            size = recv(fd, data, sizeof(data), MSG_DONTWAIT);
        } while (size < 0 && errno == EINTR);
        if (size < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("Cannot receive H4 controller data");
                mainloop_remove_fd(fd);
            }
        } else if (!size) {
            if (client->received) {
                fprintf(stderr, "H4 connection closed with incomplete packet\n");
            }
            client->read_closed = true;
            if (client->output_start == client->output_end ||
                watch_client(client) < 0) {
                mainloop_remove_fd(fd);
            }
        } else if (!receive_bytes(client, data, size)) {
            mainloop_remove_fd(fd);
        }
    }
}

static void accept_client(int fd, uint32_t events, void *opaque)
{
    struct client *client;
    int peer, i;

    if (events & (EPOLLERR | EPOLLHUP)) {
        fprintf(stderr, "H4 listening socket failed\n");
        mainloop_exit_failure();
        return;
    }
    peer = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (peer < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            perror("Cannot accept H4 controller client");
        }
        return;
    }
    for (i = 0; i < MAX_CLIENTS && clients[i]; i++) {
    }
    if (i == MAX_CLIENTS) {
        fprintf(stderr, "H4 controller client capacity exceeded\n");
        close(peer);
        return;
    }
    client = calloc(1, sizeof(*client));
    if (!client) {
        perror("Cannot allocate H4 controller client");
        close(peer);
        return;
    }
    client->fd = peer;
    client->expected = 1;
    client->dev = btdev_create(BTDEV_TYPE_BREDRLE, 0x42 + i);
    if (!client->dev) {
        fprintf(stderr, "Cannot create BlueZ controller\n");
        close(peer);
        free(client);
        return;
    }
    if (mainloop_add_fd(peer, EPOLLIN, client_event, client,
                       destroy_client) < 0) {
        fprintf(stderr, "Cannot register H4 controller client\n");
        destroy_client(client);
        return;
    }
    clients[i] = client;
    client->le_event_mask[0] = 0x1f;
    btdev_set_send_handler(client->dev, send_response, client);
    btdev_set_command_handler(client->dev, controller_command, client);
}

static void stop_controller(int signum, void *opaque)
{
    mainloop_quit();
}

static int remove_socket(const char *path, const struct stat *bound)
{
    struct stat status;

    if (lstat(path, &status) < 0) {
        if (errno == ENOENT) {
            return 0;
        }
        perror("Cannot inspect controller socket for cleanup");
        return -1;
    }
    if (status.st_dev == bound->st_dev && status.st_ino == bound->st_ino &&
        unlink(path) < 0) {
        perror("Cannot remove controller socket");
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    struct stat status, bound;
    int fd, result, i;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s SOCKET_PATH\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (!argv[1][0] ||
        strlen(argv[1]) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        fprintf(stderr, "Invalid UNIX socket path length\n");
        return EXIT_FAILURE;
    }
    if (lstat(argv[1], &status) == 0) {
        fprintf(stderr, "Refusing to replace existing socket path: %s\n", argv[1]);
        return EXIT_FAILURE;
    }
    if (errno != ENOENT) {
        fprintf(stderr, "Cannot inspect socket path: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }
    umask(0077);
    mainloop_init();
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        perror("Cannot create H4 listening socket");
        return EXIT_FAILURE;
    }
    strcpy(address.sun_path, argv[1]);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Cannot bind H4 listening socket");
        close(fd);
        return EXIT_FAILURE;
    }
    if (lstat(argv[1], &bound) < 0) {
        perror("Cannot inspect created controller socket");
        close(fd);
        return EXIT_FAILURE;
    }
    if (listen(fd, MAX_CLIENTS) < 0 ||
        mainloop_add_fd(fd, EPOLLIN, accept_client, NULL, NULL) < 0) {
        perror("Cannot listen for H4 controller clients");
        close(fd);
        remove_socket(argv[1], &bound);
        return EXIT_FAILURE;
    }
    printf("BlueZ H4 controller listening at %s\n", argv[1]);
    fflush(stdout);
    result = mainloop_run_with_signal(stop_controller, NULL);
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i]) {
            mainloop_remove_fd(clients[i]->fd);
        }
    }
    mainloop_remove_fd(fd);
    close(fd);
    if (remove_socket(argv[1], &bound) < 0) {
        return EXIT_FAILURE;
    }
    return result;
}

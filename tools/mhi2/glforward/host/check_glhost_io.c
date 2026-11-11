/* Command stream framing and exact preview bytes, without a guest boot.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define main glhost_main
#include "glhost.c"
#undef main
#include <assert.h>
#include <pthread.h>

struct TestWriter {
    int fd;
    const uint8_t *bytes;
    size_t size;
};

static void *write_commands(void *opaque)
{
    struct TestWriter *writer = opaque;
    size_t at = 0;

    while (at < writer->size) {
        size_t count = (at % 8191) + 1;
        if (count > writer->size - at) count = writer->size - at;
        ssize_t written = write(writer->fd, writer->bytes + at, count);
        if (written < 0 && errno == EINTR) continue;
        assert(written > 0);
        at += written;
    }
    assert(!close(writer->fd));
    return NULL;
}

static void check_stream(void)
{
    enum { SIZE = 2 * 1024 * 1024 + 97 };
    uint8_t *source = malloc(SIZE), *received = malloc(SIZE);
    assert(source && received);
    for (unsigned i = 0; i < SIZE; i++) source[i] = i * 31 + i / 256;
    int pair[2];
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    struct CommandStream stream = {.fd = pair[0]};
    struct TestWriter writer = {pair[1], source, SIZE};
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, write_commands, &writer));
    assert(!read_full(&stream, received, 0));
    for (unsigned i = 0; i < 100; i++)
        assert(!read_full(&stream, received + i * 8, 8));
    assert(!read_full(&stream, received + 800, SIZE - 817));
    assert(!read_full(&stream, received + SIZE - 17, 17));
    assert(!memcmp(source, received, SIZE));
    assert(read_full(&stream, received, 1) == -1);
    assert(!pthread_join(thread, NULL));
    assert(!close(pair[0]));
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    stream = (struct CommandStream){.fd = pair[0]};
    assert(write(pair[1], source, 7) == 7);
    assert(!close(pair[1]));
    assert(read_full(&stream, received, 8) == -1);
    assert(!close(pair[0]));
    free(source);
    free(received);
}

static void check_frame(const char *path, bool inverted)
{
    /* Two odd-width rows with padding; alpha and padding must not leak. */
    const uint8_t rgba[] = {
        1, 2, 3, 99, 4, 5, 6, 99, 7, 8, 9, 99, 255, 255, 255, 255,
        10, 11, 12, 88, 13, 14, 15, 88, 16, 17, 18, 88, 255, 255, 255, 255,
    };
    uint8_t actual[64], expected[18];
    for (unsigned y = 0; y < 2; y++)
        for (unsigned x = 0; x < 3; x++)
            memcpy(expected + (y * 3 + x) * 3,
                   rgba + (inverted ? 1 - y : y) * 16 + x * 4, 3);
    assert(write_frame(path, rgba, 3, 2, 16, inverted));
    FILE *file = fopen(path, "rb");
    assert(file);
    size_t bytes = fread(actual, 1, sizeof(actual), file);
    const char header[] = "P6\n3 2\n255\n";
    assert(bytes == sizeof(header) - 1 + sizeof(expected));
    assert(!memcmp(actual, header, sizeof(header) - 1));
    assert(!memcmp(actual + sizeof(header) - 1, expected, sizeof(expected)));
    assert(!fclose(file));
}

int main(void)
{
    check_stream();
    char directory[] = "/tmp/glhost-io-XXXXXX", path[256];
    assert(mkdtemp(directory));
    assert(snprintf(path, sizeof(path), "%s/frame.ppm", directory) > 0);
    check_frame(path, false);
    check_frame(path, true);
    assert(!setenv("MHI2_GL_NO_FRAME_DUMP", "1", 1));
    assert(!frame_dump_enabled());
    assert(!setenv("MHI2_GL_NO_FRAME_DUMP", "0", 1));
    assert(frame_dump_enabled());
    assert(!unsetenv("MHI2_GL_NO_FRAME_DUMP"));
    assert(frame_dump_enabled());
    assert(!unlink(path));
    assert(!rmdir(directory));
    puts("glhost I/O checks passed");
    return 0;
}

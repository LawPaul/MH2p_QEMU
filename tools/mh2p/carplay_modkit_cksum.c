/* SPDX-License-Identifier: GPL-2.0-or-later */
/* P2865 has no cksum utility; this implements POSIX cksum on standard input. */
extern int read(int fd, void *buffer, unsigned size);
extern int write(int fd, const void *buffer, unsigned size);
extern void exit(int status) __attribute__((noreturn));

static unsigned table[256];

static unsigned update(unsigned crc, unsigned byte)
{
    return (crc << 8) ^ table[(crc >> 24) ^ byte];
}

static char *decimal(char *end, unsigned long long value)
{
    do {
        *--end = '0' + value % 10;
        value /= 10;
    } while (value);
    return end;
}

static void fail(const char *message)
{
    unsigned size = 0;
    while (message[size]) {
        size++;
    }
    write(2, message, size);
    exit(1);
}

void checksum_main(void) __attribute__((noreturn));

#ifdef MH2P_HOST_TEST
int main(void)
{
    checksum_main();
}
#else
__attribute__((naked, noreturn)) void _start(void)
{
    __asm__ volatile("b checksum_main");
}
#endif

void checksum_main(void)
{
    for (unsigned byte = 0; byte < 256; byte++) {
        unsigned crc = byte << 24;
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (crc << 1) ^ ((crc & 0x80000000U) ? 0x04c11db7U : 0);
        }
        table[byte] = crc;
    }
    unsigned crc = 0;
    unsigned long long length = 0;
    unsigned char buffer[16384];
    for (;;) {
        int count = read(0, buffer, sizeof(buffer));
        if (count < 0) {
            fail("CarPlay_Multitouch ERROR: checksum input read failed\n");
        }
        if (!count) {
            break;
        }
        if (length + (unsigned)count < length) {
            fail("CarPlay_Multitouch ERROR: checksum length overflow\n");
        }
        length += (unsigned)count;
        for (int index = 0; index < count; index++) {
            crc = update(crc, buffer[index]);
        }
    }
    for (unsigned long long remaining = length; remaining; remaining >>= 8) {
        crc = update(crc, remaining & 255);
    }
    char output[64];
    char *end = output + sizeof(output);
    *--end = '\n';
    char *start = decimal(end, length);
    *--start = ' ';
    start = decimal(start, ~crc);
    unsigned size = output + sizeof(output) - start;
    while (size) {
        int count = write(1, start, size);
        if (count <= 0) {
            fail("CarPlay_Multitouch ERROR: checksum output write failed\n");
        }
        start += count;
        size -= (unsigned)count;
    }
    exit(0);
}

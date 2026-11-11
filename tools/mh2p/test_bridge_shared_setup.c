#include <assert.h>
#include <string.h>
#define HOST_TEST
#include "bridge_shared_setup.c"

static unsigned regs[3072];
static int step, fail_at, error_value = 5;
static unsigned opens, created, closed, unlinked, published;

static int operation(void)
{
    return ++step == fail_at;
}

int *__get_errno_ptr(void) { return &error_value; }

int shm_open(const char *name, int flags, unsigned mode)
{
    unsigned i = opens++;
    assert(i < 2 && flags == (2 | 000400 | 002000) && mode == 0600);
    assert(!strcmp(name, i ? "/mh2p-glbridge-bulk" : "/mh2p-glbridge-mmio"));
    if (operation()) return -1;
    created |= 1u << i;
    return 100 + i;
}

int shm_ctl(int fd, int flags, unsigned long long physical,
            unsigned long long size)
{
    assert(fd == 100 || fd == 101);
    assert(flags == 2);
    assert(physical == (fd == 100 ? 0x5f000000ULL : 0x5e000000ULL));
    assert(size == (fd == 100 ? 12288 : 2 * 1024 * 1024));
    return operation() ? -1 : 0;
}

void *mmap64(void *addr, unsigned size, int prot, int flags, int fd, long long off)
{
    assert(!addr && size == 12288 && prot == 0xb00 && flags == 1);
    assert(fd == 100 && !off);
    return operation() ? (void *)-1 : regs;
}

int munmap(void *addr, unsigned size)
{
    assert(addr == regs && size == 12288);
    return operation() ? -1 : 0;
}

int fchmod(int fd, unsigned mode)
{
    assert(fd == 100 || fd == 101);
    assert(mode == 0666);
    if (operation()) return -1;
    published |= 1u << (fd - 100);
    return 0;
}

int close(int fd)
{
    assert(fd == 100 || fd == 101);
    closed |= 1u << (fd - 100);
    return operation() ? -1 : 0;
}

int shm_unlink(const char *name)
{
    unsigned mask = !strcmp(name, "/mh2p-glbridge-mmio") ? 1 : 2;
    assert(created & mask);
    unlinked |= mask;
    return 0;
}

static void reset(void)
{
    step = fail_at = 0;
    opens = created = closed = unlinked = published = 0;
    regs[0] = 0x474c4252;
    regs[6] = 1024 * 1024;
}

int main(void)
{
    reset();
    assert(setup() == 0);
    assert(created == 3 && closed == 3 && published == 3 && !unlinked);
    assert(step == 10);
    for (int failure = 1; failure <= 10; failure++) {
        reset();
        fail_at = failure;
        assert(setup() == 1);
        assert(closed == created && unlinked == created);
    }
    reset();
    regs[0] = 0;
    assert(setup() == 1 && !published);
    assert(created == 3 && closed == 3 && unlinked == 3);
    reset();
    regs[6] = 0;
    assert(setup() == 1 && !published);
    assert(created == 3 && closed == 3 && unlinked == 3);
    return 0;
}

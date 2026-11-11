/*
 * Development-only fixed-range graphics transport for unprivileged QNX clients.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
extern int shm_open(const char *, int, unsigned);
extern int shm_ctl(int, int, unsigned long long, unsigned long long);
extern int shm_unlink(const char *);
extern int fchmod(int, unsigned);
extern int close(int);
extern void *mmap64(void *, unsigned, int, int, int, long long);
extern int munmap(void *, unsigned);
extern int *__get_errno_ptr(void);
extern int printf(const char *, ...);
extern void exit(int) __attribute__((noreturn));

static int setup(void)
{
    const char *names[] = {"/mh2p-glbridge-mmio", "/mh2p-glbridge-bulk"};
    const unsigned long long physical[] = {0x5f000000, 0x5e000000};
    const unsigned sizes[] = {12288, 2 * 1024 * 1024};
    int fds[] = {-1, -1};
    void *mapping = (void *)-1;
    int ready = 0;

    for (unsigned i = 0; i < 2; i++) {
        fds[i] = shm_open(names[i], 2 | 000400 | 002000, 0600);
        if (fds[i] < 0) {
            printf("shared bridge: creating %s failed, errno=%d\n",
                   names[i], *__get_errno_ptr());
            goto cleanup;
        }
        if (shm_ctl(fds[i], 2, physical[i], sizes[i])) {
            printf("shared bridge: assigning %s failed, errno=%d\n",
                   names[i], *__get_errno_ptr());
            goto cleanup;
        }
    }
    mapping = mmap64(0, sizes[0], 0xb00, 1, fds[0], 0);
    if (mapping == (void *)-1) {
        printf("shared bridge: verifying mapping failed, errno=%d\n",
               *__get_errno_ptr());
        goto cleanup;
    }
    if (*(volatile unsigned *)mapping != 0x474c4252 ||
        ((volatile unsigned *)mapping)[6] != 1024 * 1024) {
        printf("shared bridge: unexpected device identity or bulk size\n");
        goto cleanup;
    }
    if (munmap(mapping, sizes[0])) {
        printf("shared bridge: unmapping verification failed, errno=%d\n",
               *__get_errno_ptr());
        goto cleanup;
    }
    mapping = (void *)-1;
    for (unsigned i = 0; i < 2; i++) {
        if (fchmod(fds[i], 0666)) {
            printf("shared bridge: publishing %s failed, errno=%d\n",
                   names[i], *__get_errno_ptr());
            goto cleanup;
        }
    }
    ready = 1;
cleanup:
    if (mapping != (void *)-1 && munmap(mapping, sizes[0])) {
        printf("shared bridge: cleanup unmap failed, errno=%d\n",
               *__get_errno_ptr());
        ready = 0;
    }
    for (unsigned i = 0; i < 2; i++) {
        if (fds[i] >= 0 && close(fds[i])) {
            printf("shared bridge: closing %s failed, errno=%d\n",
                   names[i], *__get_errno_ptr());
            ready = 0;
        }
    }
    if (!ready) {
        for (unsigned i = 0; i < 2; i++) {
            if (fds[i] >= 0 && shm_unlink(names[i]))
                printf("shared bridge: removing %s failed, errno=%d\n",
                       names[i], *__get_errno_ptr());
        }
        return 1;
    }
    printf("SHARED_BRIDGE_FIXED_REGIONS_READY\n");
    return 0;
}

#ifdef MH2P_FREESTANDING
void setup_main(void)
{
    exit(setup());
}

__attribute__((naked, noreturn)) void _start(void)
{
    __asm__ volatile("b setup_main");
}
#elif !defined(HOST_TEST)
int main(void)
{
    return setup();
}
#endif

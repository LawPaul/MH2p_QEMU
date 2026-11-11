#include <assert.h>
#include <string.h>

#include "../mhi2/glforward/guest/bridge_transport.h"

static u32 registers[12288 / sizeof(u32)];
static unsigned char shared_bulk[2 * 1024 * 1024];
static int main_mapping_fails, bulk_mapping_fails, mapping_errno;
static unsigned mapping_calls;
static char diagnostics[1024];
static unsigned diagnostic_bytes;
static int shared_transport, shared_open_fails, shared_close_fails;
static unsigned shared_opens, shared_closes, shared_unmaps, physical_maps;

char *getenv(const char *name)
{
    assert(!strcmp(name, "GLBRIDGE_SHARED_TRANSPORT"));
    return shared_transport ? "1" : 0;
}

int shm_open(const char *name, int flags, unsigned mode)
{
    assert(flags == 2 && !mode);
    assert(!strcmp(name, "/mh2p-glbridge-mmio") ||
           !strcmp(name, "/mh2p-glbridge-bulk"));
    shared_opens++;
    if (shared_open_fails)
        return -1;
    return !strcmp(name, "/mh2p-glbridge-mmio") ? 201 : 202;
}

void *mmap64(void *address, unsigned long size, int prot, int flags,
             int fd, long long offset)
{
    assert(!address && prot == 0xb00 && flags == 1 && !offset);
    mapping_calls++;
    if (fd == 201) {
        assert(size == sizeof(registers));
        return main_mapping_fails ? (void *)-1 : registers;
    }
    assert(fd == 202 && size == sizeof(shared_bulk));
    return bulk_mapping_fails ? (void *)-1 : shared_bulk;
}

int close(int fd)
{
    assert(fd == 201 || fd == 202);
    shared_closes++;
    return shared_close_fails ? -1 : 0;
}

int munmap(void *address, unsigned long size)
{
    assert((address == registers && size == sizeof(registers)) ||
           (address == shared_bulk && size == sizeof(shared_bulk)));
    shared_unmaps++;
    return 0;
}

long write(int fd, const void *data, unsigned long size)
{
    assert(fd == 2);
    assert(size < sizeof(diagnostics) - diagnostic_bytes);
    memcpy(diagnostics + diagnostic_bytes, data, size);
    diagnostic_bytes += size;
    diagnostics[diagnostic_bytes] = 0;
    return size;
}

int *__get_errno_ptr(void)
{
    return &mapping_errno;
}

int usleep(unsigned delay)
{
    (void)delay;
    assert(0 && "single-threaded connection should not wait");
    return -1;
}

void *mmap_device_memory(void *address, unsigned size, int prot, int flags,
                         unsigned long long physical)
{
    assert(!address && prot == 0xb00 && !flags);
    physical_maps++;
    mapping_calls++;
    if (physical == 0x5f000000ULL) {
        assert(size == sizeof(registers));
        return main_mapping_fails ? (void *)-1 : registers;
    }
    assert(physical == 0x5e000000ULL && size == sizeof(shared_bulk));
    return bulk_mapping_fails ? (void *)-1 : shared_bulk;
}

static void reset_connection(void)
{
    connection_state = 0;
    bridge = 0;
    bulk = 0;
    bulk_size = 0;
    g_fd = -1;
    g_id = 0;
    mapping_calls = 0;
    shared_transport = shared_open_fails = shared_close_fails = 0;
    shared_opens = shared_closes = shared_unmaps = physical_maps = 0;
    main_mapping_fails = bulk_mapping_fails = 0;
    diagnostic_bytes = 0;
    diagnostics[0] = 0;
    memset(registers, 0, sizeof(registers));
    registers[0] = 0x474c4252;
    registers[6] = 1024 * 1024;
}

int main(void)
{
    reset_connection();
    main_mapping_fails = 1;
    mapping_errno = 1;
    assert(gl_conn() == -1);
    assert(connection_state == 0 && !bridge && g_fd == -1);
    assert(!strcmp(diagnostics,
                   "glbridge-transport: MMIO mapping failed, errno=0x00000001\n"));
    main_mapping_fails = 0;
    assert(gl_conn() == 1 && connection_state == 2);
    assert(bulk == shared_bulk && bulk_size == 1024 * 1024);
    assert(mapping_calls == 3);
    assert(gl_conn() == 1 && mapping_calls == 3);

    reset_connection();
    registers[0] = 0x12345678;
    assert(gl_conn() == -1);
    assert(!bridge && connection_state == 0 && g_fd == -1);
    assert(shared_unmaps == 1);
    assert(!strcmp(diagnostics,
                   "glbridge-transport: unexpected MMIO magic=0x12345678\n"));

    reset_connection();
    bulk_mapping_fails = 1;
    mapping_errno = 12;
    assert(gl_conn() == 1 && connection_state == 2);
    assert(!bulk && !bulk_size && bridge == (void *)registers);
    assert(!strcmp(diagnostics,
                   "glbridge-transport: bulk mapping unavailable; using register "
                   "transport, errno=0x0000000c\n"));

    reset_connection();
    registers[6] = 0;
    assert(gl_conn() == 1 && mapping_calls == 1);
    assert(!bulk && !bulk_size && !diagnostic_bytes);

    reset_connection();
    shared_transport = 1;
    assert(gl_conn() == 1);
    assert(bridge == (void *)registers && bulk == shared_bulk);
    assert(!physical_maps && shared_opens == 2 && shared_closes == 2);
    assert(!diagnostic_bytes && !shared_unmaps);

    reset_connection();
    shared_transport = shared_open_fails = 1;
    mapping_errno = 13;
    assert(gl_conn() == -1 && connection_state == 0);
    assert(!physical_maps && shared_opens == 1 && !shared_closes);
    assert(strstr(diagnostics, "opening shared bridge region failed, errno=0x0000000d\n"));

    reset_connection();
    shared_transport = main_mapping_fails = 1;
    mapping_errno = 12;
    assert(gl_conn() == -1 && connection_state == 0);
    assert(!physical_maps && shared_opens == 1 && shared_closes == 1);
    assert(!shared_unmaps);

    reset_connection();
    shared_transport = shared_close_fails = 1;
    mapping_errno = 5;
    assert(gl_conn() == -1 && connection_state == 0);
    assert(!physical_maps && shared_opens == 1 && shared_closes == 1);
    assert(shared_unmaps == 1);
    assert(strstr(diagnostics, "closing shared bridge region failed, errno=0x00000005\n"));

    reset_connection();
    shared_transport = bulk_mapping_fails = 1;
    mapping_errno = 12;
    assert(gl_conn() == 1 && !bulk && !bulk_size);
    assert(!physical_maps && shared_opens == 2 && shared_closes == 2);
    return 0;
}

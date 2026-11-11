/*
 * Bounded local configuration RPC over a dedicated inherited Unix socket.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/sockets.h"
#include "qemu/timer.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/mh2p_pcie_config.h"
#include "io/channel-socket.h"
#include "migration/vmstate.h"
#include "monitor/monitor.h"

#define TYPE_MH2P_PCIE_CONFIG "mh2p-pcie-config"
OBJECT_DECLARE_SIMPLE_TYPE(MH2PPCIeConfigState, MH2P_PCIE_CONFIG)

#define CONFIG_MAGIC 0x4348324d /* "M2HC", little endian */
#define FRAME_SIZE 32
#define RESPONSE 0x80000000U
#define STATE 0
#define READ 1
#define WRITE 2
#define MEMORY_READ 3
#define MEMORY_WRITE 4
#define ABSENT 1
#define UNSUPPORTED 2
#define RPC_TIMEOUT_NS 1000000000LL

struct MH2PPCIeConfigState {
    SysBusDevice parent;
    char *fd;
    QIOChannelSocket *socket;
    MH2PPCIeReady *ready;
    MH2PPCIeConfig *config;
    MH2PPCIeConfig *memory;
    void *opaque;
    guint watch;
    uint8_t rx[FRAME_SIZE], tx[FRAME_SIZE];
    unsigned rx_count, tx_count;
    uint32_t sequence, epoch;
    bool failed, in_flight, peer_ready, replying;
};

static void config_close(MH2PPCIeConfigState *s)
{
    s->failed = true;
    s->peer_ready = false;
    if (s->watch) {
        g_source_remove(s->watch);
        s->watch = 0;
    }
    if (s->socket) {
        qio_channel_shutdown(QIO_CHANNEL(s->socket),
                             QIO_CHANNEL_SHUTDOWN_BOTH, NULL);
    }
}

static void config_fail(MH2PPCIeConfigState *s, const char *reason)
{
    error_report("mh2p-pcie-config: %s; disabling transport", reason);
    config_close(s);
}

static bool config_request_valid(const uint8_t *frame)
{
    uint32_t op = ldl_le_p(frame + 8);
    uint32_t reg = ldl_le_p(frame + 20);
    uint32_t size = ldl_le_p(frame + 24);

    if (ldl_le_p(frame) != CONFIG_MAGIC || ldl_le_p(frame + 4) != 1 ||
        op > MEMORY_WRITE || ldl_le_p(frame + 16) > 1) {
        return false;
    }
    if (op == STATE) {
        return reg == 0 && size == 0 && ldl_le_p(frame + 28) == 0;
    }
    return (size == 1 || size == 2 || size == 4) &&
           (op >= MEMORY_READ || reg < 4096) && !(reg & (size - 1)) &&
           (op == WRITE || op == MEMORY_WRITE ||
            ldl_le_p(frame + 28) == 0);
}

static void config_respond(MH2PPCIeConfigState *s)
{
    unsigned op = ldl_le_p(s->rx + 8);
    uint32_t value = ldl_le_p(s->rx + 28);
    uint32_t status = 0;
    MH2PPCIeConfig *access = op >= MEMORY_READ ? s->memory : s->config;

    s->peer_ready = ldl_le_p(s->rx + 16);
    if (op == STATE) {
        value = s->peer_ready && s->ready(s->opaque);
    } else if (!s->peer_ready || !s->ready(s->opaque)) {
        status = ABSENT;
        value = UINT32_MAX;
    } else if (!access ||
               access(s->opaque, ldl_le_p(s->rx + 20),
                      ldl_le_p(s->rx + 24), &value,
                      op == WRITE || op == MEMORY_WRITE) != MEMTX_OK) {
        status = UNSUPPORTED;
        value = 0;
    }
    memcpy(s->tx, s->rx, FRAME_SIZE);
    stl_le_p(s->tx + 8, op | RESPONSE);
    stl_le_p(s->tx + 16, status);
    stl_le_p(s->tx + 28, value);
    s->tx_count = 0;
    s->rx_count = 0;
    s->replying = true;
}

static gboolean config_receive(QIOChannel *ioc, GIOCondition condition,
                               gpointer opaque)
{
    MH2PPCIeConfigState *s = opaque;
    Error *error = NULL;
    ssize_t count;

    s->watch = 0;
    if (condition & (G_IO_ERR | G_IO_NVAL)) {
        config_fail(s, "socket I/O error");
        return G_SOURCE_REMOVE;
    }
    if ((condition & G_IO_HUP) && !(condition & G_IO_IN)) {
        if (s->rx_count || s->replying) {
            config_fail(s, "peer closed during a request or completion");
        } else {
            config_close(s);
        }
        return G_SOURCE_REMOVE;
    }
    if (condition & G_IO_IN) {
        count = qio_channel_read(ioc, s->rx + s->rx_count,
                                FRAME_SIZE - s->rx_count, &error);
        if (count == 0) {
            if (s->rx_count) {
                config_fail(s, "peer closed during a request");
            } else {
                config_close(s);
            }
            return G_SOURCE_REMOVE;
        }
        if (count < 0 && count != QIO_CHANNEL_ERR_BLOCK) {
            config_fail(s, error_get_pretty(error));
            error_free(error);
            return G_SOURCE_REMOVE;
        }
        if (count > 0) {
            s->rx_count += count;
        }
        if (s->rx_count != FRAME_SIZE) {
            s->watch = qio_channel_add_watch(
                ioc, G_IO_IN, config_receive, s, NULL);
            return G_SOURCE_REMOVE;
        }
        if (!config_request_valid(s->rx)) {
            config_fail(s, "invalid version-1 request");
            return G_SOURCE_REMOVE;
        }
        config_respond(s);
    }
    count = qio_channel_write(ioc, s->tx + s->tx_count,
                             FRAME_SIZE - s->tx_count, &error);
    if (!count) {
        config_fail(s, "peer closed during a completion");
        return G_SOURCE_REMOVE;
    }
    if (count < 0 && count != QIO_CHANNEL_ERR_BLOCK) {
        config_fail(s, error_get_pretty(error));
        error_free(error);
        return G_SOURCE_REMOVE;
    }
    if (count > 0) {
        s->tx_count += count;
    }
    if (s->tx_count == FRAME_SIZE) {
        s->replying = false;
    }
    s->watch = qio_channel_add_watch(
        ioc, s->tx_count == FRAME_SIZE ? G_IO_IN : G_IO_OUT,
        config_receive, s, NULL);
    return G_SOURCE_REMOVE;
}

static bool config_transfer(MH2PPCIeConfigState *s, uint8_t *frame,
                            bool write, int64_t deadline, Error **errp)
{
    QIOChannel *ioc = QIO_CHANNEL(s->socket);
    unsigned offset = 0;

    while (offset < FRAME_SIZE) {
        ssize_t count = write ?
            qio_channel_write(ioc, frame + offset, FRAME_SIZE - offset, errp) :
            qio_channel_read(ioc, frame + offset, FRAME_SIZE - offset, errp);

        if (count == QIO_CHANNEL_ERR_BLOCK) {
            GPollFD pollfd = {
                .fd = s->socket->fd,
                .events = write ? G_IO_OUT : G_IO_IN,
            };
            int64_t remaining =
                deadline - qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
            int result;

            result = remaining > 0 ? qemu_poll_ns(&pollfd, 1, remaining) : 0;
            if (!result) {
                error_setg(errp, "configuration request timed out");
                return false;
            }
            if (result < 0 && errno != EINTR) {
                error_setg_errno(errp, errno, "configuration socket poll");
                return false;
            }
            continue;
        }
        if (count <= 0) {
            if (!count) {
                error_setg(errp, "peer closed during configuration request");
            }
            return false;
        }
        offset += count;
        if (qemu_clock_get_ns(QEMU_CLOCK_REALTIME) > deadline) {
            error_setg(errp, "configuration request timed out");
            return false;
        }
    }
    return true;
}

static MemTxResult config_rpc(MH2PPCIeConfigState *s, bool ready, unsigned op,
                              unsigned reg, unsigned size, uint32_t *value)
{
    uint8_t request[FRAME_SIZE] = { 0 }, reply[FRAME_SIZE];
    Error *error = NULL;
    int64_t deadline = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) + RPC_TIMEOUT_NS;
    uint32_t epoch = s->epoch, status;
    bool success, locked = bql_locked();

    assert(!s->ready);
    if (s->failed) {
        return MEMTX_ERROR;
    }
    if (s->in_flight) {
        error_report("mh2p-pcie-config: concurrent configuration request");
        return MEMTX_ERROR;
    }
    stl_le_p(request, CONFIG_MAGIC);
    stl_le_p(request + 4, 1);
    stl_le_p(request + 8, op);
    stl_le_p(request + 12, ++s->sequence);
    stl_le_p(request + 16, ready);
    stl_le_p(request + 20, reg);
    stl_le_p(request + 24, size);
    stl_le_p(request + 28, op == WRITE || op == MEMORY_WRITE ? *value : 0);
    s->in_flight = true;
    /* No channel watch consumes replies. Do not hold the BQL while waiting. */
    if (locked) {
        bql_unlock();
    }
    success = config_transfer(s, request, true, deadline, &error) &&
              config_transfer(s, reply, false, deadline, &error);
    if (locked) {
        bql_lock();
    }
    s->in_flight = false;
    if (!success) {
        config_fail(s, error_get_pretty(error));
        error_free(error);
        return MEMTX_ERROR;
    }
    status = ldl_le_p(reply + 16);
    if (epoch != s->epoch || ldl_le_p(reply) != CONFIG_MAGIC ||
        ldl_le_p(reply + 4) != 1 || ldl_le_p(reply + 8) != (op | RESPONSE) ||
        ldl_le_p(reply + 12) != s->sequence || status > UNSUPPORTED ||
        ldl_le_p(reply + 20) != reg || ldl_le_p(reply + 24) != size ||
        (op == STATE && (status || ldl_le_p(reply + 28) > 1))) {
        error_report("mh2p-pcie-config: completion magic=%x version=%u "
                     "op=%x seq=%u status=%u reg=%x size=%u value=%x "
                     "(expected op=%x seq=%u epoch=%u/%u reg=%x size=%u)",
                     ldl_le_p(reply), ldl_le_p(reply + 4),
                     ldl_le_p(reply + 8), ldl_le_p(reply + 12), status,
                     ldl_le_p(reply + 20), ldl_le_p(reply + 24),
                     ldl_le_p(reply + 28), op | RESPONSE, s->sequence,
                     epoch, s->epoch, reg, size);
        config_fail(s, "invalid or reset-stale completion");
        return MEMTX_ERROR;
    }
    *value = ldl_le_p(reply + 28);
    if (status == ABSENT && op >= MEMORY_READ) {
        error_report("mh2p-pcie-config: memory access to absent peer "
                     "at 0x%x size %u", reg, size);
        return MEMTX_ERROR;
    }
    if (status == UNSUPPORTED) {
        error_report("mh2p-pcie-config: unsupported peer %s "
                     "%s at 0x%x size %u",
                     op >= MEMORY_READ ? "memory" : "configuration",
                     op == WRITE || op == MEMORY_WRITE ? "write" : "read",
                     reg, size);
        return MEMTX_ERROR;
    }
    return MEMTX_OK;
}

bool mh2p_pcie_config_link(DeviceState *dev, bool local_ready)
{
    uint32_t value = 0;

    return dev && config_rpc(MH2P_PCIE_CONFIG(dev), local_ready,
                             STATE, 0, 0, &value) == MEMTX_OK && value;
}

bool mh2p_pcie_config_peer(DeviceState *dev)
{
    MH2PPCIeConfigState *s;

    if (!dev) {
        return false;
    }
    s = MH2P_PCIE_CONFIG(dev);
    return !s->failed && s->peer_ready;
}

MemTxResult mh2p_pcie_config_access(DeviceState *dev, bool local_ready,
                                   unsigned reg, unsigned size,
                                   uint32_t *value, bool write)
{
    return config_rpc(MH2P_PCIE_CONFIG(dev), local_ready,
                      write ? WRITE : READ, reg, size, value);
}

MemTxResult mh2p_pcie_memory_access(DeviceState *dev, bool local_ready,
                                  uint32_t address, unsigned size,
                                  uint32_t *value, bool write)
{
    return config_rpc(MH2P_PCIE_CONFIG(dev), local_ready,
                      write ? MEMORY_WRITE : MEMORY_READ, address, size, value);
}

static void config_reset(DeviceState *dev)
{
    MH2PPCIeConfigState *s = MH2P_PCIE_CONFIG(dev);

    s->epoch++;
    s->peer_ready = false;
    if (s->rx_count || s->replying) {
        config_fail(s, "reset during an incomplete request or completion");
    }
}

static void config_realize(DeviceState *dev, Error **errp)
{
    MH2PPCIeConfigState *s = MH2P_PCIE_CONFIG(dev);
    struct sockaddr_un peer;
    socklen_t length = sizeof(peer);
    int type, fd = monitor_fd_param(monitor_cur(), s->fd, errp);
    socklen_t type_length = sizeof(type);

    if (fd < 0) {
        return;
    }
    if (getpeername(fd, (struct sockaddr *)&peer, &length) < 0 ||
        peer.sun_family != AF_UNIX ||
        getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &type_length) < 0 ||
        type != SOCK_STREAM) {
        error_setg(errp, "mh2p-pcie-config requires "
                   "a connected Unix stream fd");
        return;
    }
    s->socket = qio_channel_socket_new_fd(fd, errp);
    if (!s->socket ||
        !qio_channel_set_blocking(QIO_CHANNEL(s->socket), false, errp)) {
        return;
    }
    if (s->ready) {
        s->watch = qio_channel_add_watch(QIO_CHANNEL(s->socket),
                                        G_IO_IN, config_receive, s, NULL);
    }
}

static void config_unrealize(DeviceState *dev)
{
    MH2PPCIeConfigState *s = MH2P_PCIE_CONFIG(dev);

    config_close(s);
    if (s->socket) {
        qio_channel_close(QIO_CHANNEL(s->socket), NULL);
        object_unref(s->socket);
    }
}

static const Property config_properties[] = {
    DEFINE_PROP_STRING("fd", MH2PPCIeConfigState, fd),
};

static const VMStateDescription config_vmstate = {
    .name = TYPE_MH2P_PCIE_CONFIG,
    .unmigratable = 1,
};

static void config_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = config_realize;
    dc->unrealize = config_unrealize;
    dc->user_creatable = false;
    dc->vmsd = &config_vmstate;
    device_class_set_legacy_reset(dc, config_reset);
    device_class_set_props(dc, config_properties);
}

static const TypeInfo config_info = {
    .name = TYPE_MH2P_PCIE_CONFIG,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MH2PPCIeConfigState),
    .class_init = config_class_init,
};

static void config_register_types(void)
{
    type_register_static(&config_info);
}

type_init(config_register_types)

DeviceState *mh2p_pcie_config_create(Object *parent, const char *name,
                                   const char *fd,
                                   MH2PPCIeReady *ready,
                                   MH2PPCIeConfig *config,
                                   MH2PPCIeConfig *memory, void *opaque)
{
    DeviceState *dev;
    MH2PPCIeConfigState *s;

    if (!fd) {
        return NULL;
    }
    dev = qdev_new(TYPE_MH2P_PCIE_CONFIG);
    s = MH2P_PCIE_CONFIG(dev);
    s->ready = ready;
    s->config = config;
    s->memory = memory;
    s->opaque = opaque;
    object_property_add_child(parent, name, OBJECT(dev));
    qdev_prop_set_string(dev, "fd", fd);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    return dev;
}

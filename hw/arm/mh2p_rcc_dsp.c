/*
 * Experimental DRA7 DSP1 external-memory execution bridge.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include <gmodule.h>
#include "exec/icount.h"
#include "hw/arm/mh2p_rcc_dsp.h"
#include "hw/arm/mh2p_rcc_mmu.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qapi/qapi-builtin-visit.h"
#include "qapi/util.h"
#include "qobject/qdict.h"
#include "qemu/error-report.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/cutils.h"
#include "qemu/timer.h"
#include "system/address-spaces.h"
#include "system/cpu-timers.h"
#include "system/reset.h"

#define TYPE_MH2P_RCC_DSP "mh2p-rcc-dsp"
#define DSP_TICK_NS (100 * SCALE_US)
OBJECT_DECLARE_SIMPLE_TYPE(MH2PRCCDSP, MH2P_RCC_DSP)

typedef struct RCCDSPHostMemory {
    void *opaque;
    int32_t (*read)(void *opaque, uint32_t address, uint8_t *data, size_t size);
    int32_t (*write)(void *opaque, uint32_t address,
                     const uint8_t *data, size_t size);
} RCCDSPHostMemory;

struct MH2PRCCDSP {
    SysBusDevice parent_obj;
    MemoryRegion registers;
    MemoryRegion local[3];
    AddressSpace translated[2];
    DeviceState *mmu[2];
    uint32_t mmu_config, asserted;
    uint32_t diagnostic_cycles_per_tick;
    uint32_t event_levels[4], pending_events[4];
    bool clock_running, failed, executing, pending_reset;
    QEMUTimer *timer;
    int64_t next_tick;
    GModule *module;
    void *backend;
    void *(*create)(uint32_t pc, RCCDSPHostMemory memory);
    void (*destroy)(void *backend);
    int32_t (*step)(void *backend, uint32_t cycles);
    int32_t (*event)(void *backend, uint32_t event);
    uint32_t (*pc)(const void *backend);
    uint64_t (*cycles)(const void *backend);
    int32_t (*snapshot)(const void *backend, uint32_t *values, size_t count);
    int32_t (*cached_bytes)(const void *backend, uint32_t address,
                            uint8_t *data, size_t count);
    const char *(*error)(const void *backend);
};

static void dsp_stop(MH2PRCCDSP *s)
{
    timer_del(s->timer);
    if (s->backend) {
        s->destroy(s->backend);
        s->backend = NULL;
    }
}

static MemTxResult dsp_memory(MH2PRCCDSP *s, uint32_t address, uint8_t *data,
                              size_t size, bool write)
{
    AddressSpace *as = s->mmu_config & 1 ?
                       &s->translated[0] : &address_space_memory;
    static const struct {
        uint32_t base, size, target;
    } local[] = {
        { 0x00800000, 0x48000, 0x40800000 },
        { 0x00e00000, 0x8000, 0x40e00000 },
        { 0x00f00000, 0x8000, 0x40f00000 },
        { 0x01d00000, 0x8000, 0x40d00000 },
        { 0x01d10000, 0x8000, 0x40d10000 },
    };

    if (s->pending_reset || size > UINT64_C(0x100000000) - address) {
        return MEMTX_ERROR;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(local); i++) {
        uint32_t offset = address - local[i].base;

        if (offset < local[i].size && size <= local[i].size - offset) {
            as = &address_space_memory;
            address = local[i].target + offset;
            break;
        }
    }
    if (write) {
        return address_space_write(as, address, MEMTXATTRS_UNSPECIFIED,
                                    data, size);
    }
    return address_space_read(as, address, MEMTXATTRS_UNSPECIFIED, data, size);
}

MemTxResult mh2p_rcc_dsp_dma_read(DeviceState *dev, uint32_t address,
                                 uint8_t *data, size_t size)
{
    MH2PRCCDSP *s = MH2P_RCC_DSP(dev);
    AddressSpace *as = s->mmu_config & BIT(4) ?
                       &s->translated[1] : &address_space_memory;
    hwaddr translated, length = size;
    MemoryRegion *region;

    if (size > UINT64_C(0x100000000) - address) {
        return MEMTX_ERROR;
    }
    region = address_space_translate(as, address, &translated, &length,
                                     false, MEMTXATTRS_UNSPECIFIED);
    if (!memory_region_is_ram(region) || length < size) {
        return MEMTX_ERROR;
    }
    return address_space_read(as, address, MEMTXATTRS_UNSPECIFIED, data, size);
}

static struct {
    uint64_t reads, writes, bytes, mem_ns, step_ns, steps, cycles, last;
} dsp_stats;

static void dsp_stats_report(void)
{
    int64_t now = get_clock();

    if (!dsp_stats.last) {
        dsp_stats.last = now;
    }
    if (now - dsp_stats.last >= NANOSECONDS_PER_SECOND * 10) {
        fprintf(stderr, "mh2p-rcc-dsp-stats: wall=%.1fs steps=%" PRIu64
                " cycles=%" PRIu64 " step=%.2fs mem=%.2fs reads=%" PRIu64
                " writes=%" PRIu64 " bytes=%" PRIu64 "\n",
                (now - dsp_stats.last) / 1e9, dsp_stats.steps,
                dsp_stats.cycles, dsp_stats.step_ns / 1e9,
                dsp_stats.mem_ns / 1e9, dsp_stats.reads, dsp_stats.writes,
                dsp_stats.bytes);
        memset(&dsp_stats, 0, sizeof(dsp_stats));
        dsp_stats.last = now;
    }
}

static bool dsp_stats_enabled(void)
{
    static int enabled = -1;

    if (enabled < 0) {
        enabled = getenv("MH2P_DSP_STATS") != NULL;
    }
    return enabled;
}

static int32_t dsp_read(void *opaque, uint32_t address,
                        uint8_t *data, size_t size)
{
    MH2PRCCDSP *s = opaque;
    int64_t t0 = dsp_stats_enabled() ? get_clock() : 0;
    BQL_LOCK_GUARD();
    MemTxResult result = dsp_memory(s, address, data, size, false);

    if (t0) {
        dsp_stats.mem_ns += get_clock() - t0;
        dsp_stats.reads++;
        dsp_stats.bytes += size;
    }

    if (result != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR, "mh2p-rcc-dsp: failed read at "
                      "0x%08x size %zu result %u\n", address, size, result);
        return -1;
    }
    return 0;
}

static int32_t dsp_write(void *opaque, uint32_t address,
                         const uint8_t *data, size_t size)
{
    MH2PRCCDSP *s = opaque;
    int64_t t0 = dsp_stats_enabled() ? get_clock() : 0;
    BQL_LOCK_GUARD();
    MemTxResult result = dsp_memory(s, address, (uint8_t *)data, size, true);

    if (t0) {
        dsp_stats.mem_ns += get_clock() - t0;
        dsp_stats.writes++;
        dsp_stats.bytes += size;
    }

    if (result != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR, "mh2p-rcc-dsp: failed write at "
                      "0x%08x size %zu result %u\n", address, size, result);
        return -1;
    }
    return 0;
}

/*
 * Experimental MH2P_DSP_PACE_CLOCK: slow the guest clock while the native DSP
 * cannot deliver its cycle budget per virtual tick, and speed it back up when
 * the DSP has spare host time. Without it, real-time DSP work (BIOS clock,
 * audio periods) consumes the whole budget and lower-priority tasks starve.
 */
static unsigned dsp_pace_min_percent(void)
{
    static int percent = -1;

    if (percent < 0) {
        const char *env = getenv("MH2P_DSP_PACE_CLOCK");

        percent = 0;
        if (env && (qemu_strtoi(env, NULL, 10, &percent) ||
                    percent < 1 || percent > 100 || icount_enabled())) {
            warn_report("MH2P_DSP_PACE_CLOCK requires a minimum clock "
                        "percentage of 1..100 and cannot be used with icount");
            percent = 0;
        }
    }
    return percent;
}

static void dsp_pace_clock(uint32_t cycles, int64_t step_ns)
{
    static int64_t last_wall, last_virt, busy_ns, prev_call;
    static uint64_t done;
    int64_t wall = get_clock(), virt, wall_ns, virt_ns;
    unsigned min_percent = dsp_pace_min_percent();
    uint32_t ppm;
    double ratio, busy;

    if (!min_percent) {
        return;
    }
    virt = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (wall - prev_call > 50 * SCALE_MS) {
        last_wall = 0;
        done = busy_ns = 0;
    }
    prev_call = wall;
    if (!last_wall) {
        last_wall = wall;
        last_virt = virt;
        return;
    }
    done += cycles;
    busy_ns += step_ns;
    wall_ns = wall - last_wall;
    if (wall_ns < 100 * SCALE_MS) {
        return;
    }
    virt_ns = virt - last_virt;
    ppm = cpu_clock_get_rate_ppm();
    /* Fraction of the per-tick budget delivered over this virtual interval. */
    ratio = virt_ns > 0 ? (double)done * DSP_TICK_NS / virt_ns / cycles : 1;
    busy = (double)busy_ns / wall_ns;
    /* Tick re-arm latency alone loses ~5%, so only react to real shortfall. */
    if (ratio < 0.8 && busy > 0.5) {
        ppm = ppm * MAX(ratio, 0.5) * 1.1;
    } else if (busy < 0.6) {
        ppm = MIN(ppm * 1.1 + 1000, 1000000);
    }
    ppm = MAX(ppm, min_percent * 10000);
    if (ppm != cpu_clock_get_rate_ppm()) {
        cpu_clock_set_rate_ppm(ppm);
    }
    if (getenv("MH2P_DSP_PACE_LOG")) {
        fprintf(stderr, "mh2p-rcc-dsp-pace: virt=%.3f ratio=%.2f busy=%.2f "
                "rate=%u%%\n", virt / 1e9, ratio, busy, ppm / 10000);
    }
    last_wall = wall;
    last_virt = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    done = busy_ns = 0;
}

static void dsp_tick(void *opaque)
{
    MH2PRCCDSP *s = opaque;
    int64_t step_t0, step_ns, now;
    int32_t result;

    if (!s->backend || !s->clock_running || s->asserted || s->failed) {
        return;
    }
    for (unsigned bank = 0; bank < ARRAY_SIZE(s->pending_events); bank++) {
        uint32_t pending = s->pending_events[bank];

        s->pending_events[bank] = 0;
        while (pending) {
            unsigned bit = ctz32(pending);

            if (s->event(s->backend, bank * 32 + bit)) {
                error_report("mh2p-rcc-dsp: backend rejected system event %u",
                             bank * 32 + bit);
                s->failed = true;
                return;
            }
            pending &= pending - 1;
        }
    }
    /* Bounded diagnostic pacing; DPLL-derived DSP timing is not implemented. */
    s->executing = true;
    step_t0 = dsp_stats_enabled() || dsp_pace_min_percent() ? get_clock() : 0;
    if (getenv("MH2P_DSP_UNLOCKED_STEP")) {
        /*
         * The backend is only used from the main loop; vCPU threads touch
         * device state under the BQL and defer resets through executing.
         */
        bql_unlock();
        result = s->step(s->backend, s->diagnostic_cycles_per_tick);
        bql_lock();
    } else {
        result = s->step(s->backend, s->diagnostic_cycles_per_tick);
    }
    s->executing = false;
    step_ns = step_t0 ? get_clock() - step_t0 : 0;
    if (!result && !s->pending_reset && dsp_pace_min_percent()) {
        dsp_pace_clock(s->diagnostic_cycles_per_tick, step_ns);
    }
    if (dsp_stats_enabled()) {
        dsp_stats.step_ns += step_ns;
        dsp_stats.steps++;
        dsp_stats.cycles += s->diagnostic_cycles_per_tick;
        dsp_stats_report();
    }
    if (s->pending_reset) {
        dsp_stop(s);
        s->pending_reset = s->failed = false;
        memset(s->pending_events, 0, sizeof(s->pending_events));
        return;
    }
    if (result != 0) {
        const char *error = s->error(s->backend);

        error_report("mh2p-rcc-dsp: native execution stopped at 0x%08x: %s",
                     s->pc(s->backend), error ? error : "backend failure");
        s->failed = true;
        return;
    }
    if (!s->clock_running || s->asserted) {
        return;
    }
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (dsp_pace_min_percent()) {
        /* Host-clock mode: virtual time advances during the step itself. */
        s->next_tick += DSP_TICK_NS;
        if (s->next_tick < now - 10 * DSP_TICK_NS ||
            s->next_tick > now + DSP_TICK_NS) {
            s->next_tick = now + DSP_TICK_NS;
        }
    } else {
        s->next_tick = now + DSP_TICK_NS;
    }
    timer_mod(s->timer, s->next_tick);
}

uint32_t mh2p_rcc_dsp_control(DeviceState *dev, uint32_t reset,
                             bool clock_running, uint32_t vector)
{
    MH2PRCCDSP *s = MH2P_RCC_DSP(dev);
    uint32_t old = s->asserted;

    s->clock_running = clock_running;
    s->asserted |= reset;
    if (reset & 2) {
        s->asserted |= 1;
    }
    if (s->asserted & 1) {
        if (s->executing) {
            s->pending_reset = true;
            timer_del(s->timer);
        } else {
            dsp_stop(s);
            s->failed = false;
            memset(s->pending_events, 0, sizeof(s->pending_events));
        }
    }
    if ((s->asserted & 2) && !(old & 2)) {
        s->mmu_config = 0;
        for (unsigned i = 0; i < 2; i++) {
            device_cold_reset(s->mmu[i]);
        }
    }
    if (clock_running && !(reset & 2)) {
        s->asserted &= ~2U;
        if (!(reset & 1) && !s->pending_reset) {
            if (!s->backend) {
                RCCDSPHostMemory memory = {
                    .opaque = s, .read = dsp_read, .write = dsp_write,
                };

                s->backend = s->create(vector << 10, memory);
                if (!s->backend) {
                    error_report("mh2p-rcc-dsp: backend creation failed");
                    return s->asserted;
                }
                memcpy(s->pending_events, s->event_levels,
                       sizeof(s->pending_events));
            }
            s->asserted &= ~1U;
        }
    }
    if (!clock_running) {
        timer_del(s->timer);
    } else if (s->backend && !s->asserted && !s->failed &&
               !timer_pending(s->timer)) {
        s->next_tick = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + DSP_TICK_NS;
        timer_mod(s->timer, s->next_tick);
    }
    return s->asserted;
}

static MemTxResult dsp_register_read(void *opaque, hwaddr address,
                                      uint64_t *value, unsigned size,
                                      MemTxAttrs attrs)
{
    MH2PRCCDSP *s = opaque;

    if (address == 0x18) {
        *value = s->mmu_config;
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP, "mh2p-rcc-dsp: unsupported system read at "
                  "0x%" HWADDR_PRIx "\n", address);
    return MEMTX_ERROR;
}

static MemTxResult dsp_register_write(void *opaque, hwaddr address,
                                       uint64_t value, unsigned size,
                                       MemTxAttrs attrs)
{
    MH2PRCCDSP *s = opaque;

    if (address == 0x18 && !(value & ~0x11U)) {
        s->mmu_config = value;
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP, "mh2p-rcc-dsp: unsupported system write at "
                  "0x%" HWADDR_PRIx " value 0x%" PRIx64 "\n", address, value);
    return MEMTX_ERROR;
}

static const MemoryRegionOps dsp_register_ops = {
    .read_with_attrs = dsp_register_read,
    .write_with_attrs = dsp_register_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void dsp_reset(DeviceState *dev)
{
    MH2PRCCDSP *s = MH2P_RCC_DSP(dev);

    dsp_stop(s);
    s->asserted = 3;
    s->mmu_config = 0;
    s->clock_running = false;
    s->failed = false;
    s->pending_reset = false;
    memset(s->event_levels, 0, sizeof(s->event_levels));
    memset(s->pending_events, 0, sizeof(s->pending_events));
}

static void dsp_event_input(void *opaque, int event, int level)
{
    MH2PRCCDSP *s = opaque;
    unsigned bank = event / 32;
    uint32_t bit = 1U << (event % 32);

    /* MMIO callbacks during step must not re-enter the borrowed native CPU. */
    if (level && !(s->event_levels[bank] & bit) && s->backend &&
        !s->asserted) {
        s->pending_events[bank] |= bit;
    }
    if (level) {
        s->event_levels[bank] |= bit;
    } else {
        s->event_levels[bank] &= ~bit;
    }
}

static void dsp_get_pc(Object *obj, Visitor *v, const char *name,
                       void *opaque, Error **errp)
{
    MH2PRCCDSP *s = MH2P_RCC_DSP(obj);
    uint64_t value = s->backend ? s->pc(s->backend) : 0;

    visit_type_uint64(v, name, &value, errp);
}

static void dsp_get_cycles(Object *obj, Visitor *v, const char *name,
                           void *opaque, Error **errp)
{
    MH2PRCCDSP *s = MH2P_RCC_DSP(obj);
    uint64_t value = s->backend ? s->cycles(s->backend) : 0;

    visit_type_uint64(v, name, &value, errp);
}

static bool dsp_get_failed(Object *obj, Error **errp)
{
    return MH2P_RCC_DSP(obj)->failed;
}

static void dsp_get_cycle_budget(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    uint32_t value = MH2P_RCC_DSP(obj)->diagnostic_cycles_per_tick;

    visit_type_uint32(v, name, &value, errp);
}

static void dsp_set_cycle_budget(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (!value || value > 100000) {
        error_setg(errp, "diagnostic-cycles-per-tick must be between 1 and 100000");
        return;
    }
    MH2P_RCC_DSP(obj)->diagnostic_cycles_per_tick = value;
}

static void dsp_get_registers(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    MH2PRCCDSP *s = MH2P_RCC_DSP(obj);
    uint32_t values[97];
    uint32List *list = NULL, **tail = &list;

    if (!s->backend) {
        error_setg(errp, "DSP1 has no running or stopped backend instance");
        return;
    }
    if (s->snapshot(s->backend, values, ARRAY_SIZE(values))) {
        error_setg(errp, "DSP1 backend could not capture registers");
        return;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(values); i++) {
        QAPI_LIST_APPEND(tail, values[i]);
    }
    visit_type_uint32List(v, name, &list, errp);
    qapi_free_uint32List(list);
}

static char *dsp_get_error(Object *obj, Error **errp)
{
    MH2PRCCDSP *s = MH2P_RCC_DSP(obj);
    const char *error = s->backend ? s->error(s->backend) : NULL;

    return g_strdup(error ? error : "");
}

static void dsp_get_cached_register_memory(Object *obj, Visitor *v,
                                           const char *name, void *opaque,
                                           Error **errp)
{
    MH2PRCCDSP *s = MH2P_RCC_DSP(obj);
    uint32_t registers[97];
    QDict *result = qdict_new();
    QObject *value = QOBJECT(result);

    if (!s->backend ||
        s->snapshot(s->backend, registers, ARRAY_SIZE(registers))) {
        error_setg(errp, "DSP1 has no register snapshot to inspect");
        goto out;
    }
    for (unsigned i = 0; i < 64; i++) {
        uint32_t address = registers[i] & ~31U;
        uint8_t data[32];
        char hex[65];
        g_autofree char *reg = NULL;
        QDict *entry;
        int32_t status = s->cached_bytes(s->backend, address,
                                         data, sizeof(data));

        if (status == 1) {
            continue;
        }
        if (status != 0) {
            error_setg(errp, "DSP1 backend could not inspect cached bytes");
            goto out;
        }
        for (unsigned j = 0; j < sizeof(data); j++) {
            snprintf(hex + j * 2, 3, "%02x", data[j]);
        }
        reg = g_strdup_printf("%c%u", i < 32 ? 'a' : 'b', i % 32);
        entry = qdict_new();

        qdict_put_int(entry, "address", address);
        qdict_put_str(entry, "bytes", hex);
        qdict_put_obj(result, reg, QOBJECT(entry));
    }
    visit_type_any(v, name, &value, errp);
out:
    qobject_unref(value);
}

static void dsp_init(Object *obj)
{
    MH2PRCCDSP *s = MH2P_RCC_DSP(obj);

    s->asserted = 3;
    s->diagnostic_cycles_per_tick = 1000;
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, dsp_tick, s);
    qdev_init_gpio_in_named(DEVICE(obj), dsp_event_input, "events", 128);
    memory_region_init_io(&s->registers, obj, &dsp_register_ops, s,
                          "dsp1-system", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->registers);
    object_property_add(obj, "pc", "uint64", dsp_get_pc, NULL, NULL, NULL);
    object_property_add(obj, "cycles", "uint64",
                         dsp_get_cycles, NULL, NULL, NULL);
    object_property_add(obj, "diagnostic-cycles-per-tick", "uint32",
                         dsp_get_cycle_budget, dsp_set_cycle_budget, NULL, NULL);
    object_property_add_bool(obj, "failed", dsp_get_failed, NULL);
    object_property_add_str(obj, "error", dsp_get_error, NULL);
    object_property_add(obj, "registers", "uint32List",
                         dsp_get_registers, NULL, NULL, NULL);
    object_property_add(obj, "cached-register-memory", "any",
                         dsp_get_cached_register_memory, NULL, NULL, NULL);
}

static void dsp_finalize(Object *obj)
{
    MH2PRCCDSP *s = MH2P_RCC_DSP(obj);

    dsp_stop(s);
    timer_free(s->timer);
    for (unsigned i = 0; i < 2; i++) {
        address_space_destroy(&s->translated[i]);
    }
    if (s->module) {
        g_module_close(s->module);
    }
}

DeviceState *mh2p_rcc_dsp_create(Object *owner, DeviceState *mmu[2],
                                const char *backend)
{
    DeviceState *dev = qdev_new(TYPE_MH2P_RCC_DSP);
    MH2PRCCDSP *s = MH2P_RCC_DSP(dev);
    uint32_t (*version)(void);
    static const struct {
        const char *name;
        hwaddr base, size;
    } ram[] = {
        { "dsp1-l2", 0x40800000, 0x48000 },
        { "dsp1-l1p", 0x40e00000, 0x8000 },
        { "dsp1-l1d", 0x40f00000, 0x8000 },
    };

    object_property_add_child(owner, "dsp1", OBJECT(dev));
    s->module = g_module_open(backend, G_MODULE_BIND_LOCAL);
    if (!s->module) {
        error_report("mh2p-rcc-dsp: cannot load backend: %s", g_module_error());
        exit(EXIT_FAILURE);
    }
#define DSP_SYMBOL(name, dest) do { \
    if (!g_module_symbol(s->module, "c64xemu_backend_" name, \
                         (gpointer *)&dest)) { \
        error_report("mh2p-rcc-dsp: missing backend symbol: %s", \
                     g_module_error()); \
        exit(EXIT_FAILURE); \
    } \
} while (0)
    DSP_SYMBOL("abi_version", version);
    if (version() != 4) {
        error_report("mh2p-rcc-dsp: incompatible backend ABI");
        exit(EXIT_FAILURE);
    }
    DSP_SYMBOL("create", s->create);
    DSP_SYMBOL("destroy", s->destroy);
    DSP_SYMBOL("step", s->step);
    DSP_SYMBOL("event", s->event);
    DSP_SYMBOL("pc", s->pc);
    DSP_SYMBOL("cycles", s->cycles);
    DSP_SYMBOL("snapshot", s->snapshot);
    DSP_SYMBOL("cached_bytes", s->cached_bytes);
    DSP_SYMBOL("error", s->error);
#undef DSP_SYMBOL
    for (unsigned i = 0; i < 2; i++) {
        g_autofree char *name = g_strdup_printf("dsp1-mmu%u", i);

        s->mmu[i] = mmu[i];
        address_space_init(&s->translated[i],
            mh2p_ipu_mmu_memory(mmu[i], &address_space_memory), name);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(ram); i++) {
        memory_region_init_ram(&s->local[i], OBJECT(dev), ram[i].name,
                                ram[i].size, &error_fatal);
        memory_region_add_subregion(get_system_memory(), ram[i].base,
                                     &s->local[i]);
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0x40d00000);
    return dev;
}

static const VMStateDescription dsp_vmstate = {
    .name = TYPE_MH2P_RCC_DSP,
    .unmigratable = true,
};

static void dsp_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, dsp_reset);
    dc->vmsd = &dsp_vmstate;
    dc->user_creatable = false;
}

static const TypeInfo dsp_type = {
    .name = TYPE_MH2P_RCC_DSP,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MH2PRCCDSP),
    .instance_init = dsp_init,
    .instance_finalize = dsp_finalize,
    .class_init = dsp_class_init,
};

static void dsp_register_types(void)
{
    type_register_static(&dsp_type);
}

type_init(dsp_register_types)

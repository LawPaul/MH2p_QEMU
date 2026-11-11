/*
 * Bounded T124 security-engine AES DMA, key/IV packets and TZRAM.
 * Register protocol: NVIDIA arse.h and original T124 service accesses.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "crypto/aes.h"
#include "crypto/secret.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/misc/tegra124_se.h"
#include "system/address-spaces.h"

OBJECT_DECLARE_SIMPLE_TYPE(Tegra124SEState, TEGRA124_SE)

#define SE_MAX_DMA (1024 * 1024)
#define SE_TZRAM_SIZE (64 * 1024)

struct Tegra124SEState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    MemoryRegion tzram_mmio;
    qemu_irq irq;
    uint32_t tzram, config, crypto, input, output, blocks;
    uint32_t int_enable, int_status, error;
    uint32_t packet, staging[4];
    uint8_t keyiv[16][64];
    uint8_t valid[16];
    uint8_t access[16];
    uint8_t tzram_data[SE_TZRAM_SIZE];
    char *boot_key14_secret, *boot_key15_secret;
    uint8_t boot_keys[2][64];
    uint8_t boot_key_valid[2];
};

static void se_irq(Tegra124SEState *s)
{
    qemu_set_irq(s->irq, !!(s->int_status & s->int_enable));
}

static MemTxResult se_error(Tegra124SEState *s, const char *reason)
{
    qemu_log_mask(LOG_GUEST_ERROR, "tegra124-se: %s (key slot %u)\n",
                  reason, s->crypto >> 24 & 15);
    s->error |= 2;
    s->int_status |= 1U << 16;
    se_irq(s);
    return MEMTX_ERROR;
}

static bool se_ram(uint64_t address, size_t length)
{
    /* Reject MMIO DMA, including recursive accesses to this device. */
    MemoryRegionSection section = memory_region_find(
        get_system_memory(), address, length);
    bool valid = section.mr && memory_region_is_ram(section.mr) &&
                 !memory_region_is_rom(section.mr) &&
                 int128_ge(section.size, int128_make64(length));

    if (section.mr) {
        memory_region_unref(section.mr);
    }
    return valid;
}

static bool se_descriptor(uint32_t address, size_t length, uint32_t *buffer)
{
    uint8_t descriptor[12];

    if ((address & 3) || !se_ram(address, sizeof(descriptor)) ||
        address_space_read(&address_space_memory, address,
                           MEMTXATTRS_UNSPECIFIED, descriptor,
                           sizeof(descriptor)) != MEMTX_OK) {
        return false;
    }
    *buffer = ldl_le_p(descriptor + 4);
    return ldl_le_p(descriptor) == 0 &&
           ldl_le_p(descriptor + 8) >= length && se_ram(*buffer, length);
}

static MemTxResult se_execute(Tegra124SEState *s)
{
    unsigned slot = s->crypto >> 24 & 15;
    bool encrypt = s->crypto & (1U << 8);
    unsigned mode = s->config >> (encrypt ? 24 : 16) & 0xff;
    unsigned bits = mode == 0 ? 128 : mode == 1 ? 192 : 256;
    unsigned key_quads = bits == 128 ? 1 : 3;
    unsigned iv_quad = s->crypto & 0x80 ? 3 : 2;
    unsigned chaining = s->crypto & 0x7f;
    bool cbc = chaining == (encrypt ? 0x44 : 0x66);
    uint32_t input, output;
    uint64_t bytes = ((uint64_t)s->blocks + 1) * 16;
    size_t length;
    g_autofree uint8_t *source = NULL;
    g_autofree uint8_t *result = NULL;
    uint8_t iv[16], block[16];
    AES_KEY key;

    if (mode > 2 || s->config != ((mode << (encrypt ? 24 : 16)) |
                                 (encrypt ? 0x1000 : 0x100)) ||
        (s->crypto & ~0x0f0001ffU) ||
        (!cbc && chaining != 0 && chaining != 0x40) || bytes > SE_MAX_DMA) {
        return se_error(s, "unsupported AES configuration or DMA length");
    }
    length = bytes;
    if ((s->valid[slot] & key_quads) != key_quads) {
        return se_error(s, "AES key slot is unprovisioned");
    }
    if (cbc && !(s->valid[slot] & (1U << iv_quad))) {
        return se_error(s, "AES CBC IV is unprovisioned");
    }
    if (!se_descriptor(s->input, length, &input) ||
        !se_descriptor(s->output, length, &output)) {
        return se_error(s, "invalid or unsupported AES DMA descriptor");
    }
    source = g_malloc(length);
    result = g_malloc(length);
    if (address_space_read(&address_space_memory, input, MEMTXATTRS_UNSPECIFIED,
                           source, length) != MEMTX_OK) {
        return se_error(s, "AES DMA input failed");
    }
    if ((encrypt ? AES_set_encrypt_key(s->keyiv[slot], bits, &key) :
                   AES_set_decrypt_key(s->keyiv[slot], bits, &key)) != 0) {
        return se_error(s, "invalid AES key length");
    }
    memcpy(iv, s->keyiv[slot] + iv_quad * 16, 16);
    for (size_t offset = 0; offset < length; offset += 16) {
        memcpy(block, source + offset, 16);
        if (cbc && encrypt) {
            for (unsigned i = 0; i < 16; i++) {
                block[i] ^= iv[i];
            }
        }
        if (encrypt) {
            AES_encrypt(block, result + offset, &key);
        } else {
            AES_decrypt(block, result + offset, &key);
        }
        if (cbc) {
            for (unsigned i = 0; i < 16; i++) {
                if (!encrypt) {
                    result[offset + i] ^= iv[i];
                }
                iv[i] = encrypt ? result[offset + i] : source[offset + i];
            }
        }
    }
    memset(&key, 0, sizeof(key));
    if (address_space_write(&address_space_memory, output,
                            MEMTXATTRS_UNSPECIFIED, result,
                            length) != MEMTX_OK) {
        return se_error(s, "AES DMA output failed");
    }
    if (cbc) {
        memcpy(s->keyiv[slot] + 48, iv, 16);
        s->valid[slot] |= 8;
    }
    s->int_status |= 1U << 4;
    se_irq(s);
    return MEMTX_OK;
}

static MemTxResult se_read(void *opaque, hwaddr address, uint64_t *value,
                           unsigned size, MemTxAttrs attrs)
{
    Tegra124SEState *s = opaque;

    switch (address) {
    case 0x004:
        *value = s->tzram;
        break;
    case 0x00c:
        *value = s->int_enable;
        break;
    case 0x010:
        *value = s->int_status;
        break;
    case 0x014:
        *value = s->config;
        break;
    case 0x018:
        *value = s->input;
        break;
    case 0x024:
        *value = s->output;
        break;
    case 0x284 ... 0x2c0:
        *value = s->access[(address - 0x284) / 4];
        break;
    case 0x304:
        *value = s->crypto;
        break;
    case 0x318:
        *value = s->blocks;
        break;
    case 0x31c:
        *value = s->packet;
        break;
    case 0x320 ... 0x32c:
        *value = s->staging[(address - 0x320) / 4];
        break;
    case 0x800:
        *value = 0; /* Operations execute synchronously. */
        break;
    case 0x804:
        *value = s->error;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "tegra124-se: unsupported read 0x%"
                      HWADDR_PRIx "\n", address);
        return MEMTX_ERROR;
    }
    return MEMTX_OK;
}

static MemTxResult se_write(void *opaque, hwaddr address, uint64_t value,
                            unsigned size, MemTxAttrs attrs)
{
    Tegra124SEState *s = opaque;

    switch (address) {
    case 0x004:
        if (!attrs.secure && !(s->tzram & 1)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "tegra124-se: nonsecure TZRAM policy write denied\n");
            return MEMTX_OK;
        }
        if (value & ~3U) {
            return se_error(s, "unsupported TZRAM security policy");
        }
        s->tzram = value;
        break;
    case 0x008:
        if (value != 1) {
            return se_error(s, "unsupported SE operation");
        }
        return se_execute(s);
    case 0x00c:
        s->int_enable = value;
        se_irq(s);
        break;
    case 0x010:
        s->int_status &= ~value;
        se_irq(s);
        break;
    case 0x014:
        s->config = value;
        break;
    case 0x018:
        s->input = value;
        break;
    case 0x024:
        s->output = value;
        break;
    case 0x284 ... 0x2c0:
        if (value & ~0x3fU) {
            return se_error(s, "unsupported key-table access permissions");
        }
        s->access[(address - 0x284) / 4] = value;
        break;
    case 0x304:
        s->crypto = value;
        break;
    case 0x318:
        s->blocks = value;
        break;
    case 0x320 ... 0x32c:
        s->staging[(address - 0x320) / 4] = value;
        break;
    case 0x31c: {
        unsigned slot = value >> 4 & 15, quad = value >> 2 & 3;
        unsigned permission = (quad < 2 ? 0 : (quad - 1) * 2) +
                              !!(value & 0x200);

        if (value & ~0x2fcU) {
            return se_error(s, "unsupported key-table packet");
        }
        if (!(s->access[slot] & (1U << permission))) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "tegra124-se: key-table slot %u quad %u %s denied\n",
                          slot, quad, value & 0x200 ? "update" : "read");
            return se_error(s, "key-table packet access denied");
        }
        if (value & 0x200) {
            for (unsigned i = 0; i < 4; i++) {
                stl_le_p(s->keyiv[slot] + quad * 16 + i * 4, s->staging[i]);
            }
            s->valid[slot] |= 1U << quad;
        } else {
            if (!(s->valid[slot] & (1U << quad))) {
                return se_error(s, "key-table packet is unprovisioned");
            }
            for (unsigned i = 0; i < 4; i++) {
                s->staging[i] = ldl_le_p(s->keyiv[slot] + quad * 16 + i * 4);
            }
        }
        s->packet = value;
        break;
    }
    case 0x804:
        s->error &= ~value;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "tegra124-se: unsupported write 0x%"
                      HWADDR_PRIx "\n", address);
        return MEMTX_ERROR;
    }
    return MEMTX_OK;
}

static const MemoryRegionOps se_ops = {
    .read_with_attrs = se_read,
    .write_with_attrs = se_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static MemTxResult se_tzram_read(void *opaque, hwaddr address, uint64_t *value,
                                 unsigned size, MemTxAttrs attrs)
{
    Tegra124SEState *s = opaque;

    *value = 0;
    if (!attrs.secure && !(s->tzram & 1)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "tegra124-se: nonsecure TZRAM read denied\n");
        return MEMTX_ERROR;
    }
    for (unsigned i = 0; i < size; i++) {
        *value |= (uint64_t)s->tzram_data[address + i] << (8 * i);
    }
    return MEMTX_OK;
}

static MemTxResult se_tzram_write(void *opaque, hwaddr address, uint64_t value,
                                  unsigned size, MemTxAttrs attrs)
{
    Tegra124SEState *s = opaque;

    if (!attrs.secure && !(s->tzram & 1)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "tegra124-se: nonsecure TZRAM write denied\n");
        return MEMTX_ERROR;
    }
    for (unsigned i = 0; i < size; i++) {
        s->tzram_data[address + i] = value >> (8 * i);
    }
    return MEMTX_OK;
}

static const MemoryRegionOps se_tzram_ops = {
    .read_with_attrs = se_tzram_read,
    .write_with_attrs = se_tzram_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

static void se_reset(Object *obj, ResetType type)
{
    Tegra124SEState *s = TEGRA124_SE(obj);

    s->tzram = 1;
    s->config = s->crypto = s->input = s->output = s->blocks = 0;
    s->int_enable = s->int_status = s->error = s->packet = 0;
    memset(s->staging, 0, sizeof(s->staging));
    memset(s->keyiv, 0, sizeof(s->keyiv));
    memset(s->valid, 0, sizeof(s->valid));
    memset(s->access, 0x3f, sizeof(s->access));
    for (unsigned i = 0; i < 2; i++) {
        if (s->boot_key_valid[i]) {
            memcpy(s->keyiv[14 + i], s->boot_keys[i], 64);
            s->valid[14 + i] = s->boot_key_valid[i];
        }
    }
    se_irq(s);
}

static void se_realize(DeviceState *dev, Error **errp)
{
    Tegra124SEState *s = TEGRA124_SE(dev);
    const char *secrets[] = { s->boot_key14_secret, s->boot_key15_secret };

    for (unsigned i = 0; i < G_N_ELEMENTS(secrets); i++) {
        g_autofree uint8_t *key = NULL;
        size_t length;

        if (!secrets[i]) {
            continue;
        }
        if (qcrypto_secret_lookup(secrets[i], &key, &length, errp) < 0) {
            return;
        }
        if (length != 16 && length != 64) {
            memset(key, 0, length);
            error_setg(errp, "tegra124-se: boot-slot%u-secret requires "
                       "16 or 64 bytes",
                       14 + i);
            return;
        }
        memcpy(s->boot_keys[i], key, length);
        memset(key, 0, length);
        s->boot_key_valid[i] = length == 16 ? 1 : 15;
    }
}

static void se_finalize(Object *obj)
{
    Tegra124SEState *s = TEGRA124_SE(obj);

    memset(s->boot_keys, 0, sizeof(s->boot_keys));
    memset(s->keyiv, 0, sizeof(s->keyiv));
    memset(s->tzram_data, 0, sizeof(s->tzram_data));
}

static void se_init(Object *obj)
{
    Tegra124SEState *s = TEGRA124_SE(obj);

    memory_region_init_io(&s->mmio, obj, &se_ops, s, TYPE_TEGRA124_SE, 0x2000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    memory_region_init_io(&s->tzram_mmio, obj, &se_tzram_ops, s,
                          "tegra124-tzram", SE_TZRAM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->tzram_mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void se_class_init(ObjectClass *klass, const void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);
    static const Property properties[] = {
        DEFINE_PROP_STRING("boot-slot14-secret", Tegra124SEState,
                           boot_key14_secret),
        DEFINE_PROP_STRING("boot-slot15-secret", Tegra124SEState,
                           boot_key15_secret),
    };

    rc->phases.hold = se_reset;
    dc->realize = se_realize;
    device_class_set_props(dc, properties);
}

static const TypeInfo se_info = {
    .name = TYPE_TEGRA124_SE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Tegra124SEState),
    .instance_init = se_init,
    .instance_finalize = se_finalize,
    .class_init = se_class_init,
};

static void se_register_types(void)
{
    type_register_static(&se_info);
}
type_init(se_register_types)

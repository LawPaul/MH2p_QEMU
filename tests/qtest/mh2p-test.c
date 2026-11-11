/*
 * Firmware-free checks for the experimental MH2P direct-start machine.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"
#include "libqtest.h"

static QTestState *start_mh2p(const char *extra)
{
    g_autofree char *image = NULL;
    g_autoptr(GError) error = NULL;
    const uint8_t branch_to_self[] = { 0xfe, 0xff, 0xff, 0xea };
    int fd = g_file_open_tmp("mh2p-test-XXXXXX", &image, &error);
    QTestState *s;

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, branch_to_self, sizeof(branch_to_self)), ==,
                    sizeof(branch_to_self));
    close(fd);
    s = qtest_initf("-M mh2p -m 256M -kernel %s -serial null -serial null %s",
                    image, extra);
    unlink(image);
    return s;
}

static void test_boot_registers(void)
{
    QTestState *s = start_mh2p("");
    g_assert_cmphex(qtest_readl(s, 0x70000804) >> 8 & 0xff, ==, 0x40);
    g_assert_cmphex(qtest_readl(s, 0x60006050), ==, 0x80000000);
    g_assert_cmphex(qtest_readl(s, 0x50041004) & 0x1f, ==, 5);
    g_assert_cmphex(qtest_readl(s, 0x80000104), ==, 0x54410001);
    g_assert_cmphex(qtest_readl(s, 0x8000011c), ==, 0x10000000);
    g_assert_cmphex(qtest_readl(s, 0x80008000), ==, 0xeafffffe);

    qtest_writel(s, 0x60006310, 0x55);
    g_assert_cmphex(qtest_readl(s, 0x6000600c), ==, 0x55);
    qtest_writel(s, 0x60006314, 0x05);
    g_assert_cmphex(qtest_readl(s, 0x6000600c), ==, 0x50);
    qtest_writel(s, 0x60006440, 0x1234);
    qtest_writel(s, 0x60006444, 0x0030);
    g_assert_cmphex(qtest_readl(s, 0x60006360), ==, 0x1204);
    qtest_writel(s, 0x60006178, 0x40000004);
    g_assert_cmphex(qtest_readl(s, 0x60006178), ==, 0x40000004);
    qtest_writel(s, 0x7000ead8, 0x12345678);
    g_assert_cmphex(qtest_readl(s, 0x7000ead8), ==, 0x12345678);
    g_assert_cmphex(qtest_readl(s, 0x7000e400), ==, 0);
    qtest_writel(s, 0x7000e400, 1 << 17);
    g_assert_cmphex(qtest_readl(s, 0x7000e400), ==, 1 << 17);
    g_assert_cmphex(qtest_readl(s, 0x7000e5b4), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x6000c200), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x60006048), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x70012004), ==, 1);
    qtest_writel(s, 0x70012004, 3);
    g_assert_cmphex(qtest_readl(s, 0x70012004), ==, 3);
    qtest_writel(s, 0x60006328, 1 << 7);
    qtest_writel(s, 0x60006048, 1 << 28);
    g_assert_cmphex(qtest_readl(s, 0x60006048), ==, 1 << 28);
    g_assert_cmphex(qtest_readl(s, 0x7000f800), ==, 4 << 16);
    for (unsigned cpu = 0; cpu < 4; cpu++) {
        uint32_t csr = 0x60007000 + (cpu ? 0x10 + cpu * 8 : 8);
        uint32_t halt = cpu ? csr - 4 : 0x60007000;

        qtest_writel(s, halt, 0);
        qtest_writel(s, csr, 0);
        g_assert_cmphex(qtest_readl(s, halt), ==, 0);
        g_assert_cmphex(qtest_readl(s, csr), ==, 0);
        qtest_writel(s, csr, (1 << 14) | (1 << 15));
        g_assert_cmphex(qtest_readl(s, csr), ==, 0);
    }

    qtest_writel(s, 0x7000605c, 0x5a); /* UART B scratch register */
    g_assert_cmphex(qtest_readl(s, 0x7000605c), ==, 0x5a);
    qtest_writel(s, 0x80008000, 0);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, 0x80008000), ==, 0xeafffffe);
    g_assert_cmphex(qtest_readl(s, 0x7000ead8), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x6000600c), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x60006360), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x60006178), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x7000e400), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x7000e5b4), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x60006048), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x70012004), ==, 1);

    qtest_quit(s);
}

static void test_legacy_interrupt_gates(void)
{
    QTestState *s = start_mh2p("");

    for (unsigned bank = 0; bank < 5; bank++) {
        uint32_t base = 0x60004000 + bank * 0x100;
        uint32_t pending = 0x50041204 + bank * 4;
        uint32_t clear = 0x50041284 + bank * 4;

        g_assert_cmphex(qtest_readl(s, base + 0x20), ==, UINT32_MAX);
        qtest_writel(s, base + 0x28, UINT32_MAX);
        qtest_writel(s, clear, UINT32_MAX);
        qtest_writel(s, base + 0x18, 1);
        g_assert_cmphex(qtest_readl(s, base + 0x14), ==, 1);
        g_assert_cmphex(qtest_readl(s, pending) & 1, ==, 0);
        qtest_writel(s, base + 0x24, 1);
        g_assert_cmphex(qtest_readl(s, base + 0x20), ==, 1);
        g_assert_cmphex(qtest_readl(s, pending) & 1, ==, 1);
        qtest_writel(s, base + 0x1c, 1);
        qtest_writel(s, clear, UINT32_MAX);
        g_assert_cmphex(qtest_readl(s, pending) & 1, ==, 0);
    }
    qtest_system_reset(s);
    for (unsigned bank = 0; bank < 5; bank++) {
        uint32_t base = 0x60004000 + bank * 0x100;

        g_assert_cmphex(qtest_readl(s, base + 0x20), ==, UINT32_MAX);
        g_assert_cmphex(qtest_readl(s, base + 0x14), ==, 0);
    }
    qtest_quit(s);

    s = start_mh2p("-M experimental-el3=on");
    for (unsigned bank = 0; bank < 5; bank++) {
        uint32_t base = 0x60004000 + bank * 0x100;

        g_assert_cmphex(qtest_readl(s, base + 0x20), ==, 0);
        qtest_writel(s, base + 0x24, UINT32_MAX);
        qtest_writel(s, base + 0x18, 1);
    }
    qtest_system_reset(s);
    for (unsigned bank = 0; bank < 5; bank++) {
        uint32_t base = 0x60004000 + bank * 0x100;

        g_assert_cmphex(qtest_readl(s, base + 0x20), ==, 0);
        g_assert_cmphex(qtest_readl(s, base + 0x14), ==, 0);
    }
    qtest_quit(s);
}

static void test_pmc_secure_scratch(void)
{
    QTestState *s = start_mh2p("");
    const uint32_t disable = 0x7000e6c4;

    g_assert_cmphex(qtest_readl(s, disable), ==, 0);
    for (unsigned index = 0; index < 16; index++) {
        uint32_t address = 0x7000e700 + index * 4;
        uint32_t value = 0x12340000 + index;

        g_assert_cmphex(qtest_readl(s, address), ==, 0);
        qtest_writel(s, address, value);
        g_assert_cmphex(qtest_readl(s, address), ==, value);
        qtest_writel(s, disable, 1U << (index * 2));
        g_assert_cmphex(qtest_readl(s, disable), ==, 1U << (index * 2));
        qtest_writel(s, address, ~value);
        g_assert_cmphex(qtest_readl(s, address), ==, value);
        qtest_writel(s, disable, 2U << (index * 2));
        g_assert_cmphex(qtest_readl(s, address), ==, 0);
        qtest_writel(s, address, ~value);
        qtest_writel(s, disable, 0);
        g_assert_cmphex(qtest_readl(s, address), ==, ~value);
    }
    qtest_writel(s, disable, UINT32_MAX);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, disable), ==, 0);
    for (unsigned index = 0; index < 16; index++) {
        g_assert_cmphex(qtest_readl(s, 0x7000e700 + index * 4), ==, 0);
    }
    qtest_quit(s);
}

static void test_fuse_security_mode(void)
{
    const char *profiles[] = {
        "-M experimental-fuse-security-mode=development",
        "-M experimental-fuse-security-mode=production",
    };

    for (unsigned mode = 0; mode < G_N_ELEMENTS(profiles); mode++) {
        QTestState *s = start_mh2p(profiles[mode]);

        qtest_writel(s, 0x60006328, 1 << 7);
        qtest_writel(s, 0x60006048, 1 << 28);
        g_assert_cmphex(qtest_readl(s, 0x7000f9a0), ==, mode);
        qtest_system_reset(s);
        qtest_writel(s, 0x60006328, 1 << 7);
        qtest_writel(s, 0x60006048, 1 << 28);
        g_assert_cmphex(qtest_readl(s, 0x7000f9a0), ==, mode);
        qtest_quit(s);
    }
}

static void se_test_bytes(const char *hex, uint8_t *bytes)
{
    g_assert_cmpuint(strlen(hex) % 2, ==, 0);
    for (unsigned i = 0; hex[i * 2]; i++) {
        bytes[i] = (g_ascii_xdigit_value(hex[i * 2]) << 4) |
                    g_ascii_xdigit_value(hex[i * 2 + 1]);
    }
}

static void se_test_packet(QTestState *s, unsigned packet, const uint8_t *data)
{
    for (unsigned i = 0; i < 4; i++) {
        qtest_writel(s, 0x70012320 + i * 4, ldl_le_p(data + i * 4));
    }
    qtest_writel(s, 0x7001231c, 0x200 | packet);
}

static void test_se_aes_dma(void)
{
    const char *keys[] = {
        "2b7e151628aed2a6abf7158809cf4f3c",
        "8e73b0f7da0e6452c810f32b809079e562f8ead2522c6b7b",
        "603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4",
    };
    const char *ecb[] = {
        "3ad77bb40d7a3660a89ecaf32466ef97"
        "f5d3d58503b9699de785895a96fdbaaf",
        "bd334f1d6e45f25ff712a214571fa5cc"
        "974104846d0ad3ad7734ecb3ecee4eef",
        "f3eed1bdb5d2a03c064b5a7e3db181f8"
        "591ccb10d410ed26dc5ba74a31362870",
    };
    const char *cbc[] = {
        "7649abac8119b246cee98e9b12e9197d"
        "5086cb9b507219ee95db113a917678b2",
        "4f021db243bc633d7178183a9fa071e8"
        "b4d9ada9ad7dedf4e5e738763f69145a",
        "f58c4c04d6e5f1ba779eabfb5f7bfbd6"
        "9cfc4e967edb808d679f777bc6702c7d",
    };
    uint8_t plain[32], key[32], iv[16], expected[32], actual[32];
    QTestState *s = start_mh2p("");

    se_test_bytes("6bc1bee22e409f96e93d7e117393172a"
                  "ae2d8a571e03ac9c9eb76fac45af8e51", plain);
    se_test_bytes("000102030405060708090a0b0c0d0e0f", iv);
    qtest_writel(s, 0x80010000, 0);
    qtest_writel(s, 0x80010004, 0x80011000);
    qtest_writel(s, 0x80010008, 32);
    qtest_writel(s, 0x80010010, 0);
    qtest_writel(s, 0x80010014, 0x80012000);
    qtest_writel(s, 0x80010018, 32);
    qtest_writel(s, 0x70012018, 0x80010000);
    qtest_writel(s, 0x70012024, 0x80010010);
    qtest_writel(s, 0x70012318, 1);
    qtest_writel(s, 0x7001200c, (1 << 4) | (1 << 16));

    for (unsigned mode = 0; mode < 3; mode++) {
        memset(key, 0, sizeof(key));
        se_test_bytes(keys[mode], key);
        se_test_packet(s, 0, key);
        if (mode) {
            se_test_packet(s, 4, key + 16);
        }
        qtest_writel(s, 0x7001231c, 0);
        for (unsigned i = 0; i < 4; i++) {
            g_assert_cmpuint(qtest_readl(s, 0x70012320 + i * 4),
                            ==, (uint32_t)ldl_le_p(key + i * 4));
        }
        for (unsigned chain = 0; chain < 2; chain++) {
            g_test_message("AES key mode %u chaining %u", mode, chain);
            se_test_bytes(chain ? cbc[mode] : ecb[mode], expected);
            se_test_packet(s, 8, iv);
            qtest_memwrite(s, 0x80011000, plain, 32);
            qtest_writel(s, 0x70012014, (mode << 24) | 0x1000);
            qtest_writel(s, 0x70012304, chain ? 0x144 : 0x140);
            qtest_writel(s, 0x70012008, 1);
            g_assert_cmphex(qtest_readl(s, 0x70012804), ==, 0);
            g_assert_cmphex(qtest_readl(s, 0x70012010), ==, 1 << 4);
            g_assert_cmphex(qtest_readl(s, 0x50041208) & (1 << 26),
                            ==, 1 << 26);
            qtest_memread(s, 0x80012000, actual, 32);
            if (memcmp(actual, expected, 32)) {
                for (unsigned i = 0; i < 32; i++) {
                    g_test_message("byte %u: actual %02x expected %02x",
                                   i, actual[i], expected[i]);
                }
            }
            g_assert_cmpmem(actual, 32, expected, 32);
            if (chain) {
                qtest_writel(s, 0x7001231c, 12);
                for (unsigned i = 0; i < 4; i++) {
                    g_assert_cmpuint(qtest_readl(s, 0x70012320 + i * 4),
                                    ==, (uint32_t)ldl_le_p(expected + 16 + i * 4));
                }
            }
            qtest_writel(s, 0x70012010, 1 << 4);
            qtest_writel(s, 0x50041288, 1 << 26);
            g_assert_cmphex(qtest_readl(s, 0x50041208) & (1 << 26), ==, 0);
            /* In-place decryption must preserve CBC's previous ciphertext. */
            qtest_writel(s, 0x70012018, 0x80010010);
            qtest_writel(s, 0x70012014, (mode << 16) | 0x100);
            qtest_writel(s, 0x70012304, chain ? 0x66 : 0);
            qtest_writel(s, 0x70012008, 1);
            qtest_memread(s, 0x80012000, actual, 32);
            g_assert_cmpmem(actual, 32, plain, 32);
            qtest_writel(s, 0x70012010, 1 << 4);
            qtest_writel(s, 0x70012018, 0x80010000);
        }
    }
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, 0x70012010), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x7001200c), ==, 0);
    qtest_memwrite(s, 0x80012000, plain, 32);
    qtest_writel(s, 0x70012014, 0x1000);
    qtest_writel(s, 0x70012304, 0x140);
    qtest_writel(s, 0x70012008, 1);
    g_assert_cmphex(qtest_readl(s, 0x70012010), ==, 1 << 16);
    g_assert_cmphex(qtest_readl(s, 0x70012804), !=, 0);
    qtest_memread(s, 0x80012000, actual, 32);
    g_assert_cmpmem(actual, 32, plain, 32);
    qtest_quit(s);
}

static void test_se_boot_keys(void)
{
    uint8_t key[16], plain[16], expected[16], actual[16], zero[16] = { 0 };
    g_autofree char *encoded = NULL;

    se_test_bytes("2b7e151628aed2a6abf7158809cf4f3c", key);
    se_test_bytes("6bc1bee22e409f96e93d7e117393172a", plain);
    se_test_bytes("3ad77bb40d7a3660a89ecaf32466ef97", expected);
    encoded = g_base64_encode(key, sizeof(key));
    for (unsigned supplied = 1; supplied <= 3; supplied++) {
        g_autofree char *extra = g_strdup_printf(
            "-object secret,id=bootkey,format=base64,data=%s %s %s",
            encoded,
            supplied & 1 ?
            "-global tegra124-se.boot-slot14-secret=bootkey" : "",
            supplied & 2 ?
            "-global tegra124-se.boot-slot15-secret=bootkey" : "");
        QTestState *s = start_mh2p(extra);

        for (unsigned reset = 0; reset < 2; reset++) {
            qtest_writel(s, 0x80010000, 0);
            qtest_writel(s, 0x80010004, 0x80011000);
            qtest_writel(s, 0x80010008, 16);
            qtest_writel(s, 0x80010010, 0);
            qtest_writel(s, 0x80010014, 0x80012000);
            qtest_writel(s, 0x80010018, 16);
            qtest_memwrite(s, 0x80011000, plain, 16);
            qtest_writel(s, 0x70012018, 0x80010000);
            qtest_writel(s, 0x70012024, 0x80010010);
            qtest_writel(s, 0x70012014, 0x1000);
            for (unsigned slot = 14; slot <= 15; slot++) {
                bool present = supplied & (1U << (slot - 14));

                qtest_memwrite(s, 0x80012000, plain, 16);
                qtest_writel(s, 0x70012010, UINT32_MAX);
                qtest_writel(s, 0x70012804, UINT32_MAX);
                qtest_writel(s, 0x70012304, (slot << 24) | 0x140);
                qtest_writel(s, 0x70012008, 1);
                g_assert_cmphex(qtest_readl(s, 0x70012010), ==,
                               present ? 1 << 4 : 1 << 16);
                qtest_memread(s, 0x80012000, actual, 16);
                g_assert_cmpmem(actual, 16, present ? expected : plain, 16);
                if (present) {
                    /* A boot key does not invent an IV or a second key quad. */
                    qtest_writel(s, 0x70012010, UINT32_MAX);
                    qtest_writel(s, 0x70012304, (slot << 24) | 0x144);
                    qtest_writel(s, 0x70012008, 1);
                    g_assert_cmphex(qtest_readl(s, 0x70012010), ==, 1 << 16);
                    qtest_writel(s, 0x70012010, UINT32_MAX);
                    qtest_writel(s, 0x70012014, 0x01001000);
                    qtest_writel(s, 0x70012304, (slot << 24) | 0x140);
                    qtest_writel(s, 0x70012008, 1);
                    g_assert_cmphex(qtest_readl(s, 0x70012010), ==, 1 << 16);
                    qtest_writel(s, 0x70012014, 0x1000);
                }
                se_test_packet(s, slot << 4, zero);
            }
            qtest_writel(s, 0x70012010, UINT32_MAX);
            qtest_writel(s, 0x70012304, 0x140);
            qtest_writel(s, 0x70012008, 1);
            g_assert_cmphex(qtest_readl(s, 0x70012010), ==, 1 << 16);
            qtest_system_reset(s);
        }
        qtest_quit(s);
    }
}

static void test_se_boot_slot_context(void)
{
    uint8_t context[64], plain[16], expected[16], actual[16], zero[16] = { 0 };
    g_autofree char *encoded = NULL;
    g_autofree char *extra = NULL;
    QTestState *s;

    se_test_bytes("2b7e151628aed2a6abf7158809cf4f3c"
                  "00112233445566778899aabbccddeeff"
                  "000102030405060708090a0b0c0d0e0f"
                  "0f0e0d0c0b0a09080706050403020100", context);
    se_test_bytes("6bc1bee22e409f96e93d7e117393172a", plain);
    se_test_bytes("7649abac8119b246cee98e9b12e9197d", expected);
    encoded = g_base64_encode(context, sizeof(context));
    extra = g_strdup_printf(
        "-object secret,id=bootcontext,format=base64,data=%s "
        "-global tegra124-se.boot-slot15-secret=bootcontext", encoded);
    s = start_mh2p(extra);
    for (unsigned reset = 0; reset < 2; reset++) {
        for (unsigned quad = 0; quad < 4; quad++) {
            qtest_writel(s, 0x7001231c, 0xf0 | (quad << 2));
            for (unsigned word = 0; word < 4; word++) {
                g_assert_cmphex(qtest_readl(s, 0x70012320 + word * 4), ==,
                               (uint32_t)ldl_le_p(context + quad * 16 + word * 4));
            }
        }
        qtest_writel(s, 0x80010000, 0);
        qtest_writel(s, 0x80010004, 0x80011000);
        qtest_writel(s, 0x80010008, 16);
        qtest_writel(s, 0x80010010, 0);
        qtest_writel(s, 0x80010014, 0x80012000);
        qtest_writel(s, 0x80010018, 16);
        qtest_memwrite(s, 0x80011000, plain, 16);
        qtest_writel(s, 0x70012018, 0x80010000);
        qtest_writel(s, 0x70012024, 0x80010010);
        qtest_writel(s, 0x70012014, 0x1000);
        qtest_writel(s, 0x70012304, 0x0f000144);
        qtest_writel(s, 0x700122c0, 0x3c);
        qtest_writel(s, 0x70012008, 1);
        g_assert_cmphex(qtest_readl(s, 0x70012010), ==, 1 << 4);
        qtest_memread(s, 0x80012000, actual, 16);
        g_assert_cmpmem(actual, 16, expected, 16);
        qtest_writel(s, 0x700122c0, 0x3f);
        for (unsigned quad = 0; quad < 4; quad++) {
            se_test_packet(s, 0xf0 | (quad << 2), zero);
        }
        qtest_system_reset(s);
    }
    qtest_quit(s);
}

static void test_se_keytable_access(void)
{
    uint8_t original[16], replacement[16] = { 0 };
    QTestState *s = start_mh2p("");

    se_test_bytes("00112233445566778899aabbccddeeff", original);
    for (unsigned slot = 0; slot < 16; slot++) {
        uint32_t access = 0x70012284 + slot * 4;

        g_assert_cmphex(qtest_readl(s, access), ==, 0x3f);
        for (unsigned quad = 0; quad < 4; quad++) {
            unsigned packet = (slot << 4) | (quad << 2);
            unsigned bit = quad < 2 ? 0 : (quad - 1) * 2;

            se_test_packet(s, packet, original);
            qtest_writel(s, access, 0x3f & ~(2U << bit));
            se_test_packet(s, packet, replacement);
            g_assert_cmphex(qtest_readl(s, 0x70012010), ==, 1 << 16);
            qtest_writel(s, 0x70012010, UINT32_MAX);
            qtest_writel(s, 0x7001231c, packet);
            for (unsigned word = 0; word < 4; word++) {
                g_assert_cmphex(qtest_readl(s, 0x70012320 + word * 4), ==,
                               (uint32_t)ldl_le_p(original + word * 4));
            }
            qtest_writel(s, access, 0x3f & ~(1U << bit));
            se_test_packet(s, packet, replacement);
            g_assert_cmphex(qtest_readl(s, 0x70012010), ==, 0);
            qtest_writel(s, 0x7001231c, packet);
            g_assert_cmphex(qtest_readl(s, 0x70012010), ==, 1 << 16);
            qtest_writel(s, 0x70012010, UINT32_MAX);
            qtest_writel(s, access, 0x3f);
            qtest_writel(s, 0x7001231c, packet);
            for (unsigned word = 0; word < 4; word++) {
                g_assert_cmphex(qtest_readl(s, 0x70012320 + word * 4), ==, 0);
            }
        }
        qtest_writel(s, access, 0x3d);
        qtest_writel(s, access, 0x40);
        g_assert_cmphex(qtest_readl(s, access), ==, 0x3d);
        qtest_writel(s, 0x70012010, UINT32_MAX);
    }
    qtest_system_reset(s);
    for (unsigned slot = 0; slot < 16; slot++) {
        g_assert_cmphex(qtest_readl(s, 0x70012284 + slot * 4), ==, 0x3f);
    }
    qtest_quit(s);
}

static void test_se_tzram(void)
{
    const uint64_t base = 0x7c010000, end = base + 64 * 1024;
    QTestState *s = start_mh2p("");

    g_assert_cmphex(qtest_readq(s, base), ==, 0);
    g_assert_cmphex(qtest_readq(s, end - 8), ==, 0);
    qtest_writeq(s, base, 0x1122334455667788ULL);
    qtest_writeq(s, end - 8, 0x8877665544332211ULL);
    g_assert_cmphex(qtest_readb(s, base), ==, 0x88);
    g_assert_cmphex(qtest_readw(s, base + 1), ==, 0x6677);
    g_assert_cmphex(qtest_readl(s, base + 3), ==, 0x22334455);
    g_assert_cmphex(qtest_readq(s, end - 8), ==, 0x8877665544332211ULL);
    qtest_writel(s, 0x70012004, 3);
    qtest_writeb(s, base + 7, 0xa5);
    g_assert_cmphex(qtest_readq(s, base), ==, 0xa522334455667788ULL);
    qtest_writel(s, 0x70012004, 0);
    g_assert_cmphex(qtest_readl(s, base), ==, 0);
    qtest_writel(s, base, 0xdeadbeef);
    qtest_writeq(s, end - 8, 0);
    qtest_writel(s, 0x70012004, 1);
    g_assert_cmphex(qtest_readl(s, 0x70012004), ==, 0);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, 0x70012004), ==, 1);
    g_assert_cmphex(qtest_readq(s, base), ==, 0xa522334455667788ULL);
    g_assert_cmphex(qtest_readq(s, end - 8), ==, 0x8877665544332211ULL);
    qtest_quit(s);
}

static void test_se_dma_rejections(void)
{
    const uint32_t invalid[][3] = {
        { 1, 0x80011000, 16 }, /* Scatter/gather not implemented. */
        { 0, 0x80011000, 15 },
        { 0, 0x70012008, 16 }, /* Recursive device DMA. */
        { 0, 0xfffffff8, 16 },
        { 0, 0x8ffffff8, 16 }, /* Partially outside configured RAM. */
    };
    uint8_t key[16] = { 0 }, sentinel[16], actual[16];
    QTestState *s = start_mh2p("");

    memset(sentinel, 0xa5, sizeof(sentinel));
    se_test_packet(s, 0, key);
    qtest_writel(s, 0x70012014, 0x1000);
    qtest_writel(s, 0x70012304, 0x100);
    qtest_writel(s, 0x70012018, 0x80010000);
    qtest_writel(s, 0x70012024, 0x80010010);
    qtest_writel(s, 0x80010010, 0);
    qtest_writel(s, 0x80010014, 0x80012000);
    qtest_writel(s, 0x80010018, 16);
    qtest_memwrite(s, 0x80012000, sentinel, 16);
    for (unsigned i = 0; i < G_N_ELEMENTS(invalid); i++) {
        qtest_writel(s, 0x70012010, UINT32_MAX);
        qtest_writel(s, 0x70012804, UINT32_MAX);
        for (unsigned word = 0; word < 3; word++) {
            qtest_writel(s, 0x80010000 + word * 4, invalid[i][word]);
        }
        qtest_writel(s, 0x70012008, 1);
        g_assert_cmphex(qtest_readl(s, 0x70012010), ==, 1 << 16);
        g_assert_cmphex(qtest_readl(s, 0x70012804), !=, 0);
        qtest_memread(s, 0x80012000, actual, 16);
        g_assert_cmpmem(actual, 16, sentinel, 16);
    }
    qtest_writel(s, 0x70012010, UINT32_MAX);
    qtest_writel(s, 0x70012318, UINT32_MAX);
    qtest_writel(s, 0x70012008, 1);
    g_assert_cmphex(qtest_readl(s, 0x70012010), ==, 1 << 16);
    qtest_memread(s, 0x80012000, actual, 16);
    g_assert_cmpmem(actual, 16, sentinel, 16);
    qtest_writel(s, 0x70012318, 0);
    qtest_writel(s, 0x80010000, 0);
    qtest_writel(s, 0x80010004, 0x80011000);
    qtest_writel(s, 0x80010008, 16);
    qtest_writel(s, 0x80010014, 0x70012320);
    qtest_writel(s, 0x70012010, UINT32_MAX);
    qtest_writel(s, 0x70012008, 1);
    g_assert_cmphex(qtest_readl(s, 0x70012010), ==, 1 << 16);
    qtest_memread(s, 0x80012000, actual, 16);
    g_assert_cmpmem(actual, 16, sentinel, 16);
    qtest_writel(s, 0x70012804, UINT32_MAX);
    g_assert_cmphex(qtest_readl(s, 0x70012804), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x70012010), ==, 1 << 16);
    qtest_writel(s, 0x70012010, UINT32_MAX);
    g_assert_cmphex(qtest_readl(s, 0x70012010), ==, 0);
    qtest_quit(s);
}

static void test_diagnostic_pointer(void)
{
    g_autofree char *path = NULL;
    g_autofree char *extra = NULL;
    g_autofree char *contents = NULL;
    g_autoptr(GError) error = NULL;
    int fd = g_file_open_tmp("mh2p-pointer-XXXXXX", &path, &error);
    QTestState *s;

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    extra = g_strdup_printf("-chardev file,id=touch,path=%s "
                            "-device mh2p-diagnostic-pointer,chardev=touch",
                            path);
    s = start_mh2p(extra);
    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    qtest_qmp_assert_success(s,
        "{'execute':'input-send-event','arguments':{'events':["
        "{'type':'abs','data':{'axis':'x','value':32767}},"
        "{'type':'abs','data':{'axis':'y','value':32767}},"
        "{'type':'btn','data':{'button':'left','down':true}}]}}");
    qtest_qmp_assert_success(s,
        "{'execute':'input-send-event','arguments':{'events':["
        "{'type':'btn','data':{'button':'left','down':true}}]}}");
    qtest_qmp_assert_success(s,
        "{'execute':'input-send-event','arguments':{'events':["
        "{'type':'abs','data':{'axis':'x','value':0}},"
        "{'type':'abs','data':{'axis':'y','value':0}}]}}");
    qtest_qmp_assert_success(s,
        "{'execute':'input-send-event','arguments':{'events':["
        "{'type':'btn','data':{'button':'left','down':false}}]}}");
    qtest_qmp_assert_success(s,
        "{'execute':'input-send-event','arguments':{'events':["
        "{'type':'btn','data':{'button':'left','down':true}}]}}");
    qtest_system_reset(s);
    qtest_qmp_assert_success(s,
        "{'execute':'input-send-event','arguments':{'events':["
        "{'type':'abs','data':{'axis':'x','value':0}},"
        "{'type':'abs','data':{'axis':'y','value':0}},"
        "{'type':'btn','data':{'button':'right','down':true}}]}}");
    qtest_qmp_assert_success(s,
        "{'execute':'input-send-event','arguments':{'events':["
        "{'type':'btn','data':{'button':'left','down':true}}]}}");
    qtest_qmp_assert_success(s,
        "{'execute':'input-send-event','arguments':{'events':["
        "{'type':'btn','data':{'button':'left','down':false}}]}}");
    qtest_qmp_assert_success(s,
        "{'execute':'input-send-event','arguments':{'events':["
        "{'type':'btn','data':{'button':'left','down':true}},"
        "{'type':'btn','data':{'button':'left','down':false}}]}}");
    qtest_quit(s);
    g_assert_true(g_file_get_contents(path, &contents, NULL, &error));
    g_assert_no_error(error);
    g_assert_cmpstr(contents, ==, "press 1919 719\nrelease 0 0\n"
                                 "press 0 0\ncancel 0 0\n"
                                 "press 0 0\nrelease 0 0\n"
                                 "press 0 0\nrelease 0 0\n");
    unlink(path);
}

static void test_diagnostic_pointer_motion(void)
{
    g_autofree char *path = NULL;
    g_autofree char *extra = NULL;
    g_autofree char *contents = NULL;
    g_autoptr(GError) error = NULL;
    int fd = g_file_open_tmp("mh2p-motion-XXXXXX", &path, &error);
    QTestState *s;

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    extra = g_strdup_printf("-chardev file,id=touch,path=%s "
                            "-device mh2p-diagnostic-pointer,chardev=touch,"
                            "motion=on", path);
    s = start_mh2p(extra);
    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    qtest_qmp_assert_success(s,
        "{'execute':'input-send-event','arguments':{'events':["
        "{'type':'abs','data':{'axis':'x','value':32767}},"
        "{'type':'abs','data':{'axis':'y','value':32767}}]}}");
    qtest_qmp_assert_success(s,
        "{'execute':'input-send-event','arguments':{'events':["
        "{'type':'btn','data':{'button':'left','down':true}}]}}");
    qtest_qmp_assert_success(s,
        "{'execute':'input-send-event','arguments':{'events':["
        "{'type':'abs','data':{'axis':'x','value':0}},"
        "{'type':'abs','data':{'axis':'y','value':0}}]}}");
    qtest_qmp_assert_success(s,
        "{'execute':'input-send-event','arguments':{'events':["
        "{'type':'abs','data':{'axis':'x','value':0}},"
        "{'type':'abs','data':{'axis':'y','value':0}},"
        "{'type':'btn','data':{'button':'left','down':true}}]}}");
    qtest_qmp_assert_success(s,
        "{'execute':'input-send-event','arguments':{'events':["
        "{'type':'abs','data':{'axis':'x','value':32767}},"
        "{'type':'btn','data':{'button':'left','down':false}}]}}");
    qtest_qmp_assert_success(s,
        "{'execute':'input-send-event','arguments':{'events':["
        "{'type':'abs','data':{'axis':'y','value':32767}}]}}");
    qtest_qmp_assert_success(s,
        "{'execute':'input-send-event','arguments':{'events':["
        "{'type':'btn','data':{'button':'left','down':true}}]}}");
    qtest_qmp_assert_success(s,
        "{'execute':'input-send-event','arguments':{'events':["
        "{'type':'abs','data':{'axis':'x','value':0}}]}}");
    qtest_system_reset(s);
    qtest_qmp_assert_success(s,
        "{'execute':'input-send-event','arguments':{'events':["
        "{'type':'abs','data':{'axis':'x','value':32767}}]}}");
    qtest_quit(s);
    g_assert_true(g_file_get_contents(path, &contents, NULL, &error));
    g_assert_no_error(error);
    g_assert_cmpstr(contents, ==, "press 1919 719\nmove 0 0\n"
                                 "release 1919 0\npress 1919 719\n"
                                 "move 0 719\ncancel 0 719\n");
    unlink(path);
}

static void test_runtime_clocks(void)
{
    static const uint64_t sources[] = {
        0x600063c4, 0x600063fc, 0x60006600, 0x60006610,
        0x6000662c, 0x60006630, 0x60006658, 0x6000665c,
    };
    QTestState *s = start_mh2p("");

    g_assert_cmphex(qtest_readl(s, 0x6000605c), ==, 0);
    qtest_writel(s, 0x60006058, 1);
    qtest_clock_step(s, 100000);
    g_assert_cmphex(qtest_readl(s, 0x6000605c), ==, 0);
    qtest_writel(s, 0x60006058, 0x80000001);
    g_assert_cmphex(qtest_readl(s, 0x60006058), ==, 1);
    g_assert_cmphex(qtest_readl(s, 0x6000605c), ==, 0x80000000);
    qtest_clock_step(s, 61035);
    g_assert_cmphex(qtest_readl(s, 0x6000605c), ==, 0x80000000);
    qtest_clock_step(s, 1);
    g_assert_cmphex(qtest_readl(s, 0x6000605c), ==, 732);
    qtest_writel(s, 0x60006058, 0x80000002);
    g_assert_cmphex(qtest_readl(s, 0x60006058), ==, 1);
    g_assert_cmphex(qtest_readl(s, 0x6000605c), ==, 732);

    g_assert_cmphex(qtest_readl(s, 0x60006028), ==, 0x20000000);
    qtest_writel(s, 0x60006028, 0x20004444);
    qtest_writel(s, 0x6000602c, 0x80000000);
    qtest_writel(s, 0x60006030, 0x11);
    g_assert_cmphex(qtest_readl(s, 0x60006028), ==, 0x20004444);
    g_assert_cmphex(qtest_readl(s, 0x6000602c), ==, 0x80000000);
    g_assert_cmphex(qtest_readl(s, 0x60006030), ==, 0x11);
    for (unsigned i = 0; i < ARRAY_SIZE(sources); i++) {
        qtest_writel(s, sources[i], 0x80000000 | i);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(sources); i++) {
        g_assert_cmphex(qtest_readl(s, sources[i]), ==, 0x80000000 | i);
    }

    qtest_writel(s, 0x60006080, 0x08003201);
    g_assert_cmphex(qtest_readl(s, 0x60006080), ==, 0x3201);
    qtest_writel(s, 0x60006080, 0x40103201);
    g_assert_cmphex(qtest_readl(s, 0x60006080), ==, 0x48103201);
    qtest_writel(s, 0x6000608c, 1 << 26);
    g_assert_cmphex(qtest_readl(s, 0x60006080), ==, 0x40103201);
    qtest_writel(s, 0x6000608c, 0);
    qtest_writel(s, 0x60006080, 0x40003200);
    g_assert_cmphex(qtest_readl(s, 0x60006080), ==, 0x40003200);
    for (unsigned i = 0; i < 2; i++) {
        uint64_t base = 0x600064e8 + i * 20;

        qtest_writel(s, base, 0x48003201);
        g_assert_cmphex(qtest_readl(s, base), ==, 0x48003201);
        qtest_writel(s, base + 4, 1 << 30);
        g_assert_cmphex(qtest_readl(s, base), ==, 0x40003201);
    }
    qtest_writel(s, 0x600065a4, 0x48003201);
    g_assert_cmphex(qtest_readl(s, 0x600065a4), ==, 0x48003201);
    qtest_writel(s, 0x600065a4, 0x48083201);
    g_assert_cmphex(qtest_readl(s, 0x600065a4), ==, 0x40083201);
    qtest_writel(s, 0x600064c8, 1 << 24);
    g_assert_cmphex(qtest_readl(s, 0x600064c8), ==, 0);
    qtest_writel(s, 0x600064c4, 0x40003201);
    g_assert_cmphex(qtest_readl(s, 0x600064c8), ==, 1 << 24);
    qtest_writel(s, 0x600064c8, 1 << 16);
    g_assert_cmphex(qtest_readl(s, 0x600064c8), ==, 1 << 16);
    for (unsigned i = 0; i < 3; i++) {
        uint64_t base = 0x600060b0 + i * 16;

        qtest_writel(s, base, 0x48003201);
        g_assert_cmphex(qtest_readl(s, base), ==, 0x48003201);
        qtest_writel(s, base, 0x08003201);
        g_assert_cmphex(qtest_readl(s, base), ==, 0x3201);
        qtest_writel(s, base, 0x48000001);
        g_assert_cmphex(qtest_readl(s, base), ==, 0x40000001);
    }
    qtest_writel(s, 0x600060b4, 0x1c03);
    g_assert_cmphex(qtest_readl(s, 0x600060b4), ==, 0x1c03);
    qtest_writel(s, 0x600060d0, 0x40040001);
    g_assert_cmphex(qtest_readl(s, 0x600060d0), ==, 0x48040001);
    qtest_writel(s, 0x60006484, 0x80000177);
    qtest_writel(s, 0x60006488, 0x40000000);
    g_assert_cmphex(qtest_readl(s, 0x60006484), ==, 0x80000177);
    g_assert_cmphex(qtest_readl(s, 0x60006488), ==, 0x40000000);
    qtest_writel(s, 0x60006058, 0x80000001);
    qtest_system_reset(s);
    qtest_clock_step(s, 100000);
    g_assert_cmphex(qtest_readl(s, 0x60006058), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x6000605c), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x60006028), ==, 0x20000000);
    g_assert_cmphex(qtest_readl(s, 0x6000602c), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x60006030), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x60006080), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x600064e8), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x600064fc), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x600065a4), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x600064c4), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x600064c8), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x60006484), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x60006488), ==, 0);
    for (unsigned i = 0; i < 3; i++) {
        g_assert_cmphex(qtest_readl(s, 0x600060b0 + i * 16),
                       ==, i == 2 ? 0x10c : 0);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(sources); i++) {
        g_assert_cmphex(qtest_readl(s, sources[i]), ==, 0);
    }
    qtest_quit(s);
}

static void test_usb_pll_handoff(void)
{
    QTestState *s = start_mh2p("");

    g_assert_cmphex(qtest_readl(s, 0x6000652c), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x60006530), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x7009f040), ==, 9);
    qtest_writel(s, 0x6000652c, 0x03000043);
    g_assert_cmphex(qtest_readl(s, 0x6000652c), ==, 0x03000043);
    qtest_writel(s, 0x6000652c, 0x04000000);
    g_assert_cmphex(qtest_readl(s, 0x6000652c), ==, 0x03000043);

    qtest_writel(s, 0x600060c0, 0x0003c00c);
    qtest_writel(s, 0x60006530, 0x03000040);
    g_assert_cmphex(qtest_readl(s, 0x600060c0), ==, 0x0803c00c);
    qtest_writel(s, 0x60006530, 0x01000040);
    g_assert_cmphex(qtest_readl(s, 0x600060c0), ==, 0x0803c00c);
    qtest_writel(s, 0x60006530, 0);
    g_assert_cmphex(qtest_readl(s, 0x600060c0), ==, 0x0003c00c);
    qtest_writel(s, 0x60006530, 0x01000040);
    g_assert_cmphex(qtest_readl(s, 0x600060c0), ==, 0x0003c00c);
    qtest_writel(s, 0x600060c0, 0x4103c00c); /* Software override. */
    g_assert_cmphex(qtest_readl(s, 0x600060c0), ==, 0x4903c00c);
    qtest_writel(s, 0x60006484, 1 << 16); /* Force PLLU powerdown. */
    g_assert_cmphex(qtest_readl(s, 0x600060c0), ==, 0x4103c00c);
    qtest_writel(s, 0x60006484, 0);
    qtest_writel(s, 0x600060c0, 0x4003c00c);
    qtest_writel(s, 0x60006530, 0x01000044); /* Software enable control. */
    g_assert_cmphex(qtest_readl(s, 0x600060c0), ==, 0x4803c00c);
    qtest_writel(s, 0x600060c0, 0x4003c000);
    g_assert_cmphex(qtest_readl(s, 0x600060c0), ==, 0x4003c000);

    qtest_writel(s, 0x600060ec, 1 << 11); /* Lock is read-only. */
    g_assert_cmphex(qtest_readl(s, 0x600060ec), ==, 0);
    qtest_writel(s, 0x600060e8, 0x4dc80101);
    qtest_writel(s, 0x600060ec, 0x433c);
    g_assert_cmphex(qtest_readl(s, 0x600060ec), ==, 0x4b3c);
    qtest_writel(s, 0x600060e8, 0x4fc80101); /* Reserved CML divider. */
    g_assert_cmphex(qtest_readl(s, 0x600060ec), ==, 0x433c);
    qtest_writel(s, 0x600060e8, 0x4dc80101);
    qtest_writel(s, 0x600060ec, 0x633c);
    g_assert_cmphex(qtest_readl(s, 0x600060ec), ==, 0x633c);
    qtest_writel(s, 0x600060ec, 0x033c);
    qtest_writel(s, 0x6000648c, 0x13000008); /* Unpowered PLLRE parent. */
    g_assert_cmphex(qtest_readl(s, 0x600060ec), ==, 0x033c);
    g_assert_cmphex(qtest_readl(s, 0x6000648c), ==, 0x1b000008);
    qtest_writel(s, 0x600064c4, 0x40003201);
    g_assert_cmphex(qtest_readl(s, 0x600060ec), ==, 0x0b3c);
    g_assert_cmphex(qtest_readl(s, 0x6000648c), ==, 0x17000008);
    qtest_writel(s, 0x6000648c, 0x07000008); /* Ignore echoed status bits. */
    g_assert_cmphex(qtest_readl(s, 0x6000648c), ==, 0x07000008);

    qtest_writel(s, 0x6000651c, 0x03000040);
    g_assert_cmphex(qtest_readl(s, 0x7009f040), ==, 9);
    qtest_writel(s, 0x7009f040, 0);
    g_assert_cmphex(qtest_readl(s, 0x7009f040), ==, 1 << 19);
    qtest_writel(s, 0x7009f040, 1 << 19);
    g_assert_cmphex(qtest_readl(s, 0x7009f040), ==, 1 << 19);
    qtest_writel(s, 0x600060ec, 0x633c);
    g_assert_cmphex(qtest_readl(s, 0x7009f040), ==, 0);
    qtest_writel(s, 0x600060ec, 0x033c);
    qtest_writel(s, 0x7009f040, 9);
    g_assert_cmphex(qtest_readl(s, 0x7009f040), ==, 9);
    qtest_writel(s, 0x7009f040, 0x1000);
    g_assert_cmphex(qtest_readl(s, 0x7009f040), ==, 9);
    qtest_writel(s, 0x6000651c, 0);
    qtest_writel(s, 0x7009f040, 2);
    g_assert_cmphex(qtest_readl(s, 0x7009f040), ==, 0x80002);

    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, 0x6000652c), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x60006530), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x6000651c), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x6000648c), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x600060e8), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x600060ec), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x600060c0), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x7009f040), ==, 9);
    qtest_quit(s);
}

static void test_runtime_power_memory(void)
{
    static const unsigned partitions[] = {
        2, 3, 4, 6, 7, 8, 17, 18, 19, 20, 21, 22, 23,
    };
    QTestState *s = start_mh2p("");
    uint32_t boot = qtest_readl(s, 0x7000e438);
    uint32_t clamps = qtest_readl(s, 0x7000e42c);

    g_assert_cmphex(boot, ==, 0x0100c001);
    for (unsigned i = 0; i < ARRAY_SIZE(partitions); i++) {
        unsigned id = partitions[i];
        unsigned clamp = id == 3 ? 4 : id == 4 ? 3 : id;
        uint32_t bit = 1U << id, clamp_bit = 1U << clamp;

        qtest_writel(s, 0x7000e430, id);
        g_assert_cmphex(qtest_readl(s, 0x7000e438), ==, boot);
        qtest_writel(s, 0x7000e434, clamp_bit);
        g_assert_cmphex(qtest_readl(s, 0x7000e42c), ==, clamps);
        qtest_writel(s, 0x7000e430, 0x100 | id);
        g_assert_cmphex(qtest_readl(s, 0x7000e430), ==, id);
        g_assert_cmphex(qtest_readl(s, 0x7000e438), ==, boot | bit);
        g_assert_cmphex(qtest_readl(s, 0x7000e42c), ==, clamps);
        qtest_writel(s, 0x7000e434, clamp_bit);
        g_assert_cmphex(qtest_readl(s, 0x7000e434), ==, 0);
        g_assert_cmphex(qtest_readl(s, 0x7000e42c), ==, clamps & ~clamp_bit);
        qtest_writel(s, 0x7000e430, 0x100 | id);
        g_assert_cmphex(qtest_readl(s, 0x7000e438), ==, boot);
        g_assert_cmphex(qtest_readl(s, 0x7000e42c), ==, clamps);
    }
    /* CPU/GPU transitions and reserved partitions remain unsupported. */
    qtest_writel(s, 0x7000e430, 0x100);
    qtest_writel(s, 0x7000e430, 0x101);
    qtest_writel(s, 0x7000e430, 0x11f);
    g_assert_cmphex(qtest_readl(s, 0x7000e438), ==, boot);
    qtest_writel(s, 0x70019004, 0xffffffff);
    g_assert_cmphex(qtest_readl(s, 0x70019004), ==, 0x13f40);
    qtest_writel(s, 0x70019000, 0xffffffff);
    g_assert_cmphex(qtest_readl(s, 0x70019000), ==, 0);
    qtest_writel(s, 0x70019200, 0xffffffff);
    qtest_writel(s, 0x70019970, 0xffffffff);
    g_assert_cmphex(qtest_readl(s, 0x70019200), ==, 0x02ffcfcf);
    g_assert_cmphex(qtest_readl(s, 0x70019204), ==, 0x02ffcfcf);
    g_assert_cmphex(qtest_readl(s, 0x70019970), ==, 7);
    g_assert_cmphex(qtest_readl(s, 0x70019974), ==, 7);
    qtest_writel(s, 0x70019200, 0);
    g_assert_cmphex(qtest_readl(s, 0x70019204), ==, 0);
    qtest_writel(s, 0x7000e430, 0x114);
    qtest_writel(s, 0x7000e434, 1 << 20);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, 0x7000e438), ==, boot);
    g_assert_cmphex(qtest_readl(s, 0x7000e42c), ==, clamps);
    g_assert_cmphex(qtest_readl(s, 0x7000e430), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x70019004), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x70019204), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x70019974), ==, 0);
    qtest_quit(s);
}

static void test_pcie_pad_configuration(void)
{
    QTestState *s = start_mh2p("");
    const uint32_t request = 0x7000e5b8, status = 0x7000e5bc;
    static const struct {
        uint32_t address, reset, configured, invalid;
    } pads[] = {
        { 0x7009f044, 0, 0x70, 0x71 },
        { 0x7009f134, 0, 0x38, 0x39 },
        { 0x7009f01c, 0x07000000, 0, 1 },
        { 0x50060000, 0, 0x20, 0x21 },
    };

    g_assert_cmphex(qtest_readl(s, request), ==, 0);
    g_assert_cmphex(qtest_readl(s, status), ==, 0);
    for (unsigned i = 4; i <= 6; i++) {
        uint32_t bit = 1U << i;

        qtest_writel(s, request, 0x80000000U | bit);
        g_assert_cmphex(qtest_readl(s, request), ==, bit);
        g_assert_cmphex(qtest_readl(s, status), ==, bit);
        qtest_writel(s, request, 0x40000000U | bit);
        g_assert_cmphex(qtest_readl(s, status), ==, 0);
    }
    qtest_writel(s, request, 0x80000070);
    qtest_writel(s, request, 0x40000020);
    g_assert_cmphex(qtest_readl(s, status), ==, 0x50);
    qtest_writel(s, request, 0xc0000070);
    qtest_writel(s, request, 0x40000008);
    qtest_writel(s, request, 0x70);
    qtest_writew(s, request, 0x70);
    qtest_writel(s, status, 0);
    g_assert_cmphex(qtest_readl(s, request), ==, 0x20);
    g_assert_cmphex(qtest_readl(s, status), ==, 0x50);
    for (unsigned i = 0; i < ARRAY_SIZE(pads); i++) {
        g_assert_cmphex(qtest_readl(s, pads[i].address), ==, pads[i].reset);
        qtest_writel(s, pads[i].address, pads[i].configured);
        qtest_writel(s, pads[i].address, pads[i].invalid);
        qtest_writew(s, pads[i].address, 0);
        g_assert_cmphex(qtest_readl(s, pads[i].address), ==, pads[i].configured);
    }
    /* Configuration does not by itself provide a reference clock or link. */
    qtest_writel(s, 0x7009f040, 2);
    g_assert_cmphex(qtest_readl(s, 0x7009f040), ==, 2);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, request), ==, 0);
    g_assert_cmphex(qtest_readl(s, status), ==, 0);
    for (unsigned i = 0; i < ARRAY_SIZE(pads); i++) {
        g_assert_cmphex(qtest_readl(s, pads[i].address), ==, pads[i].reset);
    }
    qtest_quit(s);
}

static void pcie_enable(QTestState *s)
{
    qtest_writel(s, 0x7000e430, 0x103);
    qtest_writel(s, 0x7000e434, 0x10);
    qtest_writel(s, 0x60006330, 0x140);
    qtest_writel(s, 0x60006314, 0x140);
}

static void test_pcie_controller_configuration(void)
{
    QTestState *s = start_mh2p("");
    static const struct {
        uint32_t address, reset, configured, invalid;
    } regs[] = {
        { 0x01000088, 0, 0xf, 0x10 },
        { 0x01001088, 0, 0xf, 0x10 },
        { 0x010000b0, 0, 2, 0x10 },
        { 0x010010b0, 0, 2, 0x10 },
        { 0x01000494, 0, 0x3fe, 1 },
        { 0x01001494, 0, 0x3fe, 1 },
        { 0x01000e00, 0, 0xe00, 1 },
        { 0x01001e00, 0, 0xe00, 1 },
        { 0x01000f00, 0, 0x1bfc0000, 1 },
        { 0x01001f00, 0, 0x1bfc0000, 1 },
        { 0x01000f04, 0, 0x200000, 1 },
        { 0x01001f04, 0, 0x200000, 1 },
        { 0x01000f48, 0, 0x2000, 1 },
        { 0x01001f48, 0, 0x2000, 1 },
        { 0x01000f4c, 0, 0x10000000, 1 },
        { 0x01001f4c, 0, 0x10000000, 1 },
        { 0x01000fd4, 0, 0x10000000, 1 },
        { 0x01001fd4, 0, 0x10000000, 1 },
        { 0x01000fd8, 0, 0x10000d00, 1 },
        { 0x01001fd8, 0, 0x10000d00, 1 },
        { 0x01000fe0, 0xf, 0xe, 0x10 },
        { 0x01001fe0, 0xf, 0xe, 0x10 },
        { 0x010030d0, 0, 0x30, 1 },
        { 0x010038ac, 0, 1, 2 },
        { 0x010038b0, 0, 0, 1 },
        { 0x010038b4, 0, 0x101, 2 },
        { 0x010038c8, 0, 0x17f, 0x200 },
        { 0x010038f8, 6, 0x100000, 0x200000 },
        { 0x01003904, 4, 0, 1 },
        { 0x01003910, 0, 0x1b, 4 },
        { 0x01003918, 0, 0x1b, 4 },
        { 0x01003960, 0x200, 2, 1 },
        { 0x01003968, 0, 0, 1 },
    };

    qtest_writel(s, 0x01003910, 0x1b); /* Power/clock prerequisites absent. */
    qtest_writel(s, 0x7000e430, 0x103);
    qtest_writel(s, 0x60006330, 0x140);
    qtest_writel(s, 0x01003910, 0x1b); /* Powered but still clamped. */
    qtest_writel(s, 0x7000e434, 0x10);
    for (unsigned i = 0; i < ARRAY_SIZE(regs); i++) {
        g_assert_cmphex(qtest_readl(s, regs[i].address), ==, regs[i].reset);
        qtest_writel(s, regs[i].address, regs[i].configured);
        qtest_writel(s, regs[i].address, regs[i].invalid);
        qtest_writew(s, regs[i].address, 0);
        g_assert_cmphex(qtest_readl(s, regs[i].address), ==, regs[i].configured);
    }
    qtest_writel(s, 0x010030c8, 0xd7acd8ac);
    g_assert_cmphex(qtest_readl(s, 0x010030c8), ==, 0xd7acd8ac);
    qtest_writel(s, 0x010038c4, 0xffffffff);
    g_assert_cmphex(qtest_readl(s, 0x010038c4), ==, 0xff);
    for (unsigned i = 0; i < 8; i++) {
        qtest_writel(s, 0x0100388c + 4 * i, 1U << i);
        g_assert_cmphex(qtest_readl(s, 0x0100388c + 4 * i), ==, 1U << i);
    }
    qtest_writel(s, 0x60006334, 0x100); /* Clock gating preserves state. */
    qtest_writel(s, 0x01003910, 0);
    g_assert_cmphex(qtest_readl(s, 0x01003910), ==, 0);
    qtest_writel(s, 0x60006330, 0x100);
    g_assert_cmphex(qtest_readl(s, 0x01003910), ==, 0x1b);
    qtest_writel(s, 0x60006310, 0x40); /* PEX reset, not AFI reset. */
    qtest_writel(s, 0x01001fe0, 0xe);
    g_assert_cmphex(qtest_readl(s, 0x01001fe0), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x01003910), ==, 0x1b);
    qtest_writel(s, 0x60006314, 0x40);
    g_assert_cmphex(qtest_readl(s, 0x01001fe0), ==, 0xf);
    g_assert_cmphex(qtest_readl(s, 0x01003910), ==, 0x1b);
    qtest_writel(s, 0x60006310, 0x100);
    qtest_writel(s, 0x60006314, 0x100);
    g_assert_cmphex(qtest_readl(s, 0x01003910), ==, 0);
    qtest_writel(s, 0x01003910, 0x1b);
    qtest_writel(s, 0x7000e430, 0x103); /* Power loss clears configuration. */
    pcie_enable(s);
    g_assert_cmphex(qtest_readl(s, 0x01003910), ==, 0);
    qtest_writel(s, 0x01003910, 0x1b);
    qtest_system_reset(s);
    pcie_enable(s);
    for (unsigned i = 0; i < ARRAY_SIZE(regs); i++) {
        g_assert_cmphex(qtest_readl(s, regs[i].address), ==, regs[i].reset);
    }
    qtest_quit(s);
}

static void test_pcie_no_endpoint(void)
{
    QTestState *s = start_mh2p("");

    pcie_enable(s);
    qtest_writel(s, 0x01003910, 0x1b);
    qtest_writel(s, 0x01003918, 0x1b);
    qtest_writel(s, 0x010038ac, 1);
    for (unsigned port = 0; port < 2; port++) {
        uint32_t base = 0x01000000 + port * 0x1000;

        qtest_writel(s, base + 0xfe0, 0xe); /* Presence override is not link-up. */
        qtest_writel(s, base + 0xf00, 0x40000000);
        qtest_writel(s, base + 0x90, 0x20000000);
        qtest_clock_step(s, 1000000000);
        g_assert_cmphex(qtest_readl(s, base + 0xf00), ==, 0);
        g_assert_cmphex(qtest_readl(s, base + 0x90), ==, 0);
    }
    qtest_quit(s);
}

static void test_pcie_root_discovery(void)
{
    QTestState *s = start_mh2p("");
    static const struct {
        uint32_t offset, value;
    } readonly[] = {
        { 0x08, 0x060400a1 }, { 0x0c, 0x10000 },
        { 0x10, 0 }, { 0x14, 0 }, { 0x34, 0x80 }, { 0x38, 0 },
        { 0x80, 0x00420010 }, { 0x84, 0 }, { 0x94, 0 }, { 0x98, 0 },
        { 0xa0, 0 }, { 0xa4, 0 }, { 0xa8, 0 }, { 0xac, 6 }, { 0x100, 0 },
    };

    pcie_enable(s);
    for (unsigned port = 0; port < 2; port++) {
        uint32_t base = 0x01000000 + port * 0x1000;
        uint32_t identity = port ? 0x0e1310de : 0x0e1210de;

        qtest_writel(s, base, 0);
        qtest_writel(s, base, 0xffffffff);
        g_assert_cmphex(qtest_readl(s, base), ==, identity);
        for (unsigned i = 0; i < ARRAY_SIZE(readonly); i++) {
            qtest_writel(s, base + readonly[i].offset, 0);
            qtest_writel(s, base + readonly[i].offset, 0xffffffff);
            g_assert_cmphex(qtest_readl(s, base + readonly[i].offset),
                            ==, readonly[i].value);
        }
        qtest_writel(s, base + 0x8c, 0);
        qtest_writel(s, base + 0x8c, 0xffffffff);
        g_assert_cmphex(qtest_readl(s, base + 0x8c), ==, port ? 0x12 : 0x42);
        qtest_writel(s, base + 4, 0xf9100007);
        g_assert_cmphex(qtest_readl(s, base + 4), ==, 0x00100007);
        qtest_writel(s, base + 4, 0);
        g_assert_cmphex(qtest_readl(s, base + 4), ==, 0x00100000);
        qtest_writel(s, base + 0x18, port ? 0x030302 : 0x010100);
        qtest_writel(s, base + 0x20, 0x20001000);
        qtest_writel(s, base + 0x24, 0x30012001);
        g_assert_cmphex(qtest_readl(s, base + 0x20), ==, 0x20001000);
        g_assert_cmphex(qtest_readl(s, base + 0x24), ==, 0x30012001);
        qtest_writel(s, base + 0xfa8, 0x800);
        qtest_writel(s, base + 0xfa8, 0);
        g_assert_cmphex(qtest_readl(s, base + 0xfa8), ==, 0);
        g_assert_cmphex(qtest_readl(s, base + 0x90), ==, 0);
        g_assert_cmphex(qtest_readl(s, base + 0xf00), ==, 0);
    }
    for (unsigned bank = 0; bank < 8; bank++) {
        qtest_writel(s, 0x0100386c + bank * 4, 0xffffffff);
        g_assert_cmphex(qtest_readl(s, 0x0100386c + bank * 4), ==, 0);
    }
    qtest_writel(s, 0x010038b8, 0);
    g_assert_cmphex(qtest_readl(s, 0x010038b8), ==, 0);
    qtest_writel(s, 0x60006310, 0x40);
    qtest_writel(s, 0x60006314, 0x40);
    for (unsigned port = 0; port < 2; port++) {
        uint32_t base = 0x01000000 + port * 0x1000;

        g_assert_cmphex(qtest_readl(s, base + 4), ==, 0x00100000);
        g_assert_cmphex(qtest_readl(s, base + 0x18), ==, 0);
        g_assert_cmphex(qtest_readl(s, base + 0x20), ==, 0);
        g_assert_cmphex(qtest_readl(s, base + 0x24), ==, 0x10001);
    }
    qtest_quit(s);
}

static void test_pcie_window_configuration(void)
{
    QTestState *s = start_mh2p("");
    static const struct {
        uint32_t size, axi, fpci;
    } windows[] = {
        { 0x3800, 0x3818, 0x3830 }, { 0x3804, 0x381c, 0x3834 },
        { 0x3808, 0x3820, 0x3838 }, { 0x380c, 0x3824, 0x383c },
        { 0x3810, 0x3828, 0x3840 }, { 0x3814, 0x382c, 0x3844 },
        { 0x3934, 0x3940, 0x394c }, { 0x3938, 0x3944, 0x3950 },
        { 0x393c, 0x3948, 0x3954 }, { 0x3860, 0x3868, 0x3864 },
    };

    pcie_enable(s);
    for (unsigned i = 0; i < ARRAY_SIZE(windows); i++) {
        uint32_t size = 0x01000000 + windows[i].size;
        uint32_t axi = 0x01000000 + windows[i].axi;
        uint32_t fpci = 0x01000000 + windows[i].fpci;

        g_assert_cmphex(qtest_readl(s, size), ==, 0);
        g_assert_cmphex(qtest_readl(s, axi), ==, 0);
        g_assert_cmphex(qtest_readl(s, fpci), ==, 0);
        qtest_writel(s, size, 0x100);
        qtest_writel(s, axi, 0x20000000);
        qtest_writel(s, fpci, 0xfe100000);
        qtest_writel(s, size, 0x100000);
        qtest_writel(s, axi, 0x20000001);
        qtest_writel(s, fpci, 0xfe100002);
        qtest_writew(s, size, 0);
        g_assert_cmphex(qtest_readl(s, size), ==, 0x100);
        g_assert_cmphex(qtest_readl(s, axi), ==, 0x20000000);
        g_assert_cmphex(qtest_readl(s, fpci), ==, 0xfe100000);
    }
    qtest_writel(s, 0x60006334, 0x100);
    qtest_writel(s, 0x01003800, 0);
    g_assert_cmphex(qtest_readl(s, 0x01003800), ==, 0);
    qtest_writel(s, 0x60006330, 0x100);
    g_assert_cmphex(qtest_readl(s, 0x01003800), ==, 0x100);
    qtest_writel(s, 0x60006310, 0x40);
    qtest_writel(s, 0x60006314, 0x40);
    g_assert_cmphex(qtest_readl(s, 0x01003800), ==, 0x100);
    qtest_system_reset(s);
    pcie_enable(s);
    for (unsigned i = 0; i < ARRAY_SIZE(windows); i++) {
        g_assert_cmphex(qtest_readl(s, 0x01000000 + windows[i].size), ==, 0);
        g_assert_cmphex(qtest_readl(s, 0x01000000 + windows[i].axi), ==, 0);
        g_assert_cmphex(qtest_readl(s, 0x01000000 + windows[i].fpci), ==, 0);
    }
    qtest_quit(s);
}

static void pcie_config_window(QTestState *s, uint32_t size_reg,
                                uint32_t axi_reg, uint32_t fpci_reg,
                                uint32_t axi, uint32_t fpci, uint32_t pages)
{
    qtest_writel(s, 0x01000000 + size_reg, 0);
    qtest_writel(s, 0x01000000 + axi_reg, axi);
    qtest_writel(s, 0x01000000 + fpci_reg, fpci);
    qtest_writel(s, 0x01000000 + size_reg, pages);
}

static void test_pcie_config_aperture(void)
{
    QTestState *s = start_mh2p("");
    const uint32_t base = 0x02000000;
    static const struct {
        uint32_t size, axi, fpci;
    } windows[] = {
        { 0x3800, 0x3818, 0x3830 }, { 0x3804, 0x381c, 0x3834 },
        { 0x3808, 0x3820, 0x3838 }, { 0x380c, 0x3824, 0x383c },
        { 0x3810, 0x3828, 0x3840 }, { 0x3814, 0x382c, 0x3844 },
        { 0x3934, 0x3940, 0x394c }, { 0x3938, 0x3944, 0x3950 },
        { 0x393c, 0x3948, 0x3954 },
    };

    pcie_enable(s);
    g_assert_cmphex(qtest_readl(s, base), ==, 0); /* No fallback aperture. */
    pcie_config_window(s, 0x3800, 0x3818, 0x3830, base, 0xfe100000, 1);
    g_assert_cmphex(qtest_readl(s, base), ==, 0); /* FPCI disabled. */
    qtest_writel(s, 0x010038ac, 1);
    for (unsigned i = 0; i < ARRAY_SIZE(windows); i++) {
        pcie_config_window(s, windows[i].size, windows[i].axi,
                           windows[i].fpci, base, 0xfe100000, 1);
        g_assert_cmphex(qtest_readb(s, base), ==, 0xff);
        g_assert_cmphex(qtest_readw(s, base + 2), ==, 0xffff);
        g_assert_cmphex(qtest_readl(s, base + 4), ==, 0xffffffff);
        g_assert_cmphex(qtest_readl(s, base + 0xffc), ==, 0xffffffff);
        g_assert_cmphex(qtest_readl(s, base + 0x1000), ==, 0);
        qtest_writeb(s, base, 0x10);
        qtest_writew(s, base + 2, 0x104c);
        qtest_writel(s, base + 4, 0xffffffff);
        g_assert_cmphex(qtest_readl(s, base), ==, 0xffffffff);
        qtest_writel(s, 0x01000000 + windows[i].size, 0);
        g_assert_cmphex(qtest_readl(s, base), ==, 0);
    }
    pcie_config_window(s, 0x3800, 0x3818, 0x3830, base, 0xfe100000, 1);
    pcie_config_window(s, 0x3804, 0x381c, 0x3834, base, 0xfe000000, 1);
    g_assert_cmphex(qtest_readl(s, base), ==, 0); /* Ambiguous overlap. */
    qtest_writel(s, 0x01003804, 0);
    g_assert_cmphex(qtest_readl(s, base), ==, 0xffffffff);
    for (unsigned i = 0; i < 5; i++) {
        static const uint32_t invalid[] = {
            0xfdfc0000, 0xfe200000, 0xfd000000, 0x00000001, 0xfe100001,
        };

        qtest_writel(s, 0x01003830, invalid[i]);
        g_assert_cmphex(qtest_readl(s, base), ==, 0);
    }
    /* MSI's inbound window must never masquerade as an outbound window. */
    qtest_writel(s, 0x01003800, 0);
    pcie_config_window(s, 0x3860, 0x3868, 0x3864, base, 0xfe100000, 1);
    g_assert_cmphex(qtest_readl(s, base), ==, 0);
    pcie_config_window(s, 0x3800, 0x3818, 0x3830, base, 0xfe100000, 1);
    qtest_writel(s, 0x60006334, 0x100);
    g_assert_cmphex(qtest_readl(s, base), ==, 0);
    qtest_writel(s, 0x60006330, 0x100);
    g_assert_cmphex(qtest_readl(s, base), ==, 0xffffffff);
    qtest_writel(s, 0x60006310, 0x40);
    g_assert_cmphex(qtest_readl(s, base), ==, 0);
    qtest_writel(s, 0x60006314, 0x40);
    g_assert_cmphex(qtest_readl(s, base), ==, 0xffffffff);
    qtest_writel(s, 0x60006310, 0x100);
    qtest_writel(s, 0x60006314, 0x100);
    g_assert_cmphex(qtest_readl(s, base), ==, 0);
    qtest_writel(s, 0x010038ac, 1);
    pcie_config_window(s, 0x3800, 0x3818, 0x3830, base, 0xfe100000, 1);
    qtest_system_reset(s);
    pcie_enable(s);
    qtest_writel(s, 0x010038ac, 1);
    g_assert_cmphex(qtest_readl(s, base), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x01000000), ==, 0x0e1210de);
    g_assert_cmphex(qtest_readl(s, 0x01001000), ==, 0x0e1310de);
    g_assert_cmphex(qtest_readl(s, 0x01000f00), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x01001f00), ==, 0);
    qtest_quit(s);
}

static void test_pcie_config_translation(void)
{
#ifdef CONFIG_TRACE_LOG
    g_autofree char *path = NULL;
    g_autofree char *options = NULL;
    g_autofree char *log = NULL;
    g_autoptr(GError) error = NULL;
    static const struct {
        uint32_t target, offset;
        unsigned type, bus, devfn, reg;
    } cases[] = {
        { 0xfdfe0000, 0x00031d08, 0, 3, 0x1d, 8 },
        { 0xfdff0000, 0x00ffa5fc, 1, 255, 0xa5, 0xfc },
        { 0xfe000000, 0x0a031dfc, 0, 3, 0x1d, 0xafc },
        { 0xfe100000, 0x0fffffff, 1, 255, 0xff, 0xfff },
    };
    int fd = g_file_open_tmp("mh2p-pcie-trace-XXXXXX", &path, &error);
    QTestState *s;

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    options = g_strdup_printf("-trace enable=mh2p_pcie_config,file=%s", path);
    s = start_mh2p(options);
    pcie_enable(s);
    qtest_writel(s, 0x010038ac, 1);
    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        uint32_t address = 0x02000000 + (cases[i].offset & 0xfff);
        uint32_t target = cases[i].target + ((cases[i].offset & ~0xfff) >> 8);

        pcie_config_window(s, 0x3800, 0x3818, 0x3830,
                           0x02000000, target, 1);
        g_assert_cmphex(qtest_readb(s, address), ==, 0xff);
        qtest_writeb(s, address, 0xaa);
    }
    /* Maximum AXI address must not wrap into IRAM or local root registers. */
    pcie_config_window(s, 0x393c, 0x3948, 0x3954,
                       0x3ffff000, 0xfe1ffff0, 1);
    g_assert_cmphex(qtest_readl(s, 0x3ffffffc), ==, 0xffffffff);
    /* A target at the top of 40-bit FPCI must not wrap into config space. */
    pcie_config_window(s, 0x3800, 0x3818, 0x3830,
                       0x02000000, 0xfffffff0, 2);
    g_assert_cmphex(qtest_readl(s, 0x02001000), ==, 0);
    qtest_quit(s);
    g_assert_true(g_file_get_contents(path, &log, NULL, &error));
    g_assert_no_error(error);
    unlink(path);
    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        for (unsigned write = 0; write < 2; write++) {
            g_autofree char *expected = g_strdup_printf(
                "mh2p_pcie_config axi=0x%x fpci=0x%" PRIx64 " window=0 "
                "type=%u bus=%u devfn=0x%x reg=0x%x size=1 write=%u",
                0x02000000 + (cases[i].offset & 0xfff),
                ((uint64_t)cases[i].target << 8) + cases[i].offset,
                cases[i].type, cases[i].bus, cases[i].devfn, cases[i].reg,
                write);

            g_assert_nonnull(strstr(log, expected));
        }
    }
    g_assert_nonnull(strstr(log, "axi=0x3ffffffc fpci=0xfe1ffffffc window=8 "
                           "type=1 bus=255 devfn=0xff reg=0xffc size=4 "
                           "write=0"));
    g_assert_null(strstr(log, "axi=0x2001000"));
#else
    g_test_skip("Exact configuration translation trace requires log backend");
#endif
}

static void test_avp_registers(void)
{
    QTestState *s = start_mh2p("");

    /* devm-nvavp's COP bring-up sequence, in the original order. */
    qtest_writel(s, 0x60001020, 0);
    qtest_writel(s, 0x60007004, 0x40000000);
    qtest_writel(s, 0x6000f200, 0x80024000);
    g_assert_cmphex(qtest_readl(s, 0x60001020), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x60007004), ==, 0x40000000);
    g_assert_cmphex(qtest_readl(s, 0x6000f200), ==, 0x80024000);

    qtest_writel(s, 0x60001010, 0x12345678);
    g_assert_cmphex(qtest_readl(s, 0x60001010), ==, 0x12345678);
    qtest_writel(s, 0x60001004, 0x81);
    qtest_writel(s, 0x60001008, 0x01);
    g_assert_cmphex(qtest_readl(s, 0x60001000), ==, 0x80);

    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, 0x60001000), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x60001010), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x60007004), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x6000f200), ==, 0);
    qtest_quit(s);
}

static void test_gpio(void)
{
    static const unsigned irqs[] = { 32, 33, 34, 35, 55, 87, 89, 125 };
    QTestState *s = start_mh2p("");

    qtest_irq_intercept_out(s, "/machine/gpio");
    for (unsigned bank = 0; bank < ARRAY_SIZE(irqs); bank++) {
        uint64_t base = 0x6000d000 + bank * 0x100;
        unsigned irq = irqs[bank] + 32;
        uint32_t mask = 1U << (irq % 32);
        uint64_t pending = 0x50041200 + (irq / 32) * 4;

        qtest_writel(s, base + 0x80, 0x0301);
        qtest_writel(s, base + 0x80, 0x0202);
        g_assert_cmphex(qtest_readl(s, base), ==, 3);
        qtest_writel(s, base + 0x10, 1);
        qtest_writel(s, base + 0xa0, 0x0101);
        g_assert_cmphex(qtest_readl(s, base + 0x30), ==, 1);
        g_assert_true(qtest_get_irq(s, bank * 32));

        /* Pin 1 is an external rising-edge input; pin 0 stays driven high. */
        qtest_writel(s, base + 0x60, 0x202);
        qtest_writel(s, base + 0x50, 2);
        qtest_writel(s, 0x50041100 + (irq / 32) * 4, mask);
        qtest_set_irq_in(s, "/machine/gpio", NULL, bank * 32 + 1, 1);
        g_assert_cmphex(qtest_readl(s, base + 0x30), ==, 3);
        g_assert_cmphex(qtest_readl(s, base + 0x40), ==, 2);
        g_assert_cmphex(qtest_readl(s, pending) & mask, ==, mask);
        qtest_writel(s, base + 0x70, 2);
        g_assert_cmphex(qtest_readl(s, base + 0x40), ==, 0);
        g_assert_cmphex(qtest_readl(s, pending) & mask, ==, 0);
    }
    qtest_system_reset(s);
    for (unsigned bank = 0; bank < ARRAY_SIZE(irqs); bank++) {
        uint64_t base = 0x6000d000 + bank * 0x100;

        g_assert_cmphex(qtest_readl(s, base), ==, 0);
        g_assert_cmphex(qtest_readl(s, base + 0x10), ==, 0);
        g_assert_cmphex(qtest_readl(s, base + 0x20), ==, 0);
        g_assert_cmphex(qtest_readl(s, base + 0x40), ==, 0);
        g_assert_cmphex(qtest_readl(s, base + 0x50), ==, 0);
        g_assert_cmphex(qtest_readl(s, base + 0x60), ==, 0);
        g_assert_cmphex(qtest_readl(s, base + 0x30), ==, 2);
        g_assert_false(qtest_get_irq(s, bank * 32));
    }
    qtest_quit(s);
}

static void test_usb_overcurrent_input(void)
{
    QTestState *s = start_mh2p("");
    const uint64_t port = 0x6000d504; /* V: bank 5, port 1. */
    const uint32_t irq_mask = 1U << ((87 + 32) % 32);
    const uint64_t irq_pending = 0x50041200 + ((87 + 32) / 32) * 4;

    g_assert_cmphex(qtest_readl(s, port + 0x30), ==, 2);
    g_assert_cmphex(qtest_readl(s, port), ==, 0);
    g_assert_cmphex(qtest_readl(s, port + 0x10), ==, 0);

    /* A falling edge on active-low V1 still reports a real connector fault. */
    qtest_writel(s, port + 0x60, 0x200);
    qtest_writel(s, port + 0x50, 2);
    qtest_writel(s, 0x50041100 + ((87 + 32) / 32) * 4, irq_mask);
    qtest_set_irq_in(s, "/machine/gpio", NULL, 21 * 8 + 1, 0);
    g_assert_cmphex(qtest_readl(s, port + 0x30), ==, 0);
    g_assert_cmphex(qtest_readl(s, port + 0x40), ==, 2);
    g_assert_cmphex(qtest_readl(s, irq_pending) & irq_mask, ==, irq_mask);
    qtest_writel(s, port + 0x70, 2);
    g_assert_cmphex(qtest_readl(s, irq_pending) & irq_mask, ==, 0);

    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, port + 0x30), ==, 0);
    qtest_set_irq_in(s, "/machine/gpio", NULL, 21 * 8 + 1, 1);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, port + 0x30), ==, 2);
    g_assert_cmphex(qtest_readl(s, port + 0x50), ==, 0);
    g_assert_cmphex(qtest_readl(s, irq_pending) & irq_mask, ==, 0);
    qtest_quit(s);
}

static void test_nor_dma(void)
{
    g_autofree char *image = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree char *args = NULL;
    uint8_t expected[65536], actual[65536], word[2];
    const unsigned lengths[] = { 4, 512, sizeof(expected) };
    int fd = g_file_open_tmp("mh2p-nor-XXXXXX", &image, &error);
    QTestState *s;

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 64 * 1024 * 1024), ==, 0);
    for (unsigned i = 0; i < sizeof(expected); i++) {
        expected[i] = i * 13 + 7;
    }
    g_assert_cmpint(pwrite(fd, expected, sizeof(expected), 0x2000), ==,
                    sizeof(expected));
    args = g_strdup_printf("-bios %s", image);
    s = start_mh2p(args);
    qtest_writew(s, 0x480000aa, 0x98);
    g_assert_cmphex(qtest_readw(s, 0x48000020), ==, 'Q');
    g_assert_cmphex(qtest_readw(s, 0x48000022), ==, 'R');
    g_assert_cmphex(qtest_readw(s, 0x48000024), ==, 'Y');
    qtest_writew(s, 0x48000000, 0xf0);
    qtest_writel(s, 0x50041110, 1); /* SPI 96 = interrupt 128 */

    for (unsigned i = 0; i < ARRAY_SIZE(lengths); i++) {
        unsigned len = lengths[i];
        uint32_t config = 0x90000000 | ((len / 4 - 1) << 2);

        qtest_memset(s, 0x81000000, 0xaa, len + 4);
        qtest_writel(s, 0x70009008, 0x48002000);
        qtest_writel(s, 0x7000900c, 0x81000000);
        qtest_writel(s, 0x70009020, config);
        qtest_writel(s, 0x70009000, 0x80000000);
        qtest_memread(s, 0x81000000, actual, len);
        g_assert_cmpmem(actual, len, expected, len);
        g_assert_cmphex(qtest_readl(s, 0x81000000 + len), ==, 0xaaaaaaaa);
        g_assert_cmphex(qtest_readl(s, 0x70009008), ==, 0x48002000 + len);
        g_assert_cmphex(qtest_readl(s, 0x7000900c), ==, 0x81000000 + len);
        g_assert_cmphex(qtest_readl(s, 0x70009000) >> 31, ==, 0);
        g_assert_cmphex(qtest_readl(s, 0x70009020) & 0xc8000000, ==,
                        0x08000000);
        g_assert_cmphex(qtest_readl(s, 0x50041210) & 1, ==, 1);
        qtest_writel(s, 0x70009020, 0); /* Mask a pending completion. */
        g_assert_cmphex(qtest_readl(s, 0x50041210) & 1, ==, 0);
        qtest_writel(s, 0x70009020, 0x10000000);
        g_assert_cmphex(qtest_readl(s, 0x50041210) & 1, ==, 1);
        qtest_writel(s, 0x70009020, 0x18000000); /* W1C */
        g_assert_cmphex(qtest_readl(s, 0x50041210) & 1, ==, 0);
        g_assert_cmphex(qtest_readl(s, 0x70009020), ==, 0x10000000);
    }

    /* Failed bus transactions must not copy garbage or report completion. */
    for (unsigned destination_fault = 0; destination_fault < 2;
         destination_fault++) {
        qtest_writel(s, 0x81000000, 0xdeadbeef);
        qtest_writel(s, 0x70009008,
                     destination_fault ? 0x48002000 : 0x7000a000);
        qtest_writel(s, 0x7000900c,
                     destination_fault ? 0x7000a000 : 0x81000000);
        qtest_writel(s, 0x70009020, 0x90000000);
        qtest_writel(s, 0x70009000, 0x80000000);
        g_assert_cmphex(qtest_readl(s, 0x81000000), ==, 0xdeadbeef);
        g_assert_cmphex(qtest_readl(s, 0x70009020) & 0xc8000000, ==, 0);
        g_assert_cmphex(qtest_readl(s, 0x70009004) & 0x40000000, ==, 0);
        g_assert_cmphex(qtest_readl(s, 0x50041210) & 1, ==, 0);
    }

    /* S29GL512S autoselect and CFI identity used by devf-generic-tegra2. */
    qtest_writew(s, 0x48000aaa, 0xaa);
    qtest_writew(s, 0x48000554, 0x55);
    qtest_writew(s, 0x48000aaa, 0x90);
    g_assert_cmphex(qtest_readw(s, 0x48000000), ==, 0x0001);
    g_assert_cmphex(qtest_readw(s, 0x48000002), ==, 0x227e);
    g_assert_cmphex(qtest_readw(s, 0x4800001c), ==, 0x2223);
    g_assert_cmphex(qtest_readw(s, 0x4800001e), ==, 0x2201);
    qtest_writew(s, 0x480000aa, 0x98);
    g_assert_cmphex(qtest_readw(s, 0x48000054), ==, 0x09);
    g_assert_cmphex(qtest_readw(s, 0x4800004e), ==, 0x1a);
    g_assert_cmphex(qtest_readw(s, 0x48000086), ==, '1');
    g_assert_cmphex(qtest_readw(s, 0x48000088), ==, '5');
    g_assert_cmphex(qtest_readw(s, 0x4800008a), ==, 0x1c);
    qtest_writew(s, 0x48000000, 0xf0);
    qtest_writew(s, 0x48000000, 0xf0);
    g_assert_cmphex(qtest_readw(s, 0x48002000), ==,
                    expected[0] | expected[1] << 8);

    /* Write-to-buffer: 0xF0 data must not reset; 0x29 programs. */
    qtest_writew(s, 0x48000aaa, 0xaa);
    qtest_writew(s, 0x48000554, 0x55);
    qtest_writew(s, 0x48002000, 0x25);
    qtest_writew(s, 0x48002000, 1);
    qtest_writew(s, 0x48002004, 0x00f0);
    qtest_writew(s, 0x48002006, 0x0012);
    qtest_writew(s, 0x48002000, 0x29);
    g_assert_cmphex(qtest_readw(s, 0x48002004), ==,
                    0x00f0 & (expected[4] | expected[5] << 8));
    g_assert_cmphex(qtest_readw(s, 0x48002006), ==,
                    0x0012 & (expected[6] | expected[7] << 8));
    g_assert_cmphex(qtest_readw(s, 0x48002002), ==,
                    expected[2] | expected[3] << 8);

    /* Eclipse status register read reports ready, then returns to array. */
    qtest_writew(s, 0x48000aaa, 0x70);
    g_assert_cmphex(qtest_readw(s, 0x48002000), ==, 0x80);
    g_assert_cmphex(qtest_readw(s, 0x48002000), ==,
                    expected[0] | expected[1] << 8);

    qtest_writew(s, 0x48000aaa, 0xaa);
    qtest_writew(s, 0x48000554, 0x55);
    qtest_writew(s, 0x48000aaa, 0xa0);
    qtest_writew(s, 0x48002000, 0);
    g_assert_cmphex(qtest_readw(s, 0x48002000), ==, 0);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, 0x70009020), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x70009008), ==, 0);
    g_assert_cmphex(qtest_readw(s, 0x48002000), ==, 0);
    qtest_quit(s);
    g_assert_cmpint(pread(fd, word, sizeof(word), 0x2000), ==, sizeof(word));
    g_assert_cmpmem(word, sizeof(word), expected, sizeof(word));
    close(fd);
    unlink(image);
}

static void sd_command(QTestState *s, uint64_t base, uint16_t command,
                       uint32_t argument)
{
    qtest_writew(s, base + 0x30, 0xffff);
    qtest_writel(s, base + 8, argument);
    qtest_writew(s, base + 0xe, command);
    qtest_clock_step(s, 1000000);
    g_assert_cmphex(qtest_readw(s, base + 0x30) & 1, ==, 1);
    g_assert_cmphex(qtest_readw(s, base + 0x32), ==, 0);
}

static void sd_setup_host(QTestState *s, uint64_t base)
{
    qtest_writeb(s, base + 0x29, 0x0f);
    qtest_writew(s, base + 0x2c, 7);
    qtest_writel(s, base + 0x34, 0xffffffff);
    qtest_writel(s, base + 0x38, 0xffffffff);
}

static void test_mnand_handoff(void)
{
    g_autofree char *image = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree char *args = NULL;
    const uint64_t base = 0x700b0600;
    int fd = g_file_open_tmp("mh2p-handoff-XXXXXX", &image, &error);
    QTestState *s;

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 4ULL * 1024 * 1024 * 1024), ==, 0);
    args = g_strdup_printf("-drive if=sd,index=0,format=raw,snapshot=on,"
                           "file=%s", image);
    s = start_mh2p(args);
    for (unsigned boot = 0; boot < 2; boot++) {
        sd_setup_host(s, base);
        sd_command(s, base, 0x0102, 0x40000000);
        g_assert_cmphex(qtest_readl(s, base + 0x10) & 0x80000000, ==, 0);
        qtest_clock_step(s, 98000000);
        sd_command(s, base, 0x0102, 0x40000000);
        g_assert_cmphex(qtest_readl(s, base + 0x10) & 0x80000000, ==, 0);
        sd_command(s, base, 0x0102, 0x40000000);
        g_assert_cmphex(qtest_readl(s, base + 0x10) & 0xc0000000, ==,
                        0xc0000000);
        sd_command(s, base, 0x0209, 0);
        if (!boot) {
            qtest_system_reset(s);
        }
    }

    /* CMD0 discards the handoff: an enquiry alone must not enable CMD2. */
    sd_command(s, base, 0, 0);
    sd_command(s, base, 0x0102, 0x40000000);
    qtest_clock_step(s, 100000000);
    sd_command(s, base, 0x0102, 0x40000000);
    g_assert_cmphex(qtest_readl(s, base + 0x10) & 0x80000000, ==,
                    0x80000000);
    qtest_writel(s, base + 8, 0);
    qtest_writew(s, base + 0xe, 0x0209);
    qtest_clock_step(s, 1000000);
    g_assert_cmphex(qtest_readw(s, base + 0x32) & 1, ==, 1);
    qtest_writew(s, base + 0x32, 0xffff);

    /* A different voltage/access-mode request cannot replace the first. */
    sd_command(s, base, 0, 0);
    sd_command(s, base, 0x0102, 0x40ff8000);
    g_assert_cmphex(qtest_readl(s, base + 0x10) & 0x80000000, ==, 0);
    qtest_clock_step(s, 100000000);
    sd_command(s, base, 0x0102, 0x80);
    g_assert_cmphex(qtest_readl(s, base + 0x10) & 0xc0000000, ==,
                    0xc0000000);
    sd_command(s, base, 0x0209, 0);

    /* Reset during power-up must cancel the old deadline as well. */
    sd_command(s, base, 0, 0);
    sd_command(s, base, 0x0102, 0x40ff8000);
    qtest_clock_step(s, 50000000);
    sd_command(s, base, 0, 0);
    qtest_clock_step(s, 50000000);
    sd_command(s, base, 0x0102, 0x40ff8000);
    g_assert_cmphex(qtest_readl(s, base + 0x10) & 0x80000000, ==, 0);
    qtest_clock_step(s, 100000000);
    sd_command(s, base, 0x0102, 0x40000000);
    sd_command(s, base, 0x0209, 0);
    qtest_quit(s);
    close(fd);
    unlink(image);
}

static void test_sdmmc(void)
{
    g_autofree char *image = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree char *args = NULL;
    const uint64_t bases[] = { 0x700b0600, 0x700b0400 };
    const unsigned irqs[] = { 31, 19 };
    uint8_t expected[512], actual[512];
    int fd = g_file_open_tmp("mh2p-mnand-XXXXXX", &image, &error);
    QTestState *s;

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 4ULL * 1024 * 1024 * 1024), ==, 0);
    for (unsigned i = 0; i < sizeof(expected); i++) {
        expected[i] = i * 5 + 3;
    }
    g_assert_cmpint(pwrite(fd, expected, sizeof(expected), 7 * 512), ==, 512);
    args = g_strdup_printf("-drive if=sd,index=0,format=raw,snapshot=on,"
                           "file=%s "
                           "-drive if=sd,index=1,format=raw,snapshot=on,file=%s",
                           image, image);
    s = start_mh2p(args);
    qtest_clock_step(s, 100000000);
    for (unsigned bank = 0; bank < ARRAY_SIZE(bases); bank++) {
        uint64_t base = bases[bank];
        uint32_t mask = 1U << irqs[bank];

        qtest_writel(s, base + 0x100, 0x00120008);
        qtest_writel(s, base + 0x120, 0x218);
        qtest_writel(s, base + 0x1e0, 0x80000007);
        qtest_writel(s, base + 0x1e4, 0xa0007f5f);
        g_assert_cmphex(qtest_readl(s, base + 0x100), ==, 0x00120008);
        g_assert_cmphex(qtest_readl(s, base + 0x120), ==, 0x218);
        g_assert_cmphex(qtest_readl(s, base + 0x1e4), ==, 0x20007f5f);
        g_assert_cmphex(qtest_readl(s, base + 0x1ec), ==, 0);
        sd_setup_host(s, base);
        qtest_writel(s, 0x50041104, mask);
        sd_command(s, base, 0x0102, 0x40ff8000);
        g_assert_cmphex(qtest_readl(s, base + 0x10) & 0xc0000000, ==,
                        0xc0000000);
        g_assert_cmphex(qtest_readl(s, 0x50041204) & mask, ==, mask);
        qtest_writew(s, base + 0x30, 1);
        g_assert_cmphex(qtest_readl(s, 0x50041204) & mask, ==, 0);
        sd_command(s, base, 0x0209, 0);
        sd_command(s, base, 0x031a, 0x10000);
        sd_command(s, base, 0x071b, 0x10000);
        qtest_writel(s, base + 4, 0x00010200);
        qtest_writew(s, base + 0xc, 0x10);
        sd_command(s, base, 0x113a, 7);
        for (unsigned i = 0; i < sizeof(actual); i += 4) {
            uint32_t word = qtest_readl(s, base + 0x20);

            for (unsigned j = 0; j < 4; j++) {
                actual[i + j] = word >> (8 * j);
            }
        }
        g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
        qtest_writel(s, base + 4, 0x00010200);
        qtest_writew(s, base + 0xc, 0);
        sd_command(s, base, 0x183a, 7);
        for (unsigned i = 0; i < sizeof(actual); i += 4) {
            qtest_writel(s, base + 0x20, 0);
        }
        qtest_clock_step(s, 1000000);
        g_assert_cmphex(qtest_readw(s, base + 0x30) & 2, ==, 2);
    }
    qtest_system_reset(s);
    for (unsigned bank = 0; bank < ARRAY_SIZE(bases); bank++) {
        g_assert_cmphex(qtest_readl(s, bases[bank] + 0x100), ==, 0);
        g_assert_cmphex(qtest_readl(s, bases[bank] + 0x120), ==, 0);
        g_assert_cmphex(qtest_readl(s, bases[bank] + 0x1e0), ==, 0);
        g_assert_cmphex(qtest_readl(s, bases[bank] + 0x1e4), ==, 0);
    }
    qtest_quit(s);
    g_assert_cmpint(pread(fd, actual, sizeof(actual), 7 * 512), ==, 512);
    g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
    close(fd);
    unlink(image);
}

static void i2c_nack_packet(QTestState *s, uint64_t base)
{
    qtest_writel(s, base, 0xc00);
    qtest_writel(s, base + 0x50, 0x10);
    qtest_writel(s, base + 0x50, 0);
    qtest_writel(s, base + 0x50, (1 << 17) | (0x50 << 1));
    qtest_writel(s, base + 0x50, 3);
}

static void test_i2c_nack_payload(void)
{
    QTestState *s = start_mh2p("");
    uint64_t base = 0x7000c400;

    qtest_writel(s, base, 0xc00);
    for (unsigned length = 1; length <= 9; length++) {
        qtest_writel(s, base + 0x68, 0xffffffff);
        qtest_writel(s, base + 0x50, 0x10);
        qtest_writel(s, base + 0x50, length - 1);
        qtest_writel(s, base + 0x50, (1 << 16) | (0x4c << 1));
        g_assert_cmphex(qtest_readl(s, base + 0x68) & 0xc8, ==, 0x88);
        for (unsigned i = 0; i < length; i += 4) {
            qtest_writel(s, base + 0x50, 0x03030303);
        }
        qtest_writel(s, base + 0x68, 0xffffffff);
        qtest_writel(s, base + 0x50, 0x10);
        qtest_writel(s, base + 0x50, 0);
        qtest_writel(s, base + 0x50, (1 << 19) | (0x4c << 1));
        g_assert_cmphex(qtest_readl(s, base + 0x68) & 0xc8, ==, 0xc8);
    }

    /* A flush or controller reset also abandons an unsent NACKed payload. */
    for (unsigned reset = 0; reset < 2; reset++) {
        qtest_writel(s, base + 0x50, 0x10);
        qtest_writel(s, base + 0x50, 8);
        qtest_writel(s, base + 0x50, 0x4c << 1);
        if (reset) {
            qtest_writel(s, 0x60006308, 1U << 22);
            qtest_writel(s, 0x6000630c, 1U << 22);
        } else {
            qtest_writel(s, base + 0x5c, 2);
        }
        qtest_writel(s, base + 0x68, 0xffffffff);
        i2c_nack_packet(s, base);
        g_assert_cmphex(qtest_readl(s, base + 0x68) & 0xc8, ==, 0xc8);
    }
    qtest_quit(s);
}

static void test_i2c(void)
{
    static const struct {
        uint64_t base, reset_set, reset_clear;
        unsigned reset_bit, spi;
    } controllers[] = {
        { 0x7000c000, 0x60006300, 0x60006304, 12, 38 },
        { 0x7000c400, 0x60006308, 0x6000630c, 22, 84 },
        { 0x7000c500, 0x60006310, 0x60006314, 3, 92 },
        { 0x7000c700, 0x60006430, 0x60006434, 7, 120 },
        { 0x7000d000, 0x60006308, 0x6000630c, 15, 53 },
        { 0x7000d100, 0x60006290, 0x60006294, 6, 63 },
    };
    QTestState *s = start_mh2p("");

    g_assert_cmphex(qtest_readl(s, 0x545c0124), ==, 0);
    qtest_writel(s, 0x545c0124, 0xc001);
    g_assert_cmphex(qtest_readl(s, 0x545c0124), ==, 0xc001);
    qtest_writel(s, 0x545c0124, 0xffffffff);
    g_assert_cmphex(qtest_readl(s, 0x545c0124), ==, 0xc001);
    qtest_writel(s, 0x6000665c, 0x40000004);
    g_assert_cmphex(qtest_readl(s, 0x6000665c), ==, 0x40000004);

    for (unsigned i = 0; i < ARRAY_SIZE(controllers); i++) {
        uint64_t base = controllers[i].base;
        unsigned irq = controllers[i].spi + 32;
        uint32_t mask = 1U << (irq % 32);
        uint64_t pending = 0x50041200 + (irq / 32) * 4;

        g_assert_cmphex(qtest_readl(s, base), ==, 0x800);
        qtest_writel(s, base + 0x8c, 7);
        g_assert_cmphex(qtest_readl(s, base + 0x8c), ==, 0);
        qtest_writel(s, base + 4, 0x50 << 1);
        qtest_writel(s, base, 0xa00);
        g_assert_cmphex(qtest_readl(s, base + 0x1c), ==, 1);
        g_assert_cmphex(qtest_readl(s, base) & 0x200, ==, 0);

        qtest_writel(s, 0x50041100 + (irq / 32) * 4, mask);
        i2c_nack_packet(s, base);
        g_assert_cmphex(qtest_readl(s, base + 0x68) & 0xc8, ==, 0xc8);
        g_assert_cmphex(qtest_readl(s, pending) & mask, ==, 0);
        qtest_writel(s, base + 0x64, 8);
        g_assert_cmphex(qtest_readl(s, pending) & mask, ==, mask);
        qtest_writel(s, base + 0x68, 8);
        g_assert_cmphex(qtest_readl(s, pending) & mask, ==, 0);

        i2c_nack_packet(s, base);
        qtest_writel(s, controllers[i].reset_set,
                     1U << controllers[i].reset_bit);
        g_assert_cmphex(qtest_readl(s, pending) & mask, ==, 0);
        qtest_writel(s, base + 0x6c, 0x1234);
        g_assert_cmphex(qtest_readl(s, base), ==, 0);
        g_assert_cmphex(qtest_readl(s, base + 0x6c), ==, 0);
        qtest_writel(s, controllers[i].reset_clear,
                     1U << controllers[i].reset_bit);
        g_assert_cmphex(qtest_readl(s, base), ==, 0x800);
        g_assert_cmphex(qtest_readl(s, base + 0x64), ==, 0);
        g_assert_cmphex(qtest_readl(s, base + 0x68), ==, 0);
        qtest_writel(s, base + 0x6c, 0x100 + i);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(controllers); i++) {
        uint64_t base = controllers[i].base;

        g_assert_cmphex(qtest_readl(s, base + 0x6c), ==, 0x100 + i);
        qtest_writel(s, base + 0x64, 8);
        i2c_nack_packet(s, base);
    }
    qtest_system_reset(s);
    for (unsigned i = 0; i < ARRAY_SIZE(controllers); i++) {
        uint64_t base = controllers[i].base;

        g_assert_cmphex(qtest_readl(s, base), ==, 0x800);
        g_assert_cmphex(qtest_readl(s, base + 0x64), ==, 0);
        g_assert_cmphex(qtest_readl(s, base + 0x68), ==, 0);
        g_assert_cmphex(qtest_readl(s, base + 0x6c), ==, 0);
    }
    g_assert_cmphex(qtest_readl(s, 0x545c0124), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x6000665c), ==, 0);
    qtest_quit(s);
}

static void test_spi(void)
{
    static const struct {
        uint64_t base, reset_set, reset_clear;
        unsigned reset_bit, spi;
    } controllers[] = {
        { 0x7000d400, 0x60006308, 0x6000630c, 9, 59 },
        { 0x7000d600, 0x60006308, 0x6000630c, 12, 82 },
        { 0x7000d800, 0x60006308, 0x6000630c, 14, 83 },
        { 0x7000da00, 0x60006310, 0x60006314, 4, 93 },
        { 0x7000dc00, 0x60006430, 0x60006434, 8, 94 },
        { 0x7000de00, 0x60006430, 0x60006434, 9, 79 },
    };
    QTestState *s = start_mh2p("");

    for (unsigned i = 0; i < ARRAY_SIZE(controllers); i++) {
        uint64_t base = controllers[i].base;
        unsigned irq = controllers[i].spi + 32;
        uint32_t mask = 1U << (irq % 32);
        uint64_t pending = 0x50041200 + (irq / 32) * 4;

        qtest_writel(s, controllers[i].reset_clear,
                     1U << controllers[i].reset_bit);
        qtest_writel(s, 0x50041100 + (irq / 32) * 4, mask);
        /* Sequence used by the factory spi-tegra124.so driver. */
        qtest_writel(s, base, (1U << 26) | (1U << 5) | (1 << 12) |
                              (1 << 11) | 7);
        qtest_writel(s, base + 0x14, 0xc000);
        g_assert_cmphex(qtest_readl(s, base + 0x14) & 0xc000, ==, 0);
        g_assert_cmphex(qtest_readl(s, base + 0x14) & 0x05, ==, 0x05);
        qtest_writel(s, base + 0x108, 0x44332211);
        qtest_writel(s, base + 0x108, 0x55);
        g_assert_cmphex(qtest_readl(s, base + 0x14) >> 16 & 0x7f, ==, 62);
        qtest_writel(s, base + 0x24, 4);
        qtest_writel(s, base + 0x20, 1U << 29);
        g_assert_cmphex(qtest_readl(s, pending) & mask, ==, 0);
        qtest_writel(s, base, qtest_readl(s, base) | (1U << 31));

        g_assert_cmphex(qtest_readl(s, base) >> 31, ==, 0);
        g_assert_cmphex(qtest_readl(s, base + 0x10), ==, (1U << 30) | 5);
        g_assert_cmphex(qtest_readl(s, pending) & mask, ==, mask);
        g_assert_cmphex(qtest_readl(s, base + 0x14) & 0x05, ==, 0x04);
        g_assert_cmphex(qtest_readl(s, base + 0x14) >> 23 & 0x7f, ==, 2);
        g_assert_cmphex(qtest_readl(s, base + 0x188), ==, 0);
        g_assert_cmphex(qtest_readl(s, base + 0x188), ==, 0);
        g_assert_cmphex(qtest_readl(s, base + 0x14) & 0x01, ==, 0x01);
        qtest_writel(s, base + 0x10, qtest_readl(s, base + 0x10));
        g_assert_cmphex(qtest_readl(s, pending) & mask, ==, 0);

        qtest_writel(s, base + 0x108, 0x1234);
        qtest_writel(s, controllers[i].reset_set,
                     1U << controllers[i].reset_bit);
        qtest_writel(s, base + 0x24, 0x77);
        g_assert_cmphex(qtest_readl(s, base + 0x24), ==, 0);
        qtest_writel(s, controllers[i].reset_clear,
                     1U << controllers[i].reset_bit);
        g_assert_cmphex(qtest_readl(s, base + 0x14) & 0x05, ==, 0x05);
        g_assert_cmphex(qtest_readl(s, base + 0x20), ==, 0);
    }
    qtest_quit(s);
}

static void test_smmu_init(void)
{
    static const uint16_t groups[] = {
        0x238, 0x23c, 0x240, 0x244, 0x250, 0x254, 0x258, 0x264,
        0x270, 0x274, 0x27c, 0x280, 0x284, 0x288, 0x28c, 0x290,
        0x294, 0xa88, 0xa94, 0xa98, 0xa9c, 0xaa0, 0xaa4, 0xaa8, 0xaac,
    };
    static const uint16_t security[] = {
        0x38, 0x3c, 0x9e0, 0x9e4, 0x9e8, 0x9ec, 0x9f0, 0x9f4,
    };
    QTestState *s = start_mh2p("");

    for (unsigned boot = 0; boot < 2; boot++) {
        g_assert_cmphex(qtest_readl(s, 0x70000460), ==, 0);
        g_assert_cmphex(qtest_readl(s, 0x7000f9c4), ==, 0);
        qtest_writel(s, 0x70000460, 1);
        qtest_writel(s, 0x7000f9c4, 1);
        g_assert_cmphex(qtest_readl(s, 0x70000460), ==, 0);
        g_assert_cmphex(qtest_readl(s, 0x7000f9c4), ==, 0);
        g_assert_cmphex(qtest_readl(s, 0x70019010), ==, 0);
        g_assert_cmphex(qtest_readl(s, 0x70019014), ==, 0x20000000);
        g_assert_cmphex(qtest_readl(s, 0x70019018), ==, 0x2000003f);
        g_assert_cmphex(qtest_readl(s, 0x7001901c), ==, 0);
        g_assert_cmphex(qtest_readl(s, 0x700199b8), ==, 0);

        for (unsigned asid = 0; asid < 128; asid++) {
            qtest_writel(s, 0x7001901c, asid);
            g_assert_cmphex(qtest_readl(s, 0x70019020), ==, 0);
            qtest_writel(s, 0x70019020, 0xe0280000 | asid);
        }
        for (unsigned asid = 0; asid < 128; asid++) {
            qtest_writel(s, 0x7001901c, asid);
            g_assert_cmphex(qtest_readl(s, 0x70019020), ==,
                           0xe0280000 | asid);
        }
        qtest_writel(s, 0x7001901c, 0xffffffff);
        g_assert_cmphex(qtest_readl(s, 0x7001901c), ==, 127);
        qtest_writel(s, 0x70019020, 0xffffffff);
        g_assert_cmphex(qtest_readl(s, 0x70019020), ==, 0xe03fffff);

        for (unsigned i = 0; i < ARRAY_SIZE(groups); i++) {
            g_assert_cmphex(qtest_readl(s, 0x70019000 + groups[i]), ==, 0);
            qtest_writel(s, 0x70019000 + groups[i], 0xffffff80 | i);
            g_assert_cmphex(qtest_readl(s, 0x70019000 + groups[i]), ==,
                           0x80000000 | i);
        }
        for (unsigned i = 0; i < 4; i++) {
            g_assert_cmphex(qtest_readl(s, 0x70019228 + 4 * i), ==, 0);
            qtest_writel(s, 0x70019228 + 4 * i, 0x12345678 + i);
            g_assert_cmphex(qtest_readl(s, 0x70019228 + 4 * i), ==,
                           0x12345678 + i);
        }
        for (unsigned i = 0; i < ARRAY_SIZE(security); i++) {
            qtest_writel(s, 0x70019000 + security[i], 0);
            qtest_writel(s, 0x70019000 + security[i], 0xffffffff);
            g_assert_cmphex(qtest_readl(s, 0x70019000 + security[i]), ==, 0);
        }

        qtest_writel(s, 0x70019014, 0x30000020);
        qtest_writel(s, 0x70019018, 0x2800003f);
        qtest_writel(s, 0x70019010, 1);
        qtest_writel(s, 0x70019014, 0xb0000020);
        qtest_writel(s, 0x70019018, 0xa800003f);
        qtest_writel(s, 0x70019010, 3);
        g_assert_cmphex(qtest_readl(s, 0x70019014), ==, 0x30000020);
        g_assert_cmphex(qtest_readl(s, 0x70019018), ==, 0x2800003f);
        g_assert_cmphex(qtest_readl(s, 0x70019010), ==, 1);
        qtest_writel(s, 0x700199b8, 0xffffffff);
        g_assert_cmphex(qtest_readl(s, 0x700199b8), ==, 3);
        qtest_writel(s, 0x70019034, 0x81234001);
        qtest_writel(s, 0x70019030, 0xff012342);
        qtest_writel(s, 0x70019034, 0);
        qtest_writel(s, 0x70019030, 0);
        g_assert_cmphex(qtest_readl(s, 0x70019030), ==, 0);
        g_assert_cmphex(qtest_readl(s, 0x70019034), ==, 0);
        g_assert_cmphex(qtest_readl(s, 0x70019020), ==, 0xe03fffff);
        qtest_system_reset(s);
    }
    qtest_quit(s);
}

static void test_host1x_sync(void)
{
    const uint64_t sync = 0x50002100;
    static const unsigned ids[] = {
        0, 31, 32, 63, 64, 127, 128, 159, 160, 191
    };
    static const unsigned config[] = { 4, 8, 0x24, 0x2c, 0x1a4, 0x1a8, 0x1bc };
    QTestState *s = start_mh2p("");

    /* Output interception disconnects the GIC, so check routing first. */
    qtest_writel(s, 0x5004110c, 2); /* SPI 65 = GIC interrupt 97. */
    qtest_writel(s, sync + 0x1380, 1);
    qtest_writel(s, sync + 0xf20, 1);
    qtest_writel(s, sync + 0xf60, 1);
    g_assert_cmphex(qtest_readl(s, 0x5004120c) & 2, ==, 2);
    qtest_writel(s, sync + 0xf00, 1);
    qtest_writel(s, sync + 0xe80, 1);
    g_assert_cmphex(qtest_readl(s, 0x5004120c) & 2, ==, 0);
    qtest_system_reset(s);
    qtest_irq_intercept_out_named(s, "/machine/host1x", "sysbus-irq");
    for (unsigned boot = 0; boot < 2; boot++) {
        for (unsigned id = 0; id < 192; id++) {
            g_assert_cmphex(qtest_readl(s, sync + 0xf80 + 4 * id), ==, 0);
            g_assert_cmphex(qtest_readl(s, sync + 0x1380 + 4 * id), ==, 0);
            qtest_writel(s, sync + 0xf80 + 4 * id, 0x12340000 + id);
            qtest_writel(s, sync + 0x1380 + 4 * id, 0x12340002 + id);
        }
        for (unsigned id = 0; id < 192; id++) {
            g_assert_cmphex(qtest_readl(s, sync + 0xf80 + 4 * id), ==,
                           0x12340000 + id);
        }
        for (unsigned i = 0; i < ARRAY_SIZE(ids); i++) {
            unsigned id = ids[i], bank = id / 32;
            uint32_t bit = 1U << (id % 32);

            qtest_writel(s, sync + 0xf20 + 4 * bank, bit);
            g_assert_false(qtest_get_irq(s, 0));
            qtest_writel(s, sync + 0xf60 + 4 * bank, bit);
            g_assert_false(qtest_get_irq(s, 0));
            qtest_writel(s, sync + 0xf60 + 4 * bank, bit);
            g_assert_cmphex(qtest_readl(s, sync + 0xf80 + 4 * id), ==,
                           0x12340002 + id);
            g_assert_cmphex(qtest_readl(s, sync + 0xe80 + 4 * bank), ==, bit);
            g_assert_true(qtest_get_irq(s, 0));
            qtest_writel(s, sync + 0xe80 + 4 * bank, bit);
            g_assert_true(qtest_get_irq(s, 0));
            qtest_writel(s, sync + 0xf00 + 4 * bank, bit);
            g_assert_false(qtest_get_irq(s, 0));
            qtest_writel(s, sync + 0xe80 + 4 * bank, bit);
            g_assert_cmphex(qtest_readl(s, sync + 0xe80 + 4 * bank), ==, 0);
        }

        qtest_writel(s, sync + 0x127c, 0xffffffff);
        qtest_writel(s, sync + 0x167c, 0);
        qtest_writel(s, sync + 0xf34, 0x80000000);
        g_assert_false(qtest_get_irq(s, 0));
        qtest_writel(s, sync + 0xf74, 0x80000000);
        g_assert_cmphex(qtest_readl(s, sync + 0x127c), ==, 0);
        g_assert_true(qtest_get_irq(s, 0));
        qtest_writel(s, sync + 0xf14, 0x80000000);
        qtest_writel(s, sync + 0xe94, 0x80000000);

        for (unsigned id = 0; id < 16; id++) {
            g_assert_cmphex(qtest_readl(s, sync + 0x340 + id * 4), ==, 0);
            g_assert_cmphex(qtest_readl(s, sync + 0x2c0 + id * 4), ==, 0);
            g_assert_cmphex(qtest_readl(s, sync + 0x2c0 + id * 4), ==, 1);
            g_assert_cmphex(qtest_readl(s, sync + 0x340 + id * 4), ==, 2);
            qtest_writel(s, sync + 0x2c0 + id * 4, 0);
            g_assert_cmphex(qtest_readl(s, sync + 0x340 + id * 4), ==, 0);
        }
        for (unsigned id = 0; id < 64; id++) {
            g_assert_cmphex(qtest_readl(s, sync + 0x600 + id * 4), ==, 0);
            qtest_writel(s, sync + 0x600 + id * 4, id + 1);
            g_assert_cmphex(qtest_readl(s, sync + 0x600 + id * 4), ==, id + 1);
        }
        for (unsigned id = 0; id < 12; id++) {
            g_assert_cmphex(qtest_readl(s, sync + 0xc00 + id * 4), ==, 0);
            qtest_writel(s, sync + 0xc00 + id * 4, 0xffffffff);
            g_assert_cmphex(qtest_readl(s, sync + 0xc00 + id * 4), ==,
                           0x03ff03ff);
        }
        for (unsigned i = 0; i < ARRAY_SIZE(config); i++) {
            g_assert_cmphex(qtest_readl(s, sync + config[i]), ==, 0);
            qtest_writel(s, sync + config[i], i + 1);
            g_assert_cmphex(qtest_readl(s, sync + config[i]), ==, i + 1);
        }
        qtest_writel(s, sync + 0x20, 0xffffffff);
        qtest_writel(s, sync + 0x28, 0xffffffff);
        g_assert_cmphex(qtest_readl(s, sync + 0x20), ==, 0);
        g_assert_cmphex(qtest_readl(s, sync + 0x28), ==, 0);
        qtest_writel(s, sync + 0xf40, 1);
        g_assert_cmphex(qtest_readl(s, sync + 0xf40), ==, 0);

        g_assert_cmphex(qtest_readl(s, 0x7000e6d4), ==, 1);
        qtest_writel(s, 0x7000e6d4, 0);
        g_assert_cmphex(qtest_readl(s, 0x7000e6d4), ==, 0);
        qtest_writel(s, 0x7000e6d4, 2);
        g_assert_cmphex(qtest_readl(s, 0x7000e6d4), ==, 0);

        qtest_writel(s, sync + 0xf34, 0x80000000);
        g_assert_true(qtest_get_irq(s, 0));
        qtest_writel(s, 0x60006300, 1U << 28);
        g_assert_false(qtest_get_irq(s, 0));
        qtest_writel(s, sync + 0xf80, 0xdeadbeef);
        qtest_writel(s, 0x60006304, 1U << 28);
        g_assert_cmphex(qtest_readl(s, sync + 0xf80), ==, 0);
        g_assert_cmphex(qtest_readl(s, sync + 0xf34), ==, 0);
        qtest_writel(s, sync + 0xf80, 1);
        qtest_system_reset(s);
        g_assert_false(qtest_get_irq(s, 0));
    }
    qtest_quit(s);
}

static void test_gk20a_init(void)
{
    static const struct {
        unsigned offset;
        uint32_t mask;
    } config[] = {
        { 0x1140, 0xe },
        { 0x137250, 0x80003f3f },
        { 0x200, 0x20 },
        { 0x9080, 0x80ffffff },
    };
    const uint64_t gpu = 0x57000000;
    QTestState *s = start_mh2p("");

    for (unsigned boot = 0; boot < 2; boot++) {
        g_assert_cmphex(qtest_readl(s, 0x7000e6d4), ==, 1);
        g_assert_cmphex(qtest_readl(s, gpu), ==, 0);
        qtest_writel(s, gpu + 0x1140, 0xe);
        qtest_writel(s, 0x7000e6d4, 0);
        g_assert_cmphex(qtest_readl(s, gpu), ==, 0x0ea000a1);
        qtest_writel(s, gpu, 0xffffffff);
        g_assert_cmphex(qtest_readl(s, gpu), ==, 0x0ea000a1);
        qtest_writel(s, gpu + 0x204, 1);
        qtest_writel(s, gpu + 0x2140, UINT32_MAX);
        g_assert_cmphex(qtest_readl(s, gpu + 0x204), ==, 0);
        g_assert_cmphex(qtest_readl(s, gpu + 0x2140), ==, 0);
        g_assert_cmphex(qtest_readl(s, gpu + 0x2390), ==, 0);

        for (unsigned i = 0; i < ARRAY_SIZE(config); i++) {
            uint64_t address = gpu + config[i].offset;

            g_assert_cmphex(qtest_readl(s, address), ==, 0);
            qtest_writel(s, address, config[i].mask);
            g_assert_cmphex(qtest_readl(s, address), ==, config[i].mask);
            qtest_writel(s, address, ~config[i].mask);
            g_assert_cmphex(qtest_readl(s, address), ==, config[i].mask);
            qtest_writeb(s, address, 0);
            g_assert_cmphex(qtest_readl(s, address), ==, config[i].mask);
        }
        qtest_writel(s, gpu + 0x1100, 0xe);
        g_assert_cmphex(qtest_readl(s, gpu + 0x1100), ==, 0);
        qtest_writel(s, gpu + 0x200, 0);
        g_assert_cmphex(qtest_readl(s, gpu + 0x200), ==, 0);
        g_assert_cmphex(qtest_readl(s, gpu + 0x1140), ==, 0xe);

        /* Neither input may release the GPU while the other holds it. */
        for (unsigned order = 0; order < 2; order++) {
            if (order == 0) {
                qtest_writel(s, 0x60006290, 1U << 24);
                qtest_writel(s, 0x7000e6d4, 1);
                qtest_writel(s, 0x60006294, 1U << 24);
            } else {
                qtest_writel(s, 0x7000e6d4, 1);
                qtest_writel(s, 0x60006290, 1U << 24);
                qtest_writel(s, 0x7000e6d4, 0);
            }
            g_assert_cmphex(qtest_readl(s, gpu), ==, 0);
            for (unsigned i = 0; i < ARRAY_SIZE(config); i++) {
                qtest_writel(s, gpu + config[i].offset, config[i].mask);
            }
            if (order == 0) {
                qtest_writel(s, 0x7000e6d4, 0);
            } else {
                qtest_writel(s, 0x60006294, 1U << 24);
            }
            g_assert_cmphex(qtest_readl(s, gpu), ==, 0x0ea000a1);
            for (unsigned i = 0; i < ARRAY_SIZE(config); i++) {
                g_assert_cmphex(qtest_readl(s, gpu + config[i].offset), ==, 0);
                qtest_writel(s, gpu + config[i].offset, config[i].mask);
            }
        }
        qtest_system_reset(s);
    }
    qtest_quit(s);
}

static void assert_gk20a_ring(QTestState *s, bool started, uint32_t decode)
{
    g_assert_cmphex(qtest_readl(s, 0x5712004c), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x57120050), ==, started);
    g_assert_cmphex(qtest_readl(s, 0x57120070), ==, started);
    g_assert_cmphex(qtest_readl(s, 0x57120074), ==, started);
    g_assert_cmphex(qtest_readl(s, 0x57120078), ==, started);
    g_assert_cmphex(qtest_readl(s, 0x57120058), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x5712005c), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x57122204), ==, decode);
}

static void test_gk20a_ring(void)
{
    static const uint32_t unsupported_commands[] = {
        1, 3, 5, 0x44, 0xffffffff,
    };
    static const uint64_t read_only[] = {
        0x57120050, 0x57120070, 0x57120074, 0x57120078,
        0x57120058, 0x5712005c,
    };
    QTestState *s = start_mh2p("");

    for (unsigned boot = 0; boot < 2; boot++) {
        qtest_writel(s, 0x7000e6d4, 0);
        assert_gk20a_ring(s, false, 0);
        qtest_writel(s, 0x5712004c, 4);
        qtest_writel(s, 0x57122204, 2);
        assert_gk20a_ring(s, false, 0);
        qtest_writel(s, 0x57000200, 0x20);
        assert_gk20a_ring(s, false, 0);
        qtest_writel(s, 0x5712004c, 4);
        assert_gk20a_ring(s, true, 0);
        for (unsigned decode = 0; decode <= 2; decode++) {
            qtest_writel(s, 0x57122204, decode);
            assert_gk20a_ring(s, true, decode);
        }
        qtest_writel(s, 0x57009080, 0x800186a0);
        for (unsigned i = 0; i < ARRAY_SIZE(unsupported_commands); i++) {
            qtest_writel(s, 0x5712004c, unsupported_commands[i]);
            assert_gk20a_ring(s, true, 2);
        }
        for (unsigned i = 0; i < ARRAY_SIZE(read_only); i++) {
            qtest_writel(s, read_only[i], 0xffffffff);
        }
        qtest_writel(s, 0x57122204, 3);
        qtest_writel(s, 0x57122204, 0xffffffff);
        qtest_writel(s, 0x57000200, 0x200120);
        g_assert_cmphex(qtest_readl(s, 0x57000200), ==, 0x20);
        assert_gk20a_ring(s, true, 2);
        qtest_writel(s, 0x5712004c, 0);
        qtest_writel(s, 0x5712004c, 2);
        assert_gk20a_ring(s, true, 2);

        for (unsigned reset = 0; reset < 3; reset++) {
            if (reset == 0) {
                qtest_writel(s, 0x57000200, 0);
            } else if (reset == 1) {
                qtest_writel(s, 0x60006290, 1U << 24);
            } else {
                qtest_writel(s, 0x7000e6d4, 1);
            }
            qtest_writel(s, 0x5712004c, 4);
            qtest_writel(s, 0x57122204, 2);
            if (reset == 1) {
                qtest_writel(s, 0x60006294, 1U << 24);
            } else if (reset == 2) {
                qtest_writel(s, 0x7000e6d4, 0);
            }
            assert_gk20a_ring(s, false, 0);
            g_assert_cmphex(qtest_readl(s, 0x57009080), ==,
                           reset == 0 ? 0x800186a0 : 0);
            qtest_writel(s, 0x57000200, 0x20);
            qtest_writel(s, 0x5712004c, 4);
            qtest_writel(s, 0x57122204, 2);
            assert_gk20a_ring(s, true, 2);
        }
        qtest_system_reset(s);
    }
    qtest_quit(s);
}

static void test_gk20a_engine_controls(void)
{
    static const struct {
        uint32_t offset;
        uint32_t mask;
    } config[] = {
        { 0x2140, UINT32_MAX }, { 0x2528, UINT32_MAX },
        { 0x2a04, 0x3fffffff }, { 0x2a0c, UINT32_MAX },
        { 0x2a08, 0x80000000 },
        { 0x4010c, UINT32_MAX }, { 0x4014c, UINT32_MAX },
        { 0x4013c, UINT32_MAX }, { 0x4012c, UINT32_MAX },
    };
    static const uint32_t status[] = {
        0x2100, 0x2a00, 0x40108, 0x40148,
    };
    const uint64_t gpu = 0x57000000;
    QTestState *s = start_mh2p("-global gk20a.experimental-engine-init=on");

    for (unsigned boot = 0; boot < 2; boot++) {
        qtest_writel(s, 0x7000e6d4, 0);
        g_assert_cmphex(qtest_readl(s, 0x57000200), ==, 0);
        g_assert_cmphex(qtest_readl(s, 0x57000204), ==, 0);
        g_assert_cmphex(qtest_readl(s, 0x5700020c), ==, 0);
        g_assert_cmphex(qtest_readl(s, gpu + 0x2390), ==, 0);
        for (unsigned reset = 0; reset < 3; reset++) {
            qtest_writel(s, 0x5700020c, 0x20100004);
            qtest_writel(s, 0x5700020c, UINT32_MAX);
            qtest_writeb(s, 0x5700020c, 0);
            g_assert_cmphex(qtest_readl(s, 0x5700020c), ==, 0x20100004);
            qtest_writel(s, 0x57000200, 0x200120);
            g_assert_cmphex(qtest_readl(s, 0x57000200), ==, 0x200120);
            g_assert_cmphex(qtest_readl(s, gpu + 0x2390), ==, 1);
            qtest_writel(s, gpu + 0x2390, 0);
            qtest_writel(s, gpu + 0x2390, UINT32_MAX);
            g_assert_cmphex(qtest_readl(s, gpu + 0x2390), ==, 1);
            g_assert_cmphex(qtest_readl(s, gpu + 0x2394), ==, 0);
            qtest_writel(s, 0x57000204, 1);
            qtest_writel(s, 0x57000204, 2);
            g_assert_cmphex(qtest_readl(s, 0x57000204), ==, 1);
            for (unsigned i = 0; i < ARRAY_SIZE(config); i++) {
                uint64_t address = gpu + config[i].offset;

                g_assert_cmphex(qtest_readl(s, address), ==, 0);
                qtest_writel(s, address, config[i].mask);
                qtest_writeb(s, address, 0);
                if (config[i].mask != UINT32_MAX) {
                    qtest_writel(s, address, ~config[i].mask);
                }
                g_assert_cmphex(qtest_readl(s, address), ==, config[i].mask);
            }
            for (unsigned i = 0; i < ARRAY_SIZE(status); i++) {
                qtest_writel(s, gpu + status[i], UINT32_MAX);
                g_assert_cmphex(qtest_readl(s, gpu + status[i]), ==, 0);
            }
            assert_gk20a_ring(s, false, 0);
            qtest_writel(s, 0x5712004c, 4);
            qtest_writel(s, 0x57122204, 2);
            assert_gk20a_ring(s, true, 2);
            qtest_writel(s, 0x57000200, 0x201120);
            g_assert_cmphex(qtest_readl(s, 0x57000200), ==, 0x201120);
            qtest_writel(s, 0x57000200, 0x2030012c);
            g_assert_cmphex(qtest_readl(s, 0x57000200), ==, 0x2030012c);
            qtest_writel(s, 0x57000200, 0x3830317c);
            g_assert_cmphex(qtest_readl(s, 0x57000200), ==, 0x3830317c);
            qtest_writel(s, 0x57000200, UINT32_MAX);
            qtest_writeb(s, 0x57000200, 0);
            g_assert_cmphex(qtest_readl(s, 0x57000200), ==, 0x3830317c);
            assert_gk20a_ring(s, true, 2);

            qtest_writel(s, 0x57000200, 0x200100);
            g_assert_cmphex(qtest_readl(s, 0x57000200), ==, 0x200100);
            assert_gk20a_ring(s, false, 0);
            for (unsigned i = 0; i < ARRAY_SIZE(config); i++) {
                g_assert_cmphex(qtest_readl(s, gpu + config[i].offset),
                                ==, config[i].mask);
            }
            qtest_writel(s, 0x57000204, 0);
            g_assert_cmphex(qtest_readl(s, gpu + 0x2390), ==, 1);
            qtest_writel(s, 0x5704012c, 123);
            qtest_writel(s, 0x57000204, 1);
            for (unsigned i = 0; i < ARRAY_SIZE(config); i++) {
                uint32_t expected = config[i].offset < 0x40000 ?
                                    config[i].mask : 0;

                g_assert_cmphex(qtest_readl(s, gpu + config[i].offset), ==,
                                expected);
                qtest_writel(s, gpu + config[i].offset, config[i].mask);
            }
            qtest_writel(s, 0x57000200, 0x200120);
            if (reset == 0) {
                qtest_writel(s, 0x57000200, 0);
            } else if (reset == 1) {
                qtest_writel(s, 0x60006290, 1U << 24);
                qtest_writel(s, 0x57000200, 0x200120);
                qtest_writel(s, 0x60006294, 1U << 24);
            } else {
                qtest_writel(s, 0x7000e6d4, 1);
                qtest_writel(s, 0x57000200, 0x200120);
                qtest_writel(s, 0x7000e6d4, 0);
            }
            g_assert_cmphex(qtest_readl(s, 0x57000200), ==, 0);
            g_assert_cmphex(qtest_readl(s, gpu + 0x2390), ==, 0);
            g_assert_cmphex(qtest_readl(s, 0x57000204), ==, reset == 0 ? 1 : 0);
            g_assert_cmphex(qtest_readl(s, 0x5700020c), ==,
                            reset == 0 ? 0x20100004 : 0);
            assert_gk20a_ring(s, false, 0);
            qtest_writel(s, 0x57002a04, 123);
            qtest_writel(s, 0x57000200, 0x200120);
            qtest_writel(s, 0x57000204, 1);
            for (unsigned i = 0; i < ARRAY_SIZE(config); i++) {
                g_assert_cmphex(qtest_readl(s, gpu + config[i].offset), ==, 0);
            }
        }
        qtest_writel(s, 0x57000200, 0x200120);
        for (unsigned i = 0; i < ARRAY_SIZE(config); i++) {
            qtest_writel(s, gpu + config[i].offset, config[i].mask);
        }
        qtest_system_reset(s);
    }
    qtest_quit(s);
}

static void test_gk20a_mmu_page_size(void)
{
    for (unsigned enabled = 0; enabled < 2; enabled++) {
        QTestState *s = start_mh2p(enabled ?
            "-global gk20a.experimental-engine-init=on" : "");

        for (unsigned boot = 0; boot < 2; boot++) {
            qtest_writel(s, 0x7000e6d4, 0);
            qtest_writel(s, 0x57000200, 0x100020);
            g_assert_cmphex(qtest_readl(s, 0x57100c80), ==, 0);
            qtest_writel(s, 0x57100c80, 1);
            qtest_writeb(s, 0x57100c80, 0);
            qtest_writel(s, 0x57100c80, 2);
            qtest_writel(s, 0x57100c80, UINT32_MAX);
            g_assert_cmphex(qtest_readl(s, 0x57100c80), ==, enabled);
            qtest_writel(s, 0x57100c80, 0);
            g_assert_cmphex(qtest_readl(s, 0x57100c80), ==, 0);
            qtest_writel(s, 0x57070000, 1);
            g_assert_cmphex(qtest_readl(s, 0x57070000), ==, 0);
            qtest_writel(s, 0x57070000, 0);
            qtest_writel(s, 0x57070000, 2);
            qtest_writel(s, 0x57070000, UINT32_MAX);
            qtest_writeb(s, 0x57070000, 1);
            g_assert_cmphex(qtest_readl(s, 0x57070000), ==, 0);
            for (unsigned reset = 0; reset < 3; reset++) {
                qtest_writel(s, 0x57100c80, 1);
                g_assert_cmphex(qtest_readl(s, 0x57100c80), ==, enabled);
                if (reset == 0) {
                    qtest_writel(s, 0x57000200, 0x20);
                    qtest_writel(s, 0x57100c80, 1);
                } else if (reset == 1) {
                    qtest_writel(s, 0x60006290, 1U << 24);
                    qtest_writel(s, 0x57100c80, 1);
                    qtest_writel(s, 0x60006294, 1U << 24);
                } else {
                    qtest_writel(s, 0x7000e6d4, 1);
                    qtest_writel(s, 0x57100c80, 1);
                    qtest_writel(s, 0x7000e6d4, 0);
                }
                qtest_writel(s, 0x57000200, 0x100020);
                g_assert_cmphex(qtest_readl(s, 0x57100c80), ==, 0);
            }
            qtest_writel(s, 0x57100c80, 1);
            qtest_system_reset(s);
        }
        qtest_quit(s);
    }
}

static void test_gk20a_bar1_config(void)
{
    static const uint32_t config[] = {
        0, 0x804809a2, 0x20000000, 0x3fffffff, 0xbfffffff,
    };

    for (unsigned enabled = 0; enabled < 2; enabled++) {
        QTestState *s = start_mh2p(enabled ?
            "-global gk20a.experimental-engine-init=on" : "");

        for (unsigned boot = 0; boot < 2; boot++) {
            qtest_writel(s, 0x7000e6d4, 0);
            g_assert_cmphex(qtest_readl(s, 0x57001704), ==, 0);
            for (unsigned i = 0; i < ARRAY_SIZE(config); i++) {
                qtest_writel(s, 0x57001704, config[i]);
                qtest_writel(s, 0x57001704, 0x40000000);
                qtest_writel(s, 0x57001704, 0x10000000);
                qtest_writel(s, 0x57001704, UINT32_MAX);
                qtest_writeb(s, 0x57001704, 0);
                g_assert_cmphex(qtest_readl(s, 0x57001704), ==,
                                enabled ? config[i] : 0);
            }
            qtest_writel(s, 0x60006290, 1U << 24);
            qtest_writel(s, 0x57001704, 1);
            qtest_writel(s, 0x60006294, 1U << 24);
            g_assert_cmphex(qtest_readl(s, 0x57001704), ==, 0);
            qtest_writel(s, 0x57001704, 0x804809a2);
            qtest_writel(s, 0x7000e6d4, 1);
            qtest_writel(s, 0x57001704, 1);
            qtest_writel(s, 0x7000e6d4, 0);
            g_assert_cmphex(qtest_readl(s, 0x57001704), ==, 0);
            qtest_writel(s, 0x57001704, 0x804809a2);
            qtest_system_reset(s);
        }
        qtest_quit(s);
    }
}

static void test_sor_configuration(void)
{
    static const struct {
        unsigned index;
        uint32_t mask;
    } config[] = {
        { 0x13, 0x7f }, { 0x17, 0x0f000ff5 }, { 0x18, 0x03f01f00 },
        { 0x19, 0x03c60001 }, { 0x1a, 0x2000 },
        { 0x5c, UINT32_MAX }, { 0x5d, UINT32_MAX },
        { 0x60, UINT32_MAX }, { 0x61, UINT32_MAX },
    };
    const uint64_t sor = 0x54540000;

    for (unsigned enabled = 0; enabled < 2; enabled++) {
        QTestState *s = start_mh2p(enabled ?
            "-global tegra124-sor.experimental-configuration=on" : "");

        for (unsigned boot = 0; boot < 2; boot++) {
            qtest_writel(s, 0x60006288, 1U << 22);
            qtest_writel(s, sor + 0x170, 123);
            qtest_writel(s, 0x60006284, 1U << 22);
            qtest_writel(s, sor + 0x54, 0x80000001);
            g_assert_cmphex(qtest_readl(s, sor + 0x54), ==, 0);
            for (unsigned i = 0; i < ARRAY_SIZE(config); i++) {
                uint64_t address = sor + config[i].index * 4;

                g_assert_cmphex(qtest_readl(s, address), ==, 0);
                qtest_writel(s, address, config[i].mask);
                qtest_writeb(s, address, 0);
                if (config[i].mask != UINT32_MAX) {
                    qtest_writel(s, address, ~config[i].mask);
                }
                g_assert_cmphex(qtest_readl(s, address), ==,
                                enabled ? config[i].mask : 0);
            }
            qtest_writel(s, 0x60006288, 1U << 22);
            for (unsigned i = 0; i < ARRAY_SIZE(config); i++) {
                qtest_writel(s, sor + config[i].index * 4, 0);
            }
            qtest_writel(s, 0x60006284, 1U << 22);
            for (unsigned i = 0; i < ARRAY_SIZE(config); i++) {
                g_assert_cmphex(qtest_readl(s, sor + config[i].index * 4), ==,
                                enabled ? config[i].mask : 0);
            }
            qtest_writel(s, 0x60006290, 1U << 22);
            qtest_writel(s, sor + 0x170, 123);
            qtest_writel(s, 0x60006294, 1U << 22);
            for (unsigned i = 0; i < ARRAY_SIZE(config); i++) {
                g_assert_cmphex(qtest_readl(s, sor + config[i].index * 4),
                                ==, 0);
                qtest_writel(s, sor + config[i].index * 4, config[i].mask);
            }
            qtest_system_reset(s);
        }
        qtest_quit(s);
    }
}

static void host1x_queue(QTestState *s, unsigned channel, uint32_t get,
                         uint32_t put)
{
    uint64_t base = 0x50000000 + channel * 0x4000;

    qtest_writel(s, base + 0x24, 1);
    qtest_writel(s, base + 0x14, 0);
    qtest_writel(s, base + 0x20, UINT32_MAX);
    qtest_writel(s, base + 0x18, get);
    qtest_writel(s, base + 0x24, 7);
    qtest_writel(s, base + 0x18, put);
    qtest_writel(s, base + 0x24, 0);
}

static void test_host1x_dma(void)
{
    const uint32_t queue = 0x40001000, gather = 0x40002000;
    const uint64_t sync = 0x50002100;
    QTestState *s = start_mh2p("");

    for (unsigned boot = 0; boot < 2; boot++) {
        for (unsigned channel = 0; channel < 12; channel++) {
            uint64_t base = 0x50000000 + channel * 0x4000;
            unsigned head = channel % 2;
            uint64_t dc = 0x54200000 + head * 0x40000;

            g_assert_cmphex(qtest_readl(s, base + 0x24), ==, 1);
            g_assert_cmphex(qtest_readl(s, base + 0x1c), ==, 0);
            qtest_writel(s, queue, 0x60000006);
            qtest_writel(s, queue + 4, gather);
            qtest_writel(s, gather, 0xe0000006);
            qtest_writel(s, gather + 4, (0x70 + head) << 6);
            qtest_writel(s, gather + 8, 0x14090001);
            qtest_writel(s, gather + 12, 0x200040 + channel);
            qtest_writel(s, gather + 16, 0xe1000006);
            qtest_writel(s, gather + 20, 0x40000007);
            host1x_queue(s, channel, queue, queue + 8);
            g_assert_cmphex(qtest_readl(s, base + 0x1c), ==, queue + 8);
            g_assert_cmphex(qtest_readl(s, dc + 0x1024),
                           ==, 0x200040 + channel);
            g_assert_cmphex(qtest_readl(s, sync + 0x358), ==, 0);
            g_assert_cmphex(qtest_readl(s, sync + 0xf9c), ==, channel + 1);
        }

        /* Channel ownership survives CPU lock reads; CPU cannot release it. */
        qtest_writel(s, queue, 0xe0000006);
        host1x_queue(s, 8, queue, queue + 4);
        g_assert_cmphex(qtest_readl(s, sync + 0x358), ==, 0x801);
        g_assert_cmphex(qtest_readl(s, sync + 0x2d8), ==, 1);
        qtest_writel(s, sync + 0x2d8, 0);
        g_assert_cmphex(qtest_readl(s, sync + 0x358), ==, 0x801);
        host1x_queue(s, 9, queue, queue + 4);
        g_assert_cmphex(qtest_readl(s, 0x5002401c), ==, queue);
        qtest_writel(s, queue + 4, 0xe1000006);
        qtest_writel(s, 0x50020018, queue + 8);
        g_assert_cmphex(qtest_readl(s, sync + 0x358), ==, 0);

        /* A malformed gather executes its prefix only once, even if retried. */
        qtest_writel(s, gather, 1 << 6);
        qtest_writel(s, gather + 4, 0x40000009);
        qtest_writel(s, gather + 8, 0xf0000000);
        qtest_writel(s, queue, 0x60000003);
        qtest_writel(s, queue + 4, gather);
        host1x_queue(s, 0, queue, queue + 8);
        g_assert_cmphex(qtest_readl(s, sync + 0xfa4), ==, 1);
        g_assert_cmphex(qtest_readl(s, 0x5000001c), ==, queue);
        qtest_writel(s, gather + 8, 0x40000009);
        qtest_writel(s, 0x50000018, queue + 8);
        g_assert_cmphex(qtest_readl(s, sync + 0xfa4), ==, 1);
        host1x_queue(s, 0, queue, queue + 8);
        g_assert_cmphex(qtest_readl(s, sync + 0xfa4), ==, 3);

        /* Nonincrementing and masked methods, plus a satisfied HOST wait. */
        qtest_writel(s, queue, 1 << 6);
        qtest_writel(s, queue + 4, 0x20000002);
        qtest_writel(s, queue + 8, 10);
        qtest_writel(s, queue + 12, 10);
        qtest_writel(s, queue + 16, 0x30000001);
        qtest_writel(s, queue + 20, 10);
        qtest_writel(s, queue + 24, 0x10080001);
        qtest_writel(s, queue + 28, 0x0a000003);
        host1x_queue(s, 0, queue, queue + 32);
        g_assert_cmphex(qtest_readl(s, 0x5000001c), ==, queue + 32);
        g_assert_cmphex(qtest_readl(s, sync + 0xfa8), ==, 3);
        qtest_writel(s, queue + 32, 0x10080001);
        qtest_writel(s, queue + 36, 0x0a000004);
        qtest_writel(s, 0x50000018, queue + 40);
        g_assert_cmphex(qtest_readl(s, 0x5000001c), ==, queue + 32);
        qtest_system_reset(s);
    }
    qtest_quit(s);
}

static void test_host1x_wait_resume(void)
{
    const uint32_t queue = 0x40001000, gather = 0x40002000;
    const uint32_t nested = 0x40003000, signal = 0x40004000;
    const uint64_t sync = 0x50002100;
    QTestState *s = start_mh2p("");

    for (unsigned boot = 0; boot < 2; boot++) {
        for (unsigned channel = 0; channel < 12; channel++) {
            uint64_t base = 0x50000000 + channel * 0x4000;

            qtest_writel(s, sync + 0xf84, 0);
            qtest_writel(s, sync + 0xf88, 0);
            qtest_writel(s, sync + 0xfa8, 0);
            qtest_writel(s, queue, 1 << 6);
            qtest_writel(s, queue + 4, 0x4000000a);
            qtest_writel(s, queue + 8, 0x20080002);
            qtest_writel(s, queue + 12, 0x01000001);
            qtest_writel(s, queue + 16, 0x02000001);
            qtest_writel(s, queue + 20, 0x4000000a);
            host1x_queue(s, channel, queue, queue + 24);
            g_assert_cmphex(qtest_readl(s, base + 0x1c), ==, queue + 8);
            g_assert_cmphex(qtest_readl(s, sync + 0xfa8), ==, 1);

            qtest_writel(s, sync + 0xf60, 1 << 3);
            qtest_clock_step(s, 1);
            g_assert_cmphex(qtest_readl(s, sync + 0xfa8), ==, 1);
            qtest_writel(s, sync + 0xf60, 1 << 1);
            qtest_clock_step(s, 1);
            g_assert_cmphex(qtest_readl(s, base + 0x1c), ==, queue + 8);
            g_assert_cmphex(qtest_readl(s, sync + 0xfa8), ==, 1);

            /* Stopping DMA retains the partial packet and a new PUT. */
            qtest_writel(s, base + 0x24, 1);
            qtest_writel(s, sync + 0xf60, 1 << 2);
            qtest_clock_step(s, 1);
            qtest_writel(s, queue + 24, 0x4000000a);
            qtest_writel(s, base + 0x18, queue + 28);
            g_assert_cmphex(qtest_readl(s, sync + 0xfa8), ==, 1);
            qtest_writel(s, base + 0x24, 0);
            g_assert_cmphex(qtest_readl(s, base + 0x1c), ==, queue + 28);
            g_assert_cmphex(qtest_readl(s, sync + 0xfa8), ==, 3);
        }

        /* Neither a nested gather nor a masked packet may replay its prefix. */
        qtest_writel(s, sync + 0xf80, 0);
        qtest_writel(s, sync + 0xf84, 0);
        qtest_writel(s, sync + 0xf88, 0);
        qtest_writel(s, sync + 0xfa8, 0);
        qtest_writel(s, queue, 0x60000005);
        qtest_writel(s, queue + 4, gather);
        qtest_writel(s, gather, 1 << 6);
        qtest_writel(s, gather + 4, 0x4000000a);
        qtest_writel(s, gather + 8, 0x60000004);
        qtest_writel(s, gather + 12, nested);
        qtest_writel(s, gather + 16, 0x4000000a);
        qtest_writel(s, nested, 0x30000101);
        qtest_writel(s, nested + 4, 10);
        qtest_writel(s, nested + 8, 0x01000001);
        qtest_writel(s, nested + 12, 0x4000000a);
        host1x_queue(s, 0, queue, queue + 8);
        g_assert_cmphex(qtest_readl(s, 0x5000001c), ==, queue);
        g_assert_cmphex(qtest_readl(s, sync + 0xfa8), ==, 2);
        qtest_writel(s, queue + 8, 0x40080001);
        qtest_writel(s, queue + 12, 0x4000000a);
        qtest_writel(s, queue + 16, 0x00080041);
        qtest_writel(s, queue + 20, 0x02000001);
        qtest_writel(s, queue + 24, 0x4000000a);
        qtest_writel(s, 0x50000018, queue + 28);
        g_assert_cmphex(qtest_readl(s, sync + 0xfa8), ==, 2);

        qtest_writel(s, signal, 1 << 6);
        qtest_writel(s, signal + 4, 0x40000001);
        host1x_queue(s, 11, signal, signal + 8);
        qtest_clock_step(s, 1);
        g_assert_cmphex(qtest_readl(s, sync + 0xfa8), ==, 4);
        g_assert_cmphex(qtest_readl(s, 0x5000001c), ==, queue + 8);
        qtest_writel(s, 0x54200000, 0); /* Real DC syncpoint increment. */
        qtest_clock_step(s, 1);
        g_assert_cmphex(qtest_readl(s, sync + 0xfa8), ==, 5);
        g_assert_cmphex(qtest_readl(s, 0x5000001c), ==, queue + 16);
        qtest_writel(s, sync + 0xf88, 1);
        qtest_clock_step(s, 1);
        g_assert_cmphex(qtest_readl(s, sync + 0xfa8), ==, 6);
        g_assert_cmphex(qtest_readl(s, 0x5000001c), ==, queue + 28);

        /* Reset discards suspended work, including subsequent wakeups. */
        qtest_writel(s, queue, 0x00080041);
        qtest_writel(s, queue + 4, 0x01000002);
        qtest_writel(s, queue + 8, 0x4000000a);
        host1x_queue(s, 0, queue, queue + 12);
        qtest_writel(s, 0x50000024, 7);
        qtest_writel(s, 0x50000024, 0);
        qtest_writel(s, sync + 0xf84, 2);
        qtest_clock_step(s, 1);
        g_assert_cmphex(qtest_readl(s, sync + 0xfa8), ==, 6);

        /* Wrapped counters remain explicitly unsupported, not ready. */
        qtest_writel(s, sync + 0xf84, 0x1000000);
        host1x_queue(s, 0, queue, queue + 12);
        g_assert_cmphex(qtest_readl(s, 0x5000001c), ==, queue);
        qtest_writel(s, sync + 0xf84, 2);
        qtest_clock_step(s, 1);
        g_assert_cmphex(qtest_readl(s, sync + 0xfa8), ==, 6);
        qtest_writel(s, sync + 0xf84, 0);
        host1x_queue(s, 0, queue, queue + 12);
        qtest_system_reset(s);
        qtest_writel(s, sync + 0xf84, 2);
        qtest_clock_step(s, 1);
        g_assert_cmphex(qtest_readl(s, sync + 0xfa8), ==, 0);
        g_assert_cmphex(qtest_readl(s, 0x5000001c), ==, 0);
    }
    qtest_quit(s);
}

static void test_host1x_delay(void)
{
    const uint32_t queue = 0x40001000;
    const uint64_t sync = 0x50003080;
    QTestState *s = start_mh2p("");

    for (unsigned boot = 0; boot < 2; boot++) {
        for (unsigned channel = 0; channel < 12; channel++) {
            uint32_t stream = queue + channel * 32;

            qtest_writel(s, stream, 1 << 6);
            qtest_writel(s, stream + 4, 0x40100001 + channel);
            qtest_writel(s, stream + 8, 0x40000020 + channel);
            host1x_queue(s, channel, stream, stream + 12);
        }
        for (unsigned channel = 0; channel < 12; channel++) {
            qtest_clock_step(s, 999);
            g_assert_cmphex(qtest_readl(s, sync + (32 + channel) * 4), ==, 0);
            qtest_clock_step(s, 1);
            g_assert_cmphex(qtest_readl(s, sync + (32 + channel) * 4), ==, 1);
        }

        /* Two delays in one gathered packet must not replay the prefix. */
        qtest_writel(s, queue, 0x60000007);
        qtest_writel(s, queue + 4, queue + 256);
        qtest_writel(s, queue + 256, 1 << 6);
        qtest_writel(s, queue + 260, 0x4000000a);
        qtest_writel(s, queue + 264, 0x20100002);
        qtest_writel(s, queue + 268, 80);
        qtest_writel(s, queue + 272, 20);
        qtest_writel(s, queue + 276, 0x40100000);
        qtest_writel(s, queue + 280, 0x4000000a);
        host1x_queue(s, 0, queue, queue + 8);
        qtest_clock_step(s, 79999);
        g_assert_cmphex(qtest_readl(s, sync + 10 * 4), ==, 1);
        g_assert_cmphex(qtest_readl(s, 0x5000001c), ==, queue);
        qtest_clock_step(s, 1);
        qtest_clock_step(s, 19999);
        g_assert_cmphex(qtest_readl(s, sync + 10 * 4), ==, 1);
        qtest_clock_step(s, 1);
        g_assert_cmphex(qtest_readl(s, sync + 10 * 4), ==, 2);
        g_assert_cmphex(qtest_readl(s, 0x5000001c), ==, queue + 8);

        qtest_writel(s, queue, 1 << 6);
        qtest_writel(s, queue + 4, 0x10100001);
        qtest_writel(s, queue + 8, 0xfffff);
        qtest_writel(s, queue + 12, 0x4000000a);
        host1x_queue(s, 0, queue, queue + 16);
        qtest_clock_step(s, 0xfffffLL * 1000 - 1);
        g_assert_cmphex(qtest_readl(s, sync + 10 * 4), ==, 2);
        qtest_writel(s, 0x50000024, 1);
        qtest_clock_step(s, 1000000);
        g_assert_cmphex(qtest_readl(s, sync + 10 * 4), ==, 2);
        qtest_writel(s, 0x50000024, 0);
        g_assert_cmphex(qtest_readl(s, sync + 10 * 4), ==, 3);

        qtest_writel(s, queue + 8, 0x100000);
        host1x_queue(s, 0, queue, queue + 16);
        qtest_clock_step(s, 2000000000);
        g_assert_cmphex(qtest_readl(s, sync + 10 * 4), ==, 3);
        qtest_writel(s, queue + 8, 80);
        host1x_queue(s, 0, queue, queue + 16);
        qtest_writel(s, 0x50000024, 7);
        qtest_writel(s, 0x50000024, 0);
        qtest_clock_step(s, 80000);
        g_assert_cmphex(qtest_readl(s, sync + 10 * 4), ==, 3);
        host1x_queue(s, 0, queue, queue + 16);
        qtest_system_reset(s);
        qtest_clock_step(s, 80000);
        g_assert_cmphex(qtest_readl(s, sync + 10 * 4), ==, 0);
    }
    qtest_quit(s);
}

static void test_host1x_restart(void)
{
    const uint32_t queue = 0x40001000, tail = queue + 0xff0;
    const uint64_t sync = 0x50002100;
    QTestState *s = start_mh2p("");

    for (unsigned boot = 0; boot < 2; boot++) {
        for (unsigned channel = 0; channel < 12; channel++) {
            uint64_t base = 0x50000000 + channel * 0x4000;

            qtest_writel(s, sync + 0xfa4, 0);
            qtest_writel(s, tail, 1 << 6);
            qtest_writel(s, tail + 4, 0x20000000);
            qtest_writel(s, tail + 8, 0x20000000);
            qtest_writel(s, tail + 12, 0x50000000 | (queue >> 4));
            qtest_writel(s, queue, 0x40000009);
            qtest_writel(s, queue + 4, 0x40000009);
            host1x_queue(s, channel, tail, queue + 8);
            g_assert_cmphex(qtest_readl(s, base + 0x1c), ==, queue + 8);
            g_assert_cmphex(qtest_readl(s, sync + 0xfa4), ==, 2);

            /* PUT can wrap to an empty head, then publish new commands. */
            qtest_writel(s, queue + 8, 0x50000000 | (tail >> 4));
            qtest_writel(s, base + 0x18, queue);
            g_assert_cmphex(qtest_readl(s, base + 0x1c), ==, queue);
            g_assert_cmphex(qtest_readl(s, sync + 0xfa4), ==, 2);
            qtest_writel(s, base + 0x18, queue + 8);
            g_assert_cmphex(qtest_readl(s, base + 0x1c), ==, queue + 8);
            g_assert_cmphex(qtest_readl(s, sync + 0xfa4), ==, 4);

            /* A tail wait resumes once, then follows the restart to head. */
            qtest_writel(s, sync + 0xf84, 0);
            qtest_writel(s, tail + 4, 0x10080001);
            qtest_writel(s, tail + 8, 0x01000001);
            host1x_queue(s, channel, tail, queue + 8);
            g_assert_cmphex(qtest_readl(s, base + 0x1c), ==, tail + 4);
            qtest_writel(s, sync + 0xf60, 1 << 1);
            qtest_clock_step(s, 1);
            g_assert_cmphex(qtest_readl(s, base + 0x1c), ==, queue + 8);
            g_assert_cmphex(qtest_readl(s, sync + 0xfa4), ==, 6);
        }

        /* Exact native RM boundary: GET at tail, PUT at the empty head. */
        qtest_writel(s, 0x80018ff8, 0x20000000);
        qtest_writel(s, 0x80018ffc, 0x58001800);
        host1x_queue(s, 8, 0x80018ff8, 0x80018000);
        g_assert_cmphex(qtest_readl(s, 0x5002001c), ==, 0x80018000);

        /* Restart is not accepted inside a bounded gather. */
        qtest_writel(s, queue, 0x60000001);
        qtest_writel(s, queue + 4, tail);
        qtest_writel(s, tail, 0x50000000 | (queue >> 4));
        host1x_queue(s, 0, queue, queue + 8);
        g_assert_cmphex(qtest_readl(s, 0x5000001c), ==, queue);

        /* A restart target must remain in the configured channel range. */
        for (unsigned target = 0; target < 2; target++) {
            qtest_writel(s, 0x50000024, 1);
            qtest_writel(s, 0x50000014, queue);
            qtest_writel(s, 0x50000020, queue + 0x1000);
            qtest_writel(s, 0x50000018, queue);
            qtest_writel(s, 0x50000024, 7);
            qtest_writel(s, queue, 0x50000000 |
                         ((target ? queue + 0x1000 : queue - 16) >> 4));
            qtest_writel(s, 0x50000018, queue + 4);
            qtest_writel(s, 0x50000024, 0);
            g_assert_cmphex(qtest_readl(s, 0x5000001c), ==, queue);
        }

        /* Cyclic restarts hit the existing budget and require a reset. */
        qtest_writel(s, queue, 1 << 6);
        qtest_writel(s, queue + 4, 0x40000009);
        qtest_writel(s, queue + 8, 0x50000000 | (queue >> 4));
        host1x_queue(s, 0, queue, queue + 12);
        uint32_t completed = qtest_readl(s, sync + 0xfa4);
        g_assert_cmpuint(completed, >, 6);
        g_assert_cmpuint(completed, <, 65536);
        qtest_writel(s, 0x50000018, queue + 12);
        g_assert_cmphex(qtest_readl(s, sync + 0xfa4), ==, completed);
        qtest_system_reset(s);
    }
    qtest_quit(s);
}

static void test_host1x_dma_faults(void)
{
    const uint32_t queue = 0x40001000;
    static const uint32_t bad_ops[] = {
        0x70000000, 0xffffffff, 0xe2000000,
        0xe0000010, 0xe0000100, 0x60004001,
        0x10000002, /* Incomplete payload. */
    };
    QTestState *s = start_mh2p("");

    for (unsigned i = 0; i < ARRAY_SIZE(bad_ops); i++) {
        qtest_writel(s, queue, bad_ops[i]);
        host1x_queue(s, 0, queue, queue + 4);
        g_assert_cmphex(qtest_readl(s, 0x5000001c), ==, queue);
    }
    /* Recursive gathers terminate at the depth bound, not the host stack. */
    qtest_writel(s, queue, 0x60000002);
    qtest_writel(s, queue + 4, queue);
    host1x_queue(s, 0, queue, queue + 8);
    g_assert_cmphex(qtest_readl(s, 0x5000001c), ==, queue);
    qtest_writel(s, queue + 4, 0xfffffffc);
    host1x_queue(s, 0, queue, queue + 8);
    g_assert_cmphex(qtest_readl(s, 0x5000001c), ==, queue);
    host1x_queue(s, 0, 0x30000000, 0x30000004);
    g_assert_cmphex(qtest_readl(s, 0x5000001c), ==, 0x30000000);
    host1x_queue(s, 0, queue + 4, queue);
    g_assert_cmphex(qtest_readl(s, 0x5000001c), ==, queue + 4);
    qtest_memset(s, 0x80010000, 0, 65537 * 4);
    host1x_queue(s, 0, 0x80010000, 0x80010000 + 65537 * 4);
    g_assert_cmphex(qtest_readl(s, 0x5000001c),
                   ==, 0x80010000 + 65536 * 4);
    qtest_quit(s);
}

static void test_host1x_iommu(void)
{
    const uint32_t iova = 0x90000ffc, ptb = 0x80000000, ptes = 0x80001000;
    const uint64_t mc = 0x70019000, dc = 0x54200000;
    QTestState *s = start_mh2p("");

    for (unsigned boot = 0; boot < 2; boot++) {
        qtest_writel(s, mc + 0x10, 1);
        qtest_writel(s, mc + 0x250, 0x80000007);
        qtest_writel(s, mc + 0x228, 1U << 22);
        qtest_writel(s, mc + 0x1c, 7);
        qtest_writel(s, mc + 0x20, 0xe0080000);
        qtest_writel(s, ptb + (iova >> 22) * 4, 0xf0080001);
        qtest_writel(s, ptes, 0xe0080002);
        qtest_writel(s, ptes + 4, 0xe0080005);
        qtest_writel(s, 0x80002ffc, 0x70 << 6);
        qtest_writel(s, 0x80005000, 0x14090001);
        qtest_writel(s, 0x80005004, 0x200040);
        host1x_queue(s, 0, iova, iova + 12);
        g_assert_cmphex(qtest_readl(s, 0x5000001c), ==, iova + 12);
        g_assert_cmphex(qtest_readl(s, dc + 0x1024), ==, 0x200040);

        /* Permission intersection, without accidentally aliasing high PFNs. */
        for (unsigned level = 0; level < 4; level++) {
            qtest_writel(s, mc + 0x20, level == 0 ? 0x60080000 : 0xe0080000);
            qtest_writel(s, ptb + (iova >> 22) * 4,
                         level == 1 ? 0x70080001 : 0xf0080001);
            qtest_writel(s, ptes,
                         level == 2 ? 0x60080002 :
                         level == 3 ? 0xe0180002 : 0xe0080002);
            host1x_queue(s, 0, iova, iova + 4);
            g_assert_cmphex(qtest_readl(s, 0x5000001c), ==, iova);
        }
        qtest_writel(s, ptes, 0xe0080002);
        qtest_writel(s, mc + 0x250, 0x80000008);
        host1x_queue(s, 0, iova, iova + 4);
        g_assert_cmphex(qtest_readl(s, 0x5000001c), ==, iova);
        qtest_writel(s, mc + 0x250, 0x80000007);

        /* A four-megabyte section, then all three independent bypass gates. */
        qtest_writel(s, ptb + (iova >> 22) * 4, 0xe0080000);
        qtest_writel(s, 0x80003000, 0x70 << 6);
        host1x_queue(s, 0, 0x90003000, 0x90003004);
        g_assert_cmphex(qtest_readl(s, 0x5000001c), ==, 0x90003004);
        for (unsigned gate = 0; gate < 3; gate++) {
            qtest_writel(s, mc + 0x10, gate != 0);
            qtest_writel(s, mc + 0x250, gate == 1 ? 7 : 0x80000007);
            qtest_writel(s, mc + 0x228, gate == 2 ? 0 : 1U << 22);
            host1x_queue(s, 0, 0x80003000, 0x80003004);
            g_assert_cmphex(qtest_readl(s, 0x5000001c), ==, 0x80003004);
        }
        qtest_system_reset(s);
    }
    qtest_quit(s);
}

static void vic_upload(QTestState *s, uint32_t source, uint32_t destination,
                       uint32_t command)
{
    qtest_writel(s, 0x54341110, source >> 8);
    qtest_writel(s, 0x54341114, destination);
    qtest_writel(s, 0x5434111c, source & 255);
    qtest_writel(s, 0x54341118, command);
}

static void test_vic_dma(void)
{
    const uint64_t vic = 0x54340000;
    QTestState *s = start_mh2p("");

    for (unsigned boot = 0; boot < 2; boot++) {
        g_assert_cmphex(qtest_readl(s, vic + 0x110c), ==, 0);
        g_assert_cmphex(qtest_readl(s, vic + 0x1118), ==, 2);
        qtest_writel(s, 0x60006284, 1U << 18);
        qtest_writel(s, vic + 0x1604, 3);
        for (unsigned i = 0; i < 64; i++) {
            qtest_writel(s, 0x80001000 + i * 4, 0x89abcd00 + i);
        }
        vic_upload(s, 0x80001000, 0x200, 0x600);
        g_assert_cmphex(qtest_readl(s, vic + 0x1118), ==, 0x602);
        qtest_writel(s, vic + 0x11c0, 0x02000200);
        for (unsigned i = 0; i < 64; i++) {
            g_assert_cmphex(qtest_readl(s, vic + 0x11c4), ==, 0x89abcd00 + i);
        }
        g_assert_cmphex(qtest_readl(s, vic + 0x11c0), ==, 0x02000300);
        vic_upload(s, 0x80001000, 0, 0x610);
        g_assert_cmphex(qtest_readl(s, vic + 0x1118), ==, 0x612);
        qtest_writel(s, vic + 0x1180, 0x02000000);
        for (unsigned i = 0; i < 64; i++) {
            g_assert_cmphex(qtest_readl(s, vic + 0x1184), ==, 0x89abcd00 + i);
        }
        qtest_writel(s, vic + 0x1110, 0x800000);
        qtest_writel(s, vic + 0x1114, 0);
        qtest_writel(s, vic + 0x111c, 0x1000);
        qtest_writel(s, vic + 0x1118, 0x610);
        qtest_writel(s, vic + 0x1180, 0);
        g_assert_cmphex(qtest_readl(s, vic + 0x1188), ==, 0x10);

        /* Supported data sizes, with no clobber beyond the transfer. */
        for (unsigned size_code = 2; size_code <= 6; size_code++) {
            unsigned words = 1U << size_code;

            qtest_writel(s, vic + 0x11c0, 0x01000400);
            for (unsigned i = 0; i < 65; i++) {
                qtest_writel(s, vic + 0x11c4, 0xcafebabe);
            }
            vic_upload(s, 0x80001000, 0x400, size_code << 8);
            qtest_writel(s, vic + 0x11c0, 0x02000400);
            for (unsigned i = 0; i < 65; i++) {
                g_assert_cmphex(qtest_readl(s, vic + 0x11c4), ==,
                                 i < words ? 0x89abcd00 + i : 0xcafebabe);
            }
        }

        qtest_writel(s, vic + 0x11c0, 0);
        qtest_writel(s, vic + 0x11c4, 0xcafebabe);
        qtest_writel(s, 0x60006288, 1U << 18);
        vic_upload(s, 0x80001000, 0, 0x600);
        g_assert_cmphex(qtest_readl(s, vic + 0x11c4), ==, 0xcafebabe);
        qtest_writel(s, 0x60006284, 1U << 18);
        vic_upload(s, 0x80001004, 0, 0x600);
        vic_upload(s, 0x80001000, 0, 0x620);
        vic_upload(s, 0x80001000, 0, 0x1600);
        vic_upload(s, 0x80001000, 0xff80, 0x600);
        qtest_writel(s, vic + 0x1114, 0);
        qtest_writel(s, vic + 0x1110, 0x1800010);
        qtest_writel(s, vic + 0x1118, 0x600);
        qtest_writeb(s, vic + 0x11c4, 0);
        g_assert_cmphex(qtest_readl(s, vic + 0x11c4), ==, 0xcafebabe);

        /* Failed source reads cannot publish partial data or report idle. */
        vic_upload(s, 0x70000000, 0, 0x600);
        g_assert_cmphex(qtest_readl(s, vic + 0x1118) & 2, ==, 0);
        g_assert_cmphex(qtest_readl(s, vic + 0x11c4), ==, 0xcafebabe);
        vic_upload(s, 0x80001000, 0, 0x600);
        g_assert_cmphex(qtest_readl(s, vic + 0x1118) & 2, ==, 0);
        g_assert_cmphex(qtest_readl(s, vic + 0x11c4), ==, 0xcafebabe);
        qtest_writel(s, vic + 0x110c, 0);
        g_assert_cmphex(qtest_readl(s, vic + 0x1118) & 2, ==, 0);

        qtest_writel(s, vic + 0x1010, 0xfff2);
        qtest_writel(s, vic + 0x1014, 0xf000);
        g_assert_cmphex(qtest_readl(s, vic + 0x1018), ==, 0xff2);
        qtest_writel(s, vic + 0x1048, 3);
        qtest_writel(s, vic + 0x1100, 2);
        g_assert_cmphex(qtest_readl(s, vic + 0x1100), ==, 0x10);
        qtest_writel(s, 0x60006290, 1U << 18);
        qtest_writel(s, vic + 0x11c4, 0x12345678);
        qtest_writel(s, 0x60006294, 1U << 18);
        g_assert_cmphex(qtest_readl(s, vic + 0x1018), ==, 0);
        g_assert_cmphex(qtest_readl(s, vic + 0x1048), ==, 0);
        g_assert_cmphex(qtest_readl(s, vic + 0x1118), ==, 2);
        g_assert_cmphex(qtest_readl(s, vic + 0x1184), ==, 0);
        g_assert_cmphex(qtest_readl(s, vic + 0x11c4), ==, 0);
        qtest_writel(s, vic + 0x1604, 3);
        vic_upload(s, 0x80001000, 0, 0x600);
        g_assert_cmphex(qtest_readl(s, vic + 0x11c4), ==, 0x89abcd00);
        qtest_system_reset(s);
        g_assert_cmphex(qtest_readl(s, vic + 0x11c4), ==, 0);
    }
    qtest_quit(s);
}

static void test_vic_iommu(void)
{
    const uint64_t mc = 0x70019000, vic = 0x54340000;
    const uint32_t iova = 0x88001000;
    QTestState *s = start_mh2p("");

    for (unsigned boot = 0; boot < 2; boot++) {
        qtest_writel(s, 0x60006284, 1U << 18);
        qtest_writel(s, vic + 0x1604, 3);
        qtest_writel(s, mc + 0x10, 1);
        qtest_writel(s, mc + 0x284, 0x8000000b);
        qtest_writel(s, mc + 0x234, 1U << 12);
        qtest_writel(s, mc + 0x1c, 11);
        qtest_writel(s, mc + 0x20, 0xe0080000);
        qtest_writel(s, 0x80000000 + (iova >> 22) * 4, 0xf0080001);
        qtest_writel(s, 0x80001004, 0xe0080003);
        qtest_writel(s, 0x80003000, 0x12345678);
        qtest_writel(s, iova, 0xaabbccdd);
        vic_upload(s, iova, 0, 0x600);
        g_assert_cmphex(qtest_readl(s, vic + 0x11c4), ==, 0x12345678);

        /* HOST1X's client bit must not control VIC translation. */
        qtest_writel(s, mc + 0x228, 1U << 22);
        qtest_writel(s, mc + 0x234, 0);
        vic_upload(s, iova, 0, 0x600);
        g_assert_cmphex(qtest_readl(s, vic + 0x11c4), ==, 0xaabbccdd);
        qtest_writel(s, mc + 0x234, 1U << 12);
        qtest_writel(s, 0x80001004, 0x60080003);
        vic_upload(s, iova, 0, 0x600);
        g_assert_cmphex(qtest_readl(s, vic + 0x1118) & 2, ==, 0);
        g_assert_cmphex(qtest_readl(s, vic + 0x11c4), ==, 0xaabbccdd);
        qtest_system_reset(s);
    }
    qtest_quit(s);
}

static uint32_t vic_tlb(QTestState *s, uint32_t command)
{
    qtest_writel(s, 0x54341140, command);
    return qtest_readl(s, 0x54341144);
}

static void test_vic_code_paging(void)
{
    const uint64_t vic = 0x54340000;
    QTestState *s = start_mh2p("");

    for (unsigned boot = 0; boot < 2; boot++) {
        qtest_writel(s, 0x60006284, 1U << 18);
        qtest_writel(s, vic + 0x1604, 3);
        g_assert_cmphex(vic_tlb(s, 0x02000003), ==, 0);
        g_assert_cmphex(vic_tlb(s, 0x03000600), ==, 0x80000000);
        g_assert_cmphex(vic_tlb(s, 0x03000000), ==, 0x80000000);

        /* The native bootstrap maps virtual 0x600 at physical IMEM 0x300. */
        qtest_writel(s, 0x80001600, 0x01234567);
        qtest_writel(s, vic + 0x1110, 0x800010);
        qtest_writel(s, vic + 0x1114, 0x300);
        qtest_writel(s, vic + 0x111c, 0x600);
        qtest_writel(s, vic + 0x1118, 0x610);
        g_assert_cmphex(vic_tlb(s, 0x02000003), ==, 0x01000600);
        g_assert_cmphex(vic_tlb(s, 0x0300067a), ==, 0x01000003);
        g_assert_cmphex(vic_tlb(s, 0x03000300), ==, 0x80000000);
        qtest_writel(s, vic + 0x1180, 0x300);
        g_assert_cmphex(qtest_readl(s, vic + 0x1184), ==, 0x01234567);
        g_assert_cmphex(qtest_readl(s, vic + 0x1188), ==, 6);

        qtest_writel(s, vic + 0x1114, 0x400);
        qtest_writel(s, vic + 0x1118, 0x610);
        g_assert_cmphex(vic_tlb(s, 0x03000600), ==, 0x41000004);
        vic_tlb(s, 0x01000003);
        g_assert_cmphex(vic_tlb(s, 0x02000003), ==, 0);
        g_assert_cmphex(vic_tlb(s, 0x03000600), ==, 0x01000004);
        g_assert_cmphex(qtest_readl(s, vic + 0x1184), ==, 0x01234567);
        g_assert_cmphex(qtest_readl(s, vic + 0x1188), ==, 0);

        /* Invalid commands, indices and virtual tags cannot mutate the TLB. */
        g_assert_cmphex(vic_tlb(s, 0x02000100), ==, 0x01000004);
        g_assert_cmphex(vic_tlb(s, 0x01000100), ==, 0x01000004);
        g_assert_cmphex(vic_tlb(s, 0x04000004), ==, 0x01000004);
        g_assert_cmphex(qtest_readl(s, vic + 0x1140), ==, 0x03000600);
        qtest_writel(s, vic + 0x1144, 0);
        g_assert_cmphex(qtest_readl(s, vic + 0x1144), ==, 0x01000004);
        qtest_writel(s, vic + 0x111c, 0x1000600);
        qtest_writel(s, vic + 0x1118, 0x610);
        g_assert_cmphex(vic_tlb(s, 0x02000004), ==, 0x01000600);

        qtest_writel(s, vic + 0x1114, 0x300);
        qtest_writel(s, vic + 0x111c, 0x700);
        qtest_writel(s, vic + 0x1118, 0x610);
        g_assert_cmphex(vic_tlb(s, 0x03000700), ==, 0x01000003);

        /* Replacing a page with a failed load must not leave it executable. */
        qtest_writel(s, vic + 0x1110, 0x700000);
        qtest_writel(s, vic + 0x1114, 0x400);
        qtest_writel(s, vic + 0x1118, 0x610);
        g_assert_cmphex(qtest_readl(s, vic + 0x1118) & 2, ==, 0);
        g_assert_cmphex(vic_tlb(s, 0x02000004), ==, 0x02000700);
        g_assert_cmphex(vic_tlb(s, 0x03000600), ==, 0x80000000);
        g_assert_cmphex(vic_tlb(s, 0x03000700), ==, 0x43000004);
        qtest_writel(s, vic + 0x1180, 0x400);
        g_assert_cmphex(qtest_readl(s, vic + 0x1184), ==, 0x01234567);
        vic_tlb(s, 0x01000004);
        g_assert_cmphex(vic_tlb(s, 0x03000700), ==, 0x01000003);
        g_assert_cmphex(qtest_readl(s, vic + 0x1118) & 2, ==, 0);

        qtest_writel(s, 0x60006290, 1U << 18);
        vic_tlb(s, 0x03000600);
        qtest_writel(s, 0x60006294, 1U << 18);
        g_assert_cmphex(qtest_readl(s, vic + 0x1140), ==, 0);
        g_assert_cmphex(qtest_readl(s, vic + 0x1144), ==, 0);
        g_assert_cmphex(vic_tlb(s, 0x03000700), ==, 0x80000000);

        qtest_writel(s, vic + 0x1604, 3);
        qtest_writel(s, vic + 0x1110, 0x800010);
        qtest_writel(s, vic + 0x1114, 0x300);
        qtest_writel(s, vic + 0x111c, 0x600);
        qtest_writel(s, vic + 0x1118, 0x610);
        g_assert_cmphex(vic_tlb(s, 0x03000600), ==, 0x01000003);
        qtest_system_reset(s);
        g_assert_cmphex(qtest_readl(s, vic + 0x1140), ==, 0);
        g_assert_cmphex(qtest_readl(s, vic + 0x1144), ==, 0);
        g_assert_cmphex(vic_tlb(s, 0x03000600), ==, 0x80000000);
    }
    qtest_quit(s);
}

static void vic_cpu_load(QTestState *s, const uint8_t *code, size_t length)
{
    g_assert_cmpuint(length, <=, 256);
    qtest_writel(s, 0x70019010, 0);
    qtest_writel(s, 0x60006290, 1U << 18);
    qtest_writel(s, 0x60006294, 1U << 18);
    qtest_writel(s, 0x60006284, 1U << 18);
    qtest_writel(s, 0x54341604, 3);
    qtest_memset(s, 0x80000000, 0xff, 256);
    qtest_memwrite(s, 0x80000000, code, length);
    vic_upload(s, 0x80000000, 0, 0x610);
    g_assert_cmphex(qtest_readl(s, 0x54341118), ==, 0x612);
    g_assert_cmphex(qtest_readl(s, 0x54341100), ==, 0x10);
    g_assert_cmphex(qtest_readl(s, 0x54341008), ==, 0);
}

static uint32_t vic_data_word(QTestState *s, unsigned address)
{
    qtest_writel(s, 0x543411c0, address);
    return qtest_readl(s, 0x543411c4);
}

static uint32_t vic_cpu_property(QTestState *s, const char *property)
{
    g_autoptr(QDict) result = qtest_qmp(s,
        "{ 'execute': 'qom-get', 'arguments': {"
        " 'path': '/machine/vic', 'property': %s } }", property);

    g_assert_true(qdict_haskey(result, "return"));
    return qdict_get_int(result, "return");
}

static void test_vic_cpu_alu(void)
{
    static const struct {
        uint8_t op, operands, immediate;
        uint32_t input, output, flags;
    } cases[] = {
        { 0x10, 0x11, 1, 0x123456ff, 0x12345600, 0x900 },
        { 0x10, 0x11, 1, 0x1234567f, 0x12345680, 0x600 },
        { 0x52, 0x11, 1, 0x12340000, 0x1234ffff, 0x500 },
        { 0x52, 0x11, 1, 0x12348000, 0x12347fff, 0x200 },
        { 0x95, 0x11, 1, 0x80000001, 0x40000000, 0x100 },
        { 0x97, 0x11, 1, 0x80000001, 0xc0000000, 0x500 },
        { 0x54, 0x11, 1, 0xabcd8001, 0xabcd0002, 0x100 },
        { 0x95, 0x11, 0, 0, 0, 0x800 },
        { 0x30, 0x16, 0x7f, 0x12340080, 0x12340080, 0x200 },
        { 0x30, 0x14, 0xff, 0x12340080, 0x12340080, 0x100 },
        { 0x30, 0x15, 0xff, 0x12340080, 0x12340080, 0x100 },
        { 0xb0, 0x16, 0xff, 0, 0, 0x100 },
    };
    uint8_t program[] = {
        0x40, 0x00, 0x01,             /* mov r0, 0x100 */
        0xd1, 0, 0, 0, 0,            /* mov r1, patched input */
        0x10, 0x11, 0,               /* patched ALU/compare instruction */
        0xfe, 0x82, 0x01,            /* mov r2, flags */
        0xa0, 0x01,                  /* st b32 D[r0], r1 */
        0xb5, 0x02, 0x01,            /* st b32 D[r0+4], r2 */
        0xf8, 0x02,                  /* exit */
    };
    QTestState *s = start_mh2p(
        "-global tegra124-vic.experimental-execution=on");

    for (unsigned boot = 0; boot < 2; boot++) {
        for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
            stl_le_p(program + 4, cases[i].input);
            program[8] = cases[i].op;
            program[9] = cases[i].operands;
            program[10] = cases[i].immediate;
            vic_cpu_load(s, program, sizeof(program));
            g_assert_cmphex(qtest_readl(s, 0x54341108), ==, 0x10080);
            qtest_writel(s, 0x54341100, 2);
            g_assert_cmphex(qtest_readl(s, 0x54341100), ==, 0x10);
            g_assert_cmphex(qtest_readl(s, 0x54341008), ==, 0x10);
            g_assert_cmphex(vic_data_word(s, 0x100), ==, cases[i].output);
            g_assert_cmphex(vic_data_word(s, 0x104), ==, cases[i].flags);
            qtest_writel(s, 0x54341004, 0x10);
            g_assert_cmphex(qtest_readl(s, 0x54341008), ==, 0);
        }
        qtest_system_reset(s);
    }
    qtest_quit(s);
}

static void test_vic_cpu_control(void)
{
    uint8_t indirect[] = {
        0xd0, 9, 0, 0, 0,            /* mov patched register, routine */
        0xf9, 0x05,                  /* call patched register */
        0xf8, 0x02,                  /* exit */
        0x40, 0x00, 0x01,            /* routine: mov r0, 0x100 */
        0x01, 0x5a,                  /* mov r1, 0x5a */
        0xa0, 0x01,                  /* st b32 D[r0], r1 */
        0xf8, 0x00,                  /* ret */
    };
    static const uint8_t program[] = {
        0x40, 0x00, 0x01,             /* mov r0, 0x100 */
        0x01, 0x00, 0x02, 0x03,       /* mov r1, 0; mov r2, 3 */
        0x10, 0x11, 0x01,             /* loop: add b8 r1, r1, 1 */
        0x26, 0x12,                   /* cmp b8 r1, r2 */
        0xf4, 0x08, 0xfb,             /* bra below, loop (current PC - 5) */
        0x7e, 0x1a, 0x00, 0x00,       /* lcall routine */
        0xa0, 0x01,                   /* st b32 D[r0], r1 */
        0xb5, 0x03, 0x01,             /* st b32 D[r0+4], r3 */
        0xf8, 0x02,                   /* exit */
        0x10, 0x11, 0x05,             /* routine: add b8 r1, r1, 5 */
        0xfe, 0x43, 0x01,             /* mov r3, sp */
        0xf8, 0x00,                   /* ret */
    };
    static const uint8_t continuation[] = {
        0x12,                        /* final byte of mov r4, 0x1234 */
        0x40, 0x00, 0x01,            /* mov r0, 0x100 */
        0xa0, 0x04,                  /* st b32 D[r0], r4 */
        0xf8, 0x02,                  /* exit */
    };
    uint8_t boundary[256] = { 0 };
    QTestState *s = start_mh2p(
        "-global tegra124-vic.experimental-execution=on");

    for (unsigned boot = 0; boot < 2; boot++) {
        vic_cpu_load(s, program, sizeof(program));
        qtest_writel(s, 0x60006288, 1U << 18);
        qtest_writel(s, 0x54341100, 2);
        g_assert_cmphex(qtest_readl(s, 0x54341008), ==, 0);
        qtest_writel(s, 0x60006284, 1U << 18);
        qtest_writel(s, 0x54341100, 2);
        g_assert_cmphex(qtest_readl(s, 0x54341008), ==, 0x10);
        g_assert_cmphex(vic_data_word(s, 0x100), ==, 8);
        g_assert_cmphex(vic_data_word(s, 0x104), ==, 0x7ffc);
        g_assert_cmphex(vic_data_word(s, 0x7ffc), ==, 19);

        for (unsigned reg = 0; reg < 16; reg++) {
            indirect[0] = 0xd0 | reg;
            indirect[6] = (reg << 4) | 5;
            stl_le_p(indirect + 1, 9);
            vic_cpu_load(s, indirect, sizeof(indirect));
            qtest_writel(s, 0x54341100, 2);
            g_assert_cmphex(qtest_readl(s, 0x54341008), ==, 0x10);
            g_assert_cmphex(vic_data_word(s, 0x100), ==, 0x5a);
            g_assert_cmphex(vic_data_word(s, 0x7ffc), ==, 7);
            g_assert_cmphex(vic_cpu_property(s, "cpu-sp"), ==, 0);
            g_assert_cmphex(vic_cpu_property(s, "cpu-pc"), ==, 9);
        }
        for (unsigned i = 0; i < 2; i++) {
            uint32_t target = i ? 0x1000000 : 0x100;

            stl_le_p(indirect + 1, target);
            vic_cpu_load(s, indirect, sizeof(indirect));
            qtest_writel(s, 0x54341100, 2);
            g_assert_cmphex(qtest_readl(s, 0x54341008), ==, 0);
            g_assert_cmphex(vic_cpu_property(s, "cpu-pc"), ==, target);
            g_assert_cmphex(vic_cpu_property(s, "cpu-sp"), ==, 0x7ffc);
            g_assert_cmphex(vic_data_word(s, 0x7ffc), ==, 7);
        }

        boundary[254] = 0xf8;
        boundary[255] = 2;
        vic_cpu_load(s, boundary, sizeof(boundary));
        qtest_writel(s, 0x54341104, 254);
        qtest_writel(s, 0x54341100, 2);
        g_assert_cmphex(qtest_readl(s, 0x54341008), ==, 0x10);
        qtest_writel(s, 0x543411c0, 0x8000);
        g_assert_cmphex(qtest_readl(s, 0x543411c0), ==, 0);

        boundary[254] = 0x44;
        boundary[255] = 0x34;
        vic_cpu_load(s, boundary, sizeof(boundary));
        qtest_writel(s, 0x54341104, 254);
        qtest_writel(s, 0x54341100, 2);
        g_assert_cmphex(qtest_readl(s, 0x54341100), ==, 0);
        g_assert_cmphex(qtest_readl(s, 0x54341008), ==, 0);

        vic_cpu_load(s, boundary, sizeof(boundary));
        qtest_memwrite(s, 0x80000100, continuation, sizeof(continuation));
        qtest_writel(s, 0x54341114, 0x500);
        qtest_writel(s, 0x5434111c, 0x100);
        qtest_writel(s, 0x54341118, 0x610);
        qtest_writel(s, 0x54341104, 254);
        qtest_writel(s, 0x54341100, 2);
        g_assert_cmphex(qtest_readl(s, 0x54341008), ==, 0x10);
        g_assert_cmphex(vic_data_word(s, 0x100), ==, 0x1234);
        qtest_system_reset(s);
    }
    qtest_quit(s);
}

static void test_vic_cpu_multistack(void)
{
    static const unsigned last_regs[] = { 0, 1, 15 };
    static const uint8_t prefix[] = {
        0x0f, 4, 0xfe, 0xf4, 0,       /* mov r15, 4; mov sp, r15 */
        0xf4, 0x31, 8,                /* bset flags, carry */
    };
    static const uint8_t tail[] = {
        0xfe, 0x70, 1, 0xb5, 0xf0, 15, /* save restored r15 from xdbase */
        0xfe, 0x40, 1, 0xb5, 0xf0, 16, /* save sp */
        0xfe, 0x80, 1, 0xb5, 0xf0, 17, /* save flags */
        0xf8, 2,
    };
    static const struct {
        unsigned op;
        int adjustment;
    } pops[] = {
        { 1, 0 }, { 2, 256 }, { 2, -16 }, { 3, 256 }, { 3, -16 },
        { 4, 8 }, { 4, -16 }, { 5, 8 }, { 5, -16 },
    };
    static const uint8_t save[] = {
        0x4f, 0, 1,                   /* mov r15, 0x100 */
        0xb5, 0xf0, 0, 0xb5, 0xf1, 1, 0xb5, 0xf2, 2,
        0xfe, 0x43, 1, 0xb5, 0xf3, 3, /* save sp */
        0xf8, 2,
    };
    QTestState *s = start_mh2p(
        "-global tegra124-vic.experimental-execution=on");

    for (unsigned boot = 0; boot < 2; boot++) {
        for (unsigned c = 0; c < ARRAY_SIZE(last_regs); c++) {
            uint8_t code[256];
            unsigned length = sizeof(prefix), last = last_regs[c];

            memcpy(code, prefix, sizeof(prefix));
            for (unsigned i = 0; i < 16; i++) {
                code[length++] = 0xd0 | i;
                stl_le_p(code + length, 0x11110000 + i);
                length += 4;
            }
            code[length++] = 0xf9;
            code[length++] = (last << 4) | 2; /* mpush rLast */
            for (unsigned i = 0; i <= last; i++) {
                code[length++] = 0xbd;
                code[length++] = (i << 4) | 4; /* clear saved registers */
            }
            code[length++] = 0xfb;
            code[length++] = last << 4;       /* mpop rLast */
            memcpy(code + length, (uint8_t[]) {
                0xfe, 0xf7, 0, 0x4f, 0, 1,   /* save r15; output base */
            }, 6);
            length += 6;
            for (unsigned i = 0; i < 15; i++) {
                code[length++] = 0xb5;
                code[length++] = 0xf0 | i;
                code[length++] = i;
            }
            memcpy(code + length, tail, sizeof(tail));
            length += sizeof(tail);
            vic_cpu_load(s, code, length);
            qtest_writel(s, 0x54341100, 2);
            g_assert_cmphex(qtest_readl(s, 0x54341008), ==, 0x10);
            for (unsigned i = 0; i < 16; i++) {
                g_assert_cmphex(vic_data_word(s, 0x100 + 4 * i), ==,
                                0x11110000 + i);
            }
            for (unsigned i = 0; i <= last; i++) {
                g_assert_cmphex(vic_data_word(s, (0U - 4 * i) & 0x7ffc), ==,
                                0x11110000 + i);
            }
            g_assert_cmphex(vic_data_word(s, 0x140), ==, 4);
            g_assert_cmphex(vic_data_word(s, 0x144), ==, 0x100);
        }
        for (unsigned i = 0; i < ARRAY_SIZE(pops); i++) {
            uint8_t code[64] = { 0xfb, 0x20 | pops[i].op };
            unsigned length = pops[i].op >= 4 ? 3 : pops[i].op >= 2 ? 4 : 2;
            uint32_t sp = (12 + pops[i].adjustment) & 0x7ffc;

            code[2] = pops[i].adjustment;
            code[3] = (uint16_t)pops[i].adjustment >> 8;
            memcpy(code + length, (uint8_t[]) { 0x3e, 0x20, 0, 0 }, 4);
            memcpy(code + 32, save, sizeof(save));
            vic_cpu_load(s, code, sizeof(code));
            qtest_writel(s, 0x543411c0, 0x01000000);
            qtest_writel(s, 0x543411c4, 0x33333333);
            qtest_writel(s, 0x543411c4, 0x22222222);
            qtest_writel(s, 0x543411c4, 0x11111111);
            if (pops[i].op & 1) {
                qtest_writel(s, 0x543411c0, sp);
                qtest_writel(s, 0x543411c4, 32);
                sp = (sp + 4) & 0x7ffc;
            }
            qtest_writel(s, 0x54341100, 2);
            g_assert_cmphex(qtest_readl(s, 0x54341008), ==, 0x10);
            g_assert_cmphex(vic_data_word(s, 0x100), ==, 0x11111111);
            g_assert_cmphex(vic_data_word(s, 0x104), ==, 0x22222222);
            g_assert_cmphex(vic_data_word(s, 0x108), ==, 0x33333333);
            g_assert_cmphex(vic_data_word(s, 0x10c), ==, sp);
        }
        qtest_system_reset(s);
    }
    qtest_quit(s);
}

static void test_vic_cpu_predicates(void)
{
    static const uint8_t bits[] = {
        0x40, 0, 1,                   /* mov r0, 0x100 */
        0x48, 1, 7, 0xfe, 0x88, 0,    /* flags = C | O | S | p0 */
        0x02, 0xff,                   /* mov r2, -1 */
        0xf0, 0x2c, 0, 0xa0, 0x02,    /* xbit p0; save result */
        0xfe, 0x83, 1, 0xb5, 0x03, 1, /* save flags */
        0xf0, 0x2c, 1, 0xb5, 0x02, 2, /* xbit p1; save result */
        0xfe, 0x83, 1, 0xb5, 0x03, 3, /* save flags */
        0x06, 43, 0xfe, 0x62, 12,     /* xbit flags[43 & 31] */
        0xb5, 0x02, 4,
        0xd5, 0, 0, 0, 0x80,          /* mov r5, 0x80000000 */
        0xc8, 0x52, 255, 0xb5, 0x02, 5,
        0x06, 63, 0xff, 0x56, 0x28,   /* xbit r2, r5, r6 */
        0xb5, 0x02, 6, 0xf8, 2,
    };
    static const uint32_t expected[] = { 1, 0x301, 0, 0xb01, 1, 1, 1 };
    uint8_t sleep[] = {
        0x40, 0, 1, 0x01, 1, 0xa0, 0x01, /* D[0x100] = 1 */
        0xf4, 0x31, 0,                /* bset p0 (patched to bclr) */
        0xf4, 0x28, 0,                /* sleep p0 */
        0x01, 2, 0xa0, 0x01, 0xf8, 2, /* D[0x100] = 2; exit */
    };
    QTestState *s = start_mh2p(
        "-global tegra124-vic.experimental-execution=on");

    for (unsigned boot = 0; boot < 2; boot++) {
        vic_cpu_load(s, bits, sizeof(bits));
        qtest_writel(s, 0x54341100, 2);
        g_assert_cmphex(qtest_readl(s, 0x54341008), ==, 0x10);
        for (unsigned i = 0; i < ARRAY_SIZE(expected); i++) {
            g_assert_cmphex(vic_data_word(s, 0x100 + 4 * i), ==, expected[i]);
        }
        sleep[8] = 0x31;
        vic_cpu_load(s, sleep, sizeof(sleep));
        qtest_writel(s, 0x54341100, 2);
        g_assert_cmphex(qtest_readl(s, 0x54341100), ==, 0x20);
        g_assert_cmphex(qtest_readl(s, 0x5434104c), ==, 0);
        g_assert_cmphex(vic_cpu_property(s, "cpu-pc"), ==, 10);
        g_assert_cmphex(qtest_readl(s, 0x54341008), ==, 0);
        g_assert_cmphex(vic_data_word(s, 0x100), ==, 1);
        qtest_clock_step(s, 1000000000);
        qtest_writel(s, 0x54341100, 2);
        g_assert_cmphex(qtest_readl(s, 0x54341100), ==, 0x20);
        g_assert_cmphex(vic_data_word(s, 0x100), ==, 1);
        g_assert_cmphex(vic_cpu_property(s, "cpu-pc"), ==, 10);
        qtest_writel(s, 0x54341000, 1);
        g_assert_cmphex(qtest_readl(s, 0x54341008), ==, 1);
        g_assert_cmphex(qtest_readl(s, 0x54341100), ==, 0x20);
        qtest_system_reset(s);
        g_assert_cmphex(qtest_readl(s, 0x54341100), ==, 0x10);
        g_assert_cmphex(vic_cpu_property(s, "cpu-pc"), ==, 0);
        g_assert_cmphex(vic_cpu_property(s, "cpu-sp"), ==, 0);

        sleep[8] = 0x32;
        vic_cpu_load(s, sleep, sizeof(sleep));
        qtest_writel(s, 0x54341100, 2);
        g_assert_cmphex(qtest_readl(s, 0x54341100), ==, 0x10);
        g_assert_cmphex(qtest_readl(s, 0x54341008), ==, 0x10);
        g_assert_cmphex(vic_data_word(s, 0x100), ==, 2);
        qtest_system_reset(s);
        g_assert_cmphex(qtest_readl(s, 0x54341100), ==, 0x10);
        g_assert_cmphex(vic_cpu_property(s, "cpu-pc"), ==, 0);
        g_assert_cmphex(vic_cpu_property(s, "cpu-sp"), ==, 0);
    }
    qtest_quit(s);
}

static void test_vic_method_fifo(void)
{
    static const uint8_t program[] = { 0xf8, 2 };
    QTestState *s = start_mh2p(
        "-global tegra124-vic.experimental-execution=on");

    qtest_irq_intercept_out_named(s, "/machine/vic", "sysbus-irq");
    for (unsigned boot = 0; boot < 2; boot++) {
        vic_cpu_load(s, program, sizeof(program));
        g_assert_cmphex(qtest_readl(s, 0x5434100c), ==, 0xfc04);
        qtest_writel(s, 0x54340044, 1);
        g_assert_cmphex(qtest_readl(s, 0x54341008), ==, 0);
        qtest_writel(s, 0x54341048, 2);
        for (unsigned i = 0; i < 16; i++) {
            qtest_writel(s, 0x54340040, 0x80 + i);
            qtest_writel(s, 0x54340044, 0x12340000 + i);
        }
        qtest_writel(s, 0x54340040, 0x7ff);
        qtest_writel(s, 0x54340044, 0xdeadbeef);
        qtest_writel(s, 0x54341004, 4);
        g_assert_cmphex(qtest_readl(s, 0x54341008), ==, 4);
        qtest_writel(s, 0x54341074, 2);
        for (unsigned i = 0; i < 16; i++) {
            g_assert_cmphex(qtest_readl(s, 0x54341068), ==, 0x80 + i);
            g_assert_cmphex(qtest_readl(s, 0x54341064), ==, 0x12340000 + i);
            qtest_writel(s, 0x54341074, 1);
            g_assert_cmphex(qtest_readl(s, 0x54341008), ==,
                           i == 15 ? 0 : 4);
        }
        qtest_writel(s, 0x54341000, 4);
        g_assert_cmphex(qtest_readl(s, 0x54341008), ==, 0);
        qtest_writel(s, 0x54340040, 0x800);
        qtest_writel(s, 0x54340044, 0x55);
        g_assert_cmphex(qtest_readl(s, 0x54341068), ==, 0x7ff);
        g_assert_cmphex(qtest_readl(s, 0x54341064), ==, 0x55);
        qtest_writel(s, 0x54341074, 1);
        qtest_writel(s, 0x5434101c, 0x40);
        qtest_writel(s, 0x54341000, 0x40);
        g_assert_cmphex(qtest_readl(s, 0x54341008), ==, 0x40);
        g_assert_false(qtest_get_irq(s, 0));
        qtest_writel(s, 0x54341010, 0x40);
        g_assert_true(qtest_get_irq(s, 0));
        qtest_writel(s, 0x54341014, 0x40);
        g_assert_false(qtest_get_irq(s, 0));
        qtest_writel(s, 0x54341010, 0x40);
        g_assert_true(qtest_get_irq(s, 0));
        qtest_writel(s, 0x54341004, 0x40);
        g_assert_false(qtest_get_irq(s, 0));
        qtest_writel(s, 0x54341000, 0x40);
        g_assert_true(qtest_get_irq(s, 0));
        qtest_writel(s, 0x54340044, 0x77);
        qtest_system_reset(s);
        g_assert_false(qtest_get_irq(s, 0));
        g_assert_cmphex(qtest_readl(s, 0x54341008), ==, 0);
        g_assert_cmphex(qtest_readl(s, 0x54341048), ==, 0);
        g_assert_cmphex(qtest_readl(s, 0x5434104c), ==, 0);
    }
    qtest_quit(s);
}

static void test_vic_cpu_interrupts(void)
{
    uint8_t program[] = {
        0x41, 0x00, 0x04,             /* mov r1, 0x400 */
        0xfe, 0x14, 0x00,             /* mov sp, r1 */
        0x01, 0x32,                   /* mov r1, handler */
        0xfe, 0x10, 0x00,             /* mov iv0, r1 */
        0xfe, 0x11, 0x00,             /* mov iv1, r1 */
        0x40, 0x00, 0x02,             /* mov r0, 0x200 */
        0x08, 0x00,                   /* mov r8, 0 */
        0x49, 0x00, 0x01,             /* mov r9, 0x100 */
        0x81, 0x01, 0x00, 0x07,       /* mov r1, IE0|IE1|IE2|p0 */
        0xfe, 0x18, 0x00,             /* mov flags, r1 */
        0xf4, 0x28, 0x00,             /* 0x1d: sleep p0 */
        0xb6, 0x80, 0x01,             /* add b32 r8, 1 */
        0xa0, 0x98,                   /* st b32 D[r9], r8 */
        0xfe, 0x8a, 0x01,             /* mov r10, flags */
        0xb5, 0x9a, 0x01,             /* st b32 D[r9+4], r10 */
        0xf4, 0x31, 0x00,             /* bset p0 */
        0x3e, 0x1d, 0x00, 0x00,       /* lbra sleep */
        0xfe, 0x8f, 0x01,             /* 0x32: mov r15, flags */
        0xa0, 0x0f,                   /* st b32 D[r0], r15 */
        0xfe, 0x41, 0x01,             /* mov r1, sp */
        0xb5, 0x01, 0x01,             /* st b32 D[r0+4], r1 */
        0x98, 0x12, 0x00,             /* ld b32 r2, D[r1] */
        0xb5, 0x02, 0x02,             /* st b32 D[r0+8], r2 */
        0x43, 0x00, 0x19,             /* mov r3, FIFO_DATA */
        0xcf, 0x34, 0x00,             /* iord r4, I[r3] */
        0xb5, 0x04, 0x03,             /* st b32 D[r0+12], r4 */
        0x43, 0x00, 0x1a,             /* mov r3, FIFO_CMD */
        0xcf, 0x34, 0x00,             /* iord r4, I[r3] */
        0xb5, 0x04, 0x04,             /* st b32 D[r0+16], r4 */
        0xb6, 0x00, 0x20,             /* add b32 r0, 0x20 */
        0x43, 0x00, 0x1d,             /* mov r3, FIFO_ACK */
        0x04, 0x01,                   /* mov r4, 1 */
        0xf6, 0x34, 0x00,             /* iowr I[r3], r4 */
        0xfe, 0xf8, 0x00,             /* mov flags, r15 */
        0xf4, 0x32, 0x00,             /* bclr p0 */
        0xf8, 0x01,                   /* iret */
    };
    QTestState *s = start_mh2p(
        "-global tegra124-vic.experimental-execution=on");

    for (unsigned vector = 0; vector < 2; vector++) {
        vic_cpu_load(s, program, sizeof(program));
        qtest_writel(s, 0x54341048, 2);
        qtest_writel(s, 0x5434101c, vector ? 4 << 16 : 0);
        qtest_writel(s, 0x54341100, 2);
        g_assert_cmphex(vic_cpu_property(s, "cpu-pc"), ==, 0x1d);
        for (unsigned i = 0; i < 2; i++) {
            qtest_writel(s, 0x54340040, 0x80 + i);
            qtest_writel(s, 0x54340044, 0x12340000 + i);
        }
        g_assert_cmphex(qtest_readl(s, 0x54341008), ==, 4);
        g_assert_cmphex(vic_data_word(s, 0x100), ==, 0);
        g_assert_cmphex(vic_cpu_property(s, "cpu-pc"), ==, 0x1d);
        qtest_writel(s, 0x54340000, 0x112);
        g_assert_cmphex(qtest_readl(s, 0x500030c8), ==, 0);
        qtest_writel(s, 0x60006288, 1U << 18);
        qtest_writel(s, 0x54341010, 4);
        g_assert_cmphex(vic_data_word(s, 0x100), ==, 0);
        g_assert_cmphex(qtest_readl(s, 0x500030c8), ==, 0);
        qtest_writel(s, 0x60006284, 1U << 18);
        for (unsigned i = 0; i < 2; i++) {
            unsigned record = 0x200 + i * 0x20;

            g_assert_cmphex(vic_data_word(s, record), ==,
                           i ? 0x700000 : 0x700001);
            g_assert_cmphex(vic_data_word(s, record + 4), ==, 0x3fc);
            g_assert_cmphex(vic_data_word(s, record + 8), ==, 0x1d);
            g_assert_cmphex(vic_data_word(s, record + 12), ==, 0x12340000 + i);
            g_assert_cmphex(vic_data_word(s, record + 16), ==, 0x80 + i);
        }
        g_assert_cmphex(qtest_readl(s, 0x54341008), ==, 0);
        g_assert_cmphex(qtest_readl(s, 0x54341100), ==, 0x20);
        g_assert_cmphex(vic_cpu_property(s, "cpu-pc"), ==, 0x1d);
        g_assert_cmphex(vic_cpu_property(s, "cpu-sp"), ==, 0x400);
        g_assert_cmphex(vic_data_word(s, 0x100), ==, 1);
        g_assert_cmphex(vic_data_word(s, 0x104), ==, 0x770000);
        g_assert_cmphex(qtest_readl(s, 0x500030c8), ==, 1);
        qtest_writel(s, 0x54340044, 0xabcdef);
        g_assert_cmphex(vic_data_word(s, 0x24c), ==, 0xabcdef);
        g_assert_cmphex(vic_data_word(s, 0x100), ==, 2);
        qtest_system_reset(s);
        g_assert_cmphex(vic_cpu_property(s, "cpu-sp"), ==, 0);
    }

    program[sizeof(program) - 4] = 0x31;
    vic_cpu_load(s, program, sizeof(program));
    qtest_writel(s, 0x54341048, 2);
    qtest_writel(s, 0x54341010, 4);
    qtest_writel(s, 0x54340040, 0x80);
    qtest_writel(s, 0x54340044, 0x42);
    qtest_writel(s, 0x54341100, 2);
    g_assert_cmphex(vic_data_word(s, 0x20c), ==, 0x42);
    g_assert_cmphex(vic_data_word(s, 0x100), ==, 0);
    g_assert_cmphex(vic_cpu_property(s, "cpu-sp"), ==, 0x400);
    g_assert_cmphex(vic_cpu_property(s, "cpu-pc"), ==, 0x1d);
    g_assert_cmphex(qtest_readl(s, 0x54341100), ==, 0x20);

    {
        uint8_t sleeping_handler[sizeof(program) + 1];

        memcpy(sleeping_handler, program, sizeof(program));
        sleeping_handler[sizeof(program) - 2] = 0xf4;
        sleeping_handler[sizeof(program) - 1] = 0x28;
        sleeping_handler[sizeof(program)] = 0;
        vic_cpu_load(s, sleeping_handler, sizeof(sleeping_handler));
        qtest_writel(s, 0x54341048, 2);
        qtest_writel(s, 0x54341010, 4);
        qtest_writel(s, 0x54341100, 2);
        qtest_writel(s, 0x54340044, 0x42);
        g_assert_cmphex(qtest_readl(s, 0x54341008), ==, 0);
        g_assert_cmphex(qtest_readl(s, 0x54341100), ==, 0x20);
        g_assert_cmphex(vic_cpu_property(s, "cpu-sp"), ==, 0x3fc);
        qtest_writel(s, 0x54340000, 0x112);
        qtest_clock_step(s, 1000000000);
        g_assert_cmphex(qtest_readl(s, 0x500030c8), ==, 0);
    }

    program[0x32] = 0xf8;
    program[0x33] = 0x01; /* iret without acknowledging the level interrupt */
    vic_cpu_load(s, program, sizeof(program));
    qtest_writel(s, 0x54341048, 2);
    qtest_writel(s, 0x54341010, 4);
    qtest_writel(s, 0x54341100, 2);
    qtest_writel(s, 0x54340044, 0x11);
    g_assert_cmphex(qtest_readl(s, 0x54341100), ==, 0);
    g_assert_cmphex(vic_cpu_property(s, "cpu-pc"), ==, 0x1d);
    g_assert_cmphex(vic_cpu_property(s, "cpu-sp"), ==, 0x400);
    g_assert_cmphex(qtest_readl(s, 0x54341008), ==, 4);

    program[0x32] = 0xf4;
    program[0x33] = 0x31;
    program[0x34] = 16; /* re-enable vector zero inside its unacked handler */
    vic_cpu_load(s, program, sizeof(program));
    qtest_writel(s, 0x54341048, 2);
    qtest_writel(s, 0x54341010, 4);
    qtest_writel(s, 0x54341100, 2);
    qtest_writel(s, 0x54340044, 0x11);
    g_assert_cmphex(qtest_readl(s, 0x54341100), ==, 0);
    g_assert_cmphex(vic_cpu_property(s, "cpu-pc"), ==, 0x35);
    g_assert_cmphex(vic_cpu_property(s, "cpu-sp"), ==, 0x3fc);
    g_assert_cmphex(qtest_readl(s, 0x54341008), ==, 4);

    program[0x32] = 0xf8;
    program[0x33] = 0x08;
    vic_cpu_load(s, program, sizeof(program));
    qtest_writel(s, 0x54341048, 2);
    qtest_writel(s, 0x54341010, 4);
    qtest_writel(s, 0x54340044, 0x11);
    qtest_writel(s, 0x54340000, 0x112);
    qtest_writel(s, 0x54341100, 2);
    g_assert_cmphex(vic_cpu_property(s, "cpu-pc"), ==, 0x32);
    g_assert_cmphex(qtest_readl(s, 0x54341100), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x500030c8), ==, 0);
    qtest_writel(s, 0x54340044, 0x22);
    g_assert_cmphex(qtest_readl(s, 0x54341064), ==, 0x11);
    qtest_writel(s, 0x54341074, 1);
    g_assert_cmphex(qtest_readl(s, 0x54341008), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x500030c8), ==, 0);
    qtest_system_reset(s);
    qtest_quit(s);
}

static void test_vic_host1x_methods(void)
{
    static const uint8_t program[] = { 0xf8, 2 };
    static const uint32_t packet[] = {
        0x00001740,
        0x10100002, 0x80, 1,
        0x10100002, 0x1c7, 0x3b4,
        0x10100002, 0x1cb, 0x80017b,
        0x40000112,
    };
    const uint32_t queue = 0x40001000;

    for (unsigned enabled = 0; enabled < 2; enabled++) {
        QTestState *s = start_mh2p(enabled ?
            "-global tegra124-vic.experimental-execution=on" : "");

        for (unsigned boot = 0; boot < 2; boot++) {
            if (enabled) {
                vic_cpu_load(s, program, sizeof(program));
            } else {
                qtest_writel(s, 0x60006284, 1U << 18);
                qtest_writel(s, 0x54340000, 18);
                qtest_writel(s, 0x54340000, 0x112);
                g_assert_cmphex(qtest_readl(s, 0x500030c8), ==, 0);
            }
            qtest_writel(s, 0x54341048, 2);
            for (unsigned i = 0; i < ARRAY_SIZE(packet); i++) {
                qtest_writel(s, queue + i * 4, packet[i]);
            }
            host1x_queue(s, 0, queue, queue + sizeof(packet));
            g_assert_cmphex(qtest_readl(s, 0x5000001c), ==,
                           queue + (enabled ? sizeof(packet) : 4));
            g_assert_cmphex(qtest_readl(s, 0x500030c8), ==, 0);
            if (enabled) {
                for (unsigned i = 0; i < 3; i++) {
                    g_assert_cmphex(qtest_readl(s, 0x54341068), ==,
                                   packet[2 + i * 3]);
                    g_assert_cmphex(qtest_readl(s, 0x54341064), ==,
                                   packet[3 + i * 3]);
                    qtest_writel(s, 0x54341074, 1);
                }
                g_assert_cmphex(qtest_readl(s, 0x54341008), ==, 0);
            }
            g_assert_cmphex(qtest_readl(s, 0x500030c8), ==, 0);
            qtest_system_reset(s);
        }
        qtest_quit(s);
    }
}

static void test_vic_syncpts(void)
{
    static const uint8_t program[] = {
        0x40, 0, 1,                  /* mov r0, 0x100 */
        0xd1, 0x78, 0x56, 0x34, 0x12, /* mov r1, 0x12345678 */
        0xa0, 0x01,                  /* st b32 D[r0], r1 */
        0xf4, 0x31, 0,               /* bset p0 */
        0xf4, 0x28, 0,               /* sleep p0 */
    };
    const uint64_t counter = 0x500030c8;
    QTestState *s = start_mh2p(
        "-global tegra124-vic.experimental-execution=on");

    qtest_irq_intercept_out_named(s, "/machine/host1x", "sysbus-irq");
    for (unsigned boot = 0; boot < 2; boot++) {
        vic_cpu_load(s, program, sizeof(program));
        qtest_writel(s, 0x500034c8, 9);
        qtest_writel(s, 0x50003020, 1U << 18);
        for (unsigned i = 0; i < 16; i++) {
            qtest_writel(s, 0x54340000, 0x112 + (i & 1));
        }
        qtest_writel(s, 0x54340000, 0x114); /* queue full */
        g_assert_cmphex(qtest_readl(s, counter), ==, 0);
        g_assert_cmphex(qtest_readl(s, counter + 4), ==, 0);
        g_assert_false(qtest_get_irq(s, 0));
        qtest_writel(s, 0x54340000, 18); /* immediate, independent condition */
        g_assert_cmphex(qtest_readl(s, counter), ==, 1);
        g_assert_false(qtest_get_irq(s, 0));
        g_assert_cmphex(vic_data_word(s, 0x100), ==, 0);
        qtest_writel(s, 0x54341100, 2);
        g_assert_cmphex(vic_data_word(s, 0x100), ==, 0x12345678);
        g_assert_cmphex(qtest_readl(s, counter), ==, 9);
        g_assert_cmphex(qtest_readl(s, counter + 4), ==, 8);
        g_assert_cmphex(qtest_readl(s, counter + 8), ==, 0);
        g_assert_true(qtest_get_irq(s, 0));
        qtest_writel(s, 0x50003000, 1U << 18);
        g_assert_false(qtest_get_irq(s, 0));

        qtest_writel(s, 0x54340000, 0x212);
        qtest_writel(s, 0x54340000, 0x10012);
        qtest_writel(s, 0x54340000, 0xc0);
        qtest_writel(s, 0x54340000, 0x1c0);
        qtest_writel(s, 0x60006288, 1U << 18);
        qtest_writel(s, 0x54340000, 0x112);
        qtest_writel(s, 0x60006284, 1U << 18);
        g_assert_cmphex(qtest_readl(s, counter), ==, 9);
        qtest_writel(s, 0x54340000, 0x1bf);
        g_assert_cmphex(qtest_readl(s, 0x5000337c), ==, 1);

        qtest_writel(s, 0x54341000, 1);
        qtest_writel(s, 0x54340000, 0x112);
        g_assert_cmphex(qtest_readl(s, counter), ==, 9);
        qtest_writel(s, 0x54341004, 1);
        g_assert_cmphex(qtest_readl(s, counter), ==, 10);
        qtest_writel(s, 0x54341000, 1);
        qtest_writel(s, 0x54340000, 0x112);
        qtest_writel(s, 0x60006288, 1U << 18);
        qtest_writel(s, 0x54341004, 1);
        g_assert_cmphex(qtest_readl(s, counter), ==, 10);
        qtest_writel(s, 0x60006284, 1U << 18);
        g_assert_cmphex(qtest_readl(s, counter), ==, 11);

        qtest_writel(s, 0x54341000, 1);
        qtest_writel(s, 0x54340000, 0x112);
        vic_cpu_load(s, program, sizeof(program));
        qtest_writel(s, 0x54341100, 2);
        g_assert_cmphex(qtest_readl(s, counter), ==, 11);
        qtest_writel(s, 0x54341000, 1);
        qtest_writel(s, 0x54340000, 0x112);
        vic_upload(s, 0x10000000, 0, 0x600);
        qtest_writel(s, 0x54341004, 1);
        qtest_writel(s, 0x54340000, 0x112);
        g_assert_cmphex(qtest_readl(s, counter), ==, 11);
        qtest_system_reset(s);
        g_assert_cmphex(qtest_readl(s, counter), ==, 0);
    }
    qtest_quit(s);
}

static void test_vic_cpu_method_ops(void)
{
    static const unsigned branches[] = { 0, 4, 9, 10, 11, 13, 14, 15 };
    uint8_t indexed[] = {
        0x40, 0x00, 0x01,             /* mov r0, 0x100 */
        0x01, 0x03,                   /* mov r1, 3 */
        0xd2, 0xef, 0xbe, 0xad, 0xde, /* mov r2, 0xdeadbeef */
        0x43, 0x00, 0x0f,             /* mov r3, 0xf00 */
        0xfe, 0x38, 0x00,             /* mov flags, r3 */
        0xbc, 0x02, 0x19,             /* st b32 D[r0+r1*4], r2 */
        0xbc, 0x01, 0x48,             /* ld b32 r4, D[r0+r1*4] */
        0xb5, 0x04, 0x08,             /* st b32 D[r0+32], r4 */
        0xfe, 0x85, 0x01,             /* mov r5, flags */
        0xb5, 0x05, 0x09,             /* st b32 D[r0+36], r5 */
        0xf8, 0x02,
    };
    QTestState *s = start_mh2p(
        "-global tegra124-vic.experimental-execution=on");

    for (unsigned size = 0; size < 3; size++) {
        uint32_t mask = UINT32_MAX >> (32 - (8U << size));
        uint32_t value = 0xdeadbeef & mask;
        unsigned offset = 3U << size;

        indexed[16] = indexed[19] = 0x3c | (size << 6);
        vic_cpu_load(s, indexed, sizeof(indexed));
        qtest_writel(s, 0x54341100, 2);
        g_assert_cmphex(qtest_readl(s, 0x54341100), ==, 0x10);
        g_assert_cmphex(vic_data_word(s, 0x120), ==, value);
        g_assert_cmphex(vic_data_word(s, 0x124), ==, 0xf00);
        g_assert_cmphex(vic_data_word(s, 0x100 + (offset & ~3U)), ==,
                       value << ((offset & 3) * 8));

        for (unsigned form = 0; form < ARRAY_SIZE(branches); form++) {
            unsigned sub = branches[form];
            unsigned length = sub & 8 ? 5 + ((sub & 3) == 3) : 4;
            unsigned displacement = 3 + !!(sub & 2);
            unsigned immediate = sub & 2 ? 0x1234 : 0x34;

            for (unsigned equal = 0; equal < 2; equal++) {
                uint8_t code[32] = {
                    0x40, 0x00, 0x01, /* mov r0, 0x100 */
                    0xd1, 0, 0, 0, 0, /* mov r1, operand */
                    0x02, 0,          /* mov r2, 0 */
                };
                static const uint8_t end[] = {
                    0x02, 0x11,       /* mov r2, 0x11 */
                    0xa0, 0x02,       /* st b32 D[r0], r2 */
                    0xfe, 0x83, 1,    /* mov r3, flags */
                    0xb5, 0x03, 1,    /* st b32 D[r0+4], r3 */
                    0xf8, 2,
                };
                uint32_t operand = immediate + !equal;
                bool taken = equal ^ !!(sub & 4);

                operand |= 0xa5000000 & ~mask;
                for (unsigned byte = 0; byte < 4; byte++) {
                    code[4 + byte] = operand >> (byte * 8);
                }
                code[10] = 0x33 | (size << 6);
                code[11] = 0x10 | sub;
                code[12] = immediate;
                if (sub & 2) {
                    code[13] = immediate >> 8;
                }
                code[10 + displacement] = length + 2;
                memcpy(code + 10 + length, end, sizeof(end));
                vic_cpu_load(s, code, 10 + length + sizeof(end));
                qtest_writel(s, 0x54341100, 2);
                g_assert_cmphex(qtest_readl(s, 0x54341100), ==, 0x10);
                g_assert_cmphex(vic_data_word(s, 0x100), ==,
                               taken ? 0 : 0x11);
                g_assert_cmphex(vic_data_word(s, 0x104), ==, 0);
            }
        }
    }
    for (unsigned form = 0; form < ARRAY_SIZE(branches); form++) {
        unsigned sub = branches[form];
        unsigned length = sub & 8 ? 5 + ((sub & 3) == 3) : 4;
        unsigned displacement = 3 + !!(sub & 2);
        unsigned immediate = sub & 2 ? 0x1234 : 0x34;
        uint32_t operand = immediate + (sub & 4 ? 2 : 1);
        uint8_t code[32] = {
            0x40, 0, 1,              /* mov r0, 0x100 */
            0xd1, 0, 0, 0, 0,        /* mov r1, operand */
            0xb6, 0x12, 1,           /* sub b32 r1, 1 */
        };
        static const uint8_t end[] = { 0xa0, 0x01, 0xf8, 2 };

        for (unsigned byte = 0; byte < 4; byte++) {
            code[4 + byte] = operand >> (byte * 8);
        }
        code[11] = 0xb3;
        code[12] = 0x10 | sub;
        code[13] = immediate;
        if (sub & 2) {
            code[14] = immediate >> 8;
        }
        code[11 + displacement] = 0xfd;
        if (length - displacement == 2) {
            code[12 + displacement] = 0xff;
        }
        memcpy(code + 11 + length, end, sizeof(end));
        vic_cpu_load(s, code, 11 + length + sizeof(end));
        qtest_writel(s, 0x54341100, 2);
        g_assert_cmphex(qtest_readl(s, 0x54341100), ==, 0x10);
        g_assert_cmphex(vic_data_word(s, 0x100), ==,
                       immediate - !(sub & 4));
    }
    indexed[1] = 0xfc;
    indexed[2] = 0x7f;
    vic_cpu_load(s, indexed, sizeof(indexed));
    qtest_writel(s, 0x54341100, 2);
    g_assert_cmphex(qtest_readl(s, 0x54341100), ==, 0);
    g_assert_cmphex(vic_data_word(s, 0x7ffc), ==, 0);

    for (unsigned form = 0; form < 2; form++) {
        for (unsigned op = 9; op <= 11; op++) {
            uint8_t code[] = {
                0x40, 0, 1,              /* mov r0, 0x100 */
                0xd1, 1, 0, 0, 0x80,     /* mov r1, 0x80000001 */
                0x02, 63,                /* mov r2, 63 */
                0x43, 0, 0x0f,           /* mov r3, 0xf00 */
                0xfe, 0x38, 0,           /* mov flags, r3 */
                0, 0, 0,                 /* bset/bclr/btgl */
                0xa0, 0x01,              /* st b32 D[r0], r1 */
                0xfe, 0x83, 1,           /* mov r3, flags */
                0xb5, 0x03, 1,           /* st b32 D[r0+4], r3 */
                0xf8, 2,
            };

            code[16] = form ? 0xfd : 0xf0;
            code[17] = form ? 0x12 : 0x10 | op;
            code[18] = form ? op : 63;
            vic_cpu_load(s, code, sizeof(code));
            qtest_writel(s, 0x54341100, 2);
            g_assert_cmphex(qtest_readl(s, 0x54341100), ==, 0x10);
            g_assert_cmphex(vic_data_word(s, 0x100), ==,
                           op == 9 ? 0x80000001 : 1);
            g_assert_cmphex(vic_data_word(s, 0x104), ==, 0xf00);
        }
    }
    for (unsigned form = 0; form < 2; form++) {
        for (unsigned op = 9; op <= 11; op++) {
            uint8_t code[24] = {
                0x40, 0, 1,              /* mov r0, 0x100 */
                0x01, 49,                /* mov r1, 49 */
                0x82, 0, 0x0f, 2,        /* mov r2, IE1|0xf00 */
                0xfe, 0x28, 0,           /* mov flags, r2 */
            };
            static const uint8_t end[] = {
                0xfe, 0x83, 1,           /* mov r3, flags */
                0xa0, 0x03,              /* st b32 D[r0], r3 */
                0xf8, 2,
            };

            code[12] = form ? 0xf4 : 0xf9;
            code[13] = form ? op + 0x28 : 0x10 | op;
            code[14] = 49;
            memcpy(code + 14 + form, end, sizeof(end));
            vic_cpu_load(s, code, 14 + form + sizeof(end));
            qtest_writel(s, 0x54341100, 2);
            g_assert_cmphex(qtest_readl(s, 0x54341100), ==, 0x10);
            g_assert_cmphex(vic_data_word(s, 0x100), ==,
                           op == 9 ? 0x20f00 : 0xf00);
        }
    }
    qtest_system_reset(s);
    qtest_quit(s);
}

static void test_vic_cpu_io(void)
{
    static const uint8_t program[] = {
        0x49, 0x00, 0x42,             /* mov r9, 0x4200 */
        0xcf, 0x99, 0x00,             /* iord r9, I[r9] (HWCFG) */
        0xb6, 0x95, 0x01,             /* shr b32 r9, 1 */
        0xf1, 0x94, 0x00, 0xff,       /* and r9, 0xff00 */
        0xfe, 0x94, 0x00,             /* mov sp, r9 */
        0x40, 0x00, 0x10,             /* mov r0, 0x1000 */
        0xd1, 0x10, 0x32, 0x54, 0x76, /* mov r1, 0x76543210 */
        0xf6, 0x01, 0x00,             /* iowr I[r0], r1 */
        0xcf, 0x02, 0x00,             /* iord r2, I[r0] */
        0x43, 0x00, 0x01,             /* mov r3, 0x100 */
        0xa0, 0x32,                   /* st b32 D[r3], r2 */
        0xfe, 0x44, 0x01,             /* mov r4, sp */
        0xb5, 0x34, 0x01,             /* st b32 D[r3+4], r4 */
        0xf8, 0x02,                   /* exit */
    };
    QTestState *s = start_mh2p(
        "-global tegra124-vic.experimental-execution=on");

    for (unsigned boot = 0; boot < 2; boot++) {
        vic_cpu_load(s, program, sizeof(program));
        g_assert_cmphex(qtest_readl(s, 0x54341040), ==, 0);
        qtest_writel(s, 0x54341100, 2);
        g_assert_cmphex(qtest_readl(s, 0x54341008), ==, 0x10);
        g_assert_cmphex(qtest_readl(s, 0x54341040), ==, 0x76543210);
        g_assert_cmphex(vic_data_word(s, 0x100), ==, 0x76543210);
        g_assert_cmphex(vic_data_word(s, 0x104), ==, 0);
        qtest_writel(s, 0x54341038, 1);
        g_assert_cmphex(qtest_readl(s, 0x54341038), ==, 0);
        qtest_system_reset(s);
        g_assert_cmphex(qtest_readl(s, 0x54341040), ==, 0);
    }
    qtest_quit(s);
}

static void test_vic_cpu_dma(void)
{
    static const uint8_t program[] = {
        0x40, 0x00, 0x01,             /* mov r0, 0x100 */
        0xd1, 0x10, 0x00, 0x90, 0x00, /* mov r1, 0x900010 */
        0xfe, 0x16, 0x00,             /* mov xcbase, r1 */
        0x42, 0x00, 0x06,             /* mov r2, 0x600 */
        0x43, 0x00, 0x03,             /* mov r3, 0x300 */
        0xfa, 0x23, 0x04,             /* xcld r2, r3 */
        0xf8, 0x07,                   /* xcwait */
        0x7e, 0x00, 0x06, 0x00,       /* lcall virtual 0x600 */
        0xa0, 0x04,                   /* st b32 D[r0], r4 */
        0xd7, 0x20, 0x00, 0x90, 0x00, /* mov r7, 0x900020 */
        0xfe, 0x77, 0x00,             /* mov xdbase, r7 */
        0x45, 0x00, 0x01,             /* mov r5, 0x100 */
        0x86, 0x00, 0x02, 0x06,       /* mov r6, 0x60200 */
        0xfa, 0x56, 0x05,             /* xdld r5, r6 */
        0xf8, 0x03,                   /* xdwait */
        0x46, 0x00, 0x02,             /* mov r6, 0x200 */
        0x98, 0x65, 0x00,             /* ld b32 r5, D[r6] */
        0xb5, 0x05, 0x01,             /* st b32 D[r0+4], r5 */
        0xf8, 0x02,                   /* exit */
    };
    static const uint8_t routine[] = {
        0xd4, 0x78, 0x56, 0x34, 0x12, /* mov r4, 0x12345678 */
        0xf8, 0x00,                   /* ret */
    };
    QTestState *s = start_mh2p(
        "-global tegra124-vic.experimental-execution=on");

    for (unsigned boot = 0; boot < 2; boot++) {
        vic_cpu_load(s, program, sizeof(program));
        qtest_memwrite(s, 0x80001600, routine, sizeof(routine));
        qtest_writel(s, 0x80002100, 0xfeedface);
        qtest_writel(s, 0x70019010, 1);
        qtest_writel(s, 0x70019284, 0x8000000b);
        qtest_writel(s, 0x70019234, 1U << 12);
        qtest_writel(s, 0x7001901c, 11);
        qtest_writel(s, 0x70019020, 0xe0080004);
        qtest_writel(s, 0x80004900, 0xe0080000);
        qtest_writel(s, 0x54341100, 2);
        g_assert_cmphex(qtest_readl(s, 0x54341008), ==, 0x10);
        g_assert_cmphex(vic_data_word(s, 0x100), ==, 0x12345678);
        g_assert_cmphex(vic_data_word(s, 0x104), ==, 0xfeedface);
        g_assert_cmphex(vic_tlb(s, 0x02000003), ==, 0x01000600);
        g_assert_cmphex(qtest_readl(s, 0x54341110), ==, 0x800000);
        g_assert_cmphex(qtest_readl(s, 0x54341114), ==, 0);
        g_assert_cmphex(qtest_readl(s, 0x5434111c), ==, 0);
        g_assert_cmphex(qtest_readl(s, 0x54341118), ==, 0x612);

        vic_cpu_load(s, program, sizeof(program));
        qtest_writel(s, 0x70019010, 1);
        qtest_writel(s, 0x80004900, 0x60080000);
        qtest_writel(s, 0x54341100, 2);
        g_assert_cmphex(qtest_readl(s, 0x54341100), ==, 0);
        g_assert_cmphex(qtest_readl(s, 0x54341008), ==, 0);
        g_assert_cmphex(vic_tlb(s, 0x02000003), ==, 0x02000600);
        g_assert_cmphex(qtest_readl(s, 0x54341118) & 2, ==, 0);
        g_assert_cmphex(vic_data_word(s, 0x100), ==, 0);
        qtest_system_reset(s);
    }
    qtest_quit(s);
}

static void test_vic_cpu_faults(void)
{
    static const uint8_t programs[][16] = {
        { 0xf8, 0x01 },                          /* iret outside interrupt */
        { 0x00, 31, 0xf9, 0x09 },                /* unsupported flag bit */
        { 0xfb, 0x06 },                          /* invalid multi-pop */
        { 0x40, 0, 0x13, 0xcf, 0x01, 0, 0xf8, 2 }, /* busy idle-state read */
        { 0x3e, 0x00, 0x01, 0x00 },              /* unmapped code page */
        { 0xd1, 0x00, 0x80, 0, 0, 0xbf, 0x10 }, /* out-of-range data */
        { 0x41, 0x01, 0, 0xbf, 0x10 },          /* unaligned data */
        { 0x40, 0x00, 0x40, 0x01, 2,
          0xfa, 0x01, 0 },                       /* recursive CPU start */
        { 0x40, 0, 1, 0x01, 0, 0x02, 0,
          0x90, 0x11, 1, 0xa0, 0x01,
          0x3e, 7, 0, 0 },                       /* bounded counter loop */
    };
    static const uint8_t exit_program[] = { 0xf8, 2 };
    QTestState *s = start_mh2p(
        "-global tegra124-vic.experimental-execution=on");

    for (unsigned boot = 0; boot < 2; boot++) {
        for (unsigned i = 0; i < ARRAY_SIZE(programs); i++) {
            vic_cpu_load(s, programs[i], sizeof(programs[i]));
            qtest_writel(s, 0x54341100, 2);
            g_assert_cmphex(qtest_readl(s, 0x54341100), ==, 0);
            g_assert_cmphex(qtest_readl(s, 0x54341008), ==, 0);
            if (i == ARRAY_SIZE(programs) - 1) {
                /* Three setup steps; the final loop step is add, not store. */
                g_assert_cmpuint(vic_data_word(s, 0x100), ==, 21844);
            }
            qtest_memwrite(s, 0x80000000, exit_program, sizeof(exit_program));
            vic_upload(s, 0x80000000, 0, 0x610);
            qtest_writel(s, 0x54341100, 2);
            g_assert_cmphex(qtest_readl(s, 0x54341008), ==, 0);
        }
        vic_cpu_load(s, exit_program, sizeof(exit_program));
        vic_upload(s, 0x80000000, 0x100, 0x610);
        qtest_writel(s, 0x54341100, 2);
        g_assert_cmphex(qtest_readl(s, 0x54341100), ==, 0);
        g_assert_cmphex(qtest_readl(s, 0x54341008), ==, 0);
        vic_cpu_load(s, exit_program, sizeof(exit_program));
        qtest_writel(s, 0x54341100, 2);
        g_assert_cmphex(qtest_readl(s, 0x54341008), ==, 0x10);
        qtest_system_reset(s);
    }
    qtest_quit(s);
}

static void dc_test_mode(QTestState *s, unsigned head, unsigned parent)
{
    uint64_t dc = 0x54200000 + head * 0x40000;

    qtest_writel(s, 0x60006320, 1U << (27 - head));
    qtest_writel(s, 0x60006138 + head * 4, parent << 29);
    qtest_writel(s, dc + 0x101c, 0x80010);
    qtest_writel(s, dc + 0x1020, 0xa000a);
    qtest_writel(s, dc + 0x1024, 0x200040);
    qtest_writel(s, dc + 0x1028, 0xa000a);
    qtest_writel(s, dc + 0x10b8, 0);
    qtest_writel(s, dc + 0xc8, 0x20);
    qtest_writel(s, dc + 0x104, 0x101);
}

static GBytes *dc_read_pixels(QTestState *s, unsigned head, unsigned width,
                              unsigned height)
{
    g_autofree char *filename = NULL, *contents = NULL;
    g_autofree char *device = g_strdup_printf("display%u", head);
    g_autofree char *header = g_strdup_printf("P6\n%u %u\n255\n",
                                            width, height);
    g_autoptr(GError) error = NULL;
    size_t size, prefix = strlen(header);
    unsigned stride = ROUND_UP(width * 3, 4);
    uint8_t *pixels;
    int fd = g_file_open_tmp("mh2p-frame-XXXXXX", &filename, &error);

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    qtest_qmp_assert_success(s, "{'execute':'screendump', 'arguments':"
                            "{'filename':%s, 'device':%s}}", filename, device);
    g_assert_true(g_file_get_contents(filename, &contents, &size, &error));
    g_assert_no_error(error);
    unlink(filename);
    /* QMP's PPM writer includes the pixman line-buffer padding. */
    g_assert_cmpuint(size, ==, prefix + stride * height);
    g_assert_cmpmem(contents, prefix, header, prefix);
    pixels = g_malloc(width * height * 3);
    for (unsigned y = 0; y < height; y++) {
        memcpy(pixels + y * width * 3, contents + prefix + y * stride,
               width * 3);
    }
    return g_bytes_new_take(pixels, width * height * 3);
}

static void assert_dc_pixels(QTestState *s, unsigned head, unsigned width,
                             unsigned height, uint32_t rgb)
{
    g_autoptr(GBytes) image = dc_read_pixels(s, head, width, height);
    size_t size;
    const uint8_t *pixels = g_bytes_get_data(image, &size);

    for (size_t i = 0; i < size; i += 3) {
        g_assert_cmphex(pixels[i], ==, (rgb >> 16) & 255);
        g_assert_cmphex(pixels[i + 1], ==, (rgb >> 8) & 255);
        g_assert_cmphex(pixels[i + 2], ==, rgb & 255);
    }
}

static void test_dc_window_alpha(void)
{
    const uint64_t dc = 0x54200000;
    g_autofree uint8_t *data = g_malloc(256 * 256 * 4);
    QTestState *s = start_mh2p(
        "-global tegra124-dc.experimental-window-scanout=on");

    qtest_writel(s, 0x600060d0, 0x40305001);
    qtest_writel(s, 0x600060dc, 0x40000000);
    dc_test_mode(s, 0, 2);
    qtest_writel(s, dc + 0x1024, 0x1000100);
    qtest_writel(s, dc + 0x108, 1 << 4);
    qtest_writel(s, dc + 0x1c00, 1U << 30);
    qtest_writel(s, dc + 0x1c14, 0x1000100);
    qtest_writel(s, dc + 0x1c18, 0x1000400);
    qtest_writel(s, dc + 0x1c24, 0x10001000);
    qtest_writel(s, dc + 0x1c28, 1024);
    qtest_writel(s, dc + 0x2000, 0x80010000);
    qtest_writel(s, dc + 0x1c58, 0x00ffff01);
    qtest_writel(s, dc + 0x1c5c, 0x262);
    qtest_writel(s, dc + 0x1c60, 0x224);
    qtest_writel(s, dc + 0x1c64, 0xff00);

    for (unsigned depth = 12; depth <= 13; depth++) {
        for (unsigned alpha = 0; alpha < 256; alpha++) {
            for (unsigned x = 0; x < 256; x++) {
                uint32_t rgb = x | ((255 - x) << 8) | ((x ^ alpha) << 16);
                uint32_t p = depth == 12 ? rgb : bswap32(rgb) >> 8;

                stl_le_p(data + (alpha * 256 + x) * 4,
                         (alpha << 24) | p);
            }
        }
        stl_le_p(data, 0);
        qtest_memwrite(s, 0x80010000, data, 256 * 256 * 4);
        qtest_writel(s, dc + 0x1c0c, depth);
        for (unsigned bg = 0; bg <= 256; bg++) {
            uint32_t background = bg == 256 ? 0 :
                (bg << 16) | (((bg * 173) & 255) << 8) | (255 - bg);
            g_autoptr(GBytes) image = NULL;
            const uint8_t *pixels;

            qtest_writel(s, dc + 0x1390, bswap32(background) >> 8);
            qtest_writel(s, dc + 0x104, 0x303);
            qtest_clock_step(s, 20000000);
            image = dc_read_pixels(s, 0, 256, 256);
            pixels = g_bytes_get_data(image, NULL);
            for (unsigned alpha = 0; alpha < 256; alpha++) {
                for (unsigned x = 0; x < 256; x++) {
                    uint32_t rgb = x | ((255 - x) << 8) |
                                   ((x ^ alpha) << 16);
                    unsigned at = (alpha * 256 + x) * 3;

                    if (!alpha && !x) {
                        rgb = 0;
                    }
                    for (unsigned channel = 0; channel < 3; channel++) {
                        unsigned shift = (2 - channel) * 8;
                        unsigned expected = ((rgb >> shift) & 255) +
                            (((background >> shift) & 255) * (255 - alpha) +
                             127) / 255;

                        g_assert_cmpuint(pixels[at + channel], ==,
                                         MIN(expected, 255));
                    }
                }
            }
        }
    }
    qtest_quit(s);
}

static unsigned dc_blend_channel(unsigned src, unsigned dst, unsigned alpha)
{
    return MIN(255, src + (dst * (255 - alpha) + 127) / 255);
}

static void test_dc_two_windows_mode(bool tiled)
{
    static const unsigned gob_x[] = {
        0, 4, 8, 12, 32, 36, 40, 44,
        256, 260, 264, 268, 288, 292, 296, 300,
    };
    static const unsigned gob_y[] = { 0, 16, 64, 80, 128, 144, 192, 208 };
    const uint64_t dc = 0x54200000;
    const unsigned width = 17, height = 9, pitch = 128;
    uint32_t colors[2][9][17];
    uint8_t data[2][2048];
    QTestState *s = start_mh2p(
        "-global tegra124-dc.experimental-window-scanout=on");

    qtest_writel(s, 0x600060d0, 0x40305001);
    qtest_writel(s, 0x600060dc, 0x40000000);
    dc_test_mode(s, 0, 2);
    qtest_writel(s, dc + 0x1024, (height << 16) | width);
    qtest_writel(s, dc + 0x1390, 0x785a3c);
    for (unsigned layer = 0; layer < 2; layer++) {
        unsigned window = layer ? 2 : 0;

        memset(data[layer], 0, sizeof(data[layer]));
        for (unsigned y = 0; y < height; y++) {
            for (unsigned x = 0; x < width; x++) {
                unsigned alpha = (x * 17 + y * 31 + layer * 89) & 255;
                unsigned red = (x * 13 + layer * 47) & 255;
                unsigned green = (y * 29 + layer * 33) & 255;
                unsigned blue = (x * 19 + y * 7 + layer * 23) & 255;
                uint32_t rgb = red << 16 | green << 8 | blue;
                uint32_t p = layer ? bswap32(rgb) >> 8 : rgb;

                colors[layer][y][x] = alpha << 24 | rgb;
                unsigned offset = tiled ?
                    (y / 8) * pitch * 8 + (x / 16) * 512 +
                    gob_y[y % 8] + gob_x[x % 16] : y * pitch + x * 4;

                stl_le_p(data[layer] + offset, alpha << 24 | p);
            }
        }
        qtest_memwrite(s, 0x80010000 + layer * 0x10000,
                       data[layer], sizeof(data[layer]));
        qtest_writel(s, dc + 0x108, 1U << (4 + window));
        qtest_writel(s, dc + 0x1c00, 1U << 30);
        qtest_writel(s, dc + 0x1c0c, layer ? 13 : 12);
        qtest_writel(s, dc + 0x1c14, (height << 16) | width);
        qtest_writel(s, dc + 0x1c18, (height << 16) | (width * 4));
        qtest_writel(s, dc + 0x1c24, 0x10001000);
        qtest_writel(s, dc + 0x1c28, pitch);
        qtest_writel(s, dc + 0x2000, 0x80010000 + layer * 0x10000);
        qtest_writel(s, dc + 0x202c, tiled ? 2 : 0);
        qtest_writel(s, dc + 0x1c5c, 0x262);
        qtest_writel(s, dc + 0x1c60, 0x224);
        qtest_writel(s, dc + 0x1c64, 0xff00);
    }
    for (unsigned top = 0; top < 2; top++) {
        g_autoptr(GBytes) image = NULL;
        const uint8_t *pixels;

        for (unsigned layer = 0; layer < 2; layer++) {
            unsigned window = layer ? 2 : 0;

            qtest_writel(s, dc + 0x108, 1U << (4 + window));
            qtest_writel(s, dc + 0x1c58,
                         0x00ffff00 | (layer == top ? 1 : 3));
        }
        qtest_writel(s, dc + 0x104, 0xb0b);
        qtest_clock_step(s, 400000);
        image = dc_read_pixels(s, 0, width, height);
        pixels = g_bytes_get_data(image, NULL);
        for (unsigned y = 0; y < height; y++) {
            for (unsigned x = 0; x < width; x++) {
                uint32_t lower = colors[1 - top][y][x];
                uint32_t upper = colors[top][y][x];
                unsigned at = (y * width + x) * 3;
                const unsigned bg[] = { 60, 90, 120 };

                for (unsigned c = 0; c < 3; c++) {
                    unsigned shift = 16 - 8 * c;
                    unsigned bottom = dc_blend_channel((lower >> shift) & 255,
                                                       bg[c], lower >> 24);
                    unsigned expected = dc_blend_channel((upper >> shift) & 255,
                                                         bottom, upper >> 24);

                    g_assert_cmpuint(pixels[at + c], ==, expected);
                }
            }
        }
    }
    {
        g_autoptr(GBytes) expected = dc_read_pixels(s, 0, width, height);
        g_autoptr(GBytes) actual = NULL;
        size_t expected_size, actual_size;
        const void *expected_data = g_bytes_get_data(expected, &expected_size);
        const void *actual_data;

        qtest_writel(s, dc + 0x108, 1U << 6);
        qtest_writel(s, dc + 0x1c58, 0x00ffff03);
        qtest_writel(s, dc + 0x104, 0x808);
        qtest_clock_step(s, 400000);
        actual = dc_read_pixels(s, 0, width, height);
        actual_data = g_bytes_get_data(actual, &actual_size);
        g_assert_cmpmem(actual_data, actual_size, expected_data, expected_size);
        g_clear_pointer(&actual, g_bytes_unref);

        qtest_writel(s, dc + 0x108, 1U << 4);
        qtest_writel(s, dc + 0x1c58, 0x01000000);
        qtest_writel(s, dc + 0x104, 0x202);
        qtest_clock_step(s, 400000);
        actual = dc_read_pixels(s, 0, width, height);
        actual_data = g_bytes_get_data(actual, &actual_size);
        g_assert_cmpmem(actual_data, actual_size, expected_data, expected_size);
        g_clear_pointer(&actual, g_bytes_unref);

        qtest_writel(s, dc + 0x108, 1U << 4);
        qtest_writel(s, dc + 0x1c58, 0x00ffff03);
        qtest_writel(s, dc + 0x108, 1U << 6);
        qtest_writel(s, dc + 0x1c58, 0x00ffff01);
        qtest_writel(s, dc + 0x108, 1U << 5);
        qtest_writel(s, dc + 0x1c00, 1U << 30);
        qtest_writel(s, dc + 0x1c0c, 12);
        qtest_writel(s, dc + 0x1c14, (height << 16) | width);
        qtest_writel(s, dc + 0x1c18, (height << 16) | (width * 4));
        qtest_writel(s, dc + 0x1c24, 0x10001000);
        qtest_writel(s, dc + 0x1c28, pitch);
        qtest_writel(s, dc + 0x2000, 0x80010000);
        qtest_writel(s, dc + 0x202c, tiled ? 2 : 0);
        qtest_writel(s, dc + 0x1c58, 0x00ffff05);
        qtest_writel(s, dc + 0x1c5c, 0x262);
        qtest_writel(s, dc + 0x1c60, 0x224);
        qtest_writel(s, dc + 0x1c64, 0xff00);
        qtest_writel(s, dc + 0x104, 0xf0f);
        qtest_clock_step(s, 400000);
        actual = dc_read_pixels(s, 0, width, height);
        actual_data = g_bytes_get_data(actual, &actual_size);
        g_assert_cmpmem(actual_data, actual_size, expected_data, expected_size);
    }
    qtest_quit(s);
}

static void test_dc_two_windows(void)
{
    test_dc_two_windows_mode(false);
    test_dc_two_windows_mode(true);
}

static void test_dc_window_scanout_size(unsigned width, unsigned height,
                                        bool swap_depth)
{
    static const unsigned gob_x[] = {
        0, 4, 8, 12, 32, 36, 40, 44,
        256, 260, 264, 268, 288, 292, 296, 300,
    };
    static const unsigned gob_y[] = { 0, 16, 64, 80, 128, 144, 192, 208 };
    const uint64_t mc = 0x70019000;
    const uint32_t iova = 0x90000000, physical = 0x80010000;
    QTestState *s = start_mh2p(
        "-global tegra124-dc.experimental-window-scanout=on");

    for (unsigned head = 0; head < 2; head++) {
        uint64_t dc = 0x54200000 + head * 0x40000;
        uint32_t ptb = 0x80000000 + head * 8192, ptes = ptb + 4096;

        for (unsigned window = 0; window < 3; window++) {
            for (unsigned mode = 0; mode < 7; mode++) {
                unsigned block = mode ? 1U << (mode - 1) : 0;
                unsigned rows = block ? ROUND_UP(height, block * 8) : height;
                unsigned bytes = 256 * rows;
                unsigned depth = 12 + ((mode + swap_depth) & 1);
                /* Unaligned pitch-linear bases make rows straddle pages. */
                unsigned offset = block ? 0 : 0x7c4;
                unsigned extent = ROUND_UP(offset + bytes, 4096);
                g_autofree uint8_t *data = g_malloc0(bytes);
                g_autofree uint8_t *memory = g_malloc0(extent);
                g_autoptr(GBytes) image = NULL;
                const uint8_t *pixels;

                if (!block) {
                    for (unsigned y = 0; y < height; y++) {
                        for (unsigned x = 0; x < 64; x++) {
                            stl_le_p(data + y * 256 + x * 4,
                                     0xff000000 | x | (y << 8) |
                                     ((x ^ y) << 16));
                        }
                    }
                } else {
                    unsigned gob = 0;

                    for (unsigned by = 0; by < rows; by += block * 8) {
                        for (unsigned bx = 0; bx < 4; bx++) {
                            for (unsigned gy = 0; gy < block; gy++, gob++) {
                                for (unsigned y = 0; y < 8; y++) {
                                    for (unsigned x = 0; x < 16; x++) {
                                        unsigned px = bx * 16 + x;
                                        unsigned py = by + gy * 8 + y;

                                        stl_le_p(data + gob * 512 +
                                                 gob_y[y] + gob_x[x],
                                                 0xff000000 | px | (py << 8) |
                                                 ((px ^ py) << 16));
                                    }
                                }
                            }
                        }
                    }
                }
                qtest_writel(s, mc + 0x10, 1);
                qtest_writel(s, mc + 0x240 + head * 4, 0x80000007 + head);
                qtest_writel(s, mc + 0x228, 1U << (1 + head + window * 2));
                qtest_writel(s, mc + 0x1c, 7 + head);
                qtest_writel(s, mc + 0x20, 0x80000000 | (ptb >> 12));
                qtest_writel(s, ptb + (iova >> 22) * 4,
                             0x90000000 | (ptes >> 12));
                memcpy(memory + offset, data, bytes);
                for (unsigned page = 0; page < extent / 4096; page++) {
                    uint32_t address = physical + page * 8192;

                    qtest_writel(s, ptes + page * 4,
                                 0x80000000 | (address >> 12));
                    qtest_memwrite(s, address, memory + page * 4096, 4096);
                }
                qtest_writel(s, 0x600060d0, 0x40305001);
                qtest_writel(s, 0x600060dc, 0x40000000);
                dc_test_mode(s, head, 2);
                qtest_writel(s, dc + 0x1024, (height << 16) | width);
                qtest_writel(s, dc + 0x1028,
                             ((42 - height) << 16) | (74 - width));
                qtest_writel(s, dc + 0xa0, 0x108);
                qtest_writel(s, dc + 0x108, 1U << (4 + window));
                qtest_writel(s, dc + 0x1c00, 1U << 30);
                qtest_writel(s, dc + 0x1c0c, depth);
                qtest_writel(s, dc + 0x1c14, (height << 16) | width);
                qtest_writel(s, dc + 0x1c18, (height << 16) | (width * 4));
                qtest_writel(s, dc + 0x1c24, 0x10001000);
                qtest_writel(s, dc + 0x1c28, 256);
                qtest_writel(s, dc + 0x2000, iova + offset);
                qtest_writel(s, dc + 0x202c, mode ? 2 | ((mode - 1) << 4) : 0);
                if (mode & 1) {
                    qtest_writel(s, dc + 0x1c58, 0x00ffff01);
                    qtest_writel(s, dc + 0x1c5c, 0x262);
                    qtest_writel(s, dc + 0x1c60, 0x224);
                    qtest_writel(s, dc + 0x1c64, 0xff00);
                }
                qtest_writel(s, dc + 0x104, 0x101 | (0x202U << window));
                qtest_clock_step(s, 200000);
                image = dc_read_pixels(s, head, width, height);
                pixels = g_bytes_get_data(image, NULL);
                for (unsigned y = 0; y < height; y++) {
                    for (unsigned x = 0; x < width; x++) {
                        unsigned at = (y * width + x) * 3;

                        g_assert_cmpuint(pixels[at], ==,
                                         depth == 13 ? x : x ^ y);
                        g_assert_cmpuint(pixels[at + 1], ==, y);
                        g_assert_cmpuint(pixels[at + 2], ==,
                                         depth == 13 ? x ^ y : x);
                    }
                }
                g_assert_cmphex(qtest_readl(s, 0x500030a0), ==, 2);
                if (mode & 1) {
                    static const uint32_t colors[] = {
                        0, 0x550a1428, 0x800a1428, 0xff123456,
                        0x01ffffff, 0x7fffeedd, 0xfe123456, 0xffabcdef,
                        0x00123456, 0x01000000, 0x7f000000, 0xfe000000,
                        0xff000000, 0x80ffffff, 0x55ff00ff, 0x5500ff00,
                    };
                    uint8_t expected[ARRAY_SIZE(colors) * 3];
                    unsigned count = MIN(width, ARRAY_SIZE(colors));
                    g_autoptr(GBytes) blended = NULL;

                    qtest_writel(s, dc + 0x1390, 0x785a3c);
                    qtest_writel(s, dc + 0x104, 0x101);
                    for (unsigned x = 0; x < count; x++) {
                        uint32_t p = colors[x];
                        unsigned alpha = p >> 24;
                        unsigned red = depth == 13 ? p & 255 : (p >> 16) & 255;
                        unsigned green = (p >> 8) & 255;
                        unsigned blue = depth == 13 ? (p >> 16) & 255 : p & 255;

                        qtest_writel(s, physical + gob_x[x], p);
                        expected[x * 3] =
                            MIN(255, red + (60 * (255 - alpha) + 127) / 255);
                        expected[x * 3 + 1] =
                            MIN(255, green + (90 * (255 - alpha) + 127) / 255);
                        expected[x * 3 + 2] =
                            MIN(255, blue + (120 * (255 - alpha) + 127) / 255);
                    }
                    qtest_clock_step(s, 200000);
                    blended = dc_read_pixels(s, head, width, height);
                    g_assert_cmpmem(g_bytes_get_data(blended, NULL),
                                    count * 3, expected, count * 3);
                    g_assert_cmphex(qtest_readl(s, 0x500030a0), ==, 4);
                }
                if (mode == 6) {
                    /* Re-arming must not bypass validation of pending state. */
                    qtest_writel(s, dc + 0x104, 0x202U << window);
                    qtest_writel(s, dc + 0x1c14, 0x200fff);
                    qtest_writel(s, dc + 0x104, 0x200U << window);
                    qtest_clock_step(s, 200000);
                    g_assert_cmphex(qtest_readl(s, 0x500030a0), ==, 3);
                } else {
                    qtest_writel(s, ptes, 0x40000000 | (physical >> 12));
                    qtest_clock_step(s, 200000);
                    g_assert_cmphex(qtest_readl(s, 0x500030a0), ==,
                                    mode & 1 ? 4 : 2);
                    assert_dc_pixels(s, head, width, height,
                                     mode & 1 ? 0x3c5a78 : 0);
                }
                qtest_system_reset(s);
                g_assert_cmphex(qtest_readl(s, 0x500030a0), ==, 0);
                qtest_writel(s, dc + 0x108, 1U << (4 + window));
                g_assert_cmphex(qtest_readl(s, dc + 0x1c00), ==, 0);
            }
        }
    }
    qtest_quit(s);
}

static void test_dc_window_scanout(void)
{
    static const unsigned widths[] = {
        3, 4, 5, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64,
    };

    for (unsigned i = 0; i < ARRAY_SIZE(widths); i++) {
        test_dc_window_scanout_size(widths[i], 32, false);
    }
    test_dc_window_scanout_size(17, 32, true);
    for (unsigned height = 1; height <= 9; height++) {
        test_dc_window_scanout_size(17, height, height & 1);
    }
    test_dc_window_scanout_size(33, 33, false);
}

static void test_dc_frames_hdmi(void)
{
    const uint64_t dc = 0x54200000, hdmi = 0x54280000, sync = 0x50003080;
    QTestState *s = start_mh2p("");

    qtest_irq_intercept_out_named(s, "/machine/display0", "sysbus-irq");
    for (unsigned boot = 0; boot < 2; boot++) {
        qtest_writel(s, 0x600060d0, 0x40305001);
        qtest_writel(s, 0x600060dc, 0x40000000);
        qtest_writel(s, dc + 0xa0, 0x108);
        qtest_writel(s, dc + 0xe0, 6);
        qtest_writel(s, dc + 0xe4, 6);
        qtest_writel(s, dc + 0x1390, 0xff0080);
        dc_test_mode(s, 0, 2);
        /* Reasserting VSYNC selection during scanout is a valid DMA write. */
        qtest_writel(s, 0x40001000, 0x70 << 6);
        qtest_writel(s, 0x40001004, 0x40430000);
        qtest_writel(s, 0x40001008, 0x4000000d);
        host1x_queue(s, 0, 0x40001000, 0x4000100c);
        g_assert_cmphex(qtest_readl(s, 0x5000001c), ==, 0x4000100c);
        g_assert_cmphex(qtest_readl(s, sync + 13 * 4), ==, 1);
        qtest_writel(s, 0x40001004, 0x40430001);
        host1x_queue(s, 0, 0x40001000, 0x4000100c);
        g_assert_cmphex(qtest_readl(s, 0x5000001c), ==, 0x40001004);
        g_assert_cmphex(qtest_readl(s, sync + 13 * 4), ==, 1);
        g_assert_cmphex(qtest_readl(s, dc + 0x10c), ==, 0);
        for (unsigned queue = 0; queue < 4; queue++) {
            qtest_writel(s, dc + queue * 32, 0x500 | (9 + queue));
        }
        qtest_writel(s, hdmi + 0x118, 0x82000220);
        qtest_writel(s, hdmi + 0x118, 0x40000000);
        g_assert_cmphex(qtest_readl(s, hdmi + 0x118), ==, 0x82000220);
        qtest_writel(s, hdmi + 0x130, 0x80002000);
        qtest_writeb(s, hdmi + 0x130, 1);
        g_assert_cmphex(qtest_readl(s, hdmi + 0x130), ==, 0x80002000);
        qtest_writel(s, hdmi + 0x180, 0x802001);
        qtest_writel(s, hdmi + 0x154, 0x80000001);
        g_assert_cmphex(qtest_readl(s, hdmi + 0x154), ==, 0);
        qtest_writel(s, hdmi + 0x180, 0x80a001);
        qtest_writel(s, hdmi + 0x154, 0x80000001);
        qtest_writel(s, hdmi + 0x154, 1);
        g_assert_cmphex(qtest_readl(s, hdmi + 0x154), ==, 0x80000001);
        qtest_clock_step(s, 99999);
        g_assert_cmphex(qtest_readl(s, sync + 8 * 4), ==, 0);
        g_assert_false(qtest_get_irq(s, 0));
        qtest_clock_step(s, 1);
        g_assert_cmphex(qtest_readl(s, sync + 8 * 4), ==, 1);
        g_assert_true(qtest_get_irq(s, 0));
        g_assert_cmphex(qtest_readl(s, hdmi + 0x154), ==, 1);
        g_assert_cmphex(qtest_readl(s, hdmi + 0x17c) & (1U << 28), ==, 0);
        for (unsigned queue = 0; queue < 4; queue++) {
            g_assert_cmphex(qtest_readl(s, sync + (9 + queue) * 4), ==, 1);
        }
        assert_dc_pixels(s, 0, 64, 32, 0x8000ff);
        qtest_writel(s, hdmi + 0xc, 0x171);
        qtest_writel(s, hdmi + 8, 6);
        qtest_writel(s, hdmi + 4, 1);
        qtest_writel(s, hdmi + 8, 14);
        qtest_writel(s, hdmi + 4, 0);
        g_assert_cmphex(qtest_readl(s, hdmi + 0x158), ==, 0x800600);

        /* A submitted update becomes active only after the current frame. */
        qtest_writel(s, dc + 0x1390, 0x00ff00);
        qtest_writel(s, dc + 0x104, 0x101);
        qtest_writel(s, dc + 0x100, 1);
        g_assert_cmphex(qtest_readl(s, dc + 0x1390), ==, 0xff0080);
        g_assert_cmphex(qtest_readl(s, dc + 0x104), ==, 1);
        qtest_writel(s, dc + 0xdc, 6);
        g_assert_false(qtest_get_irq(s, 0));
        qtest_clock_step(s, 100000);
        g_assert_cmphex(qtest_readl(s, dc + 0x1390), ==, 0x00ff00);
        g_assert_cmphex(qtest_readl(s, dc + 0x104), ==, 0);
        assert_dc_pixels(s, 0, 64, 32, 0x8000ff);
        qtest_clock_step(s, 100000);
        assert_dc_pixels(s, 0, 64, 32, 0x00ff00);

        qtest_writel(s, 0x60006324, 1U << 27);
        qtest_clock_step(s, 1000000);
        g_assert_cmphex(qtest_readl(s, sync + 8 * 4), ==, 3);
        qtest_writel(s, 0x60006320, 1U << 27);
        qtest_writel(s, dc + 0x100, 0);
        qtest_writel(s, dc + 0xc8, 0);
        qtest_writel(s, dc + 0x104, 0x101);
        qtest_clock_step(s, 100000);
        qtest_clock_step(s, 1000000);
        g_assert_cmphex(qtest_readl(s, sync + 8 * 4), ==, 4);

        /* A stopped source cannot finish a new power request. */
        qtest_writel(s, hdmi + 0x154, 0x80000001);
        qtest_clock_step(s, 1000000);
        g_assert_cmphex(qtest_readl(s, hdmi + 0x154), ==, 0x80000001);
        qtest_writel(s, 0x60006308, 1U << 19);
        qtest_writel(s, hdmi + 0x130, 1234);
        qtest_writel(s, 0x6000630c, 1U << 19);
        g_assert_cmphex(qtest_readl(s, hdmi + 0x154), ==, 0);
        g_assert_cmphex(qtest_readl(s, hdmi + 0x158), ==, 0x800000);
        g_assert_cmphex(qtest_readl(s, hdmi + 0x118), ==, 0x82000210);
        g_assert_cmphex(qtest_readl(s, hdmi + 0x130), ==, 0);
        qtest_writel(s, 0x60006300, 1U << 27);
        qtest_writel(s, 0x60006304, 1U << 27);
        g_assert_false(qtest_get_irq(s, 0));
        assert_dc_pixels(s, 0, 4, 4, 0);
        qtest_system_reset(s);
    }
    qtest_quit(s);
}

static void test_dc_plld2(void)
{
    const uint64_t dc = 0x54240000, sync = 0x50003080;
    QTestState *s = start_mh2p("");

    g_assert_cmphex(qtest_readl(s, 0x600064b8), ==, 0x8010c);
    qtest_writel(s, 0x600064b8, 0x40703c01);
    qtest_writel(s, 0x600064bc, 0x40000000);
    g_assert_cmphex(qtest_readl(s, 0x600064b8), ==, 0x58703c01);
    qtest_writel(s, dc + 0xa0, 0x110);
    dc_test_mode(s, 1, 5);
    qtest_clock_step(s, 83332);
    g_assert_cmphex(qtest_readl(s, sync + 16 * 4), ==, 0);
    qtest_clock_step(s, 1);
    g_assert_cmphex(qtest_readl(s, sync + 16 * 4), ==, 1);
    qtest_writel(s, 0x600064b8, 0x40783c01);
    g_assert_cmphex(qtest_readl(s, 0x600064b8), ==, 0x40783c01);
    qtest_clock_step(s, 1000000);
    g_assert_cmphex(qtest_readl(s, sync + 16 * 4), ==, 1);

    /* Original G35 timing: 2048 * 729 pixels at 89.5 MHz. */
    qtest_writel(s, 0x600060d0, 0x4820b303);
    qtest_writel(s, 0x600060dc, 0x40400320);
    qtest_writel(s, 0x60006320, 1U << 27);
    qtest_writel(s, 0x60006138, 2U << 29);
    qtest_writel(s, 0x5420101c, 0x20028);
    qtest_writel(s, 0x54201020, 0x30030);
    qtest_writel(s, 0x54201024, 0x2d00780);
    qtest_writel(s, 0x54201028, 0x40028);
    qtest_writel(s, 0x542010b8, 0);
    qtest_writel(s, 0x542000a0, 0x108);
    qtest_writel(s, 0x542000c8, 0x20);
    qtest_writel(s, 0x54200104, 0x101);
    qtest_clock_step(s, 16681473);
    g_assert_cmphex(qtest_readl(s, sync + 8 * 4), ==, 0);
    qtest_clock_step(s, 1);
    g_assert_cmphex(qtest_readl(s, sync + 8 * 4), ==, 1);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, 0x600064b8), ==, 0x8010c);
    g_assert_cmphex(qtest_readl(s, 0x60006570), ==, 0x10000000);
    qtest_quit(s);
}

static void test_dc_banks(void)
{
    static const struct {
        unsigned index;
        uint32_t reset;
        uint32_t value;
        uint32_t invalid;
    } config[] = {
        { 0x036, 0, 0x50155, 0x80000000 },
        { 0x304, 0x510104, 0x550515, 0x80000000 },
        { 0x305, 0x555, 0x501, 2 },
        { 0x314, 0, 0x77777777, 0x80000000 },
        { 0x405, 0, 0x123, 0x2000 },
        { 0x409, 0x30003, 0x02d00780, 0x20000000 },
        { 0x40b, 0, 0xfd8, 1 },
        { 0x42e, 6, 0x103, 0x1000 },
        { 0x42f, 0, 0x30f, 0x10 },
        { 0x430, 0, 0xf1733cf, 0x80000000 },
    };
    QTestState *s = start_mh2p("");

    for (unsigned boot = 0; boot < 2; boot++) {
        for (unsigned head = 0; head < 2; head++) {
            uint64_t dc = 0x54200000 + head * 0x40000;

            g_assert_cmphex(qtest_readl(s, dc + 0x100), ==, 0);
            for (unsigned i = 0; i < ARRAY_SIZE(config); i++) {
                uint64_t address = dc + config[i].index * 4;

                g_assert_cmphex(qtest_readl(s, address), ==, config[i].reset);
                qtest_writel(s, address, config[i].value);
                qtest_writel(s, address, config[i].invalid);
                qtest_writeb(s, address, 0);
                g_assert_cmphex(qtest_readl(s, address), ==, config[i].value);
            }
            qtest_writel(s, dc + 0x100, 1);
            for (unsigned i = 0; i < ARRAY_SIZE(config); i++) {
                g_assert_cmphex(qtest_readl(s, dc + config[i].index * 4),
                               ==, config[i].reset);
            }
            qtest_writel(s, dc + 0x104, 1);
            g_assert_cmphex(qtest_readl(s, dc + 0x1024), ==, 0x30003);
            qtest_writel(s, dc + 0x104, 0x100);
            g_assert_cmphex(qtest_readl(s, dc + 0x1024), ==, 0x30003);
            qtest_writel(s, dc + 0x104, 1);
            for (unsigned i = 0; i < ARRAY_SIZE(config); i++) {
                g_assert_cmphex(qtest_readl(s, dc + config[i].index * 4),
                               ==, config[i].value);
            }

            /* Independent write mux; activation uses armed, not assembly. */
            qtest_writel(s, dc + 0x100, 0);
            qtest_writel(s, dc + 0x1024, 0x035c0500);
            qtest_writel(s, dc + 0x104, 1);
            qtest_writel(s, dc + 0x100, 1);
            g_assert_cmphex(qtest_readl(s, dc + 0x1024), ==, 0x02d00780);
            qtest_writel(s, dc + 0x100, 4);
            qtest_writel(s, dc + 0x1024, 0x10001);
            g_assert_cmphex(qtest_readl(s, dc + 0x1024), ==, 0x035c0500);
            qtest_writel(s, dc + 0x100, 5);
            g_assert_cmphex(qtest_readl(s, dc + 0x1024), ==, 0x10001);
            qtest_writel(s, dc + 0x104, 0x181);
            g_assert_cmphex(qtest_readl(s, dc + 0x1024), ==, 0x10001);
            qtest_writel(s, dc + 0x104, 1);
            g_assert_cmphex(qtest_readl(s, dc + 0x1024), ==, 0x02d00780);
            qtest_writel(s, dc + 0x104, 0x101);
            g_assert_cmphex(qtest_readl(s, dc + 0x1024), ==, 0x035c0500);
            qtest_writel(s, dc + 0x100, 2);
            g_assert_cmphex(qtest_readl(s, dc + 0x100), ==, 5);
            g_assert_cmphex(qtest_readl(s, dc + 0x104), ==, 0);
            qtest_writel(s, dc + 0xc8, 0x20);
            qtest_writel(s, dc + 0xc8, 0x40);
            g_assert_cmphex(qtest_readl(s, dc + 0xc8), ==, 0);

            qtest_writel(s, dc + 0x108, 0x70);
            qtest_writel(s, dc + 0x108, 0x80);
            g_assert_cmphex(qtest_readl(s, dc + 0x108), ==, 0x70);
            qtest_writel(s, dc + 0x1c00, 0);
            qtest_writel(s, dc + 0x1c00, 1U << 30);
            qtest_writel(s, dc + 0x104, 0xe0e);
            for (unsigned window = 0; window < 3; window++) {
                qtest_writel(s, dc + 0x108, 1U << (window + 4));
                g_assert_cmphex(qtest_readl(s, dc + 0x1c00), ==, 0);
            }
            qtest_writel(s, dc + 0x10c, 0x54);
            qtest_writel(s, dc + 0x10c, 0x80);
            g_assert_cmphex(qtest_readl(s, dc + 0x10c), ==, 0x54);
            for (unsigned queue = 0; queue < 4; queue++) {
                uint64_t control = dc + 4 + queue * 32;

                qtest_writel(s, control, 0x101);
                qtest_writel(s, control, 2);
                g_assert_cmphex(qtest_readl(s, control), ==, 0x100);
                g_assert_cmphex(qtest_readl(s, control + 4), ==, 0);
            }
            for (unsigned i = 0; i < 4; i++) {
                uint64_t address = dc + 0xe0 + i * 4;
                uint32_t reset = i == 1 ? 1 : i == 3 ? 0x400000 : 0;

                g_assert_cmphex(qtest_readl(s, address), ==, reset);
                qtest_writel(s, address, UINT32_MAX);
                g_assert_cmphex(qtest_readl(s, address), ==,
                               i < 2 ? 0x3c1f7bf : 0x3c1f7be);
            }
            qtest_writel(s, dc + 0xdc, UINT32_MAX);
            g_assert_cmphex(qtest_readl(s, dc + 0xdc), ==, 0);
        }

        for (unsigned head = 0; head < 2; head++) {
            uint64_t dc = 0x54200000 + head * 0x40000;
            uint64_t other = 0x54200000 + (1 - head) * 0x40000;

            qtest_writel(s, other + 0x100, 4);
            qtest_writel(s, other + 0x1024, 0x1111222);
            qtest_writel(s, 0x60006300, 1U << (27 - head));
            qtest_writel(s, dc + 0x1024, 0x123);
            qtest_writel(s, dc + 0x100, 5);
            g_assert_cmphex(qtest_readl(s, dc + 0x100), ==, 0);
            qtest_writel(s, 0x60006304, 1U << (27 - head));
            g_assert_cmphex(qtest_readl(s, dc + 0x100), ==, 0);
            g_assert_cmphex(qtest_readl(s, dc + 0x1024), ==, 0x30003);
            qtest_writel(s, dc + 0x100, 1);
            qtest_writel(s, dc + 0x104, 1);
            g_assert_cmphex(qtest_readl(s, dc + 0x1024), ==, 0x30003);
            g_assert_cmphex(qtest_readl(s, dc + 0x108), ==, 0);
            g_assert_cmphex(qtest_readl(s, dc + 0x10c), ==, 0);
            g_assert_cmphex(qtest_readl(s, dc + 4), ==, 0);
            qtest_writel(s, other + 0x100, 1);
            g_assert_cmphex(qtest_readl(s, other + 0x1024), ==, 0x1111222);
        }
        qtest_system_reset(s);
    }
    qtest_quit(s);
}

static void test_dc_sync(void)
{
    const uint64_t sync = 0x50002100;
    static const unsigned ids[] = { 0, 1, 31, 32, 191 };
    static const uint32_t unsupported[] = {
        0x101, 0x201, 0x401, 0x501, 0x601, 0x701, 0x10001, 192, 0x3ff,
    };
    QTestState *s = start_mh2p("");

    qtest_irq_intercept_out_named(s, "/machine/host1x", "sysbus-irq");
    for (unsigned boot = 0; boot < 2; boot++) {
        for (unsigned head = 0; head < 2; head++) {
            uint64_t dc = 0x54200000 + head * 0x40000;

            for (unsigned i = 0; i < ARRAY_SIZE(ids); i++) {
                unsigned id = ids[i], bank = id / 32;
                uint32_t bit = 1U << (id % 32);

                qtest_writel(s, sync + 0xf80 + id * 4, UINT32_MAX);
                qtest_writel(s, sync + 0x1380 + id * 4, 1);
                qtest_writel(s, sync + 0xf20 + bank * 4, bit);
                qtest_writel(s, dc, id);
                g_assert_cmphex(qtest_readl(s, sync + 0xf80 + id * 4), ==, 0);
                g_assert_false(qtest_get_irq(s, 0));
                qtest_writel(s, dc, 0x300 | id);
                g_assert_cmphex(qtest_readl(s, sync + 0xf80 + id * 4), ==, 1);
                g_assert_true(qtest_get_irq(s, 0));
                qtest_writel(s, sync + 0xf00 + bank * 4, bit);
                qtest_writel(s, sync + 0xe80 + bank * 4, bit);
                g_assert_false(qtest_get_irq(s, 0));
            }
            for (unsigned i = 0; i < ARRAY_SIZE(unsupported); i++) {
                qtest_writel(s, dc, unsupported[i]);
                g_assert_cmphex(qtest_readl(s, sync + 0xf84), ==, 1);
                g_assert_false(qtest_get_irq(s, 0));
            }
            qtest_writel(s, 0x60006300, 1U << (27 - head));
            qtest_writel(s, dc, 0x301);
            qtest_writel(s, 0x60006304, 1U << (27 - head));
            g_assert_cmphex(qtest_readl(s, sync + 0xf84), ==, 1);
            qtest_writel(s, 0x60006300, 1U << 28);
            qtest_writel(s, dc, 0x301);
            qtest_writel(s, 0x60006304, 1U << 28);
            g_assert_cmphex(qtest_readl(s, sync + 0xf84), ==, 0);
            qtest_writel(s, dc, 0x301);
            g_assert_cmphex(qtest_readl(s, sync + 0xf84), ==, 1);
        }
        qtest_system_reset(s);
        g_assert_cmphex(qtest_readl(s, sync + 0xf84), ==, 0);
    }
    qtest_quit(s);
}

#define RSTP_UART 0x70006000

static uint16_t rstp_crc(const uint8_t *buf, size_t len)
{
    uint16_t crc = 0xffff;

    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i] << 8;
        for (int bit = 0; bit < 8; bit++) {
            crc = crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1;
        }
    }
    return crc;
}

static void rstp_put(QTestState *s, uint8_t byte)
{
    qtest_writeb(s, RSTP_UART, byte);
}

static void rstp_send(QTestState *s, const uint8_t *body, size_t len)
{
    uint16_t crc = rstp_crc(body, len);
    uint8_t tail[] = { crc & 0xff, crc >> 8 };

    rstp_put(s, 0x7e);
    for (size_t i = 0; i < len + 2; i++) {
        uint8_t b = i < len ? body[i] : tail[i - len];

        if (b == 0x7e || b == 0x7d) {
            rstp_put(s, 0x7d);
            b ^= 0x20;
        }
        rstp_put(s, b);
    }
}

/* Read and unescape every pending peer frame, verifying start and CRC. */
static GPtrArray *rstp_recv(QTestState *s)
{
    GPtrArray *frames = g_ptr_array_new_with_free_func(
        (GDestroyNotify)g_byte_array_unref);
    GByteArray *cur = NULL;
    bool esc = false;

    while (qtest_readb(s, RSTP_UART + 0x14) & 1) {
        uint8_t b = qtest_readb(s, RSTP_UART);

        if (b == 0x7e) {
            cur = g_byte_array_new();
            g_ptr_array_add(frames, cur);
            continue;
        }
        g_assert_nonnull(cur);
        if (b == 0x7d) {
            esc = true;
            continue;
        }
        if (esc) {
            b ^= 0x20;
            esc = false;
        }
        g_byte_array_append(cur, &b, 1);
    }
    for (guint i = 0; i < frames->len; i++) {
        GByteArray *f = g_ptr_array_index(frames, i);

        g_assert_cmpuint(f->len, >=, 3);
        g_assert_cmphex(rstp_crc(f->data, f->len - 2), ==,
                        f->data[f->len - 2] | f->data[f->len - 1] << 8);
        g_byte_array_set_size(f, f->len - 2);
    }
    return frames;
}

static void rstp_expect(GPtrArray *frames, guint index,
                        const uint8_t *body, size_t len)
{
    GByteArray *f;

    g_assert_cmpuint(index, <, frames->len);
    f = g_ptr_array_index(frames, index);
    g_assert_cmpuint(f->len, ==, len);
    for (size_t i = 0; i < len; i++) {
        if (f->data[i] != body[i]) {
            g_test_message("RSTP frame %u byte %zu: got 0x%02x, expected 0x%02x",
                           index, i, f->data[i], body[i]);
        }
        g_assert_cmpuint(f->data[i], ==, body[i]);
    }
}

static void test_rstp_watchdog_backlog(void)
{
    g_autofree char *image = NULL;
    g_autoptr(GError) error = NULL;
    const uint8_t branch_to_self[] = { 0xfe, 0xff, 0xff, 0xea };
    uint8_t reply[] = { 0x20, 6, 0xff, 0, 1, 0, 0, 0 };
    uint8_t expected[] = { 0x20, 6, 0xff, 0, 0, 0, 0, 0 };
    int fd = g_file_open_tmp("mh2p-rstp-watchdog-XXXXXX", &image, &error);
    g_autoptr(GPtrArray) frames = NULL;
    QTestState *s;

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, branch_to_self, sizeof(branch_to_self)), ==,
                   sizeof(branch_to_self));
    close(fd);
    s = qtest_initf("-M mh2p -m 256M -kernel %s -chardev mh2p-rstp,id=rstp "
                    "-serial chardev:rstp -serial null", image);
    unlink(image);
    qtest_writeb(s, RSTP_UART + 0x08, 0x07);
    rstp_send(s, (const uint8_t[]){ 1 }, 1);

    qtest_clock_step(s, 1000000000);
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 1);
    rstp_expect(frames, 0, expected, sizeof(expected));
    g_clear_pointer(&frames, g_ptr_array_unref);
    rstp_send(s, reply, sizeof(reply));

    /* Hold the UART unread for the observed fourteen-request backlog. */
    for (unsigned i = 0; i < 14; i++) {
        qtest_clock_step(s, 1000000000);
    }
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 14);
    for (unsigned i = 0; i < 14; i++) {
        stl_le_p(expected + 4, i + 2);
        rstp_expect(frames, i, expected, sizeof(expected));
    }
    g_clear_pointer(&frames, g_ptr_array_unref);

    /* Delayed, duplicated and out-of-order replies must not rewind TX. */
    for (unsigned i = 0; i < 14; i++) {
        stl_le_p(reply + 4, 15 - i);
        rstp_send(s, reply, sizeof(reply));
        rstp_send(s, reply, sizeof(reply));
    }
    qtest_clock_step(s, 1000000000);
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 1);
    stl_le_p(expected + 4, 16);
    rstp_expect(frames, 0, expected, sizeof(expected));
    g_clear_pointer(&frames, g_ptr_array_unref);

    /* Only the latest request's counter + 1 advances the next request. */
    stl_le_p(reply + 4, 17);
    rstp_send(s, reply, sizeof(reply));
    rstp_send(s, reply, sizeof(reply));
    stl_le_p(reply + 4, UINT32_MAX);
    rstp_send(s, reply, sizeof(reply));
    qtest_clock_step(s, 1000000000);
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 1);
    stl_le_p(expected + 4, 18);
    rstp_expect(frames, 0, expected, sizeof(expected));
    g_clear_pointer(&frames, g_ptr_array_unref);

    qtest_system_reset(s);
    qtest_writeb(s, RSTP_UART + 0x08, 0x07);
    rstp_send(s, (const uint8_t[]){ 1 }, 1);
    qtest_clock_step(s, 1000000000);
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 1);
    stl_le_p(expected + 4, 0);
    rstp_expect(frames, 0, expected, sizeof(expected));
    qtest_quit(s);
}

static void test_rstp_clamp_state(void)
{
    g_autofree char *image = NULL;
    g_autoptr(GError) error = NULL;
    const uint8_t branch_to_self[] = { 0xfe, 0xff, 0xff, 0xea };
    const uint8_t init_ack[] = { 1 };
    const uint8_t request[] = { 0x10, 5, 0x10, 0, 1, 14, 2 };
    const uint8_t ack_request[] = { 0x11, 2, 0x10, 0 };
    const uint8_t clamp_on[] = { 0x10, 5, 0x10, 0, 1, 2, 3 };
    const uint8_t clamp_off[] = { 0x10, 5, 0x10, 1, 1, 2, 0 };
    int fd = g_file_open_tmp("mh2p-rstp-power-XXXXXX", &image, &error);
    g_autoptr(GPtrArray) frames = NULL;
    QTestState *s;

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, branch_to_self, sizeof(branch_to_self)), ==,
                   sizeof(branch_to_self));
    close(fd);
    s = qtest_initf("-M mh2p -m 256M -kernel %s -chardev mh2p-rstp,id=rstp "
                    "-serial chardev:rstp -serial null", image);
    unlink(image);
    qtest_writeb(s, RSTP_UART + 0x08, 0x07);
    rstp_send(s, init_ack, sizeof(init_ack));

    /* No vehicle state is invented when the operator has not supplied one. */
    rstp_send(s, request, sizeof(request));
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 1);
    rstp_expect(frames, 0, ack_request, sizeof(ack_request));
    g_clear_pointer(&frames, g_ptr_array_unref);

    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':'/chardevs/rstp',"
        "'property':'clamp-state','value':3}}");
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 1);
    rstp_expect(frames, 0, clamp_on, sizeof(clamp_on));
    g_clear_pointer(&frames, g_ptr_array_unref);

    /* ACKing keypanel sequence zero must not acknowledge power sequence zero. */
    rstp_send(s, (const uint8_t[]){ 0x11, 2, 0x40, 0 }, 4);
    qtest_clock_step(s, 210 * 1000 * 1000);
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 1);
    rstp_expect(frames, 0, clamp_on, sizeof(clamp_on));
    g_clear_pointer(&frames, g_ptr_array_unref);
    rstp_send(s, ack_request, sizeof(ack_request));

    rstp_send(s, (const uint8_t[]){ 0x10, 3, 0x40, 0, 0x7e }, 5);
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 5);
    g_clear_pointer(&frames, g_ptr_array_unref);

    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':'/chardevs/rstp',"
        "'property':'clamp-state','value':0}}");
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 1);
    rstp_expect(frames, 0, clamp_off, sizeof(clamp_off));
    g_clear_pointer(&frames, g_ptr_array_unref);
    rstp_send(s, (const uint8_t[]){ 0x11, 2, 0x10, 1 }, 4);

    /* Power ACKs likewise leave pending keypanel records intact. */
    qtest_clock_step(s, 210 * 1000 * 1000);
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 4);
    for (unsigned i = 0; i < frames->len; i++) {
        GByteArray *f = g_ptr_array_index(frames, i);

        g_assert_cmpuint(f->data[2], ==, 0x40);
        g_assert_cmpuint(f->data[3], ==, i);
    }
    g_clear_pointer(&frames, g_ptr_array_unref);
    rstp_send(s, (const uint8_t[]){ 0x11, 2, 0x40, 3 }, 4);

    qobject_unref(qtest_qmp_assert_failure_ref(s,
        "{'execute':'qom-set','arguments':{'path':'/chardevs/rstp',"
        "'property':'clamp-state','value':4}}"));
    /* Repeated state and duplicate requests do not create extra events. */
    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':'/chardevs/rstp',"
        "'property':'clamp-state','value':0}}");
    rstp_send(s, request, sizeof(request));
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 1);
    rstp_expect(frames, 0, ack_request, sizeof(ack_request));
    g_clear_pointer(&frames, g_ptr_array_unref);
    rstp_send(s, (const uint8_t[]){ 0x10, 5, 0x10, 1, 1, 14, 2 }, 7);
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 2);
    rstp_expect(frames, 1,
                (const uint8_t[]){ 0x10, 5, 0x10, 2, 1, 2, 0 }, 7);
    g_clear_pointer(&frames, g_ptr_array_unref);

    /* Reset retains the selected physical inputs, not transport sequences. */
    qtest_system_reset(s);
    qtest_writeb(s, RSTP_UART + 0x08, 0x07);
    rstp_send(s, init_ack, sizeof(init_ack));
    rstp_send(s, request, sizeof(request));
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 2);
    rstp_expect(frames, 0, ack_request, sizeof(ack_request));
    rstp_expect(frames, 1,
                (const uint8_t[]){ 0x10, 5, 0x10, 0, 1, 2, 0 }, 7);
    g_clear_pointer(&frames, g_ptr_array_unref);
    rstp_send(s, ack_request, sizeof(ack_request));

    qobject_unref(qtest_qmp_assert_failure_ref(s,
        "{'execute':'qom-set','arguments':{'path':'/chardevs/rstp',"
        "'property':'power-state','value':19}}"));
    qobject_unref(qtest_qmp_assert_failure_ref(s,
        "{'execute':'qom-set','arguments':{'path':'/chardevs/rstp',"
        "'property':'power-state','value':-2}}"));
    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':'/chardevs/rstp',"
        "'property':'power-state','value':1}}");
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 1);
    rstp_expect(frames, 0,
                (const uint8_t[]){ 0x10, 7, 0x10, 1, 1, 1, 1, 1, 0 }, 9);
    g_clear_pointer(&frames, g_ptr_array_unref);
    rstp_send(s, (const uint8_t[]){ 0x11, 2, 0x10, 1 }, 4);

    /* A main-power query publishes power only, not a second clamp event. */
    rstp_send(s, (const uint8_t[]){ 0x10, 5, 0x10, 1, 1, 14, 1 }, 7);
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 2);
    rstp_expect(frames, 1,
                (const uint8_t[]){ 0x10, 7, 0x10, 2, 1, 1, 1, 1, 0 }, 9);
    g_clear_pointer(&frames, g_ptr_array_unref);
    rstp_send(s, (const uint8_t[]){ 0x11, 2, 0x10, 2 }, 4);

    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':'/chardevs/rstp',"
        "'property':'power-state','value':3}}");
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 1);
    rstp_expect(frames, 0,
                (const uint8_t[]){ 0x10, 7, 0x10, 3, 1, 1, 1, 3, 0 }, 9);
    g_clear_pointer(&frames, g_ptr_array_unref);
    rstp_send(s, (const uint8_t[]){ 0x11, 2, 0x10, 3 }, 4);
    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':'/chardevs/rstp',"
        "'property':'power-state','value':3}}");
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 0);
    g_clear_pointer(&frames, g_ptr_array_unref);

    /* Disabled publication must not invent a main power-state response. */
    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':'/chardevs/rstp',"
        "'property':'power-state','value':-1}}");
    rstp_send(s, (const uint8_t[]){ 0x10, 5, 0x10, 2, 1, 14, 1 }, 7);
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 1);
    g_clear_pointer(&frames, g_ptr_array_unref);

    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':'/chardevs/rstp',"
        "'property':'power-state','value':1}}");
    g_ptr_array_unref(rstp_recv(s));
    qtest_system_reset(s);
    qtest_writeb(s, RSTP_UART + 0x08, 0x07);
    rstp_send(s, init_ack, sizeof(init_ack));
    rstp_send(s, request, sizeof(request));
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 3);
    rstp_expect(frames, 0, ack_request, sizeof(ack_request));
    rstp_expect(frames, 1,
                (const uint8_t[]){ 0x10, 5, 0x10, 0, 1, 2, 0 }, 7);
    rstp_expect(frames, 2,
                (const uint8_t[]){ 0x10, 7, 0x10, 1, 1, 1, 1, 1, 0 }, 9);
    qtest_quit(s);
}

static void rstp_touch_expect(QTestState *s, unsigned sequence,
                              unsigned active, const uint8_t *contacts,
                              unsigned count)
{
    g_autoptr(GPtrArray) frames = rstp_recv(s);
    uint8_t expected[36] = {
        0x10, 8 + count * 6, 0x40, sequence, 0x0f, 13,
        0, count, active, 0,
    };
    GByteArray *frame;

    g_assert_cmpuint(count, <=, 4);
    g_assert_cmpuint(frames->len, ==, 1);
    frame = g_ptr_array_index(frames, 0);
    g_assert_cmpuint(frame->len, ==, 10 + count * 6);
    expected[6] = frame->data[6];
    memcpy(expected + 10, contacts, count * 6);
    rstp_expect(frames, 0, expected, 10 + count * 6);
    rstp_send(s, (const uint8_t[]){ 0x11, 2, 0x40, sequence }, 4);
}

static void rstp_touch_event(QTestState *s, const char *type,
                             int slot, int tracking_id, int x, int y)
{
    qtest_qmp_assert_success(s,
        "{'execute':'input-send-event','arguments':{'events':["
        "{'type':'mtt','data':{'type':%s,'slot':%d,'tracking-id':%d,"
        "'axis':'x','value':%d}},"
        "{'type':'mtt','data':{'type':'data','slot':%d,'tracking-id':%d,"
        "'axis':'x','value':%d}},"
        "{'type':'mtt','data':{'type':'data','slot':%d,'tracking-id':%d,"
        "'axis':'y','value':%d}}]}}",
        type, slot, tracking_id, x, slot, tracking_id, x,
        slot, tracking_id, y);
}

static void test_rstp_multitouch(void)
{
    g_autofree char *image = NULL;
    g_autoptr(GError) error = NULL;
    const uint8_t branch_to_self[] = { 0xfe, 0xff, 0xff, 0xea };
    int fd = g_file_open_tmp("mh2p-rstp-touch-XXXXXX", &image, &error);
    g_autoptr(GPtrArray) frames = NULL;
    QTestState *s;

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, branch_to_self, sizeof(branch_to_self)), ==,
                   sizeof(branch_to_self));
    close(fd);
    s = qtest_initf("-M mh2p -m 256M -kernel %s -chardev mh2p-rstp,id=rstp "
                    "-serial chardev:rstp -serial null", image);
    unlink(image);
    qtest_writeb(s, RSTP_UART + 0x08, 0x07);
    rstp_send(s, (const uint8_t[]){ 1 }, 1);
    rstp_send(s, (const uint8_t[]){ 0x10, 3, 0x40, 0, 0x7e }, 5);
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 5);
    g_clear_pointer(&frames, g_ptr_array_unref);
    rstp_send(s, (const uint8_t[]){ 0x11, 2, 0x40, 3 }, 4);

    /* A synchronized QMP batch is one native two-contact report. */
    qtest_qmp_assert_success(s,
        "{'execute':'input-send-event','arguments':{'events':["
        "{'type':'mtt','data':{'type':'begin','slot':0,'tracking-id':41,"
        "'axis':'x','value':0}},"
        "{'type':'mtt','data':{'type':'begin','slot':1,'tracking-id':92,"
        "'axis':'x','value':0}},"
        "{'type':'mtt','data':{'type':'data','slot':1,'tracking-id':92,"
        "'axis':'x','value':32767}},"
        "{'type':'mtt','data':{'type':'data','slot':1,'tracking-id':92,"
        "'axis':'y','value':32767}}]}}");
    rstp_touch_expect(s, 4, 2, (const uint8_t[]) {
        0, 0, 0, 0, 0, 0,
        1, 0, 0xff, 3, 0x1b, 2,
    }, 2);

    qtest_qmp_assert_success(s,
        "{'execute':'input-send-event','arguments':{'events':["
        "{'type':'mtt','data':{'type':'data','slot':0,'tracking-id':41,"
        "'axis':'x','value':32767}},"
        "{'type':'mtt','data':{'type':'data','slot':1,'tracking-id':92,"
        "'axis':'x','value':0}}]}}");
    rstp_touch_expect(s, 5, 2, (const uint8_t[]) {
        0, 0, 0xff, 3, 0, 0,
        1, 0, 0, 0, 0x1b, 2,
    }, 2);

    /* A lifted contact is explicit; the other finger retains its native ID. */
    qtest_qmp_assert_success(s,
        "{'execute':'input-send-event','arguments':{'events':["
        "{'type':'mtt','data':{'type':'end','slot':0,'tracking-id':-1,"
        "'axis':'x','value':0}}]}}");
    rstp_touch_expect(s, 6, 1, (const uint8_t[]) {
        0, 0xff, 0xff, 3, 0, 0,
        1, 0, 0, 0, 0x1b, 2,
    }, 2);
    qtest_qmp_assert_success(s,
        "{'execute':'input-send-event','arguments':{'events':["
        "{'type':'mtt','data':{'type':'cancel','slot':1,'tracking-id':92,"
        "'axis':'x','value':0}}]}}");
    rstp_touch_expect(s, 7, 0,
                      (const uint8_t[]){ 1, 0xff, 0, 0, 0x1b, 2 }, 1);

    /* Slot reuse, clamping, and the maximum single-frame contact count. */
    for (unsigned i = 0; i < 4; i++) {
        uint8_t contacts[24] = { 0 };

        rstp_touch_event(s, "begin", i, 100 + i, -100, 40000);
        for (unsigned j = 0; j <= i; j++) {
            contacts[j * 6] = j;
            contacts[j * 6 + 4] = 0x1b;
            contacts[j * 6 + 5] = 2;
        }
        rstp_touch_expect(s, 8 + i, i + 1, contacts, i + 1);
    }

    /* Switching to mouse input releases every MTT contact before the press. */
    qtest_qmp_assert_success(s,
        "{'execute':'input-send-event','arguments':{'events':["
        "{'type':'btn','data':{'button':'left','down':true}}]}}");
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 2);
    {
        GByteArray *release = g_ptr_array_index(frames, 0);

        g_assert_cmpuint(release->len, ==, 34);
        g_assert_cmpuint(release->data[7], ==, 4);
        g_assert_cmpuint(release->data[8], ==, 0);
        for (unsigned i = 0; i < 4; i++) {
            g_assert_cmpuint(release->data[10 + i * 6], ==, i);
            g_assert_cmphex(release->data[11 + i * 6], ==, 0xff);
        }
    }
    rstp_expect(frames, 1, (const uint8_t[]) {
        0x10, 14, 0x40, 13, 0x0f, 13, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0,
    }, 16);
    g_clear_pointer(&frames, g_ptr_array_unref);
    rstp_send(s, (const uint8_t[]){ 0x11, 2, 0x40, 13 }, 4);

    /* MTT also releases a held mouse; a reboot must forget all contacts. */
    rstp_touch_event(s, "begin", 2, 200, 0, 0);
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 2);
    g_assert_cmpuint(((GByteArray *)g_ptr_array_index(frames, 0))->data[8], ==, 0);
    g_assert_cmpuint(((GByteArray *)g_ptr_array_index(frames, 1))->data[10], ==, 2);
    g_clear_pointer(&frames, g_ptr_array_unref);
    qtest_system_reset(s);
    qtest_writeb(s, RSTP_UART + 0x08, 0x07);
    rstp_send(s, (const uint8_t[]){ 1 }, 1);
    rstp_touch_event(s, "begin", 0, 300, 32767, 0);
    rstp_touch_expect(s, 0, 1,
                      (const uint8_t[]){ 0, 0, 0xff, 3, 0, 0 }, 1);

    /* Unsupported slots and stale tracking IDs cannot mutate a held finger. */
    qtest_qmp_assert_success(s,
        "{'execute':'input-send-event','arguments':{'events':["
        "{'type':'mtt','data':{'type':'begin','slot':4,'tracking-id':400,"
        "'axis':'x','value':0}},"
        "{'type':'mtt','data':{'type':'begin','slot':-1,'tracking-id':400,"
        "'axis':'x','value':0}},"
        "{'type':'mtt','data':{'type':'data','slot':0,'tracking-id':301,"
        "'axis':'x','value':0}},"
        "{'type':'mtt','data':{'type':'begin','slot':0,'tracking-id':301,"
        "'axis':'x','value':0}}]}}");
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 0);
    g_clear_pointer(&frames, g_ptr_array_unref);

    /* Even an entire tap in one QMP batch must not lose its press. */
    qtest_qmp_assert_success(s,
        "{'execute':'input-send-event','arguments':{'events':["
        "{'type':'mtt','data':{'type':'end','slot':0,'tracking-id':-1,"
        "'axis':'x','value':0}},"
        "{'type':'mtt','data':{'type':'begin','slot':0,'tracking-id':301,"
        "'axis':'x','value':0}},"
        "{'type':'mtt','data':{'type':'end','slot':0,'tracking-id':-1,"
        "'axis':'x','value':0}}]}}");
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 3);
    for (unsigned i = 0; i < 3; i++) {
        GByteArray *frame = g_ptr_array_index(frames, i);

        g_assert_cmpuint(frame->data[8], ==, i == 1);
        g_assert_cmpuint(frame->data[10], ==, 0);
        g_assert_cmpuint(frame->data[11], ==, i == 1 ? 0 : 0xff);
    }
    g_clear_pointer(&frames, g_ptr_array_unref);
    rstp_send(s, (const uint8_t[]){ 0x11, 2, 0x40, 3 }, 4);

    qtest_qmp_assert_success(s,
        "{'execute':'input-send-event','arguments':{'events':["
        "{'type':'mtt','data':{'type':'begin','slot':1,"
        "'tracking-id':4294967300,'axis':'x','value':0}}]}}");
    rstp_touch_expect(s, 4, 1, (const uint8_t[]){ 1, 0, 0, 0, 0, 0 }, 1);
    /* A 64-bit ID must not alias an unrelated truncated ID. */
    qtest_qmp_assert_success(s,
        "{'execute':'input-send-event','arguments':{'events':["
        "{'type':'mtt','data':{'type':'data','slot':1,'tracking-id':4,"
        "'axis':'x','value':32767}}]}}");
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 0);
    g_clear_pointer(&frames, g_ptr_array_unref);
    qtest_qmp_assert_success(s,
        "{'execute':'input-send-event','arguments':{'events':["
        "{'type':'mtt','data':{'type':'end','slot':1,'tracking-id':-1,"
        "'axis':'x','value':0}}]}}");
    rstp_touch_expect(s, 5, 0, (const uint8_t[]){ 1, 0xff, 0, 0, 0, 0 }, 1);
    qtest_quit(s);
}

static void test_rstp_ack_validation(void)
{
    g_autofree char *image = NULL;
    g_autoptr(GError) error = NULL;
    const uint8_t branch_to_self[] = { 0xfe, 0xff, 0xff, 0xea };
    const uint8_t ready_kbds[] = { 1, 4, 13, 17 };
    g_autoptr(GPtrArray) frames = NULL;
    int fd = g_file_open_tmp("mh2p-rstp-ack-XXXXXX", &image, &error);
    QTestState *s;

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, branch_to_self, sizeof(branch_to_self)), ==,
                    sizeof(branch_to_self));
    close(fd);
    s = qtest_initf("-M mh2p -m 256M -kernel %s -chardev mh2p-rstp,id=rstp "
                    "-serial chardev:rstp -serial null", image);
    unlink(image);
    qtest_writeb(s, RSTP_UART + 0x08, 0x07);
    rstp_send(s, (const uint8_t[]){ 0x01 }, 1);
    rstp_send(s, (const uint8_t[]){ 0x10, 3, 0x40, 0, 0x7e }, 5);
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 5);
    g_clear_pointer(&frames, g_ptr_array_unref);

    /* Neither a future ACK nor a stale ACK acknowledges a sent frame. */
    rstp_send(s, (const uint8_t[]){ 0x11, 2, 0x40, 4 }, 4);
    rstp_send(s, (const uint8_t[]){ 0x11, 2, 0x40, 0xff }, 4);
    qtest_clock_step(s, 250 * 1000 * 1000);
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 4);
    for (unsigned i = 0; i < 4; i++) {
        const uint8_t ready[] = { 0x10, 5, 0x40, i, 0x0d, ready_kbds[i], 1 };

        rstp_expect(frames, i, ready, sizeof(ready));
    }
    g_clear_pointer(&frames, g_ptr_array_unref);

    /* A valid cumulative ACK retires only its prefix. */
    rstp_send(s, (const uint8_t[]){ 0x11, 2, 0x40, 1 }, 4);
    rstp_send(s, (const uint8_t[]){ 0x11, 2, 0x40, 1 }, 4);
    qtest_clock_step(s, 250 * 1000 * 1000);
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 2);
    for (unsigned i = 0; i < 2; i++) {
        const uint8_t ready[] = { 0x10, 5, 0x40, i + 2,
                                  0x0d, ready_kbds[i + 2], 1 };

        rstp_expect(frames, i, ready, sizeof(ready));
    }
    g_clear_pointer(&frames, g_ptr_array_unref);

    /* An unknown NACK resynchronizes without discarding pending records. */
    rstp_send(s, (const uint8_t[]){ 0x12, 2, 0x40, 10 }, 4);
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 2);
    for (unsigned i = 0; i < 2; i++) {
        const uint8_t ready[] = { 0x10, 5, 0x40, i + 10,
                                  0x0d, ready_kbds[i + 2], 1 };

        rstp_expect(frames, i, ready, sizeof(ready));
    }
    g_clear_pointer(&frames, g_ptr_array_unref);
    rstp_send(s, (const uint8_t[]){ 0x11, 2, 0x40, 11 }, 4);
    qtest_clock_step(s, 250 * 1000 * 1000);
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 0);
    g_clear_pointer(&frames, g_ptr_array_unref);

    /* Advance through sequence wrap without changing cumulative ACK semantics. */
    for (unsigned pair = 0; pair < 128; pair++) {
        uint8_t first = 12 + pair * 2;
        uint8_t last = first + 1;

        for (unsigned down = 0; down < 2; down++) {
            qtest_qmp_assert_success(s,
                "{'execute':'input-send-event','arguments':{'events':["
                "{'type':'key','data':{'down':%i,"
                "'key':{'type':'qcode','data':'home'}}}]}}",
                !down);
        }
        frames = rstp_recv(s);
        g_assert_cmpuint(frames->len, ==, 2);
        rstp_expect(frames, 0, (const uint8_t[]){ 0x10, 7, 0x40, first,
                                                0, 1, 116, 1, 0 }, 9);
        rstp_expect(frames, 1, (const uint8_t[]){ 0x10, 7, 0x40, last,
                                                0, 1, 116, 0, 0 }, 9);
        g_clear_pointer(&frames, g_ptr_array_unref);
        if (first == 254 || first == 0) {
            uint8_t invalid = first == 254 ? 0 : 255;

            rstp_send(s, (const uint8_t[]){ 0x11, 2, 0x40, invalid }, 4);
            qtest_clock_step(s, 250 * 1000 * 1000);
            frames = rstp_recv(s);
            if (first == 254) {
                g_assert_cmpuint(frames->len, ==, 3);
                rstp_expect(frames, 2,
                            (const uint8_t[]){ 0x20, 6, 0xff, 0, 0, 0, 0, 0 },
                            8);
                g_ptr_array_remove_index(frames, 2);
            }
            g_assert_cmpuint(frames->len, ==, 2);
            g_assert_cmpuint(((GByteArray *)g_ptr_array_index(frames, 0))->data[3],
                            ==, first);
            g_assert_cmpuint(((GByteArray *)g_ptr_array_index(frames, 1))->data[3],
                            ==, last);
            g_clear_pointer(&frames, g_ptr_array_unref);
        }
        rstp_send(s, (const uint8_t[]){ 0x11, 2, 0x40, last }, 4);
    }

    /* An ACK cannot release records waiting behind the eight-frame window. */
    for (unsigned event = 0; event < 12; event++) {
        qtest_qmp_assert_success(s,
            "{'execute':'input-send-event','arguments':{'events':["
            "{'type':'key','data':{'down':%i,"
            "'key':{'type':'qcode','data':'home'}}}]}}",
            !(event % 2));
    }
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 8);
    g_clear_pointer(&frames, g_ptr_array_unref);
    rstp_send(s, (const uint8_t[]){ 0x11, 2, 0x40, 20 }, 4);
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 0);
    g_clear_pointer(&frames, g_ptr_array_unref);
    qtest_clock_step(s, 250 * 1000 * 1000);
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 8);
    for (unsigned i = 0; i < 8; i++) {
        g_assert_cmpuint(((GByteArray *)g_ptr_array_index(frames, i))->data[3],
                        ==, i + 12);
    }
    g_clear_pointer(&frames, g_ptr_array_unref);
    rstp_send(s, (const uint8_t[]){ 0x11, 2, 0x40, 15 }, 4);
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 4);
    for (unsigned i = 0; i < 4; i++) {
        g_assert_cmpuint(((GByteArray *)g_ptr_array_index(frames, i))->data[3],
                        ==, i + 20);
    }
    g_clear_pointer(&frames, g_ptr_array_unref);
    rstp_send(s, (const uint8_t[]){ 0x11, 2, 0x40, 23 }, 4);
    qtest_clock_step(s, 250 * 1000 * 1000);
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 0);
    qtest_quit(s);
}

static void test_rstp_keypanel(void)
{
    g_autofree char *image = NULL;
    g_autoptr(GError) error = NULL;
    const uint8_t branch_to_self[] = { 0xfe, 0xff, 0xff, 0xea };
    int fd = g_file_open_tmp("mh2p-rstp-XXXXXX", &image, &error);
    /* First frame the original MMX rstp transmits after start. */
    const uint8_t pwrman[] = { 0x10, 0x06, 0x10, 0x00, 0x01, 0x0a, 0x21,
                               0x0d };
    const uint8_t keypanel_req[] = { 0x10, 0x03, 0x40, 0x00, 0x7e };
    const uint8_t init[] = { 0x00, 0xc1, 0x80, 0x80, 0x80 };
    const uint8_t init_ack[] = { 0x01 };
    const uint8_t ack_pwrman[] = { 0x11, 0x02, 0x10, 0x00 };
    const uint8_t ack_keypanel[] = { 0x11, 0x02, 0x40, 0x00 };
    const uint8_t ack_ready[] = { 0x11, 0x02, 0x40, 0x03 };
    const uint8_t ready_kbds[] = { 1, 4, 13, 17 };
    g_autoptr(GPtrArray) frames = NULL;
    QTestState *s;

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, branch_to_self, sizeof(branch_to_self)), ==,
                    sizeof(branch_to_self));
    close(fd);
    s = qtest_initf("-M mh2p -m 256M -kernel %s -chardev mh2p-rstp,id=rstp "
                    "-serial chardev:rstp -serial null", image);
    unlink(image);
    qtest_writeb(s, RSTP_UART + 0x08, 0x07);

    /* Like a booting RCC, INIT is repeated until the MMX acknowledges. */
    for (unsigned i = 0; i < 2; i++) {
        qtest_clock_step(s, 100 * 1000 * 1000);
        frames = rstp_recv(s);
        g_assert_cmpuint(frames->len, ==, 1);
        rstp_expect(frames, 0, init, sizeof(init));
        g_ptr_array_unref(frames);
    }
    rstp_send(s, pwrman, sizeof(pwrman));
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 1);
    rstp_expect(frames, 0, ack_pwrman, sizeof(ack_pwrman));
    g_ptr_array_unref(frames);
    rstp_send(s, init_ack, sizeof(init_ack));
    qtest_clock_step(s, 300 * 1000 * 1000);
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 0);
    g_ptr_array_unref(frames);

    /* A duplicate is re-ACKed but not redelivered. */
    rstp_send(s, pwrman, sizeof(pwrman));
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 1);
    rstp_expect(frames, 0, ack_pwrman, sizeof(ack_pwrman));
    g_ptr_array_unref(frames);

    /* First keypanel traffic announces the input panels. */
    rstp_send(s, keypanel_req, sizeof(keypanel_req));
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 5);
    rstp_expect(frames, 0, ack_keypanel, sizeof(ack_keypanel));
    for (unsigned i = 0; i < 4; i++) {
        const uint8_t ready[] = { 0x10, 5, 0x40, i, 0x0d, ready_kbds[i], 1 };

        rstp_expect(frames, i + 1, ready, sizeof(ready));
    }
    g_ptr_array_unref(frames);
    rstp_send(s, ack_ready, sizeof(ack_ready));

    /* Touch press and release become TouchCoordinate records. */
    qtest_qmp_assert_success(s,
        "{'execute':'input-send-event','arguments':{'events':["
        "{'type':'abs','data':{'axis':'x','value':0}},"
        "{'type':'abs','data':{'axis':'y','value':0}},"
        "{'type':'btn','data':{'button':'left','down':true}}]}}");
    qtest_qmp_assert_success(s,
        "{'execute':'input-send-event','arguments':{'events':["
        "{'type':'btn','data':{'button':'left','down':false}}]}}");
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 2);
    {
        GByteArray *press = g_ptr_array_index(frames, 0);
        GByteArray *release = g_ptr_array_index(frames, 1);
        const uint8_t press_hdr[] = { 0x10, 14, 0x40, 4, 0x0f, 13, 0, 1, 1,
                                      0, 0, 0, 0, 0, 0, 0 };

        rstp_expect(frames, 0, press_hdr, sizeof(press_hdr));
        g_assert_cmpuint(press->len, ==, release->len);
        g_assert_cmpuint(release->data[3], ==, 5);
        g_assert_cmpuint(release->data[4], ==, 0x0f);
        g_assert_cmpuint(release->data[7], ==, 1);
        g_assert_cmpuint(release->data[8], ==, 0);
    }
    g_ptr_array_unref(frames);

    /* Unacknowledged frames are retransmitted with the same SQN. */
    qtest_clock_step(s, 250 * 1000 * 1000);
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 2);
    g_assert_cmpuint(((GByteArray *)g_ptr_array_index(frames, 0))->data[3],
                     ==, 4);
    g_assert_cmpuint(((GByteArray *)g_ptr_array_index(frames, 1))->data[3],
                     ==, 5);
    g_ptr_array_unref(frames);

    /* NACKs right after a transmission are left to the timer... */
    rstp_send(s, (const uint8_t[]){ 0x12, 0x02, 0x40, 0x05 }, 4);
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 0);
    g_ptr_array_unref(frames);

    /* ...later ones retransmit from the expected SQN only. */
    qtest_clock_step(s, 25 * 1000 * 1000);
    rstp_send(s, (const uint8_t[]){ 0x12, 0x02, 0x40, 0x05 }, 4);
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 1);
    g_assert_cmpuint(((GByteArray *)g_ptr_array_index(frames, 0))->data[3],
                     ==, 5);
    g_ptr_array_unref(frames);
    rstp_send(s, (const uint8_t[]){ 0x11, 0x02, 0x40, 0x05 }, 4);

    /* Host keys become Key2 records; auto-repeat is suppressed. */
    qtest_qmp_assert_success(s,
        "{'execute':'input-send-event','arguments':{'events':["
        "{'type':'key','data':{'down':true,"
        "'key':{'type':'qcode','data':'home'}}}]}}");
    qtest_qmp_assert_success(s,
        "{'execute':'input-send-event','arguments':{'events':["
        "{'type':'key','data':{'down':true,"
        "'key':{'type':'qcode','data':'home'}}}]}}");
    qtest_qmp_assert_success(s,
        "{'execute':'input-send-event','arguments':{'events':["
        "{'type':'key','data':{'down':false,"
        "'key':{'type':'qcode','data':'home'}}}]}}");
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 2);
    rstp_expect(frames, 0, (const uint8_t[]){ 0x10, 7, 0x40, 6,
                                              0x00, 1, 116, 1, 0 }, 9);
    rstp_expect(frames, 1, (const uint8_t[]){ 0x10, 7, 0x40, 7,
                                              0x00, 1, 116, 0, 0 }, 9);
    g_ptr_array_unref(frames);
    rstp_send(s, (const uint8_t[]){ 0x11, 0x02, 0x40, 0x07 }, 4);

    /* P2865's unequal axis calibration requires 1024x540 raw coordinates. */
    qtest_qmp_assert_success(s,
        "{'execute':'input-send-event','arguments':{'events':["
        "{'type':'abs','data':{'axis':'x','value':32767}},"
        "{'type':'abs','data':{'axis':'y','value':32767}},"
        "{'type':'btn','data':{'button':'left','down':true}}]}}");
    qtest_qmp_assert_success(s,
        "{'execute':'input-send-event','arguments':{'events':["
        "{'type':'btn','data':{'button':'left','down':false}}]}}");
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 2);
    rstp_expect(frames, 0,
                (const uint8_t[]){ 0x10, 14, 0x40, 8, 0x0f, 13, 0, 1, 1,
                                  0, 0, 0, 0xff, 3, 0x1b, 2 }, 16);
    {
        GByteArray *release = g_ptr_array_index(frames, 1);
        uint8_t expected[] = { 0x10, 14, 0x40, 9, 0x0f, 13, 0, 1, 0,
                               0, 0, 0, 0xff, 3, 0x1b, 2 };

        g_assert_cmpuint(release->len, ==, sizeof(expected));
        /* Time between the two QMP commands is not necessarily zero. */
        expected[6] = release->data[6];
        rstp_expect(frames, 1, expected, sizeof(expected));
    }
    g_ptr_array_unref(frames);
    rstp_send(s, (const uint8_t[]){ 0x11, 0x02, 0x40, 9 }, 4);

    /* The RCC watchdog counter follows runmode-mgr's counter + 1 reply. */
    qtest_clock_step(s, 1000 * 1000 * 1000);
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 1);
    rstp_expect(frames, 0, (const uint8_t[]){ 0x20, 6, 0xff, 0,
                                              0, 0, 0, 0 }, 8);
    g_ptr_array_unref(frames);
    rstp_send(s, (const uint8_t[]){ 0x20, 6, 0xff, 0, 1, 0, 0, 0 }, 8);
    qtest_clock_step(s, 1000 * 1000 * 1000);
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 1);
    rstp_expect(frames, 0, (const uint8_t[]){ 0x20, 6, 0xff, 0,
                                              2, 0, 0, 0 }, 8);
    g_ptr_array_unref(frames);

    /* Machine reset restarts the link handshake and sequence numbers. */
    qtest_system_reset(s);
    qtest_writeb(s, RSTP_UART + 0x08, 0x07);
    qtest_clock_step(s, 100 * 1000 * 1000);
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 1);
    rstp_expect(frames, 0, init, sizeof(init));
    g_ptr_array_unref(frames);
    rstp_send(s, pwrman, sizeof(pwrman));
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 1);
    rstp_expect(frames, 0, ack_pwrman, sizeof(ack_pwrman));
    g_ptr_array_unref(frames);

    /* An INIT from a restarted MMX multiplexer is answered and ends ours. */
    rstp_send(s, init, sizeof(init));
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 1);
    rstp_expect(frames, 0, init_ack, sizeof(init_ack));
    g_ptr_array_unref(frames);
    qtest_clock_step(s, 300 * 1000 * 1000);
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 0);
    g_ptr_array_unref(frames);
    rstp_send(s, pwrman, sizeof(pwrman));
    frames = rstp_recv(s);
    g_assert_cmpuint(frames->len, ==, 1);
    rstp_expect(frames, 0, ack_pwrman, sizeof(ack_pwrman));
    qtest_quit(s);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/mh2p/boot-registers-and-reset", test_boot_registers);
    qtest_add_func("/mh2p/legacy-interrupt-gates", test_legacy_interrupt_gates);
    qtest_add_func("/mh2p/pmc-secure-scratch", test_pmc_secure_scratch);
    qtest_add_func("/mh2p/fuse-security-mode", test_fuse_security_mode);
    qtest_add_func("/mh2p/se-aes-dma", test_se_aes_dma);
    qtest_add_func("/mh2p/se-boot-keys", test_se_boot_keys);
    qtest_add_func("/mh2p/se-boot-slot-context", test_se_boot_slot_context);
    qtest_add_func("/mh2p/se-keytable-access", test_se_keytable_access);
    qtest_add_func("/mh2p/se-tzram", test_se_tzram);
    qtest_add_func("/mh2p/se-dma-rejections", test_se_dma_rejections);
    qtest_add_func("/mh2p/runtime-clocks-and-reset", test_runtime_clocks);
    qtest_add_func("/mh2p/smmu-init-and-reset", test_smmu_init);
    qtest_add_func("/mh2p/host1x-sync-and-reset", test_host1x_sync);
    qtest_add_func("/mh2p/gk20a-init-and-reset", test_gk20a_init);
    qtest_add_func("/mh2p/gk20a-ring-and-reset", test_gk20a_ring);
    qtest_add_func("/mh2p/gk20a-engine-controls", test_gk20a_engine_controls);
    qtest_add_func("/mh2p/gk20a-mmu-page-size", test_gk20a_mmu_page_size);
    qtest_add_func("/mh2p/gk20a-bar1-config", test_gk20a_bar1_config);
    qtest_add_func("/mh2p/sor-configuration", test_sor_configuration);
    qtest_add_func("/mh2p/rstp-keypanel", test_rstp_keypanel);
    qtest_add_func("/mh2p/rstp-ack-validation", test_rstp_ack_validation);
    qtest_add_func("/mh2p/rstp-multitouch", test_rstp_multitouch);
    qtest_add_func("/mh2p/rstp-clamp-state", test_rstp_clamp_state);
    qtest_add_func("/mh2p/rstp-watchdog-backlog", test_rstp_watchdog_backlog);
    qtest_add_func("/mh2p/diagnostic-pointer", test_diagnostic_pointer);
    qtest_add_func("/mh2p/diagnostic-pointer-motion",
                   test_diagnostic_pointer_motion);
    qtest_add_func("/mh2p/dc-banks-and-reset", test_dc_banks);
    qtest_add_func("/mh2p/dc-host1x-sync-and-reset", test_dc_sync);
    qtest_add_func("/mh2p/host1x-display-dma", test_host1x_dma);
    qtest_add_func("/mh2p/host1x-wait-resume", test_host1x_wait_resume);
    qtest_add_func("/mh2p/host1x-delay", test_host1x_delay);
    qtest_add_func("/mh2p/host1x-dma-faults", test_host1x_dma_faults);
    qtest_add_func("/mh2p/host1x-restart", test_host1x_restart);
    qtest_add_func("/mh2p/host1x-iommu", test_host1x_iommu);
    qtest_add_func("/mh2p/vic-firmware-dma", test_vic_dma);
    qtest_add_func("/mh2p/vic-iommu", test_vic_iommu);
    qtest_add_func("/mh2p/vic-code-paging", test_vic_code_paging);
    qtest_add_func("/mh2p/vic-cpu-alu", test_vic_cpu_alu);
    qtest_add_func("/mh2p/vic-cpu-control", test_vic_cpu_control);
    qtest_add_func("/mh2p/vic-cpu-dma", test_vic_cpu_dma);
    qtest_add_func("/mh2p/vic-cpu-io", test_vic_cpu_io);
    qtest_add_func("/mh2p/vic-cpu-multistack", test_vic_cpu_multistack);
    qtest_add_func("/mh2p/vic-cpu-predicates", test_vic_cpu_predicates);
    qtest_add_func("/mh2p/vic-cpu-faults", test_vic_cpu_faults);
    qtest_add_func("/mh2p/vic-method-fifo", test_vic_method_fifo);
    qtest_add_func("/mh2p/vic-cpu-interrupts", test_vic_cpu_interrupts);
    qtest_add_func("/mh2p/vic-host1x-methods", test_vic_host1x_methods);
    qtest_add_func("/mh2p/vic-syncpoints", test_vic_syncpts);
    qtest_add_func("/mh2p/vic-cpu-method-ops", test_vic_cpu_method_ops);
    qtest_add_func("/mh2p/dc-frames-and-hdmi", test_dc_frames_hdmi);
    qtest_add_func("/mh2p/dc-window-scanout", test_dc_window_scanout);
    qtest_add_func("/mh2p/dc-window-alpha", test_dc_window_alpha);
    qtest_add_func("/mh2p/dc-two-windows", test_dc_two_windows);
    qtest_add_func("/mh2p/dc-plld2-clock", test_dc_plld2);
    qtest_add_func("/mh2p/usb-pll-handoff-and-reset", test_usb_pll_handoff);
    qtest_add_func("/mh2p/runtime-power-memory-and-reset",
                   test_runtime_power_memory);
    qtest_add_func("/mh2p/pcie-pad-configuration", test_pcie_pad_configuration);
    qtest_add_func("/mh2p/pcie-controller-configuration",
                   test_pcie_controller_configuration);
    qtest_add_func("/mh2p/pcie-no-endpoint", test_pcie_no_endpoint);
    qtest_add_func("/mh2p/pcie-root-discovery", test_pcie_root_discovery);
    qtest_add_func("/mh2p/pcie-window-configuration",
                   test_pcie_window_configuration);
    qtest_add_func("/mh2p/pcie-config-aperture", test_pcie_config_aperture);
    qtest_add_func("/mh2p/pcie-config-translation",
                   test_pcie_config_translation);
    qtest_add_func("/mh2p/gpio-banks-and-reset", test_gpio);
    qtest_add_func("/mh2p/avp-registers-and-reset", test_avp_registers);
    qtest_add_func("/mh2p/usb-overcurrent-input", test_usb_overcurrent_input);
    qtest_add_func("/mh2p/nor-dma-and-overlay", test_nor_dma);
    qtest_add_func("/mh2p/sdmmc-and-overlay", test_sdmmc);
    qtest_add_func("/mh2p/mnand-handoff-and-cold-reset", test_mnand_handoff);
    qtest_add_func("/mh2p/i2c-nack-and-reset", test_i2c);
    qtest_add_func("/mh2p/i2c-nack-payload", test_i2c_nack_payload);
    qtest_add_func("/mh2p/spi-pio-and-reset", test_spi);
    return g_test_run();
}

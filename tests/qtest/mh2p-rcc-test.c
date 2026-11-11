/*
 * Firmware-free checks for the experimental MH2P RCC direct-start machine.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"

static QTestState *start_rcc_options(const char *options)
{
    g_autofree char *image = NULL;
    g_autoptr(GError) error = NULL;
    uint8_t payload[0x110] = {0};
    int fd = g_file_open_tmp("mh2p-rcc-test-XXXXXX", &image, &error);
    QTestState *s;

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    stl_le_p(payload + 8, 0x00ff7eeb);
    stw_le_p(payload + 12, 1);
    stw_le_p(payload + 16, 0x100);
    stw_le_p(payload + 18, 40);
    stl_le_p(payload + 20, 0x80100100);
    stl_le_p(payload + 28, 0x80100008);
    stl_le_p(payload + 40, sizeof(payload) - 8);
    stl_le_p(payload + 44, sizeof(payload) - 8);
    stl_le_p(payload + 0x100, 0xeafffffe);
    g_assert_cmpint(write(fd, payload, sizeof(payload)), ==, sizeof(payload));
    close(fd);
    s = qtest_initf("-M mh2p-rcc -kernel %s -serial null "
                    "-serial null -serial null %s", image, options);
    unlink(image);
    return s;
}

static QTestState *start_rcc(void)
{
    return start_rcc_options("");
}

static void test_mpu_watchdog(void)
{
    g_autofree char *logfile = NULL;
    g_autofree char *contents = NULL;
    g_autoptr(GError) error = NULL;
    int fd = g_file_open_tmp("mh2p-rcc-wdt-XXXXXX", &logfile, &error);
    g_autofree char *options = NULL;
    QTestState *s;
    const uint32_t base = 0x482a0000;

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    options = g_strdup_printf("-d unimp -D %s", logfile);
    s = start_rcc_options(options);
    qtest_irq_intercept_in(s, "/machine/interrupt-controller");
    qtest_writel(s, 0x4a002a48, 5 | (139 << 16));
    for (unsigned offset = 0; offset < 0x18; offset += 4) {
        g_assert_cmphex(qtest_readl(s, base + offset), ==, 0);
    }
    qtest_writel(s, base + 4, 0x12345678);
    g_assert_cmphex(qtest_readl(s, base + 4), ==, 0);
    qtest_writel(s, base + 0x10, 0x103);
    g_assert_cmphex(qtest_readl(s, base + 0x14), ==, 3);
    g_assert_true(qtest_get_irq(s, 4));
    g_assert_true(qtest_get_irq(s, 7));
    qtest_writel(s, base + 0x10, 0);
    g_assert_false(qtest_get_irq(s, 4));
    g_assert_false(qtest_get_irq(s, 7));
    qtest_writel(s, base + 0x14, 3);
    qtest_writel(s, base + 8, 2);
    qtest_writel(s, base + 0xc, 9);
    qtest_writel(s, base, 9);
    qtest_writel(s, base + 0xc, 99);
    qtest_writel(s, base + 0x10, 0x103);
    qtest_clock_step(s, 69);
    g_assert_cmphex(qtest_readl(s, base + 4), ==, 3);
    g_assert_false(qtest_get_irq(s, 4));
    qtest_clock_step(s, 1);
    g_assert_cmphex(qtest_readl(s, base + 4), ==, 2);
    g_assert_cmphex(qtest_readl(s, base + 0x14), ==, 2);
    g_assert_true(qtest_get_irq(s, 4));
    g_assert_false(qtest_get_irq(s, 7));
    qtest_writel(s, base + 0x14, 2);
    g_assert_false(qtest_get_irq(s, 4));
    qtest_clock_step(s, 29);
    g_assert_cmphex(qtest_readl(s, base + 0x14), ==, 0);
    qtest_clock_step(s, 1);
    g_assert_true(qtest_get_irq(s, 7));
    g_assert_cmphex(qtest_readl(s, base + 0x14), ==, 1);
    qtest_writel(s, base + 0x14, 1);
    qtest_clock_step(s, 1000);
    g_assert_false(qtest_get_irq(s, 7));
    g_assert_cmphex(qtest_readl(s, base + 4), ==, 0);
    qtest_writel(s, base + 0x10, 0);
    qtest_writel(s, base, 9);
    qtest_clock_step(s, 1000);
    g_assert_cmphex(qtest_readl(s, base + 4), ==, 9);
    qtest_writel(s, base + 0x10, 1);
    qtest_clock_step(s, 100);
    g_assert_cmphex(qtest_readl(s, base + 4), ==, 8);
    qtest_writel(s, base + 0x10, 0);
    qtest_clock_step(s, 1000);
    g_assert_cmphex(qtest_readl(s, base + 4), ==, 8);
    qtest_writel(s, base + 0xc, 0xffffffff);
    g_assert_cmphex(qtest_readl(s, base + 0xc), ==, 0x3ff);
    qtest_writel(s, base + 0xc, 0);
    qtest_writel(s, base, 1);
    qtest_writel(s, 0x4a005160, 6);
    qtest_writel(s, base + 0x10, 3);
    qtest_clock_step(s, 49);
    g_assert_cmphex(qtest_readl(s, base + 4), ==, 1);
    qtest_clock_step(s, 1);
    g_assert_cmphex(qtest_readl(s, base + 4), ==, 0);
    g_assert_false(qtest_get_irq(s, 7));
    qtest_writel(s, 0x4a005160, 7);
    qtest_clock_step(s, 1);
    g_assert_true(qtest_get_irq(s, 7));
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, base + 0x14), ==, 1);
    g_assert_cmphex(qtest_readl(s, base + 0x10), ==, 0);
    g_assert_cmphex(qtest_readl(s, base + 4), ==, 0);
    qtest_writel(s, 0x4a002a48, 5 | (139 << 16));
    qtest_writel(s, base + 0x14, 3);
    qtest_writel(s, base, 0xffffffff);
    g_assert_cmphex(qtest_readl(s, base + 4), ==, 0xffffffff);
    qtest_writel(s, base + 8, 0);
    qtest_writel(s, base + 0xc, 9);
    qtest_writel(s, base, 1);
    qtest_writel(s, base + 0x10, 3);
    qtest_clock_step(s, 5);
    qtest_writel(s, 0x4a005160, 6);
    qtest_clock_step(s, 249);
    g_assert_cmphex(qtest_readl(s, base + 4), ==, 1);
    qtest_clock_step(s, 1);
    g_assert_cmphex(qtest_readl(s, base + 4), ==, 0);
    qtest_clock_step(s, 250);
    qtest_writel(s, 0x4a005160, 7);
    qtest_clock_step(s, 4);
    g_assert_false(qtest_get_irq(s, 7));
    qtest_clock_step(s, 1);
    g_assert_true(qtest_get_irq(s, 7));
    qtest_writel(s, base + 0x14, 3);
    qtest_writel(s, base + 0xc, 0);
    qtest_writel(s, base, 9);
    qtest_writel(s, base + 0x10, 0xfffffff7);
    g_assert_cmphex(qtest_readl(s, base + 0x10), ==, 0x103);
    qtest_clock_step(s, 9);
    g_assert_true(qtest_get_irq(s, 4));
    qtest_writel(s, base, 9);
    qtest_writel(s, base + 0x14, 3);
    g_assert_false(qtest_get_irq(s, 4));
    qtest_clock_step(s, 9);
    g_assert_false(qtest_get_irq(s, 7));
    qtest_clock_step(s, 1);
    g_assert_true(qtest_get_irq(s, 7));
    qtest_writel(s, base + 0x10, 0);
    qtest_writel(s, base + 0x14, 3);
    qtest_writel(s, base, 9);
    qtest_writel(s, base + 0x10, 9);
    qtest_clock_step(s, 10);
    qtest_qmp_eventwait(s, "RESET");
    g_assert_cmphex(qtest_readl(s, base + 0x14), ==, 1);
    g_assert_cmphex(qtest_readl(s, base + 0x10), ==, 0);
    qtest_quit(s);
    g_assert_true(g_file_get_contents(logfile, &contents, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(contents,
        "unsupported MPU watchdog write offset=0x4 value=0x12345678"));
    g_assert_null(strstr(contents, "unsupported MPU watchdog read"));
    unlink(logfile);
}

static void test_gmac_reset(void)
{
    static const uint32_t resets[] = {
        0x48484008, 0x4848481c, 0x48484d8c, 0x48484dcc, 0x48485204,
    };
    g_autofree char *logfile = NULL;
    g_autofree char *contents = NULL;
    g_autoptr(GError) error = NULL;
    int fd = g_file_open_tmp("mh2p-rcc-gmac-XXXXXX", &logfile, &error);
    QTestState *s;

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    g_autofree char *options = g_strdup_printf("-d unimp -D %s", logfile);
    s = start_rcc_options(options);
    for (unsigned i = 0; i < ARRAY_SIZE(resets); i++) {
        g_assert_cmphex(qtest_readl(s, resets[i]), ==, 0);
        qtest_writel(s, resets[i], 1);
        g_assert_cmphex(qtest_readl(s, resets[i]), ==, 0);
        qtest_writel(s, resets[i], 0);
    }
    qtest_writel(s, 0x48484008, 2);
    qtest_writel(s, 0x48484004, 1);
    for (unsigned i = 0; i < 2; i++) {
        uint32_t address = 0x48484208 + i * 0x100;

        g_assert_cmphex(qtest_readl(s, address), ==, 0x113);
        qtest_writel(s, address, 0xe6);
        g_assert_cmphex(qtest_readl(s, address), ==, 0xe6);
        qtest_writel(s, address, 0xffffffff);
        g_assert_cmphex(qtest_readl(s, address), ==, 0x1ff);
    }
    qtest_writel(s, 0x4848481c, 1);
    g_assert_cmphex(qtest_readl(s, 0x48484208), ==, 0x1ff);
    qtest_writel(s, 0x48484008, 1);
    g_assert_cmphex(qtest_readl(s, 0x48484208), ==, 0x113);
    qtest_writel(s, 0x48484208, 0xe6);
    g_assert_cmphex(qtest_readl(s, 0x48484024), ==, 0);
    qtest_writel(s, 0x48484024, 0xffffffff);
    g_assert_cmphex(qtest_readl(s, 0x48484024), ==, 7);
    qtest_writel(s, 0x48486000, 0x12345678);
    qtest_writel(s, 0x48487ffc, 0xabcdef01);
    g_assert_cmphex(qtest_readl(s, 0x48486000), ==, 0x12345678);
    g_assert_cmphex(qtest_readl(s, 0x48487ffc), ==, 0xabcdef01);
    qtest_writeb(s, 0x48486001, 0x90);
    g_assert_cmphex(qtest_readl(s, 0x48486000), ==, 0x12349078);
    qtest_writel(s, 0x48484a00, 0x48486000);
    g_assert_cmphex(qtest_readl(s, 0x48484a00), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x48484884), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x48486000), ==, 0x12349078);
    qtest_writel(s, 0x48484d08, 0xa0000000);
    g_assert_cmphex(qtest_readl(s, 0x48484d08), ==, 0);
    qtest_system_reset(s);
    for (unsigned i = 0; i < ARRAY_SIZE(resets); i++) {
        g_assert_cmphex(qtest_readl(s, resets[i]), ==, 0);
    }
    g_assert_cmphex(qtest_readl(s, 0x48484208), ==, 0x113);
    g_assert_cmphex(qtest_readl(s, 0x48484024), ==, 0);
    qtest_quit(s);
    g_assert_true(g_file_get_contents(logfile, &contents, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(contents, "reserved GMAC reset bits"));
    g_assert_nonnull(strstr(contents,
                           "unsupported write at 0x48484004 size=4"));
    g_assert_null(strstr(contents, "unsupported write at 0x48484008 size=4"));
    g_assert_null(strstr(contents, "unsupported write at 0x48486000"));
    g_assert_null(strstr(contents, "unsupported write at 0x48487ffc"));
    g_assert_nonnull(strstr(contents,
        "unsupported GMAC DMA write offset=0x200 value=0x48486000"));
    g_assert_nonnull(strstr(contents,
        "unsupported GMAC ALE write offset=0x8 value=0xa0000000"));
    unlink(logfile);
}

static void test_control_io1(void)
{
    g_autofree char *logfile = NULL;
    g_autofree char *contents = NULL;
    g_autoptr(GError) error = NULL;
    int fd = g_file_open_tmp("mh2p-rcc-io1-XXXXXX", &logfile, &error);
    g_autofree char *options = NULL;
    QTestState *s;
    const uint32_t reg = 0x4a002554;

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    options = g_strdup_printf("-d unimp -D %s", logfile);
    s = start_rcc_options(options);
    g_assert_cmphex(qtest_readl(s, reg), ==, 0x3300);
    for (unsigned burst = 0; burst < 4; burst++) {
        for (unsigned gmii1 = 0; gmii1 < 3; gmii1++) {
            for (unsigned gmii2 = 0; gmii2 < 3; gmii2++) {
                uint32_t value = (burst << 12) | ((3 - burst) << 8) |
                                 (gmii2 << 4) | gmii1;

                qtest_writel(s, reg, value);
                g_assert_cmphex(qtest_readl(s, reg), ==, value);
            }
        }
    }
    qtest_writel(s, reg, 0xffeecfee);
    g_assert_cmphex(qtest_readl(s, reg), ==, 0x322);
    qtest_writel(s, reg, 0x3322);
    static const uint32_t unsupported[] = {
        0x103322, 0x13322, 0x3332, 0x3323,
    };
    for (unsigned i = 0; i < ARRAY_SIZE(unsupported); i++) {
        qtest_writel(s, reg, unsupported[i]);
        g_assert_cmphex(qtest_readl(s, reg), ==, 0x3322);
    }
    qtest_writel(s, 0x48484008, 1);
    qtest_writel(s, 0x48485204, 1);
    g_assert_cmphex(qtest_readl(s, reg), ==, 0x3322);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, reg), ==, 0x3300);
    qtest_quit(s);
    g_assert_true(g_file_get_contents(logfile, &contents, NULL, &error));
    g_assert_no_error(error);
    for (unsigned i = 0; i < ARRAY_SIZE(unsupported); i++) {
        g_autofree char *message = g_strdup_printf(
            "unsupported write at 0x4a002554 size=4 value=0x%x",
            unsupported[i]);

        g_assert_nonnull(strstr(contents, message));
    }
    unlink(logfile);
}

static void test_gmac_mdio(void)
{
    g_autofree char *logfile = NULL;
    g_autofree char *contents = NULL;
    g_autoptr(GError) error = NULL;
    int fd = g_file_open_tmp("mh2p-rcc-mdio-XXXXXX", &logfile, &error);
    g_autofree char *options = NULL;
    QTestState *s;
    const uint32_t base = 0x48485000;

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    options = g_strdup_printf("-d unimp -D %s", logfile);
    s = start_rcc_options(options);
    g_assert_cmphex(qtest_readl(s, base + 4), ==, 0x81000000);
    qtest_writel(s, base + 0x80, 0x80200000);
    g_assert_cmphex(qtest_readl(s, base + 0x80), ==, 0x00200000);
    qtest_writel(s, base + 4, 0x40c0007c);
    g_assert_cmphex(qtest_readl(s, base + 4), ==, 0x4100007c);
    qtest_writel(s, base + 0x80, 0x80200000);
    qtest_writel(s, base + 0x88, 0xc0411234);
    qtest_writel(s, base + 0x80, 0);
    g_assert_cmphex(qtest_readl(s, base + 0x80), ==, 0x80200000);
    qtest_clock_step(s, 63999);
    g_assert_cmphex(qtest_readl(s, base + 0x80), ==, 0x80200000);
    qtest_clock_step(s, 1);
    g_assert_cmphex(qtest_readl(s, base + 0x80), ==, 0x0020ffff);
    g_assert_cmphex(qtest_readl(s, base + 0x88), ==, 0xc0411234);
    g_assert_cmphex(qtest_readl(s, base + 0x20), ==, 1);
    qtest_writel(s, base + 4, 0);
    g_assert_cmphex(qtest_readl(s, base + 4), ==, 0x01000000);
    qtest_clock_step(s, 64000);
    g_assert_cmphex(qtest_readl(s, base + 4), ==, 0x81000000);
    g_assert_cmphex(qtest_readl(s, base + 0x88), ==, 0x40411234);
    g_assert_cmphex(qtest_readl(s, base + 0x20), ==, 3);
    g_assert_cmphex(qtest_readl(s, base + 0x24), ==, 0);
    qtest_writel(s, base + 0x24, 1);
    g_assert_cmphex(qtest_readl(s, base + 0x20), ==, 2);
    qtest_writel(s, base + 0x20, 2);
    g_assert_cmphex(qtest_readl(s, base + 0x20), ==, 0);
    for (unsigned offset = 8; offset <= 0x14; offset += 4) {
        g_assert_cmphex(qtest_readl(s, base + offset), ==, 0);
    }
    qtest_writel(s, base + 4, 0x40000000);
    qtest_writel(s, base + 0x80, 0x80200000);
    qtest_clock_step(s, 1000000);
    g_assert_cmphex(qtest_readl(s, base + 0x80), ==, 0x80200000);
    qtest_writel(s, base + 4, 0x4010007c);
    qtest_clock_step(s, 31999);
    g_assert_cmphex(qtest_readl(s, base + 0x80), ==, 0x80200000);
    qtest_clock_step(s, 1);
    g_assert_cmphex(qtest_readl(s, base + 0x80), ==, 0x0020ffff);
    qtest_writel(s, base + 0x84, 0xffffff3f);
    g_assert_cmphex(qtest_readl(s, base + 0x84), ==, 0x1f);
    qtest_writel(s, base + 0x84, 0x40);
    g_assert_cmphex(qtest_readl(s, base + 0x84), ==, 0x1f);
    qtest_writel(s, base + 4, 0x4002007c);
    g_assert_cmphex(qtest_readl(s, base + 4), ==, 0x4110007c);
    qtest_writel(s, base + 0x28, 1);
    g_assert_cmphex(qtest_readl(s, base + 0x28), ==, 0);
    qtest_writel(s, base + 0x80, 0x80200000);
    qtest_system_reset(s);
    qtest_clock_step(s, 1000000);
    g_assert_cmphex(qtest_readl(s, base + 4), ==, 0x81000000);
    g_assert_cmphex(qtest_readl(s, base + 0x80), ==, 0);
    g_assert_cmphex(qtest_readl(s, base + 0x20), ==, 0);
    g_assert_cmphex(qtest_readl(s, base + 0x84), ==, 0);
    qtest_quit(s);
    g_assert_true(g_file_get_contents(logfile, &contents, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(contents,
        "unsupported GMAC MDIO write offset=0x84 value=0x40"));
    g_assert_nonnull(strstr(contents,
        "unsupported GMAC MDIO write offset=0x4 value=0x4002007c"));
    g_assert_nonnull(strstr(contents,
        "unsupported GMAC MDIO write offset=0x28 value=0x1"));
    unlink(logfile);
}

static void test_gmac_configuration(void)
{
    QTestState *s = start_rcc();
    const uint32_t dma = 0x48484800, ale = 0x48484d00;

    qtest_writel(s, 0x48484110, 0x40c0);
    g_assert_cmphex(qtest_readl(s, 0x48484110), ==, 0x40c0);
    qtest_writel(s, 0x48484210, 0xffffffff);
    g_assert_cmphex(qtest_readl(s, 0x48484210), ==, 0x0ff3f3ff);
    qtest_writel(s, 0x48484118, 0xffffffff);
    g_assert_cmphex(qtest_readl(s, 0x48484118), ==, 0x33333333);
    qtest_writel(s, 0x48484120, 0xffffffff);
    g_assert_cmphex(qtest_readl(s, 0x48484120), ==, 0x77777777);
    qtest_writel(s, 0x48484314, 0xffffffff);
    g_assert_cmphex(qtest_readl(s, 0x48484314), ==, 0xffff);
    qtest_writel(s, 0x48485214, 0xffffffff);
    g_assert_cmphex(qtest_readl(s, 0x48485214), ==, 0xff);
    qtest_writel(s, 0x48485204, 1);
    g_assert_cmphex(qtest_readl(s, 0x48485214), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x48484314), ==, 0xffff);
    qtest_writel(s, dma + 0x004, 1);
    qtest_writel(s, dma + 0x014, 1);
    g_assert_cmphex(qtest_readl(s, dma + 0x004), ==, 1);
    g_assert_cmphex(qtest_readl(s, dma + 0x014), ==, 1);
    qtest_writel(s, dma + 0x028, 0xffffffff);
    g_assert_cmphex(qtest_readl(s, dma + 0x028), ==, 0xffff);
    for (unsigned i = 0; i < 3; i++) {
        uint32_t set = dma + (i == 0 ? 0x88 : i == 1 ? 0xa8 : 0xb8);
        uint32_t mask = i == 2 ? 3 : 0xff;

        qtest_writel(s, set, 0xffffffff);
        g_assert_cmphex(qtest_readl(s, set), ==, mask);
        qtest_writel(s, set + 4, 1);
        g_assert_cmphex(qtest_readl(s, set), ==, mask & ~1);
        qtest_writel(s, set + 4, 0xffffffff);
        g_assert_cmphex(qtest_readl(s, set), ==, 0);
    }
    for (unsigned channel = 0; channel < 8; channel++) {
        qtest_writel(s, dma + 0x200 + channel * 4, 0);
        qtest_writel(s, dma + 0x220 + channel * 4, 0x48486000 + channel * 16);
        g_assert_cmphex(qtest_readl(s, dma + 0x220 + channel * 4), ==,
                        0x48486000 + channel * 16);
    }
    g_assert_cmphex(qtest_readl(s, dma + 0x084), ==, 0);
    g_assert_cmphex(qtest_readl(s, dma + 0x0a4), ==, 0);
    qtest_writel(s, 0x48485214, 0xff);
    qtest_writel(s, 0x48484008, 1);
    g_assert_cmphex(qtest_readl(s, dma + 0x004), ==, 1);
    g_assert_cmphex(qtest_readl(s, dma + 0x220), ==, 0x48486000);
    g_assert_cmphex(qtest_readl(s, 0x48485214), ==, 0xff);
    qtest_writel(s, dma + 0x01c, 1);
    g_assert_cmphex(qtest_readl(s, dma + 0x004), ==, 0);
    g_assert_cmphex(qtest_readl(s, dma + 0x220), ==, 0);
    qtest_writel(s, ale + 0x34, 0xffffffff);
    qtest_writel(s, ale + 0x38, 0x12345678);
    qtest_writel(s, ale + 0x3c, 0xabcdef01);
    qtest_writel(s, ale + 0x20, 0x800003ff);
    g_assert_cmphex(qtest_readl(s, ale + 0x20), ==, 1023);
    qtest_writel(s, ale + 0x20, 0);
    g_assert_cmphex(qtest_readl(s, ale + 0x38), ==, 0);
    qtest_writel(s, ale + 0x20, 1023);
    g_assert_cmphex(qtest_readl(s, ale + 0x34), ==, 0xff);
    g_assert_cmphex(qtest_readl(s, ale + 0x38), ==, 0x12345678);
    g_assert_cmphex(qtest_readl(s, ale + 0x3c), ==, 0xabcdef01);
    qtest_writel(s, ale + 0x08, 0xc0000000);
    g_assert_cmphex(qtest_readl(s, ale + 0x08), ==, 0x80000000);
    qtest_writel(s, ale + 0x20, 1023);
    g_assert_cmphex(qtest_readl(s, ale + 0x38), ==, 0);
    qtest_writel(s, ale + 0x40, 0xffffffff);
    g_assert_cmphex(qtest_readl(s, ale + 0x40), ==, 0xffff003f);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, ale + 0x08), ==, 0);
    g_assert_cmphex(qtest_readl(s, ale + 0x40), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x48485214), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x48484118), ==, 0x33221100);
    qtest_quit(s);
}

static void test_pbias_controls(void)
{
    g_autofree char *logfile = NULL;
    g_autofree char *contents = NULL;
    g_autoptr(GError) error = NULL;
    int fd = g_file_open_tmp("mh2p-rcc-pbias-XXXXXX", &logfile, &error);
    g_autofree char *options = NULL;
    QTestState *s;
    const uint32_t pbias = 0x4a002e00;

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    options = g_strdup_printf("-d unimp -D %s", logfile);
    s = start_rcc_options(options);
    g_assert_cmphex(qtest_readl(s, pbias), ==, 0x00200000);
    qtest_writel(s, pbias, 0);
    g_assert_cmphex(qtest_readl(s, pbias), ==, 0);
    qtest_writel(s, pbias, 0x04200000);
    g_assert_cmphex(qtest_readl(s, pbias), ==, 0x04200000);
    qtest_writel(s, pbias, 0xf1dfffff); /* Read-only fields are ignored. */
    g_assert_cmphex(qtest_readl(s, pbias), ==, 0);
    qtest_writew(s, pbias, 0xffff);
    g_assert_cmphex(qtest_readl(s, pbias), ==, 0);
    qtest_writel(s, pbias, 0x0c200000); /* Original IPL power request. */
    g_assert_cmphex(qtest_readl(s, pbias), ==, 0); /* Unsupported status. */
    qtest_writel(s, pbias, 0x04200000);
    g_assert_cmphex(qtest_readl(s, pbias), ==, 0x04200000);
    qtest_writel(s, pbias, 0x02200000); /* Hi-Z error status unsupported. */
    g_assert_cmphex(qtest_readl(s, pbias), ==, 0);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, pbias), ==, 0x00200000);
    qtest_quit(s);
    g_assert_true(g_file_get_contents(logfile, &contents, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(contents, "ignoring read-only PBIAS bits"));
    g_assert_nonnull(strstr(contents,
                           "unsupported read at 0x4a002e00 size=4"));
    g_assert_nonnull(strstr(contents,
                           "unsupported write at 0x4a002e00 size=2"
                           " value=0xffff cpu=-1 pc=0x0"));
    unlink(logfile);
}

static void test_ddr_pad_controls(void)
{
    static const struct {
        uint32_t address, reset, original, expected;
    } regs[] = {
        { 0x4a002e30, 0x4a4a4a4a, 0x60606080, 0x60606080 },
        { 0x4a002e38, 0x4a4a4a4a, 0x40404040, 0x40404040 },
        { 0x4a002e3c, 0x4a4a4a4a, 0x40404040, 0x40404040 },
        { 0x4a002e48, 0x004a4a00, 0x00808000, 0x00808000 },
        { 0x4a002e50, 0x0009ce60, 0xa2084210, 0x00084260 },
        { 0x4ae0c144, 0x00000107, 0x0001c1a7, 0x0001c1a7 },
    };
    QTestState *s = start_rcc();

    for (unsigned i = 0; i < ARRAY_SIZE(regs); i++) {
        g_assert_cmphex(qtest_readl(s, regs[i].address), ==, regs[i].reset);
        qtest_writel(s, regs[i].address, regs[i].original);
        g_assert_cmphex(qtest_readl(s, regs[i].address), ==, regs[i].expected);
        qtest_writew(s, regs[i].address, 0);
        g_assert_cmphex(qtest_readl(s, regs[i].address), ==, regs[i].expected);
        if (i < 4) {
            qtest_writel(s, regs[i].address, 0x00e0e000);
            g_assert_cmphex(qtest_readl(s, regs[i].address), ==,
                            regs[i].expected);
        }
    }
    qtest_system_reset(s);
    for (unsigned i = 0; i < ARRAY_SIZE(regs); i++) {
        g_assert_cmphex(qtest_readl(s, regs[i].address), ==, regs[i].reset);
    }
    qtest_quit(s);
}

static void test_boot_devices(void)
{
    static const uint32_t uart[] = {
        0x4806a000, 0x4806c000, 0x48020000, 0x4806e000,
        0x48068000, 0x48066000, 0x48420000, 0x48422000, 0x48424000,
        0x4ae2b000,
    };
    QTestState *s = start_rcc();

    g_assert_cmphex(qtest_readl(s, 0x80100100), ==, 0xeafffffe);
    g_assert_cmphex(qtest_readl(s, 0x4ae0c204), ==, 0x0b9bc02f);
    qtest_writel(s, 0x4ae0c204, 0);
    g_assert_cmphex(qtest_readl(s, 0x4ae0c204), ==, 0x0b9bc02f);
    g_assert_cmphex(qtest_readl(s, 0x48211004) & 0x1f, ==, 5);
    qtest_writel(s, 0x80000000, 0x12345678);
    g_assert_cmphex(qtest_readl(s, 0x80000000), ==, 0x12345678);
    qtest_writel(s, 0x8ffffffc, 0xabcdef01);
    g_assert_cmphex(qtest_readl(s, 0x8ffffffc), ==, 0xabcdef01);
    for (unsigned i = 0; i < ARRAY_SIZE(uart); i++) {
        g_assert_cmphex(qtest_readb(s, uart[i] + 0x20), ==, 7);
        qtest_writeb(s, uart[i] + 0x20, 0);
        g_assert_cmphex(qtest_readb(s, uart[i] + 0x20), ==, 0);
        qtest_writeb(s, uart[i] + 0x20, 1);
        g_assert_cmphex(qtest_readb(s, uart[i] + 0x20), ==, 0);
        qtest_writel(s, uart[i] + 0x1c, 0x50 + i);
        g_assert_cmphex(qtest_readl(s, uart[i] + 0x1c), ==, 0x50 + i);
        g_assert_cmphex(qtest_readl(s, uart[i] + 0x14) & 0x60, ==, 0x60);
    }
    qtest_writel(s, 0x80100100, 0);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, 0x80100100), ==, 0xeafffffe);
    for (unsigned i = 0; i < ARRAY_SIZE(uart); i++) {
        g_assert_cmphex(qtest_readb(s, uart[i] + 0x20), ==, 7);
        g_assert_cmphex(qtest_readl(s, uart[i] + 0x1c), ==, 0);
    }
    qtest_quit(s);
}

static void test_clock_watchdog_dma(void)
{
    QTestState *s = start_rcc();

    g_assert_cmphex(qtest_readl(s, 0x4ae06110), ==, 2);
    g_assert_cmphex(qtest_readl(s, 0x4a005164), ==, 0x11);
    g_assert_cmphex(qtest_readl(s, 0x4a00516c), ==, (500 << 8) | 9);
    g_assert_cmphex(qtest_readl(s, 0x4a005170), ==, 1);
    qtest_writel(s, 0x4a00516c, 0); /* Read-only bootloader clock handoff. */
    g_assert_cmphex(qtest_readl(s, 0x4a00516c), ==, (500 << 8) | 9);
    qtest_writel(s, 0x4ae14048, 0x5555); /* Stop must start with AAAA. */
    g_assert_cmphex(qtest_readl(s, 0x4ae14048), ==, 0);
    qtest_writel(s, 0x4ae14048, 0xaaaa);
    g_assert_cmphex(qtest_readl(s, 0x4ae14034), ==, 0);
    qtest_writel(s, 0x4ae14048, 0x5555);
    g_assert_cmphex(qtest_readl(s, 0x4ae14048), ==, 0x5555);
    qtest_writel(s, 0x4ae14048, 0xbbbb); /* Enabling is unsupported. */
    g_assert_cmphex(qtest_readl(s, 0x4ae14048), ==, 0x5555);
    g_assert_cmphex(qtest_readl(s, 0x4a056078), ==, 0x10010);
    qtest_writel(s, 0x4a056078, 0xff0010);
    g_assert_cmphex(qtest_readl(s, 0x4a056078), ==, 0xff0010);
    qtest_writel(s, 0x4a05602c, 0x2011);
    g_assert_cmphex(qtest_readl(s, 0x4a05602c), ==, 0x2011);
    for (unsigned i = 0; i < 4; i++) {
        qtest_writel(s, 0x4a056008 + i * 4, UINT32_MAX);
        g_assert_cmphex(qtest_readl(s, 0x4a056008 + i * 4), ==, 0);
        qtest_writel(s, 0x4a056018 + i * 4, 1U << i);
        g_assert_cmphex(qtest_readl(s, 0x4a056018 + i * 4), ==, 1U << i);
    }
    for (unsigned i = 0; i < 32; i++) {
        uint32_t channel = 0x4a056080 + i * 0x60;

        qtest_writel(s, channel, i);
        qtest_writel(s, channel + 8, i + 1);
        qtest_writel(s, channel + 12, 0x5dfe);
        g_assert_cmphex(qtest_readl(s, channel), ==, i);
        g_assert_cmphex(qtest_readl(s, channel + 8), ==, i + 1);
        g_assert_cmphex(qtest_readl(s, channel + 12), ==, 0x5dfe);
        qtest_writel(s, channel, 0x80);
        g_assert_cmphex(qtest_readl(s, channel), ==, i);
    }
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, 0x4ae14048), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x4a056078), ==, 0x10010);
    g_assert_cmphex(qtest_readl(s, 0x4a05602c), ==, 0);
    for (unsigned i = 0; i < 4; i++) {
        g_assert_cmphex(qtest_readl(s, 0x4a056018 + i * 4), ==, 0);
    }
    for (unsigned i = 0; i < 32; i++) {
        uint32_t channel = 0x4a056080 + i * 0x60;

        g_assert_cmphex(qtest_readl(s, channel), ==, 0);
        g_assert_cmphex(qtest_readl(s, channel + 8), ==, 0);
        g_assert_cmphex(qtest_readl(s, channel + 12), ==, 0);
    }
    qtest_quit(s);
}

static void test_sdma_index_configuration(void)
{
    static const uint32_t elements[] = { 1, 0xffff, 0x8000, 0x7fff };
    static const uint32_t frames[] = {
        1, 0xffffffff, 0x80000000, 0x7fffffff,
    };
    QTestState *s = start_rcc();

    for (unsigned boot = 0; boot < 2; boot++) {
        for (unsigned i = 0; i < 32; i++) {
            uint32_t channel = 0x4a056080 + i * 0x60;

            for (unsigned reg = 0; reg < 4; reg++) {
                g_assert_cmphex(qtest_readl(s, channel + 0x24 + reg * 4),
                                ==, 0);
            }
            for (unsigned j = 0; j < ARRAY_SIZE(elements); j++) {
                for (unsigned reg = 0; reg < 4; reg++) {
                    uint32_t address = channel + 0x24 + reg * 4;
                    uint32_t value = ((reg & 1) ? frames[j] : elements[j]) ^
                                     (i * 4 + reg);

                    qtest_writel(s, address, value);
                    g_assert_cmphex(qtest_readl(s, address), ==, value);
                    if (!(reg & 1)) {
                        qtest_writel(s, address, value | 0x10000);
                    }
                    qtest_writeb(s, address, 0);
                    qtest_writew(s, address, 0);
                    g_assert_cmphex(qtest_readl(s, address), ==, value);
                }
            }
            qtest_writel(s, channel, 0x80); /* No DMA execution engine. */
            g_assert_cmphex(qtest_readl(s, channel), ==, 0);
            g_assert_cmphex(qtest_readl(s, channel + 12), ==, 0);
        }
        qtest_writel(s, 0x4a05602c, 2); /* Software reset still unsupported. */
        for (unsigned i = 0; i < 32; i++) {
            uint32_t channel = 0x4a056080 + i * 0x60;

            for (unsigned reg = 0; reg < 4; reg++) {
                uint32_t value = ((reg & 1) ? 0x7fffffff : 0x7fff) ^
                                 (i * 4 + reg);

                g_assert_cmphex(qtest_readl(s, channel + 0x24 + reg * 4),
                                ==, value);
            }
        }
        for (unsigned i = 0; i < 4; i++) {
            g_assert_cmphex(qtest_readl(s, 0x4a056008 + i * 4), ==, 0);
        }
        qtest_system_reset(s);
    }
    qtest_quit(s);
}

static void test_uart_disabled_data(void)
{
    QTestState *s = start_rcc();
    const uint32_t uart = 0x48020000;

    qtest_writeb(s, uart + 0x40, 0);
    g_assert_cmphex(qtest_readb(s, uart + 0x40), ==, 0);
    qtest_writeb(s, uart + 0x40, 0x10);
    g_assert_cmphex(qtest_readl(s, uart + 0x40), ==, 0);
    g_assert_cmphex(qtest_readb(s, uart + 0x44), ==, 4);
    qtest_writeb(s, uart + 0x10, 0x10); /* Internal loopback. */
    qtest_writeb(s, uart, 'A');
    qtest_clock_step(s, 10000000);
    g_assert_cmphex(qtest_readb(s, uart + 0x14) & 1, ==, 0);
    qtest_writeb(s, uart + 0x20, 0);
    qtest_writeb(s, uart, 'B');
    qtest_clock_step(s, 10000000);
    g_assert_cmphex(qtest_readb(s, uart + 0x14) & 1, ==, 1);
    qtest_writeb(s, uart + 0x20, 7);
    g_assert_cmphex(qtest_readb(s, uart), ==, 0);
    qtest_writeb(s, uart + 0x20, 0);
    g_assert_cmphex(qtest_readb(s, uart), ==, 'B');
    qtest_quit(s);
}

static void test_gpio_outputs_reset(void)
{
    static const uint32_t base[] = {
        0x4ae10000, 0x48055000, 0x48057000, 0x48059000,
        0x4805b000, 0x4805d000, 0x48051000, 0x48053000,
    };
    QTestState *s = start_rcc();

    qtest_irq_intercept_out_named(s, "/machine/gpio1", "pins");
    for (unsigned i = 0; i < ARRAY_SIZE(base); i++) {
        g_assert_cmphex(qtest_readl(s, base[i] + 0x134), ==, UINT32_MAX);
        qtest_writel(s, base[i] + 0x13c, 0x80000000U | i);
    }
    g_assert_false(qtest_get_irq(s, 31));
    qtest_writel(s, base[0] + 0x134, 0x7fffffff);
    g_assert_true(qtest_get_irq(s, 31));
    qtest_writel(s, base[0] + 0x190, 0x80000000);
    g_assert_false(qtest_get_irq(s, 31));
    qtest_writel(s, base[0] + 0x194, 0x80000000);
    g_assert_true(qtest_get_irq(s, 31));
    qtest_writel(s, base[0] + 0x130, 1);
    g_assert_false(qtest_get_irq(s, 31));
    qtest_writel(s, base[0] + 0x130, 0);
    g_assert_true(qtest_get_irq(s, 31));
    for (unsigned i = 0; i < ARRAY_SIZE(base); i++) {
        g_assert_cmphex(qtest_readl(s, base[i] + 0x13c), ==, 0x80000000U | i);
    }
    qtest_system_reset(s);
    g_assert_false(qtest_get_irq(s, 31));
    for (unsigned i = 0; i < ARRAY_SIZE(base); i++) {
        g_assert_cmphex(qtest_readl(s, base[i] + 0x13c), ==, 0);
        g_assert_cmphex(qtest_readl(s, base[i] + 0x134), ==, UINT32_MAX);
    }
    qtest_quit(s);
}

static void test_gpio_interrupts(void)
{
    QTestState *s = start_rcc();
    const uint32_t base = 0x4ae10000;
    const uint32_t pin = 0x80000000;

    qtest_irq_intercept_out_named(s, "/machine/gpio1", "sysbus-irq");
    qtest_writel(s, base + 0x148, pin);
    qtest_set_irq_in(s, "/machine/gpio1", NULL, 31, 1);
    g_assert_cmphex(qtest_readl(s, base + 0x138), ==, pin);
    g_assert_cmphex(qtest_readl(s, base + 0x24), ==, pin);
    g_assert_cmphex(qtest_readl(s, base + 0x2c), ==, 0);
    g_assert_false(qtest_get_irq(s, 0));
    qtest_writel(s, base + 0x34, pin);
    qtest_writel(s, base + 0x38, pin);
    g_assert_true(qtest_get_irq(s, 0));
    g_assert_true(qtest_get_irq(s, 1));
    qtest_writel(s, base + 0x2c, pin);
    g_assert_false(qtest_get_irq(s, 0));
    g_assert_true(qtest_get_irq(s, 1));
    qtest_writel(s, base + 0x40, pin);
    g_assert_false(qtest_get_irq(s, 1));
    qtest_writel(s, base + 0x38, pin);
    g_assert_true(qtest_get_irq(s, 1));
    qtest_writel(s, base + 0x30, pin);
    g_assert_false(qtest_get_irq(s, 1));
    qtest_writel(s, base + 0x14c, pin);
    qtest_set_irq_in(s, "/machine/gpio1", NULL, 31, 0);
    g_assert_true(qtest_get_irq(s, 0));
    g_assert_true(qtest_get_irq(s, 1));
    qtest_writel(s, base + 0x2c, pin);
    qtest_writel(s, base + 0x30, pin);
    qtest_writel(s, base + 0x140, pin);
    g_assert_true(qtest_get_irq(s, 0));
    qtest_writel(s, base + 0x2c, pin);
    g_assert_true(qtest_get_irq(s, 0)); /* Active level remains asserted. */
    qtest_writel(s, base + 0x140, 0);
    g_assert_false(qtest_get_irq(s, 0));
    qtest_set_irq_in(s, "/machine/gpio1", NULL, 31, 1);
    /* Soft reset retains the externally driven input. */
    qtest_writel(s, base + 0x10, 0xb);
    g_assert_cmphex(qtest_readl(s, base + 0x10), ==, 9);
    g_assert_cmphex(qtest_readl(s, base + 0x114), ==, 1);
    g_assert_cmphex(qtest_readl(s, base + 0x138), ==, pin);
    g_assert_cmphex(qtest_readl(s, base + 0x148), ==, 0);
    g_assert_cmphex(qtest_readl(s, base + 0x14c), ==, 0);
    g_assert_cmphex(qtest_readl(s, base + 0x24), ==, 0);
    g_assert_cmphex(qtest_readl(s, base + 0x34), ==, 0);
    g_assert_false(qtest_get_irq(s, 0));
    g_assert_false(qtest_get_irq(s, 1));
    qtest_quit(s);
}

static void test_gpio6_wakeup_input_readback(void)
{
    QTestState *s = start_rcc();
    const uint32_t base = 0x4805d000;
    const uint32_t pin21 = 1U << 21;
    const uint32_t pin22 = 1U << 22;

    g_assert_cmphex(qtest_readl(s, base + 0x138), ==, 0);
    qtest_set_irq_in(s, "/machine/gpio6", NULL, 21, 1);
    g_assert_cmphex(qtest_readl(s, base + 0x138), ==, pin21);
    qtest_set_irq_in(s, "/machine/gpio6", NULL, 22, 1);
    g_assert_cmphex(qtest_readl(s, base + 0x138), ==, pin21 | pin22);
    qtest_set_irq_in(s, "/machine/gpio6", NULL, 21, 0);
    g_assert_cmphex(qtest_readl(s, base + 0x138), ==, pin22);
    qtest_set_irq_in(s, "/machine/gpio6", NULL, 22, 0);
    g_assert_cmphex(qtest_readl(s, base + 0x138), ==, 0);
    qtest_quit(s);
}

static uint32_t mailbox_base(unsigned number)
{
    return number == 1 ? 0x4a0f4000 : 0x48840000 + (number - 5) * 0x2000;
}

static void test_mailbox_fifo(gconstpointer data)
{
    unsigned number = GPOINTER_TO_UINT(data);
    unsigned fifos = number == 1 ? 8 : 12;
    QTestState *s = start_rcc();
    const uint32_t base = mailbox_base(number);

    qtest_writel(s, base + 0x10, 9);
    g_assert_cmphex(qtest_readl(s, base + 0x10), ==, 8);
    for (unsigned fifo = 0; fifo < fifos; fifo++) {
        uint32_t message = base + 0x40 + fifo * 4;
        uint32_t full = base + 0x80 + fifo * 4;
        uint32_t count = base + 0xc0 + fifo * 4;

        for (unsigned i = 0; i < 4; i++) {
            g_assert_cmphex(qtest_readl(s, count), ==, i);
            g_assert_cmphex(qtest_readl(s, full), ==, 0);
            qtest_writel(s, message, (fifo << 16) | i);
        }
        g_assert_cmphex(qtest_readl(s, full), ==, 1);
        qtest_writel(s, message, 0xdeadbeef); /* Overflow must not overwrite. */
        g_assert_cmphex(qtest_readl(s, count), ==, 4);
    }
    for (unsigned fifo = 0; fifo < fifos; fifo++) {
        uint32_t message = base + 0x40 + fifo * 4;

        for (unsigned i = 0; i < 2; i++) {
            g_assert_cmphex(qtest_readl(s, message), ==, (fifo << 16) | i);
        }
        qtest_writel(s, message, (fifo << 16) | 4);
        qtest_writel(s, message, (fifo << 16) | 5);
        for (unsigned i = 2; i < 6; i++) {
            g_assert_cmphex(qtest_readl(s, message), ==, (fifo << 16) | i);
        }
        g_assert_cmphex(qtest_readl(s, base + 0xc0 + fifo * 4), ==, 0);
        g_assert_cmphex(qtest_readl(s, message), ==, 0);
        g_assert_cmphex(qtest_readl(s, base + 0xc0 + fifo * 4), ==, 0);
        qtest_writel(s, message, fifo + 1);
    }
    qtest_system_reset(s);
    for (unsigned fifo = 0; fifo < fifos; fifo++) {
        g_assert_cmphex(qtest_readl(s, base + 0xc0 + fifo * 4), ==, 0);
    }
    g_assert_cmphex(qtest_readl(s, base + 0x10), ==, number == 1 ? 0 : 8);
    qtest_quit(s);
}

static void test_mailbox_interrupts(gconstpointer data)
{
    unsigned number = GPOINTER_TO_UINT(data);
    QTestState *s = start_rcc();
    const uint32_t base = mailbox_base(number);
    g_autofree char *path = g_strdup_printf("/machine/mailbox%u", number);

    qtest_irq_intercept_out_named(s, path, "sysbus-irq");
    g_assert_cmphex(qtest_readl(s, base + 0x100), ==,
                    number == 1 ? 0xaaaa : 0xaaaaaa);
    qtest_writel(s, base + 0x108, 1); /* User 0: FIFO 0 new message. */
    qtest_writel(s, base + 0x128, 1); /* User 2: same FIFO, independent mask. */
    g_assert_false(qtest_get_irq(s, 0));
    qtest_writel(s, base + 0x40, 0x12345678);
    g_assert_true(qtest_get_irq(s, 0));
    g_assert_true(qtest_get_irq(s, 2));
    qtest_writel(s, base + 0x104, 1);
    g_assert_true(qtest_get_irq(s, 0)); /* Still a message in the FIFO. */
    g_assert_cmphex(qtest_readl(s, base + 0x40), ==, 0x12345678);
    qtest_writel(s, base + 0x104, 1);
    g_assert_false(qtest_get_irq(s, 0));
    g_assert_true(qtest_get_irq(s, 2));
    qtest_writel(s, base + 0x124, 1);
    g_assert_false(qtest_get_irq(s, 2));
    qtest_writel(s, base + 0x118, 2); /* User 1: FIFO 0 not full. */
    g_assert_true(qtest_get_irq(s, 1));
    for (unsigned i = 0; i < 4; i++) {
        qtest_writel(s, base + 0x40, i);
    }
    qtest_writel(s, base + 0x114, 2);
    g_assert_false(qtest_get_irq(s, 1));
    qtest_writel(s, base + 0x40, 0xdeadbeef);
    g_assert_cmphex(qtest_readl(s, base + 0xc0), ==, 4);
    g_assert_false(qtest_get_irq(s, 1));
    g_assert_cmphex(qtest_readl(s, base + 0x40), ==, 0);
    g_assert_true(qtest_get_irq(s, 1));
    qtest_writel(s, base + 0x11c, 2);
    g_assert_false(qtest_get_irq(s, 1));
    qtest_writel(s, base + 0x118, 2);
    g_assert_true(qtest_get_irq(s, 1));
    qtest_writel(s, base + 0x10, 9);
    for (unsigned user = 0; user < 3; user++) {
        g_assert_false(qtest_get_irq(s, user));
        g_assert_cmphex(qtest_readl(s, base + 0x108 + 0x10 * user), ==, 0);
    }
    g_assert_cmphex(qtest_readl(s, base + 0xc0), ==, 0);
    qtest_quit(s);
}

static void test_dra7_mailbox_routing(gconstpointer data)
{
    static const unsigned destinations[] = { 4, 7, 8, 9 };
    unsigned number = GPOINTER_TO_UINT(data);
    const uint32_t base = mailbox_base(number);
    unsigned source = 249 + (number - 5) * 4;
    QTestState *s = start_rcc();

    g_assert_cmphex(qtest_readl(s, base + 0x10), ==, 8);
    qtest_irq_intercept_in(s, "/machine/interrupt-controller");
    qtest_writel(s, 0x4a002a48, source | ((source + 1) << 16));
    qtest_writel(s, 0x4a002a4c, (source + 2) | ((source + 3) << 16));
    for (unsigned user = 0; user < 4; user++) {
        uint32_t irq = base + 0x100 + user * 0x10;
        uint32_t bit = 1 << (2 * (8 + user));

        qtest_writel(s, irq + 8, UINT32_MAX);
        g_assert_cmphex(qtest_readl(s, irq + 8), ==, 0xffffff);
        qtest_writel(s, irq + 12, UINT32_MAX);
        qtest_writel(s, irq + 8, bit);
        g_assert_false(qtest_get_irq(s, destinations[user]));
        qtest_writel(s, base + 0x40 + (8 + user) * 4, 0x12340000 + user);
        g_assert_true(qtest_get_irq(s, destinations[user]));
        qtest_writel(s, irq + 4, bit);
        g_assert_true(qtest_get_irq(s, destinations[user]));
        g_assert_cmphex(qtest_readl(s, base + 0x40 + (8 + user) * 4),
                        ==, 0x12340000 + user);
        qtest_writel(s, irq + 4, bit);
        g_assert_false(qtest_get_irq(s, destinations[user]));
        qtest_writel(s, irq, bit);
        g_assert_true(qtest_get_irq(s, destinations[user]));
        qtest_writel(s, irq + 4, bit);
        g_assert_false(qtest_get_irq(s, destinations[user]));
    }
    qtest_writel(s, base + 0x6c, 0xaabbccdd);
    g_assert_cmphex(qtest_readl(s, mailbox_base(number == 5 ? 6 : 5) + 0xec),
                    ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x4a0f40c0), ==, 0);
    qtest_writel(s, base + 0x10, 1);
    for (unsigned user = 0; user < 4; user++) {
        g_assert_cmphex(qtest_readl(s, base + 0x108 + user * 0x10), ==, 0);
        g_assert_false(qtest_get_irq(s, destinations[user]));
    }
    g_assert_cmphex(qtest_readl(s, base + 0x10), ==, 0);
    qtest_writel(s, base + 0x6c, 0xabcdef);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, base + 0xec), ==, 0);
    g_assert_cmphex(qtest_readl(s, base + 0x10), ==, 8);
    qtest_quit(s);
}

static void test_iodelay_handoff(void)
{
    QTestState *s = start_rcc();
    const uint32_t base = 0x4844a000;
    uint32_t period = qtest_readl(s, base + 0x14);
    uint32_t coarse = qtest_readl(s, base + 0x18);
    uint32_t fine = qtest_readl(s, base + 0x1c);

    g_assert_cmpuint(5 * period * (coarse & 0xffff) / ((coarse >> 16) * 88),
                     ==, 920);
    g_assert_cmpuint(5 * period * (fine & 0xffff) / ((fine >> 16) * 264),
                     ==, 60);
    g_assert_cmphex(qtest_readl(s, base + 0x2c), ==, 0xaaab);
    qtest_writel(s, base + 0x6f0, 0x29405);
    g_assert_cmphex(qtest_readl(s, base + 0x6f0), ==, 0);
    qtest_writel(s, base + 0x2c, 0xaaaa);
    g_assert_cmphex(qtest_readl(s, base + 0x2c), ==, 0xaaaa);
    qtest_writel(s, base + 0x6f0, 5); /* Signature is mandatory. */
    g_assert_cmphex(qtest_readl(s, base + 0x6f0), ==, 0);
    qtest_writel(s, base + 0x6f0, 0x29405);
    g_assert_cmphex(qtest_readl(s, base + 0x6f0), ==, 0x29405);
    qtest_writel(s, base + 0x6f0, 0x29406); /* Per-pad lock stays set. */
    g_assert_cmphex(qtest_readl(s, base + 0x6f0), ==, 0x29405);
    qtest_writel(s, base + 0x30, 0x29001);
    qtest_writel(s, base + 0xd1c, 0x29002);
    g_assert_cmphex(qtest_readl(s, base + 0x30), ==, 0x29001);
    g_assert_cmphex(qtest_readl(s, base + 0xd1c), ==, 0x29002);
    qtest_writel(s, base + 0x2c, 0xaaab);
    qtest_writel(s, base + 0x30, 0x29002);
    g_assert_cmphex(qtest_readl(s, base + 0x30), ==, 0x29001);
    qtest_writel(s, base + 0x14, 0); /* Handoff calibration is read-only. */
    g_assert_cmphex(qtest_readl(s, base + 0x14), ==, period);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, base + 0x2c), ==, 0xaaab);
    g_assert_cmphex(qtest_readl(s, base + 0x6f0), ==, 0);
    g_assert_cmphex(qtest_readl(s, base + 0x30), ==, 0);
    g_assert_cmphex(qtest_readl(s, base + 0xd1c), ==, 0);
    qtest_quit(s);
}

static void test_edma_quiescent_configuration(void)
{
    static const uint32_t clear[] = {
        0x308, 0x30c, 0x314, 0x31c,
        0x1008, 0x100c, 0x1028, 0x102c, 0x1040, 0x1044,
        0x1058, 0x105c, 0x1070, 0x1074, 0x1088, 0x1094,
    };
    QTestState *s = start_rcc();
    const uint32_t base = 0x43300000;

    for (unsigned i = 0; i < 64; i++) {
        qtest_writel(s, base + 0x100 + i * 4, i << 5);
    }
    for (unsigned i = 0; i < 64; i++) {
        g_assert_cmphex(qtest_readl(s, base + 0x100 + i * 4), ==, i << 5);
    }
    qtest_writel(s, base + 0x100, 1); /* Misaligned PaRAM mapping rejected. */
    g_assert_cmphex(qtest_readl(s, base + 0x100), ==, 0);
    qtest_writel(s, base + 0x340, 0xff3fffff);
    qtest_writel(s, base + 0x344, UINT32_MAX);
    qtest_writel(s, base + 0x380, 0xff);
    g_assert_cmphex(qtest_readl(s, base + 0x340), ==, 0xff3fffff);
    g_assert_cmphex(qtest_readl(s, base + 0x344), ==, UINT32_MAX);
    g_assert_cmphex(qtest_readl(s, base + 0x380), ==, 0xff);
    for (unsigned i = 0; i < ARRAY_SIZE(clear); i++) {
        qtest_writel(s, base + clear[i], UINT32_MAX);
    }
    qtest_writel(s, base + 0x1030, 1); /* Enabling DMA remains unsupported. */
    qtest_writel(s, base + 0x1010, 1); /* No software event processing. */
    g_assert_cmphex(qtest_readl(s, base + 0x1020), ==, 0);
    g_assert_cmphex(qtest_readl(s, base + 0x1000), ==, 0);
    g_assert_cmphex(qtest_readl(s, base + 0x1068), ==, 0);
    qtest_writel(s, base + 0x1308, UINT32_MAX);
    qtest_writel(s, base + 0x130c, UINT32_MAX);
    qtest_writel(s, base + 0x1314, UINT32_MAX);
    g_assert_cmphex(qtest_readl(s, base + 0x1308), ==, 0);
    g_assert_cmphex(qtest_readl(s, base + 0x640), ==, 0);
    g_assert_cmphex(qtest_readl(s, base + 0x340), ==, 0xff3fffff);
    qtest_system_reset(s);
    for (unsigned i = 0; i < 64; i++) {
        g_assert_cmphex(qtest_readl(s, base + 0x100 + i * 4), ==, 0);
    }
    g_assert_cmphex(qtest_readl(s, base + 0x340), ==, 0);
    g_assert_cmphex(qtest_readl(s, base + 0x344), ==, 0);
    g_assert_cmphex(qtest_readl(s, base + 0x380), ==, 0);
    qtest_quit(s);
}

static void test_edma_qdma_configuration(void)
{
    QTestState *s = start_rcc();
    const uint32_t base = 0x43300000;

    for (unsigned i = 0; i < 8; i++) {
        uint32_t mapping = ((448 + i * 8) << 5) | (7 << 2);

        qtest_writel(s, base + 0x200 + i * 4, mapping);
        g_assert_cmphex(qtest_readl(s, base + 0x200 + i * 4), ==, mapping);
    }
    qtest_writel(s, base + 0x218, 1);
    g_assert_cmphex(qtest_readl(s, base + 0x218), ==, (496 << 5) | 28);
    for (unsigned i = 0; i < 512; i++) {
        qtest_writel(s, base + 0x4000 + i * 32, 0x100000 + i);
        g_assert_cmphex(qtest_readl(s, base + 0x4000 + i * 32),
                        ==, 0x100000 + i);
    }
    for (unsigned i = 0; i < 9; i++) {
        uint32_t bank = i ? 0x2000 + (i - 1) * 0x100 : 0x1000;

        qtest_writel(s, base + bank + 0x60, 0x1234);
        qtest_writel(s, base + bank + 0x64, 0xc0000000);
        qtest_writel(s, base + bank + 0x58, 0x34);
        g_assert_cmphex(qtest_readl(s, base + bank + 0x50), ==, 0x1200);
        g_assert_cmphex(qtest_readl(s, base + bank + 0x54), ==, 0xc0000000);
        qtest_writel(s, base + bank + 0x8c, 0xc0);
        qtest_writel(s, base + bank + 0x88, 0x80);
        g_assert_cmphex(qtest_readl(s, base + bank + 0x84), ==, 0x40);
    }
    /* Unsupported PaRAM must fault, not invent data or completions. */
    qtest_writel(s, base + 0x4000 + 496 * 32 + 28, 1);
    g_assert_cmphex(qtest_readl(s, base + 0x310), ==, 0x40);
    g_assert_cmphex(qtest_readl(s, base + 0x106c), ==, 0);
    qtest_writel(s, base + 0x314, 0x40);
    g_assert_cmphex(qtest_readl(s, base + 0x310), ==, 0);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, base + 0x218), ==, 0);
    g_assert_cmphex(qtest_readl(s, base + 0x7e1c), ==, 0);
    g_assert_cmphex(qtest_readl(s, base + 0x1054), ==, 0);
    g_assert_cmphex(qtest_readl(s, base + 0x2784), ==, 0);
    qtest_quit(s);
}

static void test_usb_clock_controls(void)
{
    static const uint32_t module[] = {
        0x4a0093f0, 0x4a009340, 0x4a009348, 0x4a009350,
    };
    static const uint32_t phy[] = { 0x4a008640, 0x4a008688 };
    QTestState *s = start_rcc();

    for (unsigned i = 0; i < ARRAY_SIZE(module); i++) {
        g_assert_cmphex(qtest_readl(s, module[i]), ==, 0x30000);
        qtest_writel(s, module[i], 0x101);
        g_assert_cmphex(qtest_readl(s, module[i]), ==, 0x101);
        qtest_writel(s, module[i], 2); /* HW-controlled modules reject SW_EN. */
        g_assert_cmphex(qtest_readl(s, module[i]), ==, 0x101);
        qtest_writel(s, module[i], 0x100);
        g_assert_cmphex(qtest_readl(s, module[i]), ==, 0x30100);
        qtest_writel(s, module[i], 1);
        g_assert_cmphex(qtest_readl(s, module[i]), ==, 1);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(phy); i++) {
        g_assert_cmphex(qtest_readl(s, phy[i]), ==, 0);
        qtest_writel(s, phy[i], 0x100);
        g_assert_cmphex(qtest_readl(s, phy[i]), ==, 0x100);
        qtest_writel(s, phy[i], 1);
        g_assert_cmphex(qtest_readl(s, phy[i]), ==, 0x100);
    }
    qtest_system_reset(s);
    for (unsigned i = 0; i < ARRAY_SIZE(module); i++) {
        g_assert_cmphex(qtest_readl(s, module[i]), ==, 0x30000);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(phy); i++) {
        g_assert_cmphex(qtest_readl(s, phy[i]), ==, 0);
    }
    qtest_quit(s);
}

static void test_usb_host_setup(void)
{
    static const uint32_t clocks[] = { 0x4a0093f0, 0x4a009340 };
    static const uint32_t phy[] = { 0x4a08404c, 0x4a08504c };
    QTestState *s = start_rcc();

    for (unsigned i = 0; i < ARRAY_SIZE(clocks); i++) {
        uint32_t base = 0x48890000 + i * 0x40000;
        uint32_t op;

        g_assert_cmphex(qtest_readl(s, base + 0xc120), ==, 0);
        qtest_writel(s, clocks[i], 0x101);
        g_assert_cmphex(qtest_readl(s, base + 0xc120), ==, 0x5533330a);
        g_assert_cmphex(qtest_readl(s, base + 0xc200), ==, 0x40102400);
        qtest_writel(s, base + 0xc200, 0x40102400);
        g_assert_cmphex(qtest_readl(s, base + 0xc200), ==, 0x40102400);
        qtest_writel(s, base + 0xc200, 0x40102440);
        g_assert_cmphex(qtest_readl(s, base + 0xc200), ==, 0x40102440);
        op = base + (qtest_readl(s, base) & 0xff);
        g_assert_cmphex(qtest_readl(s, op + 4) & 1, ==, 1);
        qtest_writel(s, op, 1);
        g_assert_cmphex(qtest_readl(s, op + 4) & 1, ==, 0);
        qtest_writel(s, op, 0);
        g_assert_cmphex(qtest_readl(s, op + 4) & 1, ==, 1);
        qtest_writel(s, base + 0xc110,
                     qtest_readl(s, base + 0xc110) | (1 << 11));
        g_assert_cmphex(qtest_readl(s, base + 0xc200), ==, 0x40102400);
        qtest_writel(s, base + 0xc200, 0x40102440);
        qtest_writel(s, clocks[i], 0);
        g_assert_cmphex(qtest_readl(s, base + 0xc120), ==, 0);
        qtest_writel(s, clocks[i], 1);
        g_assert_cmphex(qtest_readl(s, base + 0xc200), ==, 0x40102440);
        g_assert_cmphex(qtest_readl(s, phy[i]), ==, 0);
        qtest_writel(s, phy[i], 0x80000000);
        qtest_writel(s, phy[i], 1); /* Unmodeled analog settings fault. */
        g_assert_cmphex(qtest_readl(s, phy[i]), ==, 0x80000000);
    }
    g_assert_cmphex(qtest_readl(s, 0x4a0086c0), ==, 0);
    qtest_writel(s, 0x4a0086c0, 0x100);
    g_assert_cmphex(qtest_readl(s, 0x4a0086c0), ==, 0x100);
    g_assert_cmphex(qtest_readl(s, 0x4a002e74), ==, 0x10000000);
    qtest_writel(s, 0x4a002e74, 0);
    g_assert_cmphex(qtest_readl(s, 0x4a002e74), ==, 0);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, 0x4a0086c0), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x4a002e74), ==, 0x10000000);
    for (unsigned i = 0; i < ARRAY_SIZE(clocks); i++) {
        uint32_t base = 0x48890000 + i * 0x40000;

        g_assert_cmphex(qtest_readl(s, base + 0xc120), ==, 0);
        qtest_writel(s, clocks[i], 1);
        g_assert_cmphex(qtest_readl(s, base + 0xc200), ==, 0x40102400);
        g_assert_cmphex(qtest_readl(s, phy[i]), ==, 0);
    }
    qtest_quit(s);
}

static void test_pipe3_bridge_and_pll(void)
{
    static const struct {
        uint32_t m, n, fraction, range;
        bool locks;
    } vectors[] = {
        { 299, 7, 0, 2, false },
        { 300, 7, 0, 2, true },
        { 600, 7, 0, 2, true },
        { 601, 7, 0, 2, false },
        { 499, 7, 0x3ffff, 4, false },
        { 500, 7, 0, 4, true },
        { 500, 7, 1, 4, true },
        { 1000, 7, 0, 4, true },
        { 1000, 7, 1, 4, false },
        { 1001, 7, 0, 4, false },
        { 1000, 7, 0, 3, false },
        { 625, 9, 0, 4, true },
    };
    QTestState *s = start_rcc();
    const uint32_t pll = 0x4a084c00;
    const uint32_t bridge = 0x4a080000;

    g_assert_cmphex(qtest_readl(s, bridge + 0x10), ==, 0x11);
    g_assert_cmphex(qtest_readl(s, bridge + 0x14), ==, 1);
    qtest_writel(s, bridge + 0x18, 6);
    g_assert_cmphex(qtest_readl(s, bridge + 0x18), ==, 6);
    qtest_writel(s, bridge + 0x18, 0x400);
    g_assert_cmphex(qtest_readl(s, bridge + 0x18), ==, 6);
    qtest_writel(s, bridge + 0x10, 0x13);
    g_assert_cmphex(qtest_readl(s, bridge + 0x10), ==, 0x11);
    g_assert_cmphex(qtest_readl(s, bridge + 0x18), ==, 1);
    qtest_writel(s, 0x4a08481c, 0x24000000);
    g_assert_cmphex(qtest_readl(s, 0x4a08481c), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x4a084824), ==, 0);
    g_assert_cmphex(qtest_readl(s, pll + 4), ==, 1);
    qtest_writel(s, pll + 8, 1); /* No coefficients, no lock. */
    g_assert_cmphex(qtest_readl(s, pll + 4), ==, 1);
    for (unsigned i = 0; i < ARRAY_SIZE(vectors); i++) {
        uint32_t expected = vectors[i].locks ? 3 : 1;

        qtest_system_reset(s);
        g_assert_cmphex(qtest_readl(s, pll + 4), ==, 1);
        qtest_writel(s, pll + 0xc, (vectors[i].m << 9) | (vectors[i].n << 1));
        qtest_writel(s, pll + 0x10, 0x2000 | (vectors[i].range << 1));
        qtest_writel(s, pll + 0x14, 10 << 10);
        qtest_writel(s, pll + 0x20, vectors[i].fraction);
        qtest_writel(s, pll + 8, 1);
        g_assert_cmphex(qtest_readl(s, pll + 4), ==, expected);
        g_assert_cmphex(qtest_readl(s, pll + 8), ==, 0);
    }
    qtest_writel(s, pll + 0x10, 0x2009);
    g_assert_cmphex(qtest_readl(s, pll + 4), ==, 1);
    qtest_writel(s, pll + 0x10, 0x2008);
    g_assert_cmphex(qtest_readl(s, pll + 4), ==, 3);
    qtest_writel(s, 0x4a002370, 0x04c0c000);
    g_assert_cmphex(qtest_readl(s, 0x4a002370), ==, 0x04c0c000);
    qtest_writel(s, 0x4a002370, 1);
    g_assert_cmphex(qtest_readl(s, 0x4a002370), ==, 0x04c0c000);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, pll + 4), ==, 1);
    g_assert_cmphex(qtest_readl(s, pll + 0xc), ==, 0);
    g_assert_cmphex(qtest_readl(s, pll + 0x10), ==, 0x2008);
    g_assert_cmphex(qtest_readl(s, 0x4a002370), ==, 0);
    qtest_quit(s);
}

static uint32_t crossbar_test_levels(QTestState *s)
{
    return (qtest_get_irq(s, 4) << 4) | (qtest_get_irq(s, 7) << 7);
}

static void test_edma_qdma_transfer(void)
{
    QTestState *s = start_rcc();
    const uint32_t base = 0x43300000, param = base + 0x7800;
    const uint32_t source = 0x80011000, target = 0x80012000;

    qtest_irq_intercept_in(s, "/machine/interrupt-controller");
    qtest_writel(s, 0x4a002a48, (361 << 16) | 360);
    qtest_writel(s, base + 0x344, 1U << 30);
    qtest_writel(s, base + 0x1064, 1U << 30);
    qtest_writel(s, base + 0x2064, 1U << 30);
    qtest_writel(s, base + 0x218, (448 << 5) | 28);
    for (unsigned i = 0; i < 3; i++) {
        qtest_writel(s, source + i * 4, 0x11223344 + i);
    }
    qtest_writel(s, param, 0x0013e00c);
    qtest_writel(s, param + 4, source);
    qtest_writel(s, param + 8, 0x30002);
    qtest_writel(s, param + 12, target);
    qtest_writel(s, param + 16, 0x60004);
    qtest_writel(s, param + 20, 0xffff);
    qtest_writel(s, param + 24, 0);
    qtest_writel(s, param + 28, 1); /* Disabled channel cannot copy. */
    g_assert_cmphex(qtest_readl(s, target), ==, 0);
    g_assert_cmphex(crossbar_test_levels(s), ==, 0);
    qtest_writel(s, base + 0x108c, 0x40);
    qtest_writel(s, param + 28, 1);
    for (unsigned i = 0; i < 3; i++) {
        g_assert_cmphex(qtest_readw(s, target + i * 6), ==, 0x3344 + i);
        g_assert_cmphex(qtest_readl(s, target + i * 6 + 2), ==, 0);
    }
    g_assert_cmphex(qtest_readl(s, base + 0x106c), ==, 1U << 30);
    g_assert_cmphex(qtest_readl(s, base + 0x206c), ==, 1U << 30);
    g_assert_cmphex(qtest_readl(s, param + 4), ==, source);
    g_assert_cmphex(qtest_readl(s, param + 28), ==, 1);
    g_assert_cmphex(crossbar_test_levels(s), ==, 0x90);
    qtest_writel(s, base + 0x1074, 1U << 30);
    g_assert_cmphex(crossbar_test_levels(s), ==, 0x80);
    qtest_writel(s, base + 0x2074, 1U << 30);
    g_assert_cmphex(crossbar_test_levels(s), ==, 0);
    qtest_writel(s, param, 0x0003e004); /* Final non-static null link. */
    qtest_writel(s, param + 28, 1);
    g_assert_cmphex(qtest_readl(s, param), ==, 0);
    g_assert_cmphex(qtest_readl(s, param + 20), ==, 0xffff);
    g_assert_cmphex(qtest_readl(s, param + 28), ==, 0);
    qtest_writel(s, param + 32, 0x0013e00c);
    qtest_writel(s, param + 36, source + 8);
    qtest_writel(s, param + 40, 0x30004);
    qtest_writel(s, param + 44, target + 0x104);
    qtest_writel(s, param + 48, 0x4fffc); /* Signed negative source B index. */
    qtest_writel(s, param + 52, 0xffff);
    qtest_writel(s, param + 60, 1);
    qtest_writel(s, param, 0x0003e004);
    qtest_writel(s, param + 4, source);
    qtest_writel(s, param + 8, 0x10004);
    qtest_writel(s, param + 12, target + 0x100);
    qtest_writel(s, param + 16, 0);
    qtest_writel(s, param + 20, 0x7820);
    qtest_writel(s, param + 28, 1);
    g_assert_cmphex(qtest_readl(s, target + 0x100), ==, 0x11223344);
    for (unsigned i = 0; i < 3; i++) {
        g_assert_cmphex(qtest_readl(s, target + 0x104 + i * 4),
                        ==, 0x11223346 - i);
    }
    g_assert_cmphex(qtest_readl(s, param), ==, 0x0013e00c);
    g_assert_cmphex(qtest_readl(s, base + 0x310), ==, 0);
    g_assert_cmphex(crossbar_test_levels(s), ==, 0x90);
    qtest_writel(s, base + 0x1074, 1U << 30);
    qtest_writel(s, base + 0x2074, 1U << 30);
    qtest_writel(s, param + 8, 0x2ffff); /* Reject more than 64 KiB. */
    qtest_writel(s, param + 28, 1);
    g_assert_cmphex(qtest_readl(s, base + 0x310), ==, 0x40);
    g_assert_cmphex(qtest_readl(s, target + 0x104), ==, 0x11223346);
    g_assert_cmphex(crossbar_test_levels(s), ==, 0);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, base + 0x106c), ==, 0);
    g_assert_cmphex(qtest_readl(s, base + 0x206c), ==, 0);
    g_assert_cmphex(crossbar_test_levels(s), ==, 0);
    qtest_quit(s);
}

static void test_crossbar_routing(void)
{
    QTestState *s = start_rcc();
    const uint32_t xbar = 0x4a002a48;
    const uint32_t mailbox = 0x4a0f4000;
    const uint32_t gpio = 0x4ae10000;

    qtest_irq_intercept_in(s, "/machine/interrupt-controller");

    g_assert_cmphex(qtest_readl(s, xbar), ==, 0x00020001);
    g_assert_cmphex(qtest_readl(s, xbar + 0xbc), ==, 0x0060018b);
    g_assert_cmphex(qtest_readl(s, xbar + 0xf8), ==, 0x0000007d);
    g_assert_cmphex(qtest_readl(s, xbar + 0x12c), ==, 0);
    qtest_writel(s, xbar, 0xffff0015); /* Reject the whole register write. */
    g_assert_cmphex(qtest_readl(s, xbar), ==, 0x00020001);
    qtest_writew(s, xbar, 0x1ff);
    g_assert_cmphex(qtest_readl(s, xbar), ==, 0x000201ff);
    qtest_writeb(s, xbar, 0);
    g_assert_cmphex(qtest_readw(s, xbar), ==, 0x1ff);
    qtest_writew(s, xbar + 1, 0);
    g_assert_cmphex(qtest_readl(s, xbar), ==, 0x000201ff);
    qtest_writel(s, xbar, 0x00150015); /* Fan out mailbox user 0 to MPU 4/7. */
    qtest_writel(s, mailbox + 0x108, 1);
    qtest_writel(s, mailbox + 0x40, 0x12345678);
    g_assert_cmphex(crossbar_test_levels(s), ==, 0x90);
    qtest_writew(s, xbar, 0);
    g_assert_cmphex(crossbar_test_levels(s), ==, 0x80);
    qtest_writew(s, xbar + 2, 0);
    g_assert_cmphex(crossbar_test_levels(s), ==, 0);
    qtest_writew(s, xbar, 138); /* GPIO1's second, independent IRQ output. */
    qtest_writel(s, gpio + 0x144, 1);
    qtest_writel(s, gpio + 0x38, 1);
    qtest_set_irq_in(s, "/machine/gpio1", NULL, 0, 1);
    g_assert_cmphex(crossbar_test_levels(s), ==, 0x10);
    qtest_writew(s, xbar + 2, 138); /* Re-route an already asserted source. */
    g_assert_cmphex(crossbar_test_levels(s), ==, 0x90);
    qtest_writel(s, gpio + 0x40, 1);
    g_assert_cmphex(crossbar_test_levels(s), ==, 0);
    qtest_writew(s, xbar, 21);
    g_assert_cmphex(crossbar_test_levels(s), ==, 0x10);
    g_assert_cmphex(qtest_readl(s, mailbox + 0x40), ==, 0x12345678);
    qtest_writel(s, mailbox + 0x104, 1);
    g_assert_cmphex(crossbar_test_levels(s), ==, 0);
    qtest_writew(s, xbar, 69);
    qtest_writeb(s, 0x48020020, 0);
    qtest_writeb(s, 0x48020004, 2); /* UART3 transmitter-empty interrupt. */
    g_assert_cmphex(crossbar_test_levels(s), ==, 0x10);
    qtest_writeb(s, 0x48020004, 0);
    g_assert_cmphex(crossbar_test_levels(s), ==, 0);
    qtest_writew(s, xbar, 135);
    qtest_writew(s, xbar + 2, 134);
    qtest_writel(s, mailbox + 0x118, 2); /* User 1: FIFO not full. */
    g_assert_cmphex(crossbar_test_levels(s), ==, 0x10);
    qtest_writel(s, mailbox + 0x128, 1); /* User 2: new message. */
    qtest_writel(s, mailbox + 0x40, 0xabcdef01);
    g_assert_cmphex(crossbar_test_levels(s), ==, 0x90);
    qtest_writel(s, mailbox + 0x11c, 2);
    qtest_writel(s, mailbox + 0x12c, 1);
    g_assert_cmphex(crossbar_test_levels(s), ==, 0);
    for (unsigned i = 0; i < 2; i++) {
        uint32_t dsp = 0x4a002948 + i * 0x80;

        g_assert_cmphex(qtest_readl(s, dsp), ==, 0x00020001);
        g_assert_cmphex(qtest_readl(s, dsp + 0x7c), ==, 0x0040003f);
        qtest_writel(s, dsp + 0x54, 0);
        g_assert_cmphex(qtest_readl(s, dsp + 0x54), ==, 0);
    }
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, xbar), ==, 0x00020001);
    g_assert_cmphex(crossbar_test_levels(s), ==, 0);
    for (unsigned i = 0; i < 2; i++) {
        g_assert_cmphex(qtest_readl(s, 0x4a00299c + i * 0x80), ==, 0x002c002b);
    }
    qtest_quit(s);
}

static void test_peripheral_clock_controls(void)
{
    static const uint32_t edma_clocks[] = {
        0x4a008730, 0x4a008770, 0x4a008778, 0x4a008780,
    };
    static const uint32_t clocks[] = {
        0x4a0097f0, 0x4a0097f8, 0x4a0097b0,
        0x4a0097b8, 0x4a005578, 0x4a005580,
        0x4a0097a0, 0x4a0097a8,
        0x4a009800, 0x4a009808,
        0x4a009840, 0x4a009848, 0x4a009850,
        0x4a009870,
        0x4a0098d0,
        0x4a009858, 0x4a0098e0, 0x4a0098e8,
        0x4ae07880,
    };
    QTestState *s = start_rcc();

    for (unsigned i = 0; i < ARRAY_SIZE(edma_clocks); i++) {
        g_assert_cmphex(qtest_readl(s, edma_clocks[i]), ==, 1);
        qtest_writel(s, edma_clocks[i], 0);
        qtest_writel(s, edma_clocks[i], 2);
        g_assert_cmphex(qtest_readl(s, edma_clocks[i]), ==, 1);
    }
    qtest_writel(s, 0x4a008778, 1);
    qtest_writel(s, 0x4a008780, 1);
    for (unsigned i = 0; i < ARRAY_SIZE(clocks); i++) {
        g_assert_cmphex(qtest_readl(s, clocks[i]), ==,
                       (clocks[i] == 0x4a005580 || clocks[i] == 0x4a009870 ||
                        clocks[i] == 0x4a0098d0 ||
                        clocks[i] == 0x4a0098e0 ||
                        clocks[i] == 0x4a0098e8 ||
                        clocks[i] == 0x4ae07880 ||
                        (clocks[i] >= 0x4a009840 &&
                         clocks[i] <= 0x4a009858)) ? 2 : 0x30000);
        qtest_writel(s, clocks[i], 0x30002);
        g_assert_cmphex(qtest_readl(s, clocks[i]), ==, 2);
        qtest_writel(s, clocks[i], 1);
        qtest_writel(s, clocks[i], 3);
        qtest_writel(s, clocks[i], 0x01000002);
        qtest_writew(s, clocks[i], 0);
        g_assert_cmphex(qtest_readl(s, clocks[i]), ==, 2);
        qtest_writel(s, clocks[i], 0);
        g_assert_cmphex(qtest_readl(s, clocks[i]), ==, 0x30000);
        qtest_writel(s, clocks[i], 2);
    }
    qtest_system_reset(s);
    for (unsigned i = 0; i < ARRAY_SIZE(edma_clocks); i++) {
        g_assert_cmphex(qtest_readl(s, edma_clocks[i]), ==, 1);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(clocks); i++) {
        g_assert_cmphex(qtest_readl(s, clocks[i]), ==,
                       (clocks[i] == 0x4a005580 || clocks[i] == 0x4a009870 ||
                        clocks[i] == 0x4a0098d0 ||
                        clocks[i] == 0x4a0098e0 ||
                        clocks[i] == 0x4a0098e8 ||
                        clocks[i] == 0x4ae07880 ||
                        (clocks[i] >= 0x4a009840 &&
                         clocks[i] <= 0x4a009858)) ? 2 : 0x30000);
    }
    qtest_quit(s);
}

static void test_uart_clock_irq(gconstpointer data)
{
    static const uint32_t uarts[] = {
        0x4806a000, 0x4806c000, 0x48020000, 0x48068000,
        0x48066000, 0x48420000, 0x4ae2b000,
        0x4806e000, 0x48422000, 0x48424000,
    };
    static const uint32_t clocks[] = {
        0x4a009840, 0x4a009848, 0x4a009850, 0x4a005580,
        0x4a009870, 0x4a0098d0, 0x4ae07880,
        0x4a009858, 0x4a0098e0, 0x4a0098e8,
    };
    static const unsigned sources[] = {
        67, 68, 69, 101, 100, 218, 221, 65, 219, 220,
    };
    const unsigned index = GPOINTER_TO_UINT(data);
    QTestState *s = start_rcc();
    const uint32_t uart = uarts[index], clock = clocks[index];

    g_assert_cmphex(qtest_readl(s, clock), ==, 2);
    qtest_writel(s, clock, 0);
    qtest_irq_intercept_in(s, "/machine/interrupt-controller");
    qtest_writew(s, 0x4a002a48, sources[index]);
    qtest_writeb(s, uart + 0x20, 0);
    qtest_writeb(s, uart + 0x10, 0x10);
    qtest_writeb(s, uart, 'A'); /* No functional clock: no data transfer. */
    qtest_clock_step(s, 10000000);
    g_assert_cmphex(qtest_readb(s, uart + 0x14) & 1, ==, 0);
    qtest_writel(s, clock, 2);
    qtest_writeb(s, uart, 'B');
    qtest_clock_step(s, 10000000);
    g_assert_cmphex(qtest_readb(s, uart + 0x14) & 1, ==, 1);
    g_assert_cmphex(qtest_readb(s, uart), ==, 'B');
    qtest_writeb(s, uart + 4, 2);
    g_assert_true(qtest_get_irq(s, 4));
    for (unsigned i = 0; i < 3; i++) {
        g_assert_cmphex(qtest_readb(s, uart + 8) & 0x3f, ==, 2);
        g_assert_true(qtest_get_irq(s, 4));
    }
    qtest_writeb(s, uart + 4, 3);
    qtest_writeb(s, uart, 'C');
    qtest_clock_step(s, 10000000);
    g_assert_cmphex(qtest_readb(s, uart + 8) & 0x3f, ==, 4);
    g_assert_cmphex(qtest_readb(s, uart), ==, 'C');
    g_assert_cmphex(qtest_readb(s, uart + 8) & 0x3f, ==, 2);
    g_assert_true(qtest_get_irq(s, 4));
    qtest_writeb(s, uart + 4, 2);
    qtest_writel(s, clock, 0);
    g_assert_false(qtest_get_irq(s, 4));
    qtest_writel(s, clock, 2);
    g_assert_true(qtest_get_irq(s, 4));
    qtest_writeb(s, uart + 4, 0);
    g_assert_false(qtest_get_irq(s, 4));
    qtest_writel(s, uart + 0x54, 0x19);
    g_assert_cmphex(qtest_readl(s, uart + 0x54), ==, 0x19);
    qtest_writel(s, uart + 0x54, 0x1d); /* Wakeup signaling unsupported. */
    g_assert_cmphex(qtest_readl(s, uart + 0x54), ==, 0x19);
    qtest_writel(s, uart + 0x54, 2);
    g_assert_cmphex(qtest_readl(s, uart + 0x54), ==, 0);
    g_assert_cmphex(qtest_readl(s, uart + 0x58), ==, 1);
    g_assert_cmphex(qtest_readl(s, uart + 0x20), ==, 7);
    qtest_writel(s, uart + 0x20, 0);
    qtest_writel(s, uart + 0x20, 8);
    g_assert_cmphex(qtest_readl(s, uart + 0x20), ==, 0);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readb(s, uart + 0x20), ==, 7);
    g_assert_cmphex(qtest_readl(s, clock), ==, 2);
    g_assert_false(qtest_get_irq(s, 4));
    qtest_quit(s);
}

static void test_gpio_clock_gates(void)
{
    static const uint32_t bases[] = {
        0x4ae10000, 0x48055000, 0x48057000, 0x48059000,
        0x4805b000, 0x4805d000, 0x48051000, 0x48053000,
    };
    static const uint32_t clocks[] = {
        0x4ae07838, 0x4a009760, 0x4a009768, 0x4a009770,
        0x4a009778, 0x4a009780, 0x4a009810, 0x4a009818,
    };
    QTestState *s = start_rcc();

    for (unsigned i = 0; i < ARRAY_SIZE(bases); i++) {
        g_assert_cmphex(qtest_readl(s, clocks[i]), ==, 1);
        qtest_writel(s, bases[i] + 0x13c, 0x12340000 + i);
        qtest_writel(s, clocks[i], 0);
        g_assert_cmphex(qtest_readl(s, clocks[i]), ==, 0x30000);
        qtest_writel(s, bases[i] + 0x13c, 0xdeadbeef);
        qtest_writel(s, clocks[i], 0x101);
        g_assert_cmphex(qtest_readl(s, clocks[i]), ==, 0x101);
        g_assert_cmphex(qtest_readl(s, bases[i] + 0x13c), ==, 0x12340000 + i);
        qtest_writel(s, clocks[i], 2);
        qtest_writeb(s, clocks[i], 0);
        g_assert_cmphex(qtest_readl(s, clocks[i]), ==, 0x101);
        qtest_writel(s, clocks[i], 0);
    }
    qtest_system_reset(s);
    for (unsigned i = 0; i < ARRAY_SIZE(bases); i++) {
        g_assert_cmphex(qtest_readl(s, clocks[i]), ==, 1);
        g_assert_cmphex(qtest_readl(s, bases[i] + 0x13c), ==, 0);
    }
    qtest_quit(s);
}

static void test_hardware_spinlocks(void)
{
    QTestState *s = start_rcc();
    const uint32_t base = 0x4a0f6000;

    g_assert_cmphex(qtest_readl(s, base + 0x10), ==, 0x11);
    g_assert_cmphex(qtest_readl(s, base + 0x14), ==, 0x08000001);
    for (unsigned i = 0; i < 256; i++) {
        g_assert_cmphex(qtest_readl(s, base + 0x800 + i * 4), ==, 0);
        g_assert_cmphex(qtest_readl(s, base + 0x800 + i * 4), ==, 1);
        qtest_writel(s, base + 0x800 + i * 4, UINT32_MAX);
        g_assert_cmphex(qtest_readl(s, base + 0x800 + i * 4), ==, 1);
    }
    g_assert_cmphex(qtest_readl(s, base + 0x14), ==, 0x0800ff01);
    qtest_writeb(s, base + 0x800, 0);
    g_assert_cmphex(qtest_readl(s, base + 0x800), ==, 1);
    for (unsigned i = 0; i < 256; i++) {
        qtest_writel(s, base + 0x800 + i * 4, 0xfffffffe);
    }
    g_assert_cmphex(qtest_readl(s, base + 0x14), ==, 0x08000001);
    g_assert_cmphex(qtest_readl(s, base + 0x814), ==, 0);
    qtest_writel(s, base + 0x10, 2);
    g_assert_cmphex(qtest_readl(s, base + 0x814), ==, 0);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, base + 0x14), ==, 0x08000001);
    g_assert_cmphex(qtest_readl(s, base + 0x814), ==, 0);
    qtest_quit(s);
}

static void test_ipu_crossbar_mmr_lock(void)
{
    QTestState *s = start_rcc();
    const uint32_t lock = 0x4a002544;
    const uint32_t xbar[] = { 0x4a0027e0, 0x4a002a48, 0x4a002948 };
    uint32_t saved[3];

    g_assert_cmphex(qtest_readl(s, 0x4a002808), ==, 0x00390038);
    g_assert_cmphex(qtest_readl(s, 0x4a00281c), ==, 0x00230022);
    g_assert_cmphex(qtest_readl(s, 0x4a002850), ==, 63);
    for (unsigned i = 0; i < ARRAY_SIZE(xbar); i++) {
        saved[i] = qtest_readl(s, xbar[i]);
    }
    qtest_writel(s, lock, 0xfdf45530);
    for (unsigned i = 0; i < ARRAY_SIZE(xbar); i++) {
        qtest_writel(s, xbar[i], 0x002a0022);
        g_assert_cmphex(qtest_readl(s, xbar[i]), ==, saved[i]);
    }
    qtest_writel(s, lock, 0);
    g_assert_cmphex(qtest_readl(s, lock), ==, 0xfdf45530);
    qtest_writel(s, lock, 0xf757fdc0);
    for (unsigned i = 0; i < ARRAY_SIZE(xbar); i++) {
        qtest_writel(s, xbar[i], 0x002a0022);
        g_assert_cmphex(qtest_readl(s, xbar[i]), ==, 0x002a0022);
    }
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, lock), ==, 0xf757fdc0);
    for (unsigned i = 0; i < ARRAY_SIZE(xbar); i++) {
        g_assert_cmphex(qtest_readl(s, xbar[i]), ==, saved[i]);
    }
    qtest_quit(s);
}

static void test_sync32k_counter(void)
{
    QTestState *s = start_rcc();
    uint32_t start = qtest_readl(s, 0x4ae04030);

    qtest_clock_step(s, 30517);
    g_assert_cmphex(qtest_readl(s, 0x4ae04030), ==, start);
    qtest_clock_step(s, 1);
    g_assert_cmphex(qtest_readl(s, 0x4ae04030), ==, start + 1);
    qtest_clock_step(s, 1000000000 - 30518);
    g_assert_cmphex(qtest_readl(s, 0x4ae04030), ==, start + 32768);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, 0x4ae04030), ==, start + 32768);
    qtest_writel(s, 0x4ae04030, 0);
    g_assert_cmphex(qtest_readl(s, 0x4ae04030), ==, start + 32768);
    qtest_quit(s);
}

static void test_i2c_clocks_reset_irq_nack(void)
{
    static const uint32_t bases[] = {
        0x48070000, 0x48072000, 0x48060000, 0x4807a000, 0x4807c000,
    };
    static const uint32_t clocks[] = {
        0x4a0097a0, 0x4a0097a8, 0x4a0097b0, 0x4a0097b8, 0x4a005578,
    };
    static const unsigned irqs[] = { 56, 57, 61, 62, 60 };
    QTestState *s = start_rcc();

    qtest_irq_intercept_in(s, "/machine/interrupt-controller");
    for (unsigned i = 0; i < ARRAY_SIZE(bases); i++) {
        uint32_t base = bases[i];

        qtest_writel(s, clocks[i], 2);
        g_assert_cmphex(qtest_readl(s, base + 0x10), ==, 1);
        g_assert_cmphex(qtest_readl(s, base + 0x90), ==, 1);
        g_assert_cmphex(qtest_readl(s, base + 0xc0), ==, 0x4000);
        g_assert_cmphex(qtest_readl(s, base + 0xbc), ==, 0x1e0);
        qtest_writel(s, base + 0xbc, 0x81e0);
        g_assert_cmphex(qtest_readl(s, base + 0xbc), ==, 0x1e0);
        qtest_writel(s, base + 0xbc, 0x41e0);
        g_assert_cmphex(qtest_readl(s, base + 0xbc), ==, 0x41e0);
        qtest_writel(s, base + 0x2c, 2);
        qtest_writel(s, base + 0xac, 0x49);
        qtest_writel(s, base + 0x98, 1);
        qtest_writel(s, base + 0xa4, 0x8603);
        g_assert_cmphex(qtest_readl(s, base + 0x24), ==, 0x102);
        g_assert_cmphex(qtest_readl(s, base + 0x28), ==, 2);
        g_assert_cmphex(qtest_readl(s, base + 0xa4), ==, 0x8200);
        g_assert_true(qtest_get_irq(s, irqs[i]));
        qtest_writel(s, clocks[i], 0);
        g_assert_false(qtest_get_irq(s, irqs[i]));
        qtest_writel(s, base + 0x10, 2);
        qtest_writel(s, clocks[i], 2);
        g_assert_true(qtest_get_irq(s, irqs[i]));
        qtest_writel(s, base + 0x30, 2);
        g_assert_false(qtest_get_irq(s, irqs[i]));
        qtest_writel(s, base + 0x2c, 0xffff9fe0);
        g_assert_cmphex(qtest_readl(s, base + 0x2c), ==, 0xfe0);
        qtest_writel(s, base + 0x30, 0xffff9fe0);
        g_assert_cmphex(qtest_readl(s, base + 0x2c), ==, 0);
        qtest_writel(s, base + 0x28, 0x102);
        g_assert_cmphex(qtest_readl(s, base + 0x24), ==, 0);
        qtest_writel(s, base + 0x2c, 4);
        qtest_writel(s, base + 0x24, 4);
        g_assert_true(qtest_get_irq(s, irqs[i]));
        qtest_writel(s, base + 0x10, 2);
        g_assert_false(qtest_get_irq(s, irqs[i]));
        g_assert_cmphex(qtest_readl(s, base + 0x2c), ==, 0);
        g_assert_cmphex(qtest_readl(s, base + 0xac), ==, 0);
        qtest_writew(s, base + 0x10, 0x308);
        qtest_writel(s, base + 0x10, 0x30c);
        qtest_writeb(s, base + 0x10, 0);
        g_assert_cmphex(qtest_readl(s, base + 0x10), ==, 0x30c);
        qtest_writel(s, base + 0xb0, 7);
        qtest_writel(s, base + 0xb0, 256);
        g_assert_cmphex(qtest_readw(s, base + 0xb0), ==, 7);
    }
    qtest_system_reset(s);
    for (unsigned i = 0; i < ARRAY_SIZE(bases); i++) {
        g_assert_cmphex(qtest_readl(s, clocks[i]), ==, 0x30000);
        qtest_writel(s, clocks[i], 2);
        g_assert_cmphex(qtest_readl(s, bases[i] + 0x10), ==, 1);
        g_assert_cmphex(qtest_readl(s, bases[i] + 0xb0), ==, 0);
    }
    qtest_quit(s);
}

static void test_peripheral_context_loss(void)
{
    static const struct {
        uint32_t context, mask, clock, sysconfig, reset, soft_reset;
    } regs[] = {
        { 0x4ae074f4, 1, 0x4a0097f0, 0x48098110, 0x15, 0 },
        { 0x4ae074fc, 1, 0x4a0097f8, 0x4809a110, 0x15, 0 },
        { 0x4ae07504, 1, 0x4a009800, 0x480b8110, 0x15, 0 },
        { 0x4ae0750c, 1, 0x4a009808, 0x480ba110, 0x15, 0 },
        { 0x4ae074a4, 2, 0x4a0097a0, 0x48070010, 1, 1 },
        { 0x4ae074ac, 1, 0x4a0097a8, 0x48072010, 1, 1 },
        { 0x4ae074b4, 1, 0x4a0097b0, 0x48060010, 1, 1 },
        { 0x4ae074bc, 1, 0x4a0097b8, 0x4807a010, 1, 1 },
        { 0x4ae0657c, 1, 0x4a005578, 0x4807c010, 1, 1 },
    };
    QTestState *s = start_rcc();

    for (unsigned boot = 0; boot < 2; boot++) {
        if (boot) {
            qtest_system_reset(s);
        }
        for (unsigned i = 0; i < ARRAY_SIZE(regs); i++) {
            uint32_t context = regs[i].context, mask = regs[i].mask;

            g_assert_cmphex(qtest_readl(s, context), ==, mask);
            qtest_writel(s, context, 0);
            qtest_writew(s, context, mask);
            qtest_writeb(s, context, mask);
            qtest_writel(s, context, ~mask);
            g_assert_cmphex(qtest_readl(s, context), ==, mask);
            qtest_writel(s, regs[i].clock, 2);
            g_assert_cmphex(qtest_readl(s, regs[i].sysconfig), ==,
                            regs[i].reset);
            qtest_writel(s, regs[i].sysconfig, 0x308);
            qtest_writel(s, context, mask);
            g_assert_cmphex(qtest_readl(s, context), ==, 0);
            g_assert_cmphex(qtest_readl(s, regs[i].sysconfig), ==, 0x308);
            qtest_writel(s, regs[i].clock, 0);
            g_assert_cmphex(qtest_readl(s, context), ==, 0);
            qtest_writel(s, regs[i].clock, 2);
            g_assert_cmphex(qtest_readl(s, regs[i].sysconfig), ==, 0x308);
            qtest_writel(s, regs[i].sysconfig, 2);
            g_assert_cmphex(qtest_readl(s, regs[i].sysconfig), ==,
                            regs[i].soft_reset);
            g_assert_cmphex(qtest_readl(s, context), ==, 0);
            qtest_writel(s, context, UINT32_MAX);
            g_assert_cmphex(qtest_readl(s, context), ==, 0);
        }
    }
    qtest_quit(s);
}

static void test_i2c_dormant_wakeup_configuration(void)
{
    static const uint32_t bases[] = {
        0x48070000, 0x48072000, 0x48060000, 0x4807a000, 0x4807c000,
    };
    static const uint32_t clocks[] = {
        0x4a0097a0, 0x4a0097a8, 0x4a0097b0, 0x4a0097b8, 0x4a005578,
    };
    static const unsigned irqs[] = { 56, 57, 61, 62, 60 };
    QTestState *s = start_rcc();

    qtest_irq_intercept_in(s, "/machine/interrupt-controller");
    for (unsigned boot = 0; boot < 2; boot++) {
        if (boot) {
            qtest_system_reset(s);
        }
        for (unsigned i = 0; i < ARRAY_SIZE(bases); i++) {
            uint32_t base = bases[i];

            qtest_writel(s, clocks[i], 2);
            g_assert_cmphex(qtest_readl(s, base + 0x10), ==, 1);
            for (unsigned activity = 0; activity < 4; activity++) {
                for (unsigned idle = 0; idle < 4; idle++) {
                    uint32_t config = (activity << 8) | (idle << 3) | 5;

                    qtest_writew(s, base + 0x10, config);
                    g_assert_cmphex(qtest_readl(s, base + 0x10), ==, config);
                    g_assert_cmphex(qtest_readl(s, base + 0x34), ==, 0);
                    g_assert_cmphex(qtest_readl(s, base + 0x24), ==, 0);
                    g_assert_false(qtest_get_irq(s, irqs[i]));
                }
            }
            qtest_writel(s, base + 0x10, 0x215); /* Original IPL request. */
            g_assert_cmphex(qtest_readw(s, base + 0x10), ==, 0x215);
            qtest_writel(s, base + 0x10, 0x615); /* Reserved bit rejected. */
            g_assert_cmphex(qtest_readl(s, base + 0x10), ==, 0x215);
            qtest_writel(s, base + 0x34, 0x636f);
            g_assert_cmphex(qtest_readl(s, base + 0x34), ==, 0x636f);
            qtest_writel(s, base + 0x34, 0x6f6f);
            g_assert_cmphex(qtest_readl(s, base + 0x34), ==, 0x6f6f);
            qtest_writel(s, base + 0x34, 0xffff);
            g_assert_cmphex(qtest_readl(s, base + 0x34), ==, 0x6f6f);
            g_assert_cmphex(qtest_readl(s, base + 0x24), ==, 0);
            g_assert_false(qtest_get_irq(s, irqs[i]));
            qtest_writel(s, clocks[i], 0);
            g_assert_false(qtest_get_irq(s, irqs[i]));
            qtest_writel(s, clocks[i], 2);
            g_assert_cmphex(qtest_readl(s, base + 0x10), ==, 0x215);
            g_assert_cmphex(qtest_readl(s, base + 0x34), ==, 0x6f6f);
            g_assert_cmphex(qtest_readl(s, base + 0x24), ==, 0);
            g_assert_false(qtest_get_irq(s, irqs[i]));
            qtest_writel(s, base + 0x10, 0x217);
            g_assert_cmphex(qtest_readl(s, base + 0x10), ==, 1);
            g_assert_cmphex(qtest_readl(s, base + 0x34), ==, 0);
            g_assert_false(qtest_get_irq(s, irqs[i]));
        }
    }
    qtest_quit(s);
}

static void test_i2c_real_slave_fifo_transfer(void)
{
    QTestState *s = start_rcc_options(
        "-device tmp105,bus=i2c1,address=0x49,id=i2c-sensor");
    const uint32_t base = 0x48070000, clock = 0x4a0097a0;

    qtest_irq_intercept_in(s, "/machine/interrupt-controller");
    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':'i2c-sensor',"
        "'property':'temperature','value':25000}}");
    qtest_writel(s, clock, 2);
    qtest_writel(s, base + 0x10, 0x215);
    g_assert_cmphex(qtest_readl(s, base + 0x10), ==, 0x215);
    g_assert_cmphex(qtest_readl(s, base + 0x34), ==, 0);
    qtest_writel(s, base + 0xac, 0x49);
    qtest_writel(s, base + 0x98, 1);
    qtest_writel(s, base + 0xa4, 0x8601);
    g_assert_cmphex(qtest_readl(s, base + 0x24), ==, 0x1010);
    qtest_writel(s, clock, 0); /* Cannot gate a bus owned by the controller. */
    g_assert_cmphex(qtest_readl(s, clock), ==, 2);
    qtest_writeb(s, base + 0x9c, 0); /* Actual TMP105 temperature pointer. */
    g_assert_cmphex(qtest_readl(s, base + 0x24), ==, 0x1004);
    qtest_writel(s, base + 0x28, 0x7fff);
    qtest_writel(s, base + 0x98, 2);
    qtest_writel(s, base + 0x2c, 0x2008);
    qtest_writel(s, base + 0xa4, 0x8403); /* Repeated START, receive, STOP. */
    g_assert_cmphex(qtest_readl(s, base + 0x98), ==, 0);
    g_assert_cmphex(qtest_readl(s, base + 0xc0), ==, 0x4200);
    g_assert_true(qtest_get_irq(s, 56));
    g_assert_cmphex(qtest_readw(s, base + 0x9c), ==, 0x19);
    g_assert_cmphex(qtest_readl(s, base + 0xc0), ==, 0x4000);
    g_assert_false(qtest_get_irq(s, 56));

    qtest_writel(s, base + 0x98, 2);
    qtest_writel(s, base + 0xa4, 0x8603);
    qtest_writel(s, base + 0xa4, 0x8403); /* Cannot change an active direction. */
    g_assert_cmphex(qtest_readl(s, base + 0xa4), ==, 0x8602);
    qtest_writew(s, base + 0x9c, 0x0101); /* TMP105 shutdown configuration. */
    g_assert_cmphex(qtest_readl(s, base + 0x98), ==, 0);
    qtest_writel(s, base + 0x98, 1);
    qtest_writel(s, base + 0xa4, 0x8601);
    qtest_writeb(s, base + 0x9c, 1);
    qtest_writel(s, base + 0x98, 1);
    qtest_writel(s, base + 0xa4, 0x8403);
    g_assert_cmphex(qtest_readb(s, base + 0x9c), ==, 1);
    qtest_writel(s, base + 0x98, 1);
    qtest_writel(s, base + 0xa4, 0x8603);
    qtest_writeb(s, base + 0x9c, 0);
    qtest_writel(s, base + 0x28, 0x7fff);
    qtest_writel(s, base + 0x98, 20);
    qtest_writel(s, base + 0xa4, 0x8403);
    g_assert_cmphex(qtest_readl(s, base + 0xc0), ==, 0x5000);
    g_assert_cmphex(qtest_readl(s, base + 0x98), ==, 4);
    for (unsigned i = 0; i < 10; i++) {
        qtest_readw(s, base + 0x9c);
    }
    g_assert_cmphex(qtest_readl(s, base + 0x98), ==, 0);
    g_assert_cmphex(qtest_readl(s, base + 0xc0), ==, 0x4000);
    g_assert_cmphex(qtest_readl(s, base + 0x24) & 0x1000, ==, 0);
    qtest_writel(s, base + 0x98, 1);
    qtest_writel(s, base + 0xa4, 0x8601);
    qtest_writel(s, base + 0x10, 2); /* Abort actual bus ownership on reset. */
    g_assert_cmphex(qtest_readl(s, base + 0x24), ==, 0);
    g_assert_cmphex(qtest_readl(s, base + 0xa4), ==, 0);
    qtest_writel(s, base + 0xa4, 0x8000);
    for (unsigned i = 0; i < 16; i++) {
        qtest_writeb(s, base + 0x9c, i);
    }
    g_assert_cmphex(qtest_readl(s, base + 0xc0), ==, 0x4010);
    qtest_writeb(s, base + 0x9c, 0xff); /* Full FIFO rejects extra data. */
    g_assert_cmphex(qtest_readl(s, base + 0xc0), ==, 0x4010);
    qtest_writel(s, base + 0x94, 0x40);
    g_assert_cmphex(qtest_readl(s, base + 0xc0), ==, 0x4000);
    qtest_writel(s, clock, 0);
    g_assert_cmphex(qtest_readl(s, clock), ==, 0x30000);
    qtest_quit(s);
}

static void test_i2c_fifo_irq_reassertion(void)
{
    const uint32_t base = 0x48072000;
    QTestState *s = start_rcc_options(
        "-device at24c-eeprom,bus=i2c2,address=0x50,rom-size=8192");

    qtest_irq_intercept_in(s, "/machine/interrupt-controller");
    for (unsigned boot = 0; boot < 2; boot++) {
        if (boot) {
            qtest_system_reset(s);
        }
        qtest_writel(s, 0x4a0097a8, 2);
        qtest_writel(s, base + 0xac, 0x50);
        for (unsigned threshold = 0; threshold <= 7; threshold += 7) {
            uint32_t ready = threshold ? 0x4000 : 0x10;

            qtest_writel(s, base + 0x94, threshold);
            qtest_writel(s, base + 0x30, 0x7fff);
            qtest_writel(s, base + 0x2c, ready);
            qtest_writel(s, base + 0x98, 2);
            qtest_writel(s, base + 0xa4, 0x8603);
            g_assert_true(qtest_get_irq(s, 57));
            qtest_writel(s, base + 0x28, ready);
            g_assert_cmphex(qtest_readl(s, base + 0x24) & ready, ==, ready);
            g_assert_true(qtest_get_irq(s, 57));
            qtest_writeb(s, base + 0x9c, 0);
            qtest_writel(s, base + 0x28, ready);
            g_assert_true(qtest_get_irq(s, 57));
            qtest_writeb(s, base + 0x9c, 0);
            g_assert_false(qtest_get_irq(s, 57));
        }
        qtest_writel(s, base + 0x30, 0x7fff);
        qtest_writel(s, base + 0x28, 0x7fff);
        qtest_writel(s, base + 0x94, 0x700);
        qtest_writel(s, base + 0x2c, 0x2008);
        qtest_writel(s, base + 0x98, 43);
        qtest_writel(s, base + 0xa4, 0x8403);
        g_assert_cmphex(qtest_readl(s, base + 0x98), ==, 27);
        for (unsigned i = 0; i < 43; i++) {
            g_assert_true(qtest_get_irq(s, 57));
            g_assert_cmphex(qtest_readb(s, base + 0x9c), ==, 0);
            qtest_writel(s, base + 0x28, 0x2008);
            if (i < 42) {
                uint32_t ready = i < 35 ? 8 : 0x2000;

                g_assert_cmphex(qtest_readl(s, base + 0x24) & 0x2008,
                                ==, ready);
                g_assert_true(qtest_get_irq(s, 57));
                qtest_writel(s, base + 0x30, 0x2008);
                g_assert_false(qtest_get_irq(s, 57));
                qtest_writel(s, base + 0x2c, 0x2008);
            }
        }
        g_assert_false(qtest_get_irq(s, 57));
        g_assert_cmphex(qtest_readl(s, base + 0x98), ==, 0);
        g_assert_cmphex(qtest_readl(s, base + 0xc0), ==, 0x4000);
        g_assert_cmphex(qtest_readl(s, base + 0x24) & 0x1000, ==, 0);
    }
    qtest_quit(s);
}

static void test_i2c_eeprom(void)
{
    const uint32_t base = 0x48072000;
    QTestState *s = start_rcc_options(
        "-device at24c-eeprom,bus=i2c2,address=0x50,rom-size=8192");

    qtest_writel(s, 0x4a0097a8, 2);
    qtest_writel(s, base + 0xac, 0x50);
    qtest_writel(s, base + 0xa4, 0x8000);
    qtest_writeb(s, base + 0x9c, 0x0e);
    qtest_writeb(s, base + 0x9c, 0x88);
    for (unsigned i = 0; i < 14; i++) {
        qtest_writeb(s, base + 0x9c, 0xa0 + i);
    }
    qtest_writel(s, base + 0x98, 18);
    qtest_writel(s, base + 0xa4, 0x8603);
    g_assert_cmphex(qtest_readl(s, base + 0x98), ==, 2);
    qtest_writeb(s, base + 0x9c, 0xae);
    qtest_writeb(s, base + 0x9c, 0xaf);
    g_assert_cmphex(qtest_readl(s, base + 0x24) & 6, ==, 4);
    qtest_writel(s, base + 0x28, 0x7fff);
    qtest_writel(s, base + 0x98, 2);
    qtest_writel(s, base + 0xa4, 0x8601);
    qtest_writeb(s, base + 0x9c, 0x0e);
    qtest_writeb(s, base + 0x9c, 0x88);
    qtest_writel(s, base + 0x98, 16);
    qtest_writel(s, base + 0xa4, 0x8403);
    g_assert_cmphex(qtest_readl(s, base + 0x24) & 6, ==, 4);
    for (unsigned i = 0; i < 16; i++) {
        g_assert_cmphex(qtest_readb(s, base + 0x9c), ==, 0xa0 + i);
    }
    qtest_system_reset(s);
    qtest_writel(s, 0x4a0097a8, 2);
    qtest_writel(s, base + 0xac, 0x50);
    qtest_writel(s, base + 0x98, 2);
    qtest_writel(s, base + 0xa4, 0x8601);
    qtest_writeb(s, base + 0x9c, 0x0e);
    qtest_writeb(s, base + 0x9c, 0x88);
    qtest_writel(s, base + 0x98, 16);
    qtest_writel(s, base + 0xa4, 0x8403);
    for (unsigned i = 0; i < 16; i++) {
        g_assert_cmphex(qtest_readb(s, base + 0x9c), ==, 0xa0 + i);
    }
    qtest_quit(s);
}

static void test_mcspi_initialization(void)
{
    static const uint32_t bases[] = {
        0x48098000, 0x4809a000, 0x480b8000, 0x480ba000,
    };
    static const uint32_t clocks[] = {
        0x4a0097f0, 0x4a0097f8, 0x4a009800, 0x4a009808,
    };
    QTestState *s = start_rcc();

    for (unsigned i = 0; i < ARRAY_SIZE(bases); i++) {
        uint32_t base = bases[i];

        qtest_writel(s, clocks[i], 2);
        g_assert_cmphex(qtest_readl(s, base + 0x110), ==, 0x15);
        g_assert_cmphex(qtest_readl(s, base + 0x114), ==, 1);
        g_assert_cmphex(qtest_readl(s, base + 0x128), ==, 4);
        g_assert_cmphex(qtest_readl(s, base + 0x118), ==, 0);
        qtest_writel(s, base + 0x11c, 0x3777f);
        g_assert_cmphex(qtest_readl(s, base + 0x11c), ==, 0x3777f);
        qtest_writel(s, base + 0x128, 0x104);
        g_assert_cmphex(qtest_readl(s, base + 0x128), ==, 0x104);
        qtest_writel(s, base + 0x128, 0x10c);
        g_assert_cmphex(qtest_readl(s, base + 0x128), ==, 0x104);
        qtest_writel(s, base + 0x128, 1);
        for (unsigned channel = 0; channel < 4; channel++) {
            uint32_t config = base + 0x12c + 0x14 * channel;

            g_assert_cmphex(qtest_readl(s, config), ==, 0x60000);
            g_assert_cmphex(qtest_readl(s, config + 4), ==, 0);
            qtest_writel(s, config, 0x103c0);
            qtest_writel(s, config + 8, 0x400);
            qtest_writel(s, config, 0x133c0);
            qtest_writel(s, config + 8, 0x401); /* Reserved transfer mode. */
            g_assert_cmphex(qtest_readl(s, config + 8), ==, 0x400);
            g_assert_cmphex(qtest_readl(s, config + 4), ==, 0);
            qtest_writel(s, config, 0x103c0);
        }
        qtest_writel(s, base + 0x110, 0x308);
        qtest_writel(s, base + 0x110, 0x318); /* Reserved idle mode. */
        qtest_writew(s, base + 0x110, 0);
        g_assert_cmphex(qtest_readl(s, base + 0x110), ==, 0x308);
        qtest_writel(s, clocks[i], 0);
        qtest_writel(s, base + 0x110, 2);
        qtest_writel(s, clocks[i], 2);
        g_assert_cmphex(qtest_readl(s, base + 0x128), ==, 1);
        g_assert_cmphex(qtest_readl(s, base + 0x12c), ==, 0x103c0);
        qtest_writel(s, base + 0x110, 0x30a);
        g_assert_cmphex(qtest_readl(s, base + 0x110), ==, 0x308);
        g_assert_cmphex(qtest_readl(s, base + 0x128), ==, 4);
        g_assert_cmphex(qtest_readl(s, base + 0x11c), ==, 0);
        for (unsigned channel = 0; channel < 4; channel++) {
            uint32_t config = base + 0x12c + 0x14 * channel;

            g_assert_cmphex(qtest_readl(s, config), ==, 0x60000);
            g_assert_cmphex(qtest_readl(s, config + 8), ==, 0);
        }
    }
    qtest_system_reset(s);
    for (unsigned i = 0; i < ARRAY_SIZE(bases); i++) {
        g_assert_cmphex(qtest_readl(s, clocks[i]), ==, 0x30000);
        qtest_writel(s, clocks[i], 2);
        g_assert_cmphex(qtest_readl(s, bases[i] + 0x110), ==, 0x15);
    }
    qtest_quit(s);
}

static void test_mcspi_dormant_test_directions(void)
{
    static const uint32_t bases[] = {
        0x48098000, 0x4809a000, 0x480b8000, 0x480ba000,
    };
    static const uint32_t clocks[] = {
        0x4a0097f0, 0x4a0097f8, 0x4a009800, 0x4a009808,
    };
    static const uint32_t unsupported[] = {
        1, 0x10, 0x20, 0x40, 0x80, 0x800, 0x1000, 0x80000000,
    };
    QTestState *s = start_rcc();

    for (unsigned boot = 0; boot < 2; boot++) {
        for (unsigned i = 0; i < ARRAY_SIZE(bases); i++) {
            uint32_t base = bases[i];

            qtest_writel(s, clocks[i], 2);
            g_assert_cmphex(qtest_readl(s, base + 0x124), ==, 0);
            qtest_writel(s, base + 0x11c, 0x3777f);
            for (unsigned direction = 0; direction < 8; direction++) {
                qtest_writel(s, base + 0x124, direction << 8);
                g_assert_cmphex(qtest_readl(s, base + 0x124), ==,
                                direction << 8);
                g_assert_cmphex(qtest_readl(s, base + 0x118), ==, 0);
                g_assert_cmphex(qtest_readl(s, base + 0x128), ==, 4);
            }
            qtest_writel(s, base + 0x124, 0x200);
            for (unsigned j = 0; j < ARRAY_SIZE(unsupported); j++) {
                qtest_writel(s, base + 0x124, 0x200 | unsupported[j]);
                g_assert_cmphex(qtest_readl(s, base + 0x124), ==, 0x200);
            }
            qtest_writeb(s, base + 0x124, 0);
            qtest_writew(s, base + 0x124, 0);
            g_assert_cmphex(qtest_readl(s, base + 0x124), ==, 0x200);
            qtest_writel(s, base + 0x128, 0xc); /* No system test mode. */
            g_assert_cmphex(qtest_readl(s, base + 0x128), ==, 4);
            g_assert_cmphex(qtest_readl(s, base + 0x118), ==, 0);
            qtest_writel(s, clocks[i], 0);
            qtest_writel(s, base + 0x124, 0);
            qtest_writel(s, clocks[i], 2);
            g_assert_cmphex(qtest_readl(s, base + 0x124), ==, 0x200);
            qtest_writel(s, base + 0x110, 2);
            g_assert_cmphex(qtest_readl(s, base + 0x124), ==, 0);
            qtest_writel(s, base + 0x124, 0x700);
        }
        qtest_system_reset(s);
    }
    qtest_quit(s);
}

static void test_mcspi_transfers(gconstpointer data)
{
    unsigned number = GPOINTER_TO_UINT(data);
    const uint32_t base = number == 3 ? 0x480b8000 : 0x480ba000;
    const uint32_t clock = number == 3 ? 0x4a009800 : 0x4a009808;
    const uint32_t config = 0x603c0;
    g_autofree char *options = g_strdup_printf(
        "-device w25q80,bus=spi%u,cs=0 -device w25q80,bus=spi%u,cs=1",
        number, number);
    QTestState *s = start_rcc_options(options);

    qtest_irq_intercept_in(s, "/machine/interrupt-controller");
    qtest_writel(s, 0x4a002a48, number == 3 ? 86 : 43);
    qtest_writel(s, clock, 2);
    qtest_writel(s, base + 0x124, 0x200);
    g_assert_cmphex(qtest_readl(s, base + 0x124), ==, 0x200);
    qtest_writel(s, base + 0x128, 1);
    qtest_writel(s, base + 0x12c, config);
    qtest_writel(s, base + 0x140, config);
    for (unsigned channel = 0; channel < 2; channel++) {
        uint32_t reg = base + 0x12c + channel * 0x14;

        qtest_writel(s, reg, config);
        qtest_writel(s, reg + 8, 1);
        qtest_writel(s, reg, config | (1 << 20));
        qtest_writeb(s, reg + 0xc, 0x9f);
        qtest_readb(s, reg + 0x10);
        qtest_writeb(s, reg + 0xc, 0);
        g_assert_cmphex(qtest_readb(s, reg + 0x10), ==, 0xef);
        qtest_writeb(s, reg + 0xc, 0);
        g_assert_cmphex(qtest_readb(s, reg + 0x10), ==, 0x50);
        qtest_writeb(s, reg + 0xc, 0);
        g_assert_cmphex(qtest_readb(s, reg + 0x10), ==, 0x14);
        g_assert_cmphex(qtest_readl(s, reg + 4), ==, 6);
        qtest_writel(s, reg, config);
        qtest_writel(s, reg + 8, 0);
    }
    qtest_writel(s, base + 0x17c, 0x40000);
    qtest_writel(s, base + 0x12c, config | (3 << 27));
    qtest_writel(s, base + 0x11c, 0x20004);
    qtest_writel(s, base + 0x118, 0x3777f);
    qtest_writel(s, base + 0x134, 1);
    qtest_writel(s, base + 0x12c, config | (3 << 27) | (1 << 20));
    g_assert_false(qtest_get_irq(s, 4));
    qtest_writeb(s, base + 0x138, 0x9f);
    g_assert_true(qtest_get_irq(s, 4));
    for (unsigned i = 0; i < 3; i++) {
        qtest_writeb(s, base + 0x138, 0);
    }
    g_assert_cmphex(qtest_readl(s, base + 0x17c), ==, 0x40000);
    g_assert_cmphex(qtest_readl(s, base + 0x118) & 0x20004, ==, 0x20004);
    qtest_writel(s, clock, 0);
    g_assert_false(qtest_get_irq(s, 4));
    qtest_writel(s, clock, 2);
    g_assert_true(qtest_get_irq(s, 4));
    qtest_readb(s, base + 0x13c);
    g_assert_cmphex(qtest_readb(s, base + 0x13c), ==, 0xef);
    g_assert_cmphex(qtest_readb(s, base + 0x13c), ==, 0x50);
    g_assert_cmphex(qtest_readb(s, base + 0x13c), ==, 0x14);
    qtest_writel(s, base + 0x118, 0x3777f);
    g_assert_false(qtest_get_irq(s, 4));
    qtest_writel(s, base + 0x110, 2);
    g_assert_cmphex(qtest_readl(s, base + 0x118), ==, 0);
    g_assert_cmphex(qtest_readl(s, base + 0x12c), ==, 0x60000);

    /* Both FIFO directions share 128 bytes; reject the 65th RX byte. */
    qtest_writel(s, base + 0x128, 0x101);
    qtest_writel(s, base + 0x12c, config | (3 << 27));
    qtest_writel(s, base + 0x17c, 63 << 8);
    qtest_writel(s, base + 0x134, 1);
    qtest_writel(s, base + 0x12c, config | (3 << 27) | (1 << 20));
    qtest_writeb(s, base + 0x180, 0x9f);
    for (unsigned i = 1; i < 64; i++) {
        qtest_writeb(s, base + 0x180, 0);
    }
    g_assert_cmphex(qtest_readl(s, base + 0x130), ==, 0x4f);
    qtest_writeb(s, base + 0x180, 0);
    g_assert_cmphex(qtest_readl(s, base + 0x17c) >> 16, ==, 64);
    g_assert_cmphex(qtest_readl(s, base + 0x118) & 4, ==, 4);
    qtest_readb(s, base + 0x1a0);
    g_assert_cmphex(qtest_readb(s, base + 0x1a0), ==, 0xef);
    g_assert_cmphex(qtest_readb(s, base + 0x1a0), ==, 0x50);
    g_assert_cmphex(qtest_readb(s, base + 0x1a0), ==, 0x14);
    for (unsigned i = 4; i < 64; i++) {
        qtest_readb(s, base + 0x1a0);
    }
    g_assert_cmphex(qtest_readl(s, base + 0x130), ==, 0x2e);
    qtest_writel(s, base + 0x110, 2);

    /* Actual SSI words are masked and stored with their native FIFO width. */
    for (unsigned bits = 4; bits <= 32; bits *= 2) {
        uint32_t word_config = 0x60040 | ((bits - 1) << 7) | (1 << 20);

        qtest_writel(s, base + 0x128, 1);
        qtest_writel(s, base + 0x12c, word_config);
        qtest_writel(s, base + 0x134, 1);
        qtest_writel(s, base + 0x138, 0x9f);
        qtest_readl(s, base + 0x13c);
        qtest_writel(s, base + 0x138, 0);
        g_assert_cmphex(qtest_readl(s, base + 0x13c), ==,
                       bits == 4 ? 0 : 0xef);
        qtest_writel(s, base + 0x134, 0);
        qtest_writel(s, base + 0x110, 2);
    }

    /* An enabled but unattached channel must not manufacture a transfer. */
    qtest_writel(s, base + 0x128, 1);
    qtest_writel(s, base + 0x154, config);
    qtest_writel(s, base + 0x15c, 1);
    qtest_writeb(s, base + 0x160, 0x9f);
    g_assert_cmphex(qtest_readl(s, base + 0x158), ==, 2);
    g_assert_cmphex(qtest_readl(s, base + 0x160), ==, 0);
    qtest_system_reset(s);
    qtest_writel(s, clock, 2);
    g_assert_cmphex(qtest_readl(s, base + 0x11c), ==, 0);
    g_assert_cmphex(qtest_readl(s, base + 0x154), ==, 0x60000);
    qtest_quit(s);
}

static uint8_t tja1145_exchange(QTestState *s, uint8_t command, uint8_t value)
{
    qtest_writel(s, 0x480ba12c, 0x1603c0);
    qtest_writeb(s, 0x480ba138, command);
    qtest_readb(s, 0x480ba13c);
    qtest_writeb(s, 0x480ba138, value);
    value = qtest_readb(s, 0x480ba13c);
    qtest_writel(s, 0x480ba12c, 0x603c0);
    return value;
}

static uint8_t lis331dlh_byte(QTestState *s, uint8_t value)
{
    qtest_writeb(s, 0x480b814c, value);
    return qtest_readb(s, 0x480b8150);
}

static void lis331dlh_begin(QTestState *s, uint8_t command)
{
    qtest_writel(s, 0x480b8140, 0x1603c0);
    lis331dlh_byte(s, command);
}

static void lis331dlh_end(QTestState *s)
{
    qtest_writel(s, 0x480b8140, 0x603c0);
}

static uint8_t lis331dlh_exchange(QTestState *s, uint8_t command, uint8_t value)
{
    lis331dlh_begin(s, command);
    value = lis331dlh_byte(s, value);
    lis331dlh_end(s);
    return value;
}

static void test_lis331dlh_samples(void)
{
    QTestState *s = start_rcc_options(
        "-device lis331dlh,bus=spi3,cs=1,x-mg=-1000,y-mg=500,z-mg=1000");
    const uint8_t little_endian[] = { 0x80, 0xc1, 0x40, 0x1f, 0x80, 0x3e };
    const uint8_t big_endian[] = { 0xc1, 0x80, 0x1f, 0x40, 0x3e, 0x80 };

    for (unsigned boot = 0; boot < 2; boot++) {
        if (boot) {
            qtest_system_reset(s);
        }
        qtest_writel(s, 0x4a009800, 2);
        qtest_writel(s, 0x480b8128, 1);
        qtest_writel(s, 0x480b8140, 0x603c0);
        qtest_writel(s, 0x480b8148, 1);
        g_assert_cmphex(lis331dlh_exchange(s, 0x8f, 0), ==, 0x32);
        g_assert_cmphex(lis331dlh_exchange(s, 0xa0, 0), ==, 7);
        lis331dlh_exchange(s, 0x0f, 0); /* Identity is not writable. */
        g_assert_cmphex(lis331dlh_exchange(s, 0x8f, 0), ==, 0x32);
        qtest_clock_step(s, 100000000);
        g_assert_cmphex(lis331dlh_exchange(s, 0xa7, 0), ==, 0);

        /* First sample must wait for the real 50 Hz period. */
        lis331dlh_exchange(s, 0x20, 0x27);
        qtest_clock_step(s, 19999999);
        g_assert_cmphex(lis331dlh_exchange(s, 0xa7, 0), ==, 0);
        qtest_clock_step(s, 1);
        g_assert_cmphex(lis331dlh_exchange(s, 0xa7, 0), ==, 0x0f);
        lis331dlh_begin(s, 0xe8);
        for (unsigned i = 0; i < ARRAY_SIZE(little_endian); i++) {
            g_assert_cmphex(lis331dlh_byte(s, 0), ==, little_endian[i]);
        }
        lis331dlh_end(s);
        g_assert_cmphex(lis331dlh_exchange(s, 0xa7, 0), ==, 0);
        qtest_clock_step(s, 40000000);
        g_assert_cmphex(lis331dlh_exchange(s, 0xa7, 0), ==, 0xff);
        lis331dlh_begin(s, 0xe8);
        for (unsigned i = 0; i < 6; i++) {
            lis331dlh_byte(s, 0);
        }
        lis331dlh_end(s);
        g_assert_cmphex(lis331dlh_exchange(s, 0xa7, 0), ==, 0);

        /* Original firmware's endian/BDU setting and six-register write. */
        lis331dlh_begin(s, 0x60);
        for (unsigned i = 0; i < 5; i++) {
            lis331dlh_byte(s, i == 0 ? 0x2f : i == 3 ? 0xc0 : 0);
        }
        lis331dlh_end(s);
        g_assert_cmphex(lis331dlh_exchange(s, 0xa3, 0), ==, 0xc0);
        qtest_clock_step(s, 10000000);
        g_assert_cmphex(lis331dlh_exchange(s, 0xa7, 0), ==, 0xf);
        g_assert_cmphex(lis331dlh_exchange(s, 0xa8, 0), ==, 0xc1);
        qtest_clock_step(s, 100000000);
        g_assert_cmphex(lis331dlh_exchange(s, 0xa7, 0), ==, 0xf);
        g_assert_cmphex(lis331dlh_exchange(s, 0xa9, 0), ==, 0x80);
        g_assert_cmphex(lis331dlh_exchange(s, 0xa7, 0), ==, 6);
        lis331dlh_begin(s, 0xea);
        for (unsigned i = 2; i < ARRAY_SIZE(big_endian); i++) {
            g_assert_cmphex(lis331dlh_byte(s, 0), ==, big_endian[i]);
        }
        lis331dlh_end(s);
        g_assert_cmphex(lis331dlh_exchange(s, 0xa7, 0), ==, 0);

        /* No increment repeats WHO_AM_I; CS resets the command parser. */
        lis331dlh_begin(s, 0x8f);
        for (unsigned i = 0; i < 3; i++) {
            g_assert_cmphex(lis331dlh_byte(s, 0), ==, 0x32);
        }
        lis331dlh_end(s);
        lis331dlh_begin(s, 0x20);
        lis331dlh_end(s); /* Truncated command must not alter CTRL_REG1. */
        g_assert_cmphex(lis331dlh_exchange(s, 0xa0, 0), ==, 0x2f);
        lis331dlh_exchange(s, 0x23, 0x20); /* Reserved scale is rejected. */
        lis331dlh_exchange(s, 0x23, 0xc2); /* Self-test is unsupported. */
        g_assert_cmphex(lis331dlh_exchange(s, 0xa3, 0), ==, 0xc0);
        lis331dlh_exchange(s, 0x21, 0x80); /* BOOT self-clears. */
        g_assert_cmphex(lis331dlh_exchange(s, 0xa1, 0), ==, 0);

        /* Low-power 0.5 Hz has a two-second conversion period. */
        lis331dlh_exchange(s, 0x20, 0x47);
        qtest_clock_step(s, 1999999999);
        g_assert_cmphex(lis331dlh_exchange(s, 0xa7, 0), ==, 0);
        qtest_clock_step(s, 1);
        g_assert_cmphex(lis331dlh_exchange(s, 0xa7, 0), ==, 0xf);
        lis331dlh_begin(s, 0xe8);
        for (unsigned i = 0; i < ARRAY_SIZE(big_endian); i++) {
            g_assert_cmphex(lis331dlh_byte(s, 0), ==, big_endian[i]);
        }
        lis331dlh_end(s);
        lis331dlh_exchange(s, 0x20, 7);
        qtest_clock_step(s, 4000000000LL);
        g_assert_cmphex(lis331dlh_exchange(s, 0xa7, 0), ==, 0);

        /* Full-scale sensitivity and disabled-axis conversion gating. */
        for (unsigned scale = 0; scale < 2; scale++) {
            const uint8_t samples[][6] = {
                { 0xc0, 0xe0, 0xa0, 0x0f, 0x40, 0x1f },
                { 0x00, 0xf0, 0x00, 0x08, 0x00, 0x10 },
            };

            lis331dlh_exchange(s, 0x23, scale ? 0x30 : 0x10);
            lis331dlh_exchange(s, 0x20, 0x26);
            qtest_clock_step(s, 20000000);
            g_assert_cmphex(lis331dlh_exchange(s, 0xa7, 0), ==, 0x0e);
            lis331dlh_begin(s, 0xea);
            for (unsigned i = 2; i < 6; i++) {
                g_assert_cmphex(lis331dlh_byte(s, 0), ==, samples[scale][i]);
            }
            lis331dlh_end(s);
            g_assert_cmphex(lis331dlh_exchange(s, 0xa7, 0), ==, 0);
            lis331dlh_exchange(s, 0x20, 0x27);
            qtest_clock_step(s, 20000000);
            lis331dlh_begin(s, 0xe8);
            for (unsigned i = 0; i < 6; i++) {
                g_assert_cmphex(lis331dlh_byte(s, 0), ==, samples[scale][i]);
            }
            lis331dlh_end(s);
        }
    }
    qtest_quit(s);

    s = start_rcc_options(
        "-device lis331dlh,bus=spi3,cs=1,x-mg=-8000,y-mg=8000,z-mg=0");
    qtest_writel(s, 0x4a009800, 2);
    qtest_writel(s, 0x480b8128, 1);
    qtest_writel(s, 0x480b8140, 0x603c0);
    qtest_writel(s, 0x480b8148, 1);
    lis331dlh_exchange(s, 0x20, 0x27);
    qtest_clock_step(s, 20000000);
    lis331dlh_begin(s, 0xe8);
    for (unsigned i = 0; i < 6; i++) {
        const uint8_t clipped[] = { 0x00, 0x80, 0xf0, 0x7f, 0x00, 0x00 };

        g_assert_cmphex(lis331dlh_byte(s, 0), ==, clipped[i]);
    }
    lis331dlh_end(s);
    qtest_quit(s);
}

static void test_tja1145_control_plane(void)
{
    QTestState *s = start_rcc_options("-device tja1145,bus=spi4,cs=0");

    qtest_writel(s, 0x4a009808, 2);
    qtest_writel(s, 0x480ba128, 1);
    qtest_writel(s, 0x480ba12c, 0x603c0);
    qtest_writel(s, 0x480ba134, 1);
    g_assert_cmphex(tja1145_exchange(s, 0xfd, 0), ==, 0x70);
    g_assert_cmphex(tja1145_exchange(s, 3, 0), ==, 4);
    g_assert_cmphex(tja1145_exchange(s, 7, 0), ==, 0x20);
    g_assert_cmphex(tja1145_exchange(s, 0x45, 0), ==, 0x48);
    g_assert_cmphex(tja1145_exchange(s, 0xc1, 0), ==, 1);
    g_assert_cmphex(tja1145_exchange(s, 0xc3, 0), ==, 0x10);
    g_assert_cmphex(tja1145_exchange(s, 0xc2, 0x10), ==, 0x10);
    g_assert_cmphex(tja1145_exchange(s, 0xc1, 0), ==, 0);
    tja1145_exchange(s, 0x40, 2);
    tja1145_exchange(s, 2, 7);
    g_assert_cmphex(tja1145_exchange(s, 3, 0), ==, 7);
    g_assert_cmphex(tja1145_exchange(s, 7, 0), ==, 0);
    g_assert_cmphex(tja1145_exchange(s, 0x45, 0), ==, 0xc8);
    tja1145_exchange(s, 2, 1); /* Sleep requires a real wake source enabled. */
    g_assert_cmphex(tja1145_exchange(s, 3, 0), ==, 4);
    tja1145_exchange(s, 0x46, 1);
    tja1145_exchange(s, 2, 1);
    g_assert_cmphex(tja1145_exchange(s, 3, 0), ==, 1);
    tja1145_exchange(s, 2, 4);
    tja1145_exchange(s, 0xc, 0x5a);
    tja1145_exchange(s, 0x14, 1);
    tja1145_exchange(s, 0xc, 0xa5);
    g_assert_cmphex(tja1145_exchange(s, 0xd, 0), ==, 0x5a);
    tja1145_exchange(s, 0x14, 0);
    tja1145_exchange(s, 0xc, 0xa5);
    g_assert_cmphex(tja1145_exchange(s, 0xd, 0), ==, 0xa5);

    /* An incomplete write must not change the addressed register. */
    qtest_writel(s, 0x480ba12c, 0x1603c0);
    qtest_writeb(s, 0x480ba138, 0xc);
    qtest_readb(s, 0x480ba13c);
    qtest_writel(s, 0x480ba12c, 0x603c0);
    g_assert_cmphex(tja1145_exchange(s, 0xd, 0), ==, 0xa5);
    g_assert_cmphex(tja1145_exchange(s, 0xc3, 0), ==, 2);
    tja1145_exchange(s, 0xc2, 2);
    g_assert_cmphex(tja1145_exchange(s, 0xc1, 0), ==, 0);

    /* Three payload bytes commit atomically at chip-select release. */
    qtest_writel(s, 0x480ba12c, 0x1603c0);
    for (unsigned i = 0; i < 4; i++) {
        qtest_writeb(s, 0x480ba138, i ? i * 0x11 : 0xc);
        qtest_readb(s, 0x480ba13c);
    }
    qtest_writel(s, 0x480ba12c, 0x603c0);
    g_assert_cmphex(tja1145_exchange(s, 0xd, 0), ==, 0x11);
    g_assert_cmphex(tja1145_exchange(s, 0xf, 0), ==, 0x22);
    g_assert_cmphex(tja1145_exchange(s, 0x11, 0), ==, 0x33);

    qtest_writel(s, 0x480ba12c, 0x1603c0);
    for (unsigned i = 0; i < 5; i++) {
        qtest_writeb(s, 0x480ba138, i ? 0xa5 : 0xc);
        qtest_readb(s, 0x480ba13c);
    }
    qtest_writel(s, 0x480ba12c, 0x603c0);
    g_assert_cmphex(tja1145_exchange(s, 0xd, 0), ==, 0x11);
    g_assert_cmphex(tja1145_exchange(s, 0xc3, 0), ==, 2);
    tja1145_exchange(s, 0xc2, 2);
    qtest_writel(s, 0x480ba12c, 0x1603c0);
    for (unsigned i = 0; i < 6; i++) {
        qtest_writeb(s, 0x480ba138, i ? 0x5a : 0xd);
        uint8_t response = qtest_readb(s, 0x480ba13c);

        if (i >= 4) {
            g_assert_cmphex(response, ==, 0x5a);
        }
    }
    qtest_writel(s, 0x480ba12c, 0x603c0);
    g_assert_cmphex(tja1145_exchange(s, 0xc3, 0), ==, 0);
    qtest_system_reset(s);
    qtest_writel(s, 0x4a009808, 2);
    qtest_writel(s, 0x480ba128, 1);
    qtest_writel(s, 0x480ba12c, 0x603c0);
    qtest_writel(s, 0x480ba134, 1);
    g_assert_cmphex(tja1145_exchange(s, 0xd, 0), ==, 0);
    g_assert_cmphex(tja1145_exchange(s, 0xc3, 0), ==, 0x10);
    qtest_quit(s);
}

static void test_dcan_message_ram_interfaces(void)
{
    static const uint32_t bases[] = { 0x4ae3c000, 0x48480000 };
    static const uint32_t clocks[] = { 0x4ae07888, 0x4a0098f0 };
    QTestState *s = start_rcc();

    qtest_irq_intercept_in(s, "/machine/interrupt-controller");
    for (unsigned i = 0; i < ARRAY_SIZE(bases); i++) {
        uint32_t base = bases[i];
        unsigned source = 222 + i * 3;

        qtest_writel(s, 0x4a002a48, source | ((source + 1) << 16));
        g_assert_cmphex(qtest_readl(s, base + 0x100), ==, 1);
        g_assert_cmphex(qtest_readl(s, base + 0x104), ==, UINT32_MAX);
        for (unsigned number = 1; number <= 128; number++) {
            qtest_writel(s, base + 0x104, 0x12340000 + number);
            qtest_writel(s, base + 0x108, 0x80000000 + number);
            qtest_writel(s, base + 0x10c, 0xa188);
            qtest_writel(s, base + 0x110, 0x01020300 + number);
            qtest_writel(s, base + 0x114, 0x05060700 + number);
            qtest_writew(s, base + 0x102, 0xf3);
            qtest_writeb(s, base + 0x100, number);
            g_assert_cmphex(qtest_readw(s, base + 0x100) & 0x8000, ==, 0);
            qtest_writel(s, base + 0x120, 0x730000 | number);
            g_assert_cmphex(qtest_readl(s, base + 0x124), ==,
                           0x32340000 + number);
            g_assert_cmphex(qtest_readl(s, base + 0x128), ==,
                           0x80000000 + number);
            g_assert_cmphex(qtest_readl(s, base + 0x12c), ==, 0xa188);
            g_assert_cmphex(qtest_readl(s, base + 0x130), ==,
                           0x01020300 + number);
            g_assert_cmphex(qtest_readl(s, base + 0x134), ==,
                           0x05060700 + number);
        }
        for (unsigned kind = 0; kind < 4; kind++) {
            uint32_t summary = base + 0x84 + 0x14 * kind;

            g_assert_cmphex(qtest_readl(s, summary), ==, 0xffff);
            for (unsigned group = 0; group < 4; group++) {
                g_assert_cmphex(qtest_readl(s, summary + 4 + group * 4),
                               ==, UINT32_MAX);
            }
        }
        qtest_writel(s, base, 0x21403);
        g_assert_cmphex(qtest_readl(s, base + 0x10), ==, 1);
        g_assert_true(qtest_get_irq(s, 4));
        qtest_writel(s, base + 0xd8, 1);
        g_assert_cmphex(qtest_readl(s, base + 0x10), ==, 0x10002);
        g_assert_true(qtest_get_irq(s, 7));
        qtest_writel(s, clocks[i], 0);
        g_assert_false(qtest_get_irq(s, 4));
        g_assert_false(qtest_get_irq(s, 7));
        qtest_writel(s, clocks[i], 2);
        g_assert_true(qtest_get_irq(s, 4));
        g_assert_true(qtest_get_irq(s, 7));

        qtest_writel(s, base + 0x120, 0x1c0001);
        g_assert_cmphex(qtest_readl(s, base + 0x12c), ==, 0xa188);
        qtest_writel(s, base + 0x120, 0x100001);
        g_assert_cmphex(qtest_readl(s, base + 0x12c), ==, 0x188);
        g_assert_cmphex(qtest_readl(s, base + 0x9c), ==, 0xfffffffe);
        g_assert_cmphex(qtest_readl(s, base + 0xb0), ==, 0xfffffffe);
        g_assert_cmphex(qtest_readl(s, base + 0x10), ==, 2);
        g_assert_false(qtest_get_irq(s, 7));
        qtest_writel(s, base, 0x21402);
        g_assert_cmphex(qtest_readl(s, base), ==, 0x21402);
        qtest_writel(s, base, 0x21403);
        qtest_writel(s, base + 0x120, 0x730000); /* Invalid object 0. */
        qtest_writel(s, base + 0x120, 0x734001); /* DMA unsupported. */
        qtest_writel(s, base + 0x120, 0x730081); /* Invalid object 129. */
        g_assert_cmphex(qtest_readl(s, base + 0x120), ==, 0x100001);
        qtest_writel(s, base, 0x9441);
        g_assert_cmphex(qtest_readl(s, base + 0x100), ==, 1);
        g_assert_cmphex(qtest_readl(s, base + 0x104), ==, UINT32_MAX);
        g_assert_cmphex(qtest_readl(s, base + 0xc4), ==, UINT32_MAX);
        qtest_writel(s, base + 0x120, 0x730080);
        g_assert_cmphex(qtest_readl(s, base + 0x134), ==, 0x05060780);
        qtest_writel(s, base + 0x114, 0xdecafbad);
        qtest_writel(s, base + 0x100, 0x810080); /* Only DATA_B changes. */
        qtest_writel(s, base + 0x120, 0x730080);
        g_assert_cmphex(qtest_readl(s, base + 0x130), ==, 0x01020380);
        g_assert_cmphex(qtest_readl(s, base + 0x134), ==, 0xdecafbad);
        g_assert_cmphex(qtest_readl(s, base + 0x128), ==, 0x80000080);
        qtest_writel(s, base + 0x110, 0xbadcafe0);
        qtest_writel(s, base + 0x100, 0x020080); /* Read only DATA_A. */
        g_assert_cmphex(qtest_readl(s, base + 0x110), ==, 0x01020380);
        g_assert_cmphex(qtest_readl(s, base + 0x114), ==, 0xdecafbad);
        qtest_writel(s, base + 0x10c, 0x80);
        qtest_writel(s, base + 0x100, 0x940080); /* TXRQ overrides IF flags. */
        qtest_writel(s, base + 0x120, 0x100080);
        g_assert_cmphex(qtest_readl(s, base + 0x12c), ==, 0x8180);
        g_assert_false(qtest_get_irq(s, 4));
    }
    qtest_system_reset(s);
    for (unsigned i = 0; i < ARRAY_SIZE(bases); i++) {
        g_assert_cmphex(qtest_readl(s, bases[i] + 0xc0), ==, 0);
        g_assert_cmphex(qtest_readl(s, bases[i] + 0x84), ==, 0);
    }
    qtest_quit(s);
}

static void test_dcan_disconnected_transmission(void)
{
    const uint32_t bases[] = { 0x4ae3c000, 0x48480000 };
    const uint32_t clocks[] = { 0x4ae07888, 0x4a0098f0 };
    QTestState *s = start_rcc();

    qtest_irq_intercept_in(s, "/machine/interrupt-controller");
    for (unsigned i = 0; i < ARRAY_SIZE(bases); i++) {
        uint32_t base = bases[i];

        qtest_writel(s, 0x4a002a48, 222 + i * 3);
        qtest_writel(s, base, 0x1441);
        qtest_writel(s, base + 0xc, 0x2301);
        qtest_writel(s, base + 0x108, 0xa48c0000);
        qtest_writel(s, base + 0x10c, 0x8988);
        qtest_writel(s, base + 0x110, 0x12345678);
        qtest_writel(s, base + 0x100, 0xb30001);
        qtest_writel(s, base, 0x1406);
        g_assert_cmphex(qtest_readl(s, base + 8), ==, 0);
        g_assert_cmphex(qtest_readl(s, base + 0x9c), ==, 0);
        g_assert_cmphex(qtest_readl(s, base + 0x88), ==, 1);
        qtest_clock_step(s, 123999);
        g_assert_cmphex(qtest_readl(s, base + 8), ==, 0);
        qtest_clock_step(s, 1);
        g_assert_cmphex(qtest_readl(s, base + 8), ==, 8);
        g_assert_true(qtest_get_irq(s, 4));
        g_assert_cmphex(qtest_readl(s, base + 0x10), ==, 0x8000);
        g_assert_cmphex(qtest_readl(s, base + 4), ==, 3);
        g_assert_false(qtest_get_irq(s, 4));
        g_assert_cmphex(qtest_readl(s, base + 4), ==, 7);
        g_assert_cmphex(qtest_readl(s, base + 0xac), ==, 0);
        qtest_writel(s, clocks[i], 0);
        for (unsigned attempt = 0; attempt < 20; attempt++) {
            qtest_clock_step(s, 1000000);
        }
        qtest_writel(s, clocks[i], 2);
        g_assert_cmphex(qtest_readl(s, base + 8), ==, 8);
        for (unsigned attempt = 0; attempt < 20; attempt++) {
            qtest_clock_step(s, 1000000);
        }
        g_assert_cmphex(qtest_readl(s, base + 8), ==, 128);
        g_assert_cmphex(qtest_readl(s, base + 4), ==, 0x63);
        g_assert_cmphex(qtest_readl(s, base + 4), ==, 0x67);
        g_assert_cmphex(qtest_readl(s, base + 0x88), ==, 1);
        qtest_writel(s, base, 0x1401);
        qtest_clock_step(s, 1000000);
        g_assert_cmphex(qtest_readl(s, base + 8), ==, 128);

        /* Single-shot failure clears TxRqst but retains NewDat. */
        qtest_writel(s, base, 0x9441);
        qtest_writel(s, base, 0x1441);
        qtest_writel(s, base + 0xc, 0x2303);
        qtest_writel(s, base + 0x108, 0xa48c0000);
        qtest_writel(s, base + 0x10c, 0x8988);
        qtest_writel(s, base + 0x100, 0xb00001);
        qtest_writel(s, base, 0x142a);
        g_assert_cmphex(qtest_readl(s, base + 0x88), ==, 0);
        g_assert_cmphex(qtest_readl(s, base + 0x9c), ==, 1);
        qtest_clock_step(s, 124000);
        qtest_writel(s, clocks[i], 0);
        qtest_clock_step(s, 1000000);
        qtest_writel(s, clocks[i], 2);
        qtest_clock_step(s, 123999);
        g_assert_cmphex(qtest_readl(s, base + 8), ==, 0);
        qtest_clock_step(s, 1);
        g_assert_cmphex(qtest_readl(s, base + 8), ==, 8);
        g_assert_cmphex(qtest_readl(s, base + 4), ==, 3);
        g_assert_false(qtest_get_irq(s, 4));
        qtest_clock_step(s, 1000000);
        g_assert_cmphex(qtest_readl(s, base + 8), ==, 8);
        qtest_writel(s, base, 0x1401);

        /* EIE fires at warning, not on each ACK failure; status wins priority. */
        qtest_writel(s, base, 0x9441);
        qtest_writel(s, base + 0x108, 0xa48c0000);
        qtest_writel(s, base + 0x10c, 0xa988);
        qtest_writel(s, base + 0x100, 0xb00001);
        qtest_writel(s, base, 0x140a);
        g_assert_cmphex(qtest_readl(s, base + 0x10), ==, 1);
        for (unsigned attempt = 0; attempt < 11; attempt++) {
            qtest_clock_step(s, 124000);
            g_assert_cmphex(qtest_readl(s, base + 0x10), ==, 1);
        }
        qtest_clock_step(s, 124000);
        g_assert_cmphex(qtest_readl(s, base + 8), ==, 96);
        g_assert_cmphex(qtest_readl(s, base + 0x10), ==, 0x8000);
        g_assert_cmphex(qtest_readl(s, base + 4), ==, 0x43);
        g_assert_cmphex(qtest_readl(s, base + 0x10), ==, 1);
        qtest_writel(s, base + 0x120, 0x180001);
        g_assert_false(qtest_get_irq(s, 4));
        for (unsigned attempt = 0; attempt < 6; attempt++) {
            qtest_clock_step(s, 124000);
        }
        g_assert_cmphex(qtest_readl(s, base + 8), ==, 128);
        g_assert_false(qtest_get_irq(s, 4));
        qtest_writel(s, base, 0x1401);
        qtest_writel(s, base, 0x9441);
        qtest_clock_step(s, 1000000);
        g_assert_cmphex(qtest_readl(s, base + 8), ==, 0);
        g_assert_cmphex(qtest_readl(s, base + 0x88), ==, 1);
        qtest_writel(s, base, 0x1406);
    }
    qtest_system_reset(s);
    for (unsigned i = 0; i < ARRAY_SIZE(bases); i++) {
        g_assert_cmphex(qtest_readl(s, bases[i] + 8), ==, 0);
        g_assert_cmphex(qtest_readl(s, bases[i] + 4), ==, 0);
    }
    qtest_quit(s);
}

static QTestState *start_dcan_bus(void)
{
    return start_rcc_options("-object can-bus,id=vehicle "
                             "-machine canbus0=vehicle,canbus1=vehicle");
}

static void dcan_message(QTestState *s, uint32_t base, unsigned number,
                         uint32_t mask, uint32_t arbitration, uint32_t control,
                         uint32_t data)
{
    qtest_writel(s, base + 0x104, mask);
    qtest_writel(s, base + 0x108, arbitration);
    qtest_writel(s, base + 0x10c, control);
    qtest_writel(s, base + 0x110, data);
    qtest_writel(s, base + 0x114, 0x76543210);
    qtest_writel(s, base + 0x100, 0xf30000 | number);
}

static void test_dcan_bus_receive(void)
{
    const uint32_t tx = 0x4ae3c000, rx = 0x48480000;
    QTestState *s = start_dcan_bus();

    qtest_irq_intercept_in(s, "/machine/interrupt-controller");
    qtest_writel(s, 0x4a002a48, 225 | (226 << 16));
    qtest_writel(s, tx, 0x1402);
    qtest_writel(s, rx, 0x21402);
    dcan_message(s, rx, 1, 0xdffc0000, 0x8f000000, 0x1484, 0);
    dcan_message(s, tx, 1, 0, 0xaf000000, 0x8984, 0x00020000);
    qtest_clock_step(s, 10000000);
    qtest_writel(s, rx + 0x120, 0x730001);
    g_assert_cmphex(qtest_readl(s, rx + 0x128), ==, 0x8f000000);
    g_assert_cmphex(qtest_readl(s, rx + 0x12c), ==, 0xb484);
    g_assert_cmphex(qtest_readl(s, rx + 0x130), ==, 0x00020000);
    g_assert_cmphex(qtest_readl(s, rx + 0x134), ==, 0x76543210);
    g_assert_cmphex(qtest_readl(s, rx + 0x10), ==, 1);
    g_assert_true(qtest_get_irq(s, 4));
    g_assert_cmphex(qtest_readl(s, rx + 4), ==, 0x10);
    g_assert_cmphex(qtest_readl(s, tx + 4), ==, 8);
    g_assert_cmphex(qtest_readl(s, tx + 8), ==, 0);
    g_assert_cmphex(qtest_readl(s, tx + 0x84), ==, 0);
    qtest_writel(s, tx + 0x120, 0x100001);
    g_assert_cmphex(qtest_readl(s, tx + 0x12c), ==, 0x2884);

    qtest_writel(s, rx + 0xd8, 1);
    g_assert_false(qtest_get_irq(s, 4));
    g_assert_true(qtest_get_irq(s, 7));
    qtest_writel(s, rx + 0x120, 0x1c0001);
    g_assert_false(qtest_get_irq(s, 7));
    g_assert_cmphex(qtest_readl(s, rx + 0x98), ==, 0);

    /* A healthy receiver ACKs even when its acceptance filter rejects the ID. */
    dcan_message(s, tx, 1, 0, 0xaf040000, 0x8984, 0xdeadbeef);
    qtest_clock_step(s, 10000000);
    g_assert_cmphex(qtest_readl(s, tx + 8), ==, 0);
    g_assert_cmphex(qtest_readl(s, tx + 0x84), ==, 0);
    g_assert_cmphex(qtest_readl(s, rx + 0x98), ==, 0);

    /* Extended-format reception must not alias an exact standard-ID object. */
    dcan_message(s, tx, 1, 0, 0xef000000, 0x8988, 0xdeadbeef);
    qtest_clock_step(s, 10000000);
    g_assert_cmphex(qtest_readl(s, rx + 0x98), ==, 0);
    dcan_message(s, rx, 2, 0xdfffffff, 0xcf000000, 0x1488, 0);
    dcan_message(s, tx, 1, 0, 0xef000000, 0x8988, 0x12345678);
    qtest_clock_step(s, 10000000);
    qtest_writel(s, rx + 0x120, 0x730002);
    g_assert_cmphex(qtest_readl(s, rx + 0x128), ==, 0xcf000000);
    g_assert_cmphex(qtest_readl(s, rx + 0x130), ==, 0x12345678);
    g_assert_cmphex(qtest_readl(s, rx + 0x134), ==, 0x76543210);

    qtest_writel(s, tx, 0x1422);
    dcan_message(s, tx, 1, 0, 0xaf000000, 0x8984, 0x00020000);
    g_assert_cmphex(qtest_readl(s, tx + 0x84), ==, 0);
    qtest_clock_step(s, 10000000);
    qtest_writel(s, tx + 0x120, 0x100001);
    g_assert_cmphex(qtest_readl(s, tx + 0x12c), ==, 0x2884);

    /* MDIR controls whether a transmit-direction object can accept data. */
    dcan_message(s, rx, 3, 0xdffc0000, 0xa4900000, 0x1484, 0);
    dcan_message(s, tx, 1, 0, 0xa4900000, 0x8984, 0xabcdef01);
    qtest_clock_step(s, 10000000);
    qtest_writel(s, rx + 0x120, 0x730003);
    g_assert_cmphex(qtest_readl(s, rx + 0x130), ==, 0);
    dcan_message(s, rx, 3, 0x9ffc0000, 0xa4900000, 0x1484, 0);
    dcan_message(s, tx, 1, 0, 0xa4900000, 0x8984, 0xabcdef01);
    qtest_clock_step(s, 10000000);
    qtest_writel(s, rx + 0x120, 0x730003);
    g_assert_cmphex(qtest_readl(s, rx + 0x128), ==, 0x84900000);
    g_assert_cmphex(qtest_readl(s, rx + 0x130), ==, 0xabcdef01);

    /* Standard objects ignore ID bits 17:0 even without an acceptance mask. */
    dcan_message(s, rx, 4, 0, 0x84940123, 0x484, 0);
    dcan_message(s, tx, 1, 0, 0xa4940000, 0x8984, 0x10203040);
    qtest_clock_step(s, 10000000);
    qtest_writel(s, rx + 0x120, 0x730004);
    g_assert_cmphex(qtest_readl(s, rx + 0x128), ==, 0x84940000);
    g_assert_cmphex(qtest_readl(s, rx + 0x130), ==, 0x10203040);

    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, rx + 0x98), ==, 0);
    qtest_quit(s);
}

static void test_dcan_receive_fifo(void)
{
    const uint32_t tx = 0x4ae3c000, rx = 0x48480000;
    QTestState *s = start_dcan_bus();

    qtest_writel(s, tx, 0x1402);
    qtest_writel(s, rx, 0x1402);
    dcan_message(s, rx, 1, 0xdff80000, 0x8f000000, 0x1404, 0);
    dcan_message(s, rx, 2, 0xdff80000, 0x8f000000, 0x1484, 0);
    for (unsigned i = 1; i <= 3; i++) {
        dcan_message(s, tx, 1, 0, 0xaf040000, 0x8984, i);
        qtest_clock_step(s, 10000000);
    }
    qtest_writel(s, rx + 0x120, 0x730001);
    g_assert_cmphex(qtest_readl(s, rx + 0x128), ==, 0x8f040000);
    g_assert_cmphex(qtest_readl(s, rx + 0x12c), ==, 0xb404);
    g_assert_cmphex(qtest_readl(s, rx + 0x130), ==, 1);
    qtest_writel(s, rx + 0x120, 0x730002);
    g_assert_cmphex(qtest_readl(s, rx + 0x12c), ==, 0xf484);
    g_assert_cmphex(qtest_readl(s, rx + 0x130), ==, 3);
    qtest_writel(s, rx + 0x120, 0x1c0001);
    dcan_message(s, tx, 1, 0, 0xaf000000, 0x8984, 4);
    qtest_clock_step(s, 10000000);
    qtest_writel(s, rx + 0x120, 0x730001);
    g_assert_cmphex(qtest_readl(s, rx + 0x130), ==, 4);
    qtest_quit(s);
}

static void test_dcan_peer_clock_and_init(void)
{
    const uint32_t tx = 0x4ae3c000, rx = 0x48480000;
    QTestState *s = start_dcan_bus();

    qtest_writel(s, tx, 0x1402);
    dcan_message(s, rx, 1, 0xdffc0000, 0x8f000000, 0x1484, 0);
    dcan_message(s, tx, 1, 0, 0xaf000000, 0x8984, 1);
    qtest_clock_step(s, 10000000);
    g_assert_cmphex(qtest_readl(s, tx + 8), >, 0);
    g_assert_cmphex(qtest_readl(s, rx + 0x98), ==, 0);
    qtest_writel(s, rx, 0x1402);
    qtest_clock_step(s, 10000000);
    g_assert_cmphex(qtest_readl(s, tx + 0x84), ==, 0);
    g_assert_cmphex(qtest_readl(s, rx + 0x98), ==, 1);
    qtest_writel(s, rx + 0x120, 0x1c0001);
    qtest_writel(s, 0x4a0098f0, 0);
    dcan_message(s, tx, 1, 0, 0xaf000000, 0x8984, 2);
    qtest_clock_step(s, 10000000);
    g_assert_cmphex(qtest_readl(s, tx + 0x84), ==, 1);
    g_assert_cmphex(qtest_readl(s, rx + 0x98), ==, 0);
    qtest_writel(s, 0x4a0098f0, 2);
    qtest_clock_step(s, 10000000);
    qtest_writel(s, rx + 0x120, 0x730001);
    g_assert_cmphex(qtest_readl(s, rx + 0x130), ==, 2);
    g_assert_cmphex(qtest_readl(s, tx + 0x84), ==, 0);
    qtest_quit(s);
}

static void dcan_wait_data(QTestState *s, uint32_t base, uint32_t expected)
{
    int64_t deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;

    while (qtest_readl(s, base + 0x98) != expected &&
           g_get_monotonic_time() < deadline) {
        g_usleep(1000);
    }
    g_assert_cmphex(qtest_readl(s, base + 0x98), ==, expected);
}

static void test_tja1145_can_activity(const void *opaque)
{
    bool wakeup = GPOINTER_TO_UINT(opaque);
    g_autofree char *options = g_strdup_printf(
        "-object can-bus,id=vehicle -machine canbus1=vehicle "
        "-device tja1145,bus=spi4,cs=0,canbus=vehicle,"
        "experimental-frame-wakeup=%s", wakeup ? "on" : "off");
    const uint32_t tx = 0x48480000;
    QTestState *s = start_rcc_options(options);

    qtest_writel(s, 0x4a009808, 2);
    qtest_writel(s, 0x480ba128, 1);
    qtest_writel(s, 0x480ba12c, 0x603c0);
    qtest_writel(s, 0x480ba134, 1);
    tja1145_exchange(s, 0xc2, 0x10);
    qtest_writel(s, tx, 0x1422);
    dcan_message(s, tx, 1, 0, 0xaf000000, 0x8984, 0x00020000);
    qtest_clock_step_next(s);
    g_assert_cmphex(tja1145_exchange(s, 0x45, 0), ==, 0x40);
    g_assert_cmphex(tja1145_exchange(s, 0xc7, 0), ==, 0);
    g_assert_cmphex(qtest_readl(s, tx + 8), ==, 8);

    tja1145_exchange(s, 0x46, 0x11);
    tja1145_exchange(s, 2, 1);
    g_assert_cmphex(tja1145_exchange(s, 3, 0), ==, 1);
    dcan_message(s, tx, 1, 0, 0xaf000000, 0x8984, 0x00020000);
    qtest_clock_step_next(s);
    g_assert_cmphex(tja1145_exchange(s, 3, 0), ==, wakeup ? 4 : 1);
    g_assert_cmphex(tja1145_exchange(s, 0xc7, 0), ==, wakeup ? 1 : 0);
    g_assert_cmphex(tja1145_exchange(s, 0xc1, 0), ==, wakeup ? 4 : 0);
    g_assert_cmphex(qtest_readl(s, tx + 8), ==, 16);
    tja1145_exchange(s, 0xc6, 1);
    qtest_clock_step(s, 999999999);
    g_assert_cmphex(tja1145_exchange(s, 0x45, 0), ==, 0x40);
    g_assert_cmphex(tja1145_exchange(s, 0xc7, 0), ==, 0);
    qtest_clock_step(s, 1);
    g_assert_cmphex(tja1145_exchange(s, 0x45, 0), ==, 0x48);
    g_assert_cmphex(tja1145_exchange(s, 0xc7, 0), ==, 0x10);
    tja1145_exchange(s, 0xc6, 0x10);
    g_assert_cmphex(tja1145_exchange(s, 0xc1, 0), ==, 0);

    tja1145_exchange(s, 0x40, 1);
    tja1145_exchange(s, 2, 7);
    dcan_message(s, tx, 1, 0, 0xaf000000, 0x8984, 0x00020000);
    qtest_clock_step_next(s);
    g_assert_cmphex(tja1145_exchange(s, 0x45, 0), ==, 0xc0);
    g_assert_cmphex(tja1145_exchange(s, 0xc7, 0), ==, 0);
    g_assert_cmphex(tja1145_exchange(s, 3, 0), ==, 7);
    qtest_system_reset(s);
    qtest_writel(s, 0x4a009808, 2);
    qtest_writel(s, 0x480ba128, 1);
    qtest_writel(s, 0x480ba12c, 0x603c0);
    qtest_writel(s, 0x480ba134, 1);
    g_assert_cmphex(tja1145_exchange(s, 3, 0), ==, 4);
    g_assert_cmphex(tja1145_exchange(s, 0x45, 0), ==, 0x48);
    tja1145_exchange(s, 0xc2, 0x10);
    tja1145_exchange(s, 0x46, 0x10);
    qtest_clock_step(s, 1000000000);
    g_assert_cmphex(tja1145_exchange(s, 0xc1, 0), ==, 0);
    qtest_quit(s);
}

static void test_dcan_chardev_peer(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("mh2p-can-test-XXXXXX", &error);
    g_autofree char *path = NULL;
    g_autofree char *options = NULL;
    const uint32_t base = 0x4ae3c000;
    uint8_t frame[16] = { 0 }, received[16];
    struct timeval timeout = { .tv_sec = 5 };
    QTestState *s;
    int server, peer;

    g_assert_no_error(error);
    path = g_build_filename(dir, "can.sock", NULL);
    server = qtest_socket_server(path);
    options = g_strdup_printf("-object can-bus,id=vehicle "
        "-chardev socket,id=canpeer,path=%s "
        "-object can-host-chardev,id=peer,canbus=vehicle,chardev=canpeer "
        "-machine canbus0=vehicle", path);
    s = start_rcc_options(options);
    peer = accept(server, NULL, NULL);
    g_assert_cmpint(peer, >=, 0);
    g_assert_cmpint(setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO,
                               &timeout, sizeof(timeout)), ==, 0);
    qtest_writel(s, base, 0x1402);
    dcan_message(s, base, 1, 0xdffc0000, 0x8f000000, 0x1484, 0);
    stl_le_p(frame, 0x3c0);
    frame[4] = 4;
    frame[10] = 2;
    g_assert_cmpint(write(peer, frame, 7), ==, 7);
    g_usleep(10000);
    g_assert_cmphex(qtest_readl(s, base + 0x98), ==, 0);
    g_assert_cmpint(write(peer, frame + 7, 9), ==, 9);
    dcan_wait_data(s, base, 1);
    qtest_writel(s, base + 0x120, 0x730001);
    g_assert_cmphex(qtest_readl(s, base + 0x130), ==, 0x00020000);
    qtest_writel(s, base + 0x120, 0x1c0001);

    /* A rejected record must not desynchronize the following valid one. */
    frame[4] = 9;
    g_assert_cmpint(write(peer, frame, 16), ==, 16);
    frame[4] = 4;
    frame[10] = 0;
    g_assert_cmpint(write(peer, frame, 16), ==, 16);
    dcan_wait_data(s, base, 1);
    qtest_writel(s, base + 0x120, 0x730001);
    g_assert_cmphex(qtest_readl(s, base + 0x130), ==, 0);

    dcan_message(s, base, 2, 0, 0xa48c0000, 0x8984, 0x12345678);
    qtest_clock_step(s, 10000000);
    for (unsigned used = 0; used < sizeof(received); ) {
        ssize_t size = read(peer, received + used, sizeof(received) - used);

        g_assert_cmpint(size, >, 0);
        used += size;
    }
    g_assert_cmphex(ldl_le_p(received), ==, 0x123);
    g_assert_cmphex(received[4], ==, 4);
    g_assert_cmphex(ldl_le_p(received + 8), ==, 0x12345678);
    g_assert_cmphex(ldl_le_p(received + 12), ==, 0);
    g_assert_cmphex(qtest_readl(s, base + 0x84), ==, 0);
    close(peer);
    g_usleep(10000);
    dcan_message(s, base, 2, 0, 0xa48c0000, 0x8984, 0);
    qtest_clock_step(s, 10000000);
    g_assert_cmphex(qtest_readl(s, base + 0x84), ==, 1);
    g_assert_cmphex(qtest_readl(s, base + 8), >, 0);
    qtest_quit(s);
    close(server);
    unlink(path);
    rmdir(dir);
}

static void test_dcan_initialization(void)
{
    static const uint32_t bases[] = { 0x4ae3c000, 0x48480000 };
    static const uint32_t clocks[] = { 0x4ae07888, 0x4a0098f0 };
    QTestState *s = start_rcc();

    for (unsigned i = 0; i < ARRAY_SIZE(bases); i++) {
        uint32_t base = bases[i];

        g_assert_cmphex(qtest_readl(s, clocks[i]), ==, 2);
        g_assert_cmphex(qtest_readl(s, base), ==, 0x1401);
        g_assert_cmphex(qtest_readl(s, base + 4), ==, 0);
        g_assert_cmphex(qtest_readl(s, base + 4), ==, 7);
        g_assert_cmphex(qtest_readl(s, base + 8), ==, 0);
        g_assert_cmphex(qtest_readl(s, base + 0x10), ==, 0);
        g_assert_cmphex(qtest_readl(s, base + 0xc), ==, 0x2301);
        qtest_writel(s, base + 0x14, 0x3ff);
        g_assert_cmphex(qtest_readl(s, base + 0x14), ==, 0x80);
        qtest_writel(s, base + 0x80, 0xdeadbeef);
        g_assert_cmphex(qtest_readl(s, base + 0x80), ==, 0xdeadbeef);
        qtest_writel(s, base, 0x2001441);
        g_assert_cmphex(qtest_readl(s, base), ==, 0x2001441);
        g_assert_cmphex(qtest_readl(s, base + 4) & 0x600, ==, 0);
        qtest_writel(s, base, 0x3001441); /* Power-down engine absent. */
        g_assert_cmphex(qtest_readl(s, base), ==, 0x2001441);
        qtest_writel(s, base, 0x1401);
        qtest_writel(s, base + 0xc, 0x1234);
        g_assert_cmphex(qtest_readl(s, base + 0xc), ==, 0x2301);
        qtest_writel(s, base, 0x1441);
        qtest_writel(s, base + 0xc, 0x71234);
        g_assert_cmphex(qtest_readl(s, base + 0xc), ==, 0x71234);
        qtest_writel(s, base + 0xc, 0x8000);
        g_assert_cmphex(qtest_readl(s, base + 0xc), ==, 0x71234);
        qtest_writel(s, base, 0x2140e);
        qtest_writel(s, base + 0xc, 0x1234);
        g_assert_cmphex(qtest_readl(s, base + 0xc), ==, 0x71234);
        qtest_writel(s, base, 0x2940f); /* Reset requires existing INIT. */
        g_assert_cmphex(qtest_readl(s, base), ==, 0x2140e);
        qtest_writel(s, base, 0x1441);
        qtest_writel(s, base, 0x14c1); /* Test engine absent. */
        qtest_writel(s, base, 0x4001441); /* Reserved bits. */
        qtest_writel(s, base, 0x41); /* Parity engine absent. */
        qtest_writeb(s, base, 0);
        g_assert_cmphex(qtest_readl(s, base), ==, 0x1441);
        qtest_writel(s, base, 0x9441);
        g_assert_cmphex(qtest_readl(s, base), ==, 0x1401);
        g_assert_cmphex(qtest_readl(s, base + 0x80), ==, 0);
        g_assert_cmphex(qtest_readl(s, base + 0xc), ==, 0x2301);
        g_assert_cmphex(qtest_readl(s, base + 4), ==, 0);
        qtest_writel(s, base, 0x1441);
        qtest_writel(s, base + 0xc, 0x4321);
        qtest_writel(s, clocks[i], 0);
        g_assert_cmphex(qtest_readl(s, clocks[i]), ==, 0x30000);
        qtest_writel(s, base, 0x9441);
        qtest_writel(s, clocks[i], 2);
        g_assert_cmphex(qtest_readl(s, base + 0xc), ==, 0x4321);
        qtest_writel(s, clocks[i], 1);
        qtest_writel(s, clocks[i], 3);
        g_assert_cmphex(qtest_readl(s, clocks[i]), ==, 2);
        qtest_writel(s, clocks[i], 0x1000002);
        g_assert_cmphex(qtest_readl(s, clocks[i]), ==, i ? 2 : 0x1000002);
        qtest_writel(s, clocks[i], 0);
    }
    qtest_system_reset(s);
    for (unsigned i = 0; i < ARRAY_SIZE(bases); i++) {
        g_assert_cmphex(qtest_readl(s, bases[i]), ==, 0x1401);
        g_assert_cmphex(qtest_readl(s, bases[i] + 0xc), ==, 0x2301);
        g_assert_cmphex(qtest_readl(s, bases[i] + 4), ==, 0);
        g_assert_cmphex(qtest_readl(s, clocks[i]), ==, 2);
    }
    qtest_quit(s);
}

static void test_ipu_peripheral_clock_domain(void)
{
    const uint32_t domain = 0x4a005540;
    const uint32_t uart6 = 0x4a005580;
    const uint32_t i2c5 = 0x4a005578;
    QTestState *s = start_rcc();

    g_assert_cmphex(qtest_readl(s, domain), ==, 0x6103);
    qtest_writel(s, uart6, 0);
    g_assert_cmphex(qtest_readl(s, domain), ==, 3);
    qtest_writel(s, domain, 2);
    g_assert_cmphex(qtest_readl(s, domain), ==, 0x102);
    qtest_writel(s, domain, 0);
    g_assert_cmphex(qtest_readl(s, domain), ==, 0x100);
    qtest_writel(s, domain, 3);
    g_assert_cmphex(qtest_readl(s, domain), ==, 3);
    qtest_writel(s, domain, 0);
    g_assert_cmphex(qtest_readl(s, domain), ==, 0);
    qtest_writel(s, i2c5, 2);
    g_assert_cmphex(qtest_readl(s, domain), ==, 0x2100);
    qtest_writel(s, uart6, 2);
    g_assert_cmphex(qtest_readl(s, domain), ==, 0x6100);
    qtest_writel(s, i2c5, 0);
    g_assert_cmphex(qtest_readl(s, domain), ==, 0x6100);
    qtest_writel(s, uart6, 0);
    g_assert_cmphex(qtest_readl(s, domain), ==, 0x100);
    qtest_writel(s, domain, 1);
    g_assert_cmphex(qtest_readl(s, domain), ==, 1);
    qtest_writel(s, uart6, 2);
    g_assert_cmphex(qtest_readl(s, domain), ==, 0x6101);
    qtest_writel(s, uart6, 0);
    g_assert_cmphex(qtest_readl(s, domain), ==, 1);
    for (unsigned timer = 0; timer < 4; timer++) {
        uint32_t clock = 0x4a005558 + timer * 8;

        qtest_writel(s, clock, 2);
        g_assert_cmphex(qtest_readl(s, domain),
                        ==, 0x101 | (1 << (9 + timer)));
        qtest_writel(s, clock, 0);
        g_assert_cmphex(qtest_readl(s, domain), ==, 1);
    }
    qtest_writel(s, domain, 0x77f02);
    g_assert_cmphex(qtest_readl(s, domain), ==, 0x102);
    qtest_writel(s, domain, 0x80000003);
    qtest_writew(s, domain, 3);
    g_assert_cmphex(qtest_readl(s, domain), ==, 0x102);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, domain), ==, 0x6103);
    qtest_quit(s);
}

static void test_atl_clock_controls(void)
{
    QTestState *s = start_rcc();
    const uint32_t module = 0x4a008c00;
    const uint32_t domain = 0x4a008c20;

    g_assert_cmphex(qtest_readl(s, module), ==, 0x30000);
    g_assert_cmphex(qtest_readl(s, domain), ==, 3);
    qtest_writel(s, domain, 2);
    g_assert_cmphex(qtest_readl(s, domain), ==, 0x102);
    g_assert_cmphex(qtest_readl(s, module), ==, 0x30000);
    qtest_writel(s, module, qtest_readl(s, module) | 2);
    g_assert_cmphex(qtest_readl(s, module), ==, 2);
    g_assert_cmphex(qtest_readl(s, domain), ==, 0x302);
    qtest_writel(s, domain, qtest_readl(s, domain));
    g_assert_cmphex(qtest_readl(s, domain), ==, 0x302);
    qtest_writel(s, module, 1);
    qtest_writel(s, module, 0x4000002); /* Unmodeled clock source. */
    qtest_writew(s, module, 0);
    g_assert_cmphex(qtest_readl(s, module), ==, 2);
    qtest_writel(s, domain, 1);
    g_assert_cmphex(qtest_readl(s, domain), ==, 0x302);
    qtest_writel(s, domain, 3);
    g_assert_cmphex(qtest_readl(s, domain), ==, 0x303);
    qtest_writel(s, module, 0);
    g_assert_cmphex(qtest_readl(s, module), ==, 0x30000);
    g_assert_cmphex(qtest_readl(s, domain), ==, 3);
    qtest_writel(s, domain, 0);
    g_assert_cmphex(qtest_readl(s, domain), ==, 0);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, module), ==, 0x30000);
    g_assert_cmphex(qtest_readl(s, domain), ==, 3);
    qtest_quit(s);
}

static void test_mlb_clock_controls(void)
{
    g_autofree char *logfile = NULL;
    g_autofree char *contents = NULL;
    g_autoptr(GError) error = NULL;
    int fd = g_file_open_tmp("mh2p-rcc-mlb-clock-XXXXXX", &logfile, &error);
    g_autofree char *options = NULL;
    QTestState *s;
    const uint32_t module = 0x4a009358;

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    options = g_strdup_printf("-d unimp,guest_errors -D %s", logfile);
    s = start_rcc_options(options);
    g_assert_cmphex(qtest_readl(s, module), ==, 0x70000);
    qtest_writel(s, module, 2); /* Original MOST clock request. */
    g_assert_cmphex(qtest_readl(s, module), ==, 0x40002);
    qtest_writel(s, module, 0x70002); /* Status fields are read-only. */
    g_assert_cmphex(qtest_readl(s, module), ==, 0x40002);
    qtest_writel(s, module, 1); /* Reserved MODULEMODE. */
    qtest_writel(s, module, 3);
    qtest_writel(s, module, 0x80000002);
    qtest_writew(s, module, 0);
    qtest_writeb(s, module, 0);
    qtest_readw(s, module);
    g_assert_cmphex(qtest_readl(s, module), ==, 0x40002);
    qtest_writel(s, module, 0x40000);
    g_assert_cmphex(qtest_readl(s, module), ==, 0x70000);
    qtest_writel(s, module, 2);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, module), ==, 0x70000);
    g_assert_cmphex(qtest_readl(s, 0x4ae0735c), ==, 0x101);
    qtest_writel(s, 0x4ae0735c, 0xfffffefe); /* Original MOST write. */
    g_assert_cmphex(qtest_readl(s, 0x4ae0735c), ==, 0x101);
    qtest_writew(s, 0x4ae0735c, 0x101);
    qtest_writeb(s, 0x4ae0735c, 1);
    g_assert_cmphex(qtest_readl(s, 0x4ae0735c), ==, 0x101);
    qtest_writel(s, 0x4ae0735c, 1);
    g_assert_cmphex(qtest_readl(s, 0x4ae0735c), ==, 0x100);
    qtest_writel(s, 0x4ae0735c, 0x100);
    g_assert_cmphex(qtest_readl(s, 0x4ae0735c), ==, 0);
    qtest_writel(s, 0x4ae0735c, 0x101);
    qtest_writel(s, module, 2);
    qtest_writel(s, module, 0);
    g_assert_cmphex(qtest_readl(s, 0x4ae0735c), ==, 0);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, 0x4ae0735c), ==, 0x101);
    g_assert_cmphex(qtest_readl(s, 0x4a002698), ==, 0);
    for (unsigned pressure = 0; pressure < 8; pressure++) {
        qtest_writel(s, 0x4a002698, pressure);
        g_assert_cmphex(qtest_readl(s, 0x4a002698), ==, pressure);
    }
    qtest_writel(s, 0x4a002698, 8);
    qtest_writew(s, 0x4a002698, 0);
    qtest_writeb(s, 0x4a002698, 0);
    g_assert_cmphex(qtest_readl(s, 0x4a002698), ==, 7);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, 0x4a002698), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x4a0026d0), ==, 1);
    qtest_writel(s, 0x4a0026d0, 0x400003); /* Original MOST request. */
    g_assert_cmphex(qtest_readl(s, 0x4a0026d0), ==, 3);
    qtest_writel(s, 0x4a0026d0, 0x100fc);
    g_assert_cmphex(qtest_readl(s, 0x4a0026d0), ==, 0x100fc);
    qtest_writew(s, 0x4a0026d0, 0);
    qtest_writeb(s, 0x4a0026d0, 0);
    g_assert_cmphex(qtest_readl(s, 0x4a0026d0), ==, 0x100fc);
    qtest_writel(s, 0x4a0026d0, 0);
    g_assert_cmphex(qtest_readl(s, 0x4a0026d0), ==, 0);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, 0x4a0026d0), ==, 1);
    for (unsigned io = 0; io < 2; io++) {
        uint32_t address = 0x4a0026c8 + io * 4;

        g_assert_cmphex(qtest_readl(s, address), ==, 0x30);
        qtest_writel(s, address, UINT32_MAX);
        g_assert_cmphex(qtest_readl(s, address), ==, 0x3f3f78);
        qtest_writew(s, address, 0);
        qtest_writeb(s, address, 0);
        g_assert_cmphex(qtest_readl(s, address), ==, 0x3f3f78);
        g_assert_cmphex(qtest_readl(s, 0x4a0026c8 + (1 - io) * 4), ==, 0x30);
        qtest_writel(s, address, 0);
        g_assert_cmphex(qtest_readl(s, address), ==, 0);
        qtest_system_reset(s);
        g_assert_cmphex(qtest_readl(s, address), ==, 0x30);
    }
    qtest_quit(s);
    g_assert_true(g_file_get_contents(logfile, &contents, NULL, &error));
    g_assert_no_error(error);
    g_assert_null(strstr(contents, "size=4 value=0x2 "));
    g_assert_null(strstr(contents, "size=4 value=0x70002 "));
    g_assert_null(strstr(contents,
                         "unsupported write at 0x4ae0735c size=4"));
    g_assert_nonnull(strstr(contents,
                           "ignoring reserved MLB clock/bandgap bits "
                           "0x400000"));
    g_assert_null(strstr(contents,
                         "unsupported write at 0x4a0026d0 size=4"));
    g_assert_nonnull(strstr(contents, "size=4 value=0x1 "));
    g_assert_nonnull(strstr(contents, "size=4 value=0x3 "));
    g_assert_nonnull(strstr(contents, "size=4 value=0x80000002 "));
    g_assert_nonnull(strstr(contents, "size=2 value=0x0 "));
    g_assert_nonnull(strstr(contents, "size=1 value=0x0 "));
    g_assert_nonnull(strstr(contents, "unsupported read at 0x4a009358 size=2"));
    unlink(logfile);
}

static void test_mlb_dll_clock_controls(void)
{
    g_autofree char *logfile = NULL;
    g_autofree char *contents = NULL;
    g_autoptr(GError) error = NULL;
    int fd = g_file_open_tmp("mh2p-rcc-mlb-dll-XXXXXX", &logfile, &error);
    g_autofree char *options = NULL;
    QTestState *s;
    const uint32_t dll = 0x4a002798;
    const uint32_t clock = 0x4a00279c;

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    options = g_strdup_printf("-d unimp,guest_errors -D %s", logfile);
    s = start_rcc_options(options);
    g_assert_cmphex(qtest_readl(s, dll), ==, 0);
    g_assert_cmphex(qtest_readl(s, clock), ==, 0);
    qtest_writel(s, dll, qtest_readl(s, dll) | (1U << 10));
    qtest_writel(s, clock, qtest_readl(s, clock) & ~1U);
    g_assert_cmphex(qtest_readl(s, dll), ==, 1U << 10);
    g_assert_cmphex(qtest_readl(s, clock), ==, 0);
    qtest_writel(s, 0x4a009358, 2);
    qtest_clock_step(s, 1000000000);
    g_assert_cmphex(qtest_readl(s, dll), ==, 1U << 10);
    for (unsigned bit = 0; bit <= 10; bit++) {
        qtest_writel(s, dll, 1U << bit);
        g_assert_cmphex(qtest_readl(s, dll), ==, 1U << bit);
    }
    qtest_writel(s, dll, UINT32_MAX);
    qtest_writel(s, clock, UINT32_MAX);
    g_assert_cmphex(qtest_readl(s, dll), ==, 0x7ff);
    g_assert_cmphex(qtest_readl(s, clock), ==, 1);
    qtest_writew(s, dll, 0);
    qtest_writeb(s, dll, 0);
    qtest_writew(s, clock, 0);
    qtest_writeb(s, clock, 0);
    qtest_readw(s, dll);
    qtest_readb(s, clock);
    g_assert_cmphex(qtest_readl(s, dll), ==, 0x7ff);
    g_assert_cmphex(qtest_readl(s, clock), ==, 1);
    qtest_writel(s, dll, 0);
    qtest_clock_step(s, 1000000000);
    g_assert_cmphex(qtest_readl(s, dll), ==, 0);
    g_assert_cmphex(qtest_readl(s, clock), ==, 1);
    qtest_writel(s, dll, 0x4ab);
    qtest_writel(s, clock, 0);
    g_assert_cmphex(qtest_readl(s, dll), ==, 0x4ab);
    g_assert_cmphex(qtest_readl(s, clock), ==, 0);
    qtest_writel(s, clock, 1);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, dll), ==, 0);
    g_assert_cmphex(qtest_readl(s, clock), ==, 0);
    qtest_quit(s);
    g_assert_true(g_file_get_contents(logfile, &contents, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(contents,
                           "ignoring reserved MLB DLL bits 0xfffff800"));
    g_assert_nonnull(strstr(contents,
                           "ignoring reserved MLB clock bits 0xfffffffe"));
    g_assert_null(strstr(contents,
                         "unsupported read at 0x4a002798 size=4"));
    g_assert_null(strstr(contents,
                         "unsupported write at 0x4a002798 size=4"));
    g_assert_null(strstr(contents,
                         "unsupported read at 0x4a00279c size=4"));
    g_assert_null(strstr(contents,
                         "unsupported write at 0x4a00279c size=4"));
    g_assert_nonnull(strstr(contents,
                           "unsupported read at 0x4a002798 size=2"));
    g_assert_nonnull(strstr(contents,
                           "unsupported read at 0x4a00279c size=1"));
    g_assert_nonnull(strstr(contents,
                           "unsupported write at 0x4a002798 size=2"));
    g_assert_nonnull(strstr(contents,
                           "unsupported write at 0x4a002798 size=1"));
    g_assert_nonnull(strstr(contents,
                           "unsupported write at 0x4a00279c size=2"));
    g_assert_nonnull(strstr(contents,
                           "unsupported write at 0x4a00279c size=1"));
    unlink(logfile);
}

static void test_mlb_channel_table(void)
{
    g_autofree char *logfile = NULL;
    g_autofree char *contents = NULL;
    g_autoptr(GError) error = NULL;
    int fd = g_file_open_tmp("mh2p-rcc-mlb-table-XXXXXX", &logfile, &error);
    g_autofree char *options = NULL;
    const uint32_t data = 0x4842c4c0, mask = 0x4842c4d0;
    const uint32_t control = 0x4842c4e0, address = 0x4842c4e4;
    const uint32_t masks[] = { 0xaaaaaaaa, 0x55555555, 0xffff, 0xffff0000 };
    QTestState *s;

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    options = g_strdup_printf("-d unimp,guest_errors -D %s", logfile);
    s = start_rcc_options(options);
    qtest_writel(s, 0x4a009358, 2);
    g_assert_cmphex(qtest_readl(s, control), ==, 0);
    g_assert_cmphex(qtest_readl(s, address), ==, 0);
    qtest_writel(s, control, UINT32_MAX);
    g_assert_cmphex(qtest_readl(s, control), ==, 0);
    for (unsigned word = 0; word < 4; word++) {
        g_assert_cmphex(qtest_readl(s, data + word * 4), ==, 0);
        g_assert_cmphex(qtest_readl(s, mask + word * 4), ==, 0);
        qtest_writel(s, mask + word * 4, UINT32_MAX);
    }
    for (unsigned entry = 0; entry < 256; entry++) {
        for (unsigned word = 0; word < 4; word++) {
            qtest_writel(s, data + word * 4,
                         0xf0f0f0f0U ^ (entry * 0x01010101U) ^ word);
        }
        qtest_writel(s, address, 0x80000000U | entry);
        g_assert_cmphex(qtest_readl(s, control), ==, 1);
        qtest_writel(s, control, 1);
        g_assert_cmphex(qtest_readl(s, control), ==, 1);
        qtest_writel(s, control, 0);
        g_assert_cmphex(qtest_readl(s, control), ==, 0);
    }
    for (unsigned entry = 0; entry < 256; entry++) {
        qtest_writel(s, address, entry);
        g_assert_cmphex(qtest_readl(s, address), ==, entry);
        g_assert_cmphex(qtest_readl(s, control), ==, 1);
        for (unsigned word = 0; word < 4; word++) {
            g_assert_cmphex(qtest_readl(s, data + word * 4), ==,
                            0xf0f0f0f0U ^ (entry * 0x01010101U) ^ word);
        }
    }
    for (unsigned word = 0; word < 4; word++) {
        qtest_writel(s, data + word * 4, 0x12345678);
        qtest_writel(s, mask + word * 4, masks[word]);
    }
    qtest_writel(s, address, 0x80000035);
    qtest_writel(s, address, 0x35);
    for (unsigned word = 0; word < 4; word++) {
        uint32_t old = 0xf0f0f0f0U ^ 0x35353535U ^ word;
        uint32_t expected = (old & ~masks[word]) | (0x12345678 & masks[word]);

        g_assert_cmphex(qtest_readl(s, data + word * 4), ==, expected);
        g_assert_cmphex(qtest_readl(s, mask + word * 4), ==, masks[word]);
        qtest_writel(s, mask + word * 4, 0);
        qtest_writel(s, data + word * 4, 0);
    }
    qtest_writel(s, address, 0x80000035);
    qtest_writel(s, address, 0x35);
    for (unsigned word = 0; word < 4; word++) {
        uint32_t old = 0xf0f0f0f0U ^ 0x35353535U ^ word;

        g_assert_cmphex(qtest_readl(s, data + word * 4), ==,
                        (old & ~masks[word]) | (0x12345678 & masks[word]));
    }
    qtest_writel(s, control, 0);
    qtest_writel(s, data, 0xdeadbeef);
    g_assert_cmphex(qtest_readl(s, address), ==, 0x35);
    g_assert_cmphex(qtest_readl(s, data), ==, 0xdeadbeef);
    g_assert_cmphex(qtest_readl(s, control), ==, 0);
    qtest_writew(s, data, 0);
    qtest_writeb(s, data, 0);
    qtest_writew(s, mask, UINT16_MAX);
    qtest_writeb(s, mask, UINT8_MAX);
    qtest_writew(s, address, 0);
    qtest_writeb(s, address, 0);
    qtest_readw(s, control);
    g_assert_cmphex(qtest_readl(s, data), ==, 0xdeadbeef);
    g_assert_cmphex(qtest_readl(s, mask), ==, 0);
    g_assert_cmphex(qtest_readl(s, address), ==, 0x35);
    g_assert_cmphex(qtest_readl(s, control), ==, 0);
    qtest_writel(s, address, UINT32_MAX);
    g_assert_cmphex(qtest_readl(s, address), ==, 0x800000ff);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, control), ==, 0);
    g_assert_cmphex(qtest_readl(s, address), ==, 0);
    for (unsigned word = 0; word < 4; word++) {
        g_assert_cmphex(qtest_readl(s, data + word * 4), ==, 0);
        g_assert_cmphex(qtest_readl(s, mask + word * 4), ==, 0);
    }
    qtest_writel(s, address, 0x35);
    for (unsigned word = 0; word < 4; word++) {
        g_assert_cmphex(qtest_readl(s, data + word * 4), ==, 0);
    }
    qtest_quit(s);
    g_assert_true(g_file_get_contents(logfile, &contents, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(contents,
                           "ignoring reserved MLB memory control bits "
                           "0xfffffffe"));
    g_assert_nonnull(strstr(contents,
                           "ignoring reserved MLB memory address bits "
                           "0x7fffff00"));
    g_assert_nonnull(strstr(contents,
                           "unsupported write at 0x4842c4e4 size=2"));
    g_assert_nonnull(strstr(contents,
                           "unsupported read at 0x4842c4e0 size=2"));
    unlink(logfile);
}

static void test_mlb_configuration(void)
{
    static const struct {
        uint32_t offset, reset, writable;
    } registers[] = {
        { 0x004, 0, 1 },
        { 0x100, 0x4010, 0x17733 },
        { 0x400, 0, 0x3d03d },
        { 0x408, 0, 0 },
        { 0x40c, 0, 0 },
        { 0x414, 0, 0 },
        { 0x420, 0, 0x20 },
        { 0x424, 0, 0 },
        { 0x42c, 0, 0x3f7f0003 },
        { 0x43c, 0, 0xff00 },
        { 0x480, 0, 0x8000 },
        { 0x488, 0, UINT32_MAX },
        { 0x48c, 0, UINT32_MAX },
        { 0x7c0, 0, 0x17 },
        { 0x7d0, 0, 0 },
        { 0x7d4, 0, 0 },
        { 0x7d8, 0, UINT32_MAX },
        { 0x7dc, 0, UINT32_MAX },
    };
    g_autofree char *logfile = NULL;
    g_autofree char *contents = NULL;
    g_autoptr(GError) error = NULL;
    int fd = g_file_open_tmp("mh2p-rcc-mlb-config-XXXXXX", &logfile, &error);
    g_autofree char *options = NULL;
    QTestState *s;
    const uint32_t base = 0x4842c000;

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    options = g_strdup_printf("-d unimp,guest_errors -D %s", logfile);
    s = start_rcc_options(options);
    qtest_writel(s, 0x4a009358, 2);
    for (unsigned i = 0; i < G_N_ELEMENTS(registers); i++) {
        uint32_t address = base + registers[i].offset;

        g_assert_cmphex(qtest_readl(s, address), ==, registers[i].reset);
        qtest_writel(s, address, UINT32_MAX);
        g_assert_cmphex(qtest_readl(s, address), ==, registers[i].writable);
        qtest_writew(s, address, 0);
        qtest_writeb(s, address, 0);
        g_assert_cmphex(qtest_readl(s, address), ==, registers[i].writable);
        qtest_writel(s, address, 0);
        g_assert_cmphex(qtest_readl(s, address), ==, 0);
    }
    qtest_writel(s, base + 0x7d0, UINT32_MAX);
    qtest_writel(s, base + 0x7d4, UINT32_MAX);
    qtest_writel(s, base + 0x400, 0x20);
    qtest_writel(s, base + 0x400, 0x2c);
    qtest_writel(s, base + 0x400, 0x2d);
    qtest_writel(s, base + 0x488, UINT32_MAX);
    qtest_writel(s, base + 0x48c, UINT32_MAX);
    qtest_writel(s, base + 0x480, 0x8000);
    qtest_writel(s, base + 0x7d8, UINT32_MAX);
    qtest_writel(s, base + 0x7dc, UINT32_MAX);
    qtest_writel(s, base + 0x7c0, 5);
    qtest_writel(s, base + 0x42c, 0x367d0001);
    qtest_writel(s, base + 0x4d0, UINT32_MAX);
    qtest_writel(s, base + 0x4c0, UINT32_MAX);
    qtest_writel(s, base + 0x4e4, 0x80000000);
    g_assert_cmphex(qtest_readl(s, base + 0x4e0), ==, 1);
    qtest_clock_step(s, 1000000000);
    g_assert_cmphex(qtest_readl(s, base + 0x400), ==, 0x2d);
    g_assert_cmphex(qtest_readl(s, base + 0x42c), ==, 0x367d0001);
    g_assert_cmphex(qtest_readl(s, base + 0x7c0), ==, 5);
    g_assert_cmphex(qtest_readl(s, base + 0x40c), ==, 0);
    g_assert_cmphex(qtest_readl(s, base + 0x414), ==, 0);
    g_assert_cmphex(qtest_readl(s, base + 0x7d0), ==, 0);
    g_assert_cmphex(qtest_readl(s, base + 0x7d4), ==, 0);
    qtest_readw(s, base + 0x400);
    qtest_readl(s, base + 0x404);
    qtest_system_reset(s);
    for (unsigned i = 0; i < G_N_ELEMENTS(registers); i++) {
        g_assert_cmphex(qtest_readl(s, base + registers[i].offset), ==,
                        registers[i].reset);
    }
    qtest_quit(s);
    g_assert_true(g_file_get_contents(logfile, &contents, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(contents,
                           "MLB link/DMA transfers are not implemented"));
    g_assert_nonnull(strstr(contents,
                           "ignoring reserved/read-only MLB register "
                           "0x4842c400 bits 0xfffc2fc2"));
    g_assert_nonnull(strstr(contents,
                           "unsupported read at 0x4842c400 size=2"));
    g_assert_nonnull(strstr(contents,
                           "unsupported read at 0x4842c404 size=4"));
    unlink(logfile);
}

static void test_atl_configuration(void)
{
    static const unsigned offsets[] = { 0, 8, 0x10, 0x14, 0x18, 0x1c };
    static const uint32_t masks[] = { 0x81ff, 0x1f, 1, 0xf, 0xf, 1 };
    QTestState *s = start_rcc();
    const uint32_t base = 0x4843c200;
    const uint32_t module = 0x4a008c00;

    qtest_writel(s, base, 1); /* Clock-disabled accesses cannot change state. */
    qtest_writel(s, module, 2);
    g_assert_cmphex(qtest_readl(s, base), ==, 0);
    for (unsigned bank = 0; bank < 4; bank++) {
        for (unsigned reg = 0; reg < ARRAY_SIZE(offsets); reg++) {
            uint32_t address = base + bank * 0x80 + offsets[reg];

            g_assert_cmphex(qtest_readl(s, address), ==, 0);
            qtest_writel(s, address, masks[reg]);
            g_assert_cmphex(qtest_readl(s, address), ==, masks[reg]);
            qtest_writel(s, address, 0x10000);
            qtest_writew(s, address, 0);
            g_assert_cmphex(qtest_readl(s, address), ==, masks[reg]);
        }
    }
    qtest_writel(s, module, 0);
    qtest_writel(s, base, 0);
    qtest_writel(s, module, 2);
    g_assert_cmphex(qtest_readl(s, base), ==, 0x81ff);
    qtest_system_reset(s);
    qtest_writel(s, module, 2);
    for (unsigned bank = 0; bank < 4; bank++) {
        for (unsigned reg = 0; reg < ARRAY_SIZE(offsets); reg++) {
            g_assert_cmphex(qtest_readl(s, base + bank * 0x80 + offsets[reg]),
                            ==, 0);
        }
    }
    qtest_quit(s);
}

static void test_bypass_dpll_modes(void)
{
    const uint32_t bases[] = {
        0x4a005234, 0x4a0052a8, 0x4a0052d8,
        0x4a008180, 0x4a0051a0, 0x4a0051e0,
    };
    QTestState *s = start_rcc_options("-machine experimental-power-clocks=on");

    for (unsigned i = 0; i < ARRAY_SIZE(bases); i++) {
        uint32_t base = bases[i];
        unsigned mode = i == 3 ? 5 : 6;

        g_assert_cmphex(qtest_readl(s, base), ==, 5);
        g_assert_cmphex(qtest_readl(s, base + 4), ==, 0xa);
        g_assert_cmphex(qtest_readl(s, base + 8), ==, 0);
        qtest_writel(s, base, 6);
        g_assert_cmphex(qtest_readl(s, base), ==, mode);
        g_assert_cmphex(qtest_readl(s, base + 4), ==, mode << 1);
        for (unsigned rejected = 0; rejected <= 7; rejected++) {
            if (rejected == 5 || rejected == 6) {
                continue;
            }
            qtest_writel(s, base, rejected);
            g_assert_cmphex(qtest_readl(s, base), ==, mode);
        }
        qtest_writel(s, base, 0x405);
        qtest_writew(s, base, 5);
        qtest_writel(s, base + 4, 0x11);
        qtest_writel(s, base + 8, 0);
        qtest_writel(s, base + 8, 5);
        g_assert_cmphex(qtest_readl(s, base), ==, mode);
        g_assert_cmphex(qtest_readl(s, base + 4), ==, mode << 1);
        g_assert_cmphex(qtest_readl(s, base + 8), ==, 0);
    }
    qtest_system_reset(s);
    for (unsigned i = 0; i < ARRAY_SIZE(bases); i++) {
        g_assert_cmphex(qtest_readl(s, bases[i]), ==, 5);
        g_assert_cmphex(qtest_readl(s, bases[i] + 4), ==, 0xa);
    }
    qtest_quit(s);
}

static void test_disabled_peripheral_clocks(void)
{
    static const struct {
        uint32_t address, reset;
    } registers[] = {
        { 0x4a005550, 0x30000 }, { 0x4a009860, 0x30000 },
        { 0x4a009868, 0x30000 }, { 0x4a009898, 0x30000 },
        { 0x4a009878, 0x30000 }, { 0x4a009904, 0x30000 },
        { 0x4a009908, 0x30000 }, { 0x4a009890, 0x30000 },
        { 0x4a009838, 0x30000 }, { 0x4a009220, 0x70000 },
        { 0x4a008f20, 0x70000 }, { 0x4a008f28, 0x30000 },
        { 0x4a009120, 0x70000 }, { 0x4a009130, 0x70000 },
        { 0x4a009020, 0x70000 }, { 0x4a005764, 0x70000 },
        { 0x4a009388, 0x70000 }, { 0x4a0093b8, 0x70000 },
        { 0x4a0093d0, 0x08070000 },
    };
    QTestState *s = start_rcc_options("-machine experimental-power-clocks=on");

    for (unsigned i = 0; i < ARRAY_SIZE(registers); i++) {
        uint32_t address = registers[i].address;
        uint32_t reset = registers[i].reset;
        uint32_t status = reset & 0x70000;

        g_assert_cmphex(qtest_readl(s, address), ==, reset);
        qtest_writel(s, address, 0);
        g_assert_cmphex(qtest_readl(s, address), ==, status);
        for (unsigned mode = 1; mode < 4; mode++) {
            qtest_writel(s, address, mode);
            g_assert_cmphex(qtest_readl(s, address), ==, status);
        }
        qtest_writel(s, address, 0x100);
        qtest_writew(s, address, 0);
        g_assert_cmphex(qtest_readl(s, address), ==, status);
        qtest_writel(s, address, reset);
        g_assert_cmphex(qtest_readl(s, address), ==, reset);
        qtest_writel(s, address, 0);
    }
    qtest_system_reset(s);
    for (unsigned i = 0; i < ARRAY_SIZE(registers); i++) {
        g_assert_cmphex(qtest_readl(s, registers[i].address), ==,
                       registers[i].reset);
    }
    qtest_quit(s);
}

static void test_power_clocks_default_off(void)
{
    QTestState *s = start_rcc();

    g_assert_cmphex(qtest_readl(s, 0x4a005234), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x4a0093d0), ==, 0);
    qtest_writel(s, 0x4a005234, 5);
    qtest_writel(s, 0x4a0093d0, 0);
    g_assert_cmphex(qtest_readl(s, 0x4a005234), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x4a0093d0), ==, 0);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, 0x4a005234), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x4a0093d0), ==, 0);
    qtest_quit(s);
}

static void test_mpu_dpll_modes(void)
{
    const uint32_t base = 0x4a005160;
    QTestState *s = start_rcc();

    g_assert_cmphex(qtest_readl(s, base), ==, 7);
    g_assert_cmphex(qtest_readl(s, base + 4), ==, 0x11);
    g_assert_cmphex(qtest_readl(s, base + 8), ==, 0);
    for (unsigned mode = 5; mode <= 7; mode++) {
        qtest_writel(s, base, mode);
        g_assert_cmphex(qtest_readl(s, base), ==, mode);
        g_assert_cmphex(qtest_readl(s, base + 4), ==,
                       mode == 7 ? 0x11 : 0x10 | (mode << 1));
        g_assert_cmphex(qtest_readl(s, base + 0xc), ==, (500 << 8) | 9);
        g_assert_cmphex(qtest_readl(s, base + 0x10), ==, 1);
    }
    for (unsigned mode = 0; mode < 5; mode++) {
        qtest_writel(s, base, mode);
        g_assert_cmphex(qtest_readl(s, base), ==, 7);
    }
    qtest_writel(s, base, 0x405); /* Unmodeled low-power feature. */
    qtest_writew(s, base, 5);
    qtest_writel(s, base + 4, 0);
    qtest_writel(s, base + 8, 0);
    qtest_writel(s, base + 8, 5); /* Automatic bypass is not implemented. */
    g_assert_cmphex(qtest_readl(s, base), ==, 7);
    g_assert_cmphex(qtest_readl(s, base + 4), ==, 0x11);
    g_assert_cmphex(qtest_readl(s, base + 8), ==, 0);
    qtest_writel(s, base, 5);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, base), ==, 7);
    g_assert_cmphex(qtest_readl(s, base + 4), ==, 0x11);
    g_assert_cmphex(qtest_readl(s, base + 8), ==, 0);
    qtest_quit(s);
}

static void test_pcie_reference_dpll(void)
{
    static const struct {
        uint32_t select;
        bool locks;
    } vectors[] = {
        { 0x0602ee09, true }, /* 20 MHz * 750 / 10 = 1500 MHz */
        { 0x05227109, true }, /* 20 MHz * 625 / 10 = 1250 MHz */
        { 0x0602ed09, false },
        { 0x0602ef09, false },
        { 0x0502ee09, false }, /* Wrong sigma-delta divider. */
        { 0x0702ee09, false },
        { 0x0622ee09, false }, /* Wrong DCO profile. */
        { 0x05027109, false },
        { 0x06000100, false }, /* Reserved multiplier. */
        { 0x06000000, false },
        { 0x0604b00f, true }, /* Equivalent 1500 MHz profile. */
    };
    const unsigned modes[] = { 7, 1, 1, 1, 1, 5, 5, 7 };
    QTestState *s = start_rcc();
    const uint32_t base = 0x4a008200;

    g_assert_cmphex(qtest_readl(s, base), ==, 5);
    g_assert_cmphex(qtest_readl(s, base + 4), ==, 0xa);
    g_assert_cmphex(qtest_readl(s, base + 8), ==, 0);
    g_assert_cmphex(qtest_readl(s, base + 0xc), ==, 0x04000000);
    g_assert_cmphex(qtest_readl(s, base + 0x10), ==, 1);
    qtest_writel(s, base, 7); /* Unconfigured DPLL must not lock. */
    g_assert_cmphex(qtest_readl(s, base), ==, 5);
    for (unsigned i = 0; i < ARRAY_SIZE(vectors); i++) {
        qtest_system_reset(s);
        qtest_writel(s, base + 0xc, vectors[i].select);
        qtest_writel(s, base + 0x10, 15);
        qtest_writel(s, base, 7);
        g_assert_cmphex(qtest_readl(s, base), ==, vectors[i].locks ? 7 : 5);
        g_assert_cmphex(qtest_readl(s, base + 4), ==,
                        vectors[i].locks ? 0x11 : 0xa);
        g_assert_cmphex(qtest_readl(s, base + 0x10), ==,
                        vectors[i].locks ? 0x60f : 15);
    }
    for (unsigned mode = 0; mode < 8; mode++) {
        qtest_writel(s, base, mode);
        g_assert_cmphex(qtest_readl(s, base), ==, modes[mode]);
    }
    qtest_writel(s, base + 0xc, 0x0602ee09); /* Reprogram only in bypass/stop. */
    qtest_writel(s, base + 0x10, 10);
    g_assert_cmphex(qtest_readl(s, base + 0xc), ==, 0x0604b00f);
    g_assert_cmphex(qtest_readl(s, base + 0x10), ==, 0x60f);
    qtest_writel(s, base + 8, 1);
    qtest_writel(s, base + 4, 0);
    qtest_writew(s, base, 1);
    g_assert_cmphex(qtest_readl(s, base), ==, 7);
    g_assert_cmphex(qtest_readl(s, base + 8), ==, 0);
    qtest_writel(s, base, 5);
    g_assert_cmphex(qtest_readl(s, base + 4), ==, 0x1a);
    for (unsigned divider = 1; divider <= 127; divider++) {
        qtest_writel(s, base + 0x10, divider | 0x600);
        g_assert_cmphex(qtest_readl(s, base + 0x10), ==, divider);
    }
    qtest_writel(s, base + 0x10, 0);
    qtest_writel(s, base + 0x10, 0x80);
    g_assert_cmphex(qtest_readl(s, base + 0x10), ==, 127);
    qtest_writel(s, base + 0xc, 0x0612ee09);
    qtest_writel(s, base + 0xc, 0x0642ee09);
    g_assert_cmphex(qtest_readl(s, base + 0xc), ==, 0x0604b00f);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, base), ==, 5);
    g_assert_cmphex(qtest_readl(s, base + 4), ==, 0xa);
    g_assert_cmphex(qtest_readl(s, base + 0xc), ==, 0x04000000);
    g_assert_cmphex(qtest_readl(s, base + 0x10), ==, 1);
    qtest_quit(s);
}

static void test_pcie_analog_pll(void)
{
    QTestState *s = start_rcc();
    const uint32_t base = 0x4a008200;
    const uint32_t module = 0x4a0093b0;
    const uint32_t acs = 0x4a003c14;

    g_assert_cmphex(qtest_readl(s, base + 0x1c), ==, 0);
    g_assert_cmphex(qtest_readl(s, base + 0x20), ==, 0);
    g_assert_cmphex(qtest_readl(s, acs), ==, 0);
    qtest_writel(s, acs, 0x20000);
    qtest_writel(s, acs, 0x30000);
    qtest_writel(s, acs, 0x10000); /* ACS transmit/output is unsupported. */
    qtest_writel(s, acs, 0x100);
    g_assert_cmphex(qtest_readl(s, acs), ==, 0x20000);
    qtest_writel(s, base + 0x1c, 0x101); /* No internal reference lock. */
    g_assert_cmphex(qtest_readl(s, base + 0x1c), ==, 0);
    qtest_writel(s, base + 0xc, 0x0602ee09);
    qtest_writel(s, base + 0x10, 15);
    qtest_writel(s, base, 7);
    qtest_writel(s, base + 0x1c, 0x101); /* PCIESS module remains disabled. */
    g_assert_cmphex(qtest_readl(s, base + 0x20), ==, 0);
    qtest_writel(s, module, 2);
    qtest_writel(s, base + 0x1c, 0x180);
    qtest_writel(s, base + 0x1c, 0x181); /* External MMX clock is absent. */
    g_assert_cmphex(qtest_readl(s, base + 0x1c), ==, 0x180);
    g_assert_cmphex(qtest_readl(s, base + 0x20), ==, 0);
    qtest_writel(s, base + 0x1c, 0x101);
    g_assert_cmphex(qtest_readl(s, base + 0x1c), ==, 0x101);
    g_assert_cmphex(qtest_readl(s, base + 0x20), ==, 1);
    qtest_writel(s, base + 0x1c, 0x103);
    qtest_writel(s, base + 0x1c, 0x102); /* Auto-idle is unsupported. */
    qtest_writel(s, base + 0x1c, 0x100); /* Mode zero cannot disable a live APLL. */
    qtest_writel(s, base + 0x1c, 0x141);
    qtest_writel(s, base + 0x20, 0);
    qtest_writew(s, base + 0x1c, 0);
    g_assert_cmphex(qtest_readl(s, base + 0x1c), ==, 0x101);
    g_assert_cmphex(qtest_readl(s, base + 0x20), ==, 1);
    qtest_writel(s, module, 0);
    g_assert_cmphex(qtest_readl(s, base + 0x20), ==, 0);
    qtest_writel(s, module, 2);
    g_assert_cmphex(qtest_readl(s, base + 0x20), ==, 1);
    qtest_writel(s, base, 5);
    g_assert_cmphex(qtest_readl(s, base + 0x20), ==, 0);
    qtest_writel(s, base + 0x10, 10);
    qtest_writel(s, base, 7); /* 150 MHz is not a valid 100 MHz APLL input. */
    qtest_writel(s, base + 0x1c, 0x100);
    qtest_writel(s, base + 0x1c, 0x101);
    g_assert_cmphex(qtest_readl(s, base + 0x1c), ==, 0x100);
    g_assert_cmphex(qtest_readl(s, base + 0x20), ==, 0);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, base + 0x1c), ==, 0);
    g_assert_cmphex(qtest_readl(s, base + 0x20), ==, 0);
    g_assert_cmphex(qtest_readl(s, acs), ==, 0);
    qtest_quit(s);
}

static void test_pcie_output_clocks(void)
{
    QTestState *s = start_rcc();
    const uint32_t base = 0x4a008200;
    const uint32_t module = 0x4a0093b0;

    g_assert_cmphex(qtest_readl(s, base + 0x28), ==, 0);
    qtest_writel(s, base + 0xc, 0x0602ee09);
    qtest_writel(s, base + 0x10, 15);
    qtest_writel(s, base, 7);
    qtest_writel(s, module, 2);
    qtest_writel(s, base + 0x1c, 0x101);
    g_assert_cmphex(qtest_readl(s, base + 0x20), ==, 1);
    for (unsigned clocks = 0; clocks < 8; clocks++) {
        uint32_t control = (clocks << 8) | 2;

        qtest_writel(s, module, control);
        g_assert_cmphex(qtest_readl(s, base + 0x28), ==, control & 0x600);
        g_assert_cmphex(qtest_readl(s, base + 0x20), ==, 1);
        qtest_writel(s, base + 0x28, 0x600);
        g_assert_cmphex(qtest_readl(s, base + 0x28), ==, control & 0x600);
        qtest_writel(s, module, control & ~3U);
        g_assert_cmphex(qtest_readl(s, base + 0x28), ==, 0);
        g_assert_cmphex(qtest_readl(s, base + 0x20), ==, 0);
    }
    qtest_writel(s, module, 0x702);
    g_assert_cmphex(qtest_readl(s, base + 0x28), ==, 0x600);
    qtest_writel(s, base, 5);
    g_assert_cmphex(qtest_readl(s, base + 0x28), ==, 0);
    qtest_writel(s, base, 7);
    g_assert_cmphex(qtest_readl(s, base + 0x28), ==, 0x600);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, base + 0x28), ==, 0);
    qtest_quit(s);
}

static void test_pcie_phy_configuration(void)
{
    static const struct {
        uint32_t address, reset, programmed, invalid;
    } registers[] = {
        { 0x4a09400c, 0, 0x08028000, 0x08028001 },
        { 0x4a09401c, 0, 0x80000000, 0x80000001 },
        { 0x4a094024, 0, 0xc0000000, 0xc0000001 },
        { 0x4a094028, 0x02e33000, 0x02e33000, 0x02e33001 },
        { 0x4a094038, 0, 0x0000f880, 0x0000f881 },
    };
    QTestState *s = start_rcc();
    const uint32_t clock = 0x4a0093e8;
    const uint32_t power = 0x4a003c40;
    const uint32_t pcs = 0x4a003c34;

    g_assert_cmphex(qtest_readl(s, power), ==, 0);
    g_assert_cmphex(qtest_readl(s, pcs), ==, 0);
    for (unsigned command = 0; command < 4; command++) {
        uint32_t value = (20 << 22) | (command << 14);

        qtest_writel(s, power, value);
        g_assert_cmphex(qtest_readl(s, power), ==, value);
    }
    qtest_writel(s, power, 0x05010000); /* Reserved power command. */
    qtest_writel(s, power, 0x0500c001);
    qtest_writel(s, power, 0x04c0c000); /* Wrong SYS_CLKIN1 frequency. */
    qtest_writew(s, power, 0);
    g_assert_cmphex(qtest_readl(s, power), ==, 0x0500c000);
    qtest_writel(s, pcs, 0x00960000);
    qtest_writel(s, pcs, 0x01960000);
    qtest_writel(s, pcs, 0x00960001);
    qtest_writeb(s, pcs, 0);
    g_assert_cmphex(qtest_readl(s, pcs), ==, 0x00960000);
    for (unsigned i = 0; i < ARRAY_SIZE(registers); i++) {
        qtest_writel(s, registers[i].address, registers[i].programmed);
    }
    qtest_writel(s, clock, 1);
    for (unsigned i = 0; i < ARRAY_SIZE(registers); i++) {
        g_assert_cmphex(qtest_readl(s, registers[i].address), ==,
                        registers[i].reset);
        qtest_writel(s, registers[i].address, registers[i].programmed);
        qtest_writel(s, registers[i].address, registers[i].invalid);
        qtest_writew(s, registers[i].address, 0);
        g_assert_cmphex(qtest_readl(s, registers[i].address), ==,
                        registers[i].programmed);
    }
    qtest_writel(s, 0x4a094028, 0x00e33000); /* Preserve reserved reset bits. */
    g_assert_cmphex(qtest_readl(s, 0x4a094028), ==, 0x02e33000);
    qtest_writel(s, 0x4a094028, 0x02e33800);
    g_assert_cmphex(qtest_readl(s, 0x4a094028), ==, 0x02e33800);
    qtest_writel(s, 0x4a094028, 0x02e33000);
    qtest_writel(s, clock, 0);
    for (unsigned i = 0; i < ARRAY_SIZE(registers); i++) {
        qtest_writel(s, registers[i].address, registers[i].reset);
        g_assert_cmphex(qtest_readl(s, registers[i].address), ==, 0);
    }
    qtest_writel(s, clock, 1);
    qtest_writel(s, 0x4a090010, 0x13); /* Bridge reset is not PHY reset. */
    for (unsigned i = 0; i < ARRAY_SIZE(registers); i++) {
        g_assert_cmphex(qtest_readl(s, registers[i].address), ==,
                        registers[i].programmed);
    }
    qtest_writel(s, power, 0); /* Configuration is not a powered-link status. */
    g_assert_cmphex(qtest_readl(s, power), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x4a09400c), ==, 0x08028000);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, clock), ==, 0x30000);
    g_assert_cmphex(qtest_readl(s, power), ==, 0);
    g_assert_cmphex(qtest_readl(s, pcs), ==, 0);
    qtest_writel(s, clock, 1);
    for (unsigned i = 0; i < ARRAY_SIZE(registers); i++) {
        g_assert_cmphex(qtest_readl(s, registers[i].address), ==,
                        registers[i].reset);
    }
    qtest_quit(s);
}

static void test_pcie_local_reset(void)
{
    QTestState *s = start_rcc();
    const uint32_t control = 0x4ae07310;
    const uint32_t status = 0x4ae07314;
    const uint32_t clock = 0x4a0093b0;

    g_assert_cmphex(qtest_readl(s, control), ==, 3);
    g_assert_cmphex(qtest_readl(s, status), ==, 0);
    qtest_writel(s, status, 1); /* W1C must not create completion. */
    qtest_writel(s, control, 2); /* No module clock to release reset. */
    g_assert_cmphex(qtest_readl(s, control), ==, 3);
    g_assert_cmphex(qtest_readl(s, status), ==, 0);
    qtest_writel(s, clock, 2);
    qtest_writel(s, control, 2);
    g_assert_cmphex(qtest_readl(s, control), ==, 2);
    g_assert_cmphex(qtest_readl(s, status), ==, 1);
    qtest_writel(s, status, 0);
    g_assert_cmphex(qtest_readl(s, status), ==, 1);
    qtest_writel(s, status, 1);
    g_assert_cmphex(qtest_readl(s, status), ==, 0);
    qtest_writel(s, control, 2); /* Repeated release is not another reset. */
    g_assert_cmphex(qtest_readl(s, status), ==, 0);
    qtest_writel(s, control, 3);
    g_assert_cmphex(qtest_readl(s, control), ==, 3);
    g_assert_cmphex(qtest_readl(s, status), ==, 0);
    qtest_writel(s, control, 2);
    qtest_writel(s, control, 3);
    g_assert_cmphex(qtest_readl(s, status), ==, 1); /* Latched across assertion. */
    qtest_writel(s, status, 3); /* PCIE2 is not implemented. */
    qtest_writel(s, status, 4);
    qtest_writew(s, status, 1);
    g_assert_cmphex(qtest_readl(s, status), ==, 1);
    qtest_writel(s, control, 0);
    qtest_writel(s, control, 1);
    qtest_writel(s, control, 7);
    qtest_writeb(s, control, 2);
    g_assert_cmphex(qtest_readl(s, control), ==, 3);
    qtest_writel(s, status, 1);
    qtest_writel(s, clock, 0);
    qtest_writel(s, control, 2);
    g_assert_cmphex(qtest_readl(s, control), ==, 3);
    g_assert_cmphex(qtest_readl(s, status), ==, 0);
    qtest_writel(s, clock, 2);
    qtest_writel(s, control, 2);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, control), ==, 3);
    g_assert_cmphex(qtest_readl(s, status), ==, 0);
    qtest_quit(s);
}

static void test_pcie_endpoint_configuration(void)
{
    QTestState *s = start_rcc();
    const uint32_t base = 0x51000000;

    qtest_writel(s, base + 0x2100, 0);
    qtest_writel(s, 0x4a0093b0, 2);
    g_assert_cmphex(qtest_readl(s, base + 0x2100), ==, 4);
    qtest_writel(s, base + 0x2100, 0);
    qtest_writel(s, base, 0xb801104c); /* Core is still held in local reset. */
    qtest_writel(s, 0x4ae07310, 2);
    g_assert_cmphex(qtest_readl(s, base), ==, 0x8888104c);
    g_assert_cmphex(qtest_readl(s, base + 4), ==, 0x00100000);
    g_assert_cmphex(qtest_readl(s, base + 8), ==, 1);
    g_assert_cmphex(qtest_readl(s, base + 0x7c), ==, 0x00433c22);
    qtest_writel(s, base + 0x7c, 0x00433c21);
    qtest_writel(s, base + 0x7c, 0x00433c11);
    g_assert_cmphex(qtest_readl(s, base + 0x7c), ==, 0x00433c11);
    qtest_writel(s, base + 0x7c, 0x00433c12);
    qtest_writel(s, base + 0x7c, 0x00433c14);
    qtest_writel(s, base + 0x7c, 0x00433c42);
    qtest_writel(s, base + 0x7c, 0x01433c12);
    qtest_writew(s, base + 0x7c, 0);
    qtest_writel(s, base + 0x7d, 0);
    g_assert_cmphex(qtest_readl(s, base + 0x7c), ==, 0x00433c12);
    qtest_writel(s, base, 0xb801104c);
    qtest_writel(s, base + 4, 0x00100002);
    qtest_writel(s, base + 4, 0x00100006); /* Bus mastering not implemented. */
    qtest_writel(s, base + 8, 0x07800000);
    qtest_writel(s, base + 0x2100, 1); /* Legacy endpoint not implemented. */
    g_assert_cmphex(qtest_readl(s, base + 0x2100), ==, 0);
    for (unsigned i = 0; i < 6; i++) {
        uint32_t mask = i < 3 ? (1U << (12 + i)) - 1 : 0;

        qtest_writel(s, base + 0x1010 + i * 4, mask);
        qtest_writel(s, base + 0x1010 + i * 4, 0x123);
        qtest_writel(s, base + 0x10 + i * 4, 0);
        qtest_writel(s, base + 0x10 + i * 4, 0x1000);
        g_assert_cmphex(qtest_readl(s, base + 0x1010 + i * 4), ==, mask);
        g_assert_cmphex(qtest_readl(s, base + 0x10 + i * 4), ==, 0);
    }
    for (unsigned i = 0; i < 4; i++) {
        qtest_writel(s, base + 0x900, 0x80000000U | i);
        qtest_writel(s, base + 0x904, 0);
        qtest_writel(s, base + 0x918, 0x80010000 + i * 0x1000);
        qtest_writel(s, base + 0x908, 0xc0000000U | (i << 8));
    }
    for (unsigned i = 0; i < 4; i++) {
        qtest_writel(s, base + 0x900, 0x80000000U | i);
        qtest_writel(s, base + 0x904, 1);
        qtest_writel(s, base + 0x918, 0x80010001);
        qtest_writel(s, base + 0x91c, 1);
        qtest_writel(s, base + 0x908, 0xc0000600);
        qtest_writel(s, base + 0x908, 0x80000000);
        g_assert_cmphex(qtest_readl(s, base + 0x904), ==, 0);
        g_assert_cmphex(qtest_readl(s, base + 0x908), ==,
                        0xc0000000U | (i << 8));
        g_assert_cmphex(qtest_readl(s, base + 0x918), ==,
                        0x80010000 + i * 0x1000);
        g_assert_cmphex(qtest_readl(s, base + 0x91c), ==, 0);
    }
    qtest_writel(s, base + 0x900, 0x80000004);
    g_assert_cmphex(qtest_readl(s, base + 0x900), ==, 0x80000003);
    qtest_writel(s, base + 0x900, 0);
    g_assert_cmphex(qtest_readl(s, base + 0x900), ==, 0);
    qtest_writel(s, base + 0x90c, 0x88000000);
    qtest_writel(s, base + 0x914, 0x8fffffff);
    qtest_writel(s, base + 0x918, 0x80000000);
    qtest_writel(s, base + 0x908, 0x80000000);
    g_assert_cmphex(qtest_readl(s, base + 0x90c), ==, 0x88000000);
    g_assert_cmphex(qtest_readl(s, base + 0x914), ==, 0x8fffffff);
    g_assert_cmphex(qtest_readl(s, base + 0x918), ==, 0x80000000);
    g_assert_cmphex(qtest_readl(s, base + 0x908), ==, 0x80000000);
    qtest_writel(s, base + 0x900, 0x80000003);
    qtest_writel(s, 0x4a0093b0, 0);
    qtest_writel(s, base, 0);
    qtest_writel(s, base + 0x918, 0);
    g_assert_cmphex(qtest_readl(s, base), ==, 0);
    qtest_writel(s, 0x4a0093b0, 2);
    g_assert_cmphex(qtest_readl(s, base), ==, 0xb801104c);
    g_assert_cmphex(qtest_readl(s, base + 4), ==, 0x00100002);
    g_assert_cmphex(qtest_readl(s, base + 8), ==, 0x07800000);
    g_assert_cmphex(qtest_readl(s, base + 0x918), ==, 0x80013000);
    qtest_writel(s, 0x4ae07310, 3);
    qtest_writel(s, 0x4ae07310, 2);
    g_assert_cmphex(qtest_readl(s, base + 0x2100), ==, 4);
    qtest_writel(s, base + 0x2100, 0);
    g_assert_cmphex(qtest_readl(s, base), ==, 0x8888104c);
    g_assert_cmphex(qtest_readl(s, base + 0x900), ==, 0);
    qtest_writel(s, base + 0x2100, 0);
    qtest_writel(s, base, 0xb801104c);
    qtest_system_reset(s);
    qtest_writel(s, 0x4a0093b0, 2);
    qtest_writel(s, 0x4ae07310, 2);
    g_assert_cmphex(qtest_readl(s, base + 0x2100), ==, 4);
    qtest_writel(s, base + 0x2100, 0);
    g_assert_cmphex(qtest_readl(s, base), ==, 0x8888104c);
    qtest_quit(s);
}

static void test_pcie_no_link_partner(void)
{
    QTestState *s = start_rcc();
    const uint32_t base = 0x51000000;

    qtest_writel(s, 0x4a0093b0, 2);
    qtest_writel(s, 0x4ae07310, 2);
    qtest_writel(s, base + 0x2100, 0);
    qtest_writel(s, base + 0x2104, 1);
    g_assert_cmphex(qtest_readl(s, base + 0x2104), ==, 0);
    qtest_writel(s, 0x4a00820c, 0x0602ee09);
    qtest_writel(s, 0x4a008210, 15);
    qtest_writel(s, 0x4a008200, 7);
    qtest_writel(s, 0x4a00821c, 0x101);
    qtest_writel(s, base + 0x2104, 1);
    g_assert_cmphex(qtest_readl(s, base + 0x2104), ==, 0);
    qtest_writel(s, 0x4a003c40, 0x0500c000);
    qtest_writel(s, base + 0x2104, 1);
    g_assert_cmphex(qtest_readl(s, base + 0x2104), ==, 0);
    qtest_writel(s, 0x4a0093b0, 0x702);
    qtest_writel(s, base + 0x2104, 1);
    qtest_writel(s, base + 0x2104, 3);
    qtest_writel(s, base, 0xb801104c); /* No live-core reconfiguration. */
    g_assert_cmphex(qtest_readl(s, base), ==, 0x8888104c);
    g_assert_cmphex(qtest_readl(s, base + 0x2104), ==, 1);
    qtest_clock_step(s, 1000000000);
    g_assert_cmphex(qtest_readl(s, base + 0x210c), ==, 0);
    qtest_writel(s, base + 0x2104, 0);
    qtest_writel(s, base, 0xb801104c);
    g_assert_cmphex(qtest_readl(s, base), ==, 0xb801104c);
    qtest_writel(s, base + 0x2104, 1);
    qtest_writel(s, 0x4ae07310, 3);
    g_assert_cmphex(qtest_readl(s, base + 0x2104), ==, 0);
    g_assert_cmphex(qtest_readl(s, base + 0x210c), ==, 0);
    qtest_quit(s);
}

static void test_ocp2scp3_configuration(void)
{
    QTestState *s = start_rcc();
    const uint32_t bridge = 0x4a090000;

    g_assert_cmphex(qtest_readl(s, bridge + 0x10), ==, 0x11);
    g_assert_cmphex(qtest_readl(s, bridge + 0x14), ==, 1);
    g_assert_cmphex(qtest_readl(s, bridge + 0x18), ==, 1);
    qtest_writel(s, 0x4a080018, 6);
    qtest_writel(s, bridge + 0x10, 0x19);
    qtest_writel(s, bridge + 0x10, 0x1d);
    qtest_writel(s, bridge + 0x18, 0x8f);
    qtest_writel(s, bridge + 0x18, 0x400);
    qtest_writel(s, bridge + 0x14, 0);
    g_assert_cmphex(qtest_readl(s, bridge + 0x10), ==, 0x19);
    g_assert_cmphex(qtest_readl(s, bridge + 0x14), ==, 1);
    g_assert_cmphex(qtest_readl(s, bridge + 0x18), ==, 0x8f);
    qtest_writel(s, bridge + 0x10, 0x13);
    g_assert_cmphex(qtest_readl(s, bridge + 0x10), ==, 0x11);
    g_assert_cmphex(qtest_readl(s, bridge + 0x18), ==, 1);
    g_assert_cmphex(qtest_readl(s, 0x4a080018), ==, 6);
    qtest_writel(s, bridge + 0x10, 0x19);
    qtest_writel(s, bridge + 0x18, 0x8f);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, bridge + 0x10), ==, 0x11);
    g_assert_cmphex(qtest_readl(s, bridge + 0x18), ==, 1);
    g_assert_cmphex(qtest_readl(s, 0x4a080018), ==, 1);
    qtest_quit(s);
}

static void test_pcie_clock_controls(void)
{
    QTestState *s = start_rcc();
    const uint32_t bridges[] = { 0x4a0093e0, 0x4a0093e8 };
    const uint32_t pcie = 0x4a0093b0;

    g_assert_cmphex(qtest_readl(s, 0x4a0093a0), ==, 3);
    qtest_writel(s, 0x4a0093a0, 0);
    g_assert_cmphex(qtest_readl(s, 0x4a0093a0), ==, 0);
    qtest_writel(s, 0x4a0093a0, 2);
    qtest_writel(s, 0x4a0093a0, 1);
    qtest_writel(s, 0x4a0093a0, 0x102);
    qtest_writew(s, 0x4a0093a0, 0);
    g_assert_cmphex(qtest_readl(s, 0x4a0093a0), ==, 2);
    for (unsigned i = 0; i < ARRAY_SIZE(bridges); i++) {
        g_assert_cmphex(qtest_readl(s, bridges[i]), ==, 0x30000);
        qtest_writel(s, bridges[i], qtest_readl(s, bridges[i]) | 1);
        g_assert_cmphex(qtest_readl(s, bridges[i]), ==, 1);
        qtest_writel(s, bridges[i], 2);
        qtest_writel(s, bridges[i], 3);
        qtest_writel(s, bridges[i], 0x101);
        qtest_writew(s, bridges[i], 0);
        g_assert_cmphex(qtest_readl(s, bridges[i]), ==, 1);
        qtest_writel(s, bridges[i], 0);
        g_assert_cmphex(qtest_readl(s, bridges[i]), ==, 0x30000);
        qtest_writel(s, bridges[i], 1);
    }
    g_assert_cmphex(qtest_readl(s, pcie), ==, 0x30000);
    qtest_writel(s, pcie, qtest_readl(s, pcie) | 2);
    g_assert_cmphex(qtest_readl(s, pcie), ==, 2);
    for (unsigned i = 0; i < 8; i++) {
        qtest_writel(s, pcie, (i << 8) | 2);
        g_assert_cmphex(qtest_readl(s, pcie), ==, (i << 8) | 2);
    }
    qtest_writel(s, pcie, 0x701);
    qtest_writel(s, pcie, 0x703);
    qtest_writel(s, pcie, 0xf02);
    qtest_writeb(s, pcie, 0);
    g_assert_cmphex(qtest_readl(s, pcie), ==, 0x702);
    qtest_writel(s, pcie, 0x700);
    g_assert_cmphex(qtest_readl(s, pcie), ==, 0x30700);
    qtest_writel(s, pcie, qtest_readl(s, pcie) | 2);
    g_assert_cmphex(qtest_readl(s, pcie), ==, 0x702);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, 0x4a0093a0), ==, 3);
    g_assert_cmphex(qtest_readl(s, pcie), ==, 0x30000);
    for (unsigned i = 0; i < ARRAY_SIZE(bridges); i++) {
        g_assert_cmphex(qtest_readl(s, bridges[i]), ==, 0x30000);
    }
    qtest_quit(s);
}

static void test_timer2_handoff(void)
{
    QTestState *s = start_rcc();
    const uint32_t counter = 0x4803203c;
    uint32_t initial = qtest_readl(s, counter);

    g_assert_cmphex(qtest_readl(s, counter), ==, initial);
    qtest_clock_step(s, 49);
    g_assert_cmphex(qtest_readl(s, counter), ==, initial);
    qtest_clock_step(s, 1);
    g_assert_cmphex(qtest_readl(s, counter), ==, initial + 1);
    qtest_clock_step(s, 1000000000);
    g_assert_cmphex(qtest_readl(s, counter), ==, initial + 20000001);
    qtest_writel(s, counter, 1234); /* Guest reprogramming is not supported. */
    g_assert_cmphex(qtest_readl(s, counter), ==, initial + 20000001);
    qtest_clock_step(s, (1ULL << 32) * 50);
    g_assert_cmphex(qtest_readl(s, counter), ==, initial + 20000001);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, counter), ==, 0);
    qtest_clock_step(s, 50);
    g_assert_cmphex(qtest_readl(s, counter), ==, 1);
    qtest_quit(s);
}

static void test_ocmc_ram(void)
{
    QTestState *s = start_rcc();
    static const uint32_t addresses[] = { 0x40300000, 0x4037ffa0, 0x4037fffc };

    for (unsigned i = 0; i < ARRAY_SIZE(addresses); i++) {
        g_assert_cmphex(qtest_readl(s, addresses[i]), ==, 0);
        qtest_writel(s, addresses[i], 0x12345678 + i);
    }
    qtest_system_reset(s);
    for (unsigned i = 0; i < ARRAY_SIZE(addresses); i++) {
        g_assert_cmphex(qtest_readl(s, addresses[i]), ==, 0x12345678 + i);
    }
    qtest_quit(s);
}

static void nor_unlock(QTestState *s, uint16_t command)
{
    qtest_writew(s, 0x08000aaa, 0xaa);
    qtest_writew(s, 0x08000554, 0x55);
    qtest_writew(s, 0x08000aaa, command);
}

static void test_nor_fixture(void)
{
    QTestState *s = start_rcc();
    const uint32_t base = 0x08000000;
    const uint32_t target = base + 0x20000;

    g_assert_cmphex(qtest_readl(s, base), ==, UINT32_MAX);
    g_assert_cmphex(qtest_readw(s, base + 64 * 1024 * 1024 - 2), ==, 0xffff);
    nor_unlock(s, 0x90);
    g_assert_cmphex(qtest_readw(s, base), ==, 0x0001);
    g_assert_cmphex(qtest_readw(s, base + 2), ==, 0x227e);
    qtest_writew(s, base, 0xf0);
    qtest_writew(s, base + 0xaa, 0x98);
    g_assert_cmphex(qtest_readw(s, base + 0x20), ==, 'Q');
    g_assert_cmphex(qtest_readw(s, base + 0x22), ==, 'R');
    g_assert_cmphex(qtest_readw(s, base + 0x24), ==, 'Y');
    g_assert_cmphex(qtest_readw(s, base + 0x4e), ==, 26);
    qtest_writew(s, base, 0xf0);
    nor_unlock(s, 0xa0);
    qtest_writew(s, target, 0x1234);
    g_assert_cmphex(qtest_readw(s, target), ==, 0x1234);
    nor_unlock(s, 0xa0);
    qtest_writew(s, target, 0xff00);
    g_assert_cmphex(qtest_readw(s, target), ==, 0x1200);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readw(s, target), ==, 0x1200);
    nor_unlock(s, 0x80);
    qtest_writew(s, base + 0xaaa, 0xaa);
    qtest_writew(s, base + 0x554, 0x55);
    qtest_writew(s, target, 0x30);
    qtest_clock_step(s, 50000);
    qtest_clock_step(s, 1000000);
    g_assert_cmphex(qtest_readw(s, target), ==, 0xffff);
    qtest_quit(s);
}

static void test_gpmc_nor_mapping(void)
{
    static const uint32_t original[] = {
        0x41001010, 0x000a1300, 0, 0x08021303,
        0x05110c15, 0x2c2, 0xc48,
    };
    QTestState *s = start_rcc();
    uint32_t target = 0x08020000;

    g_assert_cmphex(qtest_readl(s, 0x50000078), ==, 0xc48);
    nor_unlock(s, 0xa0);
    qtest_writew(s, target, 0x1234);
    qtest_writel(s, 0x5000001c, 0x303);
    g_assert_cmphex(qtest_readl(s, 0x5000001c), ==, 0x303);
    g_assert_cmphex(qtest_readl(s, 0x50000018), ==, 0);
    qtest_writel(s, 0x50000018, 0x303);
    g_assert_cmphex(qtest_readl(s, 0x50000018), ==, 0);
    qtest_writel(s, 0x50000078, 0);
    g_assert_cmphex(qtest_readw(s, target), !=, 0x1234);
    for (unsigned i = 0; i < ARRAY_SIZE(original); i++) {
        qtest_writel(s, 0x50000060 + i * 4, original[i]);
        g_assert_cmphex(qtest_readl(s, 0x50000060 + i * 4), ==,
                        original[i] | (i == 5 ? 0x80000000 : 0));
    }
    g_assert_cmphex(qtest_readw(s, target), ==, 0x1234);
    qtest_writel(s, 0x50000074, 0x1f0f0fcf);
    g_assert_cmphex(qtest_readl(s, 0x50000074), ==, 0x9f0f0fcf);
    qtest_writel(s, 0x50000074, 0x00100000); /* Reserved timing field. */
    g_assert_cmphex(qtest_readl(s, 0x50000074), ==, 0x9f0f0fcf);
    qtest_writel(s, 0x50000074, original[5]);
    qtest_writel(s, 0x50000078, 0xe50);
    g_assert_cmphex(qtest_readw(s, 0x10020000), ==, 0x1234);
    g_assert_cmphex(qtest_readw(s, 0x12020000), !=, 0x1234);
    qtest_writel(s, 0x50000078, 0xc51); /* Misaligned 64-MiB window. */
    g_assert_cmphex(qtest_readl(s, 0x50000078), ==, 0xe50);
    qtest_writel(s, 0x50000078, 0x850); /* Unsupported 128-MiB window. */
    g_assert_cmphex(qtest_readl(s, 0x50000078), ==, 0xe50);
    qtest_writel(s, 0x50000060, 0x10001410); /* No NAND fixture. */
    g_assert_cmphex(qtest_readl(s, 0x50000060), ==, original[0]);
    qtest_writeb(s, 0x50000078, 0);
    g_assert_cmphex(qtest_readl(s, 0x50000078), ==, 0xe50);
    qtest_writel(s, 0x50000010, 2);
    g_assert_cmphex(qtest_readl(s, 0x50000010), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x50000014), ==, 1);
    g_assert_cmphex(qtest_readl(s, 0x5000001c), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x50000078), ==, 0xf40);
    g_assert_cmphex(qtest_readw(s, 0x20000), ==, 0x1234);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, 0x50000078), ==, 0xc48);
    g_assert_cmphex(qtest_readw(s, target), ==, 0x1234);
    qtest_quit(s);
}

static void test_gpio8_pad_controls(void)
{
    const unsigned pad_count = 24;
    QTestState *s = start_rcc();

    for (unsigned i = 0; i < pad_count; i++) {
        uint32_t pad = 0x4a0035dc + 4 * i;

        g_assert_cmphex(qtest_readl(s, pad), ==, 0x0004000f);
        for (unsigned mode = 0; mode < 16; mode++) {
            qtest_writel(s, pad, 0x000f01f0 | mode);
            g_assert_cmphex(qtest_readl(s, pad), ==, 0x000f01f0 | mode);
        }
        qtest_writel(s, pad, 0x0104000e);
        qtest_writew(s, pad, 0);
        g_assert_cmphex(qtest_readl(s, pad), ==, 0x0104000e);
        qtest_writel(s, pad, 0x0005000e);
        g_assert_cmphex(qtest_readl(s, pad), ==, 0x0005000e);
    }
    qtest_system_reset(s);
    for (unsigned i = 0; i < pad_count; i++) {
        g_assert_cmphex(qtest_readl(s, 0x4a0035dc + 4 * i), ==, 0x0004000f);
    }
    qtest_quit(s);
}

static void test_pad_controls(void)
{
    static const struct {
        uint32_t address, reset, value;
        unsigned reserved_mode;
    } ipl_pads[] = {
        { 0x4a003404, 0x0005000f, 0x000501b0, 1 },
        { 0x4a0034ac, 0x0004000f, 0x000401b0, 3 },
        { 0x4a0034d8, 0x000e000f, 0x0004000e, 1 },
        { 0x4a003554, 0x0004000f, 0x00040004, 1 },
        { 0x4a0035d8, 0x0004000f, 0x00040003, 2 },
        { 0x4a00363c, 0x000e000f, 0x00040005, 2 },
        { 0x4a0036a0, 0x0004000f, 0x0004000e, 11 },
        { 0x4a003750, 0x0004000f, 0x00040002, 7 },
        { 0x4a003754, 0x0006000f, 0x0005000e, 1 },
        { 0x4a0037bc, 0x000e000f, 0x00040006, 7 },
        { 0x4a0037fc, 0x0006000f, 0x00040005, 6 },
        { 0x4a00380c, 0x0006000f, 0x00040001, 2 },
        { 0x4a003824, 0x0001000f, 0x00010001, 2 },
    };
    QTestState *s = start_rcc();
    const uint32_t pad = 0x4a0037f8;
    const unsigned modes[] = { 0, 2, 3, 4, 5, 14, 15 };

    for (unsigned i = 0; i < ARRAY_SIZE(ipl_pads); i++) {
        uint32_t address = ipl_pads[i].address;

        g_assert_cmphex(qtest_readl(s, address), ==, ipl_pads[i].reset);
        qtest_writel(s, address, ipl_pads[i].value);
        g_assert_cmphex(qtest_readl(s, address), ==, ipl_pads[i].value);
        qtest_writel(s, address, ipl_pads[i].reserved_mode);
        qtest_writew(s, address, 0xf);
        g_assert_cmphex(qtest_readl(s, address), ==, ipl_pads[i].reserved_mode);
    }
    qtest_writel(s, 0x4a003754, 0x000d000e);
    qtest_writel(s, 0x4a003800, 0x00050100);
    qtest_writel(s, 0x4a00380c, 0x000c0001);
    qtest_writel(s, 0x4a003824, 0x00050001);
    g_assert_cmphex(qtest_readl(s, 0x4a003754), ==, 0x0005000e);
    g_assert_cmphex(qtest_readl(s, 0x4a003800), ==, 0x00050000);
    g_assert_cmphex(qtest_readl(s, 0x4a00380c), ==, 0x00040001);
    g_assert_cmphex(qtest_readl(s, 0x4a003824), ==, 0x00010001);
    qtest_writel(s, 0x4a003400, 0x61862b32); /* Original IPL's final record. */
    g_assert_cmphex(qtest_readl(s, 0x4a003400), ==, 0x01060132);
    qtest_writel(s, 0x4a003400, 0x0200000f); /* No external wakeup event. */
    g_assert_cmphex(qtest_readl(s, 0x4a003400), ==, 0x0000000f);
    g_assert_cmphex(qtest_readl(s, pad), ==, 0x0006000f);
    for (unsigned i = 0; i < ARRAY_SIZE(modes); i++) {
        qtest_writel(s, pad, 0x000f01f0 | modes[i]);
        g_assert_cmphex(qtest_readl(s, pad), ==, 0x000f01f0 | modes[i]);
    }
    qtest_writel(s, pad, 0x00040002); /* uart3_rxd, receiver enabled */
    qtest_writel(s, pad, 0x01040002); /* Retain wakeup-enable configuration. */
    g_assert_cmphex(qtest_readl(s, pad), ==, 0x01040002);
    qtest_writel(s, pad, 0x00040001); /* Reserved mux selection. */
    g_assert_cmphex(qtest_readl(s, pad), ==, 0x00040001);
    qtest_writel(s, pad, 0x81040002); /* Read-only reserved bit is ignored. */
    qtest_writew(s, pad, 0);
    g_assert_cmphex(qtest_readl(s, pad), ==, 0x01040002);
    g_assert_cmphex(qtest_readl(s, 0x4a00361c), ==, 0x0004000f);
    qtest_writel(s, 0x4a00361c, 0x000b01fe);
    g_assert_cmphex(qtest_readl(s, 0x4a00361c), ==, 0x000b01fe);
    qtest_writel(s, 0x4a00361c, 0x00040005); /* Reserved on this pad. */
    g_assert_cmphex(qtest_readl(s, 0x4a00361c), ==, 0x00040005);
    g_assert_cmphex(qtest_readl(s, pad), ==, 0x01040002);
    for (unsigned i = 0; i < 4; i++) {
        uint32_t spi_pad = 0x4a003560 + 4 * i;

        g_assert_cmphex(qtest_readl(s, spi_pad), ==, 0x0004000f);
        qtest_writel(s, spi_pad, 0x00040008);
        g_assert_cmphex(qtest_readl(s, spi_pad), ==, 0x00040008);
        qtest_writel(s, spi_pad, 0x00040001);
        qtest_writel(s, spi_pad, 0x01040008);
        g_assert_cmphex(qtest_readl(s, spi_pad), ==, 0x01040008);
        qtest_writel(s, spi_pad, 0x00040003);
        g_assert_cmphex(qtest_readl(s, spi_pad), ==, 0x00040003);
    }
    g_assert_cmphex(qtest_readl(s, 0x4a003610), ==, 0x0004000f);
    qtest_writel(s, 0x4a003610, 0x0004000e);
    qtest_writel(s, 0x4a003610, 0x00040001);
    qtest_writel(s, 0x4a003610, 0x00040008);
    g_assert_cmphex(qtest_readl(s, 0x4a003610), ==, 0x00040008);
    for (unsigned i = 0; i < 2; i++) {
        uint32_t can_pad = 0x4a003688 + 4 * i;

        g_assert_cmphex(qtest_readl(s, can_pad), ==, 0x0006000f);
        qtest_writel(s, can_pad, 0x00050002);
        qtest_writel(s, can_pad, 0x00050004);
        qtest_writel(s, can_pad, 0x01050002);
        g_assert_cmphex(qtest_readl(s, can_pad), ==, 0x01050002);
        can_pad = 0x4a0037d0 + 4 * i;
        g_assert_cmphex(qtest_readl(s, can_pad), ==, 0x000e000f);
        qtest_writel(s, can_pad, 0x00050000);
        qtest_writel(s, can_pad, 0x00050001);
        qtest_writel(s, can_pad, 0x01050000);
        g_assert_cmphex(qtest_readl(s, can_pad), ==, 0x01050000);
        qtest_writel(s, can_pad, 0x00050004);
        g_assert_cmphex(qtest_readl(s, can_pad), ==, 0x00050004);
    }
    g_assert_cmphex(qtest_readl(s, 0x4a003744), ==, 0x0004000f);
    qtest_writel(s, 0x4a003744, 0x0004000e);
    g_assert_cmphex(qtest_readl(s, 0x4a003744), ==, 0x0004000e);
    qtest_system_reset(s);
    for (unsigned i = 0; i < ARRAY_SIZE(ipl_pads); i++) {
        g_assert_cmphex(qtest_readl(s, ipl_pads[i].address), ==,
                       ipl_pads[i].reset);
    }
    g_assert_cmphex(qtest_readl(s, 0x4ae0c204), ==, 0x0b9bc02f);
    g_assert_cmphex(qtest_readl(s, 0x4a003400), ==, 0x0005000f);
    g_assert_cmphex(qtest_readl(s, 0x4a003800), ==, 0x00060000);
    g_assert_cmphex(qtest_readl(s, pad), ==, 0x0006000f);
    g_assert_cmphex(qtest_readl(s, 0x4a00361c), ==, 0x0004000f);
    g_assert_cmphex(qtest_readl(s, 0x4a003610), ==, 0x0004000f);
    g_assert_cmphex(qtest_readl(s, 0x4a003744), ==, 0x0004000f);
    for (unsigned i = 0; i < 2; i++) {
        g_assert_cmphex(qtest_readl(s, 0x4a003688 + 4 * i), ==, 0x0006000f);
        g_assert_cmphex(qtest_readl(s, 0x4a0037d0 + 4 * i), ==, 0x000e000f);
    }
    for (unsigned i = 0; i < 4; i++) {
        g_assert_cmphex(qtest_readl(s, 0x4a003560 + 4 * i), ==, 0x0004000f);
    }
    qtest_quit(s);
}

static void test_timer12_fixed_clock(void)
{
    QTestState *s = start_rcc();
    const uint32_t base = 0x4ae20000, clock = 0x4ae07848;

    qtest_irq_intercept_in(s, "/machine/interrupt-controller");
    g_assert_cmphex(qtest_readl(s, clock), ==, 1);
    qtest_writel(s, base + 0x3c, 0xfffffffc);
    qtest_writel(s, base + 0x2c, 2);
    qtest_writel(s, base + 0x38, 1);
    qtest_clock_step(s, 122070);
    g_assert_cmphex(qtest_readl(s, base + 0x3c), ==, UINT32_MAX);
    g_assert_false(qtest_get_irq(s, 95));
    qtest_clock_step(s, 1);
    g_assert_cmphex(qtest_readl(s, base + 0x3c), ==, 0);
    g_assert_cmphex(qtest_readl(s, base + 0x38), ==, 0);
    g_assert_true(qtest_get_irq(s, 95));
    qtest_writel(s, base + 0x28, 2);
    g_assert_false(qtest_get_irq(s, 95));

    qtest_writel(s, base + 0x38, 1);
    qtest_clock_step(s, 1000000000);
    g_assert_cmphex(qtest_readl(s, base + 0x3c), ==, 32768);
    qtest_writel(s, clock, 0);
    qtest_writel(s, clock, 0x1000002);
    g_assert_cmphex(qtest_readl(s, clock), ==, 1);
    qtest_clock_step(s, 1000000000);
    g_assert_cmphex(qtest_readl(s, base + 0x3c), ==, 65536);
    qtest_writel(s, base + 0x10, 1);
    g_assert_cmphex(qtest_readl(s, base + 0x3c), ==, 0);
    g_assert_cmphex(qtest_readl(s, base + 0x2c), ==, 0);
    g_assert_cmphex(qtest_readl(s, clock), ==, 1);
    qtest_writel(s, base + 0x3c, UINT32_MAX);
    qtest_writel(s, base + 0x38, 1);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, base + 0x38), ==, 0);
    g_assert_cmphex(qtest_readl(s, base + 0x3c), ==, 0);
    g_assert_cmphex(qtest_readl(s, clock), ==, 1);
    g_assert_false(qtest_get_irq(s, 95));
    qtest_quit(s);
}

static void test_gp_timer_count_irq_clock_reset(gconstpointer data)
{
    const unsigned index = GPOINTER_TO_UINT(data);
    static const uint32_t bases[] = {
        0x48034000, 0x48088000, 0x48820000, 0x48822000, 0x48824000, 0x48826000,
    };
    static const uint32_t clocks[] = {
        0x4a009740, 0x4a009730, 0x4a005558, 0x4a005560, 0x4a005568, 0x4a005570,
    };
    static const unsigned irqs[] = { 39, 47, 41, 42, 43, 44 };
    QTestState *s = start_rcc();
    const uint32_t base = bases[index], clock = clocks[index];
    const unsigned irq = irqs[index];

    qtest_irq_intercept_in(s, "/machine/interrupt-controller");
    g_assert_cmphex(qtest_readl(s, clock), ==, 0x30000);
    qtest_writel(s, clock, 1);
    g_assert_cmphex(qtest_readl(s, clock), ==, 0x30000);
    qtest_writel(s, clock, 2);
    g_assert_cmphex(qtest_readl(s, clock), ==, 2);
    g_assert_cmphex(qtest_readl(s, base + 0x54), ==, 4);
    qtest_writel(s, base + 0x3c, 0xfffffffc);
    qtest_writel(s, base + 0x38, 1);
    qtest_clock_step(s, 199);
    g_assert_cmphex(qtest_readl(s, base + 0x3c), ==, UINT32_MAX);
    g_assert_cmphex(qtest_readl(s, base + 0x24), ==, 0);
    qtest_clock_step(s, 1);
    g_assert_cmphex(qtest_readl(s, base + 0x3c), ==, 0);
    g_assert_cmphex(qtest_readl(s, base + 0x38), ==, 0);
    g_assert_cmphex(qtest_readl(s, base + 0x24), ==, 2);
    g_assert_cmphex(qtest_readl(s, base + 0x28), ==, 0);
    g_assert_false(qtest_get_irq(s, irq));
    qtest_writel(s, base + 0x2c, 2);
    g_assert_true(qtest_get_irq(s, irq));
    qtest_writel(s, base + 0x30, 2);
    g_assert_false(qtest_get_irq(s, irq));
    g_assert_cmphex(qtest_readl(s, base + 0x24), ==, 2);
    qtest_writel(s, base + 0x2c, 2);
    g_assert_true(qtest_get_irq(s, irq));
    qtest_writel(s, base + 0x28, 2);
    g_assert_false(qtest_get_irq(s, irq));

    qtest_writel(s, base + 0x40, 0xfffffff0);
    qtest_writel(s, base + 0x44, 0);
    qtest_writel(s, base + 0x38, 3);
    qtest_clock_step(s, 800);
    g_assert_cmphex(qtest_readl(s, base + 0x3c), ==, 0xfffffff0);
    g_assert_true(qtest_get_irq(s, irq));
    qtest_writel(s, base + 0x28, 2);
    qtest_clock_step(s, 100);
    g_assert_cmphex(qtest_readl(s, base + 0x3c), ==, 0xfffffff2);
    qtest_writel(s, clock, 0);
    qtest_clock_step(s, 10000);
    qtest_writel(s, clock, 2);
    g_assert_cmphex(qtest_readl(s, base + 0x3c), ==, 0xfffffff2);
    qtest_writel(s, base + 0x38, 0);
    qtest_clock_step(s, 1000);
    g_assert_cmphex(qtest_readl(s, base + 0x3c), ==, 0xfffffff2);

    qtest_writel(s, base + 0x3c, 0);
    qtest_writel(s, base + 0x38, 1);
    qtest_clock_step(s, 25);
    g_assert_cmphex(qtest_readl(s, base + 0x3c), ==, 0);
    qtest_writel(s, clock, 0);
    qtest_clock_step(s, 1000);
    qtest_writel(s, clock, 2);
    qtest_clock_step(s, 24);
    g_assert_cmphex(qtest_readl(s, base + 0x3c), ==, 0);
    qtest_clock_step(s, 1);
    g_assert_cmphex(qtest_readl(s, base + 0x3c), ==, 1);
    qtest_writel(s, clock, 0x2000002); /* Unsupported parent clock. */
    g_assert_cmphex(qtest_readl(s, clock), ==, 2);
    qtest_writel(s, base + 0x38, 0);
    qtest_writel(s, base + 0x3c, 0);
    qtest_writel(s, base + 0x38, 0x21); /* Divide SYS_CLK1 by two. */
    qtest_clock_step(s, 1000);
    g_assert_cmphex(qtest_readl(s, base + 0x3c), ==, 10);
    qtest_writel(s, base + 0x38, 0);
    qtest_writel(s, base + 0x3c, 0);
    qtest_writel(s, clock, 0x1000002);
    qtest_writel(s, base + 0x38, 1);
    qtest_clock_step(s, 1000000000);
    g_assert_cmphex(qtest_readl(s, base + 0x3c), ==, 32768);
    qtest_writel(s, base + 0x38, 0x41); /* Compare mode is unsupported. */
    g_assert_cmphex(qtest_readl(s, base + 0x38), ==, 1);
    qtest_writel(s, base + 0x24, 2); /* Documented software-set IRQ bit. */
    g_assert_true(qtest_get_irq(s, irq));
    qtest_writel(s, base + 0x10, 1);
    g_assert_cmphex(qtest_readl(s, base + 0x3c), ==, 0);
    g_assert_cmphex(qtest_readl(s, base + 0x54), ==, 4);
    g_assert_false(qtest_get_irq(s, irq));
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, clock), ==, 0x30000);
    qtest_writel(s, clock, 2);
    g_assert_cmphex(qtest_readl(s, base + 0x38), ==, 0);
    g_assert_cmphex(qtest_readl(s, base + 0x3c), ==, 0);
    qtest_quit(s);
}

static void test_ipu_mmu_registers(void)
{
    QTestState *s = start_rcc();
    uint32_t base = 0x58882000;

    for (unsigned boot = 0; boot < 2; boot++) {
        g_assert_cmphex(qtest_readl(s, base + 0x14), ==, 1);
        g_assert_cmphex(qtest_readl(s, base + 0x44), ==, 0);
        g_assert_cmphex(qtest_readl(s, base + 0x4c), ==, 0);
        g_assert_cmphex(qtest_readl(s, base + 0x18), ==, 0);
        g_assert_cmphex(qtest_readl(s, base + 0x68), ==, 0);
        qtest_writel(s, base + 0x4c, 0x8cc00080);
        qtest_writel(s, base + 0x4c, 0x8cc00081);
        g_assert_cmphex(qtest_readl(s, base + 0x4c), ==, 0x8cc00080);
        for (unsigned page = 0; page < 4; page++) {
            qtest_writel(s, base + 0x50, page << 4);
            qtest_writel(s, base + 0x58, 0x70000004 | page |
                          (page == 3 ? 8 : 0));
            qtest_writel(s, base + 0x5c, 0x8e000000);
            qtest_writel(s, base + 0x54, 0);
            g_assert_cmphex(qtest_readl(s, base + 0x68), ==, 0);
            qtest_writel(s, base + 0x54, 1);
            g_assert_cmphex(qtest_readl(s, base + 0x68), ==,
                            0x70000004 | page | (page == 3 ? 8 : 0));
            g_assert_cmphex(qtest_readl(s, base + 0x6c), ==, 0x8e000000);
            g_assert_cmphex(qtest_readl(s, base + 0x54), ==, 0);
        }
        qtest_writel(s, base + 0x60, 1);
        for (unsigned page = 0; page < 4; page++) {
            qtest_writel(s, base + 0x50, page << 4);
            g_assert_cmphex(qtest_readl(s, base + 0x68) & 4, ==,
                            page == 3 ? 4 : 0);
        }
        qtest_writel(s, base + 0x58, 0x70001000);
        qtest_writel(s, base + 0x64, 1);
        g_assert_cmphex(qtest_readl(s, base + 0x68) & 4, ==, 0);
        qtest_writel(s, base + 0x1c, 0x1f);
        qtest_writel(s, base + 0x88, 0xabcd0001);
        qtest_writel(s, base + 0x44, 6);
        g_assert_cmphex(qtest_readl(s, base + 0x44), ==, 6);
        if (!boot) {
            qtest_writel(s, base + 0x10, 2);
        } else {
            qtest_system_reset(s);
        }
        g_assert_cmphex(qtest_readl(s, base + 0x1c), ==, 0);
        g_assert_cmphex(qtest_readl(s, base + 0x88), ==, 0);
    }
    g_assert_cmphex(qtest_readl(s, base + 0x44), ==, 0);
    g_assert_cmphex(qtest_readl(s, base + 0x4c), ==, 0);
    qtest_quit(s);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/mh2p-rcc/boot-devices-and-reset", test_boot_devices);
    qtest_add_func("/mh2p-rcc/pbias-controls", test_pbias_controls);
    qtest_add_func("/mh2p-rcc/ddr-pad-controls", test_ddr_pad_controls);
    qtest_add_func("/mh2p-rcc/ipu-mmu-registers", test_ipu_mmu_registers);
    qtest_add_func("/mh2p-rcc/gmac-reset", test_gmac_reset);
    qtest_add_func("/mh2p-rcc/mpu-watchdog", test_mpu_watchdog);
    qtest_add_func("/mh2p-rcc/gmac-configuration", test_gmac_configuration);
    qtest_add_func("/mh2p-rcc/control-io1", test_control_io1);
    qtest_add_func("/mh2p-rcc/gmac-mdio", test_gmac_mdio);
    qtest_add_func("/mh2p-rcc/clock-watchdog-dma", test_clock_watchdog_dma);
    qtest_add_func("/mh2p-rcc/sdma-index-configuration",
                   test_sdma_index_configuration);
    qtest_add_func("/mh2p-rcc/uart-disabled-data", test_uart_disabled_data);
    qtest_add_func("/mh2p-rcc/gpio-outputs-reset", test_gpio_outputs_reset);
    qtest_add_func("/mh2p-rcc/gpio-interrupts", test_gpio_interrupts);
    qtest_add_func("/mh2p-rcc/gpio6-wakeup-input-readback",
                   test_gpio6_wakeup_input_readback);
    qtest_add_data_func("/mh2p-rcc/mailbox-fifo",
                        GUINT_TO_POINTER(1), test_mailbox_fifo);
    qtest_add_data_func("/mh2p-rcc/mailbox-interrupts",
                        GUINT_TO_POINTER(1), test_mailbox_interrupts);
    for (unsigned number = 5; number <= 6; number++) {
        g_autofree char *fifo = g_strdup_printf(
            "/mh2p-rcc/mailbox%u-fifo", number);
        g_autofree char *interrupts = g_strdup_printf(
            "/mh2p-rcc/mailbox%u-interrupts", number);
        g_autofree char *routing = g_strdup_printf(
            "/mh2p-rcc/mailbox%u-routing", number);

        qtest_add_data_func(fifo, GUINT_TO_POINTER(number), test_mailbox_fifo);
        qtest_add_data_func(interrupts, GUINT_TO_POINTER(number),
                            test_mailbox_interrupts);
        qtest_add_data_func(routing, GUINT_TO_POINTER(number),
                            test_dra7_mailbox_routing);
    }
    qtest_add_func("/mh2p-rcc/iodelay-handoff", test_iodelay_handoff);
    qtest_add_func("/mh2p-rcc/edma-quiescent",
                   test_edma_quiescent_configuration);
    qtest_add_func("/mh2p-rcc/edma-qdma-configuration",
                   test_edma_qdma_configuration);
    qtest_add_func("/mh2p-rcc/edma-qdma-transfer",
                   test_edma_qdma_transfer);
    qtest_add_func("/mh2p-rcc/usb-clock-controls", test_usb_clock_controls);
    qtest_add_func("/mh2p-rcc/usb-host-setup", test_usb_host_setup);
    qtest_add_func("/mh2p-rcc/pipe3-bridge-pll", test_pipe3_bridge_and_pll);
    qtest_add_func("/mh2p-rcc/crossbar-routing", test_crossbar_routing);
    qtest_add_func("/mh2p-rcc/peripheral-clock-controls",
                   test_peripheral_clock_controls);
    for (unsigned i = 0; i < 10; i++) {
        static const unsigned numbers[] = { 1, 2, 3, 6, 5, 7, 10, 4, 8, 9 };
        g_autofree char *name = g_strdup_printf("/mh2p-rcc/uart%u-clock-irq",
                                                numbers[i]);

        qtest_add_data_func(name, GUINT_TO_POINTER(i), test_uart_clock_irq);
    }
    qtest_add_func("/mh2p-rcc/gpio-clock-gates", test_gpio_clock_gates);
    qtest_add_func("/mh2p-rcc/hardware-spinlocks", test_hardware_spinlocks);
    qtest_add_func("/mh2p-rcc/ipu-crossbar-mmr-lock", test_ipu_crossbar_mmr_lock);
    qtest_add_func("/mh2p-rcc/sync32k-counter", test_sync32k_counter);
    qtest_add_func("/mh2p-rcc/dcan-initialization", test_dcan_initialization);
    qtest_add_func("/mh2p-rcc/dcan-message-ram-interfaces",
                   test_dcan_message_ram_interfaces);
    qtest_add_func("/mh2p-rcc/mcspi-initialization", test_mcspi_initialization);
    qtest_add_func("/mh2p-rcc/mcspi-dormant-test-directions",
                   test_mcspi_dormant_test_directions);
    qtest_add_func("/mh2p-rcc/i2c-eeprom", test_i2c_eeprom);
    qtest_add_func("/mh2p-rcc/i2c-fifo-irq-reassertion",
                   test_i2c_fifo_irq_reassertion);
    qtest_add_func("/mh2p-rcc/dcan-disconnected-transmission",
                   test_dcan_disconnected_transmission);
    qtest_add_func("/mh2p-rcc/dcan-bus-receive", test_dcan_bus_receive);
    qtest_add_func("/mh2p-rcc/dcan-receive-fifo", test_dcan_receive_fifo);
    qtest_add_func("/mh2p-rcc/dcan-peer-clock-and-init",
                   test_dcan_peer_clock_and_init);
    qtest_add_func("/mh2p-rcc/dcan-chardev-peer", test_dcan_chardev_peer);
    qtest_add_data_func("/mh2p-rcc/mcspi-transfers",
                        GUINT_TO_POINTER(4), test_mcspi_transfers);
    qtest_add_data_func("/mh2p-rcc/mcspi3-transfers",
                        GUINT_TO_POINTER(3), test_mcspi_transfers);
    qtest_add_func("/mh2p-rcc/tja1145-control-plane", test_tja1145_control_plane);
    qtest_add_data_func("/mh2p-rcc/tja1145-can-activity",
                        GUINT_TO_POINTER(0), test_tja1145_can_activity);
    qtest_add_data_func("/mh2p-rcc/tja1145-frame-wakeup-fixture",
                        GUINT_TO_POINTER(1), test_tja1145_can_activity);
    qtest_add_func("/mh2p-rcc/i2c-clocks-reset-irq-nack",
                   test_i2c_clocks_reset_irq_nack);
    qtest_add_func("/mh2p-rcc/i2c-dormant-wakeup-configuration",
                   test_i2c_dormant_wakeup_configuration);
    qtest_add_func("/mh2p-rcc/peripheral-context-loss",
                   test_peripheral_context_loss);
    qtest_add_func("/mh2p-rcc/i2c-real-slave-fifo-transfer",
                   test_i2c_real_slave_fifo_transfer);
    qtest_add_func("/mh2p-rcc/atl-clock-controls", test_atl_clock_controls);
    qtest_add_func("/mh2p-rcc/mlb-clock-controls", test_mlb_clock_controls);
    qtest_add_func("/mh2p-rcc/mlb-dll-clock-controls",
                   test_mlb_dll_clock_controls);
    qtest_add_func("/mh2p-rcc/mlb-channel-table", test_mlb_channel_table);
    qtest_add_func("/mh2p-rcc/mlb-configuration", test_mlb_configuration);
    qtest_add_func("/mh2p-rcc/ipu-peripheral-clock-domain",
                   test_ipu_peripheral_clock_domain);
    qtest_add_func("/mh2p-rcc/atl-configuration", test_atl_configuration);
    qtest_add_func("/mh2p-rcc/pcie-clock-controls", test_pcie_clock_controls);
    qtest_add_func("/mh2p-rcc/pcie-reference-dpll", test_pcie_reference_dpll);
    qtest_add_func("/mh2p-rcc/mpu-dpll-modes", test_mpu_dpll_modes);
    qtest_add_func("/mh2p-rcc/bypass-dpll-modes", test_bypass_dpll_modes);
    qtest_add_func("/mh2p-rcc/disabled-peripheral-clocks",
                   test_disabled_peripheral_clocks);
    qtest_add_func("/mh2p-rcc/power-clocks-default-off",
                   test_power_clocks_default_off);
    qtest_add_func("/mh2p-rcc/pcie-analog-pll", test_pcie_analog_pll);
    qtest_add_func("/mh2p-rcc/pcie-output-clocks", test_pcie_output_clocks);
    qtest_add_func("/mh2p-rcc/pcie-phy-configuration",
                   test_pcie_phy_configuration);
    qtest_add_func("/mh2p-rcc/pcie-local-reset", test_pcie_local_reset);
    qtest_add_func("/mh2p-rcc/pcie-endpoint-configuration",
                   test_pcie_endpoint_configuration);
    qtest_add_func("/mh2p-rcc/pcie-no-link-partner", test_pcie_no_link_partner);
    qtest_add_func("/mh2p-rcc/ocp2scp3-configuration",
                   test_ocp2scp3_configuration);
    qtest_add_func("/mh2p-rcc/timer2-handoff", test_timer2_handoff);
    qtest_add_data_func("/mh2p-rcc/timer3-count-irq-clock-reset",
                        GUINT_TO_POINTER(0), test_gp_timer_count_irq_clock_reset);
    qtest_add_data_func("/mh2p-rcc/timer11-count-irq-clock-reset",
                        GUINT_TO_POINTER(1), test_gp_timer_count_irq_clock_reset);
    for (unsigned timer = 5; timer <= 8; timer++) {
        g_autofree char *name = g_strdup_printf(
            "/mh2p-rcc/timer%u-count-irq-clock-reset", timer);

        qtest_add_data_func(name, GUINT_TO_POINTER(timer - 3),
                            test_gp_timer_count_irq_clock_reset);
    }
    qtest_add_func("/mh2p-rcc/timer12-fixed-clock", test_timer12_fixed_clock);
    qtest_add_func("/mh2p-rcc/ocmc-ram", test_ocmc_ram);
    qtest_add_func("/mh2p-rcc/pad-controls", test_pad_controls);
    qtest_add_func("/mh2p-rcc/lis331dlh-samples", test_lis331dlh_samples);
    qtest_add_func("/mh2p-rcc/gpio8-pad-controls", test_gpio8_pad_controls);
    qtest_add_func("/mh2p-rcc/nor-fixture", test_nor_fixture);
    qtest_add_func("/mh2p-rcc/gpmc-nor-mapping", test_gpmc_nor_mapping);
    return g_test_run();
}

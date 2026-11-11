/* SPDX-License-Identifier: GPL-2.0-or-later */
#define COMPILING_SYSTEM_VS_USER
#define CONFIG_SOFTMMU
#include "qemu/osdep.h"

#include "accel/tcg/tb-hash.h"

const TargetPageBits target_page = {
    .decided = true,
    .bits = 10,
    .mask = UINT64_MAX << 10,
};

static void test_page_buckets(void)
{
    const vaddr bases[] = {
        0, 0x80000000, 0xfffffc00, UINT64_C(0x123456780000),
        UINT64_MAX & (UINT64_MAX << 10),
    };

    for (unsigned base = 0; base < G_N_ELEMENTS(bases); base++) {
        for (unsigned page = 0; page < 256; page++) {
            vaddr address = bases[base] + page * TARGET_PAGE_SIZE;
            unsigned bucket = tb_jmp_cache_hash_page(address);

            for (unsigned offset = 0; offset < TARGET_PAGE_SIZE; offset++) {
                unsigned hash = tb_jmp_cache_hash_func(address + offset);

                g_assert_cmpuint(hash, <, TB_JMP_CACHE_SIZE);
                g_assert_cmpuint(hash & TB_JMP_PAGE_MASK, ==, bucket);
            }
        }
    }
}

static void test_aligned_targets(void)
{
    bool seen[TB_JMP_CACHE_SIZE] = { false };
    unsigned unique = 0;

    for (unsigned page = 0; page < 256; page++) {
        unsigned hash = tb_jmp_cache_hash_func(0x81000000 + page * 4096);

        unique += !seen[hash];
        seen[hash] = true;
    }
    g_assert_cmpuint(unique, ==, 256);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/tb-hash/page-buckets", test_page_buckets);
    g_test_add_func("/tb-hash/aligned-targets", test_aligned_targets);
    return g_test_run();
}

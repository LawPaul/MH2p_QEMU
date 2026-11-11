/*
 * The per-CPU TranslationBlock jump cache.
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ACCEL_TCG_TB_JMP_CACHE_H
#define ACCEL_TCG_TB_JMP_CACHE_H

#include "qemu/rcu.h"
#include "exec/cpu-common.h"

#define TB_JMP_CACHE_BITS 12
#define TB_JMP_CACHE_SIZE (1 << TB_JMP_CACHE_BITS)

/*
 * Invalidated in parallel; all accesses to 'tb' must be atomic.
 * A valid entry is read/written by a single CPU, therefore there is
 * no need for qatomic_rcu_read() and pc is always consistent with a
 * non-NULL value of 'tb'.  Strictly speaking pc is only needed for
 * CF_PCREL, but it's used always for simplicity.
 */
typedef struct CPUJumpCache {
    struct rcu_head rcu;
    struct {
        TranslationBlock *tb;
        vaddr pc;
    } array[TB_JMP_CACHE_SIZE];
} CPUJumpCache;

/*
 * Guests switch ASID on every context switch.  The softmmu TLB is not
 * ASID-tagged, so it is still flushed, but the virtual-PC jump cache is
 * kept in one bank per recently used translation context.  A bank's
 * contents are only valid for the generation in which it was filled;
 * any ordinary jump-cache flush bumps the generation, which invalidates
 * every inactive bank lazily.
 */
#define TB_JMP_CACHE_BANKS 16

typedef struct CPUJumpCacheSet {
    struct rcu_head rcu;
    uint32_t gen;
    unsigned current;
    unsigned next_victim;
    bool used[TB_JMP_CACHE_BANKS];
    uint32_t bank_gen[TB_JMP_CACHE_BANKS];
    uint64_t tag[TB_JMP_CACHE_BANKS];
    CPUJumpCache bank[TB_JMP_CACHE_BANKS];
} CPUJumpCacheSet;

void tcg_switch_jmp_cache(CPUState *cpu, uint64_t tag);
void tcg_flush_jmp_cache_asid(CPUState *cpu, uint32_t asid);

#endif /* ACCEL_TCG_TB_JMP_CACHE_H */

/*
 * BCM4359 PCIe Wi-Fi function: backplane, cores and TCM as seen from the host.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_ARM_MH2P_BCM4359_H
#define HW_ARM_MH2P_BCM4359_H

#include "exec/memattrs.h"

#define MH2P_BCM4359_BAR0_SIZE 0x8000       /* 32 KiB register window */
#define MH2P_BCM4359_BAR2_SIZE 0x400000     /* 4 MiB backplane/TCM window */

typedef struct MH2PBCM4359 MH2PBCM4359;

/* The function's view of the host: bus-master DMA and its INTA line. */
typedef struct MH2PBCM4359Host {
    void *opaque;
    MemTxResult (*dma)(void *opaque, uint64_t address, void *buf,
                       unsigned len, bool write);
    void (*irq)(void *opaque, bool level);
    /* MSI: a posted memory write of 'data' to 'address'. */
    void (*msi)(void *opaque, uint64_t address, uint32_t data);
} MH2PBCM4359Host;

MH2PBCM4359 *mh2p_bcm4359_new(const MH2PBCM4359Host *host);
void mh2p_bcm4359_reset(MH2PBCM4359 *b);
/* Vendor configuration registers 0x40-0xff (BAR0 windows and friends). */
bool mh2p_bcm4359_config(MH2PBCM4359 *b, unsigned reg, uint32_t *value,
                         bool write);
void mh2p_bcm4359_bar0(MH2PBCM4359 *b, uint32_t offset, unsigned size,
                       uint64_t *value, bool write);
void mh2p_bcm4359_bar2(MH2PBCM4359 *b, uint32_t offset, unsigned size,
                       uint64_t *value, bool write);

#endif

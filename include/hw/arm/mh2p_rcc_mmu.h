/*
 * DRA7 IPU L2 MMU.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_ARM_MH2P_RCC_MMU_H
#define HW_ARM_MH2P_RCC_MMU_H

#include "hw/core/sysbus.h"

#define TYPE_MH2P_IPU_MMU "mh2p-ipu-mmu"

MemoryRegion *mh2p_ipu_mmu_memory(DeviceState *dev, AddressSpace *bypass);

#endif

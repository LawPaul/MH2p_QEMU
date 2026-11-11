/*
 * Local MMX/RCC configuration transport, not a complete PCIe fabric.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MH2P_PCIE_CONFIG_H
#define HW_MH2P_PCIE_CONFIG_H

#include "hw/core/sysbus.h"

typedef bool MH2PPCIeReady(void *opaque);
typedef MemTxResult MH2PPCIeConfig(void *opaque, unsigned reg, unsigned size,
                                 uint32_t *value, bool write);

DeviceState *mh2p_pcie_config_create(Object *parent, const char *name,
                                   const char *fd,
                                   MH2PPCIeReady *ready,
                                   MH2PPCIeConfig *config,
                                   MH2PPCIeConfig *memory, void *opaque);
bool mh2p_pcie_config_link(DeviceState *dev, bool local_ready);
bool mh2p_pcie_config_peer(DeviceState *dev);
MemTxResult mh2p_pcie_config_access(DeviceState *dev, bool local_ready,
                                   unsigned reg, unsigned size,
                                   uint32_t *value, bool write);
MemTxResult mh2p_pcie_memory_access(DeviceState *dev, bool local_ready,
                                  uint32_t address, unsigned size,
                                  uint32_t *value, bool write);

#endif

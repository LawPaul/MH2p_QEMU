/*
 * Experimental RCC native DSP integration.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_ARM_MH2P_RCC_DSP_H
#define HW_ARM_MH2P_RCC_DSP_H

#include "hw/core/sysbus.h"

DeviceState *mh2p_rcc_dsp_create(Object *owner, DeviceState *mmu[2],
                                const char *backend);
uint32_t mh2p_rcc_dsp_control(DeviceState *dev, uint32_t reset,
                             bool clock_running, uint32_t vector);
MemTxResult mh2p_rcc_dsp_dma_read(DeviceState *dev, uint32_t address,
                                 uint8_t *data, size_t size);

#endif

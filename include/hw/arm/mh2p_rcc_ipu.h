/*
 * MH2P native IPU subsystems and optional IPU1 direct-start fixture.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_ARM_MH2P_RCC_IPU_H
#define HW_ARM_MH2P_RCC_IPU_H

#include "qom/object.h"
#include "hw/core/qdev.h"

typedef struct MH2PIPU1 MH2PIPU1;

DeviceState *mh2p_rcc_ipu1_create(Object *owner, const char *firmware,
                                DeviceState *mmu, DeviceState **secondary,
                                MH2PIPU1 **state, unsigned subsystem);
void mh2p_rcc_ipu1_control(MH2PIPU1 *s, uint32_t reset, bool clock);

#endif

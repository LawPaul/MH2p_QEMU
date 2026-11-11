/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_SSI_DRA7_MCSPI_H
#define HW_SSI_DRA7_MCSPI_H

#include "hw/core/qdev.h"

#define TYPE_DRA7_MCSPI "dra7-mcspi"

void dra7_mcspi_set_clock(DeviceState *dev, bool enabled);

#endif

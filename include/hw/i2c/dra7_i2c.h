/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_I2C_DRA7_I2C_H
#define HW_I2C_DRA7_I2C_H

#include "hw/core/qdev.h"

#define TYPE_DRA7_I2C "dra7-i2c"

bool dra7_i2c_set_clock(DeviceState *dev, bool enabled);

#endif

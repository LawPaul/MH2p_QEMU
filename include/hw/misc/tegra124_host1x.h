/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_TEGRA124_HOST1X_H
#define HW_MISC_TEGRA124_HOST1X_H

#include "hw/core/sysbus.h"

#define TYPE_TEGRA124_HOST1X "tegra124-host1x"
OBJECT_DECLARE_SIMPLE_TYPE(Tegra124Host1xState, TEGRA124_HOST1X)

bool tegra124_host1x_syncpt_incr(Tegra124Host1xState *s, unsigned id);

#endif

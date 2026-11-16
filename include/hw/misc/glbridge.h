/*
 * Optional host GLES service shared by the MHI2 and MH2P experiments.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_GLBRIDGE_H
#define HW_MISC_GLBRIDGE_H

#include "qom/object.h"

void glbridge_create(Object *parent);

#endif

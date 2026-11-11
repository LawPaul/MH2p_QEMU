/*
 * Local MH2P PCIe reference-clock/reset wiring, not a TLP transport.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MH2P_PCIE_SIDEBAND_H
#define HW_MH2P_PCIE_SIDEBAND_H

#include "hw/core/sysbus.h"

/* Named socket: mh2p-pcie-sideband. Outputs: clock, reset_n. */
DeviceState *mh2p_pcie_sideband_create(Object *parent, bool transmitter);
void mh2p_pcie_sideband_set(DeviceState *dev, bool clock, bool reset_n);

#endif

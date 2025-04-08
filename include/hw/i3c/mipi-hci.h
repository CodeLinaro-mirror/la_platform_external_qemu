/*
 * MIPI HCI I3C Controller
 *
 * Copyright (C) 2025 Google, LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef MIPI_HCI_H
#define MIPI_HCI_H

#include "hw/core/sysbus.h"
#include "hw/i3c/i3c.h"
#include "hw/i3c/hci-core.h"
#include "hw/i3c/hci-dma.h"
#include "hw/i3c/hci-ext.h"
#include "hw/i3c/hci-dat.h"

#define TYPE_MIPI_HCI "mipi.hci"
OBJECT_DECLARE_TYPE(MIPIHCIState, MIPIHCIClass, MIPI_HCI)

typedef struct MIPIHCIClass {
    SysBusDeviceClass parent_class;
} MIPIHCIClass;

#define MIPI_HCI_MMIO_SIZE 0x1000

typedef struct MIPIHCIState {
    SysBusDevice parent;

    HCICoreState core;
    HCIDMAState dma;
    HCIExtCapState ext_cap;
    HCIDATState dat;

    MemoryRegion iomem;
    I3CBus *bus;
} MIPIHCIState;

#endif /* MIPI_HCI_H */

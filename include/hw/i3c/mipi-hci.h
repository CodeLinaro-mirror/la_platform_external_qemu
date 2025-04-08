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

#define TYPE_MIPI_HCI "mipi.hci"
OBJECT_DECLARE_TYPE(MIPIHCIState, MIPIHCIClass, MIPI_HCI)

typedef struct MIPIHCIClass {
    SysBusDeviceClass parent_class;
} MIPIHCIClass;

typedef struct MIPIHCIState {
    SysBusDevice parent;

    I3CBus *bus;
} MIPIHCIState;

#endif /* MIPI_HCI_H */

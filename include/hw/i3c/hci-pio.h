/*
 * MIPI HCI I3C PIO state
 *
 * Copyright (C) 2025 Google, LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HCI_PIO_H
#define HCI_PIO_H

#include "hw/core/registerfields.h"
#include "system/memory.h"

#define HCI_PIO_NUM_REGS 12

typedef struct HCIPIOState {
    MemoryRegion mmio;
} HCIPIOState;

#endif

/*
 * AST27xx I3C Controller
 *
 * Copyright (C) 2025 Google, LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef AST27XX_I3C_H
#define AST27XX_I3C_H

#include "hw/core/sysbus.h"
#include "hw/i3c/mipi-hci.h"

#define TYPE_AST27XX_I3C "ast27xx-i3c"
OBJECT_DECLARE_TYPE(AST27xxI3CState, AST27xxI3CClass, AST27XX_I3C)

typedef struct AST27xxI3CClass {
    MIPIHCIClass parent_class;

    ResettablePhases parent_phases;
    DeviceRealize parent_realize;
} AST27xxI3CClass;

typedef struct AST27xxI3CState {
    MIPIHCIState parent;

    MemoryRegion iomem;
} AST27xxI3CState;

#endif /* AST27XX_I3C_H */

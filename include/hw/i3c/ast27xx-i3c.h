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

#define AST27XX_I3C_CTRL_NUM_REGS 63
#define AST27XX_I3C_PHY_NUM_REGS 61
#define AST27XX_I3C_DMAARB_NUM_REGS 8

typedef struct AST27xxI3CClass {
    MIPIHCIClass parent_class;

    ResettablePhases parent_phases;
    DeviceRealize parent_realize;
} AST27xxI3CClass;

typedef struct AST27xxI3CState {
    MIPIHCIState parent;

    uint32_t ctrl_regs[AST27XX_I3C_CTRL_NUM_REGS];
    uint32_t phy_regs[AST27XX_I3C_PHY_NUM_REGS];
    uint32_t dmaarb_regs[AST27XX_I3C_DMAARB_NUM_REGS];

    MemoryRegion phy_iomem;
    MemoryRegion ctrl_iomem;
    MemoryRegion dmaarb_iomem;
} AST27xxI3CState;

#endif /* AST27XX_I3C_H */

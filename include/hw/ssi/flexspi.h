/*
 * QEMU model for FLEXSPI
 *
 * Copyright (c) 2024 Google LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_RT500_FLEXSPI_H
#define HW_RT500_FLEXSPI_H

#include "hw/core/sysbus.h"
#include "hw/ssi/ssi.h"
#include "hw/arm/svd/flexspi.h"

#define TYPE_FLEXSPI "flexspi"
#define FLEXSPI(obj) OBJECT_CHECK(FlexSpiState, (obj), TYPE_FLEXSPI)

typedef struct {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    uint32_t regs[FLEXSPI_REGS_NO];
    MemoryRegion mem;
    uint64_t mmap_size;
} FlexSpiState;

#endif /* HW_RT500_FLEXSPI_H */

/*
 * QEMU model for NXP's FLEXCOMM
 *
 * Copyright (c) 2024 Google LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_FLEXCOMM_FUNCTION_H
#define HW_FLEXCOMM_FUNCTION_H

#include "qemu/fifo32.h"
#include "hw/core/sysbus.h"

#define TYPE_FLEXCOMM_FUNCTION "flexcomm-function"
OBJECT_DECLARE_TYPE(FlexcommFunction, FlexcommFunctionClass, FLEXCOMM_FUNCTION);

struct FlexcommFunction {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    uint32_t *regs;
    Fifo32 *tx_fifo;
    Fifo32 *rx_fifo;
};

typedef void (*FlexcommFunctionSelect)(FlexcommFunction *f, bool selected);

struct FlexcommFunctionClass {
    SysBusDeviceClass parent_class;

    const MemoryRegionOps *mmio_ops;
    const char *name;
    bool has_fifos;
    FlexcommFunctionSelect select;
};

static inline void flexcomm_select(FlexcommFunction *obj, bool selected)
{
    FlexcommFunctionClass *klass = FLEXCOMM_FUNCTION_GET_CLASS(obj);

    klass->select(obj, selected);
}

void flexcomm_set_irq(FlexcommFunction *f, bool irq);
void flexcomm_update_fifostat(FlexcommFunction *f);
void flexcomm_clear_fifostat(FlexcommFunction *f, uint64_t value);
void flexcomm_init_fifos(FlexcommFunction *f, unsigned num);
void flexcomm_reset_fifos(FlexcommFunction *f);

#endif /* HW_FLEXCOMM_FUNCTION_H */

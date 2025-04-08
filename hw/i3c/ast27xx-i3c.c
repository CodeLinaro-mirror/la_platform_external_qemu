/*
 * AST27xx I3C Controller
 *
 * Copyright (C) 2025 Google, LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "hw/i3c/ast27xx-i3c.h"
#include "hw/i3c/mipi-hci.h"
#include "hw/i3c/dw-i3c.h"
#include "hw/core/registerfields.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "migration/vmstate.h"
#include "hw/core/qdev.h"
#include "trace.h"
#include "hw/i3c/i3c.h"
#include "hw/core/irq.h"

#define AST27XX_I3C_MMIO_SIZE 0x1000

static uint64_t ast27xx_i3c_read(void *opaque, hwaddr offset, unsigned size)
{
    return 0;
}

static void ast27xx_i3c_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
}

static const MemoryRegionOps ast27xx_i3c_ops = {
    .read = ast27xx_i3c_read,
    .write = ast27xx_i3c_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void ast27xx_i3c_instance_init(Object *obj)
{
}

static void ast27xx_i3c_realize(DeviceState *dev, Error **errp)
{
    AST27xxI3CState *s = AST27XX_I3C(dev);


    memory_region_init_io(&s->iomem, OBJECT(s), &ast27xx_i3c_ops, s,
                          TYPE_AST27XX_I3C"-mmio", AST27XX_I3C_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void ast27xx_i3c_enter_reset(Object *obj, ResetType type)
{
    AST27xxI3CClass *aic = AST27XX_I3C_GET_CLASS(obj);

    if (aic->parent_phases.enter) {
        aic->parent_phases.enter(obj, type);
    }
}

static void ast27xx_i3c_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    AST27xxI3CClass *aic = AST27XX_I3C_CLASS(klass);

    dc->desc = "AST27xx I3C Controller";

    device_class_set_parent_realize(dc, ast27xx_i3c_realize,
                                    &aic->parent_realize);
    resettable_class_set_parent_phases(rc, ast27xx_i3c_enter_reset, NULL, NULL,
                                       &aic->parent_phases);
}

static const TypeInfo ast27xx_i3c_info = {
    .name = TYPE_AST27XX_I3C,
    .parent = TYPE_MIPI_HCI,
    .instance_init = ast27xx_i3c_instance_init,
    .instance_size = sizeof(AST27xxI3CState),
    .class_init = ast27xx_i3c_class_init,
    .class_size = sizeof(AST27xxI3CClass),
};

static void ast27xx_i3c_register_types(void)
{
    type_register_static(&ast27xx_i3c_info);
}

type_init(ast27xx_i3c_register_types);

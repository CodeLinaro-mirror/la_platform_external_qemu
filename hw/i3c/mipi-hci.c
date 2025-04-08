/*
 * MIPI HCI I3C controller
 *
 * Copyright (C) 2025 Google, LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/registerfields.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "migration/vmstate.h"
#include "trace.h"
#include "hw/i3c/i3c.h"
#include "hw/i3c/mipi-hci.h"
#include "hw/core/irq.h"

static void mipi_hci_instance_init(Object *obj)
{
}

static void mipi_hci_realize(DeviceState *dev, Error **errp)
{
    MIPIHCIState *s = MIPI_HCI(dev);

    s->bus = i3c_init_bus(DEVICE(s), NULL);
}

static void mipi_hci_enter_reset(Object *obj, ResetType type)
{
}

static void mipi_hci_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.enter = mipi_hci_enter_reset;
    dc->realize = mipi_hci_realize;
    dc->desc = "MIPI HCI I3C Controller";
}

static const TypeInfo mipi_hci_info = {
    .name = TYPE_MIPI_HCI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_init = mipi_hci_instance_init,
    .instance_size = sizeof(MIPIHCIState),
    .class_init = mipi_hci_class_init,
    .class_size = sizeof(MIPIHCIClass),
};

static void mipi_hci_register_types(void)
{
    type_register_static(&mipi_hci_info);
}

type_init(mipi_hci_register_types);

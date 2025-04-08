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
#include "hw/i3c/hci-core.h"
#include "hci-core-internal.h"
#include "trace.h"
#include "hw/i3c/i3c.h"
#include "hw/i3c/mipi-hci.h"
#include "hw/core/irq.h"

static const MemoryRegionOps hci_core_ops = {
    .read = hci_core_read,
    .write = hci_core_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void mipi_hci_instance_init(Object *obj)
{
}

static void mipi_hci_realize(DeviceState *dev, Error **errp)
{
    MIPIHCIState *s = MIPI_HCI(dev);
    HCICoreState *core = &s->core;

    memory_region_init(&s->iomem, OBJECT(s), TYPE_MIPI_HCI"-mmio",
                       MIPI_HCI_MMIO_SIZE);
    memory_region_init_io(&core->iomem, OBJECT(s), &hci_core_ops, s,
                          TYPE_MIPI_HCI"-core-mmio",
                          HCI_CORE_NUM_REGS * sizeof(uint32_t));
    memory_region_add_subregion(&s->iomem, HCI_CORE_MMIO_OFFSET,
                                &core->iomem);

    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    s->bus = i3c_init_bus(DEVICE(s), NULL);
}

static void mipi_hci_enter_reset(Object *obj, ResetType type)
{
    MIPIHCIState *s = MIPI_HCI(obj);

    hci_core_reset(&s->core);
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

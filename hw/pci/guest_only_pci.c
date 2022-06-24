// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Guest Only PCI Device
 *
 * Copyright 2022 Google LLC
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pci_regs.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/registerfields.h"
#include "migration/vmstate.h"
#include "trace.h"

#define TYPE_GUEST_ONLY_PCI "guest-only-pci"
OBJECT_DECLARE_SIMPLE_TYPE(GuestOnlyPci, GUEST_ONLY_PCI)

typedef struct GuestOnlyPci {
    /*< private >*/
    PCIDevice parent;
    /*< public >*/
    MemoryRegion mmio;

    /* PCI config properties */
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t subsystem_vendor_id;
    uint16_t subsystem_device_id;
    uint32_t class_revision;
    uint64_t bar_size;
} GuestOnlyPci;

static uint64_t guest_only_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
   /* GuestOnlyPci *s = opaque; */
   return 0;
}

static void guest_only_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                                  unsigned size)
{
    /* GuestOnlyPci *s = opaque; */
}

/* This MMIO Space won't have a read-write functionality, it just exists */
static const MemoryRegionOps mmio_ops = {
    .read = guest_only_mmio_read,
    .write = guest_only_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void guest_only_pci_realize(PCIDevice *p, Error **errp)
{
    GuestOnlyPci *s = GUEST_ONLY_PCI(p);

    if (s->vendor_id == 0xffff) {
        error_setg(errp, "Vendor ID invalid, it must always be supplied");
        return;
    }
    if (s->device_id == 0xffff) {
        error_setg(errp, "Device ID invalid, it must always be supplied");
        return;
    }

    pci_set_word(&p->config[PCI_VENDOR_ID], s->vendor_id);
    pci_set_word(&p->config[PCI_DEVICE_ID], s->device_id);
    pci_set_word(&p->config[PCI_SUBSYSTEM_VENDOR_ID], s->subsystem_vendor_id);
    pci_set_word(&p->config[PCI_SUBSYSTEM_ID], s->subsystem_device_id);
    pci_set_long(&p->config[PCI_CLASS_REVISION], s->class_revision);

    if (s->bar_size > 0) {
        memory_region_init_io(/*mr=*/&s->mmio, /*owner=*/OBJECT(p), &mmio_ops,
                              /*opaque=*/s, "guest-only-bar",
                              /*size=*/s->bar_size);

        pci_register_bar(p, /*region_num=*/0,
                         /*attr=*/PCI_BASE_ADDRESS_MEM_TYPE_64, &s->mmio);
    }
}

static const VMStateDescription vmstate_guest_only_pci = {
    .name = "guest_only_pci",
    .version_id = 0,
    .minimum_version_id = 0,
    .fields = (VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent, GuestOnlyPci),
        VMSTATE_END_OF_LIST()
    }
};

static const Property guest_only_pci_properties[] = {
    DEFINE_PROP_UINT16("vendor-id", GuestOnlyPci, vendor_id, 0xffff),
    DEFINE_PROP_UINT16("device-id", GuestOnlyPci, device_id, 0xffff),
    DEFINE_PROP_UINT16("subsystem-vendor-id", GuestOnlyPci,
                       subsystem_vendor_id, 0),
    DEFINE_PROP_UINT16("subsystem-device-id", GuestOnlyPci,
                       subsystem_device_id, 0),
    DEFINE_PROP_UINT32("class-revision", GuestOnlyPci, class_revision,
                       0xff000000 /* Unknown class */),
    DEFINE_PROP_UINT64("bar-size", GuestOnlyPci, bar_size, 0),
};

static void guest_only_pci_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pdc = PCI_DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_guest_only_pci;
    device_class_set_props(dc, guest_only_pci_properties);
    pdc->realize = guest_only_pci_realize;
}

static const TypeInfo guest_only_pci_types[] = {
    {
        .name = TYPE_GUEST_ONLY_PCI,
        .parent = TYPE_PCI_DEVICE,
        .instance_size = sizeof(GuestOnlyPci),
        .class_init = guest_only_pci_class_init,
        .interfaces = (InterfaceInfo[]) {
            { INTERFACE_PCIE_DEVICE },
            { }
        }
    },
};
DEFINE_TYPES(guest_only_pci_types)

/*
 * PLX PEX PCIe Virtual Switch - Upstream
 *
 * Copyright 2024 Google LLC
 * Author: Nabih Estefan <nabihestefan@google.com>
 *
 * Based on xio3130_upstream.c and guest_only_pci.c
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/pci/pci_ids.h"
#include "hw/pci/msi.h"
#include "hw/pci/pcie.h"
#include "hw/pci/pcie_port.h"
#include "hw/pci-bridge/plx_vswitch.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/registerfields.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

static void plx_vswitch_upstream_write_config(PCIDevice *d, uint32_t address,
                                          uint32_t val, int len)
{
    pci_bridge_write_config(d, address, val, len);
    pcie_cap_flr_write_config(d, address, val, len);
    pcie_aer_write_config(d, address, val, len);
}

static void plx_vswitch_upstream_reset(DeviceState *qdev)
{
    PCIDevice *d = PCI_DEVICE(qdev);

    pci_bridge_reset(qdev);
    pcie_cap_deverr_reset(d);
}

static void plx_vswitch_upstream_realize(PCIDevice *d, Error **errp)
{
    PCIPlxVSwitchUpstream *vs = PLX_VSWITCH_UPSTREAM_PCI(d);
    PCIEPort *p = PCIE_PORT(d);
    int rc;

    if (vs->vendor_id == 0xffff) {
        error_setg(errp, "Vendor ID invalid, it must always be supplied");
        return;
    }
    if (vs->device_id == 0xffff) {
        error_setg(errp, "Device ID invalid, it must always be supplied");
        return;
    }

    if (vs->subsystem_vendor_id == 0xffff) {
        error_setg(errp, "Subsystem Vendor ID invalid, it must always be supplied");
        return;
    }

    pci_set_word(&d->config[PCI_VENDOR_ID], vs->vendor_id);
    pci_set_word(&d->config[PCI_DEVICE_ID], vs->device_id);
    pci_set_long(&d->config[PCI_CLASS_REVISION], vs->class_revision);

    pci_bridge_initfn(d, TYPE_PCIE_BUS);
    pcie_port_init_reg(d);

    rc = msi_init(d, vs->msi_offset, vs->msi_vector_count,
                  vs->msi64bit, vs->msi_per_vector_mask, errp);
    if (rc < 0) {
        assert(rc == -ENOTSUP);
        goto err_bridge;
    }

    rc = pci_bridge_ssvid_init(d, vs->ssvid_offset,
                               vs->subsystem_vendor_id, vs->subsystem_device_id,
                               errp);
    if (rc < 0) {
        goto err_msi;
    }

    rc = pcie_cap_init(d, vs->cap_offset, PCI_EXP_TYPE_UPSTREAM,
                       p->port, errp);
    if (rc < 0) {
        goto err_msi;
    }
    pcie_cap_flr_init(d);
    pcie_cap_deverr_init(d);

    rc = pcie_aer_init(d, PCI_ERR_VER, vs->aer_offset,
                       PCI_ERR_SIZEOF, errp);
    if (rc < 0) {
        goto err;
    }

    return;

err:
    pcie_cap_exit(d);
err_msi:
    msi_uninit(d);
err_bridge:
    pci_bridge_exitfn(d);
}

static void plx_vswitch_upstream_exitfn(PCIDevice *d)
{
    pcie_aer_exit(d);
    pcie_cap_exit(d);
    msi_uninit(d);
    pci_bridge_exitfn(d);
}

static const VMStateDescription vmstate_plx_vswitch_upstream = {
    .name = PLX_VSWITCH_UPSTREAM,
    .priority = MIG_PRI_PCI_BUS,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj.parent_obj, PCIEPort),
        VMSTATE_STRUCT(parent_obj.parent_obj.exp.aer_log, PCIEPort, 0,
                       vmstate_pcie_aer_log, PCIEAERLog),
        VMSTATE_END_OF_LIST()
    }
};

static const Property plx_vswitch_upstream_pci_properties[] = {
    /* PCI Config information */
    DEFINE_PROP_UINT16("vendor-id", PCIPlxVSwitchUpstream, vendor_id, 0xffff),
    DEFINE_PROP_UINT16("device-id", PCIPlxVSwitchUpstream, device_id, 0xffff),
    DEFINE_PROP_UINT16("subsystem-vendor-id", PCIPlxVSwitchUpstream,
                       subsystem_vendor_id, 0xffff),
    DEFINE_PROP_UINT16("subsystem-device-id", PCIPlxVSwitchUpstream,
                       subsystem_device_id, 0xffff),
    DEFINE_PROP_UINT32("class-revision", PCIPlxVSwitchUpstream, class_revision,
                       0xff000000 /* Unknown class */),
    /* MSI Information */
    DEFINE_PROP_UINT8("msi-offset", PCIPlxVSwitchUpstream, msi_offset,
                     PLX_VSWITCH_MSI_OFFSET),
    DEFINE_PROP_INT32("msi-vector-count", PCIPlxVSwitchUpstream,
                     msi_vector_count, PLX_VSWITCH_MSI_NR_VECTOR),
    DEFINE_PROP_BOOL("msi64bit", PCIPlxVSwitchUpstream, msi64bit,
                    PLX_VSWITCH_MSI_SUPPORTED_FLAGS & PCI_MSI_FLAGS_64BIT),
    DEFINE_PROP_BOOL("msi-per-vector-mask", PCIPlxVSwitchUpstream,
                    msi_per_vector_mask,
                    PLX_VSWITCH_MSI_SUPPORTED_FLAGS & PCI_MSI_FLAGS_MASKBIT),
    /* PCI Config offset overrides */
    DEFINE_PROP_UINT8("ssvid-offset", PCIPlxVSwitchUpstream, ssvid_offset,
                     PLX_VSWITCH_SSVID_OFFSET),
    DEFINE_PROP_UINT8("cap-offset", PCIPlxVSwitchUpstream, cap_offset,
                     PLX_VSWITCH_EXP_OFFSET),
    DEFINE_PROP_UINT16("aer-offset", PCIPlxVSwitchUpstream, aer_offset,
                     PLX_VSWITCH_AER_OFFSET),
};

static void plx_vswitch_upstream_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
    dc->desc = "Upstream Port of PLX PEX PCIe Virtual Switch";
    device_class_set_legacy_reset(dc, plx_vswitch_upstream_reset);
    dc->vmsd = &vmstate_plx_vswitch_upstream;
    device_class_set_props(dc, plx_vswitch_upstream_pci_properties);
    k->config_write = plx_vswitch_upstream_write_config;
    k->realize = plx_vswitch_upstream_realize;
    k->exit = plx_vswitch_upstream_exitfn;
}

static const TypeInfo plx_vswitch_upstream_pci_types[] = {
    {
        .name = TYPE_PLX_VSWITCH_UPSTREAM_PCI,
        .instance_size = sizeof(PCIPlxVSwitchUpstream),
        .parent = TYPE_PCIE_PORT,
        .class_init = plx_vswitch_upstream_class_init,
        .interfaces = (InterfaceInfo[]) {
            { INTERFACE_PCIE_DEVICE },
            { }
        }
    },
};
DEFINE_TYPES(plx_vswitch_upstream_pci_types)

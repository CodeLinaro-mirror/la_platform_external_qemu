/*
 * PLX PEX PCIe Virtual Switch - Downstream
 *
 * Copyright 2024 Google LLC
 * Author: Nabih Estefan <nabihestefan@google.com>
 *
 * Based on xio3130_downstream.c and guest_only_pci.c
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
#include "hw/core/qdev-properties.h"
#include "hw/core/registerfields.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"

static void plx_vswitch_downstream_write_config(PCIDevice *d, uint32_t address,
                                         uint32_t val, int len)
{
    uint16_t slt_ctl, slt_sta;

    pci_bridge_write_config(d, address, val, len);
    pcie_cap_flr_write_config(d, address, val, len);
    pcie_cap_slot_get(d, &slt_ctl, &slt_sta);
    pcie_cap_slot_write_config(d, slt_ctl, slt_sta, address, val, len);
    pcie_aer_write_config(d, address, val, len);
}

static void plx_vswitch_downstream_reset(DeviceState *qdev)
{
    PCIDevice *d = PCI_DEVICE(qdev);
    uint8_t *exp_cap = d->config + d->exp.exp_cap;

    pcie_cap_deverr_reset(d);
    pcie_cap_arifwd_reset(d);
    pcie_cap_slot_reset(d);
    /*
     * No matter if the slot is populated or not, we want to keep the power
     * bits cleared.
     */
    pci_word_test_and_clear_mask(exp_cap + PCI_EXP_SLTCTL, PCI_EXP_SLTCTL_PCC);
    pci_word_test_and_set_mask(exp_cap + PCI_EXP_SLTCTL,
                               PCI_EXP_SLTCTL_PWR_IND_ON);

    pci_bridge_reset(qdev);
}

static void plx_vswitch_downstream_realize(PCIDevice *d, Error **errp)
{
    PCIPlxVSwitchDownstream *vs = PLX_VSWITCH_DOWNSTREAM_PCI(d);
    PCIEPort *p = PCIE_PORT(d);
    PCIESlot *s = PCIE_SLOT(p);
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
        error_setg(errp,
                   "Subsystem Vendor ID invalid, it must always be supplied");
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

    rc = pcie_cap_init(d, vs->cap_offset, PCI_EXP_TYPE_DOWNSTREAM,
                       p->port, errp);
    if (rc < 0) {
        goto err_msi;
    }
    pcie_cap_flr_init(d);
    pcie_cap_deverr_init(d);
    pcie_cap_slot_init(d, s);
    pcie_cap_arifwd_init(d);

    pcie_chassis_create(s->chassis);
    rc = pcie_chassis_add_slot(s);
    if (rc < 0) {
        error_setg(errp, "Can't add chassis slot, error %d", rc);
        goto err_pcie_cap;
    }
    s->hotplug = true;

    rc = pcie_aer_init(d, PCI_ERR_VER, vs->aer_offset,
                       PCI_ERR_SIZEOF, errp);
    if (rc < 0) {
        goto err;
    }

    d->cap_present |= QEMU_PCI_CAP_MULTIFUNCTION;
    pcie_acs_init(d, vs->acs_offset);

    return;

err:
    pcie_chassis_del_slot(s);
err_pcie_cap:
    pcie_cap_exit(d);
err_msi:
    msi_uninit(d);
err_bridge:
    pci_bridge_exitfn(d);
}

static void plx_vswitch_downstream_exitfn(PCIDevice *d)
{
    PCIESlot *s = PCIE_SLOT(d);

    pcie_aer_exit(d);
    pcie_chassis_del_slot(s);
    pcie_cap_exit(d);
    msi_uninit(d);
    pci_bridge_exitfn(d);
}

static const VMStateDescription vmstate_plx_vswitch_downstream = {
    .name = PLX_VSWITCH_DOWNSTREAM,
    .priority = MIG_PRI_PCI_BUS,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = pcie_cap_slot_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj.parent_obj.parent_obj, PCIESlot),
        VMSTATE_STRUCT(parent_obj.parent_obj.exp.aer_log,
                       PCIEPort, 0, vmstate_pcie_aer_log, PCIEAERLog),
        VMSTATE_END_OF_LIST()
    }
};

static const Property plx_vswitch_downstream_pci_properties[] = {
    /* PCI Config Information */
    DEFINE_PROP_UINT16("vendor-id", PCIPlxVSwitchDownstream, vendor_id, 0xffff),
    DEFINE_PROP_UINT16("device-id", PCIPlxVSwitchDownstream, device_id, 0xffff),
    DEFINE_PROP_UINT16("subsystem-vendor-id", PCIPlxVSwitchDownstream,
                       subsystem_vendor_id, 0),
    DEFINE_PROP_UINT16("subsystem-device-id", PCIPlxVSwitchDownstream,
                       subsystem_device_id, 0),
    DEFINE_PROP_UINT32("class-revision", PCIPlxVSwitchDownstream, class_revision,
                       0xff000000 /* Unknown class */),
    /* Slot Specific Information */
    DEFINE_PROP_PCIE_LINK_SPEED("speed", PCIESlot, speed, PCIE_LINK_SPEED_2_5),
    DEFINE_PROP_PCIE_LINK_WIDTH("width", PCIESlot, width, PCIE_LINK_WIDTH_1),
    /* MSI Information */
    DEFINE_PROP_UINT8("msi-offset", PCIPlxVSwitchDownstream, msi_offset,
                     PLX_VSWITCH_MSI_OFFSET),
    DEFINE_PROP_INT32("msi-vector-count", PCIPlxVSwitchDownstream,
                     msi_vector_count, PLX_VSWITCH_MSI_NR_VECTOR),
    DEFINE_PROP_BOOL("msi64bit", PCIPlxVSwitchDownstream, msi64bit,
                    PLX_VSWITCH_MSI_SUPPORTED_FLAGS & PCI_MSI_FLAGS_64BIT),
    DEFINE_PROP_BOOL("msi-per-vector-mask", PCIPlxVSwitchDownstream,
                    msi_per_vector_mask,
                    PLX_VSWITCH_MSI_SUPPORTED_FLAGS & PCI_MSI_FLAGS_MASKBIT),
    /* PCI Config offset overrides */
    DEFINE_PROP_UINT8("ssvid-offset", PCIPlxVSwitchDownstream, ssvid_offset,
                     PLX_VSWITCH_SSVID_OFFSET),
    DEFINE_PROP_UINT8("cap-offset", PCIPlxVSwitchDownstream, cap_offset,
                     PLX_VSWITCH_EXP_OFFSET),
    DEFINE_PROP_UINT16("acs-offset", PCIPlxVSwitchDownstream, acs_offset,
                     PLX_VSWITCH_ACS_OFFSET),
    DEFINE_PROP_UINT16("aer-offset", PCIPlxVSwitchDownstream, aer_offset,
                     PLX_VSWITCH_AER_OFFSET),

    DEFINE_PROP_BIT(COMPAT_PROP_PCP, PCIDevice, cap_present,
                    QEMU_PCIE_SLTCAP_PCP_BITNR, true),
};

static void plx_vswitch_downstream_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
    dc->desc = "Downstream Port of PLX PEX PCIe Virtual Switch";
    device_class_set_legacy_reset(dc, plx_vswitch_downstream_reset);
    dc->vmsd = &vmstate_plx_vswitch_downstream;
    device_class_set_props(dc, plx_vswitch_downstream_pci_properties);

    k->config_write = plx_vswitch_downstream_write_config;
    k->realize = plx_vswitch_downstream_realize;
    k->exit = plx_vswitch_downstream_exitfn;
}

static const TypeInfo plx_vswitch_downstream_pci_types[] = {
    {
        .name = TYPE_PLX_VSWITCH_DOWNSTREAM_PCI,
        .instance_size = sizeof(PCIPlxVSwitchDownstream),
        .parent = TYPE_PCIE_SLOT,
        .class_init = plx_vswitch_downstream_class_init,
        .interfaces = (InterfaceInfo[]) {
            { INTERFACE_PCIE_DEVICE },
            { }
        }
    },
};
DEFINE_TYPES(plx_vswitch_downstream_pci_types)

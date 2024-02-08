/*
 * PLX PEX PCIe Virtual Switch
 *
 * Copyright 2024 Google LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PCI_BRIDGE_PLX_VSWITCH
#define HW_PCI_BRIDGE_PLX_VSWITCH

#include "hw/pci/pcie_port.h"

#define PLX_VSWITCH_DOWNSTREAM "plx-vswitch-downstream"
#define PLX_VSWITCH_UPSTREAM "plx-vswitch-upstream"

#define PLX_VSWITCH_MSI_OFFSET              0x70
#define PLX_VSWITCH_MSI_SUPPORTED_FLAGS     PCI_MSI_FLAGS_64BIT
#define PLX_VSWITCH_MSI_NR_VECTOR           1
#define PLX_VSWITCH_SSVID_OFFSET            0x80
#define PLX_VSWITCH_EXP_OFFSET              0x90
#define PLX_VSWITCH_AER_OFFSET              0x100
#define PLX_VSWITCH_ACS_OFFSET              0x1c0

#define TYPE_PLX_VSWITCH_UPSTREAM_PCI "plx-vswitch-upstream-pci"
OBJECT_DECLARE_SIMPLE_TYPE(PCIPlxVSwitchUpstream, PLX_VSWITCH_UPSTREAM_PCI)

typedef struct PCIPlxVSwitchUpstream {
    PCIEPort parent;

    /* PCI config properties */
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t subsystem_vendor_id;
    uint16_t subsystem_device_id;
    uint32_t class_revision;

    /* MSI properties */
    uint8_t msi_offset;
    int32_t msi_vector_count;
    bool msi64bit;
    bool msi_per_vector_mask;

    /* PCI Config offsets */
    uint8_t ssvid_offset;
    uint8_t cap_offset;
    uint16_t aer_offset;
} PCIPlxVSwitchUpstream;

#define TYPE_PLX_VSWITCH_DOWNSTREAM_PCI "plx-vswitch-downstream-pci"
OBJECT_DECLARE_SIMPLE_TYPE(PCIPlxVSwitchDownstream, PLX_VSWITCH_DOWNSTREAM_PCI)
typedef struct PCIPlxVSwitchDownstream {
    PCIESlot parent;

    /* PCI config properties */
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t subsystem_vendor_id;
    uint16_t subsystem_device_id;
    uint32_t class_revision;

    /* MSI properties */
    uint8_t msi_offset;
    int32_t msi_vector_count;
    bool msi64bit;
    bool msi_per_vector_mask;

    /* PCI Config offsets */
    uint8_t ssvid_offset;
    uint8_t cap_offset;
    uint16_t aer_offset;
    uint16_t acs_offset;
} PCIPlxVSwitchDownstream;

#endif

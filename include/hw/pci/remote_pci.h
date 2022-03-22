/*
 * Remote PCI Device
 *
 * Copyright 2026 Google LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef REMOTE_PCI_H_
#define REMOTE_PCI_H_

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "chardev/char-fe.h"
#include "hw/pci/msi.h"
#include "hw/pci/msix.h"
#include "hw/pci/pci.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "hw/core/registerfields.h"
#include "qapi/qmp/qerror.h"
#include "hw/pci/pci_regs.h"
#include "qemu/error-report.h"

#define REMOTE_PCI_BUF_SIZE 256

#define REMOTE_PCI_RESP_OK              0
#define REMOTE_PCI_RESP_PRECOND         9
#define REMOTE_PCI_RESP_INTERNAL        13

#define REMOTE_PCI_READ_DATA            1
#define REMOTE_PCI_WRITE_DATA           2
#define REMOTE_PCI_DMA_READ             3
#define REMOTE_PCI_DMA_WRITE            4
#define REMOTE_PCI_MSI                  5
#define REMOTE_PCI_CFG_READ             6
#define REMOTE_PCI_CFG_WRITE            7
#define REMOTE_PCI_RESPONSE_MASK        8

#define REMOTE_PCI_DMA_REQ_LEN          0x11

#define TYPE_REMOTE_PCI "remote-pci"

OBJECT_DECLARE_SIMPLE_TYPE(RemotePCI, REMOTE_PCI)

#define REMOTE_PCI_MAX_BUF_SIZE         20

#define REMOTE_PCI_MAX_NUM_VFS          16

typedef struct RemotePCI {
    PCIDevice parent;

    /* PCI config properties */
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t vf_device_id;
    uint16_t subsystem_vendor_id;
    uint16_t subsystem_device_id;
    uint16_t vf_subsystem_device_id;
    uint32_t class_revision;
    /* Whether this is a shim device that does not connect to a chardev.*/
    bool shim_device;
    /* Read/write config from remote device instead of QEMU internal space. */
    bool external_cfg;
    uint64_t bar_size[PCI_NUM_REGIONS];
    bool bar_io[PCI_NUM_REGIONS];

    /* MSI properties */
    uint8_t msi_offset;
    int32_t msi_vector_count;
    bool msi64bit;
    bool msi_per_vector_mask;

    /* MSIX properties */
    uint8_t msix_offset;
    int32_t msix_vector_count;
    int32_t msix_table_bar_number;
    uint32_t msix_table_bar_offset;
    int32_t msix_pba_bar_number;
    uint32_t msix_pba_bar_offset;

    /* Chardev Information */
    /* Req is for messages where QEMU is the one sending the request */
    /* i.e. PCIe Config or PCIe BAR reads and writes */
    CharFrontend chr_be_req;
    CharFrontend *chr_be_req_ptr;
    /* Resp is for messages where QEMU is the one sending the response */
    /* i.e. DMA reads and writes, and MSI/MSIX Interrupts */
    CharFrontend chr_be_resp;
    CharFrontend *chr_be_resp_ptr;
    MemoryRegion bar_mmio[PCI_NUM_REGIONS];

    uint8_t endpoint_cap_offset;
    uint16_t aer_offset;
    uint16_t aer_size;
    uint16_t ari_offset;
    uint16_t acs_offset;

    bool sriov_enable;
    uint16_t num_vfs;
    CharFrontend vf_chardevs_req[REMOTE_PCI_MAX_NUM_VFS];
    CharFrontend vf_chardevs_resp[REMOTE_PCI_MAX_NUM_VFS];
    uint16_t vf_sriov_offset;
    uint16_t vf_offset;
    uint16_t vf_stride;
    bool vf_bar_io;
    uint64_t vf_bar_size;
    int32_t vf_msi_vector_count;
    int32_t vf_msix_vector_count;
    uint32_t vf_msix_table_bar_offset;
    uint32_t vf_msix_pba_bar_offset;

    uint8_t buf[REMOTE_PCI_MAX_BUF_SIZE];
    uint32_t buf_pos;
} RemotePCI;

typedef struct RemotePCICfgRequestHead {
    uint8_t code;
    uint64_t addr;
    uint8_t size;
} __attribute__ ((__packed__)) RemotePCICfgRequestHead;

typedef struct RemotePCIBarRequestHead {
    uint8_t code;
    uint8_t bar_no;
    uint64_t offset;
    uint8_t size;
} __attribute__ ((__packed__)) RemotePCIBarRequestHead;


#define DEFINE_PROP_BAR(N) \
    DEFINE_PROP_SIZE("bar-size[" #N "]", RemotePCI, bar_size[N], 0), \
    DEFINE_PROP_BOOL("bar-io[" #N "]", RemotePCI, bar_io[N], false)

#define DEFINE_PROP_VF(N) \
    DEFINE_PROP_CHR("vf-chardevs-req[" #N "]", RemotePCI, vf_chardevs_req[N]), \
    DEFINE_PROP_CHR("vf-chardevs-resp[" #N "]", RemotePCI, vf_chardevs_resp[N])

static const Property remote_pci_properties[] = {
    DEFINE_PROP_UINT16("vendor-id", RemotePCI, vendor_id, 0xffff),
    DEFINE_PROP_UINT16("device-id", RemotePCI, device_id, 0xffff),
    DEFINE_PROP_UINT16("subsystem-vendor-id", RemotePCI,
                       subsystem_vendor_id, 0),
    DEFINE_PROP_UINT16("subsystem-device-id", RemotePCI,
                       subsystem_device_id, 0),
    DEFINE_PROP_UINT32("class-revision", RemotePCI, class_revision,
                       0xff000000 /* Unknown class */),
    DEFINE_PROP_BOOL("shim-device", RemotePCI, shim_device, false),
    DEFINE_PROP_BOOL("use-external-cfg", RemotePCI, external_cfg, false),
    DEFINE_PROP_CHR("chardev-req", RemotePCI, chr_be_req),
    DEFINE_PROP_CHR("chardev-resp", RemotePCI, chr_be_resp),
    DEFINE_PROP_UINT8("msi-offset", RemotePCI, msi_offset, 0xe0),
    DEFINE_PROP_INT32("msi-vector-count", RemotePCI, msi_vector_count, 0),
    DEFINE_PROP_BOOL("msi64bit", RemotePCI, msi64bit, true),
    DEFINE_PROP_BOOL("msi-per-vector-mask", RemotePCI, msi_per_vector_mask,
                    true),
    DEFINE_PROP_UINT8("msix-offset", RemotePCI, msix_offset, 0x40),
    DEFINE_PROP_INT32("msix-vector-count", RemotePCI, msix_vector_count, 0),
    DEFINE_PROP_INT32("msix-table-bar-number", RemotePCI,
                      msix_table_bar_number, 0),
    DEFINE_PROP_UINT32("msix-table-bar-offset", RemotePCI,
                       msix_table_bar_offset, 0),
    DEFINE_PROP_INT32("msix-pba-bar-number", RemotePCI,
                      msix_pba_bar_number, 0),
    DEFINE_PROP_UINT32("msix-pba-bar-offset", RemotePCI,
                       msix_pba_bar_offset, 0),
    DEFINE_PROP_BAR(0),
    DEFINE_PROP_BAR(1),
    DEFINE_PROP_BAR(2),
    DEFINE_PROP_BAR(3),
    DEFINE_PROP_BAR(4),
    DEFINE_PROP_BAR(5),
    DEFINE_PROP_BAR(6),
    DEFINE_PROP_UINT16("vf-device-id", RemotePCI, vf_device_id, 0xffff),
    DEFINE_PROP_UINT16("vf-subsystem-device-id", RemotePCI,
                    vf_subsystem_device_id, 0xffff),
    DEFINE_PROP_UINT16("vf-sriov-offset", RemotePCI, vf_sriov_offset, 0x1e0),
    DEFINE_PROP_UINT16("vf-offset", RemotePCI, vf_offset, 0xffff),
    DEFINE_PROP_UINT16("vf-stride", RemotePCI, vf_stride, 0xffff),
    DEFINE_PROP_BOOL("vf-bar-io", RemotePCI, vf_bar_io, false),
    DEFINE_PROP_SIZE("vf-bar-size", RemotePCI, vf_bar_size, 0),
    DEFINE_PROP_INT32("vf-msi-vector-count", RemotePCI,
                    vf_msi_vector_count, 0),
    DEFINE_PROP_INT32("vf-msix-vector-count", RemotePCI,
                    vf_msix_vector_count, 0),
    DEFINE_PROP_UINT32("vf-msix-table-bar-offset", RemotePCI,
                    vf_msix_table_bar_offset, 0),
    DEFINE_PROP_UINT32("vf-msix-pba-bar-offset", RemotePCI,
                    vf_msix_pba_bar_offset, 0),
    DEFINE_PROP_UINT8("endpoint-cap-offset", RemotePCI, endpoint_cap_offset,
                    0xa0),
    DEFINE_PROP_UINT16("aer-offset", RemotePCI, aer_offset, 0x100),
    DEFINE_PROP_UINT16("aer-size", RemotePCI, aer_size, 0x44),
    DEFINE_PROP_UINT16("ari-offset", RemotePCI, ari_offset, 0x174),
    DEFINE_PROP_UINT16("acs-offset", RemotePCI, acs_offset, 0x2bc),
    DEFINE_PROP_BOOL("enable-sriov", RemotePCI, sriov_enable, false),
    DEFINE_PROP_UINT16("num-vfs", RemotePCI, num_vfs, 0),
    DEFINE_PROP_VF(0),
    DEFINE_PROP_VF(1),
    DEFINE_PROP_VF(2),
    DEFINE_PROP_VF(3),
    DEFINE_PROP_VF(4),
    DEFINE_PROP_VF(5),
    DEFINE_PROP_VF(6),
    DEFINE_PROP_VF(7),
    DEFINE_PROP_VF(8),
    DEFINE_PROP_VF(9),
    DEFINE_PROP_VF(10),
    DEFINE_PROP_VF(11),
    DEFINE_PROP_VF(12),
    DEFINE_PROP_VF(13),
    DEFINE_PROP_VF(14),
    DEFINE_PROP_VF(15),
};

#endif  /* REMOTE_PCI_H_ */

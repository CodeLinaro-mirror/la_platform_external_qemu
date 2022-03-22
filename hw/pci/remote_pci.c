/*
 * Remote PCI Device
 *
 * Copyright 2022-2024 Google LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "chardev/char-fe.h"
#include "hw/pci/msi.h"
#include "hw/pci/remote_pci.h"
#include "hw/pci/msix.h"
#include "hw/pci/pci.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "hw/core/registerfields.h"
#include "qapi/qmp/qerror.h"
#include "hw/pci/pci_regs.h"
#include "trace.h"
#include "qemu/error-report.h"

static int remote_pci_get_bar_index(RemotePCI *s, MemoryRegion *mr)
{
    int index = mr - s->bar_mmio;

    g_assert(index >= 0 && index < PCI_NUM_REGIONS);
    return index;
}

static uint32_t remote_pci_read_external_config(RemotePCI *s, uint32_t addr,
                                                int size)
{
    uint8_t code = 0;
    uint32_t val = 0;
    uint8_t return_size = 0;
    RemotePCICfgRequestHead hdr = {
        .code = REMOTE_PCI_CFG_READ,
        .addr = addr,
        .size = size,
    };

    qemu_chr_fe_write_all(s->chr_be_req_ptr, (uint8_t *) &hdr, sizeof(hdr));
    /* Read back return code */
    qemu_chr_fe_read_all(s->chr_be_req_ptr, &code, sizeof(code));
    if (code != (REMOTE_PCI_RESPONSE_MASK | REMOTE_PCI_CFG_READ)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: config reading returned incorrect response "
                      "code: 0x%02" PRIx8 ", expected 0x%02" PRIx8 "\n",
                      DEVICE(s)->canonical_path, code,
                      (REMOTE_PCI_RESPONSE_MASK | REMOTE_PCI_CFG_READ));
      return 0;
    }
    qemu_chr_fe_read_all(s->chr_be_req_ptr, &code, sizeof(code));
    if (code != REMOTE_PCI_RESP_OK) {
        qemu_log_mask(
            LOG_GUEST_ERROR,
            "%s: failed to read config from external device at 0x%04" PRIx32
            ", size: %d, got error code: 0x%02" PRIx8 "\n",
            DEVICE(s)->canonical_path, addr, size, code);
        return 0;
    }
    qemu_chr_fe_read_all(s->chr_be_req_ptr, &return_size, sizeof(return_size));
    if (return_size != size) {
        qemu_log_mask(
            LOG_GUEST_ERROR,
            "%s: device returned incorrect read size: %d, expected: %d\n",
            DEVICE(s)->canonical_path, return_size, size);
        return 0;
    }
    qemu_chr_fe_read_all(s->chr_be_req_ptr, (uint8_t *) &val, size);
    trace_remote_pci_read_external_config(
        DEVICE(s)->canonical_path, addr, size, val);

    return val;
}

static void remote_pci_write_external_config(RemotePCI *s, uint32_t addr,
                                             uint32_t val, int size)
{
    uint8_t code = 0;
    RemotePCICfgRequestHead hdr = {
        .code = REMOTE_PCI_CFG_WRITE,
        .addr = addr,
        .size = size,
    };

    trace_remote_pci_write_external_config(
        DEVICE(s)->canonical_path, addr, size, val);
    qemu_chr_fe_write_all(s->chr_be_req_ptr, (uint8_t *) &hdr, sizeof(hdr));
    qemu_chr_fe_write_all(s->chr_be_req_ptr, (uint8_t *) &val, size);
    /* Read back return code */
    qemu_chr_fe_read_all(s->chr_be_req_ptr, &code, sizeof(code));
    if (code != (REMOTE_PCI_RESPONSE_MASK | REMOTE_PCI_CFG_WRITE)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: config write returned incorrect response "
                      "code: 0x%02" PRIx8 ", expected 0x%02" PRIx8 "\n",
                      DEVICE(s)->canonical_path, code,
                      (REMOTE_PCI_RESPONSE_MASK | REMOTE_PCI_CFG_WRITE));
    }
    qemu_chr_fe_read_all(s->chr_be_req_ptr, &code, sizeof(code));
    if (code != REMOTE_PCI_RESP_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: failed to write config to external device at 0x%04"
                      PRIx32 ", size: %d, got error code: 0x%02" PRIx8 "\n",
                      DEVICE(s)->canonical_path, addr, size, code);
    }
}

static uint32_t remote_pci_cfg_read(PCIDevice *dev, uint32_t addr, int size)
{
    RemotePCI *s = REMOTE_PCI(dev);

    if (s->external_cfg) {
        return remote_pci_read_external_config(s, addr, size);
    }
    return pci_default_read_config(dev, addr, size);
}

static void remote_pci_cfg_write(PCIDevice *dev, uint32_t addr,
                                 uint32_t val, int size)
{
    RemotePCI *s = REMOTE_PCI(dev);

    if (s->external_cfg) {
        remote_pci_write_external_config(s, addr, val, size);
    }
    pci_default_write_config(dev, addr, val, size);
}


static uint64_t remote_pci_bar_read(void *opaque, hwaddr offset, unsigned size)
{
    uint64_t value = 0;
    MemoryRegion *mr = opaque;
    RemotePCI *s = REMOTE_PCI(mr->owner);
    int index = remote_pci_get_bar_index(s, mr);
    uint8_t code;
    uint8_t return_size;
    RemotePCIBarRequestHead hdr = {
        .code = REMOTE_PCI_READ_DATA,
        .bar_no = index,
        .offset = offset,
        .size = size,
    };

    qemu_chr_fe_write_all(s->chr_be_req_ptr, (uint8_t *) &hdr, sizeof(hdr));
    /* Read back return code */
    qemu_chr_fe_read_all(s->chr_be_req_ptr, &code, sizeof(code));
    if (code != (REMOTE_PCI_RESPONSE_MASK | REMOTE_PCI_READ_DATA)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: device reading returned incorrect response "
                      "code: 0x%02" PRIx8 ", expected 0x%02" PRIx8 "\n",
                      DEVICE(s)->canonical_path, code,
                      (REMOTE_PCI_RESPONSE_MASK | REMOTE_PCI_READ_DATA));
        return 0;
    }
    qemu_chr_fe_read_all(s->chr_be_req_ptr, &code, sizeof(code));
    if (code != REMOTE_PCI_RESP_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: failed to read data from external device BAR[%d] "
                      "at 0x%08" PRIx64" , size: %u, got error code: 0x%02"
                      PRIx8 "\n",
                      DEVICE(s)->canonical_path, index, offset, size, code);
      return 0;
    }
    qemu_chr_fe_read_all(s->chr_be_req_ptr, &return_size, sizeof(return_size));
    if (return_size != size) {
        qemu_log_mask(LOG_GUEST_ERROR,
                        "%s: device returned incorrect read "
                        "size: %d, expected: %d\n",
                        DEVICE(s)->canonical_path, return_size, size);
        return 0;
    }
    qemu_chr_fe_read_all(s->chr_be_req_ptr, (uint8_t *) &value, size);
    trace_remote_pci_bar_read(DEVICE(s)->canonical_path,
                              offset, size, value);
    return value;
}

static void remote_pci_bar_write(void *opaque, hwaddr offset,
                                 uint64_t v, unsigned size)
{
    MemoryRegion *mr = opaque;
    RemotePCI *s = REMOTE_PCI(mr->owner);
    int index = remote_pci_get_bar_index(s, mr);
    uint8_t code;
    RemotePCIBarRequestHead hdr = {
        .code = REMOTE_PCI_WRITE_DATA,
        .bar_no = index,
        .offset = offset,
        .size = size,
    };

    trace_remote_pci_bar_write(DEVICE(s)->canonical_path,
                               offset, size, v);
    qemu_chr_fe_write_all(s->chr_be_req_ptr, (uint8_t *) &hdr, sizeof(hdr));
    qemu_chr_fe_write_all(s->chr_be_req_ptr, (uint8_t *) &v, size);
    /* Read back return code */
    qemu_chr_fe_read_all(s->chr_be_req_ptr, &code, sizeof(code));
    if (code != (REMOTE_PCI_RESPONSE_MASK | REMOTE_PCI_WRITE_DATA)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                        "%s: device writing returned incorrect response "
                        "code: 0x%02" PRIx8 ", expected 0x%02" PRIx8 "\n",
                        DEVICE(s)->canonical_path, code,
                        (REMOTE_PCI_RESPONSE_MASK | REMOTE_PCI_WRITE_DATA));
    }
    qemu_chr_fe_read_all(s->chr_be_req_ptr, &code, sizeof(code));
    if (code != REMOTE_PCI_RESP_OK) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: failed to write data from "
            "external device BAR[%d] at 0x%08" PRIx64 ", size: %u, val: 0x%08"
            PRIx64 ", got error code: 0x%02" PRIx8 "\n",
            DEVICE(s)->canonical_path, index, offset, size, v, code);
    }
}

static void remote_pci_dma_read(RemotePCI *s)
{
    PCIDevice *pdev = PCI_DEVICE(s);
    uint64_t addr, size;
    MemTxResult res;
    g_autofree uint8_t *buf;
    uint8_t return_code = REMOTE_PCI_DMA_READ | REMOTE_PCI_RESPONSE_MASK;
    uint8_t error_code = REMOTE_PCI_RESP_OK;

    g_assert(s->buf_pos == REMOTE_PCI_DMA_REQ_LEN);
    addr = *(uint64_t *)&s->buf[1];
    size = *(uint64_t *)&s->buf[9];
    trace_remote_pci_dma_read(DEVICE(s)->canonical_path, addr, size);
    buf = g_malloc(size);
    res = pci_dma_read(pdev, addr, buf, size);
    if (res != MEMTX_OK) {
        error_code = REMOTE_PCI_RESP_INTERNAL;
        size = 0;
    }
    qemu_chr_fe_write_all(s->chr_be_resp_ptr, &return_code,
                          sizeof(return_code));
    qemu_chr_fe_write_all(s->chr_be_resp_ptr, &error_code, sizeof(error_code));
    qemu_chr_fe_write_all(s->chr_be_resp_ptr, (uint8_t *)&size, sizeof(size));
    if (size > 0) {
        qemu_chr_fe_write_all(s->chr_be_resp_ptr, buf, size);
    }
}

static void remote_pci_dma_write(RemotePCI *s)
{
    PCIDevice *pdev = PCI_DEVICE(s);
    uint64_t addr, size;
    MemTxResult res;
    uint8_t *buf;
    uint8_t return_code = REMOTE_PCI_DMA_WRITE | REMOTE_PCI_RESPONSE_MASK;
    uint8_t error_code = REMOTE_PCI_RESP_OK;

    g_assert(s->buf_pos >= REMOTE_PCI_DMA_REQ_LEN);
    addr = *(uint64_t *)&s->buf[1];
    size = *(uint64_t *)&s->buf[9];
    trace_remote_pci_dma_write(DEVICE(s)->canonical_path, addr,
                               size);
    g_assert(s->buf_pos == REMOTE_PCI_DMA_REQ_LEN + size);
    buf = &s->buf[REMOTE_PCI_DMA_REQ_LEN];
    res = pci_dma_write(pdev, addr, buf, size);
    if (res != MEMTX_OK) {
        error_code = REMOTE_PCI_RESP_INTERNAL;
    }
    qemu_chr_fe_write_all(s->chr_be_resp_ptr, &return_code,
                          sizeof(return_code));
    qemu_chr_fe_write_all(s->chr_be_resp_ptr, &error_code, sizeof(error_code));
}

static void remote_pci_send_msix(RemotePCI *s)
{
    PCIDevice *pdev = PCI_DEVICE(s);
    uint32_t *vector_no;
    uint8_t return_code = REMOTE_PCI_MSI | REMOTE_PCI_RESPONSE_MASK;
    uint8_t error_code = REMOTE_PCI_RESP_OK;

    g_assert(s->buf_pos == 5);
    vector_no = (uint32_t *)(&s->buf[1]);
    if (msix_enabled(pdev)) {
        msix_notify(pdev, *vector_no);
    } else if (msi_enabled(pdev)) {
        msi_notify(pdev, *vector_no);
    } else {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: MSIx not supported.\n",
                      DEVICE(s)->canonical_path);
        error_code = REMOTE_PCI_RESP_PRECOND;
    }
    qemu_chr_fe_write_all(s->chr_be_resp_ptr, &return_code,
                          sizeof(return_code));
    qemu_chr_fe_write_all(s->chr_be_resp_ptr, &error_code, sizeof(error_code));
    qemu_chr_fe_write_all(s->chr_be_resp_ptr, (uint8_t *)vector_no,
                          sizeof(vector_no));
}

static void remote_pci_receive_char(RemotePCI *s, uint8_t c)
{
    if (s->buf_pos == 0) {
        /* This is OP code */
        switch (c) {
        case REMOTE_PCI_DMA_READ:
        case REMOTE_PCI_DMA_WRITE:
        case REMOTE_PCI_MSI:
            s->buf[s->buf_pos++] = c;
            return;
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "invalid op code: 0x%02" PRIx8 "\n", c);
            return;
        }
    }

    switch (s->buf[0]) {
    case REMOTE_PCI_DMA_READ:
        /* 8 bytes addr, 8 bytes size. */
        s->buf[s->buf_pos++] = c;
        if (s->buf_pos == REMOTE_PCI_DMA_REQ_LEN) {
            remote_pci_dma_read(s);
        }
        break;

    case REMOTE_PCI_DMA_WRITE:
        /* 8 bytes addr, 8 bytes size, X bytes data. */
        s->buf[s->buf_pos++] = c;
        if (s->buf_pos == REMOTE_PCI_DMA_REQ_LEN + *(uint64_t *)&s->buf[9]) {
            remote_pci_dma_write(s);
        }
        break;

    case REMOTE_PCI_MSI:
        /* we need 4 bytes vector number. */
        s->buf[s->buf_pos++] = c;
        if (s->buf_pos == 5) {
            remote_pci_send_msix(s);
            s->buf_pos = 0;
        }
        break;

    default:
        /* This should not happen, we already ignored all invalid op codes. */
        g_assert_not_reached();
    }
}

static int remote_pci_can_receive(void *opaque)
{
    RemotePCI *s = opaque;

    return REMOTE_PCI_MAX_BUF_SIZE - s->buf_pos;
}

static void remote_pci_receive(void *opaque, const uint8_t *buf, int size)
{
    RemotePCI *s = opaque;
    int i;

    for (i = 0; i < size; i++) {
        remote_pci_receive_char(s, buf[i]);
    }
}

static int remote_pci_msi_init(RemotePCI *s, Error **errp)
{
    PCIDevice *p = PCI_DEVICE(s);

    return msi_init(p, s->msi_offset, s->msi_vector_count, s->msi64bit,
             s->msi_per_vector_mask, errp);
}

static int remote_pci_msix_init(RemotePCI *s, Error **errp)
{
    PCIDevice *p = PCI_DEVICE(s);
    int i, ret;

    if (s->msix_table_bar_number >= PCI_NUM_REGIONS) {
        error_setg(errp, "MSIX table bar number too large");
        return -1;
    }
    if (s->msix_pba_bar_number >= PCI_NUM_REGIONS) {
        error_setg(errp, "MSIX pba bar number too large");
        return -1;
    }
    ret = msix_init(p, s->msix_vector_count,
                    &s->bar_mmio[s->msix_table_bar_number],
                    s->msix_table_bar_number, s->msix_table_bar_offset,
                    &s->bar_mmio[s->msix_pba_bar_number],
                    s->msix_pba_bar_number, s->msix_pba_bar_offset,
                    s->msix_offset, errp);
    if (ret < 0) {
        return ret;
    }
    for (i = 0; i < s->msix_vector_count; i++) {
        msix_vector_use(p, i);
    }
    return 0;
}

static const struct MemoryRegionOps remote_pci_bar_mmio_ops = {
    .read       = remote_pci_bar_read,
    .write      = remote_pci_bar_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid      = {
        .min_access_size        = 1,
        .max_access_size        = 8,
    },
};

static int remote_pci_init_sriov(PCIDevice *p, Error **errp)
{
    RemotePCI *s = REMOTE_PCI(p);

    if (pci_is_vf(p)) {
        error_setg(errp, "Virtual Functions cannot have SRIOV initialized.");
        return -1;
    }
    p->cap_present |= QEMU_PCI_CAP_MULTIFUNCTION;
    pcie_acs_init(p, s->acs_offset);

    if (s->num_vfs <= 0) {
        error_setg(errp, "if SRIOV is enabled, the number of virtual "
            "functions needs to be > 0 for the remote-pci device");
        return -1;
    }

    if (s->num_vfs > 16) {
        error_setg(errp, "remote-pci devices only supportup to 16 "
        "virtual functions");
        return -1;
    }

    if (s->vf_device_id == 0xffff) {
        error_setg(errp, "Virtual Function Device ID invalid, it must "
        "always be supplied if virtual functions are enabled");
        return -1;
    }
    if (s->vf_sriov_offset == 0xffff) {
        error_setg(errp, "Virtual Function SRIOV offset invalid, it "
        "must always be supplied if virtual functions are enabled");
        return -1;
    }
    if (s->vf_offset == 0xffff) {
        error_setg(errp, "Virtual Function Offset invalid, it must "
        "always be supplied if virtual functions are enabled");
        return -1;
    }
    if (s->vf_stride == 0xffff) {
        error_setg(errp, "Virtual Function Stride invalid, it must "
        "always be supplied if virtual functions are enabled");
        return -1;
    }

    pcie_sriov_pf_init(p, s->vf_sriov_offset, TYPE_REMOTE_PCI,
                        s->vf_device_id, s->num_vfs,
                        s->num_vfs, s->vf_offset, s->vf_stride, errp);
    /* VF BAR is initialized here  */
    pcie_sriov_pf_init_vf_bar(p, 0, s->vf_bar_io ? PCI_BASE_ADDRESS_SPACE_IO
                              : (PCI_BASE_ADDRESS_MEM_TYPE_64 |
                                PCI_BASE_ADDRESS_MEM_PREFETCH),
                                s->vf_bar_size * s->num_vfs);
    return 0;
}

static void remote_pci_init_vf_parameters(PCIDevice *p, Error **errp)
{
    RemotePCI *s = REMOTE_PCI(p);

    if (!pci_is_vf(p)) {
        error_setg(errp, "Initializing VF parameters for non-VF device");
        return;
    }

    RemotePCI *ps = REMOTE_PCI(pcie_sriov_get_pf(p));

    /* Since we know it's a VF, we can drag the necessary parameters down */
    memcpy(&s->vendor_id, &ps->vendor_id, sizeof(uint16_t));
    memcpy(&s->device_id, &ps->vf_device_id, sizeof(uint16_t));
    memcpy(&s->subsystem_vendor_id, &ps->subsystem_vendor_id,
            sizeof(uint16_t));
    memcpy(&s->subsystem_device_id, &ps->vf_subsystem_device_id,
            sizeof(uint16_t));
    memcpy(&s->class_revision, &ps->class_revision, sizeof(uint32_t));

    memcpy(&s->endpoint_cap_offset, &ps->endpoint_cap_offset,
            sizeof(uint8_t));
    memcpy(&s->aer_offset, &ps->aer_offset, sizeof(uint16_t));
    memcpy(&s->aer_size, &ps->aer_size, sizeof(uint16_t));
    memcpy(&s->ari_offset, &ps->ari_offset, sizeof(uint16_t));
    memcpy(&s->acs_offset, &ps->acs_offset, sizeof(uint16_t));

    memcpy(&s->bar_size[0], &ps->vf_bar_size, sizeof(uint64_t));
    memcpy(&s->msi_vector_count, &ps->vf_msi_vector_count,
            sizeof(uint16_t));
    memcpy(&s->msix_vector_count, &ps->vf_msix_vector_count,
            sizeof(uint16_t));
    memcpy(&s->msix_table_bar_offset, &ps->vf_msix_table_bar_offset,
            sizeof(uint32_t));
    memcpy(&s->msix_pba_bar_offset, &ps->vf_msix_pba_bar_offset,
            sizeof(uint32_t));
    s->chr_be_req_ptr = &ps->vf_chardevs_req[pcie_sriov_vf_number(p)];
    s->chr_be_resp_ptr = &ps->vf_chardevs_resp[pcie_sriov_vf_number(p)];
}

static int remote_pci_set_chardev_handlers(PCIDevice *p, Error **errp)
{
    RemotePCI *s = REMOTE_PCI(p);

    if (!qemu_chr_fe_backend_connected(s->chr_be_req_ptr)) {
        if (pci_is_vf(p)) {
            error_setg(errp, QERR_MISSING_PARAMETER, "vf-chardev-req");
        } else {
            error_setg(errp, QERR_MISSING_PARAMETER, "chardev-req");
        }
        return -1;
    }

    /*
     * The req chardev should never have to receive separate from a response to
     * a message, which is handled in the function for that type of read/write
     * directly. Thus we can leave that as NULL, the main difference with how
     * the resp chardev is set
     */
    qemu_chr_fe_set_handlers(s->chr_be_req_ptr, remote_pci_can_receive,
        NULL, NULL, NULL, s, NULL, true);

    if (!qemu_chr_fe_backend_connected(s->chr_be_resp_ptr)) {
        if (pci_is_vf(p)) {
            error_setg(errp, QERR_MISSING_PARAMETER, "vf-chardev-resp");
        } else {
            error_setg(errp, QERR_MISSING_PARAMETER, "chardev-resp");
        }
        return -1;
    }

    qemu_chr_fe_set_handlers(s->chr_be_resp_ptr, remote_pci_can_receive,
        remote_pci_receive, NULL, NULL, s, NULL, true);

    return 0;
}

static void remote_pci_realize(PCIDevice *p, Error **errp)
{
    RemotePCI *s = REMOTE_PCI(p);
    int i;

    bool is_vf = pci_is_vf(p);
    if (is_vf) {
        remote_pci_init_vf_parameters(p, errp);
    } else {
        s->chr_be_req_ptr = &s->chr_be_req;
        s->chr_be_resp_ptr = &s->chr_be_resp;
    }

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

    int ret = pcie_endpoint_cap_init(p, s->endpoint_cap_offset);
    if (ret < 0) {
        return;
    }

    ret = pcie_aer_init(p, 1, s->aer_offset, s->aer_size, errp);
    if (ret < 0) {
        pcie_cap_exit(p);
        return;
    }

    pcie_ari_init(p, s->ari_offset);

    /* BAR Initalization */
    if (is_vf) {
        if (s->bar_size[0] > 0) {
            memory_region_init_io(&s->bar_mmio[0], OBJECT(p),
                                &remote_pci_bar_mmio_ops, &s->bar_mmio[0],
                                "bar-mmio[*]", s->bar_size[0]);
            pci_register_bar(p, 0, s->vf_bar_io ? PCI_BASE_ADDRESS_SPACE_IO
                             : (PCI_BASE_ADDRESS_MEM_TYPE_64 |
                                PCI_BASE_ADDRESS_MEM_PREFETCH),
                                &s->bar_mmio[0]);
        }
    } else {
        /* VF BAR is set through sriov later on*/
        for (i = 0; i < PCI_NUM_REGIONS; ++i) {
            if (s->bar_size[i] > 0) {
                memory_region_init_io(&s->bar_mmio[i], OBJECT(p),
                                    &remote_pci_bar_mmio_ops, &s->bar_mmio[i],
                                    "bar-mmio[*]", s->bar_size[i]);
                pci_register_bar(p, i, s->bar_io[i] ? PCI_BASE_ADDRESS_SPACE_IO
                                 : (PCI_BASE_ADDRESS_MEM_TYPE_64 |
                                    PCI_BASE_ADDRESS_MEM_PREFETCH),
                                    &s->bar_mmio[i]);
            }
        }
    }

    /*
     * the shim device boolean is used to represent a remote_pci device
     * where we don't enable jobs.
     */
    if (s->shim_device) {
        /*
         * Since we don't initialize MSI/MSIX in shim devices, we need to set
         * the vector counts to 0.
         */
        s->msi_vector_count = 0;
        s->msix_vector_count = 0;
        return;
    }

    ret = remote_pci_set_chardev_handlers(p, errp);
    if (ret < 0) {
        error_setg(errp, "Failed initializing chardevs");
        return;
    }

    if (s->sriov_enable) {
        ret = remote_pci_init_sriov(p, errp);
        if (ret < 0) {
            error_setg(errp, "Failed initializing SRIOV");
            return;
        }
    }
    if (s->msi_vector_count > 0) {
        ret = remote_pci_msi_init(s, errp);
        if (ret < 0) {
            error_setg(errp, "Failed initializing MSI");
            return;
        }
    }
    if (s->msix_vector_count > 0) {
        ret = remote_pci_msix_init(s, errp);
        if (ret < 0) {
            error_setg(errp, "Failed initializing MSIX");
            return;
        }
    }
}

static void remote_pci_reset(DeviceState *d)
{
    RemotePCI *s = REMOTE_PCI(d);
    PCIDevice *p = PCI_DEVICE(s);
    int i;

    s->buf_pos = 0;
    for (i = 0; i < s->msix_vector_count; i++) {
        msix_vector_use(p, i);
    }
}

static void remote_pci_exit(PCIDevice *p)
{
    RemotePCI *s = REMOTE_PCI(p);
    int i;

    for (i = 0; i < s->msix_vector_count; i++) {
        msix_vector_unuse(p, i);
    }
    if (s->msix_vector_count > 0) {
        msix_uninit(p, &s->bar_mmio[s->msix_table_bar_number],
                    &s->bar_mmio[s->msix_pba_bar_number]);
    }
    pcie_cap_exit(p);

    if (!pci_is_vf(p) && s->sriov_enable) {
        pcie_sriov_pf_exit(p);
    }
}

static const VMStateDescription vmstate_remote_pci = {
    .name = "remote_pci",
    .version_id = 0,
    .minimum_version_id = 0,
    .fields = (VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent, RemotePCI),
        VMSTATE_BUFFER(buf, RemotePCI),
        VMSTATE_UINT32(buf_pos, RemotePCI),
        VMSTATE_END_OF_LIST()
    }
};

static void remote_pci_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pdc = PCI_DEVICE_CLASS(klass);

    /* Ensure BAR properties do not exceed maximum allowed amount in QEMU */
    QEMU_BUILD_BUG_ON(PCI_NUM_REGIONS < 7);
    dc->vmsd = &vmstate_remote_pci;
    device_class_set_legacy_reset(dc, remote_pci_reset);
    device_class_set_props(dc, remote_pci_properties);
    pdc->config_read = remote_pci_cfg_read;
    pdc->config_write = remote_pci_cfg_write;
    pdc->realize = remote_pci_realize;
    pdc->exit = remote_pci_exit;

    dc->desc = "Remote PCI Device";
}

static const TypeInfo remote_pci_types[] = {
    {
        .name = TYPE_REMOTE_PCI,
        .parent = TYPE_PCI_DEVICE,
        .instance_size = sizeof(RemotePCI),
        .class_init = remote_pci_class_init,
        .interfaces = (InterfaceInfo[]) {
            { INTERFACE_PCIE_DEVICE },
            { }
        }
    },
};
DEFINE_TYPES(remote_pci_types)

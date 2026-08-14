/*
 * MIPI HCI I3C controller DMA functionality
 *
 * Copyright (C) 2025 Google, LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "hw/i3c/mipi-hci.h"
#include "hw/i3c/hci-dma.h"
#include "hci-dma-internal.h"
#include "hw/core/registerfields.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "hci-cmd.h"
#include "exec/cpu-common.h"
#include "hw/i3c/hci-ibi.h"
#include "trace.h"
#include "hw/i3c/i3c.h"
#include "hw/core/irq.h"
#include "hci-dma-internal.h"
#include "hci-core-internal.h"

#define INC_AND_ROLLOVER(x, max) \
    do {                         \
        x++;                     \
        if (x >= max) {          \
            x = 0;               \
        }                        \
    } while (0)

#define IBI_CHUNK_SIZE(x) (4 << (x))

static const uint32_t hci_dma_header_ro_mask[] = {
    [R_RHS_CONTROL] = 0xfffffff0,
    [R_RH0_OFFSET]  = 0xffffffff,
};

static const uint32_t hci_dma_ro_mask[HCI_DMA_NUM_REGS] = {
    [R_CR_SETUP]              = 0xffffff00,
    [R_IBI_SETUP]             = 0xff00e000,
    [R_RH_INTR_STATUS]        = 0xffffe180,
    [R_RH_INTR_STATUS_ENABLE] = 0xffffe180,
    [R_RH_INTR_SIGNAL_ENABLE] = 0xffffe180,
    [R_RH_INTR_FORCE]         = 0xffffe180,
    [R_RH_STATUS]             = 0xffffffff,
    [R_RH_CONTROL]            = 0xfffffff8,
    [R_RH_OPERATION1]         = 0xff000000,
    [R_RH_OPERATION2]         = 0xffffffff,
};

void hci_dma_reset(HCIDMAState *s)
{
    /* Header */
    s->header_regs[R_RHS_CONTROL] = 0x02050010;
    for (int i = 0; i < s->cfg.num_ring_offsets; ++i) {
        s->header_regs[R_RH0_OFFSET + i] = s->cfg.ring_offsets[i];
    }
    /* Ring status */
    memset(s->regs, 0, sizeof(s->regs));
    ARRAY_FIELD_DP32(s->regs, RHS_CONTROL, PREAMBLE_SIZE, s->cfg.preamble_size);
    ARRAY_FIELD_DP32(s->regs, RHS_CONTROL, HEADER_SIZE, s->cfg.header_size);
    ARRAY_FIELD_DP32(s->regs, CR_SETUP, XFER_STRUCT_SIZE,
                     s->cfg.xfer_struct_size);
    ARRAY_FIELD_DP32(s->regs, CR_SETUP, RESP_STRUCT_SIZE,
                     s->cfg.resp_struct_size);
    ARRAY_FIELD_DP32(s->regs, IBI_SETUP, IBI_STATUS_STRUCT_SIZE,
                     s->cfg.ibi_status_struct_size);

    /* State. */
    s->ibi_chunks_stored = 0;
}

uint64_t hci_dma_header_read(void *opaque, hwaddr offset, unsigned size)
{
    HCIDMAState *s = &(MIPI_HCI(opaque)->dma);

    trace_hci_dma_header_read(DEVICE(opaque)->canonical_path, offset,
                              s->header_regs[offset / 4]);

    offset /= sizeof(*s->header_regs);

    /* MMIO region size should prevent this from happening. */
    g_assert(offset < ARRAY_SIZE(s->header_regs));

    return s->header_regs[offset];
}

void hci_dma_header_write(void *opaque, hwaddr offset, uint64_t value,
                          unsigned size)
{
    HCIDMAState *s = &(MIPI_HCI(opaque)->dma);

    trace_hci_dma_header_write(DEVICE(opaque)->canonical_path, offset, value);

    offset /= sizeof(*s->header_regs);

    /* MMIO region size should prevent this from happening. */
    g_assert(offset < ARRAY_SIZE(s->header_regs));

    value &= ~hci_dma_header_ro_mask[offset];
    s->header_regs[offset] = value;
}

uint64_t hci_dma_read(void *opaque, hwaddr offset, unsigned size)
{
    HCIDMAState *s = &(MIPI_HCI(opaque)->dma);

    trace_hci_dma_read(DEVICE(opaque)->canonical_path, offset,
                       s->regs[offset / 4]);

    offset /= sizeof(*s->regs);

    /* MMIO region size should prevent this from happening. */
    g_assert(offset < ARRAY_SIZE(s->regs));

    return s->regs[offset];
}

static bool hci_dma_ring_empty(HCIDMAState *s)
{
    return ARRAY_FIELD_EX32(s->regs, RH_OPERATION1, CR_ENQ_PTR) ==
           ARRAY_FIELD_EX32(s->regs, RH_OPERATION2, CR_DEQ_PTR);
}

static bool hci_dma_can_xfer(HCIDMAState *s)
{
    return ARRAY_FIELD_EX32(s->regs, RH_STATUS, RING_ENABLED) &&
           ARRAY_FIELD_EX32(s->regs, RH_STATUS, RING_RUNNING) &&
           !ARRAY_FIELD_EX32(s->regs, RH_STATUS, RING_ABORTED);
}

static bool hci_dma_ring_ok(HCIDMAState *s)
{
    bool ok = !hci_dma_ring_empty(s) &&
            (ARRAY_FIELD_EX32(s->regs, CR_SETUP, RING_SIZE) >= 2);

    if (!ok) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Attempted to start DMA transfer "
                      "when the ring wasn't set up correctly.", __func__);
    }
    return ok;
}

static void hci_dma_read_descr(MIPIHCIState *hci, TransferDescr *desc)
{
    HCIDMAState *s = &hci->dma;

    uint64_t addr = s->regs[R_RH_CMD_RING_BASE_HI];
    uint8_t dequeue_ptr = ARRAY_FIELD_EX32(s->regs, RH_OPERATION2, CR_DEQ_PTR);
    addr <<= 32;
    addr |= s->regs[R_RH_CMD_RING_BASE_LO];
    addr += (dequeue_ptr * s->cfg.xfer_struct_size);

    cpu_physical_memory_read(addr, desc, sizeof(*desc));
    trace_hci_dma_read_descr(DEVICE(hci)->canonical_path, addr,
                             desc->cmd.val64);
}

static void hci_dma_push_resp(MIPIHCIState *hci, RespDescr *resp)
{
    HCIDMAState *s = &hci->dma;

    uint64_t addr = s->regs[R_RH_RESP_RING_BASE_HI];
    /*
     * The DMA response is a pair with the command that was just handled.
     * Therefore, we use the offset of where the command was, which is the
     * dequeue pointer.
     */
    uint8_t dequeue_ptr = ARRAY_FIELD_EX32(s->regs, RH_OPERATION2, CR_DEQ_PTR);
    addr <<= 32;
    addr |= s->regs[R_RH_RESP_RING_BASE_LO];
    addr += (dequeue_ptr * s->cfg.resp_struct_size);

    cpu_physical_memory_write(addr, resp, sizeof(*resp));
    trace_hci_dma_push_resp(DEVICE(hci)->canonical_path, addr, resp->val32);
}

/* Data read from this must be freed by the caller. */
static uint8_t *hci_dma_read_memory(const DataBufferDescr *desc)
{
    uint64_t addr = desc->buffer_ptr_hi;
    addr <<= 32;
    addr |= desc->buffer_ptr_lo;

    uint8_t *data = g_new0(uint8_t, desc->block_size);
    cpu_physical_memory_read(addr, data, desc->block_size);
    return data;
}

static RespStatus hci_dma_send(MIPIHCIState *hci,
                               const TransferDescr *desc,
                               RespDescr *resp)
{
    g_autofree uint8_t *data = hci_dma_read_memory(&desc->data_buffer);

    return hci_cmd_send(hci, &desc->cmd.regular_xfer, resp, data,
                        desc->data_buffer.block_size);
}

static void hci_dma_write_memory(const DataBufferDescr *desc,
                                 const uint8_t *data, size_t len)
{
    uint64_t addr = desc->buffer_ptr_hi;
    addr <<= 32;
    addr |= desc->buffer_ptr_lo;

    cpu_physical_memory_write(addr, data, len);
}

static RespStatus hci_dma_i3c_read(MIPIHCIState *hci,
                                   const TransferDescr *desc,
                                   RespDescr *resp)
{
    g_autofree uint8_t *data = g_new0(uint8_t, desc->data_buffer.block_size);
    uint32_t num_read = 0;

    RespStatus status = hci_cmd_read(hci, &desc->cmd.regular_xfer, resp, data,
                                     desc->data_buffer.block_size, &num_read);
    if (status == RESP_STATUS_SUCCESS) {
        hci_dma_write_memory(&desc->data_buffer, data, num_read);
    }
    return status;
}

static RespStatus hci_dma_regular_xfer(MIPIHCIState *hci,
                                        const TransferDescr *desc,
                                        RespDescr *resp)
{
    if (desc->data_buffer.blp) {
        g_autofree char *path = object_get_canonical_path(OBJECT(hci));
        qemu_log_mask(LOG_UNIMP, "%s: Scatter gather DMA is not implemented.\n",
                      path);
        return RESP_STATUS_ERROR_NOT_SUPPORTED;
    }

    if (desc->cmd.regular_xfer.rnw) {
        return hci_dma_i3c_read(hci, desc, resp);
    }
    return hci_dma_send(hci, desc, resp);
}

static RespStatus hci_dma_internal_control_xfer(MIPIHCIState *hci,
                                        const InternalControl *desc,
                                        RespDescr *resp)
{
    /*
     * We don't implement this, but if we tell the guest that it succeeded,
     * everything will be fine.
     */
    g_autofree char *path = object_get_canonical_path(OBJECT(hci));
    qemu_log_mask(LOG_UNIMP, "%s: Internal control transfers are not "
                  "implemented\n", path);

    resp->resp.err = RESP_STATUS_SUCCESS;
    return RESP_STATUS_SUCCESS;
}

void hci_dma_xfer(MIPIHCIState *hci)
{
    HCIDMAState *s = &(hci->dma);
    MIPIHCIClass *c = MIPI_HCI_GET_CLASS(hci);

    if (!hci_dma_can_xfer(s) || !hci_dma_ring_ok(s) ||
        !hci_core_can_xfer(&hci->core)) {
        return;
    }

    trace_hci_dma_xfer(DEVICE(hci)->canonical_path);
    while (!hci_dma_ring_empty(s)) {
        TransferDescr desc;
        RespDescr resp;
        RespStatus status;
        hci_dma_read_descr(hci, &desc);
        bool roc = desc.cmd.shared_fields.roc;

        switch (desc.cmd.shared_fields.cmd_attr) {
        case CMD_ATTR_ADDR_ASSIGN:
            status = hci_cmd_addr_assign(hci, &desc.cmd.addr_cmd, &resp);
            break;
        case CMD_ATTR_REGULAR_XFER:
            status = hci_dma_regular_xfer(hci, &desc, &resp);
            break;
        case CMD_ATTR_IMMEDIATE_XFER:
            status = hci_cmd_immediate_xfer(hci, &desc.cmd.immediate_xfer,
                                            &resp);
            break;
        case CMD_ATTR_INTERNAL_CONTROL:
            status = hci_dma_internal_control_xfer(hci,
                                                   &desc.cmd.internal_control,
                                                   &resp);
            /*
             * Not documented, nor is it a part of the internal control data
             * structure, but the driver always expects a response to internal
             * control commands.
             */
            roc = true;
            break;
        case CMD_ATTR_COMBO_XFER:
            {
                g_autofree char *path = object_get_canonical_path(OBJECT(hci));
                qemu_log_mask(LOG_UNIMP, "%s: Unimplemented command 0x%x\n",
                              path, desc.cmd.shared_fields.cmd_attr);
            }
            status = RESP_STATUS_ERROR_NOT_SUPPORTED;
            break;
        default:
            {
                g_autofree char *path = object_get_canonical_path(OBJECT(hci));
                qemu_log_mask(LOG_UNIMP, "%s: Unknown command 0x%x\n",
                              path, desc.cmd.shared_fields.cmd_attr);
            }
            status = RESP_STATUS_ERROR_NOT_SUPPORTED;
            break;
        }

        if (status == RESP_STATUS_SUCCESS) {
            ARRAY_FIELD_DP32(s->regs, RH_INTR_STATUS, TRANSFER_COMPLETION_STAT,
                             1);
        } else {
            ARRAY_FIELD_DP32(s->regs, RH_INTR_STATUS, TRANSFER_ERR_STAT, 1);
        }
        if (status != RESP_STATUS_SUCCESS) {
            c->enter_halt(hci);
        }
        if (desc.data_buffer.ioc || status != RESP_STATUS_SUCCESS) {
            c->update_irq(hci, MIPI_HCI_IRQ_CONTEXT_DMA);
        }
        if (roc || status != RESP_STATUS_SUCCESS) {
            hci_dma_push_resp(hci, &resp);
        }

        /* Increment the ring dequeue pointer. */
        uint8_t dequeue_ptr = ARRAY_FIELD_EX32(s->regs, RH_OPERATION2,
                                               CR_DEQ_PTR);
        INC_AND_ROLLOVER(dequeue_ptr,
                         ARRAY_FIELD_EX32(s->regs, CR_SETUP, RING_SIZE));
        ARRAY_FIELD_DP32(s->regs, RH_OPERATION2, CR_DEQ_PTR, dequeue_ptr);
        trace_hci_dma_dequeue_ptr(DEVICE(hci)->canonical_path, dequeue_ptr);
    }
}

static void hci_dma_rh_control_w(MIPIHCIState *hci, uint32_t val)
{
    HCIDMAState *s = &hci->dma;
    MIPIHCIClass *c = MIPI_HCI_GET_CLASS(hci);
    uint32_t prev_val = s->regs[R_RH_CONTROL];

    s->regs[R_RH_CONTROL] = val;

    /* RH_CONTROL.ENABLED transitioning from 0 to 1 clears pointer states. */
    if (FIELD_EX32(prev_val, RH_CONTROL, RING_ENABLED) == 0 &&
        FIELD_EX32(val, RH_CONTROL, RING_ENABLED)) {
        s->regs[R_RH_OPERATION1] = 0;
        s->regs[R_RH_OPERATION2] = 0;
        s->regs[R_CHUNK_CONTROL] = 0;
    }

    /* Update RING_OP_STAT since we start running or got aborted. */
    if (FIELD_EX32(prev_val, RH_CONTROL, RING_RS) == 0 &&
        FIELD_EX32(val, RH_CONTROL, RING_RS)) {
        ARRAY_FIELD_DP32(s->regs, RH_INTR_STATUS, RING_OP_STAT, 1);
    }
    if (FIELD_EX32(prev_val, RH_CONTROL, RING_ABORT) == 0 &&
        FIELD_EX32(val, RH_CONTROL, RING_ABORT)) {
        ARRAY_FIELD_DP32(s->regs, RH_INTR_STATUS, RING_OP_STAT, 1);
    }

    ARRAY_FIELD_DP32(s->regs, RH_STATUS, RING_ENABLED,
                     FIELD_EX32(val, RH_CONTROL, RING_ENABLED));
    ARRAY_FIELD_DP32(s->regs, RH_STATUS, RING_RUNNING,
                     FIELD_EX32(val, RH_CONTROL, RING_RS));
    ARRAY_FIELD_DP32(s->regs, RH_STATUS, RING_ABORTED,
                     FIELD_EX32(val, RH_CONTROL, RING_ABORT));

    hci_dma_xfer(hci);
    c->update_irq(hci, MIPI_HCI_IRQ_CONTEXT_DMA);
}

static void hci_dma_ibi_setup_w(MIPIHCIState *hci, uint32_t val)
{
    HCIDMAState *s = &hci->dma;

    /* 0 means disabled, 1 is "illegal, do not use", 2-255 is fine. */
    if (FIELD_EX32(val, IBI_SETUP, IBI_STATUS_RING_SIZE) == 1) {
        g_autofree char *path = object_get_canonical_path(OBJECT(hci));
        qemu_log_mask(LOG_GUEST_ERROR, "%s: IBI status ring size cannot be 1.",
                      path);
    }

    s->regs[R_IBI_SETUP] = val;
}

static void hci_dma_chunk_control_w(MIPIHCIState *hci, uint32_t val)
{
    HCIDMAState *s = &hci->dma;
    /*
     * The spec says CHUNK_CONTROL must be monotonically increasing. If the
     * guest decremented it, let them do so, but warn them. From the
     * controller's PoV, this will look like the guest decided to un-free IBI
     * data chunks.
     */
    if (val < s->regs[R_CHUNK_CONTROL]) {
        g_autofree char *path = object_get_canonical_path(OBJECT(hci));
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Chunk control was decremented\n",
                      path);
        s->ibi_chunks_stored += (s->regs[R_CHUNK_CONTROL] - val);
        return;
    }

    uint32_t chunks_to_free = val - s->regs[R_CHUNK_CONTROL];
    /*
     * If they decided to tell us that they freed more chunks than were actually
     * being used, warn them about that too, but don't underflow.
     */
    if (chunks_to_free > s->ibi_chunks_stored) {
        g_autofree char *path = object_get_canonical_path(OBJECT(hci));
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Attempted to free more IBI data "
                      "chunks %d, than the amount that was actually being "
                      "used, %d\n", path, chunks_to_free, s->ibi_chunks_stored);
       chunks_to_free = s->ibi_chunks_stored;
    }

    s->ibi_chunks_stored -= chunks_to_free;
    s->regs[R_CHUNK_CONTROL] = val;
}

static void hci_dma_rh_operation1_w(MIPIHCIState *hci, uint32_t val)
{
    HCIDMAState *s = &hci->dma;
    uint32_t prev_val = s->regs[R_RH_OPERATION1];

    s->regs[R_RH_OPERATION1] = val;

    /*
     * Attempt to transfer if the enqueue pointer was updated. A simple
     * inequality check is the most robust way to handle this, as it correctly
     * covers linear advancement and all rollover scenarios. Although this
     * would also trigger on a decrement of the pointer (a driver bug), the
     * hci_dma_xfer() function is safe and will correctly handle the ring state.
     */
    if (FIELD_EX32(prev_val, RH_OPERATION1, CR_ENQ_PTR) !=
        FIELD_EX32(val, RH_OPERATION1, CR_ENQ_PTR)) {
        hci_dma_xfer(hci);
    }
}

static void hci_dma_rh_intr_status_w(MIPIHCIState *hci, uint32_t val)
{
    MIPIHCIClass *c = MIPI_HCI_GET_CLASS(hci);

    hci->dma.regs[R_RH_INTR_STATUS] &= ~val; /* W1C */
    c->update_irq(hci, MIPI_HCI_IRQ_CONTEXT_DMA);
}

static void hci_dma_rh_intr_force_w(MIPIHCIState *hci, uint32_t val)
{
    MIPIHCIClass *c = MIPI_HCI_GET_CLASS(hci);

    /*
     * Set the interrupt status. If it's not masked, it will be cleared during
     * IRQ updating.
     */
    hci->dma.regs[R_RH_INTR_STATUS] = val;
    c->update_irq(hci, MIPI_HCI_IRQ_CONTEXT_DMA);
}

void hci_dma_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    MIPIHCIState *hci = MIPI_HCI(opaque);
    HCIDMAState *s = &hci->dma;

    trace_hci_dma_write(DEVICE(hci)->canonical_path, offset, value);

    offset /= sizeof(*s->regs);

    /* MMIO region size should prevent this from happening. */
    g_assert(offset < ARRAY_SIZE(s->regs));

    value &= ~hci_dma_ro_mask[offset];
    uint32_t val32 = (uint32_t)value;

    switch (offset) {
    case R_RH_CONTROL:
        hci_dma_rh_control_w(hci, val32);
        break;
    case R_IBI_SETUP:
        hci_dma_ibi_setup_w(hci, val32);
        break;
    case R_CHUNK_CONTROL:
        hci_dma_chunk_control_w(hci, val32);
        break;
    case R_RH_OPERATION1:
        hci_dma_rh_operation1_w(hci, val32);
        break;
    case R_RH_INTR_STATUS:
        hci_dma_rh_intr_status_w(hci, val32);
        break;
    case R_RH_INTR_FORCE:
        hci_dma_rh_intr_force_w(hci, val32);
        break;
    default:
        s->regs[offset] = val32;
        break;
    }
}

static bool hci_dma_ibi_status_ring_full(HCIDMAState *s)
{
    uint8_t num_entries = ARRAY_FIELD_EX32(s->regs, RH_OPERATION2,
                                           IBI_ENQ_PTR) -
                          ARRAY_FIELD_EX32(s->regs, RH_OPERATION1, IBI_DEQ_PTR);
    return num_entries >= ARRAY_FIELD_EX32(s->regs, IBI_SETUP,
                                          IBI_STATUS_RING_SIZE);
}

static bool hci_dma_ibi_data_ring_full(HCIDMAState *s, IbiStatus *ibi)
{
    if (ibi->num_bytes == 0) {
        return false;
    }

    uint32_t max_chunks_available = IBI_CHUNK_SIZE(
        ARRAY_FIELD_EX32(s->regs, IBI_SETUP, CHUNK_SIZE));
    return s->ibi_chunks_stored >= max_chunks_available;
}

static bool hci_dma_ibi_ring_ok(MIPIHCIState *hci, IbiStatus *ibi)
{
    HCIDMAState *s = &hci->dma;

    if (ARRAY_FIELD_EX32(s->regs, IBI_SETUP, IBI_STATUS_RING_SIZE) < 2) {
        g_autofree char *path = object_get_canonical_path(OBJECT(hci));
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Received an IBI when the IBI ring "
                      "was disabled.\n", path);
        return false;
    }
    if (ARRAY_FIELD_EX32(s->regs, IBI_SETUP, IBI_STATUS_RING_SIZE) < 2) {
        g_autofree char *path = object_get_canonical_path(OBJECT(hci));
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Received an IBI when the IBI ring "
                      "was disabled.\n", path);
        return false;
    }
    if (hci_dma_ibi_status_ring_full(s)) {
        g_autofree char *path = object_get_canonical_path(OBJECT(hci));
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Received an IBI when the IBI "
                      "status ring was full.\n", path);
        ARRAY_FIELD_DP32(s->regs, RH_INTR_STATUS, IBI_RING_FULL_STAT, 1);
        return false;
    }
    if (hci_dma_ibi_data_ring_full(s, ibi)) {
        g_autofree char *path = object_get_canonical_path(OBJECT(hci));
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Received an IBI when the IBI data "
                 "ring was full.\n", path);
        ARRAY_FIELD_DP32(s->regs, RH_INTR_STATUS, IBI_RING_FULL_STAT, 1);
        return false;
    }

    return true;
}

static uint32_t hci_dma_ibi_num_chunks(HCIDMAState *s, const IbiStatus *ibi)
{
    if (ibi->num_bytes == 0) {
        return 0;
    }

    uint32_t num_chunks = (ibi->num_bytes /
        IBI_CHUNK_SIZE(ARRAY_FIELD_EX32(s->regs, IBI_SETUP, CHUNK_SIZE)));
    /* We need to allocate 1 more chunk for partial data, if present. */
    if ((ibi->num_bytes % IBI_CHUNK_SIZE(
        ARRAY_FIELD_EX32(s->regs, IBI_SETUP, CHUNK_SIZE))) == 0) {
        num_chunks++;
    }

    return num_chunks;
}

static void hci_dma_ibi_status_write(MIPIHCIState *hci, IbiStatus *ibi)
{
    HCIDMAState *s = &hci->dma;

    uint64_t addr = s->regs[R_RH_IBI_STATUS_RING_BASE_HI];
    addr <<= 32;
    addr |= s->regs[R_RH_IBI_STATUS_RING_BASE_LO];
    addr += (ARRAY_FIELD_EX32(s->regs, RH_OPERATION2, IBI_ENQ_PTR) *
             s->cfg.ibi_status_struct_size);

    ibi->ibi.chunks = hci_dma_ibi_num_chunks(s, ibi);
    /* Number of bytes in the last chunk. */
    if (ibi->ibi.chunks) {
        ibi->ibi.data_length = ibi->num_bytes %
            IBI_CHUNK_SIZE(ARRAY_FIELD_EX32(s->regs, IBI_SETUP, CHUNK_SIZE));
    }

    /* Write out the data and move the queue pointer. */
    cpu_physical_memory_write(addr, &ibi->ibi, sizeof(ibi->ibi));
    uint8_t enqueue_ptr = ARRAY_FIELD_EX32(s->regs, RH_OPERATION2, IBI_ENQ_PTR);
    INC_AND_ROLLOVER(enqueue_ptr, ARRAY_FIELD_EX32(s->regs, IBI_SETUP,
                                                   IBI_STATUS_RING_SIZE));
    ARRAY_FIELD_DP32(s->regs, RH_OPERATION2, IBI_ENQ_PTR, enqueue_ptr);

    ARRAY_FIELD_DP32(s->regs, RH_INTR_STATUS, IBI_READY_STAT, 1);

    trace_hci_dma_ibi_status_write(DEVICE(hci)->canonical_path, addr,
                                   *(uint32_t *)&ibi->ibi);
}

static void hci_dma_ibi_data_write(MIPIHCIState *hci, IbiStatus *ibi)
{
    HCIDMAState *s = &hci->dma;
    if (ibi->num_bytes == 0) {
        return;
    }

    uint64_t addr = s->regs[R_RH_IBI_DATA_RING_BASE_HI];
    addr <<= 32;
    addr |= s->regs[R_RH_IBI_DATA_RING_BASE_LO];

    /*
     * CHUNK_CONTROL is an incrementing number that the driver sets to tell us
     * where it stopped reading data. Since it won't roll over, it's up to us
     * to do the rollover.
     */
    uint32_t chunk_offset = s->regs[R_CHUNK_CONTROL] %
            ARRAY_FIELD_EX32(s->regs, IBI_SETUP, CHUNK_COUNT);
    chunk_offset *= IBI_CHUNK_SIZE(ARRAY_FIELD_EX32(s->regs, IBI_SETUP,
                                                     CHUNK_SIZE));
    addr += chunk_offset;

    /*
     * Write out the IBI data. No need to update the IRQ since it was already
     * updated when writing the status.
     */
    cpu_physical_memory_write(addr, ibi->data, ibi->num_bytes);
    trace_hci_dma_ibi_data_write(DEVICE(hci)->canonical_path, addr,
                                 ibi->num_bytes);
}

int hci_dma_report_ibi(MIPIHCIState *hci)
{
    int ret = 0;
    MIPIHCIClass *c = MIPI_HCI_GET_CLASS(hci);

    g_assert(hci->ibi_in_progress != NULL);

    if (!hci_dma_ibi_ring_ok(hci, hci->ibi_in_progress)) {
        ret = -1;
    } else {
        hci_dma_ibi_status_write(hci, hci->ibi_in_progress);
        hci_dma_ibi_data_write(hci, hci->ibi_in_progress);
    }
    c->update_irq(hci, MIPI_HCI_IRQ_CONTEXT_DMA);

    return ret;
}

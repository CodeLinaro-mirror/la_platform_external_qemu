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
#include "trace.h"
#include "hw/i3c/i3c.h"
#include "hw/core/irq.h"
#include "hci-dma-internal.h"

#define INC_AND_ROLLOVER(x, max) \
    do {                         \
        x++;                     \
        if (x >= max) {          \
            x = 0;               \
        }                        \
    } while (0)

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
}

uint64_t hci_dma_header_read(void *opaque, hwaddr offset, unsigned size)
{
    HCIDMAState *s = &(MIPI_HCI(opaque)->dma);
    offset /= sizeof(*s->header_regs);

    /* MMIO region size should prevent this from happening. */
    g_assert(offset < ARRAY_SIZE(s->header_regs));

    return s->header_regs[offset];
}

void hci_dma_header_write(void *opaque, hwaddr offset, uint64_t value,
                          unsigned size)
{
    HCIDMAState *s = &(MIPI_HCI(opaque)->dma);
    offset /= sizeof(*s->header_regs);

    /* MMIO region size should prevent this from happening. */
    g_assert(offset < ARRAY_SIZE(s->header_regs));

    value &= ~hci_dma_header_ro_mask[offset];
    s->header_regs[offset] = value;
}

uint64_t hci_dma_read(void *opaque, hwaddr offset, unsigned size)
{
    HCIDMAState *s = &(MIPI_HCI(opaque)->dma);
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

static void hci_dma_read_descr(HCIDMAState *s, TransferDescr *desc)
{
    uint64_t addr = s->regs[R_RH_CMD_RING_BASE_HI];
    uint8_t dequeue_ptr = ARRAY_FIELD_EX32(s->regs, RH_OPERATION2, CR_DEQ_PTR);
    addr <<= 32;
    addr |= s->regs[R_RH_CMD_RING_BASE_LO];
    addr += (dequeue_ptr * s->cfg.xfer_struct_size);

    cpu_physical_memory_read(addr, desc, sizeof(*desc));
}

static void hci_dma_push_resp(HCIDMAState *s, RespDescr *resp)
{
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

static void hci_dma_xfer(MIPIHCIState *hci)
{
    HCIDMAState *s = &(hci->dma);
    MIPIHCIClass *c = MIPI_HCI_GET_CLASS(hci);

    if (!hci_dma_can_xfer(s) || !hci_dma_ring_ok(s)) {
        return;
    }

    while (!hci_dma_ring_empty(s)) {
        TransferDescr desc;
        RespDescr resp;
        RespStatus status;
        bool roc = true;
        hci_dma_read_descr(s, &desc);

        switch (desc.cmd.cmd_attr) {
        case CMD_ATTR_ADDR_ASSIGN:
            status = hci_cmd_addr_assign(hci, &desc.cmd.addr_cmd, &resp);
            roc = desc.cmd.addr_cmd.roc;
            break;
        case CMD_ATTR_REGULAR_XFER:
            status = hci_dma_regular_xfer(hci, &desc, &resp);
            roc = desc.cmd.regular_xfer.roc;
            break;
        case CMD_ATTR_INTERNAL_CONTROL:
        case CMD_ATTR_COMBO_XFER:
        case CMD_ATTR_IMMEDIATE_XFER:
            {
                g_autofree char *path = object_get_canonical_path(OBJECT(hci));
                qemu_log_mask(LOG_UNIMP, "%s: Unimplemented command 0x%x\n",
                              path, desc.cmd.cmd_attr);
            }
            status = RESP_STATUS_ERROR_NOT_SUPPORTED;
            break;
        default:
            {
                g_autofree char *path = object_get_canonical_path(OBJECT(hci));
                qemu_log_mask(LOG_UNIMP, "%s: Unknown command 0x%x\n",
                              path, desc.cmd.cmd_attr);
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
        if (desc.data_buffer.ioc || status != RESP_STATUS_SUCCESS) {
            c->update_irq(hci, MIPI_HCI_IRQ_CONTEXT_DMA);
        }
        if (roc || status != RESP_STATUS_SUCCESS) {
            hci_dma_push_resp(s, &resp);
        }

        /* Increment the ring dequeue pointer. */
        uint8_t dequeue_ptr = ARRAY_FIELD_EX32(s->regs, RH_OPERATION2,
                                               CR_DEQ_PTR);
        INC_AND_ROLLOVER(dequeue_ptr,
                         ARRAY_FIELD_EX32(s->regs, CR_SETUP, RING_SIZE));
        ARRAY_FIELD_DP32(s->regs, RH_OPERATION2, CR_DEQ_PTR, dequeue_ptr);
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

static void hci_dma_rh_operation1_w(MIPIHCIState *hci, uint32_t val)
{
    HCIDMAState *s = &hci->dma;
    uint32_t prev_val = s->regs[R_RH_OPERATION1];

    s->regs[R_RH_OPERATION1] = val;

    /* Attempt to transfer if the enqueue pointer was updated. */
    if (FIELD_EX32(prev_val, RH_OPERATION1, CR_ENQ_PTR) <
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
    offset /= sizeof(*s->regs);

    /* MMIO region size should prevent this from happening. */
    g_assert(offset < ARRAY_SIZE(s->regs));

    value &= ~hci_dma_ro_mask[offset];
    uint32_t val32 = (uint32_t)value;

    switch (offset) {
    case R_RH_CONTROL:
        hci_dma_rh_control_w(hci, val32);
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

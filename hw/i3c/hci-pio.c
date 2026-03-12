/*
 * MIPI HCI I3C controller PIO functionality
 *
 * Copyright (C) 2025 Google, LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "hw/i3c/mipi-hci.h"
#include "hci-dma-internal.h"
#include "hw/core/registerfields.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "hci-cmd.h"
#include "hw/i3c/hci-ibi.h"
#include "trace.h"
#include "hw/i3c/i3c.h"
#include "hw/core/irq.h"
#include "hw/i3c/hci-pio.h"
#include "hci-pio-internal.h"
#include "qemu/fifo32.h"

#define DATA_BUFFER_THLD_CTRL_RESET 0x01010101
#define QUEUE_THLD_CTRL_RESET 0x01010101

static const uint32_t hci_pio_ro_mask[HCI_PIO_NUM_REGS] = {
    [R_RESPONSE_QUEUE_PORT]     = 0xffffffff,
    [R_IBI_PORT]                = 0xffffffff,
    [R_QUEUE_SIZE]              = 0xffffffff,
    [R_PIO_INTR_STATUS]         = 0xfffffeef,
    [R_PIO_INTR_STATUS_ENABLE]  = 0xfffffee0,
    [R_PIO_INTR_SIGNAL_ENABLE]  = 0xfffffee0,
    [R_PIO_INTR_FORCE]          = 0xfffffee0,
};

static inline uint16_t rx_buf_thld(HCIPIOState *s)
{
    return 2 << ARRAY_FIELD_EX32(s->regs, DATA_BUFFER_THLD_CTRL, RX_BUF_THLD);
}

static inline uint16_t tx_buf_thld(HCIPIOState *s)
{
    return 2 << ARRAY_FIELD_EX32(s->regs, DATA_BUFFER_THLD_CTRL, TX_BUF_THLD);
}

void hci_pio_reset(HCIPIOState *s)
{
    memset(s->regs, 0, sizeof(s->regs));

    s->regs[R_DATA_BUFFER_THLD_CTRL] = DATA_BUFFER_THLD_CTRL_RESET;
    s->regs[R_QUEUE_THLD_CTRL] = QUEUE_THLD_CTRL_RESET;
    ARRAY_FIELD_DP32(s->regs, QUEUE_SIZE, CR_QUEUE_SIZE,
                     s->cfg.cr_queue_entries);
    ARRAY_FIELD_DP32(s->regs, QUEUE_SIZE, IBI_STATUS_SIZE,
                     s->cfg.ibi_status_size);
    ARRAY_FIELD_DP32(s->regs, QUEUE_SIZE, RX_DATA_BUFFER_SIZE,
                     s->cfg.rx_data_buffer_size);
    ARRAY_FIELD_DP32(s->regs, QUEUE_SIZE, TX_DATA_BUFFER_SIZE,
                     s->cfg.tx_data_buffer_size);

    fifo32_reset(&s->cmd_fifo);
    fifo32_reset(&s->resp_fifo);
    fifo32_reset(&s->ibi_fifo);
    fifo32_reset(&s->tx_data_fifo);
    fifo32_reset(&s->rx_data_fifo);
}

static uint32_t hci_pio_response_queue_r(MIPIHCIState *hci)
{
    MIPIHCIClass *c = MIPI_HCI_GET_CLASS(hci);
    HCIPIOState *s = &hci->pio;

    uint32_t value = fifo32_pop(&s->resp_fifo);
    if (fifo32_num_used(&s->resp_fifo) < ARRAY_FIELD_EX32(s->regs,
            QUEUE_THLD_CTRL, RESP_BUF_THLD)) {
        ARRAY_FIELD_DP32(s->regs, PIO_INTR_STATUS, RESP_READY_STAT, 0);
    }
    c->update_irq(hci, MIPI_HCI_IRQ_CONTEXT_PIO);

    return value;
}

static uint32_t hci_pio_xfer_data_port_r(MIPIHCIState *hci)
{
    HCIPIOState *s = &hci->pio;
    MIPIHCIClass *c = MIPI_HCI_GET_CLASS(hci);

    if (fifo32_is_empty(&s->rx_data_fifo)) {
        g_autofree char *path = object_get_canonical_path(OBJECT(hci));
        qemu_log_mask(LOG_GUEST_ERROR, "%s: RX data FIFO is empty.\n", path);
        return 0;
    }

    uint32_t value = fifo32_pop(&s->rx_data_fifo);
    if (fifo32_num_used(&s->rx_data_fifo) < rx_buf_thld(s)) {
        ARRAY_FIELD_DP32(s->regs, PIO_INTR_STATUS, RX_THLD_STAT, 0);
        c->update_irq(hci, MIPI_HCI_IRQ_CONTEXT_PIO);
    }
    return value;
}

uint64_t hci_pio_read(void *opaque, hwaddr offset, unsigned size)
{
    MIPIHCIState *hci = MIPI_HCI(opaque);
    HCIPIOState *s = &hci->pio;

    offset /= sizeof(*s->regs);

    /* MMIO region size should prevent this from happening. */
    g_assert(offset < ARRAY_SIZE(s->regs));

    uint32_t value = 0;
    switch (offset) {
    case R_COMMAND_QUEUE_PORT: {
        g_autofree char *path = object_get_canonical_path(OBJECT(hci));
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Read from write-only register at "
                      "0x%.2lx.\n", path, offset * sizeof(*s->regs));
    }
        break;
    case R_RESPONSE_QUEUE_PORT:
        value = hci_pio_response_queue_r(hci);
        break;
    case R_XFER_DATA_PORT:
        value = hci_pio_xfer_data_port_r(hci);
        break;
    default:
        value = s->regs[offset];
        break;
    }

    trace_hci_pio_read(DEVICE(opaque)->canonical_path,
                       offset * sizeof(*s->regs), value);
    return value;
}

static void hci_pio_push_resp(MIPIHCIState *hci, RespDescr *resp)
{
    HCIPIOState *s = &hci->pio;
    MIPIHCIClass *c = MIPI_HCI_GET_CLASS(hci);

    if (fifo32_num_used(&s->resp_fifo) >= ARRAY_FIELD_EX32(s->regs, QUEUE_SIZE,
                                                           CR_QUEUE_SIZE)) {
        g_autofree char *path = object_get_canonical_path(OBJECT(hci));
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Response queue is full.\n", path);
        return;
    }

    trace_hci_pio_push_resp(DEVICE(hci)->canonical_path, resp->val32);

    fifo32_push(&s->resp_fifo, resp->val32);

    if (fifo32_num_used(&s->resp_fifo) >= ARRAY_FIELD_EX32(s->regs,
            QUEUE_THLD_CTRL, RESP_BUF_THLD)) {
        ARRAY_FIELD_DP32(s->regs, PIO_INTR_STATUS, RESP_READY_STAT, 1);
        c->update_irq(hci, MIPI_HCI_IRQ_CONTEXT_PIO);
    }
}

static void hci_pio_push_rx_fifo(MIPIHCIState *hci, const uint8_t *data,
                                 uint32_t len)
{
    HCIPIOState *s = &hci->pio;
    MIPIHCIClass *c = MIPI_HCI_GET_CLASS(hci);

    /*
     * Round up to the nearest DWORD if there's a partial one. We allocated
     * and zeroed extra bytes if they're present, so we can safely do this.
     */
    uint32_t num_words = (len / 4) + (len % 4 != 0);
    for (uint32_t i = 0; i < num_words; ++i) {
        if (fifo32_is_full(&s->rx_data_fifo)) {
            g_autofree char *path = object_get_canonical_path(OBJECT(hci));
            qemu_log_mask(LOG_GUEST_ERROR, "%s: RX data FIFO is full.\n", path);
            break;
        }

        /* We only support little-endian byte ordering. */
        fifo32_push(&s->rx_data_fifo, htole32(*((uint32_t *)&data[i * 4])));
    }

    if (fifo32_num_used(&s->rx_data_fifo) >= rx_buf_thld(s)) {
        ARRAY_FIELD_DP32(s->regs, PIO_INTR_STATUS, RX_THLD_STAT, 1);
        c->update_irq(hci, MIPI_HCI_IRQ_CONTEXT_PIO);
    }
}

static RespStatus hci_pio_i3c_read(MIPIHCIState *hci, const CmdDescr *cmd,
                                   RespDescr *resp)
{
    uint32_t len = cmd->regular_xfer.data_length;
    /* Round the data buffer up to the nearest DWORD to make storing easier. */
    g_autofree uint8_t *data = g_new0(uint8_t, len +
                                      (sizeof(uint32_t) - (len % 4)));
    uint32_t num_read = 0;
    RespStatus status = hci_cmd_read(hci, &cmd->regular_xfer, resp, data, len,
                                     &num_read);

    if (status == RESP_STATUS_SUCCESS) {
        hci_pio_push_rx_fifo(hci, data, num_read);
    }
    return status;
}

static void hci_pio_pop_tx_fifo(MIPIHCIState *hci, uint8_t *data, uint32_t len)
{
    HCIPIOState *s = &hci->pio;
    MIPIHCIClass *c = MIPI_HCI_GET_CLASS(hci);

    uint32_t num_words = (len / 4) + (len % 4 != 0);
    for (uint32_t i = 0; i < num_words; ++i) {
        if (fifo32_is_empty(&s->tx_data_fifo)) {
            g_autofree char *path = object_get_canonical_path(OBJECT(hci));
            qemu_log_mask(LOG_GUEST_ERROR, "%s: TX data FIFO is empty.\n",
                          path);
            break;
        }

        uint32_t value = fifo32_pop(&s->tx_data_fifo);
        /* We only support little-endian byte ordering. */
        *((uint32_t *)&data[i * 4]) = le32toh(value);
    }

    if (fifo32_num_used(&s->tx_data_fifo) < tx_buf_thld(s)) {
        ARRAY_FIELD_DP32(s->regs, PIO_INTR_STATUS, TX_THLD_STAT, 0);
        c->update_irq(hci, MIPI_HCI_IRQ_CONTEXT_PIO);
    }
}

static RespStatus hci_pio_i3c_send(MIPIHCIState *hci, const CmdDescr *cmd,
                                   RespDescr *resp)
{
    uint32_t len = cmd->regular_xfer.data_length;
    /* Round the data buffer up to the nearest DWORD to make storing easier. */
    g_autofree uint8_t *data = g_new0(uint8_t, len +
                                      (sizeof(uint32_t) - (len % 4)));

    hci_pio_pop_tx_fifo(hci, data, len);
    return hci_cmd_send(hci, &cmd->regular_xfer, resp, data, len);
}

static RespStatus hci_pio_regular_xfer(MIPIHCIState *hci,
                                        const CmdDescr *cmd,
                                        RespDescr *resp)
{
    if (cmd->regular_xfer.rnw) {
        return hci_pio_i3c_read(hci, cmd, resp);
    }
    return hci_pio_i3c_send(hci, cmd, resp);
}

static RespStatus hci_pio_internal_control_xfer(MIPIHCIState *hci,
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

static void hci_pio_xfer(MIPIHCIState *hci)
{
    HCIPIOState *s = &hci->pio;
    MIPIHCIClass *c = MIPI_HCI_GET_CLASS(hci);

    /* Grab the transfer command. */
    CmdDescr cmd;
    cmd.val32[0] = fifo32_pop(&s->cmd_fifo);
    cmd.val32[1] = fifo32_pop(&s->cmd_fifo);

    trace_hci_pio_xfer(DEVICE(hci)->canonical_path, cmd.val64);

    /* And execute it. */
    RespStatus status;
    RespDescr resp = {0};
    bool roc = cmd.shared_fields.roc;
    switch (cmd.shared_fields.cmd_attr) {
    case CMD_ATTR_ADDR_ASSIGN:
        status = hci_cmd_addr_assign(hci, &cmd.addr_cmd, &resp);
        break;
    case CMD_ATTR_IMMEDIATE_XFER:
        status = hci_cmd_immediate_xfer(hci, &cmd.immediate_xfer, &resp);
        break;
    case CMD_ATTR_REGULAR_XFER:
        status = hci_pio_regular_xfer(hci, &cmd, &resp);
        break;
    case CMD_ATTR_INTERNAL_CONTROL:
        status = hci_pio_internal_control_xfer(hci, &cmd.internal_control,
                                               &resp);
        /*
         * Not documented, nor is it a part of the internal control data
         * structure, but the driver always expects a response to internal
         * control commands.
         */
        roc = true;
        break;
    default: {
        g_autofree char *path = object_get_canonical_path(OBJECT(hci));
        qemu_log_mask(LOG_UNIMP, "%s: Unimplemented transfer type 0x%x\n",
                      path, cmd.shared_fields.cmd_attr);
        status = RESP_STATUS_ERROR_NOT_SUPPORTED;
        resp.resp.err = RESP_STATUS_ERROR_NOT_SUPPORTED;
        break;
    }
    }

    if (status != RESP_STATUS_SUCCESS) {
        ARRAY_FIELD_DP32(s->regs, PIO_INTR_STATUS, TRANSFER_ERR_STAT, 1);
        c->enter_halt(hci);
    }

    if (roc || status != RESP_STATUS_SUCCESS) {
        hci_pio_push_resp(hci, &resp);
    }
}

static void hci_pio_cmd_queue_w(MIPIHCIState *hci, uint32_t val)
{
    HCIPIOState *s = &hci->pio;
    MIPIHCIClass *c = MIPI_HCI_GET_CLASS(hci);

    if (fifo32_is_full(&s->cmd_fifo)) {
        g_autofree char *path = object_get_canonical_path(OBJECT(hci));
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Command was pushed when the "
                      "command queue was full.\n", path);
        return;
    }

    fifo32_push(&s->cmd_fifo, val);
    /*
     * If an even number of DWORDs have been pushed, create a new command and
     * execute it.
     * TODO(b/487379928): Handle partial transfers. It's possible that a
     * transfer could be in progress, and we need to wait before dequeueing this
     * one.
     */
    if (fifo32_num_used(&s->cmd_fifo) % 2 == 0) {
        hci_pio_xfer(hci);
    }

    if (fifo32_num_free(&s->cmd_fifo) >= ARRAY_FIELD_EX32(s->regs,
            QUEUE_THLD_CTRL, CMD_EMPTY_BUF_THLD)) {
        ARRAY_FIELD_DP32(s->regs, PIO_INTR_STATUS, CMD_QUEUE_READY_STAT, 1);
    } else {
        ARRAY_FIELD_DP32(s->regs, PIO_INTR_STATUS, CMD_QUEUE_READY_STAT, 0);
    }
    c->update_irq(hci, MIPI_HCI_IRQ_CONTEXT_PIO);
}

static void hci_pio_update_queue_thld(MIPIHCIState *hci)
{
    HCIPIOState *s = &hci->pio;
    MIPIHCIClass *c = MIPI_HCI_GET_CLASS(hci);
    bool thld_met = false;

    thld_met = fifo32_num_used(&s->ibi_fifo) >= ARRAY_FIELD_EX32(s->regs,
            QUEUE_THLD_CTRL, IBI_STATUS_THLD);
    ARRAY_FIELD_DP32(s->regs, PIO_INTR_STATUS, IBI_STATUS_THLD_STAT, thld_met);

    thld_met = fifo32_num_used(&s->resp_fifo) >= ARRAY_FIELD_EX32(s->regs,
            QUEUE_THLD_CTRL, RESP_BUF_THLD);
    ARRAY_FIELD_DP32(s->regs, PIO_INTR_STATUS, RESP_READY_STAT, thld_met);

    thld_met = fifo32_num_free(&s->cmd_fifo) >= ARRAY_FIELD_EX32(s->regs,
            QUEUE_THLD_CTRL, CMD_EMPTY_BUF_THLD);
    ARRAY_FIELD_DP32(s->regs, PIO_INTR_STATUS, CMD_QUEUE_READY_STAT, thld_met);

    c->update_irq(hci, MIPI_HCI_IRQ_CONTEXT_PIO);
}

static void hci_pio_update_data_buffer_thld(MIPIHCIState *hci)
{
    HCIPIOState *s = &hci->pio;
    MIPIHCIClass *c = MIPI_HCI_GET_CLASS(hci);
    bool thld_met = false;

    thld_met = fifo32_num_free(&s->tx_data_fifo) >= tx_buf_thld(s);
    ARRAY_FIELD_DP32(s->regs, PIO_INTR_STATUS, TX_THLD_STAT, thld_met);

    thld_met = fifo32_num_used(&s->rx_data_fifo) >= rx_buf_thld(s);
    ARRAY_FIELD_DP32(s->regs, PIO_INTR_STATUS, RX_THLD_STAT, thld_met);

    c->update_irq(hci, MIPI_HCI_IRQ_CONTEXT_PIO);
}

static void hci_pio_queue_thld_ctrl_w(MIPIHCIState *hci, uint32_t val)
{
    HCIPIOState *s = &hci->pio;

    s->regs[R_QUEUE_THLD_CTRL] = val;
    hci_pio_update_queue_thld(hci);
}

static void hci_pio_data_buffer_thld_ctrl_w(MIPIHCIState *hci, uint32_t val)
{
    HCIPIOState *s = &hci->pio;

    s->regs[R_DATA_BUFFER_THLD_CTRL] = val;
    hci_pio_update_data_buffer_thld(hci);
}

static void hci_pio_intr_status_w(MIPIHCIState *hci, uint32_t val)
{
    MIPIHCIClass *c = MIPI_HCI_GET_CLASS(hci);

    hci->pio.regs[R_PIO_INTR_STATUS] &= ~val; /* W1C */
    c->update_irq(hci, MIPI_HCI_IRQ_CONTEXT_PIO);
}

static void hci_pio_intr_force_w(MIPIHCIState *hci, uint32_t val)
{
    /* PIO_INTR_FORCE is WO, so we only need to update PIO_INTR_STATUS. */
    hci->pio.regs[R_PIO_INTR_STATUS] = val;

    /*
     * Set the interrupt status. If it's not masked, it will be cleared during
     * IRQ updating.
     */
    hci_pio_update_data_buffer_thld(hci);
    hci_pio_update_queue_thld(hci);
}

static void hci_pio_intr_status_enable_w(MIPIHCIState *hci, uint32_t val)
{
    hci->pio.regs[R_PIO_INTR_STATUS_ENABLE] = val;

    /*
     * There could be pending threshold interrupts that could now be present if
     * the mask is set.
     */
    hci_pio_update_data_buffer_thld(hci);
    hci_pio_update_queue_thld(hci);
}

static void hci_pio_intr_signal_enable_w(MIPIHCIState *hci, uint32_t val)
{
    hci->pio.regs[R_PIO_INTR_SIGNAL_ENABLE] = val;

    /*
     * There could be pending threshold interrupts that could now be present if
     * the mask is set.
     */
    hci_pio_update_data_buffer_thld(hci);
    hci_pio_update_queue_thld(hci);
}

static void hci_pio_xfer_data_port_w(MIPIHCIState *hci, uint32_t val)
{
    HCIPIOState *s = &hci->pio;
    MIPIHCIClass *c = MIPI_HCI_GET_CLASS(hci);

    if (fifo32_is_full(&s->tx_data_fifo)) {
        g_autofree char *path = object_get_canonical_path(OBJECT(hci));
        qemu_log_mask(LOG_GUEST_ERROR, "%s: TX data FIFO is full.\n", path);
        return;
    }

    fifo32_push(&s->tx_data_fifo, val);
    if (fifo32_num_used(&s->rx_data_fifo) >= tx_buf_thld(s)) {
        ARRAY_FIELD_DP32(s->regs, PIO_INTR_STATUS, TX_THLD_STAT, 1);
        c->update_irq(hci, MIPI_HCI_IRQ_CONTEXT_PIO);
    }
}

void hci_pio_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    MIPIHCIState *hci = MIPI_HCI(opaque);
    HCIPIOState *s = &hci->pio;

    trace_hci_pio_write(DEVICE(hci)->canonical_path, offset, value);

    offset /= sizeof(*s->regs);

    /* MMIO region size should prevent this from happening. */
    g_assert(offset < ARRAY_SIZE(s->regs));

    value &= ~hci_pio_ro_mask[offset];
    uint32_t val32 = (uint32_t)value;

    switch (offset) {
    case R_COMMAND_QUEUE_PORT:
        hci_pio_cmd_queue_w(hci, val32);
        break;
    case R_XFER_DATA_PORT:
        hci_pio_xfer_data_port_w(hci, val32);
        break;
    case R_QUEUE_THLD_CTRL:
        hci_pio_queue_thld_ctrl_w(hci, val32);
        break;
    case R_DATA_BUFFER_THLD_CTRL:
        hci_pio_data_buffer_thld_ctrl_w(hci, val32);
        break;
    case R_PIO_INTR_STATUS:
        hci_pio_intr_status_w(hci, val32);
        break;
    case R_PIO_INTR_STATUS_ENABLE:
        hci_pio_intr_status_enable_w(hci, val32);
        break;
    case R_PIO_INTR_SIGNAL_ENABLE:
        hci_pio_intr_signal_enable_w(hci, val32);
        break;
    case R_PIO_INTR_FORCE:
        hci_pio_intr_force_w(hci, val32);
        break;
    default:
        s->regs[offset] = val32;
        break;
    }
}


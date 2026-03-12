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
}

uint64_t hci_pio_read(void *opaque, hwaddr offset, unsigned size)
{
    HCIPIOState *s = &(MIPI_HCI(opaque)->pio);

    offset /= sizeof(*s->regs);

    /* MMIO region size should prevent this from happening. */
    g_assert(offset < ARRAY_SIZE(s->regs));

    trace_hci_pio_read(DEVICE(opaque)->canonical_path,
                       offset * sizeof(*s->regs), s->regs[offset]);

    return s->regs[offset];
}

static void hci_pio_intr_status_w(MIPIHCIState *hci, uint32_t val)
{
    MIPIHCIClass *c = MIPI_HCI_GET_CLASS(hci);

    hci->pio.regs[R_PIO_INTR_STATUS] &= ~val; /* W1C */
    c->update_irq(hci, MIPI_HCI_IRQ_CONTEXT_PIO);
}

static void hci_pio_intr_force_w(MIPIHCIState *hci, uint32_t val)
{
    MIPIHCIClass *c = MIPI_HCI_GET_CLASS(hci);

    /*
     * Set the interrupt status. If it's not masked, it will be cleared during
     * IRQ updating. PIO_INTR_FORCE is WO, so we only need to update
     * PIO_INTR_STATUS.
     */
    hci->pio.regs[R_PIO_INTR_STATUS] = val;
    c->update_irq(hci, MIPI_HCI_IRQ_CONTEXT_PIO);
}

static void hci_pio_intr_status_enable_w(MIPIHCIState *hci, uint32_t val)
{
    MIPIHCIClass *c = MIPI_HCI_GET_CLASS(hci);

    hci->pio.regs[R_PIO_INTR_STATUS_ENABLE] = val;

    /*
     * There could be pending threshold interrupts that could now be present if
     * the mask is set.
     */
    c->update_irq(hci, MIPI_HCI_IRQ_CONTEXT_PIO);
}

static void hci_pio_intr_signal_enable_w(MIPIHCIState *hci, uint32_t val)
{
    MIPIHCIClass *c = MIPI_HCI_GET_CLASS(hci);

    hci->pio.regs[R_PIO_INTR_SIGNAL_ENABLE] = val;

    /*
     * There could be pending threshold interrupts that could now be present if
     * the mask is set.
     */
    c->update_irq(hci, MIPI_HCI_IRQ_CONTEXT_PIO);
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
        /* WO. */
        break;
    case R_XFER_DATA_PORT:
        /* WO on writes. */
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


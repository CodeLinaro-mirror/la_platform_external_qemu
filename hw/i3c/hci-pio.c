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
        /* W1C fields. */
        s->regs[R_PIO_INTR_STATUS] &= ~val32;
        break;
    case R_PIO_INTR_FORCE:
        /* WO. */
        break;
    default:
        s->regs[offset] = val32;
        break;
    }
}


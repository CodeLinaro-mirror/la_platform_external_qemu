/*
 * MIPI HCI I3C IBI functionality
 *
 * Copyright (C) 2025 Google, LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/i3c/hci-ibi.h"
#include "hw/i3c/mipi-hci.h"
#include "hw/i3c/i3c.h"
#include "qemu/log.h"
#include "hw/core/registerfields.h"
#include "hci-dat-internal.h"
#include "hci-dma-internal.h"
#include "hw/i3c/hci-dat.h"
#include "trace.h"

static void hci_ibi_report_ibi(MIPIHCIState *hci)
{
    g_assert(hci->ibi_in_progress != NULL);

    hci->ibi_in_progress->ibi.last_status = 1;

    if (ARRAY_FIELD_EX32(hci->core.regs, HC_CONTROL, MODE_SELECTOR) ==
        MODE_SELECTOR_PIO) {
        g_autofree char *path = object_get_canonical_path(OBJECT(hci));
        qemu_log_mask(LOG_UNIMP, "%s: Tried to store IBI in PIO mode, but PIO "
                      "is not supported.", path);
    } else {
        hci_dma_report_ibi(hci);
    }

    free(hci->ibi_in_progress);
    hci->ibi_in_progress = NULL;
}

int hci_ibi_handle(I3CBus *bus, uint8_t addr, bool is_recv)
{
    MIPIHCIState *hci = MIPI_HCI(bus->parent_obj.parent);
    MIPIHCIClass *mhc = MIPI_HCI_GET_CLASS(OBJECT(hci));
    bool has_error = false;
    /* Mask off parity bit, if present. */
    addr &= 0x7f;
    uint32_t dev_index = mhc->dat_dev_index_from_addr(hci, addr);

    trace_hci_ibi_handle(DEVICE(hci)->canonical_path, addr, is_recv);

    if (hci->ibi_in_progress) {
        g_autofree char *path = object_get_canonical_path(OBJECT(hci));
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Received an IBI while another was "
                      "in progress", path);
        hci->ibi_in_progress->ibi.error = 1;
        goto done;
    }
    hci->ibi_in_progress = g_new0(IbiStatus, 1);

    /* We don't support PIO, just NACK and tell the user. */
    if (ARRAY_FIELD_EX32(hci->core.regs, HC_CONTROL, MODE_SELECTOR) ==
        MODE_SELECTOR_PIO) {
        g_autofree char *path = object_get_canonical_path(OBJECT(hci));
        qemu_log_mask(LOG_UNIMP, "%s: IBI was received in PIO mode, but PIO is "
                     "not supported.", path);
        hci->ibi_in_progress->ibi.error = 1;
        goto done;
    }

    /*
     * If it's a hot-join, the device index isn't pointing to a real device
     * since it's not on the bus yet, so finish the IBI here.
     */
    if (addr == I3C_HJ_ADDR) {
        if (ARRAY_FIELD_EX32(hci->core.regs, HC_CONTROL, HOT_JOIN_CTRL)) {
            hci->ibi_in_progress->ibi.error = 1;
        }
        goto done;
    }

    if (dev_index == HCI_DAT_DEV_NOT_FOUND) {
        g_autofree char *path = object_get_canonical_path(OBJECT(hci));
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Received an IBI from address "
                      " 0x%.2x which isn't in the DAT table", path, addr);
        hci->ibi_in_progress->ibi.error = 1;
        goto done;
    }
    if (is_recv && FIELD_EX32(hci->dat.regs[dev_index + R_TARGET_DAT],
                              TARGET_DAT, TARGET_IBI_REJECT)) {
        hci->ibi_in_progress->ibi.error = 1;
        goto done;
    } else if (!is_recv && FIELD_EX32(hci->dat.regs[dev_index + R_TARGET_DAT],
                                      TARGET_DAT, TARGET_CRR_REJECT)) {
        hci->ibi_in_progress->ibi.error = 1;
        goto done;
    }

done:
    hci->ibi_in_progress->ibi.ibi_id = (addr << 1) | is_recv;
    has_error = hci->ibi_in_progress->ibi.error;

    /* If we're NACKing the IBI, report it. */
    if (has_error) {
        hci_ibi_report_ibi(hci);
    }
    return has_error;
}

int hci_ibi_recv(I3CBus *bus, uint8_t data)
{
    MIPIHCIState *hci = MIPI_HCI(bus->parent_obj.parent);
    bool has_error = false;
    MIPIHCIClass *mhc = MIPI_HCI_GET_CLASS(OBJECT(hci));

    /*
     * The IBI has already ended (and NACKed), but the target tried to send data
     * anyway. Just NACK it again and return.
     */
    if (hci->ibi_in_progress == NULL) {
        return -1;
    }

    trace_hci_ibi_recv(DEVICE(hci)->canonical_path, data);

    uint8_t addr = hci->ibi_in_progress->ibi.ibi_id >> 1;
    uint32_t dev_index = mhc->dat_dev_index_from_addr(hci, addr);

    /* We don't support PIO, just NACK and tell the user. */
    if (ARRAY_FIELD_EX32(hci->core.regs, HC_CONTROL, MODE_SELECTOR) ==
        MODE_SELECTOR_PIO) {
        g_autofree char *path = object_get_canonical_path(OBJECT(hci));
        qemu_log_mask(LOG_UNIMP, "%s: IBI data was received in PIO mode, but "
                      "PIO is not supported.", path);
        hci->ibi_in_progress->ibi.error = 1;
        goto done;
    }

    if (!FIELD_EX32(hci->dat.regs[dev_index + R_TARGET_DAT], TARGET_DAT,
                    TARGET_IBI_PAYLOAD)) {
        g_autofree char *path = object_get_canonical_path(OBJECT(hci));
        qemu_log_mask(LOG_UNIMP, "%s: Received unexpected IBI data from "
                      "target 0x%.2x.", path, addr);
        hci->ibi_in_progress->ibi.error = 1;
        goto done;
    }

    g_assert(hci->ibi_in_progress->num_bytes <
             ARRAY_SIZE(hci->ibi_in_progress->data));
    hci->ibi_in_progress->data[hci->ibi_in_progress->num_bytes] = data;
    hci->ibi_in_progress->num_bytes++;

done:
    has_error = hci->ibi_in_progress->ibi.error;

    /* If we're NACKing the IBI, report it. */
    if (has_error) {
        hci_ibi_report_ibi(hci);
    }
    return has_error;
}

int hci_ibi_finish(I3CBus *bus)
{
    MIPIHCIState *hci = MIPI_HCI(bus->parent_obj.parent);
   /*
    * The IBI has already ended (and NACKed), but the target tried to send data
    * anyway. Just NACK it again and return.
    */
    if (hci->ibi_in_progress == NULL) {
        return -1;
    }

    trace_hci_ibi_finish(DEVICE(hci)->canonical_path);
    hci_ibi_report_ibi(hci);
    return 0;
}

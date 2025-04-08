/*
 * MIPI HCI I3C controller commands
 *
 * Copyright (C) 2025 Google, LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hci-cmd.h"
#include "hw/i3c/i3c.h"
#include "qemu/log.h"
#include "hw/core/registerfields.h"
#include "hw/i3c/mipi-hci.h"

#define INC_AND_ROLLOVER(x, amount, max) \
    do {                                 \
        x = (x) + amount;                \
        if (x >= max) {                  \
            x -= max;                    \
        }                                \
    } while (0)

static bool hci_cmd_addr_assign_ok(const AddrCmd *desc)
{
    if (desc->cmd_attr != CMD_ATTR_ADDR_ASSIGN) {
        return false;
    }
    if (desc->cmd != I3C_CCC_ENTDAA &&
        desc->cmd != I3C_CCCD_SETDASA) {
        return false;
    }
    if (!desc->roc) {
        return false;
    }
    if (!desc->toc) {
        return false;
    }

    return true;
}

static RespStatus hci_cmd_do_entdaa(MIPIHCIState *hci, const AddrCmd *desc,
                                    RespDescr *resp) {
    MIPIHCIClass *mhc = MIPI_HCI_GET_CLASS(hci);
    RespStatus status = RESP_STATUS_SUCCESS;
    int i = 0;
    uint32_t dat_offset = DAT_ENTRY_FROM_DEV_INDEX(desc->dev_index);
    int32_t dct_offset = 0;
    uint32_t devices_assigned = 0;

    /* Start ENTDAA. */
    if (i3c_start_send(hci->bus, I3C_BROADCAST)) {
        status = RESP_STATUS_ERROR_ADDR_HEADER;
        goto done;
    }
    i3c_send_byte(hci->bus, desc->cmd);

    for (i = 0; i < desc->dev_count; ++i) {
        /* See if anyone on the bus still needs an address. */
        if (i3c_start_send(hci->bus, I3C_BROADCAST)) {
            status = RESP_STATUS_ERROR_NACK;
            break;
        }

        /* Read its PID, BCR, and DCR. */
        uint32_t bytes_read;
        union {
            uint64_t pid:48;
            uint8_t bcr;
            uint8_t dcr;
            uint32_t w[2];
            uint8_t b[8];
        } target_info;
        if (i3c_recv(hci->bus, target_info.b, sizeof(target_info.b),
                     &bytes_read)) {
            g_autofree char *path = object_get_canonical_path(OBJECT(hci));
            qemu_log_mask(LOG_GUEST_ERROR, "%s: Target NACKed during ENTDAA\n",
                          path);
            status = RESP_STATUS_ERROR_XFER_ABORTED;
            break;
        }
        /* Shouldn't happen, the target is misbehaving. Treat it as a NACK. */
        if (bytes_read != sizeof(target_info)) {
            g_autofree char *path = object_get_canonical_path(OBJECT(hci));
            qemu_log_mask(LOG_GUEST_ERROR, "%s: Target failed to send BCR, "
                          "DCR, and PID during ENTDAA\n", path);
            status = RESP_STATUS_ERROR_XFER_ABORTED;
            break;
        }

        /* Assign the address. */
        uint8_t addr = mhc->get_next_dynamic_addr(hci, dat_offset);
        if (i3c_send_byte(hci->bus, addr)) {
            g_autofree char *path = object_get_canonical_path(OBJECT(hci));
            qemu_log_mask(LOG_GUEST_ERROR, "%s: Target NACKed address 0x%x "
                          "during ENTDAA\n", path, addr);
            status = RESP_STATUS_ERROR_XFER_ABORTED;
            break;
        }

        /* Update the DCT and increment our DAT and DCT pointers. */
        hci->dct.regs[dct_offset + R_TARGET_DCT_0] =
            target_info.pid & 0xffffffff;
        FIELD_DP32(hci->dct.regs[dct_offset + R_TARGET_DCT_1], TARGET_DCT_1,
                   TARGET_PID_LO, (uint16_t)(target_info.pid >> 32));
        FIELD_DP32(hci->dct.regs[dct_offset + R_TARGET_DCT_2], TARGET_DCT_2,
                   TARGET_DCR, target_info.dcr);
        FIELD_DP32(hci->dct.regs[dct_offset + R_TARGET_DCT_2], TARGET_DCT_2,
                   TARGET_BCR, target_info.bcr);
        FIELD_DP32(hci->dct.regs[dct_offset + R_TARGET_DCT_3], TARGET_DCT_3,
                   TARGET_DYNAMIC_ADDRESS, addr);

        /* DAT must be contiguous. */
        dat_offset += HCI_DAT_ENTRY_SIZE;
        /* DCT can roll over if we hit the end. */
        INC_AND_ROLLOVER(dct_offset, HCI_DCT_ENTRY_SIZE,
                         hci->core.cfg.dct_table_size * HCI_DCT_ENTRY_SIZE);
    }

    /*
     * Per the spec, ENTDAA must be stopped after a subsequent broadcast.
     * If we went through every device specified in the descriptor, we need to
     * broadcast again, since the last thing that happened was us was sending
     * the address to the device.
     * We also need to broadcast again if the device NACKed the address.
     */
    if (status == RESP_STATUS_SUCCESS ||
        status == RESP_STATUS_ERROR_XFER_ABORTED) {
        i3c_start_send(hci->bus, I3C_BROADCAST);
    }

done:
    devices_assigned = i;
    resp->resp.length = desc->dev_count - devices_assigned;

    return status;
}

static RespStatus hci_cmd_do_setdasa(MIPIHCIState *hci, const AddrCmd *desc,
                                     RespDescr *resp) {
    RespStatus status = RESP_STATUS_SUCCESS;
    int i = 0;
    uint32_t dat_offset = DAT_ENTRY_FROM_DEV_INDEX(desc->dev_index);
    uint32_t devices_assigned = 0;

    /* Start SETDASA. */
    if (i3c_start_send(hci->bus, I3C_BROADCAST)) {
        status = RESP_STATUS_ERROR_ADDR_HEADER;
        goto done;
    }
    i3c_send_byte(hci->bus, desc->cmd);

    for (i = 0; i < desc->dev_count; ++i) {
        /* Directly address the target. */
        uint8_t static_addr =
            FIELD_EX32(hci->dat.regs[dat_offset + R_TARGET_DAT], TARGET_DAT,
                       TARGET_STATIC_ADDRESS);
        if (i3c_start_send(hci->bus, static_addr)) {
            status = RESP_STATUS_ERROR_NACK;
            break;
        }

        /* Assign it its dynamic address. */
        uint8_t dynamic_addr =
            FIELD_EX32(hci->dat.regs[dat_offset + R_TARGET_DAT], TARGET_DAT,
                       TARGET_DYNAMIC_ADDRESS);
        if (i3c_send_byte(hci->bus, dynamic_addr)) {
            g_autofree char *path = object_get_canonical_path(OBJECT(hci));
            qemu_log_mask(LOG_GUEST_ERROR, "%s: Target NACKed address 0x%x "
                          "during SETDASA\n", path, dynamic_addr);
            status = RESP_STATUS_ERROR_XFER_ABORTED;
            break;
        }

        dat_offset += HCI_DAT_ENTRY_SIZE;
    }

done:
    devices_assigned = i;
    resp->resp.length = desc->dev_count - devices_assigned;

    return status;
}

RespStatus hci_cmd_addr_assign(MIPIHCIState *hci, const AddrCmd *desc,
                               RespDescr *resp) {
    RespStatus status = RESP_STATUS_SUCCESS;

    if (!hci_cmd_addr_assign_ok(desc)) {
        status = RESP_STATUS_ERROR_NOT_SUPPORTED;
        resp->resp.length = desc->dev_count;
        goto done;
    }

    if (desc->cmd == I3C_CCC_ENTDAA) {
        status = hci_cmd_do_entdaa(hci, desc, resp);
    } else if (desc->cmd == I3C_CCCD_SETDASA) {
        status = hci_cmd_do_setdasa(hci, desc, resp);
    } else {
        /* We check if the CCC is valid beforehand, so something went wrong. */
        g_assert_not_reached();
    }

    /* Response is set regardless of ROC. */
done:
    if (desc->toc) {
        i3c_end_transfer(hci->bus);
    }
    resp->resp.tid = desc->tid;
    resp->resp.err = status;

    return status;
}

static RespStatus hci_cmd_start_ccc(MIPIHCIState *hci, const RegularXfer *desc)
{
    MIPIHCIClass *mhc = MIPI_HCI_GET_CLASS(hci);
    uint8_t ccc = desc->cmd;
    uint8_t addr = 0;
    uint16_t dat_offset = 0;

    /* Start the CCC, both direct and broadcast start with a broadcast. */
    if (i3c_start_send(hci->bus, I3C_BROADCAST)) {
        return RESP_STATUS_ERROR_ADDR_HEADER;
    }
    if (i3c_send_byte(hci->bus, ccc)) {
        return RESP_STATUS_ERROR_XFER_ABORTED;
    }
    if (desc->dbp) {
        if (i3c_send_byte(hci->bus, desc->def_byte)) {
            return RESP_STATUS_ERROR_XFER_ABORTED;
        }
    }

    /* If we're doing a direct CCC, reSTART and address the target. */
    if (CCC_IS_DIRECT(ccc)) {
        dat_offset = DAT_ENTRY_FROM_DEV_INDEX(desc->dev_index);
        addr = mhc->get_dev_dynamic_addr(hci, dat_offset);
        if (i3c_start_send(hci->bus, addr)) {
            return RESP_STATUS_ERROR_XFER_ABORTED;
        }
    }

    return RESP_STATUS_SUCCESS;
}

static RespStatus hci_cmd_send_ccc(MIPIHCIState *hci, const RegularXfer *desc,
                                   RespDescr *resp, const uint8_t *data,
                                   size_t len)
{
    uint32_t num_sent = 0;

    RespStatus status = hci_cmd_start_ccc(hci, desc);
    if (status != RESP_STATUS_SUCCESS) {
        goto done;
    }

    /* Now send the CCC data, if any. */
    if (i3c_send(hci->bus, data, len, &num_sent)) {
        status = RESP_STATUS_ERROR_XFER_ABORTED;
    }

done:
    if (desc->toc) {
        i3c_end_transfer(hci->bus);
    }
    resp->resp.length = len - num_sent;
    return status;
}

static RespStatus hci_cmd_read_ccc(MIPIHCIState *hci, const RegularXfer *desc,
                                   RespDescr *resp, uint8_t *data, size_t len,
                                   uint32_t *num_read)
{
    *num_read = 0;

    RespStatus status = hci_cmd_start_ccc(hci, desc);
    if (status != RESP_STATUS_SUCCESS) {
        goto done;
    }

    if (i3c_recv(hci->bus, data, len, num_read)) {
        status = RESP_STATUS_ERROR_XFER_ABORTED;
    }

done:
    if (desc->toc) {
        i3c_end_transfer(hci->bus);
    }
    resp->resp.length = len - *num_read;
    return status;
}

static RespStatus hci_cmd_i3c_start_xfer(MIPIHCIState *hci,
                                         const RegularXfer *desc)
{
    MIPIHCIClass *mhc = MIPI_HCI_GET_CLASS(hci);
    uint16_t dat_offset = DAT_ENTRY_FROM_DEV_INDEX(desc->dev_index);
    uint8_t addr = mhc->get_next_dynamic_addr(hci, dat_offset);

    /* Start with a broadcast if they configured it. */
    if (ARRAY_FIELD_EX32(hci->core.regs, HC_CONTROL, IBA_INCLUDE)) {
        if (i3c_start_transfer(hci->bus, I3C_BROADCAST, desc->rnw)) {
            return RESP_STATUS_ERROR_ADDR_HEADER;
        }
    }

    if (i3c_start_transfer(hci->bus, addr, desc->rnw)) {
        return RESP_STATUS_ERROR_NACK;
    }
    return RESP_STATUS_SUCCESS;
}

static int hci_cmd_i2c_start_xfer(MIPIHCIState *hci, const RegularXfer *desc)
{
    uint16_t dat_offset = DAT_ENTRY_FROM_DEV_INDEX(desc->dev_index);
    uint8_t addr = FIELD_EX32(hci->dat.regs[dat_offset + R_TARGET_DAT],
                              TARGET_DAT, TARGET_STATIC_ADDRESS);

    if (legacy_i2c_start_transfer(hci->bus, addr, desc->rnw)) {
        return RESP_STATUS_ERROR_NACK;
    }
    return RESP_STATUS_SUCCESS;
}

static RespStatus hci_cmd_i2c_send_data(MIPIHCIState *hci,
                                        const RegularXfer *desc,
                                        RespDescr *resp, const uint8_t *data,
                                        size_t len)
{
    uint32_t num_sent = 0;

    /* Address the target and send the data. */
    RespStatus status = hci_cmd_i2c_start_xfer(hci, desc);
    if (status != RESP_STATUS_SUCCESS) {
        goto done;
    }

    for (num_sent = 0; num_sent < len; ++num_sent) {
        if (legacy_i2c_send(hci->bus, data[num_sent])) {
            status = RESP_STATUS_ERROR_XFER_ABORTED;
            break;
        }
    }

done:
    if (desc->toc) {
        legacy_i2c_end_transfer(hci->bus);
    }
    resp->resp.length = len - num_sent;
    return status;
}

static RespStatus hci_cmd_i3c_send_data(MIPIHCIState *hci,
                                        const RegularXfer *desc,
                                        RespDescr *resp, const uint8_t *data,
                                        size_t len)
{
    uint32_t num_sent = 0;

    /* Address the target and send the data. */
    RespStatus status = hci_cmd_i3c_start_xfer(hci, desc);
    if (status != RESP_STATUS_SUCCESS) {
        goto done;
    }

    if (i3c_send(hci->bus, data, len, &num_sent)) {
        status = RESP_STATUS_ERROR_XFER_ABORTED;
    }

done:
    if (desc->toc) {
        i3c_end_transfer(hci->bus);
    }
    resp->resp.length = len - num_sent;
    return status;
}

static RespStatus hci_cmd_send_data(MIPIHCIState *hci, const RegularXfer *desc,
                                     RespDescr *resp, const uint8_t *data,
                                     size_t len)
{
    uint16_t dat_offset = DAT_ENTRY_FROM_DEV_INDEX(desc->dev_index);

    if (FIELD_EX32(hci->dat.regs[dat_offset + R_TARGET_DAT], TARGET_DAT,
                   TARGET_DEVICE)) {
        return hci_cmd_i2c_send_data(hci, desc, resp, data, len);
    }
    return hci_cmd_i3c_send_data(hci, desc, resp, data, len);
}

RespStatus hci_cmd_send(MIPIHCIState *hci, const RegularXfer *desc,
                        RespDescr *resp, const uint8_t *data, size_t len) {
    RespStatus status = RESP_STATUS_SUCCESS;

    /* This is an internal error, the caller passed in bad arguments. */
    if (desc->cmd_attr != CMD_ATTR_REGULAR_XFER || desc->rnw) {
        status = RESP_STATUS_ERROR_HC_ABORTED;
        resp->resp.length = len;
        goto done;
    }
    /* We only support SDR. */
    if (desc->mode > TRANSFER_MODE_SDR4) {
        status = RESP_STATUS_ERROR_NOT_SUPPORTED;
        resp->resp.length = len;
        goto done;
    }

    if (desc->cp) {
        status = hci_cmd_send_ccc(hci, desc, resp, data, len);
    } else {
        status = hci_cmd_send_data(hci, desc, resp, data, len);
    }

done:
    resp->resp.tid = desc->tid;
    resp->resp.err = status;

    return status;
}

static RespStatus hci_cmd_i2c_read_data(MIPIHCIState *hci,
                                        const RegularXfer *desc,
                                        RespDescr *resp, uint8_t *data,
                                        uint32_t len, uint32_t *num_read)
{
    *num_read = 0;

    RespStatus status = hci_cmd_i2c_start_xfer(hci, desc);
    if (status != RESP_STATUS_SUCCESS) {
        goto done;
    }

    for (*num_read = 0; *num_read < len; ++*num_read) {
        data[*num_read] = legacy_i2c_recv(hci->bus);
    }

done:
    if (desc->toc) {
        legacy_i2c_end_transfer(hci->bus);
    }
    resp->resp.length = len - *num_read;
    return status;
}

static RespStatus hci_cmd_i3c_read_data(MIPIHCIState *hci,
                                        const RegularXfer *desc,
                                        RespDescr *resp, uint8_t *data,
                                        uint32_t len, uint32_t *num_read)
{
    *num_read = 0;

    RespStatus status = hci_cmd_i3c_start_xfer(hci, desc);
    if (status != RESP_STATUS_SUCCESS) {
        goto done;
    }

    if (i3c_recv(hci->bus, data, len, num_read)) {
        status = RESP_STATUS_ERROR_XFER_ABORTED;
    }
    /*
     * If we didn't read as much as we expected, and if the descriptor is
     * configured to treat this as an error, make it so.
     */
    if (desc->sre && *num_read != len) {
        status = RESP_STATUS_ERROR_XFER_ABORTED;
    }

done:
    if (desc->toc) {
        i3c_end_transfer(hci->bus);
    }
    resp->resp.length = len - *num_read;
    return status;
}

static RespStatus hci_cmd_read_data(MIPIHCIState *hci, const RegularXfer *desc,
                                     RespDescr *resp, uint8_t *data,
                                     uint32_t len, uint32_t *num_read)
{
    uint16_t dat_offset = DAT_ENTRY_FROM_DEV_INDEX(desc->dev_index);

    if (FIELD_EX32(hci->dat.regs[dat_offset + R_TARGET_DAT], TARGET_DAT,
                   TARGET_DEVICE)) {
        return hci_cmd_i2c_read_data(hci, desc, resp, data, len, num_read);
    }
    return hci_cmd_i3c_read_data(hci, desc, resp, data, len, num_read);
}

RespStatus hci_cmd_read(MIPIHCIState *hci, const RegularXfer *desc,
                        RespDescr *resp, uint8_t *data, uint32_t len,
                        uint32_t *num_read) {
    RespStatus status = RESP_STATUS_SUCCESS;
    /* This is an internal error, the caller passed in bad arguments. */
    if (desc->cmd_attr != CMD_ATTR_REGULAR_XFER || !desc->rnw) {
        status = RESP_STATUS_ERROR_HC_ABORTED;
        resp->resp.length = len;
        goto done;
    }
    /* We only support SDR. */
    if (desc->mode > TRANSFER_MODE_SDR4) {
        status = RESP_STATUS_ERROR_NOT_SUPPORTED;
        resp->resp.length = len;
        goto done;
    }

    if (desc->cp) {
        status = hci_cmd_read_ccc(hci, desc, resp, data, len, num_read);
    } else {
        status = hci_cmd_read_data(hci, desc, resp, data, len, num_read);
    }

done:
    resp->resp.tid = desc->tid;
    resp->resp.err = status;

    return status;
}

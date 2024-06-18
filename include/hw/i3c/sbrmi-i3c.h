/*
 * SBRMI I3C target device.
 * Based on PPR Vol 5 for AMD Family 1Ah Model 02h C0
 *
 * Copyright (c) 2024 Google LLC
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef SBRMI_I3C_H_
#define SBRMI_I3C_H_

#include "hw/i3c/i3c.h"

#define TYPE_SBRMI_I3C_TARGET "sbrmi-i3c-target"
OBJECT_DECLARE_SIMPLE_TYPE(SbrmiI3cTargetState, SBRMI_I3C_TARGET)

#define SBRMI_REV_20 (0x20)
#define SBRMI_REV_21 (0x21)

#define SBRMI_MAILBOX_CMD_DATA_IN_SIZE (4)
#define SBRMI_MAILBOX_CMD_DATA_OUT_SIZE (4)

/* TODO(b/347796186), support full list of register */
typedef enum {
    SBRMI_REG_REV = 0x0,
    SBRMI_REG_CONTROL,
    SBRMI_REG_STATUS,
    SBRMI_REG_OUTBNDMSG_INST0 = 0x30,
    SBRMI_REG_OUTBNDMSG_INST1,
    SBRMI_REG_OUTBNDMSG_INST2,
    SBRMI_REG_OUTBNDMSG_INST3,
    SBRMI_REG_OUTBNDMSG_INST4,
    SBRMI_REG_OUTBNDMSG_INST5,
    SBRMI_REG_OUTBNDMSG_INST6,
    SBRMI_REG_OUTBNDMSG_INST7,
    SBRMI_REG_INBNDMSG_INST0 = 0x38,
    SBRMI_REG_INBNDMSG_INST1,
    SBRMI_REG_INBNDMSG_INST2,
    SBRMI_REG_INBNDMSG_INST3,
    SBRMI_REG_INBNDMSG_INST4,
    SBRMI_REG_INBNDMSG_INST5,
    SBRMI_REG_INBNDMSG_INST6,
    SBRMI_REG_INBNDMSG_INST7,
    SBRMI_REG_SW_INTERRUPT = 0x40,
    SBRMI_REG_RAS_STATUS = 0x4c,
} SbrmiReg;

/* TODO(b/347796186), support full list of command */
typedef enum {
    SBRMI_MAILBOX_CMD_READ_PACKAGE_POWER_CONSUMPTION = 0x1,
    SBRMI_MAILBOX_CMD_READ_PACKAGE_POWER_LIMIT = 0x3,
    SBRMI_MAILBOX_CMD_READ_MAX_PACKAGE_POWER_LIMIT = 0x4,
    SBRMI_MAILBOX_CMD_GET_DIMM_THERMAL_SENSOR = 0x48,
} SbrmiMailboxCmd;

/* mailbox error code */
typedef enum {
    SBRMI_MAILBOX_ERROR_NONE = 0x0,
    SBRMI_MAILBOX_COMMAND_ABORTED,
    SBRMI_MAILBOX_COMMAND_UNKNOWN,
    SBRMI_MAILBOX_INVALID_CORE,
    SBRMI_MAILBOX_INVALID_INPUT_ARG = 0x9,
    SBRMI_MAILBOX_INVALID_OOB_RAS_CONFIG,
} SbrmiMailboxError;

/* BIT definition for SBRMI_REG_CONTROL */
#define SBRMI_BIT_ALERT_MASK (0)
#define SBRMI_BIT_ALERT_MASK_LEN (1)
#define SBRMI_BIT_MB_CMPL_SW_ALERT_ENABLE (5)
#define SBRMI_BIT_MB_CMPL_SW_ALERT_ENABLE_LEN (1)

/* BIT definition for SBRMI_REG_STATUS */
#define SBRMI_BIT_SW_ALERT_STATUS (1)
#define SBRMI_BIT_SW_ALERT_STATUS_LEN (1)

/* BIT definition for SBRMI_REG_SW_INTERRUPT */
#define SBRMI_BIT_SWINT (0)
#define SBRMI_BIT_SWINT_LEN (1)

/* BIT definition for SBRMI_REG_INBNDMSG_INST7 */
#define SBRMI_BIT_INBNDMSG_INST7_MB_SEND (7)
#define SBRMI_BIT_INBNDMSG_INST7_MB_SEND_LEN (1)

struct SbrmiI3cTargetState {
    I3CTarget i3c;

    /* General device state */
    I3CEvent curr_event;
    uint16_t command_code;
    uint8_t command_code_received;

    /* sbrmi registers state */
    uint8_t sbrmi_control;
    uint8_t sbrmi_status;

    /* mailbox state */
    SbrmiMailboxCmd mailbox_command;
    uint8_t mailbox_data_in[SBRMI_MAILBOX_CMD_DATA_IN_SIZE];
    uint8_t mailbox_data_out[SBRMI_MAILBOX_CMD_DATA_OUT_SIZE];
    SbrmiMailboxError mailbox_error;

    struct {
        char *name;
        /* revision 0x21: 2-bytes command code. 0x20: 1-byte command code. */
        uint8_t sbrmi_rev;
    } cfg;
};

#endif  /* SBRMI_I3C_H_ */

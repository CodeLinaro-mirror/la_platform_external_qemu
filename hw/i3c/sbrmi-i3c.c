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

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "hw/i3c/i3c.h"
#include "hw/i3c/sbrmi-i3c.h"
#include "hw/qdev-properties.h"
#include "qemu/error-report.h"
#include "trace.h"

static bool sbrmi_i3c_target_command_code_complete(SbrmiI3cTargetState *s)
{
    switch (s->cfg.sbrmi_rev) {
    case SBRMI_REV_21:
        return (s->command_code_received == 2);
    case SBRMI_REV_20:
        return (s->command_code_received == 1);
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "Unsupported sbrmi revision %d\n",
                      s->cfg.sbrmi_rev);
        return false;
    }
}

static int sbrmi_i3c_target_mailbox_handler(SbrmiI3cTargetState *s)
{
    switch (s->mailbox_command) {
    case SBRMI_MAILBOX_CMD_GET_DIMM_THERMAL_SENSOR:
    case SBRMI_MAILBOX_CMD_READ_PACKAGE_POWER_LIMIT:
    case SBRMI_MAILBOX_CMD_READ_MAX_PACKAGE_POWER_LIMIT:
    case SBRMI_MAILBOX_CMD_READ_PACKAGE_POWER_CONSUMPTION:
        /* TODO(b/347796186): return mock data */
        s->mailbox_data_out[0] = 0x0;
        s->mailbox_data_out[1] = 0x0;
        s->mailbox_data_out[2] = 0x0;
        s->mailbox_data_out[3] = 0x0;
        s->mailbox_error = SBRMI_MAILBOX_ERROR_NONE;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "Unhandled mailbox command 0x%.2x\n",
                      s->mailbox_command);
        return -1;
    }
    return 0;
}

static void sbrmi_i3c_target_mailbox_reset(SbrmiI3cTargetState *s)
{
    /* SB-RMI Soft Mailbox Message. 0 is not defined */
    s->mailbox_command = 0;
    s->mailbox_error = SBRMI_MAILBOX_ERROR_NONE;
    memset(s->mailbox_data_in, 0, sizeof(s->mailbox_data_in));
    memset(s->mailbox_data_out, 0, sizeof(s->mailbox_data_out));
}

static uint32_t sbrmi_i3c_target_rx(I3CTarget *i3c, uint8_t *data,
                               uint32_t num_to_read)
{
    SbrmiI3cTargetState *s = SBRMI_I3C_TARGET(i3c);

    if (s->curr_event != I3C_START_RECV) {
        qemu_log_mask(LOG_GUEST_ERROR, "Unexpected rx in event=%d\n",
                      s->curr_event);
        return -1;
    }

    if (!sbrmi_i3c_target_command_code_complete(s)) {
        /* incomplete command code on read */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Unexpected rx while receiving command code\n");
        return -1;
    }

    switch (s->command_code) {
    case SBRMI_REG_REV:
        *data = s->cfg.sbrmi_rev;
        break;
    case SBRMI_REG_RAS_STATUS:
        /* TODO(b/347796186): support RAS */
        *data = 0;
        break;
    case SBRMI_REG_STATUS:
        *data = s->sbrmi_status;
        break;
    case SBRMI_REG_OUTBNDMSG_INST0:
        *data = s->mailbox_command;
        break;
    case SBRMI_REG_OUTBNDMSG_INST1:
        *data = s->mailbox_data_out[0];
        break;
    case SBRMI_REG_OUTBNDMSG_INST2:
        *data = s->mailbox_data_out[1];
        break;
    case SBRMI_REG_OUTBNDMSG_INST3:
        *data = s->mailbox_data_out[2];
        break;
    case SBRMI_REG_OUTBNDMSG_INST4:
        *data = s->mailbox_data_out[3];
        break;
    case SBRMI_REG_OUTBNDMSG_INST7:
        *data = s->mailbox_error;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "Unhandled command 0x%.2x\n",
                      s->command_code);
        return -1;
    }

    if (num_to_read == 1) {
        trace_sbrmi_i3c_target_rx_single(s->cfg.name, s->command_code, *data);
    } else {
        trace_sbrmi_i3c_target_rx(s->cfg.name, s->command_code, num_to_read);

    }

    return num_to_read;
}

static int sbrmi_i3c_target_tx(I3CTarget *i3c, const uint8_t *data,
                          uint32_t num_to_send, uint32_t *num_sent)
{
    SbrmiI3cTargetState *s = SBRMI_I3C_TARGET(i3c);

    if (s->curr_event != I3C_START_SEND) {
        qemu_log_mask(LOG_GUEST_ERROR, "Unexpected tx in event=%d\n",
                      s->curr_event);
        return -1;
    }

    if (num_to_send == 1) {
        trace_sbrmi_i3c_target_tx_single(s->cfg.name, s->command_code, *data);
    } else {
        trace_sbrmi_i3c_target_tx(s->cfg.name, s->command_code, num_to_send);
    }

    if (!sbrmi_i3c_target_command_code_complete(s)) {
        /* receiving command code */
        s->command_code |= ((*data) << s->command_code_received);
        s->command_code_received++;
        *num_sent = 1;
        return 0;
    }

    switch (s->command_code) {
    case SBRMI_REG_STATUS:
        /* write one clear */
        if (extract8(*data, SBRMI_BIT_SW_ALERT_STATUS,
                     SBRMI_BIT_SW_ALERT_STATUS_LEN)) {
            s->sbrmi_status = deposit32(s->sbrmi_status,
                                        SBRMI_BIT_SW_ALERT_STATUS,
                                        SBRMI_BIT_SW_ALERT_STATUS_LEN, 0);
        }
        break;
    case SBRMI_REG_INBNDMSG_INST0:
        /* sbrmi mailbox command start */
        s->mailbox_command = *data;
        break;
    case SBRMI_REG_INBNDMSG_INST1:
        s->mailbox_data_in[0] = *data;
        break;
    case SBRMI_REG_INBNDMSG_INST2:
        s->mailbox_data_in[1] = *data;
        break;
    case SBRMI_REG_INBNDMSG_INST3:
        s->mailbox_data_in[2] = *data;
        break;
    case SBRMI_REG_INBNDMSG_INST4:
        s->mailbox_data_in[3] = *data;
        break;
    case SBRMI_REG_INBNDMSG_INST7:
        /* Set bit 7 to 1 to send message to firmware */
        if (extract8(*data, SBRMI_BIT_INBNDMSG_INST7_MB_SEND,
                     SBRMI_BIT_INBNDMSG_INST7_MB_SEND_LEN)) {
            sbrmi_i3c_target_mailbox_reset(s);
        }
        break;
    case SBRMI_REG_SW_INTERRUPT:
        /* Write 1 to indicate a firmware mailbox service request. */
        /* Call the sbrmi target mailbox handler */
        if (extract8(*data, SBRMI_BIT_SWINT, SBRMI_BIT_SWINT_LEN)) {
            if (sbrmi_i3c_target_mailbox_handler(s)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "sbrmi_i3c_target_mailbox_handler failed\n");
                return -1;
            }

            /* Request complete */
            if (extract8(s->sbrmi_control, SBRMI_BIT_MB_CMPL_SW_ALERT_ENABLE,
                         SBRMI_BIT_MB_CMPL_SW_ALERT_ENABLE_LEN)) {
                s->sbrmi_status = deposit32(s->sbrmi_status,
                                            SBRMI_BIT_SW_ALERT_STATUS,
                                            SBRMI_BIT_SW_ALERT_STATUS_LEN, 1);
            } else {
                /*
                 * We should cleard the SBRMI::SoftwareInterrupt
                 * when SBRMI::Control[MbCmplSwAlertEnable]==0.
                 * We don't do it because we are not saving the register today.
                 */
            }
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "Unhandled command 0x%.2x\n",
                      s->command_code);
        return -1;
    }

    *num_sent = num_to_send;
    return 0;
}

static int sbrmi_i3c_target_handle_ccc_read(I3CTarget *i3c, uint8_t *data,
                                       uint32_t num_to_read, uint32_t *num_read)
{
    return 0;
}

static int sbrmi_i3c_target_handle_ccc_write(I3CTarget *i3c,
                                        const uint8_t *data,
                                        uint32_t num_to_send,
                                        uint32_t *num_sent)
{
    return 0;
}

static int sbrmi_i3c_target_event(I3CTarget *i3c, enum I3CEvent event)
{
    SbrmiI3cTargetState *s = SBRMI_I3C_TARGET(i3c);

    switch (event) {
    case I3C_START_RECV:
        break;
    case I3C_START_SEND:
        if (s->curr_event == I3C_STOP) {
            /* New SEND event, reset the command code */
            s->command_code = 0;
            s->command_code_received = 0;
        }
        break;
    case I3C_STOP:
        break;
    case I3C_NACK:
        break;
    }

    trace_sbrmi_i3c_target_event(s->cfg.name, event);

    /* Update the event */
    s->curr_event = event;
    return 0;
}

static void sbrmi_i3c_target_reset(I3CTarget *i3c)
{
    SbrmiI3cTargetState *s = SBRMI_I3C_TARGET(i3c);
    s->curr_event = I3C_STOP;
    s->command_code = 0;
    s->command_code_received = 0;
    s->sbrmi_status = 0;
    s->sbrmi_control = deposit32(s->sbrmi_control, SBRMI_BIT_ALERT_MASK,
                                 SBRMI_BIT_ALERT_MASK_LEN, 1);
    s->sbrmi_control = deposit32(s->sbrmi_control,
                                 SBRMI_BIT_MB_CMPL_SW_ALERT_ENABLE,
                                 SBRMI_BIT_MB_CMPL_SW_ALERT_ENABLE_LEN, 1);


    sbrmi_i3c_target_mailbox_reset(s);
}

static void sbrmi_i3c_target_realize(DeviceState *dev, Error **errp)
{
    SbrmiI3cTargetState *s = SBRMI_I3C_TARGET(dev);
    sbrmi_i3c_target_reset(&s->i3c);
}

static void sbrmi_i3c_target_init(Object *obj)
{
    /*TODO(b/347796186): add target property for mocking e.g. temperature */
    return;
}

static const Property sbrmi_i3c_props[] = {
    DEFINE_PROP_STRING("device-name", SbrmiI3cTargetState, cfg.name),
    DEFINE_PROP_UINT8("sbrmi-rev", SbrmiI3cTargetState, cfg.sbrmi_rev,
                      SBRMI_REV_21),
};

static void sbrmi_i3c_target_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I3CTargetClass *k = I3C_TARGET_CLASS(klass);

    dc->realize = sbrmi_i3c_target_realize;
    k->event = sbrmi_i3c_target_event;
    k->recv = sbrmi_i3c_target_rx;
    k->send = sbrmi_i3c_target_tx;
    k->handle_ccc_read = sbrmi_i3c_target_handle_ccc_read;
    k->handle_ccc_write = sbrmi_i3c_target_handle_ccc_write;

    device_class_set_props(dc, sbrmi_i3c_props);
}

static const TypeInfo sbrmi_i3c_target_info = {
    .name          = TYPE_SBRMI_I3C_TARGET,
    .parent        = TYPE_I3C_TARGET,
    .instance_size = sizeof(SbrmiI3cTargetState),
    .instance_init = sbrmi_i3c_target_init,
    .class_init    = sbrmi_i3c_target_class_init,
};

static void sbrmi_i3c_target_register_types(void)
{
    type_register_static(&sbrmi_i3c_target_info);
}

type_init(sbrmi_i3c_target_register_types)

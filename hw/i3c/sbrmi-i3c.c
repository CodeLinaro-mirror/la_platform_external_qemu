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
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "trace.h"

I3CTarget *create_sbrmi_i3c_target(const char *name, uint8_t addr,
                                   uint64_t pid, const char *cpu_vendor,
                                   uint32_t ucode_rev)
{
    I3CTarget *target = i3c_target_new(TYPE_SBRMI_I3C_TARGET, addr,
                             /*dcr=*/0, /*bcr=*/0, pid);
    object_property_set_str(OBJECT(target), "device-name", name,
                               &error_abort);
    /* cpu register mockup */
    object_property_set_str(OBJECT(target), "cpu_vendor", cpu_vendor,
                               &error_abort);
    object_property_set_uint(OBJECT(target), "cpu_version", 0x0b00f20,
                               &error_abort);
    object_property_set_uint(OBJECT(target), "apic_id", 0,
                               &error_abort);
    object_property_set_uint(OBJECT(target), "nr_cores", 128,
                               &error_abort);
    object_property_set_uint(OBJECT(target), "nr_thread", 1,
                               &error_abort);
    object_property_set_uint(OBJECT(target), "ecx_fn1", 0xffffffff,
                               &error_abort);
    object_property_set_uint(OBJECT(target), "edx_fn1", 0xffffffff,
                               &error_abort);
    object_property_set_uint(OBJECT(target), "ucode_rev", ucode_rev,
                               &error_abort);
    target->address = 0;

    return target;
}

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

static int sbrmi_i3c_target_mb_get_dimm_thermal_sensor(SbrmiI3cTargetState *s)
{
    uint32_t dimm_address = extract32(s->mailbox_data_in,
                                      GET_DIMM_THERMAL_DI_DIMM_ADDR,
                                      GET_DIMM_THERMAL_DI_DIMM_ADDR_LEN);
    uint32_t umc_index = extract32(dimm_address, UMC_DIMM_ADDR_ID,
                                   UMC_DIMM_ADDR_ID_LEN);
    uint32_t mode = extract32(dimm_address, UMC_DIMM_ADDR_MODE,
                              UMC_DIMM_ADDR_MODE_LEN);
    uint32_t ts = extract32(dimm_address, UMC_DIMM_ADDR_TS,
                            UMC_DIMM_ADDR_TS_LEN);
    uint32_t dimm = extract32(dimm_address, UMC_DIMM_ADDR_DIMM,
                              UMC_DIMM_ADDR_DIMM_LEN);
    uint32_t data_out = 0;

    if (!mode) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Dimm address encode mode 0 is not supported\n");
        return -1;
    }

    if (ts) {
        qemu_log_mask(LOG_GUEST_ERROR, "Dimm address TS1 is not supported\n");
        return -1;
    }

    if (dimm) {
        qemu_log_mask(LOG_GUEST_ERROR, "Dimm address Dimm1 is not supported\n");
        return -1;
    }

    /* prepare for data out */
    data_out = deposit32(data_out, GET_DIMM_THERMAL_DO_DIMM_ADDR,
                         GET_DIMM_THERMAL_DO_DIMM_ADDR_LEN,
                         dimm_address);
    data_out = deposit32(data_out, GET_DIMM_THERMAL_DO_UPDATE_RATE,
                         GET_DIMM_THERMAL_DO_UPDATE_RATE_LEN,
                         s->umc[umc_index].dimm[dimm].update_rate[ts]);
    data_out = deposit32(data_out, GET_DIMM_THERMAL_DO_TEMP,
                         GET_DIMM_THERMAL_DO_TEMP_LEN,
                         s->umc[umc_index].dimm[dimm].temp[ts]);

    s->mailbox_data_out = data_out;

    trace_sbrmi_i3c_target_mb_get_dimm_thermal_sensor(s->cfg.name,
                                                      s->mailbox_data_in,
                                                      s->mailbox_data_out);
    return 0;
}

static int sbrmi_i3c_target_mb_read_power_limit(SbrmiI3cTargetState *s)
{
    s->mailbox_data_out = s->power_limit;

    trace_sbrmi_i3c_target_mb_read_power_limit(s->cfg.name,
                                               s->mailbox_data_out);
    return 0;
}

static int sbrmi_i3c_target_mb_read_max_power_limit(SbrmiI3cTargetState *s)
{
    s->mailbox_data_out = s->max_power_limit;

    trace_sbrmi_i3c_target_mb_read_max_power_limit(s->cfg.name,
                                                   s->mailbox_data_out);
    return 0;
}

static int sbrmi_i3c_target_mb_read_power(SbrmiI3cTargetState *s)
{
    s->mailbox_data_out = s->power;

    trace_sbrmi_i3c_target_mb_read_power(s->cfg.name, s->mailbox_data_out);
    return 0;
}

static int sbrmi_i3c_target_mb_get_ucode_revision(SbrmiI3cTargetState *s)
{
    s->mailbox_data_out = s->cpu.ucode_rev;
    trace_sbrmi_i3c_target_mb_get_ucode_revision(s->cfg.name,
                                                 s->mailbox_data_out);
    return 0;
}

static int sbrmi_i3c_target_mb_get_dimm_power_consumption(
                                                SbrmiI3cTargetState *s)
{
    uint32_t dimm_address = extract32(s->mailbox_data_in,
                                      GET_DIMM_POWER_DI_DIMM_ADDR,
                                      GET_DIMM_POWER_DI_DIMM_ADDR_LEN);
    uint32_t umc_index = extract32(dimm_address, UMC_DIMM_ADDR_ID,
                                   UMC_DIMM_ADDR_ID_LEN);
    uint32_t mode = extract32(dimm_address, UMC_DIMM_ADDR_MODE,
                              UMC_DIMM_ADDR_MODE_LEN);
    uint32_t ts = extract32(dimm_address, UMC_DIMM_ADDR_TS,
                            UMC_DIMM_ADDR_TS_LEN);
    uint32_t dimm = extract32(dimm_address, UMC_DIMM_ADDR_DIMM,
                              UMC_DIMM_ADDR_DIMM_LEN);
    uint32_t data_out = 0;

    if (!mode) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Dimm address encode mode 0 is not supported\n");
        return -1;
    }

    if (ts) {
        qemu_log_mask(LOG_GUEST_ERROR, "Dimm address TS1 is not supported\n");
        return -1;
    }

    if (dimm) {
        qemu_log_mask(LOG_GUEST_ERROR, "Dimm address Dimm1 is not supported\n");
        return -1;
    }

    /* prepare for data out */
    data_out = deposit32(data_out, GET_DIMM_POWER_DO_DIMM_ADDR,
                         GET_DIMM_POWER_DO_DIMM_ADDR_LEN,
                         dimm_address);
    data_out = deposit32(data_out, GET_DIMM_POWER_DO_UPDATE_RATE,
                         GET_DIMM_POWER_DO_UPDATE_RATE_LEN,
                         s->umc[umc_index].dimm[dimm].update_rate[ts]);
    data_out = deposit32(data_out, GET_DIMM_POWER_DO_POWER,
                         GET_DIMM_POWER_DO_POWER_LEN,
                         s->umc[umc_index].dimm[dimm].power);

    s->mailbox_data_out = data_out;

    trace_sbrmi_i3c_target_mb_get_dimm_power_consumption(s->cfg.name,
                                                      s->mailbox_data_in,
                                                      s->mailbox_data_out);
    return 0;
}

static int sbrmi_i3c_target_mb_unknown_command(SbrmiI3cTargetState *s)
{
    s->mailbox_data_out = 0;

    trace_sbrmi_i3c_target_mb_unknown_command(s->cfg.name,
                                              s->mailbox_command,
                                              s->mailbox_data_in,
                                              s->mailbox_data_out);
    return 0;
}

static int sbrmi_i3c_target_mailbox_handler(SbrmiI3cTargetState *s)
{
    switch (s->mailbox_command) {
    case SBRMI_MAILBOX_CMD_GET_DIMM_POWER_CONSUMPTION:
        return sbrmi_i3c_target_mb_get_dimm_power_consumption(s);
    case SBRMI_MAILBOX_CMD_GET_DIMM_THERMAL_SENSOR:
        return sbrmi_i3c_target_mb_get_dimm_thermal_sensor(s);
    case SBRMI_MAILBOX_CMD_READ_PACKAGE_POWER_LIMIT:
        return sbrmi_i3c_target_mb_read_power_limit(s);
    case SBRMI_MAILBOX_CMD_READ_MAX_PACKAGE_POWER_LIMIT:
        return sbrmi_i3c_target_mb_read_max_power_limit(s);
    case SBRMI_MAILBOX_CMD_READ_PACKAGE_POWER_CONSUMPTION:
        return sbrmi_i3c_target_mb_read_power(s);
    case SBRMI_MAILBOX_CMD_GET_UCODE_REVISION:
        return sbrmi_i3c_target_mb_get_ucode_revision(s);
    default:
        return sbrmi_i3c_target_mb_unknown_command(s);
    }
    return 0;
}

static void sbrmi_i3c_target_mailbox_reset(SbrmiI3cTargetState *s)
{
    /* SB-RMI Soft Mailbox Message. 0 is not defined */
    s->mailbox_command = 0;
    s->mailbox_error = SBRMI_MAILBOX_ERROR_NONE;
    s->mailbox_data_in = 0;
    s->mailbox_data_out = 0;
}

static void sbrmi_i3c_target_cpuid_read_handler(SbrmiI3cTargetState *s)
{
    int read_length =
            s->cpu_reg_write_data[SBRMI_READ_CPUID_WD_READ_LENGTH] + 1;
    uint32_t cpuid_fn =
            s->cpu_reg_write_data[SBRMI_READ_CPUID_WD_FUNCTION_0] |
            (s->cpu_reg_write_data[SBRMI_READ_CPUID_WD_FUNCTION_1] << 8) |
            (s->cpu_reg_write_data[SBRMI_READ_CPUID_WD_FUNCTION_2] << 16) |
            (s->cpu_reg_write_data[SBRMI_READ_CPUID_WD_FUNCTION_3] << 24);
    uint8_t is_ecx_edx = extract8(
        s->cpu_reg_write_data[SBRMI_READ_CPUID_WD_WRITE_DATA_9], 0, 1);

    /* prepare for data out */
    memset(s->cpu_reg_read_data, 0, sizeof(s->cpu_reg_read_data));
    s->cpu_reg_read_data[SBRMI_READ_CPUID_RD_READ_LENGTH] = read_length;
    s->cpu_reg_read_data[SBRMI_READ_CPUID_RD_STATUS] = 0;

    /* See PPR Vol 1 for AMD Family 1Ah Model 02h C0, 2.1.18 */
    switch (cpuid_fn) {
    case 0:
        if (is_ecx_edx) {
            /* ECX = vendor3 */
            s->cpu_reg_read_data[SBRMI_READ_CPUID_RD_EAX_ECX_0] =
                    extract32(s->cpu.vendor3, 0, 8);
            s->cpu_reg_read_data[SBRMI_READ_CPUID_RD_EAX_ECX_1] =
                    extract32(s->cpu.vendor3, 8, 8);
            s->cpu_reg_read_data[SBRMI_READ_CPUID_RD_EAX_ECX_2] =
                    extract32(s->cpu.vendor3, 16, 8);
            s->cpu_reg_read_data[SBRMI_READ_CPUID_RD_EAX_ECX_3] =
                    extract32(s->cpu.vendor3, 24, 8);
            /* EDX = vendor2 */
            s->cpu_reg_read_data[SBRMI_READ_CPUID_RD_EBX_EDX_0] =
                    extract32(s->cpu.vendor2, 0, 8);
            s->cpu_reg_read_data[SBRMI_READ_CPUID_RD_EBX_EDX_1] =
                    extract32(s->cpu.vendor2, 8, 8);
            s->cpu_reg_read_data[SBRMI_READ_CPUID_RD_EBX_EDX_2] =
                    extract32(s->cpu.vendor2, 16, 8);
            s->cpu_reg_read_data[SBRMI_READ_CPUID_RD_EBX_EDX_3] =
                    extract32(s->cpu.vendor2, 24, 8);
        } else {
            /* EAX ignored for now */
            /* EBX = vendor1 */
            s->cpu_reg_read_data[SBRMI_READ_CPUID_RD_EBX_EDX_0] =
                    extract32(s->cpu.vendor1, 0, 8);
            s->cpu_reg_read_data[SBRMI_READ_CPUID_RD_EBX_EDX_1] =
                    extract32(s->cpu.vendor1, 8, 8);
            s->cpu_reg_read_data[SBRMI_READ_CPUID_RD_EBX_EDX_2] =
                    extract32(s->cpu.vendor1, 16, 8);
            s->cpu_reg_read_data[SBRMI_READ_CPUID_RD_EBX_EDX_3] =
                    extract32(s->cpu.vendor1, 24, 8);
        }
        break;
    case 1:
        if (is_ecx_edx) {
            /* ECX = ecx_fn1 */
            s->cpu_reg_read_data[SBRMI_READ_CPUID_RD_EAX_ECX_0] =
                    extract32(s->cpu.ecx_fn1, 0, 8);
            s->cpu_reg_read_data[SBRMI_READ_CPUID_RD_EAX_ECX_1] =
                    extract32(s->cpu.ecx_fn1, 8, 8);
            s->cpu_reg_read_data[SBRMI_READ_CPUID_RD_EAX_ECX_2] =
                    extract32(s->cpu.ecx_fn1, 16, 8);
            s->cpu_reg_read_data[SBRMI_READ_CPUID_RD_EAX_ECX_3] =
                    extract32(s->cpu.ecx_fn1, 24, 8);
            /* EDX = edx_fn1 */
            s->cpu_reg_read_data[SBRMI_READ_CPUID_RD_EBX_EDX_0] =
                    extract32(s->cpu.edx_fn1, 0, 8);
            s->cpu_reg_read_data[SBRMI_READ_CPUID_RD_EBX_EDX_1] =
                    extract32(s->cpu.edx_fn1, 8, 8);
            s->cpu_reg_read_data[SBRMI_READ_CPUID_RD_EBX_EDX_2] =
                    extract32(s->cpu.edx_fn1, 16, 8);
            s->cpu_reg_read_data[SBRMI_READ_CPUID_RD_EBX_EDX_3] =
                    extract32(s->cpu.edx_fn1, 24, 8);
        } else {
            /* EAX = version */
            s->cpu_reg_read_data[SBRMI_READ_CPUID_RD_EAX_ECX_0] =
                    extract32(s->cpu.version, 0, 8);
            s->cpu_reg_read_data[SBRMI_READ_CPUID_RD_EAX_ECX_1] =
                    extract32(s->cpu.version, 8, 8);
            s->cpu_reg_read_data[SBRMI_READ_CPUID_RD_EAX_ECX_2] =
                    extract32(s->cpu.version, 16, 8);
            s->cpu_reg_read_data[SBRMI_READ_CPUID_RD_EAX_ECX_3] =
                    extract32(s->cpu.version, 24, 8);
            /*
             * EBX[15:8] = (CLFlush fixed 8, see CPUID_Fn00000001_EBX
             * in PPR Vol 1 for AMD Family 1Ah Model 02h C0)
             */
            s->cpu_reg_read_data[SBRMI_READ_CPUID_RD_EBX_EDX_1] = 8;
            /* EBX[23:16] = (nr_cores * nr_thread) */
            s->cpu_reg_read_data[SBRMI_READ_CPUID_RD_EBX_EDX_2] =
                    (s->cpu.nr_cores * s->cpu.nr_thread) & 0xff;
            /* EBX[31:24] = apic_id */
            s->cpu_reg_read_data[SBRMI_READ_CPUID_RD_EBX_EDX_3] =
                    s->cpu.apic_id & 0xff;
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Unsupported CPUID function 0x%.8x\n", cpuid_fn);
        return;
    }

    s->cpu_reg_read_data_ptr = 0;
}
static bool sbrmi_i3c_target_cpu_reg_write_data_complete(SbrmiI3cTargetState *s)
{
    int write_length = 0;

    /* make sure we received the data length */
    if (!s->cpu_reg_write_data_ptr) {
        return false;
    }
    write_length =
        s->cpu_reg_write_data[SBRMI_READ_CPUID_WD_WRITE_LENGTH] + 1;

    return (s->cpu_reg_write_data_ptr >= write_length);
}

static int sbrmi_i3c_target_cpu_reg_handler(SbrmiI3cTargetState *s)
{
    /* Write phase completed, dispatch the command based on code */
    int command_code =
        s->cpu_reg_write_data[SBRMI_READ_CPUID_WD_COMMAND];

    trace_sbrmi_i3c_target_cpu_reg_handler(s->cfg.name, command_code);

    switch (command_code) {
    case SBRMI_READ_CPUID_COMMAND_CODE:
        sbrmi_i3c_target_cpuid_read_handler(s);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Unsupported CPUID command 0x%.2x\n", command_code);
        return -1;
    }

    /*
     * Set HwAlertSts to indicate a command complete.
     * Will be cleared by BMC after reading the data.
     */
    s->sbrmi_status = deposit32(s->sbrmi_status,
                                SBRMI_BIT_HW_ALERT_STATUS,
                                SBRMI_BIT_HW_ALERT_STATUS_LEN, 1);
    return 0;
}

static void sbrmi_i3c_target_cpu_reg_reset(SbrmiI3cTargetState *s)
{
    s->cpu_reg_write_data_ptr = 0;
    s->cpu_reg_read_data_ptr = 0;
    memset(s->cpu_reg_write_data, 0, sizeof(s->cpu_reg_write_data));
    memset(s->cpu_reg_read_data, 0, sizeof(s->cpu_reg_read_data));
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
        *data = extract32(s->mailbox_data_out, 0, 8);
        break;
    case SBRMI_REG_OUTBNDMSG_INST2:
        *data = extract32(s->mailbox_data_out, 8, 8);
        break;
    case SBRMI_REG_OUTBNDMSG_INST3:
        *data = extract32(s->mailbox_data_out, 16, 8);
        break;
    case SBRMI_REG_OUTBNDMSG_INST4:
        *data = extract32(s->mailbox_data_out, 24, 8);
        break;
    case SBRMI_REG_OUTBNDMSG_INST7:
        *data = s->mailbox_error;
        break;
    case SBRMI_REG_THREADNUMBER:
        *data = s->cpu.nr_thread;
        break;
    case SBRMI_READ_CPU_REG_CMD:
    {
        /* total read bytes is the read data length + 1(length) */
        int remain_read = (s->cpu_reg_read_data[0] + 1) -
                           s->cpu_reg_read_data_ptr;
        if (remain_read > num_to_read) {
            /* copy num_to_read bytes to data and increase the ptr */
            memcpy(data, s->cpu_reg_read_data + s->cpu_reg_read_data_ptr,
                   num_to_read);
            s->cpu_reg_read_data_ptr += num_to_read;
        } else {
            /* copy all remaining bytes to data and increase the ptr */
            memcpy(data, s->cpu_reg_read_data + s->cpu_reg_read_data_ptr,
                   remain_read);
            s->cpu_reg_read_data_ptr += remain_read;
            /* complete the cpu register read phase. */
            sbrmi_i3c_target_cpu_reg_reset(s);
        }
        break;
    }
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

    if (!sbrmi_i3c_target_command_code_complete(s)) {
        /* receiving command code */
        s->command_code |= ((*data) << s->command_code_received);
        s->command_code_received++;
        *num_sent = 1;
        trace_sbrmi_i3c_target_tx_new_command(s->cfg.name, s->command_code,
                                              s->command_code_received);
        return 0;
    }

    /* command code is complete*/
    if (num_to_send == 1) {
        trace_sbrmi_i3c_target_tx_single(s->cfg.name, s->command_code, *data);
    } else {
        trace_sbrmi_i3c_target_tx(s->cfg.name, s->command_code, num_to_send);
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
        if (extract8(*data, SBRMI_BIT_HW_ALERT_STATUS,
                     SBRMI_BIT_HW_ALERT_STATUS_LEN)) {
            s->sbrmi_status = deposit32(s->sbrmi_status,
                                        SBRMI_BIT_HW_ALERT_STATUS,
                                        SBRMI_BIT_HW_ALERT_STATUS_LEN, 0);
        }
        break;
    case SBRMI_REG_INBNDMSG_INST0:
        /* sbrmi mailbox command start */
        s->mailbox_command = *data;
        break;
    case SBRMI_REG_INBNDMSG_INST1:
        s->mailbox_data_in = deposit32(s->mailbox_data_in, 0, 8, *data);
        break;
    case SBRMI_REG_INBNDMSG_INST2:
        s->mailbox_data_in = deposit32(s->mailbox_data_in, 8, 8, *data);
        break;
    case SBRMI_REG_INBNDMSG_INST3:
        s->mailbox_data_in = deposit32(s->mailbox_data_in, 16, 8, *data);
        break;
    case SBRMI_REG_INBNDMSG_INST4:
        s->mailbox_data_in = deposit32(s->mailbox_data_in, 24, 8, *data);
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
    case SBRMI_READ_CPU_REG_CMD:
        /* Save the write data to cpu_reg_write_data */
        if ((s->cpu_reg_write_data_ptr + num_to_send) >
            sizeof(s->cpu_reg_write_data)) {
            qemu_log_mask(LOG_GUEST_ERROR, "CPUID write data overflow\n");
            return -1;
        }
        memcpy(s->cpu_reg_write_data + s->cpu_reg_write_data_ptr, data,
               num_to_send);
        s->cpu_reg_write_data_ptr += num_to_send;

        /* Check write data completed. */
        if (sbrmi_i3c_target_cpu_reg_write_data_complete(s)) {
            if (sbrmi_i3c_target_cpu_reg_handler(s)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "sbrmi_i3c_target_cpuid_write_handler failed\n");
                return -1;
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

static void sbrmi_i3c_get_cpu_vendor(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp)
{
    SbrmiI3cTargetState *s = SBRMI_I3C_TARGET(obj);
    g_autofree char *value = g_new0(char, CPUID_VENDOR_SZ + 1);

    /* uint32_t to vendor string*/
    for (int i = 0; i < 4; i++) {
        value[i] = s->cpu.vendor1 >> (8 * i);
        value[i + 4] = s->cpu.vendor2 >> (8 * i);
        value[i + 8] = s->cpu.vendor3 >> (8 * i);
    }
    value[CPUID_VENDOR_SZ] = '\0';

    visit_type_str(v, name, &value, errp);
}

static void sbrmi_i3c_set_cpu_vendor(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp)
{
    SbrmiI3cTargetState *s = SBRMI_I3C_TARGET(obj);
    char *value;

    if (!visit_type_str(v, name, &value, errp)) {
        return;
    }

    if (strlen(value) != CPUID_VENDOR_SZ) {
        qemu_log_mask(LOG_GUEST_ERROR, "Property vendor must be %d bytes\n",
                      CPUID_VENDOR_SZ);
        return;
    }

    /* vendor string to uint32_t */
    s->cpu.vendor1 = 0;
    s->cpu.vendor2 = 0;
    s->cpu.vendor3 = 0;
    for (int i = 0; i < 4; i++) {
        s->cpu.vendor1 |= ((uint8_t)value[i]) << (8 * i);
        s->cpu.vendor2 |= ((uint8_t)value[i + 4]) << (8 * i);
        s->cpu.vendor3 |= ((uint8_t)value[i + 8]) << (8 * i);
    }
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
    SbrmiI3cTargetState *s = SBRMI_I3C_TARGET(obj);
    for (int i = 0; i < MAX_UMC_NUM; i++) {
        /*
         * Each UMC support 2 dimms and each dimm has 2 sensors.
         * Only use the first dimm first sensor for now.
         * Temperature (Deg C) is an 11-bit signed value, with
         * a scaling factor of 0.25. Thus 3FFh=255.75 (1023*0.25),
         * 400h= -256 (-1024*0.25), 1h=0.25 and 7FFh= -0.25 (-1*0.25)
         */
        object_property_add_uint16_ptr(obj, "temp[*]",
                            &s->umc[i].dimm[0].temp[0],
                            OBJ_PROP_FLAG_READWRITE);

        /* 15-bit unsigned value representing power consumed in mW (0-32767) */
        object_property_add_uint16_ptr(obj, "power[*]",
                            &s->umc[i].dimm[0].power,
                            OBJ_PROP_FLAG_READWRITE);
    }

    /* power cap */
    object_property_add_uint32_ptr(obj, "power_limit",
                        &s->power_limit,
                        OBJ_PROP_FLAG_READWRITE);

    /* max power cap */
    object_property_add_uint32_ptr(obj, "max_power_limit",
                        &s->max_power_limit,
                        OBJ_PROP_FLAG_READWRITE);

    /* power input */
    object_property_add_uint32_ptr(obj, "power",
                        &s->power,
                        OBJ_PROP_FLAG_READWRITE);
    /* cpu vendor */
    object_property_add(obj, "cpu_vendor", "string",
                        sbrmi_i3c_get_cpu_vendor,
                        sbrmi_i3c_set_cpu_vendor,
                        NULL, NULL);
    /* cpuid_version */
    object_property_add_uint32_ptr(obj, "cpu_version",
                                    &s->cpu.version,
                                    OBJ_PROP_FLAG_READWRITE);
    /* apic_id */
    object_property_add_uint32_ptr(obj, "apic_id",
                                    &s->cpu.apic_id,
                                    OBJ_PROP_FLAG_READWRITE);
    /* nr_cores */
    object_property_add_uint32_ptr(obj, "nr_cores",
                                    &s->cpu.nr_cores,
                                    OBJ_PROP_FLAG_READWRITE);
    /* nr_thread */
    object_property_add_uint32_ptr(obj, "nr_thread",
                                    &s->cpu.nr_thread,
                                    OBJ_PROP_FLAG_READWRITE);
    /* ecx_fn1 */
    object_property_add_uint32_ptr(obj, "ecx_fn1",
                                    &s->cpu.ecx_fn1,
                                    OBJ_PROP_FLAG_READWRITE);
    /* edx_fn1 */
    object_property_add_uint32_ptr(obj, "edx_fn1",
                                    &s->cpu.edx_fn1,
                                    OBJ_PROP_FLAG_READWRITE);
    /* ucode_rev */
    object_property_add_uint32_ptr(obj, "ucode_rev",
                                    &s->cpu.ucode_rev,
                                    OBJ_PROP_FLAG_READWRITE);
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

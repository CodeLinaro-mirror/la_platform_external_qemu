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

/* Registers supported in current design. */
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
    SBRMI_REG_THREADNUMBER,
    SBRMI_REG_RAS_STATUS = 0x4c,
} SbrmiReg;

/* Only list the commands we support now. */
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
#define SBRMI_BIT_HW_ALERT_STATUS (7)
#define SBRMI_BIT_HW_ALERT_STATUS_LEN (1)

/* BIT definition for SBRMI_REG_SW_INTERRUPT */
#define SBRMI_BIT_SWINT (0)
#define SBRMI_BIT_SWINT_LEN (1)

/* BIT definition for SBRMI_REG_INBNDMSG_INST7 */
#define SBRMI_BIT_INBNDMSG_INST7_MB_SEND (7)
#define SBRMI_BIT_INBNDMSG_INST7_MB_SEND_LEN (1)

#define MAX_UMC_NUM (12)
#define MAX_DIMM_PER_UMC (2)
#define MAX_THERMAL_SENSOR_PER_DIMM (2)

/* BIT definition for data_in of SBRMI_MAILBOX_CMD_GET_DIMM_THERMAL_SENSOR */
#define GET_DIMM_THERMAL_DI_DIMM_ADDR (0)
#define GET_DIMM_THERMAL_DI_DIMM_ADDR_LEN (8)

/* BIT definition for data_out of SBRMI_MAILBOX_CMD_GET_DIMM_THERMAL_SENSOR */
#define GET_DIMM_THERMAL_DO_DIMM_ADDR (0)
#define GET_DIMM_THERMAL_DO_DIMM_ADDR_LEN (8)
#define GET_DIMM_THERMAL_DO_UPDATE_RATE (8)
#define GET_DIMM_THERMAL_DO_UPDATE_RATE_LEN (9)
#define GET_DIMM_THERMAL_DO_TEMP (21)
#define GET_DIMM_THERMAL_DO_TEMP_LEN (11)

/* BIT definition for UMC dimm address */
#define UMC_DIMM_ADDR_ID (0)
#define UMC_DIMM_ADDR_ID_LEN (4)
#define UMC_DIMM_ADDR_DIMM (4)
#define UMC_DIMM_ADDR_DIMM_LEN (1)
#define UMC_DIMM_ADDR_TS (6)
#define UMC_DIMM_ADDR_TS_LEN (1)
#define UMC_DIMM_ADDR_MODE (7)
#define UMC_DIMM_ADDR_MODE_LEN (1)

/*
 * Special command for BMC to get CPU Registers.
 * See PPR Vol 5 for AMD Family 1Ah Model 02h C0, 8.4.2.1.2
 */
#define SBRMI_READ_CPU_REG_CMD (0x73)
#define SBRMI_READ_CPU_REG_WRITE_DATA_LEN (10)
#define SBRMI_READ_CPU_REG_READ_DATA_LEN (10)
#define SBRMI_READ_CPUID_COMMAND_CODE (0x91)
#define CPUID_VENDOR_SZ (12)

typedef enum {
    SBRMI_READ_CPUID_WD_WRITE_LENGTH = 0x0,
    SBRMI_READ_CPUID_WD_READ_LENGTH,
    SBRMI_READ_CPUID_WD_COMMAND,
    SBRMI_READ_CPUID_WD_WRITE_DATA_3,
    SBRMI_READ_CPUID_WD_WRITE_DATA_4,
    SBRMI_READ_CPUID_WD_FUNCTION_0,
    SBRMI_READ_CPUID_WD_FUNCTION_1,
    SBRMI_READ_CPUID_WD_FUNCTION_2,
    SBRMI_READ_CPUID_WD_FUNCTION_3,
    SBRMI_READ_CPUID_WD_WRITE_DATA_9
} SbrmiReadCpuidWriteDataOffset;

typedef enum {
    SBRMI_READ_CPUID_RD_READ_LENGTH = 0x0,
    SBRMI_READ_CPUID_RD_STATUS,
    SBRMI_READ_CPUID_RD_EAX_ECX_0,
    SBRMI_READ_CPUID_RD_EAX_ECX_1,
    SBRMI_READ_CPUID_RD_EAX_ECX_2,
    SBRMI_READ_CPUID_RD_EAX_ECX_3,
    SBRMI_READ_CPUID_RD_EBX_EDX_0,
    SBRMI_READ_CPUID_RD_EBX_EDX_1,
    SBRMI_READ_CPUID_RD_EBX_EDX_2,
    SBRMI_READ_CPUID_RD_EBX_EDX_3
} SbrmiReadCpuidReadDataOffset;

struct Dimm {
  /* 2 thermal sensors per dimm */
  uint16_t temp[MAX_THERMAL_SENSOR_PER_DIMM];
  /* update rate per sensor in ms */
  uint16_t update_rate[MAX_THERMAL_SENSOR_PER_DIMM];
};

struct Umc {
  /*
   * Unified Memory Controller
   * PPR Vol 5 for AMD Family 1Ah Model 02h C0, Chapter 12
   * each UMC controlls 2 dimms.
   */
  struct Dimm dimm[MAX_DIMM_PER_UMC];
};

/* See PPR Vol 1 for AMD Family 1Ah Model 02h C0, 2.1.18 */
struct Cpu {
  /* ebx cpuid_fn0 */
  uint32_t vendor1;
  /* edx cpuid_fn0 */
  uint32_t vendor2;
  /* ecx cpuid_fn0 */
  uint32_t vendor3;
  /* eax in cpuid_fn1 */
  uint32_t version;
  /* ebx[31:24] in cpuid_fn1 */
  uint32_t apic_id;
  /* ebx[23:16] in cpuid_fn1 */
  uint32_t nr_cores;
  uint32_t nr_thread;
  /* ecx in cpuid_fn1 */
  uint32_t ecx_fn1;
  /* edx in cpuid_fn1 */
  uint32_t edx_fn1;
};

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
    uint32_t mailbox_data_in;
    uint32_t mailbox_data_out;
    SbrmiMailboxError mailbox_error;

    /* Processor and CPUID state protocol */
    uint8_t cpu_reg_write_data[SBRMI_READ_CPU_REG_WRITE_DATA_LEN];
    uint8_t cpu_reg_write_data_ptr;
    uint8_t cpu_reg_read_data[SBRMI_READ_CPU_REG_READ_DATA_LEN];
    uint8_t cpu_reg_read_data_ptr;

    /* Unified Memory Controller state */
    struct Umc umc[MAX_UMC_NUM];
    /* power limit in mWatts */
    uint32_t power_limit;
    /* max power limit in mWatts */
    uint32_t max_power_limit;
    /* Read the average package power consumption in mWatts */
    uint32_t power;

    /*
     * target CPU registers
     * Ideally we should define a full struct of CPUArchState.
     * See qemu/target/i386/cpu.h
     * But we don't want to implement the full structure here.
     */
    struct Cpu cpu;

    struct {
        char *name;
        /* revision 0x21: 2-bytes command code. 0x20: 1-byte command code. */
        uint8_t sbrmi_rev;
    } cfg;
};

#endif  /* SBRMI_I3C_H_ */

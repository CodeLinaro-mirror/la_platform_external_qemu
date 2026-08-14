/*
 * Linear Technology LTC2991 Octal I2C Voltage, Current, and Temperature Monitor
 *
 * Copyright 2025 Google LLC
 *
 * Datasheet:
 * https://www.analog.com/media/en/technical-documentation/data-sheets/2991ff.pdf
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QEMU_LTC2991_H
#define QEMU_LTC2991_H

#include "hw/i2c/i2c.h"
#include "qom/object.h"

#define TYPE_LTC2991 "ltc2991"
OBJECT_DECLARE_SIMPLE_TYPE(LTC2991State, LTC2991)

#define LTC2991_REG_STATUS_LOW              0x00
#define LTC2991_REG_CH_EN_STAT_HI_TRIGGER   0x01
#define LTC2991_REG_V1_V4_CTRL              0x06
#define LTC2991_REG_V5_V8_CTRL              0x07
#define LTC2991_REG_PWM_VCC_TINT_CTRL       0x08
#define LTC2991_REG_PWM_THRESH_MSB          0x09
#define LTC2991_REG_V1_MSB                  0x0A
#define LTC2991_REG_V1_LSB                  0x0B
#define LTC2991_REG_V2_MSB                  0x0C
#define LTC2991_REG_V2_LSB                  0x0D
#define LTC2991_REG_V3_MSB                  0x0E
#define LTC2991_REG_V3_LSB                  0x0F
#define LTC2991_REG_V4_MSB                  0x10
#define LTC2991_REG_V4_LSB                  0x11
#define LTC2991_REG_V5_MSB                  0x12
#define LTC2991_REG_V5_LSB                  0x13
#define LTC2991_REG_V6_MSB                  0x14
#define LTC2991_REG_V6_LSB                  0x15
#define LTC2991_REG_V7_MSB                  0x16
#define LTC2991_REG_V7_LSB                  0x17
#define LTC2991_REG_V8_MSB                  0x18
#define LTC2991_REG_V8_LSB                  0x19
#define LTC2991_REG_T_INTERNAL_MSB          0x1A
#define LTC2991_REG_T_INTERNAL_LSB          0x1B
#define LTC2991_REG_VCC_MSB                 0x1C
#define LTC2991_REG_VCC_LSB                 0x1D

#define LTC2991_NUM_REGS  0x1E

struct LTC2991State {
    I2CSlave i2c;

    bool i2c_cmd;
    uint8_t reg;

    uint8_t regs[LTC2991_NUM_REGS];
};

#endif /* QEMU_LTC2991_H */

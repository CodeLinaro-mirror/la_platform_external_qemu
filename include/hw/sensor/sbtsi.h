#ifndef SBTSI_H_
#define SBTSI_H_

#include "qemu/osdep.h"

#define TYPE_SBTSI "sbtsi"
#define SBTSI(obj) OBJECT_CHECK(SBTSIState, (obj), TYPE_SBTSI)

/**
 * SBTSIState:
 * temperatures are in units of 0.125 degrees
 * @temperature: Temperature
 * @limit_low: Lowest temperature
 * @limit_high: Highest temperature
 * @status: The status register
 * @config: The config register
 * @alert_config: The config for alarm_l output.
 * @addr: The address to read/write for the next cmd.
 * @alarm: The alarm_l output pin (GPIO)
 */
typedef struct SBTSIState {
    DeviceState         parent;

    uint32_t temperature;
    uint32_t limit_low;
    uint32_t limit_high;
    uint8_t status;
    uint8_t config;
    uint8_t alert_config;
    qemu_irq alarm;
} SBTSIState;

/*
 * SB-TSI registers only support SMBus byte data access. "_INT" registers are
 * the integer part of a temperature value or limit, and "_DEC" registers are
 * corresponding decimal parts.
 */
#define SBTSI_REG_TEMP_INT      0x01 /* RO */
#define SBTSI_REG_STATUS        0x02 /* RO */
#define SBTSI_REG_CONFIG        0x03 /* RO */
#define SBTSI_REG_TEMP_HIGH_INT 0x07 /* RW */
#define SBTSI_REG_TEMP_LOW_INT  0x08 /* RW */
#define SBTSI_REG_CONFIG_WR     0x09 /* RW */
#define SBTSI_REG_TEMP_DEC      0x10 /* RO */
#define SBTSI_REG_TEMP_HIGH_DEC 0x13 /* RW */
#define SBTSI_REG_TEMP_LOW_DEC  0x14 /* RW */
#define SBTSI_REG_ALERT_CONFIG  0xBF /* RW */
#define SBTSI_REG_MAN           0xFE /* RO */
#define SBTSI_REG_REV           0xFF /* RO */

#define SBTSI_STATUS_HIGH_ALERT BIT(4)
#define SBTSI_STATUS_LOW_ALERT  BIT(3)
#define SBTSI_CONFIG_ALERT_MASK BIT(7)
#define SBTSI_ALARM_EN          BIT(0)

#define SBTSI_LIMIT_LOW_DEFAULT (0)
#define SBTSI_LIMIT_HIGH_DEFAULT (560)
#define SBTSI_MAN_DEFAULT (0)
#define SBTSI_REV_DEFAULT (4)
#define SBTSI_ALARM_L "alarm_l"

/* The temperature we stored are in units of 0.125 degrees. */
#define SBTSI_TEMP_UNIT_IN_MILLIDEGREE 125

/*
 * The integer part and decimal of the temperature both 8 bits.
 * Only the top 3 bits of the decimal parts are used.
 * So the max temperature is (2^8-1) + (2^3-1)/8 = 255.875 degrees.
 */
#define SBTSI_TEMP_MAX 255875

uint8_t sbtsi_read(SBTSIState *s, uint8_t addr);
void sbtsi_write(SBTSIState *s, uint8_t addr, uint8_t data);
void sbtsi_reset(SBTSIState *s);
void sbtsi_hold_reset(SBTSIState *s, ResetType type);

#endif  /* SBTSI_H_ */

/*
 * MAX6581 temperature sensor
 *
 * Features:
 *  - 7 external diode temperature inputs
 *
 * Datasheet:
 * https://www.analog.com/media/en/technical-documentation/data-sheets/MAX6581.pdf
 * Copyright 2024 Google LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/i2c/smbus_slave.h"
#include "hw/qdev-properties.h"
#include "hw/sensor/max6581.h"
#include "qapi/visitor.h"
#include "qom/object.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "trace.h"

#define MAX6581_TEMP_MAX                                  254
#define MAX6581_TEMP_MIN                                  0

#define MAX6581_MANUFACTURER_ID_DEFAULT                   0x4D
#define MAX6581_REMOTE_ALERT_HIGH_THRESHOLD_1_DEFAULT     0x7F
#define MAX6581_REMOTE_ALERT_HIGH_THRESHOLD_2_DEFAULT     0x64
#define MAX6581_LOCAL_ALERT_HIGH_THRESHOLD_DEFAULT        0x5A
#define MAX6581_LOCAL_OVERT_HIGH_THRESHOLD_DEFAULT        0x50
#define MAX6581_REMOTE_OVERT_HIGH_THRESHOLD_1_DEFAULT     0x6E
#define MAX6581_REMOTE_OVERT_HIGH_THRESHOLD_2_DEFAULT     0x7F
#define MAX6581_REMOTE_OVERT_HIGH_THRESHOLD_3_DEFAULT     0x5A
#define MAX6581_ALERT_LOW_DISABLE_DEFAULT                 0xFF

#define MAX6581_ALERT_STATUS_LOCAL                        0b01000000
#define MAX6581_ALERT_STATUS_REMOTE_7                     0b10000000
#define MAX6581_OVERT_STATUS_REMOTE_7                     0b01000000
#define MAX6581_OVERT_STATUS_LOCAL                        0b10000000
#define MAX6581_DIODE_STATUS_REMOTE_7                     0b01000000

#define MAX6581_EXTENDED_OFFSET                           50
#define MAX6581_OVERT_LIMIT_OFFSET                        4
#define MAX6581_DIODE_FAULT_LIMIT                         255
#define MAX6581_DEFAULT_TEMPERATURE                       32

#define MAX6581_EXTENDED_BIT_BASE                         32
#define MAX6581_EXTENDED_DECIMAL_BASE                     125

/**
 * max6581_get_temperature:
 * @temp_reg: device temperature register to get temperature from
 */
uint8_t max6581_get_temperature(uint8_t *temp_reg)
{
    return *temp_reg;
}

/**
 * max6581_set_temperature:
 * @temp_reg: the device registers
 * @value: the temperature to be written in degrees
 *
 * Take a temperature in degrees between 0C and 254C and store it in
 * @temp_reg.
 */
void max6581_set_temperature(uint8_t *temp_reg, uint8_t value)
{
    if (value > MAX6581_TEMP_MAX) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: overflow", __func__);
        value = MAX6581_TEMP_MAX;
    }

    *temp_reg = value;
}

/**
 * Update status compares the temperature values for enabled channels against
 * their respective configured thresholds and updates the relevant status
 * registers.
 * ALERT statuses are not cleared here.
 * There are 7 remote diodes and 1 local diode with respect to the MAX6581
 */
static void max6581_update_status(MAX6581State *ms)
{
    /* bit mask for the status register */
    uint8_t status_bit_mask;
    /* temperatures in degrees */
    uint8_t temperature, alert_limit, overt_limit;
    /* low threshold shared across all channels */
    uint8_t low_limits = ms->regs[A_ALERT_LOW_LIMITS];
    /* alert and overt masks for each channel  */
    uint8_t alert_mask = ms->regs[A_ALERT_MASK];
    uint8_t overt_mask = ms->regs[A_OVERT_MASK];

    ms->regs[A_OVERT_STATUS] = 0;
    ms->regs[A_DIODE_FAULT_STATUS] = 0;

    /* update the status of remote diodes 1 through 6 */
    for (int i = 0; i < MAX6581_NUM_TEMPS - 2; i++) {
        temperature = max6581_get_temperature(
            &ms->regs[A_REMOTE_1_TEMPERATURE + i]);
        alert_limit = max6581_get_temperature(
            &ms->regs[A_REMOTE_1_ALERT_HIGH_LIMIT + i]);
        overt_limit = max6581_get_temperature(
            &ms->regs[A_REMOTE_1_OVERT_HIGH_LIMIT + i]);
        status_bit_mask = BIT(i);

        if (temperature > alert_limit && !(alert_mask & status_bit_mask)) {
            ms->regs[A_ALERT_HIGH_STATUS] |= status_bit_mask;
        }
        if (temperature > (overt_limit - MAX6581_OVERT_LIMIT_OFFSET) &&
            !(overt_mask & status_bit_mask)) {
            ms->regs[A_OVERT_STATUS] |= status_bit_mask;
        }
        if (temperature == MAX6581_DIODE_FAULT_LIMIT) {
            ms->regs[A_DIODE_FAULT_STATUS] |= status_bit_mask;
        }
        if (temperature < low_limits && !(alert_mask & status_bit_mask)) {
            ms->regs[A_ALERT_LOW_STATUS] |= status_bit_mask;
        }
    }

    /* update the status of the local temperature measurement*/
    temperature = max6581_get_temperature(
        &ms->regs[A_LOCAL_TEMPERATURE]);
    alert_limit = max6581_get_temperature(
        &ms->regs[A_LOCAL_ALERT_HIGH_LIMIT]);
    overt_limit = max6581_get_temperature(
        &ms->regs[A_LOCAL_OVERT_HIGH_LIMIT]);

    if (temperature > alert_limit &&
        !(alert_mask & MAX6581_ALERT_STATUS_LOCAL)) {
        ms->regs[A_ALERT_HIGH_STATUS] |= MAX6581_ALERT_STATUS_LOCAL;
    }
    if (temperature > (overt_limit - MAX6581_OVERT_LIMIT_OFFSET) &&
        !(overt_mask & MAX6581_OVERT_STATUS_LOCAL)) {
        ms->regs[A_OVERT_STATUS] |= MAX6581_OVERT_STATUS_LOCAL;
    }
    if (temperature < low_limits &&
        !(alert_mask & MAX6581_ALERT_STATUS_LOCAL)) {
        ms->regs[A_ALERT_LOW_STATUS] |= MAX6581_ALERT_STATUS_LOCAL;
    }

    /* update the status of remote diode 7 */
    temperature = max6581_get_temperature(
        &ms->regs[A_REMOTE_7_TEMPERATURE]);
    alert_limit = max6581_get_temperature(
        &ms->regs[A_REMOTE_7_ALERT_HIGH_LIMIT]);
    overt_limit = max6581_get_temperature(
        &ms->regs[A_REMOTE_7_OVERT_HIGH_LIMIT]);

    if (temperature > alert_limit &&
        !(alert_mask & MAX6581_ALERT_STATUS_REMOTE_7)) {
        ms->regs[A_ALERT_HIGH_STATUS] |= MAX6581_ALERT_STATUS_REMOTE_7;
    }
    if (temperature > (overt_limit - MAX6581_OVERT_LIMIT_OFFSET) &&
        !(overt_mask & MAX6581_OVERT_STATUS_REMOTE_7)) {
        ms->regs[A_OVERT_STATUS] |= MAX6581_OVERT_STATUS_REMOTE_7;
    }
    if (temperature == MAX6581_DIODE_FAULT_LIMIT) {
        ms->regs[A_DIODE_FAULT_STATUS] |= MAX6581_DIODE_STATUS_REMOTE_7;
    }
    if (temperature < low_limits &&
        !(alert_mask & MAX6581_ALERT_STATUS_REMOTE_7)) {
        ms->regs[A_ALERT_LOW_STATUS] |= MAX6581_ALERT_STATUS_REMOTE_7;
    }
}

/*
 * Temperature in millidegrees ranging from 0 to 254875 with 125 millidegree
 * granularity.
 */
static void max6581_qmp_get_temp(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    uint8_t *reg = (uint8_t *)opaque;
    uint32_t value, fraction;

    value = max6581_get_temperature(reg);

    value *= 1000;
    fraction = max6581_get_temperature(reg + MAX6581_EXTENDED_OFFSET);
    value += (fraction * MAX6581_EXTENDED_DECIMAL_BASE) /
            MAX6581_EXTENDED_BIT_BASE;
    visit_type_uint32(v, name, &value, errp);
}

static void max6581_qmp_set_temp(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    uint8_t *reg = (uint8_t *)opaque;
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }

    max6581_set_temperature(reg, value / 1000);
    max6581_set_temperature(reg + MAX6581_EXTENDED_OFFSET,
            ((value % 1000) * MAX6581_EXTENDED_BIT_BASE) /
            MAX6581_EXTENDED_DECIMAL_BASE);
    max6581_update_status(MAX6581(obj));
}

static uint8_t max6581_receive(SMBusDevice *smd)
{
    MAX6581State *ms = MAX6581(smd);
    uint8_t data;

    max6581_update_status(ms);

    switch (ms->command) {
    case A_ALERT_HIGH_STATUS:
    case A_ALERT_LOW_STATUS:
        data = ms->regs[ms->command];
        ms->regs[ms->command] = 0;
        break;

    case A_REMOTE_1_TEMPERATURE ... A_OVERT_MASK:
    case A_OVERT_STATUS ... A_DIODE_FAULT_STATUS:
    case A_ALERT_LOW_DISABLE ... A_REMOTE_7_EXTENDED_TEMPERATURE:
        data = ms->regs[ms->command];
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: reading from unsupported register 0x%02x\n",
                      __func__, DEVICE(ms)->canonical_path, ms->command);
        data = 0xFF;
    }

    trace_max6581_receive(DEVICE(ms)->canonical_path, ms->command, data);

    return data;
}

static int max6581_write(SMBusDevice *smd, uint8_t *buf, uint8_t len)
{
    MAX6581State *ms = MAX6581(smd);

    ms->command = buf[0];

    if (len == 1) { /* only the register offset was sent */
        return 0;
    }

    uint8_t data = buf[1];

    if (len > 2) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: received large write %d bytes",
            DEVICE(ms)->canonical_path, __func__, len);
    }

    trace_max6581_write(DEVICE(ms)->canonical_path, ms->command, data);

    switch (ms->command) {
    case A_REMOTE_1_ALERT_HIGH_LIMIT ... A_OVERT_MASK:
    case A_ALERT_LOW_DISABLE ... A_OFFSET_SELECT:
        ms->regs[ms->command] = data;
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: writing to unsupported register 0x%02x\n",
                      __func__, DEVICE(ms)->canonical_path, ms->command);
        data = 0xFF;
    }

    max6581_update_status(ms);

    return 0;
}

static void max6581_exit_reset(Object *obj, ResetType type)
{
    MAX6581State *ms = MAX6581(obj);

    memset(ms->regs, 0, sizeof ms->regs);

    ms->regs[A_REMOTE_1_TEMPERATURE] = MAX6581_DEFAULT_TEMPERATURE;
    ms->regs[A_REMOTE_2_TEMPERATURE] = MAX6581_DEFAULT_TEMPERATURE;
    ms->regs[A_REMOTE_3_TEMPERATURE] = MAX6581_DEFAULT_TEMPERATURE;
    ms->regs[A_REMOTE_4_TEMPERATURE] = MAX6581_DEFAULT_TEMPERATURE;
    ms->regs[A_REMOTE_5_TEMPERATURE] = MAX6581_DEFAULT_TEMPERATURE;
    ms->regs[A_REMOTE_6_TEMPERATURE] = MAX6581_DEFAULT_TEMPERATURE;
    ms->regs[A_LOCAL_TEMPERATURE] = MAX6581_DEFAULT_TEMPERATURE;
    ms->regs[A_REMOTE_7_TEMPERATURE] = MAX6581_DEFAULT_TEMPERATURE;
    ms->regs[A_MANUFACTURER_ID] = MAX6581_MANUFACTURER_ID_DEFAULT;
    ms->regs[A_REMOTE_1_ALERT_HIGH_LIMIT] =
        MAX6581_REMOTE_ALERT_HIGH_THRESHOLD_1_DEFAULT;
    ms->regs[A_REMOTE_2_ALERT_HIGH_LIMIT] =
        MAX6581_REMOTE_ALERT_HIGH_THRESHOLD_1_DEFAULT;
    ms->regs[A_REMOTE_3_ALERT_HIGH_LIMIT] =
        MAX6581_REMOTE_ALERT_HIGH_THRESHOLD_2_DEFAULT;
    ms->regs[A_REMOTE_4_ALERT_HIGH_LIMIT] =
        MAX6581_REMOTE_ALERT_HIGH_THRESHOLD_2_DEFAULT;
    ms->regs[A_REMOTE_5_ALERT_HIGH_LIMIT] =
        MAX6581_REMOTE_ALERT_HIGH_THRESHOLD_2_DEFAULT;
    ms->regs[A_REMOTE_6_ALERT_HIGH_LIMIT] =
        MAX6581_REMOTE_ALERT_HIGH_THRESHOLD_2_DEFAULT;
    ms->regs[A_LOCAL_ALERT_HIGH_LIMIT] =
        MAX6581_LOCAL_ALERT_HIGH_THRESHOLD_DEFAULT;
    ms->regs[A_REMOTE_7_ALERT_HIGH_LIMIT] =
        MAX6581_REMOTE_ALERT_HIGH_THRESHOLD_2_DEFAULT;
    ms->regs[A_LOCAL_OVERT_HIGH_LIMIT] =
        MAX6581_LOCAL_OVERT_HIGH_THRESHOLD_DEFAULT;
    ms->regs[A_REMOTE_1_OVERT_HIGH_LIMIT] =
        MAX6581_REMOTE_OVERT_HIGH_THRESHOLD_1_DEFAULT;
    ms->regs[A_REMOTE_2_OVERT_HIGH_LIMIT] =
        MAX6581_REMOTE_OVERT_HIGH_THRESHOLD_1_DEFAULT;
    ms->regs[A_REMOTE_3_OVERT_HIGH_LIMIT] =
        MAX6581_REMOTE_OVERT_HIGH_THRESHOLD_1_DEFAULT;
    ms->regs[A_REMOTE_4_OVERT_HIGH_LIMIT] =
        MAX6581_REMOTE_OVERT_HIGH_THRESHOLD_2_DEFAULT;
    ms->regs[A_REMOTE_5_OVERT_HIGH_LIMIT] =
        MAX6581_REMOTE_OVERT_HIGH_THRESHOLD_3_DEFAULT;
    ms->regs[A_REMOTE_6_OVERT_HIGH_LIMIT] =
        MAX6581_REMOTE_OVERT_HIGH_THRESHOLD_3_DEFAULT;
    ms->regs[A_REMOTE_7_OVERT_HIGH_LIMIT] =
        MAX6581_REMOTE_OVERT_HIGH_THRESHOLD_3_DEFAULT;
    ms->regs[A_ALERT_LOW_DISABLE] = MAX6581_ALERT_LOW_DISABLE_DEFAULT;
}

static void max6581_init(Object *obj)
{
    MAX6581State *ms = MAX6581(obj);

    for (int i = 0; i < MAX6581_NUM_TEMPS; i++) {
        object_property_add(obj, "temperature[*]", "uint32",
                            max6581_qmp_get_temp, max6581_qmp_set_temp, NULL,
                            &ms->regs[A_REMOTE_1_TEMPERATURE + i]);
    }
}

static void max6581_class_init(ObjectClass *klass, void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);
    SMBusDeviceClass *k = SMBUS_DEVICE_CLASS(klass);

    dc->desc = "Maxim MAX6581 temperature sensor";

    k->write_data = max6581_write;
    k->receive_byte = max6581_receive;

    rc->phases.exit = max6581_exit_reset;
}

static const TypeInfo max6581_types[] = {
    {
        .name = TYPE_MAX6581,
        .parent = TYPE_SMBUS_DEVICE,
        .instance_size = sizeof(MAX6581State),
        .instance_init = max6581_init,
        .class_init = max6581_class_init,
    },
};

DEFINE_TYPES(max6581_types)

/*
 * MAX31732 temperature sensor
 *
 * Features:
 *  - 4 external diode temperature inputs
 *
 * Copyright 2023 Google LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/i2c/smbus_slave.h"
#include "hw/qdev-properties.h"
#include "hw/sensor/max31732.h"
#include "qapi/visitor.h"
#include "qom/object.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "trace.h"


#define MAX31732_TEMP_MAX                   127875
#define MAX31732_TEMP_MIN                   -63875

#define MAX31732_ENABLE_ALL                 0x7F
#define MAX31732_HIGH_THRESHOLD_DEFAULT     0x7F
#define MAX31732_TEMPERATURE_DEFAULT        32
#define MAX31732_MANUFACTURER_ID_DEFAULT    0x4F
#define MAX31732_CONFIGURATION_1_DEFAULT    0x10
#define MAX31732_CONFIGURATION_2_DEFAULT    0x11
#define MAX31732_CUSTOM_OFFSET_DEFAULT      0x77
/**
 * max31732_get_temperature:
 * @temp_reg: a pointer to the MSB of the temperature value to get
 *
 */
int32_t max31732_get_temperature(uint8_t *temp_reg)
{
    int32_t ret, fraction;
    MAX31732Temperature *t = (MAX31732Temperature *)temp_reg;
    ret = t->temp * 1000; /* convert to milidegrees */
    fraction = t->fraction * 625 / 10;
    if (!t->sign) {
        ret += fraction;
    } else {
        ret -= fraction;
    }
    return ret;
}

/**
 * max31732_set_temperature:
 * @temp_reg: the device registers
 * @temperature: the temperature to be written in millidegrees
 *
 * Take a temperature in millidegrees between -64C and 127C and store it in
 * @temp_reg.
 */
void max31732_set_temperature(uint8_t *temp_reg, int32_t value)
{
    uint32_t fraction = abs(value % 1000);
    MAX31732Temperature *t = (MAX31732Temperature *)temp_reg;

    if (value > MAX31732_TEMP_MAX) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: overflow", __func__);
        value = MAX31732_TEMP_MAX;
    } else if (value < MAX31732_TEMP_MIN) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: underflow", __func__);
        value = MAX31732_TEMP_MIN;
    }
    t->temp = value / 1000;
    t->fraction = fraction * 10 / 625;
    t->sign = value < 0;
}

/**
 * Update status compares the temperature values for enabled channels against
 * their respective configured thresholds and updates the relevant status
 * registers.
 * The highest temperature across all diodes is also updated here.
 * There are 4 remote diodes and 1 local diode with respect to the MAX31732
 */
static void max31732_update_status(MAX31732State *ms)
{
    /* temperatures in millidegrees */
    int32_t temperature, high_threshold, secondary_threshold;
    /* The low threshold is shared across all channels */
    int32_t low_threshold = max31732_get_temperature(
        &ms->regs[A_PRIMARY_THRESHOLD_LOW_LIMIT]);

    /* clear status registers */
    ms->regs[A_THERMAL_STATUS_HIGH_TEMPERATURE] = 0;
    ms->regs[A_SECONDARY_THERMAL_STATUS_HIGH_TEMPERATURE] = 0;
    ms->regs[A_THERMAL_STATUS_LOW_TEMPERATURE] = 0;

    /* update the status of the local temperature measurement */
    if (ms->regs[A_TEMPERATURE_CHANNEL_ENABLE] & BIT(0)) {
        temperature = max31732_get_temperature(
            &ms->regs[A_LOCAL_TEMPERATURE]);
        high_threshold = max31732_get_temperature(
            &ms->regs[A_LOCAL_PRIMARY_OVER_TEMPERATURE_THRESHOLD]);
        secondary_threshold =
            (int8_t) ms->regs[A_LOCAL_SECONDARY_THRESHOLD_HIGH_LIMIT] * 1000;

        if (temperature > high_threshold) {
            ms->regs[A_THERMAL_STATUS_HIGH_TEMPERATURE] |= BIT(0);
        }
        if (temperature > secondary_threshold) {
            ms->regs[A_SECONDARY_THERMAL_STATUS_HIGH_TEMPERATURE] |= BIT(0);
        }
        if (temperature < low_threshold) {
            ms->regs[A_THERMAL_STATUS_LOW_TEMPERATURE] |= BIT(0);
        }

        if (FIELD_EX8(ms->regs[A_HIGHEST_TEMPERATURE_ENABLE],
            HIGHEST_TEMPERATURE_ENABLE, LOCAL)) {
            ms->regs[A_HIGHEST_TEMPERATURE] = temperature;
        }

    }

    /* update the status of the remote diodes */
    for (int i = 0; i < MAX31732_NUM_TEMPS - 1; i++) {
        /* don't update status for disabled temperature channels */
        if (!(ms->regs[A_TEMPERATURE_CHANNEL_ENABLE] & BIT(i + 1))) {
            continue;
        }

        temperature = max31732_get_temperature(
            &ms->regs[A_REMOTE_1_TEMPERATURE + (2 * i)]);
        high_threshold = max31732_get_temperature(
            &ms->regs[A_REMOTE_1_PRIMARY_OVER_TEMPERATURE_THRESHOLD + (2 * i)]);
        secondary_threshold = 1000 *
            (int8_t) ms->regs[A_REMOTE_1_SECONDARY_THRESHOLD_HIGH_LIMIT + i];

        if (temperature > high_threshold) {
            ms->regs[A_THERMAL_STATUS_HIGH_TEMPERATURE] |= BIT(i + 1);
        }
        if (temperature > secondary_threshold) {
            ms->regs[A_SECONDARY_THERMAL_STATUS_HIGH_TEMPERATURE] |= BIT(i + 1);
        }
        if (temperature < low_threshold) {
            ms->regs[A_THERMAL_STATUS_LOW_TEMPERATURE] |= BIT(i + 1);
        }

        if (ms->regs[A_HIGHEST_TEMPERATURE_ENABLE] & BIT(i + 1) &&
            temperature > ms->regs[A_HIGHEST_TEMPERATURE]) {
            ms->regs[A_HIGHEST_TEMPERATURE] = temperature;
        }
    }
}

static uint8_t max31732_receive(SMBusDevice *smd)
{
    MAX31732State *ms = MAX31732(smd);
    uint8_t data;

    switch (ms->command) {
    case A_MANUFACTURER_ID ... A_BETA_VALUE_REMOTE_4:
        data = ms->regs[ms->command];
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: reading from unsupported register 0x%02x\n",
                      __func__, DEVICE(ms)->canonical_path, ms->command);
        data = 0xFF;
    }

    trace_max31732_receive(DEVICE(ms)->canonical_path, ms->command, data);
    return data;
}

static int max31732_write(SMBusDevice *smd, uint8_t *buf, uint8_t len)
{
    MAX31732State *ms = MAX31732(smd);

    ms->command = buf[0];

    if (len == 1) { /* only the register offset was sent */
        return 0;
    }

    uint8_t data = buf[1];

    if (len > 2) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: received large write %d bytes",
            DEVICE(ms)->canonical_path, __func__, len);
    }

    trace_max31732_write(DEVICE(ms)->canonical_path, ms->command, data);

    switch (ms->command) {
    case A_TEMPERATURE_CHANNEL_ENABLE ... (A_MTP_DIN + 1):
        ms->regs[ms->command] = data;
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: writing to unsupported register 0x%02x\n",
                      __func__, DEVICE(ms)->canonical_path, ms->command);
        data = 0xFF;
    }

    return 0;
}

/* Temperature in millidegrees range -127000 to 127000 */
static void max31732_qmp_get_temp(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    int32_t value = max31732_get_temperature((uint8_t *)opaque);
    visit_type_int32(v, name, &value, errp);
}

static void max31732_qmp_set_temp(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    int32_t value;

    if (!visit_type_int32(v, name, &value, errp)) {
        return;
    }
    max31732_set_temperature((uint8_t *)opaque, value);
    max31732_update_status(MAX31732(obj));
}

static void max31732_exit_reset(Object *obj, ResetType type)
{
    MAX31732State *ms = MAX31732(obj);

    memset(ms->regs, 0, sizeof ms->regs);

    ms->regs[A_MANUFACTURER_ID] = MAX31732_MANUFACTURER_ID_DEFAULT;
    ms->regs[A_REMOTE_1_TEMPERATURE] = MAX31732_TEMPERATURE_DEFAULT;
    ms->regs[A_REMOTE_2_TEMPERATURE] = MAX31732_TEMPERATURE_DEFAULT;
    ms->regs[A_REMOTE_3_TEMPERATURE] = MAX31732_TEMPERATURE_DEFAULT;
    ms->regs[A_REMOTE_4_TEMPERATURE] = MAX31732_TEMPERATURE_DEFAULT;
    ms->regs[A_LOCAL_TEMPERATURE] = MAX31732_TEMPERATURE_DEFAULT;
    ms->regs[A_TEMPERATURE_CHANNEL_ENABLE] = MAX31732_ENABLE_ALL | BIT(7);
    ms->regs[A_CONFIGURATION_1] = MAX31732_CONFIGURATION_1_DEFAULT;
    ms->regs[A_CONFIGURATION_2] = MAX31732_CONFIGURATION_2_DEFAULT;
    ms->regs[A_CUSTOM_OFFSET] = MAX31732_CUSTOM_OFFSET_DEFAULT;
    ms->regs[A_HIGHEST_TEMPERATURE_ENABLE] = MAX31732_ENABLE_ALL;
    ms->regs[A_REMOTE_1_PRIMARY_OVER_TEMPERATURE_THRESHOLD] =
        MAX31732_HIGH_THRESHOLD_DEFAULT;
    ms->regs[A_REMOTE_2_PRIMARY_OVER_TEMPERATURE_THRESHOLD] =
        MAX31732_HIGH_THRESHOLD_DEFAULT;
    ms->regs[A_REMOTE_3_PRIMARY_OVER_TEMPERATURE_THRESHOLD] =
        MAX31732_HIGH_THRESHOLD_DEFAULT;
    ms->regs[A_REMOTE_4_PRIMARY_OVER_TEMPERATURE_THRESHOLD] =
        MAX31732_HIGH_THRESHOLD_DEFAULT;
    ms->regs[A_LOCAL_PRIMARY_OVER_TEMPERATURE_THRESHOLD] =
        MAX31732_HIGH_THRESHOLD_DEFAULT;
    ms->regs[A_REMOTE_1_SECONDARY_THRESHOLD_HIGH_LIMIT] =
        MAX31732_HIGH_THRESHOLD_DEFAULT;
    ms->regs[A_REMOTE_2_SECONDARY_THRESHOLD_HIGH_LIMIT] =
        MAX31732_HIGH_THRESHOLD_DEFAULT;
    ms->regs[A_REMOTE_3_SECONDARY_THRESHOLD_HIGH_LIMIT] =
        MAX31732_HIGH_THRESHOLD_DEFAULT;
    ms->regs[A_REMOTE_4_SECONDARY_THRESHOLD_HIGH_LIMIT] =
        MAX31732_HIGH_THRESHOLD_DEFAULT;
    ms->regs[A_LOCAL_SECONDARY_THRESHOLD_HIGH_LIMIT] =
        MAX31732_HIGH_THRESHOLD_DEFAULT;
    ms->regs[A_SECONDARY_THRESHOLD_LOW_LIMIT] = 0;
}

static void max31732_init(Object *obj)
{
    MAX31732State *ms = MAX31732(obj);

    for (int i = 0; i < MAX31732_NUM_TEMPS; i++) {
        object_property_add(obj, "temperature[*]", "int32",
                            max31732_qmp_get_temp, max31732_qmp_set_temp, NULL,
                            &ms->regs[A_REMOTE_1_TEMPERATURE + (2 * i)]);
    }
}

static void max31732_class_init(ObjectClass *klass, void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);
    SMBusDeviceClass *k = SMBUS_DEVICE_CLASS(klass);

    dc->desc = "Maxim MAX31732 temperature sensor";

    k->write_data = max31732_write;
    k->receive_byte = max31732_receive;

    rc->phases.exit = max31732_exit_reset;
}

static const TypeInfo max31732_types[] = {
    {
        .name = TYPE_MAX31732,
        .parent = TYPE_SMBUS_DEVICE,
        .instance_size = sizeof(MAX31732State),
        .instance_init = max31732_init,
        .class_init = max31732_class_init,
    },
};

DEFINE_TYPES(max31732_types)

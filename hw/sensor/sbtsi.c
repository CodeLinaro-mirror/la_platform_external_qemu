// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * AMD SBI Temperature Sensor Interface (SB-TSI)
 * Forked from tmp_sbtsi to isolate the transport layer.
 *
 * Copyright 2024 Google LLC
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
 */
#include "qemu/osdep.h"
#include "hw/irq.h"
#include "hw/qdev-core.h"
#include "hw/i2c/smbus_slave.h"
#include "hw/sensor/sbtsi.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/log.h"
#include "trace.h"

/* The integer part of the temperature in terms of 0.125 degrees. */
static uint8_t get_temp_int(uint32_t temp)
{
    return temp >> 3;
}

/*
 * The decimal part of the temperature, in terms of 0.125 degrees.
 * H/W store it in the top 3 bits so we shift it by 5.
 */
static uint8_t get_temp_dec(uint32_t temp)
{
    return (temp & 0x7) << 5;
}

/*
 * Compute the temperature using the integer and decimal part,
 * in terms of 0.125 degrees. The decimal part are only the top 3 bits
 * so we shift it by 5 here.
 */
static uint32_t compute_temp(uint8_t integer, uint8_t decimal)
{
    return (integer << 3) + (decimal >> 5);
}

/* Compute new temp with new int part of the temperature. */
static uint32_t compute_temp_int(uint32_t temp, uint8_t integer)
{
    return compute_temp(integer, get_temp_dec(temp));
}

/* Compute new temp with new dec part of the temperature. */
static uint32_t compute_temp_dec(uint32_t temp, uint8_t decimal)
{
    return compute_temp(get_temp_int(temp), decimal);
}

/* The integer part of the temperature. */
static void sbtsi_update_status(SBTSIState *s)
{
    s->status = 0;
    if (s->alert_config & SBTSI_ALARM_EN) {
        if (s->temperature >= s->limit_high) {
            s->status |= SBTSI_STATUS_HIGH_ALERT;
        }
        if (s->temperature <= s->limit_low) {
            s->status |= SBTSI_STATUS_LOW_ALERT;
        }
    }
}

static void sbtsi_update_alarm(SBTSIState *s)
{
    sbtsi_update_status(s);
    if (s->status != 0 && !(s->config & SBTSI_CONFIG_ALERT_MASK)) {
        qemu_irq_raise(s->alarm);
    } else {
        qemu_irq_lower(s->alarm);
    }
}

uint8_t sbtsi_read(SBTSIState *s, uint8_t addr)
{
    uint8_t data = 0;

    switch (addr) {
    case SBTSI_REG_TEMP_INT:
        data = get_temp_int(s->temperature);
        break;

    case SBTSI_REG_TEMP_DEC:
        data = get_temp_dec(s->temperature);
        break;

    case SBTSI_REG_TEMP_HIGH_INT:
        data = get_temp_int(s->limit_high);
        break;

    case SBTSI_REG_TEMP_LOW_INT:
        data = get_temp_int(s->limit_low);
        break;

    case SBTSI_REG_TEMP_HIGH_DEC:
        data = get_temp_dec(s->limit_high);
        break;

    case SBTSI_REG_TEMP_LOW_DEC:
        data = get_temp_dec(s->limit_low);
        break;

    case SBTSI_REG_CONFIG:
    case SBTSI_REG_CONFIG_WR:
        data = s->config;
        break;

    case SBTSI_REG_STATUS:
        sbtsi_update_alarm(s);
        data = s->status;
        break;

    case SBTSI_REG_ALERT_CONFIG:
        data = s->alert_config;
        break;

    case SBTSI_REG_MAN:
        data = SBTSI_MAN_DEFAULT;
        break;

    case SBTSI_REG_REV:
        data = SBTSI_REV_DEFAULT;
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                "%s: reading from invalid reg: 0x%02x\n",
                __func__, addr);
        break;
    }

    trace_sbtsi_read(addr, data);
    return data;
}

void sbtsi_write(SBTSIState *s, uint8_t addr, uint8_t data)
{
    trace_sbtsi_write(addr, data);

    switch (addr) {
    case SBTSI_REG_CONFIG_WR:
        s->config = data;
        break;

    case SBTSI_REG_TEMP_HIGH_INT:
        s->limit_high = compute_temp_int(s->limit_high, data);
        break;

    case SBTSI_REG_TEMP_LOW_INT:
        s->limit_low = compute_temp_int(s->limit_low, data);
        break;

    case SBTSI_REG_TEMP_HIGH_DEC:
        s->limit_high = compute_temp_dec(s->limit_high, data);
        break;

    case SBTSI_REG_TEMP_LOW_DEC:
        s->limit_low = compute_temp_dec(s->limit_low, data);
        break;

    case SBTSI_REG_ALERT_CONFIG:
        s->alert_config = data;
        break;

    case SBTSI_REG_TEMP_INT:
    case SBTSI_REG_TEMP_DEC:
    case SBTSI_REG_CONFIG:
    case SBTSI_REG_STATUS:
    case SBTSI_REG_MAN:
    case SBTSI_REG_REV:
        qemu_log_mask(LOG_GUEST_ERROR,
                "%s: writing to read only reg: 0x%02x data: 0x%02x\n",
                __func__, addr, data);
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                "%s: writing to invalid reg: 0x%02x data: 0x%02x\n",
                __func__, addr, data);
        break;
    }
    sbtsi_update_alarm(s);
}

void sbtsi_reset(SBTSIState *s)
{
    s->config = 0;
    s->limit_low = SBTSI_LIMIT_LOW_DEFAULT;
    s->limit_high = SBTSI_LIMIT_HIGH_DEFAULT;
}

void sbtsi_hold_reset(SBTSIState *s, ResetType type)
{
    qemu_irq_lower(s->alarm);
}

/* Units are millidegrees. */
static void sbtsi_get_temperature(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    SBTSIState *s = SBTSI(obj);
    uint32_t temp = s->temperature * SBTSI_TEMP_UNIT_IN_MILLIDEGREE;

    visit_type_uint32(v, name, &temp, errp);
}

/* Units are millidegrees. */
static void sbtsi_set_temperature(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    SBTSIState *s = SBTSI(obj);
    uint32_t temp;

    if (!visit_type_uint32(v, name, &temp, errp)) {
        return;
    }
    if (temp > SBTSI_TEMP_MAX) {
        error_setg(errp, "value %" PRIu32 ".%03" PRIu32 " C is out of range",
                   temp / 1000, temp % 1000);
        return;
    }

    s->temperature = temp / SBTSI_TEMP_UNIT_IN_MILLIDEGREE;
    sbtsi_update_alarm(s);
}

static int sbtsi_post_load(void *opaque, int version_id)
{
    SBTSIState *s = opaque;

    sbtsi_update_alarm(s);
    return 0;
}

static const VMStateDescription vmstate_sbtsi = {
    .name = "SBTSI",
    .version_id = 0,
    .minimum_version_id = 0,
    .post_load = sbtsi_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(temperature, SBTSIState),
        VMSTATE_UINT32(limit_low, SBTSIState),
        VMSTATE_UINT32(limit_high, SBTSIState),
        VMSTATE_UINT8(config, SBTSIState),
        VMSTATE_UINT8(status, SBTSIState),
        VMSTATE_END_OF_LIST()
    }
};

static void sbtsi_init(Object *obj)
{
    SBTSIState *s = SBTSI(obj);

    /* Current temperature in millidegrees. */
    object_property_add(obj, "temperature", "uint32",
                        sbtsi_get_temperature, sbtsi_set_temperature,
                        NULL, NULL);
    qdev_init_gpio_out_named(DEVICE(obj), &s->alarm, SBTSI_ALARM_L, 0);
}

static void sbtsi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->desc = "SB-TSI Sensor";
    dc->vmsd = &vmstate_sbtsi;
}

static const TypeInfo sbtsi_info = {
    .name          = TYPE_SBTSI,
    .parent        = TYPE_DEVICE,
    .instance_size = sizeof(SBTSIState),
    .instance_init = sbtsi_init,
    .class_init    = sbtsi_class_init,
};

static void sbtsi_register_types(void)
{
    type_register_static(&sbtsi_info);
}

type_init(sbtsi_register_types)

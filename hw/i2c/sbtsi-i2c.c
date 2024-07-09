/*
 * AMD SBI Temperature Sensor Interface (SB-TSI) through I2C SMBUS.
 *
 * Copyright 2021 Google LLC
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
#include "hw/i2c/smbus_slave.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "trace.h"
#include "hw/sensor/sbtsi.h"

#define TYPE_SBTSI_I2C_TARGET "sbtsi-i2c-target"
OBJECT_DECLARE_SIMPLE_TYPE(SbtsiI2cTargetState, SBTSI_I2C_TARGET)

/**
 * SbtsiI2cTargetState:
 * @addr: The address to read/write for the next cmd.
 * @sbtsi: The temperature sensor.
 */
typedef struct SbtsiI2cTargetState {
    SMBusDevice parent;

    uint8_t addr;
    SBTSIState sbtsi;
} SbtsiI2cTargetState;

static uint8_t sbtsi_i2c_target_read_byte(SMBusDevice *d)
{

    SbtsiI2cTargetState *s = SBTSI_I2C_TARGET(d);

    return sbtsi_read(&s->sbtsi, s->addr);
}

static int sbtsi_i2c_target_write_data(SMBusDevice *d, uint8_t *buf,
                                       uint8_t len)
{
    SbtsiI2cTargetState *s = SBTSI_I2C_TARGET(d);

    if (len == 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: writing empty data\n", __func__);
        return -1;
    }

    s->addr = buf[0];
    if (len > 1) {
        sbtsi_write(&s->sbtsi, s->addr, buf[1]);
        if (len > 2) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: extra data at end\n", __func__);
        }
    }
    return 0;
}

static void sbtsi_i2c_target_enter_reset(Object *obj, ResetType type)
{
    SbtsiI2cTargetState *s = SBTSI_I2C_TARGET(obj);
    SMBusDeviceClass *sdc = SMBUS_DEVICE_GET_CLASS(&s->parent);

    /* reset temperature sensor */
    sbtsi_reset(&s->sbtsi);

    if (sdc->parent_phases.enter) {
        sdc->parent_phases.enter(obj, type);
    }
}

static void sbtsi_i2c_target_hold_reset(Object *obj, ResetType type)
{
    SbtsiI2cTargetState *s = SBTSI_I2C_TARGET(obj);

    /* deassert irq */
    sbtsi_hold_reset(&s->sbtsi, type);
}

static void sbtsi_i2c_target_init(Object *obj)
{
    SbtsiI2cTargetState *s = SBTSI_I2C_TARGET(obj);

    /* Initialize the temperature sensor */
    object_initialize_child(obj, "sbtsi", &s->sbtsi, TYPE_SBTSI);
}

static void sbtsi_i2c_target_realize(DeviceState *dev, Error **errp)
{
    SbtsiI2cTargetState *s = SBTSI_I2C_TARGET(dev);

    if (!qdev_realize(DEVICE(&s->sbtsi), NULL, errp)) {
        return;
    }

    sbtsi_reset(&s->sbtsi);
}

static const VMStateDescription vmstate_sbtsi = {
    .name = "SBTSI",
    .version_id = 0,
    .minimum_version_id = 0,
    .fields = (VMStateField[]) {
        VMSTATE_UINT8(addr, SbtsiI2cTargetState),
        VMSTATE_END_OF_LIST()
    }
};

static void sbtsi_i2c_target_class_init(ObjectClass *klass, void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);
    SMBusDeviceClass *k = SMBUS_DEVICE_CLASS(klass);

    dc->desc = "SB-TSI SMBUS device";
    dc->realize = sbtsi_i2c_target_realize;
    dc->vmsd = &vmstate_sbtsi;

    k->write_data = sbtsi_i2c_target_write_data;
    k->receive_byte = sbtsi_i2c_target_read_byte;
    rc->phases.enter = sbtsi_i2c_target_enter_reset;
    rc->phases.hold = sbtsi_i2c_target_hold_reset;
}

static const TypeInfo sbtsi_i2c_target_info = {
    .name          = TYPE_SBTSI_I2C_TARGET,
    .parent        = TYPE_SMBUS_DEVICE,
    .instance_size = sizeof(SbtsiI2cTargetState),
    .instance_init = sbtsi_i2c_target_init,
    .class_init    = sbtsi_i2c_target_class_init,
};

static void sbtsi_i2c_target_register_types(void)
{
    type_register_static(&sbtsi_i2c_target_info);
}

type_init(sbtsi_i2c_target_register_types)

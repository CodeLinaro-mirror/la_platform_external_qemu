/*
 * SBTSI I3C target device.
 * Based on PPR Vol 5 for AMD Family 1Ah Model 02h C0 (9.2 SB-TSI Protocol)
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
#include "qemu/log.h"
#include "hw/i3c/i3c.h"
#include "hw/i3c/sbtsi-i3c.h"
#include "hw/qdev-properties.h"
#include "hw/sensor/sbtsi.h"
#include "trace.h"

static uint32_t sbtsi_i3c_target_rx(I3CTarget *i3c, uint8_t *data,
                               uint32_t num_to_read)
{
    SbtsiI3cTargetState *s = SBTSI_I3C_TARGET(i3c);

    if (s->curr_event != I3C_START_RECV) {
        qemu_log_mask(LOG_GUEST_ERROR, "Unexpected rx in event=%d\n",
                      s->curr_event);
        return -1;
    }

    if (!s->command_code_received) {
        /* incomplete command code on read */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Unexpected rx while receiving command code\n");
        return -1;
    }

    *data = sbtsi_read(&s->sbtsi, s->command_code);

    trace_sbtsi_i3c_target_rx(s->name, s->command_code, *data);

    return num_to_read;
}

static int sbtsi_i3c_target_tx(I3CTarget *i3c, const uint8_t *data,
                          uint32_t num_to_send, uint32_t *num_sent)
{
    SbtsiI3cTargetState *s = SBTSI_I3C_TARGET(i3c);

    if (s->curr_event != I3C_START_SEND) {
        qemu_log_mask(LOG_GUEST_ERROR, "Unexpected tx in event=%d\n",
                      s->curr_event);
        return -1;
    }

    if (!s->command_code_received) {
        /* receiving command code */
        s->command_code = *data;
        s->command_code_received = true;
        *num_sent = 1;

        trace_sbtsi_i3c_target_tx_new_command(s->name, s->command_code);
        return 0;
    }

    /* command code complete */
    trace_sbtsi_i3c_target_tx(s->name, s->command_code, *data);

    /* register write */
    sbtsi_write(&s->sbtsi, s->command_code, *data);

    *num_sent = num_to_send;
    return 0;
}

static int sbtsi_i3c_target_handle_ccc_read(I3CTarget *i3c, uint8_t *data,
                                       uint32_t num_to_read, uint32_t *num_read)
{
    return 0;
}

static int sbtsi_i3c_target_handle_ccc_write(I3CTarget *i3c,
                                        const uint8_t *data,
                                        uint32_t num_to_send,
                                        uint32_t *num_sent)
{
    return 0;
}

static int sbtsi_i3c_target_event(I3CTarget *i3c, enum I3CEvent event)
{
    SbtsiI3cTargetState *s = SBTSI_I3C_TARGET(i3c);

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

    trace_sbtsi_i3c_target_event(s->name, event);

    /* Update the event */
    s->curr_event = event;
    return 0;
}

static void sbtsi_i3c_target_reset(I3CTarget *i3c)
{
    SbtsiI3cTargetState *s = SBTSI_I3C_TARGET(i3c);
    s->curr_event = I3C_STOP;
    s->command_code = 0;
    s->command_code_received = 0;
    sbtsi_reset(&s->sbtsi);
}

static void sbtsi_i3c_target_enter_reset(Object *obj, ResetType type)
{
    SbtsiI3cTargetState *s = SBTSI_I3C_TARGET(obj);
    sbtsi_reset(&s->sbtsi);
}

static void sbtsi_i3c_target_hold_reset(Object *obj, ResetType type)
{
    SbtsiI3cTargetState *s = SBTSI_I3C_TARGET(obj);
    sbtsi_hold_reset(&s->sbtsi, type);
}

static void sbtsi_i3c_target_realize(DeviceState *dev, Error **errp)
{
    SbtsiI3cTargetState *s = SBTSI_I3C_TARGET(dev);

    if (!qdev_realize(DEVICE(&s->sbtsi), NULL, errp)) {
        return;
    }

    sbtsi_i3c_target_reset(&s->i3c);
}

static const Property sbtsi_i3c_props[] = {
    DEFINE_PROP_STRING("device-name", SbtsiI3cTargetState, name),
};

static void sbtsi_i3c_target_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    I3CTargetClass *k = I3C_TARGET_CLASS(klass);

    k->event = sbtsi_i3c_target_event;
    k->recv = sbtsi_i3c_target_rx;
    k->send = sbtsi_i3c_target_tx;
    k->handle_ccc_read = sbtsi_i3c_target_handle_ccc_read;
    k->handle_ccc_write = sbtsi_i3c_target_handle_ccc_write;

    rc->phases.enter = sbtsi_i3c_target_enter_reset;
    rc->phases.hold = sbtsi_i3c_target_hold_reset;

    dc->realize = sbtsi_i3c_target_realize;
    device_class_set_props(dc, sbtsi_i3c_props);
}

static void sbtsi_i3c_target_init(Object *obj)
{
    SbtsiI3cTargetState *s = SBTSI_I3C_TARGET(obj);

    /* Initialize the temperature sensor */
    object_initialize_child(obj, "sbtsi", &s->sbtsi, TYPE_SBTSI);
}

static const TypeInfo sbtsi_i3c_target_info = {
    .name          = TYPE_SBTSI_I3C_TARGET,
    .parent        = TYPE_I3C_TARGET,
    .instance_size = sizeof(SbtsiI3cTargetState),
    .instance_init = sbtsi_i3c_target_init,
    .class_init    = sbtsi_i3c_target_class_init,
};

static void sbtsi_i3c_target_register_types(void)
{
    type_register_static(&sbtsi_i3c_target_info);
}

type_init(sbtsi_i3c_target_register_types)

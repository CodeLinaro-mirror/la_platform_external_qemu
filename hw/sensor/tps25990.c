/*
 * Texas Instruments TPS25990 PMBus High Power Positive Hot Swap Controller
 *
 * Datasheet: https://www.ti.com/lit/ds/symlink/tps25990.pdf
 *
 * Copyright 2025 Google LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/i2c/pmbus_device.h"
#include "hw/core/qdev.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "qemu/typedefs.h"
#include "qapi/visitor.h"
#include "qom/object.h"
#include "qemu/log.h"

#define TYPE_TPS25990 "tps25990"
OBJECT_DECLARE_SIMPLE_TYPE(TPS25990State, TPS25990)

#define TPS25990_NUM_PAGES       1

#define TPS25990_DEFAULT_OPERATION               0x80
#define TPS25990_DEFAULT_CAPABILITY              0xD0
#define TPS25990_DEFAULT_VOUT_MODE               0x40
#define TPS25990_DEFAULT_REVISION                0x01

typedef struct TPS25990State {
    PMBusDevice parent;
} TPS25990State;

static uint8_t tps25990_read_byte(PMBusDevice *pmdev)
{
    TPS25990State *s = TPS25990(pmdev);

    switch (pmdev->code) {
    case PMBUS_MFR_ID:
        pmbus_send_string(pmdev, "TI");
        break;

    case PMBUS_MFR_MODEL:
        pmbus_send_string(pmdev, "TPS25990");
        break;

    case PMBUS_MFR_REVISION:
    {
        uint8_t revision = TPS25990_DEFAULT_REVISION;
        pmbus_send(pmdev, &revision, sizeof(revision));
        break;
    }
        break;

    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: %s: reading from unimplemented register: 0x%02x\n",
                      DEVICE(s)->canonical_path, __func__, pmdev->code);
        return PMBUS_ERR_BYTE;
    }
    return 0;
}

static int tps25990_write_data(PMBusDevice *pmdev, const uint8_t *buf,
                              uint8_t len)
{
    TPS25990State *s = TPS25990(pmdev);

    switch (pmdev->code) {

    case PMBUS_MFR_ID:
    case PMBUS_MFR_MODEL:
    case PMBUS_MFR_REVISION:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: writing to read only register: 0x%02x\n",
                      DEVICE(s)->canonical_path, __func__, pmdev->code);
        break;

    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: %s: writing to unimplemented register: 0x%02x\n",
                      DEVICE(s)->canonical_path, __func__, pmdev->code);
        return 0;
    }
    return 0;
}

static void tps25990_exit_reset(Object *obj, ResetType type)
{
    PMBusDevice *pmdev = PMBUS_DEVICE(obj);

    pmdev->capability = TPS25990_DEFAULT_CAPABILITY;
    pmdev->pages[0].operation = TPS25990_DEFAULT_OPERATION;
    pmdev->pages[0].vout_mode = TPS25990_DEFAULT_VOUT_MODE;
    pmdev->pages[0].revision = TPS25990_DEFAULT_REVISION;

    /* random sensor readings */
    pmdev->pages[0].read_vin = 0x100;
    pmdev->pages[0].read_iin = 0x10;
    pmdev->pages[0].read_iout = 0x100;
    pmdev->pages[0].read_vout = 0x10;
    pmdev->pages[0].read_temperature_1 = 30;
}

static void tps25990_get(Object *obj, Visitor *v, const char *name, void *opaque,
                        Error **errp)
{
    uint16_t value;

    value = *(uint16_t *)opaque;

    if (!(strcmp(name, "temperature") == 0 || strcmp(name, "pin") == 0)) {
        value *= 1000; /* use milliunits for qmp */
    }

    visit_type_uint16(v, name, &value, errp);
}

static void tps25990_set(Object *obj, Visitor *v, const char *name, void *opaque,
                        Error **errp)
{
    uint16_t *internal = opaque;
    uint16_t value;
    PMBusDevice *pmdev = PMBUS_DEVICE(obj);

    if (!visit_type_uint16(v, name, &value, errp)) {
        return;
    }

    if (!(strcmp(name, "temperature") == 0 || strcmp(name, "pin") == 0)) {
        /* use milliunits for qmp */
        value /= 1000;
    }

    *internal = value;
    pmbus_check_limits(pmdev);
}

static void tps25990_init(Object *obj)
{
    PMBusDevice *pmdev = PMBUS_DEVICE(obj);
    uint64_t psu_flags = PB_HAS_VIN | PB_HAS_VOUT | PB_HAS_IIN | PB_HAS_IOUT
    | PB_HAS_PIN | PB_HAS_TEMPERATURE;

    pmbus_page_config(pmdev, 0, psu_flags);
    object_property_add(obj, "vin", "uint16", tps25990_get, tps25990_set,
                        NULL, &pmdev->pages[0].read_vin);
    object_property_add(obj, "vout", "uint16", tps25990_get, tps25990_set,
                        NULL, &pmdev->pages[0].read_vout);
    object_property_add(obj, "iin", "uint16", tps25990_get, tps25990_set,
                        NULL, &pmdev->pages[0].read_iin);
    object_property_add(obj, "iout", "uint16", tps25990_get, tps25990_set,
                        NULL, &pmdev->pages[0].read_iout);
    object_property_add(obj, "pin", "uint16", tps25990_get, tps25990_set,
                        NULL, &pmdev->pages[0].read_pin);
    object_property_add(obj, "temperature", "uint16", tps25990_get,
                        tps25990_set, NULL,
                        &pmdev->pages[0].read_temperature_1);
}

static void tps25990_class_init(ObjectClass *klass, const void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);
    PMBusDeviceClass *k = PMBUS_DEVICE_CLASS(klass);

    dc->desc = "Texas Instruments TPS25990";
    k->write_data = tps25990_write_data;
    k->receive_byte = tps25990_read_byte;
    k->device_num_pages = TPS25990_NUM_PAGES;
    rc->phases.exit = tps25990_exit_reset;
}

static const TypeInfo tps25990_info = {
    .name = TYPE_TPS25990,
    .parent = TYPE_PMBUS_DEVICE,
    .instance_size = sizeof(TPS25990State),
    .instance_init = tps25990_init,
    .class_init = tps25990_class_init,
};

static void tps25990_register_types(void)
{
    type_register_static(&tps25990_info);
}

type_init(tps25990_register_types)

/*
 * Texas Instruments TPS546D24 2.95-V to 16-V, 40-A, up to 4x Stackable, PMBus
 * Buck Converter
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
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/typedefs.h"
#include "qom/object.h"
#include "trace.h"

#define TYPE_TPS546D24 "tps546d24"
OBJECT_DECLARE_SIMPLE_TYPE(TPS546D24State, TPS546D24)

#define TPS546D24_NUM_PAGES       1

#define TPS546D24_DEFAULT_OPERATION               0x80
#define TPS546D24_DEFAULT_CAPABILITY              0x20
#define TPS546D24_DEFAULT_VOUT_MODE               0x16

typedef struct TPS546D24State {
    PMBusDevice parent;
} TPS546D24State;

static const VMStateDescription vmstate_tps546d24 = {
    .name = TYPE_TPS546D24,
    .version_id = 0,
    .minimum_version_id = 0,
    .fields = (VMStateField[]){
        VMSTATE_PMBUS_DEVICE(parent, TPS546D24State),
        VMSTATE_END_OF_LIST()
    }
};

static void tps546d24_exit_reset(Object *obj, ResetType type)
{
    PMBusDevice *pmdev = PMBUS_DEVICE(obj);

    pmdev->capability = TPS546D24_DEFAULT_CAPABILITY;
    pmdev->pages[0].operation = TPS546D24_DEFAULT_OPERATION;
    pmdev->pages[0].vout_mode = TPS546D24_DEFAULT_VOUT_MODE;

    /* random sensor readings */
    pmdev->pages[0].read_vin = 0x100;
    pmdev->pages[0].read_vout = 0x100;
    pmdev->pages[0].read_iin = 0x10;
    pmdev->pages[0].read_temperature_1 = 30;
}

#define TPS546D24_DEFAULT_REVISION                0x01

static uint8_t tps546d24_read_byte(PMBusDevice *pmdev)
{
    TPS546D24State *s = TPS546D24(pmdev);

    trace_tps546d24_read(DEVICE(s)->canonical_path, pmdev->code);
    switch (pmdev->code) {
    case PMBUS_MFR_ID:
        pmbus_send_string(pmdev, "TI");
        break;

    case PMBUS_MFR_MODEL:
        pmbus_send_string(pmdev, "TPS546D24");
        break;

    case PMBUS_MFR_REVISION:
    {
        uint8_t revision = TPS546D24_DEFAULT_REVISION;
        pmbus_send(pmdev, &revision, sizeof(revision));
        break;
    }

    case PMBUS_MFR_LOCATION:
    case PMBUS_MFR_DATE:
    case PMBUS_MFR_SERIAL:
    case PMBUS_IC_DEVICE_ID:
    case PMBUS_IC_DEVICE_REV:
        pmbus_send_string(pmdev, "");
        break;

    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: %s: reading from unimplemented register: 0x%02x\n",
                      DEVICE(s)->canonical_path, __func__, pmdev->code);
        return PMBUS_ERR_BYTE;
    }
    return 0;
}

static int tps546d24_write_data(PMBusDevice *pmdev, const uint8_t *buf,
                               uint8_t len)
{
    TPS546D24State *s = TPS546D24(pmdev);

    trace_tps546d24_write(DEVICE(s)->canonical_path, pmdev->code, len);
    switch (pmdev->code) {

    case PMBUS_MFR_ID:
    case PMBUS_MFR_MODEL:
    case PMBUS_MFR_REVISION:
    case PMBUS_MFR_LOCATION:
    case PMBUS_MFR_DATE:
    case PMBUS_MFR_SERIAL:
    case PMBUS_IC_DEVICE_ID:
    case PMBUS_IC_DEVICE_REV:
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

static void tps546d24_init(Object *obj)
{
    PMBusDevice *pmdev = PMBUS_DEVICE(obj);
    uint64_t psu_flags = PB_HAS_VIN | PB_HAS_IIN | PB_HAS_IOUT | PB_HAS_VOUT
    | PB_HAS_TEMPERATURE;

    pmbus_page_config(pmdev, 0, psu_flags);
}

static void tps546d24_class_init(ObjectClass *klass, const void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);
    PMBusDeviceClass *k = PMBUS_DEVICE_CLASS(klass);

    dc->desc = "Texas Instruments TPS546D24";
    dc->vmsd = &vmstate_tps546d24;
    k->write_data = tps546d24_write_data;
    k->receive_byte = tps546d24_read_byte;
    k->device_num_pages = TPS546D24_NUM_PAGES;
    rc->phases.exit = tps546d24_exit_reset;
}

static const TypeInfo tps546d24_info = {
    .name = TYPE_TPS546D24,
    .parent = TYPE_PMBUS_DEVICE,
    .instance_size = sizeof(TPS546D24State),
    .instance_init = tps546d24_init,
    .class_init = tps546d24_class_init,
};

static void tps546d24_register_types(void)
{
    type_register_static(&tps546d24_info);
}

type_init(tps546d24_register_types)

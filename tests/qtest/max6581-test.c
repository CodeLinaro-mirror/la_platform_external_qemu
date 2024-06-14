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
#include "hw/sensor/max6581.h"
#include "libqos/i2c.h"
#include "libqos/libqos-malloc.h"
#include "libqos/qgraph.h"
#include "libqtest-single.h"
#include "qobject/qdict.h"
#include "qobject/qnum.h"
#include "qemu/bitops.h"
#include "qemu/module.h"

#define TEST_ID "max6581-test"

static int32_t qmp_max6581_get(const char *id, const char *property)
{
    QDict *response;
    int32_t ret;
    response = qmp("{ 'execute': 'qom-get', 'arguments': { 'path': %s, "
                   "'property': %s } }", id, property);
    g_assert(qdict_haskey(response, "return"));
    ret = qdict_get_int(response, "return");
    qobject_unref(response);
    return ret;
}

static void qmp_max6581_set(const char *id,
                             const char *property,
                             int32_t value)
{
    QDict *response;

    response = qmp("{ 'execute': 'qom-set', 'arguments': { 'path': %s, "
                   "'property': %s, 'value': %d } }",
                   id, property, value);
    g_assert(qdict_haskey(response, "return"));
    qobject_unref(response);
}

/* Test read write registers can be written to */
static void test_read_write(void *obj, void *data, QGuestAllocator *alloc)
{
    QI2CDevice *i2cdev = (QI2CDevice *)obj;
    uint8_t value;
    int32_t temperature; /* degrees */

    /* R/W should be writable */
    i2c_set8(i2cdev, A_OVERT_MASK, 0x40);
    value = i2c_get8(i2cdev, A_OVERT_MASK);
    g_assert_cmphex(value, ==, 0x40);

    i2c_set8(i2cdev, A_OVERT_MASK, 0x00);
    value = i2c_get8(i2cdev, A_OVERT_MASK);
    g_assert_cmphex(value, ==, 0x00);

    /* RO should not be writable*/
    i2c_set8(i2cdev, A_REMOTE_1_TEMPERATURE, 40);
    value = i2c_get8(i2cdev, A_REMOTE_1_TEMPERATURE);
    g_assert_cmphex(value, !=, 0x40);

    i2c_set8(i2cdev, A_ALERT_HIGH_STATUS, 0xFF);
    value = i2c_get8(i2cdev, A_ALERT_HIGH_STATUS);
    g_assert_cmphex(value, !=, 0xFF);

    /* QMP should read and write temperatures */
    qmp_max6581_set(TEST_ID, "temperature[0]", 254875);
    temperature = qmp_max6581_get(TEST_ID, "temperature[0]");
    g_assert_cmpint(temperature, ==, 254875);

    qmp_max6581_set(TEST_ID, "temperature[1]", 253750);
    temperature = qmp_max6581_get(TEST_ID, "temperature[1]");
    g_assert_cmpint(temperature, ==, 253750);

    qmp_max6581_set(TEST_ID, "temperature[2]", 2625);
    temperature = qmp_max6581_get(TEST_ID, "temperature[2]");
    g_assert_cmpint(temperature, ==, 2625);

    qmp_max6581_set(TEST_ID, "temperature[3]", 1000);
    temperature = qmp_max6581_get(TEST_ID, "temperature[3]");
    g_assert_cmpint(temperature, ==, 1000);

    qmp_max6581_set(TEST_ID, "temperature[4]", 40500);
    temperature = qmp_max6581_get(TEST_ID, "temperature[4]");
    g_assert_cmpint(temperature, ==, 40500);

    qmp_max6581_set(TEST_ID, "temperature[5]", 50375);
    temperature = qmp_max6581_get(TEST_ID, "temperature[5]");
    g_assert_cmpint(temperature, ==, 50375);

    qmp_max6581_set(TEST_ID, "temperature[6]", 60250);
    temperature = qmp_max6581_get(TEST_ID, "temperature[6]");
    g_assert_cmpint(temperature, ==, 60250);

    qmp_max6581_set(TEST_ID, "temperature[7]", 70125);
    temperature = qmp_max6581_get(TEST_ID, "temperature[7]");
    g_assert_cmpint(temperature, ==, 70125);
}

/**
 *  Test Status Registers
 *
 *  The ALERT and OVERT status registers indicate over-temperature and
 *  under-temperature faults.
 *  - The ALERT High Status and OVERT Status registers indicate whether a local
 *    or remote temperature has exceeded threshold limits set in the associated
 *    ALERT and OVERT High Limit registers.
 *  - The ALERT Low Status register indicates whether the measured
 *      temperature has fallen below the threshold limit set in the
 *      ALERT Low Limits register for the local and remote sensing diodes.
 *
 *  Bits in the ALERT status registers are cleared by a successful read but
 *  set again after the next conversion unless the fault is corrected, either by
 *  a change in the measured temperature or by a change in the threshold
 *  temperature.
 *  Bits in the OVERT status register are cleared only once the temperature
 *  has fallen 4 degrees Celcius below the OVERT threshold.
 */
/* Test ALERT high status */
static void test_alert_high_temperature(void *obj,
    void *data, QGuestAllocator *alloc)
{
    QI2CDevice *i2cdev = (QI2CDevice *)obj;
    char *path;
    uint8_t value;

    /* set thresholds to [60C - 67C] */
    for (int i = 0; i < MAX6581_NUM_TEMPS; i++) {
        i2c_set8(i2cdev, A_REMOTE_1_ALERT_HIGH_LIMIT + i, 60 + i);
    }

    /* use qmp to set temperatures to [61C - 68C] so each diode is 1C over */
    for (int i = 0; i < MAX6581_NUM_TEMPS; i++) {
        path = g_strdup_printf("temperature[%d]", i);
        qmp_max6581_set(TEST_ID, path, (61 + i) * 1000);
    }
    /* check that the alert high status gets raised */
    value = i2c_get8(i2cdev, A_ALERT_HIGH_STATUS);
    g_assert_cmphex(value, ==, 0xFF);

    /* use qmp to set temperatures to [59C - 66C] so each diode is 1C under */
    for (int i = 0; i < MAX6581_NUM_TEMPS; i++) {
        path = g_strdup_printf("temperature[%d]", i);
        qmp_max6581_set(TEST_ID, path, (59 + i) * 1000);
    }

    /*
     * check that the alert high status gets lowered after reading.
     * ALERT status updated after diode 1 temperature is changed.
     */
    value = i2c_get8(i2cdev, A_ALERT_HIGH_STATUS);
    g_assert_cmphex(value, ==, 0xFE);

    value = i2c_get8(i2cdev, A_ALERT_HIGH_STATUS);
    g_assert_cmphex(value, ==, 0);
}

/* Test ALERT low status */
static void test_alert_low_temperature(void *obj,
    void *data, QGuestAllocator *alloc)
{
    QI2CDevice *i2cdev = (QI2CDevice *)obj;
    char *path;
    uint8_t value;

    /* set threshold to 2C */
    i2c_set8(i2cdev, A_ALERT_LOW_LIMITS, 2);

    /* use qmp to set temperatures to 1C so each diode is 1C under */
    for (int i = 0; i < MAX6581_NUM_TEMPS; i++) {
        path = g_strdup_printf("temperature[%d]", i);
        qmp_max6581_set(TEST_ID, path, 1000);
    }
    /* check that the low temperature status gets raised */
    value = i2c_get8(i2cdev, A_ALERT_LOW_STATUS);
    g_assert_cmphex(value, ==, 0xFF);

    /* use qmp to set temperatures to 3C so each diode is 1C over */
    for (int i = 0; i < MAX6581_NUM_TEMPS; i++) {
        path = g_strdup_printf("temperature[%d]", i);
        qmp_max6581_set(TEST_ID, path, 3000);
    }

    /*
     * check that the alert low status gets lowered after reading.
     * ALERT status updated after diode 1 temperature is changed.
     */
    value = i2c_get8(i2cdev, A_ALERT_LOW_STATUS);
    g_assert_cmphex(value, ==, 0xFE);

    value = i2c_get8(i2cdev, A_ALERT_LOW_STATUS);
    g_assert_cmphex(value, ==, 0);
}

/* Test OVERT status */
static void test_overt_temperature(void *obj,
    void *data, QGuestAllocator *alloc)
{
    QI2CDevice *i2cdev = (QI2CDevice *)obj;
    char *path;
    uint8_t value;

    /*
     * set thresholds to [60C - 67C]
     * account for placement of local and remote 7 diode addresses
     */
    for (int i = 0; i < MAX6581_NUM_TEMPS - 2; i++) {
        i2c_set8(i2cdev, A_REMOTE_1_OVERT_HIGH_LIMIT + i, 60 + i);
    }
    i2c_set8(i2cdev, A_LOCAL_OVERT_HIGH_LIMIT, 66);
    i2c_set8(i2cdev, A_REMOTE_7_OVERT_HIGH_LIMIT, 67);

    /* use qmp to set temperatures to [61C - 68C] so each diode is 1C over */
    for (int i = 0; i < MAX6581_NUM_TEMPS; i++) {
        path = g_strdup_printf("temperature[%d]", i);
        qmp_max6581_set(TEST_ID, path, (61 + i) * 1000);
    }
    /* check that the overt status gets raised */
    value = i2c_get8(i2cdev, A_OVERT_STATUS);
    g_assert_cmphex(value, ==, 0xFF);

    /* use qmp to set temperatures to [56C - 63C] so each diode is 1C under */
    for (int i = 0; i < MAX6581_NUM_TEMPS; i++) {
        path = g_strdup_printf("temperature[%d]", i);
        qmp_max6581_set(TEST_ID, path, (56 + i) * 1000);
    }

    /* check that the OVERT status gets lowered */
    value = i2c_get8(i2cdev, A_OVERT_STATUS);
    g_assert_cmphex(value, ==, 0);
}

static void max6581_register_nodes(void)
{
    QOSGraphEdgeOptions opts = {
        .extra_device_opts = "id=" TEST_ID ",address=0x4d"
    };
    add_qi2c_address(&opts, &(QI2CAddress) { 0x4d });

    qos_node_create_driver("max6581", i2c_device_create);
    qos_node_consumes("max6581", "i2c-bus", &opts);

    qos_add_test("test_read_write", "max6581", test_read_write, NULL);
    qos_add_test("test_alert_high_temp", "max6581",
                 test_alert_high_temperature, NULL);
    qos_add_test("test_alert_low_temp", "max6581",
                 test_alert_low_temperature, NULL);
    qos_add_test("test_overt_temp", "max6581", test_overt_temperature, NULL);
}
libqos_init(max6581_register_nodes);

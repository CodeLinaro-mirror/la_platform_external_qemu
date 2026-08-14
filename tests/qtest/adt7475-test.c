/*
 * ADT7475 remote thermal monitor and fan controller
 *
 * Features:
 *  - Control and monitor up to 4 fans
 *  - 1 On-Chip and 2 Remote temperature sensors
 *
 * Datasheet:
 * https://www.onsemi.com/download/data-sheet/pdf/adt7475-d.pdf
 *
 * Copyright 2025 Google LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/sensor/adt7475.h"
#include "libqos/i2c.h"
#include "libqos/libqos-malloc.h"
#include "libqos/qgraph.h"
#include "libqtest-single.h"
#include "qobject/qdict.h"
#include "qobject/qnum.h"
#include "qemu/bitops.h"
#include "qemu/module.h"

#define TEST_ID "adt7475-test"

#define ENABLE_OFFSET64     (CONFIGURATION_5_DEFAULT & ~R_CONFIGURATION_5_TWOS_COMPL_MASK)
#define EXTENDED_TEMP_0     0
#define EXTENDED_TEMP_25    1
#define EXTENDED_TEMP_50    2
#define EXTENDED_TEMP_75    3

#define SHIFT_REMOTE_1_EXTENDED(extended_temperature_) \
    extract8(extended_temperature_, R_EXTENDED_RESOLUTION_2_TDM1_SHIFT, 2);
#define SHIFT_LOCAL_EXTENDED(extended_temperature_) \
    extract8(extended_temperature_, R_EXTENDED_RESOLUTION_2_LTMP_SHIFT, 2);
#define SHIFT_REMOTE_2_EXTENDED(extended_temperature_) \
    extract8(extended_temperature_, R_EXTENDED_RESOLUTION_2_TDM2_SHIFT, 2);
#define SHIFT_VCC_EXTENDED(extended_voltage_) \
    extract8(extended_voltage_, R_EXTENDED_RESOLUTION_1_VCC_SHIFT, 2);
#define SHIFT_VCCP_EXTENDED(extended_voltage_) \
    extract8(extended_voltage_, R_EXTENDED_RESOLUTION_1_VCCP_SHIFT, 2);

static void check_tach_reading(QI2CDevice *i2cdev, uint8_t tach_low_reg,
                               uint16_t tach_reading)
{
    g_assert_cmphex(i2c_get8(i2cdev, tach_low_reg), ==, tach_reading & 0xFF);
    g_assert_cmphex(i2c_get8(i2cdev, tach_low_reg + 1), ==, (tach_reading >> 8) & 0xFF);
}

static void check_interrupts(QI2CDevice *i2cdev, uint8_t interrupt_1,
                             uint8_t interrupt_2)
{
    g_assert_cmpint(i2c_get8(i2cdev, A_INTERRUPT_STATUS_1), ==, interrupt_1);
    g_assert_cmpint(i2c_get8(i2cdev, A_INTERRUPT_STATUS_2), ==, interrupt_2);
}

static int32_t qmp_adt7475_get(const char *id, const char *property)
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

static void qmp_adt7475_set(const char *id,
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

static void test_read_write(void *obj, void *data, QGuestAllocator *alloc)
{
    QI2CDevice *i2cdev = (QI2CDevice *)obj;
    uint8_t value;

    /* R/W should be writable */
    i2c_set8(i2cdev, A_CONFIGURATION_6, 0xFF);
    value = i2c_get8(i2cdev, A_CONFIGURATION_6);
    g_assert_cmphex(value, ==, 0xFF);

    i2c_set8(i2cdev, A_CONFIGURATION_6, 0x00);
    value = i2c_get8(i2cdev, A_CONFIGURATION_6);
    g_assert_cmphex(value, ==, 0x00);

    /* RO should not be writable*/
    i2c_set8(i2cdev, A_CONFIGURATION_7, 0xFF);
    value = i2c_get8(i2cdev, A_CONFIGURATION_7);
    g_assert_cmphex(value, ==, 0);

    /* Temperature sensors */
    /* Signed temperature format */
    int32_t signed_temperature[] = { -128250, -20500, 126750 };
    int32_t expected_qmp_signed_temperature[] = { -128000, -20500, 126750 };
    int8_t expected_temperature[] = { -128, -21, 126 };
    uint8_t expected_extended_temperature[] = {
        EXTENDED_TEMP_0, EXTENDED_TEMP_50, EXTENDED_TEMP_75
    };
    int32_t read_qmp_temperature;
    int8_t read_temperature;
    uint8_t read_extended_temperature;

    uint8_t test_arr_size = sizeof(signed_temperature) / sizeof(signed_temperature[0]);
    for (int i = 0; i < test_arr_size; i++) {
        qmp_adt7475_set(TEST_ID, "temperature[0]", signed_temperature[i]);
        read_qmp_temperature = qmp_adt7475_get(TEST_ID, "temperature[0]");
        read_temperature = i2c_get8(i2cdev, A_REMOTE_1_TEMP);
        read_extended_temperature = i2c_get8(i2cdev, A_EXTENDED_RESOLUTION_2);
        read_extended_temperature = SHIFT_REMOTE_1_EXTENDED(read_extended_temperature);
        g_assert_cmpint(read_qmp_temperature, ==, expected_qmp_signed_temperature[i]);
        g_assert_cmpint(read_temperature, ==, expected_temperature[i]);
        g_assert_cmpint(read_extended_temperature, ==, expected_extended_temperature[i]);
    }


    /* Offset 64 temperature format */
    int32_t offset64_temperature[] = { -63500, 191500 };
    int32_t expected_qmp_offset64_temperature[] = { -63500, 191000 };
    uint8_t expected_offset64_temperature[] = { 0, 255 };
    uint8_t expected_extended_offset64_temperature[] = {
        EXTENDED_TEMP_50, EXTENDED_TEMP_0
    };
    uint8_t unsigned_read_temperature;

    i2c_set8(i2cdev, A_CONFIGURATION_5, ENABLE_OFFSET64);
    uint8_t config_5 = i2c_get8(i2cdev, A_CONFIGURATION_5);
    g_assert_cmpint(config_5, ==, ENABLE_OFFSET64);

    test_arr_size = sizeof(offset64_temperature) / sizeof(offset64_temperature[0]);
    for (int i = 0; i < test_arr_size; i++) {
        qmp_adt7475_set(TEST_ID, "temperature[1]", offset64_temperature[i]);
        read_qmp_temperature = qmp_adt7475_get(TEST_ID, "temperature[1]");
        unsigned_read_temperature = i2c_get8(i2cdev, A_LOCAL_TEMP);
        read_extended_temperature = i2c_get8(i2cdev, A_EXTENDED_RESOLUTION_2);
        read_extended_temperature = SHIFT_LOCAL_EXTENDED(read_extended_temperature);
        g_assert_cmpint(read_qmp_temperature, ==, expected_qmp_offset64_temperature[i]);
        g_assert_cmpint(unsigned_read_temperature, ==, expected_offset64_temperature[i]);
        g_assert_cmpint(read_extended_temperature, ==,
                        expected_extended_offset64_temperature[i]);
    }

    /* lower temperatures to mitigate overtemperature event */
    qmp_adt7475_set(TEST_ID, "temperature[0]", 0);
    qmp_adt7475_set(TEST_ID, "temperature[1]", 0);
    qmp_adt7475_set(TEST_ID, "temperature[2]", 0);

    /* Tachometers */
    uint16_t tach_reading[] = { 0xFFFF, 0xFF10, 0x10FF, 0x1010 };
    const char *tach_name[] = {
        "tachometer[0]", "tachometer[1]", "tachometer[2]", "tachometer[3]"
    };
    uint16_t read_tach_reading;

    test_arr_size = sizeof(tach_reading) / sizeof(tach_reading[0]);
    for (int i = 0; i < test_arr_size; i++) {
        qmp_adt7475_set(TEST_ID, tach_name[i], tach_reading[i]);
        read_tach_reading = qmp_adt7475_get(TEST_ID, tach_name[i]);
        g_assert_cmpint(read_tach_reading, ==, tach_reading[i]);
    }


    /* Voltages */
    /*
     * Voltage is stored across 10 bits. The two least significant bits are
     * considered extended resolution, providing a granularity across a range.
     * For Vccp, the granularity ranges are across a 0.0043V difference.
     * For Vcc, the granularity ranges are across a <=0.0033V difference, though
     * the data sheet has a typo, and the differences are all different.
     *
     * See page 13 of the spec.
     */
    uint8_t voltage[] = { 0, 253, 255, 255 };
    uint8_t extended_voltage[] = { 0, 1, 2, 3, };
    uint16_t full_voltage;
    uint16_t read_qmp_voltage;
    uint16_t read_voltage;
    uint8_t read_extended_voltage;

    /* Vcc */
    test_arr_size = sizeof(voltage) / sizeof(voltage[0]);
    for (int i = 0; i < test_arr_size; i++) {
        full_voltage = (voltage[i] << 2) + extended_voltage[i];
        qmp_adt7475_set(TEST_ID, "vcc", full_voltage);
        read_qmp_voltage = qmp_adt7475_get(TEST_ID, "vcc");
        read_voltage = i2c_get8(i2cdev, A_VCC_READING);
        read_extended_voltage = i2c_get8(i2cdev, A_EXTENDED_RESOLUTION_1);
        read_voltage = (read_voltage << 2) + SHIFT_VCC_EXTENDED(read_extended_voltage);
        g_assert_cmpint(read_qmp_voltage, ==, full_voltage);
        g_assert_cmpint(read_voltage, ==, full_voltage);
    }

    /* Vccp */
    for (int i = 0; i < test_arr_size; i++) {
        full_voltage = (voltage[i] << 2) + extended_voltage[i];
        qmp_adt7475_set(TEST_ID, "vccp", full_voltage);
        read_qmp_voltage = qmp_adt7475_get(TEST_ID, "vccp");
        read_voltage = i2c_get8(i2cdev, A_VCCP_READING);
        read_extended_voltage = i2c_get8(i2cdev, A_EXTENDED_RESOLUTION_1);
        read_voltage = (read_voltage << 2) + SHIFT_VCCP_EXTENDED(read_extended_voltage);
        g_assert_cmpint(read_qmp_voltage, ==, full_voltage);
        g_assert_cmpint(read_voltage, ==, full_voltage);
    }
}

/*
 * Convert decimal percentage to expected PWM* duty cycle format.
 *
 * The provided percentage must be in the range of [0, 10000].
 * The lower two digits of the percentage represent the fractional portion, and
 * are required.
 * e.g. 50.39% should be provided as 5039
 *
 * The ADT7475's PWM* duty cycle values can be set between 0% to 100%, going
 * by steps of 0.39%. Using steps of 0.39% gives a unique mapping to each
 * possible value of a byte.
 */
static uint8_t pwm_percent_to_adt7475_value(uint16_t percent)
{
    if (percent >= 9945) {
        return 255;
    }
    uint32_t full_percentage = ((uint32_t)percent) / 39;
    return (uint8_t)full_percentage;
}

static void test_fan_speed(void *obj, void *data, QGuestAllocator *alloc)
{
    QI2CDevice *i2cdev = (QI2CDevice *)obj;
    uint8_t pwm_percent;
    uint8_t read_pwm_percent;
    uint16_t tach_reading;

    /*
     * set each pwm duty cycle reg to 32.76%, expected fan speed is 824,
     * following the equation from page 25 of the datasheet.
     *
     * NOTE: Equation used to calculate `fan_speed` values (desmos is your friend):
     * - pwm_percent = readable_pwm_percent / 39
     * - rpm = ADT7475_RPM_MAX_DEFAULT * pwm_percent / 255 = 20000 * pwm_percent / 255
     * - fan_speed = (90,000 * 60) / rpm
     */
    pwm_percent = pwm_percent_to_adt7475_value(3276);
    tach_reading = 819;

    for (int i = 0; i < ADT7475_NUM_PWM; i++) {
        /* set pwm to manual mode */
        i2c_set8(i2cdev, A_PWM1_CONFIGURATION + i,
                 (BHVR_MANUAL_MODE << R_PWMX_CONFIGURATION_BHVR_SHIFT));
        i2c_set8(i2cdev, A_PWM1_CURRENT_DUTY_CYCLE + i, pwm_percent);
        read_pwm_percent = i2c_get8(i2cdev, A_PWM1_CURRENT_DUTY_CYCLE + i);
        check_tach_reading(i2cdev, A_TACH1_LOW_BYTE + (i * 2), tach_reading);
        g_assert_cmphex(read_pwm_percent, ==, pwm_percent);
    }

    /* check that setting PWM3 also sets TACH4 (for-loop tests TACH3) */
    check_tach_reading(i2cdev, A_TACH4_LOW_BYTE, tach_reading);

    /* use qmp to set tachometer to ensure it stays until PWM update */
    pwm_percent = pwm_percent_to_adt7475_value(5031);
    tach_reading = 533;
    qmp_adt7475_set(TEST_ID, "tachometer[2]", 0xCDEF);
    check_tach_reading(i2cdev, A_TACH3_LOW_BYTE, 0xCDEF);
    i2c_set8(i2cdev, A_PWM3_CURRENT_DUTY_CYCLE, pwm_percent);
    check_tach_reading(i2cdev, A_TACH3_LOW_BYTE, tach_reading);

    /* check SYNC feature; PWM3 controlls TACH2, TACH3, and TACH4 */
    i2c_set8(i2cdev, A_ENHANCED_ACOUSTICS_1, R_ENHANCED_ACOUSTICS_1_SYNC_MASK);
    pwm_percent = pwm_percent_to_adt7475_value(7527);
    tach_reading = 356;
    i2c_set8(i2cdev, A_PWM3_CURRENT_DUTY_CYCLE, pwm_percent);
    check_tach_reading(i2cdev, A_TACH3_LOW_BYTE, tach_reading);
}

static void adt7475_test_set_limit_registers(QI2CDevice *i2cdev)
{
    /* set temp limit registers */
    for (uint8_t i = 0; i < ADT7475_NUM_TEMPS; i++) {
        char temp_obj_name[15];
        int str_edit_ret = snprintf(temp_obj_name, 15, "temperature[%d]", i);
        g_assert_cmpint(str_edit_ret, ==, 14);

        i2c_set8(i2cdev, A_REMOTE_1_TEMP_LOW_LIMIT + (i * 2), 0);
        i2c_set8(i2cdev, A_REMOTE_1_TEMP_HIGH_LIMIT + (i * 2), 126);
        i2c_set8(i2cdev, A_REMOTE_1_THERM_TEMP_LIMIT + i, 126);
        qmp_adt7475_set(TEST_ID, temp_obj_name, 80000);
    }

    /* set tachometer limit registers */
    for (uint8_t i = 0; i < ADT7475_NUM_FANS; i++) {
        i2c_set8(i2cdev, A_TACH1_MIN_HIGH_BYTE + (i * 2), 0xFE);
        i2c_set8(i2cdev, A_TACH1_MIN_LOW_BYTE + (i * 2), 0xFE);
    }

    /* set pwm registers so tachometer values are within limits */
    for (uint8_t i = 0; i < ADT7475_NUM_PWM; i++) {
        i2c_set8(i2cdev, A_PWM1_CONFIGURATION + i,
                 (BHVR_MANUAL_MODE << R_PWMX_CONFIGURATION_BHVR_SHIFT));
        i2c_set8(i2cdev, ADT7475_PWM(i), 128);
    }

    /* set voltage limit registers */
    i2c_set8(i2cdev, A_VCC_HIGH_LIMIT, 0xFE);
    i2c_set8(i2cdev, A_VCC_LOW_LIMIT, 0);
    i2c_set8(i2cdev, A_VCCP_HIGH_LIMIT, 0xFE);
    i2c_set8(i2cdev, A_VCCP_LOW_LIMIT, 0);
    qmp_adt7475_set(TEST_ID, "vcc", 512);
    qmp_adt7475_set(TEST_ID, "vccp", 512);

    /* read interrupt regs to ensure they're not set */
    check_interrupts(i2cdev, 118, 0);
    check_interrupts(i2cdev, 0, 0);
}

static void test_fan_interrupts(void *obj, void *data, QGuestAllocator *alloc)
{
    QI2CDevice *i2cdev = (QI2CDevice *)obj;
    uint8_t pwm_percent;
    uint8_t tach_low_limit_low_byte;
    uint8_t tach_low_limit_high_byte;
    uint16_t tach;
    uint16_t tach_within_limit;
    uint16_t tach_low_limit;

    /* set limit registers so no interrupts are triggered */
    adt7475_test_set_limit_registers(i2cdev);

    pwm_percent = pwm_percent_to_adt7475_value(3276);
    tach = 819;
    tach_within_limit = tach - 2;
    tach_low_limit = tach - 1;
    tach_low_limit_low_byte = tach_low_limit & 0xFF;
    tach_low_limit_high_byte = (tach_low_limit >> 8) & 0xFF;

    for (int i = 0; i < ADT7475_NUM_PWM; i++) {
        char tach_obj_name[14];
        int str_edit_ret = snprintf(tach_obj_name, 14, "tachometer[%d]", i);
        g_assert_cmpint(str_edit_ret, ==, 13);

        i2c_set8(i2cdev, A_PWM1_CONFIGURATION + i,
                 (BHVR_MANUAL_MODE << R_PWMX_CONFIGURATION_BHVR_SHIFT));
        i2c_set8(i2cdev, A_TACH1_MIN_LOW_BYTE + (i * 2),
                 tach_low_limit_low_byte);
        i2c_set8(i2cdev, A_TACH1_MIN_HIGH_BYTE + (i * 2),
                 tach_low_limit_high_byte);
        i2c_set8(i2cdev, A_PWM1_CURRENT_DUTY_CYCLE + i, pwm_percent);
        check_interrupts(i2cdev, R_INTERRUPT_STATUS_1_OOL_MASK,
                         (R_INTERRUPT_STATUS_2_FAN1_MASK << i));

        /* ensure interrupt only cleared on read after error corrected */
        qmp_adt7475_set(TEST_ID, tach_obj_name, tach);
        check_interrupts(i2cdev, R_INTERRUPT_STATUS_1_OOL_MASK,
                         (R_INTERRUPT_STATUS_2_FAN1_MASK << i));
        qmp_adt7475_set(TEST_ID, tach_obj_name, tach_within_limit);
        check_interrupts(i2cdev, R_INTERRUPT_STATUS_1_OOL_MASK,
                         (R_INTERRUPT_STATUS_2_FAN1_MASK << i));
        check_interrupts(i2cdev, 0, 0);
    }
}

static void test_temperature_limit(QI2CDevice *i2cdev, char *temp_obj_name,
                                   int32_t temperature, int32_t safe_temperature,
                                   uint8_t interrupt_status_1,
                                   uint8_t interrupt_status_2,
                                   bool check_pwm_therm_event)
{
    /* test for out of temperature limit interrupt */
    qmp_adt7475_set(TEST_ID, temp_obj_name, temperature);
    check_interrupts(i2cdev, interrupt_status_1, interrupt_status_2);

    if (check_pwm_therm_event) {
        /* ensure fans set to max during THERM event */
        uint16_t tach_max = 270;
        for (uint8_t k = 0; k < ADT7475_NUM_PWM; k++) {
            check_tach_reading(i2cdev, ADT7475_TACH(k), tach_max);
        }
    }

    /* clear interrupt status 1 and 2 */
    qmp_adt7475_set(TEST_ID, temp_obj_name, safe_temperature);
    i2c_get8(i2cdev, A_INTERRUPT_STATUS_1);
    i2c_get8(i2cdev, A_INTERRUPT_STATUS_2);
    check_interrupts(i2cdev, 0, 0);
}

static void test_signed_temperature_interrupts(QI2CDevice *i2cdev)
{
    int32_t low_temperature = -127000;
    int32_t high_temperature = 125250;
    int32_t therm_temperature = 126750;
    int8_t low_limit = -127;
    int8_t high_limit = 125;
    int8_t therm_limit = 126;
    int32_t safe_temperature = (high_limit * 1000) - 250;
    uint8_t temp_sensor_interrupt_indicator;

    for (uint8_t i = 0; i < ADT7475_NUM_TEMPS; i++) {
        char temp_obj_name[15];
        int str_edit_ret = snprintf(temp_obj_name, 15, "temperature[%d]", i);
        g_assert_cmpint(str_edit_ret, ==, 14);

        i2c_set8(i2cdev, A_REMOTE_1_TEMP_LOW_LIMIT + (i * 2), low_limit);
        i2c_set8(i2cdev, A_REMOTE_1_TEMP_HIGH_LIMIT + (i * 2), high_limit);
        i2c_set8(i2cdev, A_REMOTE_1_THERM_TEMP_LIMIT + i, therm_limit);
        temp_sensor_interrupt_indicator = (R_INTERRUPT_STATUS_1_R1T_MASK << i);
        /* low limit met/exceeded */
        test_temperature_limit(i2cdev, temp_obj_name, low_temperature,
                               safe_temperature, temp_sensor_interrupt_indicator,
                               0, false);
        /* high limit exceeded */
        test_temperature_limit(i2cdev, temp_obj_name, high_temperature,
                               safe_temperature, temp_sensor_interrupt_indicator,
                               0, false);
        /* therm limit exceeded */
        test_temperature_limit(i2cdev, temp_obj_name, therm_temperature,
                               safe_temperature,
                               temp_sensor_interrupt_indicator
                               | R_INTERRUPT_STATUS_1_OOL_MASK,
                               R_INTERRUPT_STATUS_2_OVT_MASK, true);
    }
}

static void test_offset_64_temperature_interrupts(QI2CDevice *i2cdev)
{
    int32_t low_temperature = -63000;
    int32_t high_temperature = 181000;
    int32_t therm_temperature = 191000;
    uint8_t low_limit = 1;
    uint8_t high_limit = 244;
    uint8_t therm_limit = 254;
    int32_t safe_temperature = high_temperature - 1000;
    uint8_t temp_sensor_interrupt_indicator;

    i2c_set8(i2cdev, A_CONFIGURATION_5, ENABLE_OFFSET64);
    uint8_t config_5 = i2c_get8(i2cdev, A_CONFIGURATION_5);
    g_assert_cmpint(config_5, ==, ENABLE_OFFSET64);

    for (uint8_t i = 0; i < ADT7475_NUM_TEMPS; i++) {
        char temp_obj_name[15];
        int str_edit_ret = snprintf(temp_obj_name, 15, "temperature[%d]", i);
        g_assert_cmpint(str_edit_ret, ==, 14);

        i2c_set8(i2cdev, A_REMOTE_1_TEMP_LOW_LIMIT + (i * 2), low_limit);
        i2c_set8(i2cdev, A_REMOTE_1_TEMP_HIGH_LIMIT + (i * 2), high_limit);
        i2c_set8(i2cdev, A_REMOTE_1_THERM_TEMP_LIMIT + i, therm_limit);
        temp_sensor_interrupt_indicator = (R_INTERRUPT_STATUS_1_R1T_MASK << i);

        /* low limit met/exceeded */
        test_temperature_limit(i2cdev, temp_obj_name, low_temperature,
                               safe_temperature, temp_sensor_interrupt_indicator,
                               0, false);

        /* high limit exceeded */
        test_temperature_limit(i2cdev, temp_obj_name, high_temperature,
                               safe_temperature, temp_sensor_interrupt_indicator,
                               0, false);

        /* therm limit exceeded */
        test_temperature_limit(i2cdev, temp_obj_name, therm_temperature,
                               safe_temperature,
                               temp_sensor_interrupt_indicator
                               | R_INTERRUPT_STATUS_1_OOL_MASK,
                               R_INTERRUPT_STATUS_2_OVT_MASK, true);
    }
}

static void test_temperature_interrupts(void *obj, void *data, QGuestAllocator *alloc)
{
    QI2CDevice *i2cdev = (QI2CDevice *)obj;

    /* set limit registers so no interrupts are triggered */
    adt7475_test_set_limit_registers(i2cdev);

    /* Signed tests */
    test_signed_temperature_interrupts(i2cdev);

    /*
     * set temperatures and low limit so no interrupts triggered when
     * offset 64 limits set
     */
    for (uint8_t i = 0; i < ADT7475_NUM_TEMPS; i++) {
        char temp_obj_name[15];
        int str_edit_ret = snprintf(temp_obj_name, 15, "temperature[%d]", i);
        g_assert_cmpint(str_edit_ret, ==, 14);

        i2c_set8(i2cdev, A_REMOTE_1_TEMP_LOW_LIMIT + (i * 2), 0);
        qmp_adt7475_set(TEST_ID, temp_obj_name, 50000);
    }

    /* Offset 64 tests */
    test_offset_64_temperature_interrupts(i2cdev);
}

static void adt7475_register_nodes(void)
{
    QOSGraphEdgeOptions opts = {
        .extra_device_opts = "id=" TEST_ID ",address=0x2e"
    };
    add_qi2c_address(&opts, &(QI2CAddress) { 0x2e });

    qos_node_create_driver(TYPE_ADT7475, i2c_device_create);
    qos_node_consumes(TYPE_ADT7475, "i2c-bus", &opts);

    qos_add_test("test_read_write", TYPE_ADT7475, test_read_write, NULL);
    qos_add_test("test_fan_speed", TYPE_ADT7475, test_fan_speed, NULL);
    qos_add_test("test_fan_interrupts", TYPE_ADT7475, test_fan_interrupts, NULL);
    qos_add_test("test_temperature_interrupts", TYPE_ADT7475,
                 test_temperature_interrupts, NULL);
}
libqos_init(adt7475_register_nodes);

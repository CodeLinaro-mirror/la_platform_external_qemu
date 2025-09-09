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
#include "hw/i2c/smbus_slave.h"
#include "hw/core/qdev-properties.h"
#include "hw/sensor/adt7475.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qom/object.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "trace.h"

#define ADT7475_100_PERCENT_INT 10000
#define TACH4_OFFSET            3
#define TACH3_OFFSET            2

static bool is_signed_mode(ADT7475State *as)
{
    return as->regs[A_CONFIGURATION_5] & R_CONFIGURATION_5_TWOS_COMPL_MASK;
}

static bool is_therm_limit_enabled_by_reg(ADT7475State *as, uint8_t therm_limit_reg)
{
    return !((is_signed_mode(as) &&
        (as->regs[therm_limit_reg] ==
        (uint8_t)ADT7475_THERM_LIMIT_DISABLED_SIGNED))
        || (!is_signed_mode(as) &&
        (as->regs[therm_limit_reg] ==
        (uint8_t)ADT7475_THERM_LIMIT_DISABLED_OFFSET_64)));
}

static bool is_therm_limit_enabled(ADT7475State *as, uint8_t temperature_index)
{
    switch (temperature_index) {
    case ADT7475_REMOTE_1_OVT_INDEX:
        return (!(as->regs[A_CONFIGURATION_5] & R_CONFIGURATION_5_R1_THERM_MASK) ||
            is_therm_limit_enabled_by_reg(as, A_REMOTE_1_THERM_TEMP_LIMIT));
    case ADT7475_LOCAL_OVT_INDEX:
        return (!(as->regs[A_CONFIGURATION_5] & R_CONFIGURATION_5_LOCAL_THERM_MASK) ||
            is_therm_limit_enabled_by_reg(as, A_LOCAL_THERM_TEMP_LIMIT));
    case ADT7475_REMOTE_2_OVT_INDEX:
        return (!(as->regs[A_CONFIGURATION_5] & R_CONFIGURATION_5_R2_THERM_MASK) ||
            is_therm_limit_enabled_by_reg(as, A_REMOTE_2_THERM_TEMP_LIMIT));
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
            "%s: %s: command: 0x%02x: therm limit enabled check for invalid temp reg %d\n",
            __func__, DEVICE(as)->canonical_path, as->command, temperature_index);
        return false;
    }
}

/*
 * Determine if the provided PWM current duty cycle is inverted.
 */
static bool is_pwm_inverted(ADT7475State *as, uint8_t pwm_current_duty_cycle_reg)
{
    switch (pwm_current_duty_cycle_reg) {
    case A_PWM1_CURRENT_DUTY_CYCLE:
        return as->regs[A_PWM1_CONFIGURATION] & R_PWMX_CONFIGURATION_INV_MASK;
    case A_PWM2_CURRENT_DUTY_CYCLE:
        return as->regs[A_PWM2_CONFIGURATION] & R_PWMX_CONFIGURATION_INV_MASK;
    case A_PWM3_CURRENT_DUTY_CYCLE:
        return as->regs[A_PWM3_CONFIGURATION] & R_PWMX_CONFIGURATION_INV_MASK;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
            "%s: %s: command: 0x%02x: inverted check for invalid pwm current duty cycle %d\n",
            __func__, DEVICE(as)->canonical_path, as->command,
            pwm_current_duty_cycle_reg);
        return false;
    }
}

/*
 * Returns the two byte tachometer value from the low and high bytes of the
 * specified tachometer register.
 */
static uint16_t get_tach_reading(ADT7475State *as, uint8_t tach_reg)
{
    uint8_t low_byte = as->regs[tach_reg];
    uint8_t high_byte = as->regs[tach_reg + 1];
    return (high_byte << 8) | low_byte;
}

/*
 * Determins if the PWM corresponding to the provided TACH/PWM offset is enabled.
 * Offset can be based on either PWM registers or TACH registers.
 */
static bool is_pwm_enabled(ADT7475State *as, uint8_t offset)
{
    if (offset > TACH4_OFFSET) {
        qemu_log_mask(LOG_GUEST_ERROR,
            "%s: %s: command: 0x%02x: PWM config offset is %d > 3\n",
            __func__, DEVICE(as)->canonical_path, as->command, offset);
        return false;
    }
    /* both TACH4 and TACH3 are controlled by PWM3 */
    if (offset == TACH4_OFFSET) {
        offset = TACH3_OFFSET;
    }
    return as->regs[A_PWM1_CONFIGURATION + offset]
        & (BHVR_DISABLED << R_PWMX_CONFIGURATION_BHVR_SHIFT);
}

/*
 * Calculate the tachometer reading from the ADT7475's binary representation of
 * PWM duty cycle percentage.
 *
 * See the "Calculating Fan Speed" section on page 25 of the datasheet for the
 * equation.
 */
static uint16_t calculate_tach_reading(ADT7475State *as,
                                       uint8_t pwm_duty_cycle, bool inverted_pwm)
{
    if (inverted_pwm) {
        pwm_duty_cycle = ADT7475_PWM_MAX - pwm_duty_cycle;
    }

    if (pwm_duty_cycle == 0) {
        return ADT7475_FAN_TACH_STALL;
    }

    uint16_t rpm = as->fan_speed_rating * pwm_duty_cycle / ADT7475_PWM_MAX;
    if (rpm < as->fan_speed_min) {
        return ADT7475_FAN_TACH_STALL;
    }

    uint32_t tach_reading = (ADT7475_CLOCK_FREQ * 60) / rpm;
    if (tach_reading > ADT7475_FAN_TACH_STALL) {
        return ADT7475_FAN_TACH_STALL;
    }
    return (uint16_t)tach_reading;
}

static void set_tach_reading(ADT7475State *as, uint8_t tach_reg,
                                uint8_t pwm_current_duty_cycle, bool inverted_pwm)
{
    uint16_t fan_tach_reading = calculate_tach_reading(as,
                                                       pwm_current_duty_cycle,
                                                       inverted_pwm);
    as->regs[tach_reg] = fan_tach_reading & 0xFF;
    as->regs[tach_reg + 1] = (fan_tach_reading >> 8) & 0xFF;
}

/*
 * Update the TACH readings associated with the provided PWM current duty cycle
 * register.
 *
 * Assumes the PWM current duty cycle register is enabled.
 */
static void update_tach_reading_for_pwm(ADT7475State *as,
                                        uint8_t pwm_current_duty_cycle_reg)
{
    bool inverted_pwm = is_pwm_inverted(as, pwm_current_duty_cycle_reg);
    switch (pwm_current_duty_cycle_reg) {
    case A_PWM1_CURRENT_DUTY_CYCLE:
        set_tach_reading(as, A_TACH1_LOW_BYTE, as->regs[A_PWM1_CURRENT_DUTY_CYCLE],
                        inverted_pwm);
        break;
    case A_PWM2_CURRENT_DUTY_CYCLE:
        set_tach_reading(as, A_TACH2_LOW_BYTE, as->regs[A_PWM2_CURRENT_DUTY_CYCLE],
                        inverted_pwm);
        break;
    case A_PWM3_CURRENT_DUTY_CYCLE:
        set_tach_reading(as, A_TACH3_LOW_BYTE, as->regs[A_PWM3_CURRENT_DUTY_CYCLE],
                        inverted_pwm);
        /* PWM3 controls both TACH3 and TACH4 */
        set_tach_reading(as, A_TACH4_LOW_BYTE, as->regs[A_PWM3_CURRENT_DUTY_CYCLE],
                        inverted_pwm);
        /*
         * SYNC bit of acoustics register determines if PWM3 additionally
         * controls TACH2, see table 30 in the datasheet.
         */
        if (as->regs[A_ENHANCED_ACOUSTICS_1]
            & R_ENHANCED_ACOUSTICS_1_SYNC_MASK) {
            set_tach_reading(as, A_TACH2_LOW_BYTE,
                             as->regs[A_PWM3_CURRENT_DUTY_CYCLE], inverted_pwm);
        }
        break;
    default:
         qemu_log_mask(LOG_GUEST_ERROR,
             "%s: %s: command 0x%02x: updating tach reading for unsupported PWM reg 0x%02x\n",
             __func__, DEVICE(as)->canonical_path,
             as->command, pwm_current_duty_cycle_reg);
        break;
    }

}

static void update_pwm_current_duty_cycle(ADT7475State *as,
                                          uint8_t pwm_current_duty_cycle_reg,
                                          uint8_t pwm_current_duty_cycle)
{
    uint8_t pwm_reg_offset = pwm_current_duty_cycle_reg - A_PWM1_CURRENT_DUTY_CYCLE;
    if (is_pwm_enabled(as, pwm_reg_offset)) {
        as->regs[pwm_current_duty_cycle_reg] = pwm_current_duty_cycle;
        update_tach_reading_for_pwm(as, pwm_current_duty_cycle_reg);
    }
}

/*
 * Sets all fan speeds to max.
 */
static void all_pwm_max(ADT7475State *as)
{
    for (uint8_t i = 0; i < ADT7475_NUM_FANS; i++) {
        set_tach_reading(as, ADT7475_TACH(i), ADT7475_PWM_MAX, false);
    }
}

/*
 * Reset all fan speeds with respect to the PWM current duty cycle registers.
 */
static void reset_all_tach_reading(ADT7475State *as)
{
    for (uint8_t i = 0; i < ADT7475_NUM_PWM; i++) {
        if (is_pwm_enabled(as, i)) {
            update_tach_reading_for_pwm(as, ADT7475_PWM(i));
        } else {
            set_tach_reading(as, ADT7475_TACH(i), ADT7475_PWM_OFF, false);
            if (ADT7475_PWM(i) == A_PWM3_CURRENT_DUTY_CYCLE) {
                set_tach_reading(as, A_TACH4_LOW_BYTE, ADT7475_PWM_MAX, false);
            }
        }
    }
}

/*
 * Determine if temperature > temperature limit. This bound is exclusive, as
 * described in the datasheet on page 17.
 */
static bool high_temperature_limit_compare(ADT7475State *as, int16_t temperature,
                                           int16_t temperature_limit,
                                           uint8_t extended_resolution)
{
    /* extended resolution adds to temperature, even if negative */
    return (temperature == temperature_limit && extended_resolution != 0) ||
        (temperature > temperature_limit);
}

/* Check if an overtemperature event is occuring, and update all fan speeds
 * accordingly.
 *
 * If an overtemperature event occurs, all fan speeds are set to max.
 * Once an overtemperature event ends, all fan speeds are updated with respect
 * to their corresponding PWM current duty cycle registers, and all fans with
 * PWM disabled are turned back off.
 */
static bool check_overtemperature_event(ADT7475State *as)
{
    bool previous_overtemperature_occured = false;

    for (uint8_t i = 0; i < ADT7475_NUM_TEMPS; i++) {
        if (is_therm_limit_enabled(as, i)) {
            int16_t temperature = is_signed_mode(as)
                ? (int8_t)as->regs[ADT7475_TEMP(i)]
                : (uint8_t)as->regs[ADT7475_TEMP(i)];
            int16_t therm_limit = is_signed_mode(as)
                ? (int8_t)as->regs[ADT7475_THERM_LIMIT(i)]
                : (uint8_t)as->regs[ADT7475_THERM_LIMIT(i)];
            uint8_t extended_resolution =
                extract8(as->regs[A_EXTENDED_RESOLUTION_2],
                         R_EXTENDED_RESOLUTION_2_TDM1_SHIFT + (i * 2), 2);

            previous_overtemperature_occured |= as->overtemperature_occured[i];
            as->overtemperature_occured[i] =
                high_temperature_limit_compare(as, temperature, therm_limit,
                                               extended_resolution);
        } else {
            as->overtemperature_occured[i] = false;
        }
    }

    bool overtemperature_occured =
        as->overtemperature_occured[ADT7475_REMOTE_1_OVT_INDEX] ||
        as->overtemperature_occured[ADT7475_LOCAL_OVT_INDEX] ||
        as->overtemperature_occured[ADT7475_REMOTE_2_OVT_INDEX];

    if (overtemperature_occured) {
        all_pwm_max(as);
    } else if (previous_overtemperature_occured) {
        reset_all_tach_reading(as);
    }
    return overtemperature_occured;
}

static uint8_t temperature_limit_check(ADT7475State *as)
{
    uint8_t temperature_interrupts = 0;

    /* Bits 4:6 of interrupt status 1 are for temperature limit interrupts. */
    for (int i = 0; i < 3; i++) {
        uint8_t limit_regs = A_REMOTE_1_TEMP_LOW_LIMIT + (i * 2);
        int16_t temperature = is_signed_mode(as) ?
            (int8_t)as->regs[ADT7475_TEMP(i)] : as->regs[ADT7475_TEMP(i)];
        int16_t low_limit = is_signed_mode(as) ?
            (int8_t)as->regs[limit_regs] : as->regs[limit_regs];
        int16_t high_limit = is_signed_mode(as) ?
            (int8_t)as->regs[limit_regs + 1] : as->regs[limit_regs + 1];
        uint8_t extended_resolution =
            extract8(as->regs[A_EXTENDED_RESOLUTION_2],
                     R_EXTENDED_RESOLUTION_2_TDM1_SHIFT + (i * 2), 2);

        if ((temperature == low_limit && extended_resolution == 0)
            || temperature < low_limit
            || high_temperature_limit_compare(as, temperature, high_limit,
                                              extended_resolution)) {
            temperature_interrupts |= (R_INTERRUPT_STATUS_1_R1T_MASK << i);
        }
    }

    return temperature_interrupts;
}


/*
 * Updates both interrupt status register 1 and 2.
 */
static void adt7475_update_status(ADT7475State *as)
{
    uint8_t vccp = as->regs[A_VCCP_READING];
    uint8_t vcc = as->regs[A_VCC_READING];

    /* Update interrupt status register 2 */
    /* Bit 1 is for THERM overtemperature limit interrupts. */
    if (check_overtemperature_event(as)) {
        as->regs[A_INTERRUPT_STATUS_2] |= R_INTERRUPT_STATUS_2_OVT_MASK;
    }

    /* Bits 2:5 are for tachometer limit interrupts. */
    for (int i = 0; i < ADT7475_NUM_FANS; i++) {
        uint16_t tach_reading =
            get_tach_reading(as, ADT7475_TACH(i));
        uint16_t tach_min_limit =
            get_tach_reading(as, ADT7475_TACH_MIN(i));
        bool pwm_enabled = is_pwm_enabled(as, i);
        /* recall that the larger the tach value, the slower the fan is running */
        if (pwm_enabled &&
            (tach_reading > tach_min_limit ||
            tach_reading == ADT7475_FAN_TACH_STALL)) {
            as->regs[A_INTERRUPT_STATUS_2] |= (1 << (2 + i));
        }
    }

    /* Update interrupt status 1 */
    /* Bits 1:2 are for voltage limit interrupts. */
    if (vccp <= as->regs[A_VCCP_LOW_LIMIT] ||
        vccp > as->regs[A_VCCP_HIGH_LIMIT]) {
        as->regs[A_INTERRUPT_STATUS_1] |= R_INTERRUPT_STATUS_1_VCCP_MASK;
    }
    if (vcc <= as->regs[A_VCC_LOW_LIMIT] ||
        vcc > as->regs[A_VCC_HIGH_LIMIT]) {
        as->regs[A_INTERRUPT_STATUS_1] |= R_INTERRUPT_STATUS_1_VCC_MASK;
    }

    as->regs[A_INTERRUPT_STATUS_1] |= temperature_limit_check(as);

    if (as->regs[A_INTERRUPT_STATUS_2] != 0) {
        as->regs[A_INTERRUPT_STATUS_1] |= R_INTERRUPT_STATUS_1_OOL_MASK;
    } else {
        as->regs[A_INTERRUPT_STATUS_1] &= ~R_INTERRUPT_STATUS_1_OOL_MASK;
    }
}

static void update_pwm_config(ADT7475State *as,
                              uint8_t pwm_current_duty_cycle_reg, uint8_t config)
{
    bool inverted_pwm = config & R_PWMX_CONFIGURATION_INV_MASK;
    uint8_t pwm_bhvr_config = (config & R_PWMX_CONFIGURATION_BHVR_MASK) >>
        R_PWMX_CONFIGURATION_BHVR_SHIFT;
    uint8_t pwm_duty_cycle;

    as->regs[as->command] = config;
    switch (pwm_bhvr_config) {
    case (BHVR_FULL_SPEED):
        pwm_duty_cycle = 0xFF;
        if (inverted_pwm) {
            pwm_duty_cycle = 0;
        }
        break;
    case (BHVR_DISABLED):
        pwm_duty_cycle = 0;
        if (inverted_pwm) {
            pwm_duty_cycle = 0xFF;
        }
        break;
    case (BHVR_MANUAL_MODE):
    case (BHVR_REMOTE_1_AUTO) ... (BHVR_REMOTE_2_AUTO):
        return;
    case (BHVR_FASTEST_LOCAL_2) ... (BHVR_FASTEST_TEMP):
        qemu_log_mask(LOG_TRACE,
            "%s: %s: attempting to update PWM configuration 0x%02x to an "
            "unsupported value automatic mode 0x%02x\n",
            __func__, DEVICE(as)->canonical_path,
            as->command, config);
        return;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
            "%s: %s: attempting to update PWM configuration 0x%02x to an "
            "unknown value 0x%02x\n",
            __func__, DEVICE(as)->canonical_path,
            as->command, config);
        return;
    }
    update_pwm_current_duty_cycle(as, pwm_current_duty_cycle_reg,
                                  pwm_duty_cycle);
}

static bool is_pwm_manual_mode(ADT7475State *as, uint8_t pwm_config_offset)
{
    return as->regs[A_PWM1_CONFIGURATION + pwm_config_offset] &
        (BHVR_MANUAL_MODE << R_PWMX_CONFIGURATION_BHVR_SHIFT);
}

static void adt7475_qmp_get_temp(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    ADT7475State *as = ADT7475(obj);
    uint8_t *temperature_reg = (uint8_t *)opaque;
    int32_t value;
    uint8_t extended_temp;
    uint8_t extended_shift;

    if (strncmp(name, "temperature[0]", 14) == 0) {
        extended_shift = R_EXTENDED_RESOLUTION_2_TDM1_SHIFT;
    } else if (strncmp(name, "temperature[1]", 14) == 0) {
        extended_shift = R_EXTENDED_RESOLUTION_2_LTMP_SHIFT;
    } else if (strncmp(name, "temperature[2]", 14) == 0) {
        extended_shift = R_EXTENDED_RESOLUTION_2_TDM2_SHIFT;
    } else {
        error_setg(errp,
            "attempting to get temperature reading for unsupported register 0x%02x",
            as->command);
        return;
    }
    extended_temp = extract8(as->regs[A_EXTENDED_RESOLUTION_2], extended_shift, 2);

    value = (int8_t)*temperature_reg;
    if (!is_signed_mode(as)) {
        value = (*temperature_reg) - 64;
    }

    value = (value * 1000) + (extended_temp * 250);
    visit_type_int32(v, name, &value, errp);
}

static void millidegrees_to_adt7475(int32_t temp_millidegrees,
                                    int32_t degree_low_bound, uint8_t *temp,
                                    uint8_t *extended_temp)
{
    int32_t upper_temp = temp_millidegrees / 1000;
    *extended_temp = (temp_millidegrees % 1000) / 250;
    *extended_temp &= 3;

    /* extended resolution is positively additive, must offset negative numbers */
    if (temp_millidegrees < 0 && upper_temp > degree_low_bound && *extended_temp > 0) {
        upper_temp -= 1;
    }

    *temp = upper_temp;
}

/*
 * Temperature in millidegrees ranging from -128000 to 191250 with 250 millidegree
 * granularity.
 * Millidegree granularity is applied additively -- that is, the temperature is
 * stored arithmatically as `temp` + `granularity`.
 */
static void adt7475_qmp_set_temp(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    ADT7475State *as = ADT7475(obj);
    uint8_t *temp_reg = (uint8_t *)opaque;
    int32_t value;
    uint8_t extended_temp;

    if (!visit_type_int32(v, name, &value, errp)) {
        return;
    }

    if (is_signed_mode(as)) {
        if (value > ADT7475_MAX_MILLIDEGREE) {
            qemu_log_mask(LOG_GUEST_ERROR,
                "%s: %s: attempting to set temperature to %d > max temperature %d 0x%02x\n",
                __func__, DEVICE(as)->canonical_path, as->command, value,
                ADT7475_MAX_MILLIDEGREE);
            value = ADT7475_MAX_MILLIDEGREE;
        }
        if (value < ADT7475_MIN_MILLIDEGREE) {
            qemu_log_mask(LOG_GUEST_ERROR,
                "%s: %s: attempting to set temperature to %d < min temperature %d 0x%02x\n",
                __func__, DEVICE(as)->canonical_path, as->command, value,
                ADT7475_MAX_MILLIDEGREE);
            value = ADT7475_MIN_MILLIDEGREE;
        }
        millidegrees_to_adt7475(value, ADT7475_DEGREE_LOW_BOUND,
                                temp_reg, &extended_temp);
    } else {
        if (value > ADT7475_MAX_MILLIDEGREE_OFFSET64) {
            qemu_log_mask(LOG_GUEST_ERROR,
                "%s: %s: attempting to set temperature to %d > max temperature %d 0x%02x\n",
                __func__, DEVICE(as)->canonical_path, as->command, value,
                ADT7475_MAX_MILLIDEGREE_OFFSET64);
            value = ADT7475_MAX_MILLIDEGREE_OFFSET64;
        }
        if (value < ADT7475_MIN_MILLIDEGREE_OFFSET64) {
            qemu_log_mask(LOG_GUEST_ERROR,
                "%s: %s: attempting to set temperature to %d < min temperature %d 0x%02x\n",
                __func__, DEVICE(as)->canonical_path, as->command, value,
                ADT7475_MAX_MILLIDEGREE_OFFSET64);
            value = ADT7475_MIN_MILLIDEGREE_OFFSET64;
        }
        millidegrees_to_adt7475(value, ADT7475_DEGREE_LOW_BOUND_OFFSET64,
                                temp_reg, &extended_temp);
        /* adjust for offset 64 */
        *temp_reg += 64;
    }

    /*
     * offset into extended resolution register is based on which temperature
     * register is being written to
     */

    uint8_t old_extended_temp = as->regs[A_EXTENDED_RESOLUTION_2];
    if (strncmp(name, "temperature[0]", 14) == 0) {
        as->regs[A_EXTENDED_RESOLUTION_2] =
            (old_extended_temp & ~R_EXTENDED_RESOLUTION_2_TDM1_MASK)
            | (extended_temp << R_EXTENDED_RESOLUTION_2_TDM1_SHIFT);
    } else if (strncmp(name, "temperature[1]", 14) == 0) {
        as->regs[A_EXTENDED_RESOLUTION_2] =
            (old_extended_temp & ~R_EXTENDED_RESOLUTION_2_LTMP_MASK)
            | (extended_temp << R_EXTENDED_RESOLUTION_2_LTMP_SHIFT);
    } else if (strncmp(name, "temperature[2]", 14) == 0) {
        as->regs[A_EXTENDED_RESOLUTION_2] =
            (old_extended_temp & ~R_EXTENDED_RESOLUTION_2_TDM2_MASK)
            | (extended_temp << R_EXTENDED_RESOLUTION_2_TDM2_SHIFT);
    } else {
        error_setg(errp,
            "attempting to set temperature reading for unsupported register 0x%02x",
            as->command);
        return;
    }

    adt7475_update_status(as);
}

/*
 * Reads the 16-bit tachometer reading of the specified fan.
 */
static void adt7475_qmp_get_tach_reading(Object *obj, Visitor *v,
                                    const char *name, void *opaque, Error **errp)
{
    uint8_t *reg = (uint8_t *)opaque;
    uint8_t low_byte = *reg;
    uint8_t high_byte = *(reg + 1);
    uint16_t value = (high_byte << 8) | low_byte;
    visit_type_uint16(v, name, &value, errp);
}

/*
 * Sets the 16-bit tachometer reading of the specified fan.
 */
static void adt7475_qmp_set_tach_reading(Object *obj, Visitor *v,
                                    const char *name, void *opaque, Error **errp)
{
    uint8_t *reg = (uint8_t *)opaque;
    uint16_t value;
    if (!visit_type_uint16(v, name, &value, errp)) {
        return;
    }

    uint8_t low_byte = value & 0xFF;
    uint8_t high_byte = (value >> 8) & 0xFF;
    *reg = low_byte;
    *(reg + 1) = high_byte;

    adt7475_update_status(ADT7475(obj));
}

static void adt7475_qmp_get_voltage(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    ADT7475State *as = ADT7475(obj);
    uint8_t *reg = (uint8_t *)opaque;
    uint16_t value;
    uint8_t extended_resolution;
    bool is_vcc = (strncmp(name, "vcc", 3) == 0);
    bool is_vccp = (strncmp(name, "vccp", 4) == 0);

    if (is_vcc && !is_vccp) {
        extended_resolution = as->regs[A_EXTENDED_RESOLUTION_1]
            >> R_EXTENDED_RESOLUTION_1_VCC_SHIFT & 3;
    } else if (is_vccp) {
        extended_resolution = as->regs[A_EXTENDED_RESOLUTION_1]
            >> R_EXTENDED_RESOLUTION_1_VCCP_SHIFT & 3;
    } else {
        error_setg(errp,
            "attempting to get a voltage reading for an unsupported register 0x%02x",
            as->command);
        return;
    }

    value = (*reg << 2) | extended_resolution;

    visit_type_uint16(v, name, &value, errp);
}

static void adt7475_qmp_set_voltage(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    ADT7475State *as = ADT7475(obj);
    uint8_t *reg = (uint8_t *)opaque;
    uint16_t value;
    uint8_t extended_resolution = as->regs[A_EXTENDED_RESOLUTION_1];
    bool is_vcc = (strncmp(name, "vcc", 3) == 0);
    bool is_vccp = (strncmp(name, "vccp", 4) == 0);

    if (!visit_type_uint16(v, name, &value, errp)) {
        return;
    }

    if (value > ADT7475_VOLTAGE_MAX) {
        qemu_log_mask(LOG_GUEST_ERROR,
            "%s: %s: attempting to set voltage to %d > max voltage %d 0x%02x\n",
            __func__, DEVICE(as)->canonical_path, as->command, value,
            ADT7475_VOLTAGE_MAX);
        value = ADT7475_VOLTAGE_MAX;
    }

    if (is_vcc && !is_vccp) {
        extended_resolution =
            (extended_resolution & ~R_EXTENDED_RESOLUTION_1_VCC_MASK) |
            (value & 3) << R_EXTENDED_RESOLUTION_1_VCC_SHIFT;
    } else if (is_vccp) {
        extended_resolution =
            (extended_resolution & ~R_EXTENDED_RESOLUTION_1_VCCP_MASK) |
            (value & 3) << R_EXTENDED_RESOLUTION_1_VCCP_SHIFT;
    } else {
        error_setg(errp,
            "attempting to set a voltage reading for an unsupported register 0x%02x",
            as->command);
        return;
    }

    as->regs[A_EXTENDED_RESOLUTION_1] = extended_resolution;
    *reg = (value >> 2) & 0xFF;

    adt7475_update_status(as);
}

/*
 * Reads the specified PWM Current Duty Cycle.
 */
static void adt7475_qmp_get_pwm(Object *obj, Visitor *v,
                                const char *name, void *opaque, Error **errp)
{
    uint8_t *reg = (uint8_t *)opaque;
    uint8_t value = *reg;
    visit_type_uint8(v, name, &value, errp);
}

/*
 * Sets the specified PWM Current Duty Cycle.
 */
static void adt7475_qmp_set_pwm(Object *obj, Visitor *v,
                                const char *name, void *opaque, Error **errp)
{
    ADT7475State *as = ADT7475(obj);
    uint8_t value;

    if (!visit_type_uint8(v, name, &value, errp)) {
        return;
    }

    /* need to know which pwm register is being written to */
    uint8_t pwm_offset;
    if (strncmp(name, "pwm[0]", 6) == 0) {
        pwm_offset = 0;
    } else if (strncmp(name, "pwm[1]", 6) == 0) {
        pwm_offset = 1;
    } else if (strncmp(name, "pwm[2]", 6) == 0) {
        pwm_offset = 2;
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
            "%s: %s: writing to invalid PWM current duty cycle reg with qom name %s\n",
            __func__, DEVICE(as)->canonical_path, name);
        return;
    }

    if (!is_pwm_manual_mode(as, pwm_offset)) {
        qemu_log_mask(LOG_GUEST_ERROR,
            "%s: %s: writing to disabled PWM current duty cycle reg 0x%02x\n",
            __func__, DEVICE(as)->canonical_path, as->command);
    }

    update_pwm_current_duty_cycle(as, ADT7475_PWM(pwm_offset), value);
    adt7475_update_status(ADT7475(obj));
}

static uint8_t adt7475_receive(SMBusDevice *smd)
{
    ADT7475State *as = ADT7475(smd);
    uint8_t data;

    switch (as->command) {
    case A_INTERRUPT_STATUS_1 ... A_INTERRUPT_STATUS_2:
        data = as->regs[as->command];
        as->regs[as->command] = 0;
        adt7475_update_status(as);
        break;
    case A_CONFIGURATION_6 ... A_CONFIGURATION_7:
    case A_VCCP_READING ... A_VCC_READING:
    case A_REMOTE_1_TEMP ... A_PWM3_CURRENT_DUTY_CYCLE:
    case A_DEVICE_ID ... A_COMPANY_ID:
    case A_CONFIGURATION_1:
    case A_VCCP_LOW_LIMIT ... A_VCC_HIGH_LIMIT:
    case A_REMOTE_1_TEMP_LOW_LIMIT ... A_TEST_2:
        data = as->regs[as->command];
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
            "%s: %s: reading from unsupported register 0x%02x\n",
            __func__, DEVICE(as)->canonical_path, as->command);
        data = 0xFF;
        break;
    }

    trace_adt7475_receive(DEVICE(as)->canonical_path,
                          as->command, data);
    return data;
}

static int adt7475_write(SMBusDevice *smd, uint8_t *buf, uint8_t len)
{
    ADT7475State *as = ADT7475(smd);

    as->command = buf[0];

    if (len == 1) { /* only the register offset was sent */
        return 0;
    }

    uint8_t data = buf[1];

    if (len > 2) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: received large write %d bytes",
            DEVICE(as)->canonical_path, __func__, len);
        return -1;
    }

    trace_adt7475_write(DEVICE(as)->canonical_path,
                        as->command, data);

    uint8_t pwm_offset;
    switch (as->command) {
    case A_PWM1_CURRENT_DUTY_CYCLE ... A_PWM3_CURRENT_DUTY_CYCLE:
        pwm_offset = as->command - A_PWM1_CURRENT_DUTY_CYCLE;
        if (pwm_offset > (A_PWM3_CONFIGURATION - A_PWM1_CONFIGURATION)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                "%s: %s: writing to unsupported PWM register 0x%02x\n",
                __func__, DEVICE(as)->canonical_path, as->command);
            break;
        }
        if (!is_pwm_manual_mode(as, pwm_offset)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                "%s: %s: writing to disabled PWM current duty cycle reg 0x%02x\n",
                __func__, DEVICE(as)->canonical_path, as->command);
            break;
        }
        update_pwm_current_duty_cycle(as, as->command, data);
        break;
    case A_PWM1_CONFIGURATION ... A_PWM3_CONFIGURATION:
        pwm_offset = as->command - A_PWM1_CONFIGURATION;
        update_pwm_config(as, A_PWM1_CURRENT_DUTY_CYCLE + pwm_offset, data);
        break;
    case A_CONFIGURATION_6:
    case A_PWM1_MAX_DUTY_CYCLE ... A_PWM3_MAX_DUTY_CYCLE:
    case A_CONFIGURATION_1:
    case A_VCCP_LOW_LIMIT ... A_TACH4_MIN_HIGH_BYTE:
    case A_REMOTE_1_TRANGE_PWM1_FREQ ... A_CONFIGURATION_3:
    case A_THERM_TIMER_LIMIT ... A_CONFIGURATION_4:
        as->regs[as->command] = data;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
            "%s: %s: writing to unsupported register 0x%02x\n",
            __func__, DEVICE(as)->canonical_path, as->command);
        break;
    }

    adt7475_update_status(as);
    return 0;
}

static void adt7475_exit_reset(Object *obj, ResetType type)
{
    ADT7475State *as = ADT7475(obj);

    /* reset internal state */
    as->fan_speed_rating = ADT7475_RPM_MAX_DEFAULT;
    as->fan_speed_min = ADT7475_RPM_MIN_DEFAULT;
    memset(as->overtemperature_occured, 0, sizeof as->overtemperature_occured);

    /* reset registers */
    memset(as->regs, 0, sizeof as->regs);

    as->regs[A_CONFIGURATION_1] = CONFIGURATION_1_DEFAULT;
    as->regs[A_CONFIGURATION_2] = CONFIGURATION_2_DEFAULT;
    as->regs[A_CONFIGURATION_3] = CONFIGURATION_3_DEFAULT;
    as->regs[A_CONFIGURATION_4] = CONFIGURATION_4_DEFAULT;
    as->regs[A_CONFIGURATION_5] = CONFIGURATION_5_DEFAULT;
    as->regs[A_CONFIGURATION_6] = CONFIGURATION_6_DEFAULT;
    as->regs[A_CONFIGURATION_7] = CONFIGURATION_7_DEFAULT;
    as->regs[A_REMOTE_1_TEMP] = TEMP_DEFAULT;
    as->regs[A_LOCAL_TEMP] = TEMP_DEFAULT;
    as->regs[A_REMOTE_2_TEMP] = TEMP_DEFAULT;
    as->regs[A_PWM1_MAX_DUTY_CYCLE] = PWM_MAX_DUTY_CYCLE_DEFAULT;
    as->regs[A_PWM2_MAX_DUTY_CYCLE] = PWM_MAX_DUTY_CYCLE_DEFAULT;
    as->regs[A_PWM3_MAX_DUTY_CYCLE] = PWM_MAX_DUTY_CYCLE_DEFAULT;
    as->regs[A_VCCP_HIGH_LIMIT] = VOLTAGE_HIGH_LIMIT_DEFAULT;
    as->regs[A_VCC_HIGH_LIMIT] = VOLTAGE_HIGH_LIMIT_DEFAULT;
    as->regs[A_REMOTE_1_TEMP_LOW_LIMIT] = TEMP_LOW_LIMIT_DEFAULT;
    as->regs[A_REMOTE_1_TEMP_HIGH_LIMIT] = TEMP_HIGH_LIMIT_DEFAULT;
    as->regs[A_LOCAL_TEMP_LOW_LIMIT] = TEMP_LOW_LIMIT_DEFAULT;
    as->regs[A_LOCAL_TEMP_HIGH_LIMIT] = TEMP_HIGH_LIMIT_DEFAULT;
    as->regs[A_REMOTE_2_TEMP_LOW_LIMIT] = TEMP_LOW_LIMIT_DEFAULT;
    as->regs[A_REMOTE_2_TEMP_HIGH_LIMIT] = TEMP_HIGH_LIMIT_DEFAULT;
    as->regs[A_TACH1_MIN_LOW_BYTE] = TACH_MIN_BYTE;
    as->regs[A_TACH1_MIN_HIGH_BYTE] = TACH_MIN_BYTE;
    as->regs[A_TACH2_MIN_LOW_BYTE] = TACH_MIN_BYTE;
    as->regs[A_TACH2_MIN_HIGH_BYTE] = TACH_MIN_BYTE;
    as->regs[A_TACH3_MIN_LOW_BYTE] = TACH_MIN_BYTE;
    as->regs[A_TACH3_MIN_HIGH_BYTE] = TACH_MIN_BYTE;
    as->regs[A_TACH4_MIN_LOW_BYTE] = TACH_MIN_BYTE;
    as->regs[A_TACH4_MIN_HIGH_BYTE] = TACH_MIN_BYTE;
    as->regs[A_PWM1_CONFIGURATION] = PWM_CONFIGURATION_DEFAULT;
    as->regs[A_PWM2_CONFIGURATION] = PWM_CONFIGURATION_DEFAULT;
    as->regs[A_PWM3_CONFIGURATION] = PWM_CONFIGURATION_DEFAULT;
    as->regs[A_REMOTE_1_TRANGE_PWM1_FREQ] = TRANGE_PWM_FREQ_DEFAULT;
    as->regs[A_LOCAL_TRANGE_PWM2_FREQ] = TRANGE_PWM_FREQ_DEFAULT;
    as->regs[A_REMOTE_2_TRANGE_PWM3_FREQ] = TRANGE_PWM_FREQ_DEFAULT;
    as->regs[A_PWM1_MAX_DUTY_CYCLE] = PWM_MIN_DUTY_CYCLE_DEFAULT;
    as->regs[A_PWM2_MAX_DUTY_CYCLE] = PWM_MIN_DUTY_CYCLE_DEFAULT;
    as->regs[A_PWM3_MAX_DUTY_CYCLE] = PWM_MIN_DUTY_CYCLE_DEFAULT;
    as->regs[A_REMOTE_1_TEMP_TMIN] = TEMP_TMIN_DEFAULT;
    as->regs[A_LOCAL_TEMP_TMIN] = TEMP_TMIN_DEFAULT;
    as->regs[A_REMOTE_2_TEMP_TMIN] = TEMP_TMIN_DEFAULT;
    as->regs[A_REMOTE_1_THERM_TEMP_LIMIT] = THERM_TEMP_LIMIT_DEFAULT;
    as->regs[A_LOCAL_THERM_TEMP_LIMIT] = THERM_TEMP_LIMIT_DEFAULT;
    as->regs[A_REMOTE_2_THERM_TEMP_LIMIT] = THERM_TEMP_LIMIT_DEFAULT;
    as->regs[A_REMOTE_1_LOCAL_TEMP_TMIN_HYSTERESIS]
        = REMOTE_1_LOCAL_TMIN_HYSTERESIS_DEFAULT;
    as->regs[A_REMOTE_2_TEMP_TMIN_HYSTERESIS]
        = REMOTE_2_TMIN_HYSTERESIS_DEFAULT;
    as->regs[A_TACH_PULSES_PER_REVOLUTION]
        = TACH_PULSES_PER_REVOLUTION_DEFAULT;
    as->regs[A_DEVICE_ID] = DEVICE_ID_DEFAULT;
    as->regs[A_COMPANY_ID] = COMPANY_ID_DEFAULT;
}

static void adt7475_init(Object *obj)
{
    ADT7475State *as = ADT7475(obj);

    for (int i = 0; i < ADT7475_NUM_TEMPS; i++) {
        object_property_add(obj, "temperature[*]", "int32",
                            adt7475_qmp_get_temp,
                            adt7475_qmp_set_temp, NULL,
                            &as->regs[ADT7475_TEMP(i)]);
    }

    for (int i = 0; i < ADT7475_NUM_FANS; i++) {
        object_property_add(obj, "tachometer[*]", "uint16",
                            adt7475_qmp_get_tach_reading,
                            adt7475_qmp_set_tach_reading, NULL,
                            &as->regs[ADT7475_TACH(i)]);
    }

    object_property_add(obj, "vcc", "uint16",
                        adt7475_qmp_get_voltage,
                        adt7475_qmp_set_voltage, NULL,
                        &as->regs[A_VCC_READING]);
    object_property_add(obj, "vccp", "uint16",
                        adt7475_qmp_get_voltage,
                        adt7475_qmp_set_voltage, NULL,
                        &as->regs[A_VCCP_READING]);

    for (int i = 0; i < ADT7475_NUM_PWM; i++) {
        object_property_add(obj, "pwm[*]", "uint8",
                            adt7475_qmp_get_pwm,
                            adt7475_qmp_set_pwm, NULL,
                            &as->regs[ADT7475_PWM(i)]);
    }

    object_property_add_uint16_ptr(obj, "fan_speed_rating",
                                   &as->fan_speed_rating,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint8_ptr(obj, "fan_speed_min",
                                   &as->fan_speed_min,
                                   OBJ_PROP_FLAG_READWRITE);
}

static void adt7475_class_init(ObjectClass *klass, const void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);
    SMBusDeviceClass *k = SMBUS_DEVICE_CLASS(klass);

    dc->desc = "ADT7475 remote thermal monitor and fan controller";

    k->write_data = adt7475_write;
    k->receive_byte = adt7475_receive;

    rc->phases.exit = adt7475_exit_reset;
}

static const TypeInfo adt7475_types[] = {
    {
        .name = TYPE_ADT7475,
        .parent = TYPE_SMBUS_DEVICE,
        .instance_size = sizeof(ADT7475State),
        .instance_init = adt7475_init,
        .class_init = adt7475_class_init,
    },
};

DEFINE_TYPES(adt7475_types)

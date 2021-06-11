/*
 * NXP 4, 8, 16-bit I2C GPIO expanders
 * Low-voltage translating I2C/SMBus GPIO expanders with interrupt output,
 * reset, and configuration registers
 *
 * Note: Polarity inversion emulation not implemented
 *
 * Copyright 2021 Google LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PCA_I2C_GPIO_H
#define PCA_I2C_GPIO_H

#include "hw/i2c/i2c.h"
#include "qom/object.h"

#define PCA6416_NUM_PINS         16
#define PCA9538_NUM_PINS         8
#define PCA9536_NUM_PINS         4

typedef struct PCAGPIOClass {
    I2CSlaveClass parent;

    uint8_t num_pins;
} PCAGPIOClass;

typedef struct PCAGPIOState {
    I2CSlave parent;

    uint16_t polarity_inv;
    uint16_t config;

    /* TCAL6416 specific registers */
    uint32_t output_drive_strength;
    uint16_t input_latch;
    uint16_t pu_pd_enable;
    uint16_t pu_pd_select;
    uint16_t interrupt_mask;
    uint16_t interrupt_status;
    uint8_t output_port_config;

    /* the values of the gpio pins are mirrored in these integers */
    uint16_t curr_input;
    uint16_t curr_output;
    uint16_t new_input;
    uint16_t new_output;

    /*
     * Note that these outputs need to be consumed by some other input
     * to be useful, qemu ignores writes to disconnected gpio pins
     */
    qemu_irq output[PCA6416_NUM_PINS];

    /* i2c transaction info */
    uint8_t command;
    bool i2c_cmd;

} PCAGPIOState;

#define TYPE_PCA_I2C_GPIO "pca_i2c_gpio"
OBJECT_DECLARE_TYPE(PCAGPIOState, PCAGPIOClass, PCA_I2C_GPIO)

#define PCA6416_INPUT_PORT_0                 0x00 /* read */
#define PCA6416_INPUT_PORT_1                 0x01 /* read */
#define PCA6416_OUTPUT_PORT_0                0x02 /* read/write */
#define PCA6416_OUTPUT_PORT_1                0x03 /* read/write */
#define PCA6416_POLARITY_INVERSION_PORT_0    0x04 /* read/write */
#define PCA6416_POLARITY_INVERSION_PORT_1    0x05 /* read/write */
#define PCA6416_CONFIGURATION_PORT_0         0x06 /* read/write */
#define PCA6416_CONFIGURATION_PORT_1         0x07 /* read/write */

#define TCAL6416_OUTPUT_STRENGTH_0           0x40 /* read/write */
#define TCAL6416_OUTPUT_STRENGTH_1           0x41 /* read/write */
#define TCAL6416_OUTPUT_STRENGTH_2           0x42 /* read/write */
#define TCAL6416_OUTPUT_STRENGTH_3           0x43 /* read/write */
#define TCAL6416_INPUT_LATCH_0               0x44 /* read/write */
#define TCAL6416_INPUT_LATCH_1               0x45 /* read/write */
#define TCAL6416_PU_PD_ENABLE_0              0x46 /* read/write */
#define TCAL6416_PU_PD_ENABLE_1              0x47 /* read/write */
#define TCAL6416_PU_PD_SELECT_0              0x48 /* read/write */
#define TCAL6416_PU_PD_SELECT_1              0x49 /* read/write */
#define TCAL6416_INTERRUPT_MASK_0            0x4A /* read/write */
#define TCAL6416_INTERRUPT_MASK_1            0x4B /* read/write */
#define TCAL6416_INTERRUPT_STATUS_0          0x4C /* read */
#define TCAL6416_INTERRUPT_STATUS_1          0x4D /* read */
#define TCAL6416_OUTPUT_PORT_CONFIG          0x4F /* read/write */

#define PCA9538_INPUT_PORT                   0x00 /* read */
#define PCA9538_OUTPUT_PORT                  0x01 /* read/write */
#define PCA9538_POLARITY_INVERSION_PORT      0x02 /* read/write */
#define PCA9538_CONFIGURATION_PORT           0x03 /* read/write */

#define PCA6416_OUTPUT_DEFAULT               0xFFFF
#define PCA6416_CONFIG_DEFAULT               0xFFFF

#define PCA_I2C_OUTPUT_DEFAULT               0xFFFF
#define PCA_I2C_CONFIG_DEFAULT               0xFFFF

#define TYPE_PCA6416_GPIO "pca6416"
#define TYPE_PCA9538_GPIO "pca9538"
#define TYPE_PCA9536_GPIO "pca9536"
#define TYPE_TCAL6416_GPIO "tcal6416"

#endif

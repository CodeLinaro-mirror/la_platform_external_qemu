/*
 * Nuvoton NPCM8mnx USB 2.0 Device Controller
 *
 * Copyright (C) 2023 Google, LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 */

#include "qemu/osdep.h"
#include "hw/usb/npcm8xx-udc.h"

#include "exec/memory.h"
#include "hw/irq.h"
#include "hw/qdev-core.h"
#include "hw/registerfields.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/typedefs.h"
#include "qom/object.h"
#include "trace.h"

#define NPCM8XX_MEMORY_ADDRESS_SIZE 0x1000

/* Device Control Capability Parameters Register */
#define DCCPARAMS_INIT_VALUE 0x83
REG32(DCCPARAMS, 0x124)
    FIELD(DCCPARAMS, DEVICE_ENDPOINT_NUMBER, 0, 5)
    FIELD(DCCPARAMS, DEVICE_CAPABLE, 7, 1)
    FIELD(DCCPARAMS, HOST_CAPABLE, 8, 1)
/* USB Command Register */
#define USBCMD_INIT_VALUE 0x80002
REG32(USBCMD, 0x140)
    FIELD(USBCMD, RUN, 0, 1)
    FIELD(USBCMD, RESET, 1, 1)
    FIELD(USBCMD, SETUP_TRIP_WIRE, 13, 1)
    FIELD(USBCMD, ADD_DTD_TRIP_WIRE, 14, 1)
    FIELD(USBCMD, INT_THRESHOLD_CONTROL, 16, 8)
/* USB Status Register */
#define USBSTS_INIT_VALUE 0x0
REG32(USBSTS, 0x144)
    FIELD(USBSTS, USB_INTERRUPT, 0, 1)
    FIELD(USBSTS, USB_ERROR_INTERRUPT, 1, 1)
    FIELD(USBSTS, PORT_CHANGE_DETECT, 2, 1)
    FIELD(USBSTS, SYSTEM_ERROR, 4, 1)
    FIELD(USBSTS, USB_RESET_RECEIVED, 6, 1)
    FIELD(USBSTS, SOF_RECEIVED, 7, 1)
    FIELD(USBSTS, DCSUSPEND, 8, 1)
    FIELD(USBSTS, NAK_INTERRUPT, 16, 1)
    FIELD(USBSTS, TIMER_INTERRUPT_0, 24, 1)
    FIELD(USBSTS, TIMER_INTERRUPT_1, 25, 1)
/* USB Interrupt Enable Register */
#define USBINTR_INIT_VALUE 0x0
REG32(USBINTR, 0x148)
    FIELD(USBINTR, USB_INTERRUPT, 0, 1)
    FIELD(USBINTR, USB_ERROR_INTERRUPT, 1, 1)
    FIELD(USBINTR, PORT_CHANGE_DETECT, 2, 1)
    FIELD(USBINTR, SYSTEM_ERROR, 4, 1)
    FIELD(USBINTR, USB_RESET_RECEIVED, 6, 1)
    FIELD(USBINTR, SOF_RECEIVED, 7, 1)
    FIELD(USBINTR, DCSUSPEND, 8, 1)
    FIELD(USBINTR, NAK_INTERRUPT, 16, 1)
    FIELD(USBINTR, TIMER_INTERRUPT_0, 24, 1)
    FIELD(USBINTR, TIMER_INTERRUPT_1, 25, 1)
/* Device Controller Endpoint List Address Register */
#define ENDPOINTLISTADDR_INIT_VALUE 0x0
REG32(ENDPOINTLISTADDR, 0x158)
    FIELD(ENDPOINTLISTADDR, EPBASE, 11, 21)
/* Device Controller Port Status/Controller 1 Register */
#define PORTSC1_INIT_VALUE 0x1000000
REG32(PORTSC1, 0x184)
    FIELD(PORTSC1, CURRENT_CONNECT_STATUS, 0, 1)
    FIELD(PORTSC1, PORT_ENABLE, 2, 1)
    FIELD(PORTSC1, FORCE_PORT_RESUME, 6, 1)
    FIELD(PORTSC1, SUSPEND, 7, 1)
    FIELD(PORTSC1, PORT_RESET, 8, 1)
    FIELD(PORTSC1, HIGH_SPEED_PORT, 9, 1)
    FIELD(PORTSC1, LINE_STATUS, 10, 2)
    FIELD(PORTSC1, PORT_TEST_CONTROL, 16, 4)
    FIELD(PORTSC1, PHY_LOW_POWER_SUSPEND, 23, 1)
    FIELD(PORTSC1, PORT_FORCE_FULL_SPEED_CONNECT, 24, 1)
    FIELD(PORTSC1, PORT_SPEED, 26, 2)
    FIELD(PORTSC1, SERIAL_TRANSCEIVER_SELECT, 29, 1)
    FIELD(PORTSC1, PARALLEL_TRANSCEIVER_SELECT, 30, 2)
/* USB Device Mode Register */
#define USBMODE_INIT_VALUE 0x15002
REG32(USBMODE, 0x1A8)
    FIELD(USBMODE, ENDIAN_SELECT, 2, 1)
    FIELD(USBMODE, SETUP_LOCKOUT_MODE, 3, 1)
    FIELD(USBMODE, STREAM_DISABLE_MODE, 4, 1)
/* Endpoint Control 0 Register */
#define ENDPTCTRL0_INIT_VALUE 0x800080
REG32(ENDPTCTRL0, 0x1C0)
    FIELD(ENDPTCTRL0, RX_STALL, 0, 1)
    FIELD(ENDPTCTRL0, RX_TYPE, 2, 2)
    FIELD(ENDPTCTRL0, RX_ENABLE, 7, 1)
    FIELD(ENDPTCTRL0, TX_STALL, 16, 1)
    FIELD(ENDPTCTRL0, TX_TYPE, 18, 2)
    FIELD(ENDPTCTRL0, TX_ENABLE, 23, 1)
/* Endpoint Control 1 Register */
#define ENDPTCTRL1_INIT_VALUE 0
REG32(ENDPTCTRL1, 0x1C4)
    FIELD(ENDPTCTRL1, RX_STALL, 0, 1)
    FIELD(ENDPTCTRL1, RX_DATA_SINK, 1, 1)
    FIELD(ENDPTCTRL1, RX_TYPE, 2, 2)
    FIELD(ENDPTCTRL1, RX_DATA_TOGGLE_INHIBIT, 5, 1)
    FIELD(ENDPTCTRL1, RX_DATA_TOGGLE_RESET, 6, 1)
    FIELD(ENDPTCTRL1, RX_ENABLE, 7, 1)
    FIELD(ENDPTCTRL1, TX_STALL, 16, 1)
    FIELD(ENDPTCTRL1, TX_DATA_SOURCE, 17, 1)
    FIELD(ENDPTCTRL1, TX_TYPE, 18, 2)
    FIELD(ENDPTCTRL1, TX_DATA_TOGGLE_INIBIT, 21, 1)
    FIELD(ENDPTCTRL1, TX_DATA_TOGGLE_RESET, 22, 1)
    FIELD(ENDPTCTRL1, TX_ENABLE, 23, 1)
/* Endpoint Control 2 Register */
#define ENDPTCTRL2_INIT_VALUE 0
REG32(ENDPTCTRL2, 0x1C8)
    FIELD(ENDPTCTRL2, RX_STALL, 0, 1)
    FIELD(ENDPTCTRL2, RX_DATA_SINK, 1, 1)
    FIELD(ENDPTCTRL2, RX_TYPE, 2, 2)
    FIELD(ENDPTCTRL2, RX_DATA_TOGGLE_INHIBIT, 5, 1)
    FIELD(ENDPTCTRL2, RX_DATA_TOGGLE_RESET, 6, 1)
    FIELD(ENDPTCTRL2, RX_ENABLE, 7, 1)
    FIELD(ENDPTCTRL2, TX_STALL, 16, 1)
    FIELD(ENDPTCTRL2, TX_DATA_SOURCE, 17, 1)
    FIELD(ENDPTCTRL2, TX_TYPE, 18, 2)
    FIELD(ENDPTCTRL2, TX_DATA_TOGGLE_INIBIT, 21, 1)
    FIELD(ENDPTCTRL2, TX_DATA_TOGGLE_RESET, 22, 1)
    FIELD(ENDPTCTRL2, TX_ENABLE, 23, 1)

static void npcm8xx_udc_reset(DeviceState *dev)
{
    NPCM8xxUDC *udc = NPCM8XX_UDC(dev);
    udc->running = false;

    NPCM8xxUDCRegisters *registers = (NPCM8xxUDCRegisters *)udc->registers;
    registers->status = USBSTS_INIT_VALUE;
    registers->interrupt_enable = USBINTR_INIT_VALUE;
    registers->endpoint_list_address = ENDPOINTLISTADDR_INIT_VALUE;
    registers->port_control_status = PORTSC1_INIT_VALUE;
    registers->mode = USBMODE_INIT_VALUE;
    registers->ep0_control = ENDPTCTRL0_INIT_VALUE;
    registers->ep1_control = ENDPTCTRL1_INIT_VALUE;
    registers->ep2_control = ENDPTCTRL2_INIT_VALUE;
    registers->command = USBCMD_INIT_VALUE & ~R_USBCMD_RESET_MASK;
}

static inline void npcm8xx_udc_update_irq(NPCM8xxUDC *udc)
{
    NPCM8xxUDCRegisters *registers = (NPCM8xxUDCRegisters *)udc->registers;
    if (udc->running) {
        qemu_set_irq(udc->irq, registers->interrupt_enable & registers->status);
    } else {
        qemu_set_irq(udc->irq, 0);
    }
}

static inline void npcm8xx_udc_write_usbcmd(NPCM8xxUDC *udc, uint32_t value)
{
    NPCM8xxUDCRegisters *registers = (NPCM8xxUDCRegisters *)udc->registers;

    registers->command = value;

    if (FIELD_EX32(registers->command, USBCMD, RESET)) {
        npcm8xx_udc_reset(DEVICE(udc));
    }

    /* Handle run/stop bit toggle */
    bool new_run_state = FIELD_EX32(registers->command, USBCMD, RUN) != 0;

    if (udc->running != new_run_state) {
        udc->running = new_run_state;
        npcm8xx_udc_update_irq(udc);
    }
}

static inline void npcm8xx_udc_write_usbsts(NPCM8xxUDC *udc, uint32_t value)
{
    NPCM8xxUDCRegisters *registers = (NPCM8xxUDCRegisters *)udc->registers;

    /* Clear read-only bits */
    value = FIELD_DP32(value, USBSTS, SYSTEM_ERROR, 0);
    value = FIELD_DP32(value, USBSTS, NAK_INTERRUPT, 0);

    /* Clear write 1 clear bits */
    registers->status &= ~value;

    /* Write the read/write bit back in */
    uint8_t dcsuspend_bit = FIELD_EX32(value, USBSTS, DCSUSPEND) ? 1 : 0;
    registers->status =
        FIELD_DP32(registers->status, USBSTS, DCSUSPEND, dcsuspend_bit);

    npcm8xx_udc_update_irq(udc);
}

static inline void npcm8xx_udc_write_portsc1(NPCM8xxUDC *udc, uint32_t value)
{
    NPCM8xxUDCRegisters *registers = (NPCM8xxUDCRegisters *)udc->registers;

    /* Clear read-only bits */
    uint32_t read_only_mask =
        R_PORTSC1_CURRENT_CONNECT_STATUS_MASK | R_PORTSC1_SUSPEND_MASK |
        R_PORTSC1_PORT_RESET_MASK | R_PORTSC1_HIGH_SPEED_PORT_MASK |
        R_PORTSC1_LINE_STATUS_MASK | R_PORTSC1_PORT_SPEED_MASK |
        R_PORTSC1_SERIAL_TRANSCEIVER_SELECT_MASK;
    value &= ~read_only_mask;
    registers->port_control_status =
        value | (registers->port_control_status & read_only_mask);
}

static inline void npcm8xx_udc_write_endptctrl0(NPCM8xxUDC *udc, uint32_t value)
{
    NPCM8xxUDCRegisters *registers = (NPCM8xxUDCRegisters *)udc->registers;

    /* Clear read-only bits */
    uint32_t read_only_mask =
        R_ENDPTCTRL0_RX_ENABLE_MASK | R_ENDPTCTRL0_TX_ENABLE_MASK;
    value &= ~read_only_mask;
    registers->ep0_control = value | (registers->ep0_control & read_only_mask);
}

static uint64_t npcm8xx_udc_read(void *opaque, hwaddr offset, unsigned size)
{
    NPCM8xxUDC *udc = NPCM8XX_UDC(opaque);
    NPCM8xxUDCRegisters *registers = (NPCM8xxUDCRegisters *)udc->registers;
    uint32_t value = 0;

    switch (offset) {
    case A_DCCPARAMS:
        value = DCCPARAMS_INIT_VALUE;
        break;
    case A_USBCMD:
        value = registers->command;
        break;
    case A_USBSTS:
        value = registers->status;
        break;
    case A_USBINTR:
        value = registers->interrupt_enable;
        break;
    case A_ENDPOINTLISTADDR:
        value = registers->endpoint_list_address;
        break;
    case A_PORTSC1:
        value = registers->port_control_status;
        break;
    case A_USBMODE:
        value = registers->mode;
        break;
    case A_ENDPTCTRL0:
        value = registers->ep0_control;
        break;
    case A_ENDPTCTRL1:
        value = registers->ep1_control;
        break;
    case A_ENDPTCTRL2:
        value = registers->ep2_control;
        break;
    default:
        qemu_log_mask(
            LOG_GUEST_ERROR,
            "%s: Attempted to read from unsupported register 0x%lx\n",
            DEVICE(opaque)->canonical_path, offset);
    }

    trace_npcm8xx_udc_read(offset, value);
    return value;
}

static void npcm8xx_udc_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    NPCM8xxUDC *udc = NPCM8XX_UDC(opaque);
    NPCM8xxUDCRegisters *registers = (NPCM8xxUDCRegisters *)udc->registers;

    switch (offset) {
    case A_DCCPARAMS:
        /* Read-only register */
        qemu_log_mask(LOG_GUEST_ERROR,
                        "%s: Attempted to write to read-only register 0x%x\n",
                        DEVICE(opaque)->canonical_path,
                        A_DCCPARAMS);
        break;
    case A_USBCMD:
        npcm8xx_udc_write_usbcmd(udc, value);
        break;
    case A_USBSTS:
        npcm8xx_udc_write_usbsts(udc, value);
        break;
    case A_USBINTR:
        registers->interrupt_enable = value;
        npcm8xx_udc_update_irq(udc);
        break;
    case A_ENDPOINTLISTADDR:
        registers->endpoint_list_address = value;
        break;
    case A_PORTSC1:
        npcm8xx_udc_write_portsc1(udc, value);
        break;
    case A_USBMODE:
        registers->mode = value;
        break;
    case A_ENDPTCTRL0:
        npcm8xx_udc_write_endptctrl0(udc, value);
        break;
    case A_ENDPTCTRL1:
        registers->ep1_control = value;
        break;
    case A_ENDPTCTRL2:
        registers->ep2_control = value;
        break;
    default:
        qemu_log_mask(
            LOG_GUEST_ERROR,
            "%s: Attempted to write to unsupported register 0x%lx\n",
            DEVICE(opaque)->canonical_path, offset);
    }

    trace_npcm8xx_udc_write(offset, value);
}

static const MemoryRegionOps npcm8xx_udc_mr_ops = {
    .read = npcm8xx_udc_read,
    .write = npcm8xx_udc_write,
    .valid = {
            .min_access_size = 4,
            .max_access_size = 4,
            .unaligned = false,
        },
};

static const VMStateDescription vmstate_npcm8xx_udc = {
    .name = TYPE_NPCM8XX_UDC,
    .version_id = 0,
    .minimum_version_id = 0,
    .fields = (VMStateField[]){
        VMSTATE_UINT32_ARRAY(registers, NPCM8xxUDC, NPCM8XX_UDC_NUM_REGS),
        VMSTATE_END_OF_LIST(),
    }
};

static void npcm8xx_udc_realize(DeviceState *dev, Error **err)
{
    NPCM8xxUDC *udc = NPCM8XX_UDC(dev);
    memory_region_init_io(&udc->mr, OBJECT(udc), &npcm8xx_udc_mr_ops, udc,
                          TYPE_NPCM8XX_UDC, NPCM8XX_MEMORY_ADDRESS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &udc->mr);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &udc->irq);
}

static void npcm8xx_udc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = npcm8xx_udc_realize;
    device_class_set_legacy_reset(dc, npcm8xx_udc_reset);
    dc->vmsd = &vmstate_npcm8xx_udc;
}

static const TypeInfo npcm8xx_udc_info = {
    .name = TYPE_NPCM8XX_UDC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(NPCM8xxUDC),
    .class_init = npcm8xx_udc_class_init,
};

static void npcm8xx_udc_register_type(void)
{
    type_register_static(&npcm8xx_udc_info);
}

type_init(npcm8xx_udc_register_type)

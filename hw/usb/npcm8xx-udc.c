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

#include <libusb.h>

#include "exec/cpu-common.h"
#include "exec/memory.h"
#include "hw/irq.h"
#include "hw/qdev-core.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/sysbus.h"
#include "hw/usb/redirect-host.h"
#include "migration/vmstate.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/typedefs.h"
#include "qom/object.h"
#include "trace.h"

#define NPCM8XX_MEMORY_ADDRESS_SIZE 0x1000
#define NPCM8XX_CONTROL_EP_ADDRESS 0

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
#define PORTSC1_INIT_VALUE 0x9000204
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
/* Endpoint Setup Status Register */
REG32(ENDPTSETUPSTAT, 0x1AC)
    FIELD(ENDPTSETUPSTAT, SETUP_STATUS, 0, 5)
/* Endpoint Initialization Register */
REG32(ENDPTPRIME, 0x1B0)
    FIELD(ENDPTPRIME, RX_BUFFER, 0, 7)
    FIELD(ENDPTPRIME, TX_BUFFER, 16, 7)
/* Endpoint De-Initialization Register*/
REG32(ENDPTFLUSH, 0x1B4)
    FIELD(ENDPTFLUSH, RX_BUFFER, 0, 7)
    FIELD(ENDPTFLUSH, TX_BUFFER, 16, 7)
/* Endpoint Status Register */
REG32(ENDPTSTAT, 0x1B8)
    FIELD(ENDPTSTAT, RX_BUFFER, 0, 7)
    FIELD(ENDPTSTAT, TX_BUFFER, 16, 7)
/* Endpoint Complete Register */
REG32(ENDPTCOMPLETE, 0x1BC)
    FIELD(ENDPTCOMPLETE, RX_BUFFER, 0, 7)
    FIELD(ENDPTCOMPLETE, TX_BUFFER, 16, 7)
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

static inline void npcm8xx_udc_control_transfer(NPCM8xxUDC *udc,
                                                uint8_t request_type,
                                                uint8_t request, uint16_t value,
                                                uint16_t index, uint16_t length)
{
    NPCM8xxUDCRegisters *registers = (NPCM8xxUDCRegisters *)udc->registers;
    uint32_t ep0_qh_address = registers->endpoint_list_address;
    uint32_t setup1 = request_type | (request << 8) | (value << 16);
    uint32_t setup2 = index | (length << 16);

    cpu_physical_memory_write(ep0_qh_address + 40, &setup1, 4);
    cpu_physical_memory_write(ep0_qh_address + 44, &setup2, 4);

    registers->endpoint_setup_status |= 1;
    registers->status |= R_USBSTS_USB_INTERRUPT_MASK;
}

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

        if (udc->running && udc->attached) {
            registers->port_control_status |=
                R_PORTSC1_CURRENT_CONNECT_STATUS_MASK;
            registers->status |= R_USBSTS_PORT_CHANGE_DETECT_MASK;
        }

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

    /**
     * USB device attachment is only valid after the firmware has acknowledged
     * the port change status.
     */
    if (udc->running && udc->attached) {
        if (FIELD_EX32(value, USBSTS, PORT_CHANGE_DETECT)) {
            usbredir_host_attach_complete(udc->usbredir_host);
        }
    }

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

static inline void npcm8xx_udc_send_data(NPCM8xxUDC *udc,
                                         uint8_t endpoint_number,
                                         TransferDescriptor *td_head)
{
    TransferDescriptor next_td;
    cpu_physical_memory_read(td_head->next_pointer, &next_td,
                             sizeof(TransferDescriptor));
    int data_size =
        (next_td.info & TD_INFO_TOTAL_BYTES_MASK) >> TD_INFO_TOTAL_BYTES_SHIFT;
    int sent_data_size = 0;

    uint8_t *data = g_malloc(data_size);
    cpu_physical_memory_read(next_td.buffer_pointers[0], data, data_size);

    if (endpoint_number == 0) {
        sent_data_size = usbredir_host_control_transfer_complete(
            udc->usbredir_host, data, data_size);
    } else {
        sent_data_size =
            usbredir_host_data_in_complete(udc->usbredir_host, data, data_size);
    }

    g_free(data);

    if (data_size == sent_data_size) {
        /* Clear status if transfer succeeds */
        next_td.info = TD_INFO_INTERRUPT_ON_COMPLETE_MASK;
        cpu_physical_memory_write(td_head->next_pointer + 4, &next_td.info,
                                  sizeof(uint32_t));
    } else {
        error_report("%s: unable to send data via usbredir host.",
                     DEVICE(udc)->canonical_path);
    }
}

static inline void npcm8xx_udc_write_endptprime(NPCM8xxUDC *udc, uint32_t value)
{
    NPCM8xxUDCRegisters *registers = (NPCM8xxUDCRegisters *)udc->registers;
    QueueHead qh_in;
    const uint32_t rx_qh_base_address = registers->endpoint_list_address;
    const uint32_t tx_qh_base_address = rx_qh_base_address + sizeof(QueueHead);

    if (!udc->running) {
        qemu_log_mask(
            LOG_GUEST_ERROR,
            "%s[%u]: Attempted to send data when device is not running\n",
            DEVICE(udc)->canonical_path, udc->device_index);
        return;
    }

    if (!udc->attached) {
        qemu_log_mask(
            LOG_GUEST_ERROR,
            "%s[%u]: Attempted to send data when device is not attached\n",
            DEVICE(udc)->canonical_path, udc->device_index);
        return;
    }

    /*
     * The software write to the register must occur before processing RX
     * transfer descriptor because once the UDC's RX status is available to the
     * usbredir host, the usbredir host might send new message, causing the UDC
     * to clear endpoint status register.
     */
    registers->endpoint_status |= (value & R_ENDPTPRIME_RX_BUFFER_MASK);
    registers->endpoint_complete |= (value & R_ENDPTPRIME_TX_BUFFER_MASK);

    /* Process TX transfer descriptor */
    uint8_t tx_endpoints = FIELD_EX32(value, ENDPTPRIME, TX_BUFFER);
    for (uint8_t ep_num = 0; tx_endpoints != 0;
            ++ep_num, tx_endpoints >>= 1) {
        if ((tx_endpoints & 1) == 0) {
            continue;
        }

        cpu_physical_memory_read(
            tx_qh_base_address + (ep_num << 1) * sizeof(QueueHead),
            &qh_in, sizeof(QueueHead));
        npcm8xx_udc_send_data(udc, ep_num, &qh_in.td);
    }

    /* Process RX transfer descriptor */
    uint8_t rx_endpoints = value & R_ENDPTPRIME_RX_BUFFER_MASK;
    for (uint8_t ep_num = 0; rx_endpoints != 0;
            rx_endpoints >>= 1, ++ep_num) {
        if ((rx_endpoints & 1) == 0) {
            continue;
        }

        /*
         * Notify usbredir host that the most recently received message has
         * been processed, and the UDC is now available to process new message.
         */
        usbredir_host_data_out_complete(udc->usbredir_host, ep_num);
    }

    registers->status |= R_USBSTS_USB_INTERRUPT_MASK;
    npcm8xx_udc_update_irq(udc);
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
    case A_ENDPTSETUPSTAT:
        value = registers->endpoint_setup_status;
        break;
    case A_ENDPTPRIME:
        value = registers->endpoint_prime;
        break;
    case A_ENDPTFLUSH:
        value = registers->endpoint_flush;
        break;
    case A_ENDPTSTAT:
        value = registers->endpoint_status;
        break;
    case A_ENDPTCOMPLETE:
        value = registers->endpoint_complete;
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

    trace_npcm8xx_udc_read(udc->device_index, offset, value);
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
    case A_ENDPTSETUPSTAT:
        registers->endpoint_setup_status &= ~value;
        break;
    case A_ENDPTPRIME:
        npcm8xx_udc_write_endptprime(udc, value);
        break;
    case A_ENDPTFLUSH:
        /* Write to endpoint flush clears endpoint status bits. */
        registers->endpoint_status &= ~value;
        break;
    case A_ENDPTSTAT:
        /* Read-only register */
        qemu_log_mask(LOG_GUEST_ERROR,
                        "%s: Attempted to write to read-only register 0x%x\n",
                        DEVICE(opaque)->canonical_path,
                        A_ENDPTSTAT);
        break;
    case A_ENDPTCOMPLETE:
        registers->endpoint_complete &= ~value;
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

    trace_npcm8xx_udc_write(udc->device_index, offset, value);
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

static uint8_t npcm8xx_udc_usbredir_attach(void *opaque)
{
    NPCM8xxUDC *udc = NPCM8XX_UDC(opaque);
    NPCM8xxUDCRegisters *registers = (NPCM8xxUDCRegisters *)udc->registers;

    udc->attached = true;

    if (udc->running) {
        registers->port_control_status |= R_PORTSC1_CURRENT_CONNECT_STATUS_MASK;
        registers->status |= R_USBSTS_PORT_CHANGE_DETECT_MASK;
    }

    npcm8xx_udc_update_irq(udc);
    return NPCM8XX_CONTROL_EP_ADDRESS;
}

static void npcm8xx_udc_usbredir_reset(void *opaque)
{
    NPCM8xxUDC *udc = NPCM8XX_UDC(opaque);

    if (udc->attached) {
        /* Undefined behavior, so do nothing. */
        qemu_log_mask(
            LOG_GUEST_ERROR,
            "%s: usbredir reset request failed to reset the device.",
            DEVICE(opaque)->canonical_path);
        return;
    }

    npcm8xx_udc_reset(DEVICE(udc));
}

static void npcm8xx_udc_usbredir_control_transfer(
    void *opaque, uint8_t endpoint_address, uint8_t request_type,
    uint8_t request, uint16_t value, uint16_t index, uint16_t length,
    uint8_t *data, int data_len)
{
    NPCM8xxUDC *udc = NPCM8XX_UDC(opaque);

    if ((endpoint_address & LIBUSB_ENDPOINT_ADDRESS_MASK) != 0) {
        usbredir_host_control_transfer_complete(udc->usbredir_host, NULL, 0);
    }

    npcm8xx_udc_control_transfer(udc, request_type, request, value, index,
                                 length);
    npcm8xx_udc_update_irq(udc);
}

static void npcm8xx_udc_usbredir_write_data(void *opaque,
                                            uint8_t endpoint_address,
                                            uint8_t *data, int data_len)
{
    NPCM8xxUDC *udc = NPCM8XX_UDC(opaque);
    NPCM8xxUDCRegisters *registers = (NPCM8xxUDCRegisters *)udc->registers;
    QueueHead rx_qh;
    TransferDescriptor rx_td;
    uint32_t request_len;
    uint32_t current_rx_td_pointer;
    const uint32_t qh_base_address = registers->endpoint_list_address;
    const uint8_t ep_num = endpoint_address & LIBUSB_ENDPOINT_ADDRESS_MASK;

    cpu_physical_memory_read(
        qh_base_address + (ep_num << 1) * sizeof(QueueHead), &rx_qh,
        sizeof(QueueHead));

    if (udc->next_rx_td_pointer) {
        current_rx_td_pointer = udc->next_rx_td_pointer;
    } else {
        current_rx_td_pointer = rx_qh.td.next_pointer;
    }

    cpu_physical_memory_read(current_rx_td_pointer, &rx_td,
                             sizeof(TransferDescriptor));
    request_len = rx_td.info >> TD_INFO_TOTAL_BYTES_SHIFT;
    rx_td.info = ((request_len - data_len) << TD_INFO_TOTAL_BYTES_SHIFT) |
                 TD_INFO_INTERRUPT_ON_COMPLETE_MASK;

    cpu_physical_memory_write(current_rx_td_pointer + 4, &rx_td.info,
                              sizeof(uint32_t));
    cpu_physical_memory_write(rx_td.buffer_pointers[0], data, data_len);

    if ((rx_td.next_pointer & 1) == 0) {
        udc->next_rx_td_pointer = rx_td.next_pointer;
    } else {
        udc->next_rx_td_pointer = 0;
    }

    registers->endpoint_complete |= (1 << ep_num);
    registers->endpoint_status &= ~R_ENDPTSTAT_RX_BUFFER_MASK;
    registers->status |= R_USBSTS_USB_INTERRUPT_MASK;
    npcm8xx_udc_update_irq(udc);
}

static const USBRedirectHostOps npcm8xx_udc_usbredir_ops = {
    .on_attach = npcm8xx_udc_usbredir_attach,
    .reset = npcm8xx_udc_usbredir_reset,
    .control_transfer = npcm8xx_udc_usbredir_control_transfer,
    .data_out = npcm8xx_udc_usbredir_write_data,
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
    udc->usbredir_ops = &npcm8xx_udc_usbredir_ops;
}

static const Property npcm8xx_udc_properties[] = {
    DEFINE_PROP_UINT8("device-index", NPCM8xxUDC, device_index, 0xff),
};

static void npcm8xx_udc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_props(dc, npcm8xx_udc_properties);
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

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
#include <usbredirproto.h>

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
#define HIGH_SPEED_MAX_PACKET_SIZE 0x512

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

static inline void npcm8xx_udc_initiate_usbredir_request(
    NPCM8xxUDC *udc, int request_type, bool require_if_and_ep_info)
{
    udc->usbredir_request.request_type = request_type;
    udc->usbredir_request.active = true;
    udc->usbredir_request.require_if_and_ep_info = require_if_and_ep_info;

    if (require_if_and_ep_info) {
        /**
         * Use max packet size because we need the entire configuration
         * descriptor.
         */
        npcm8xx_udc_control_transfer(udc, LIBUSB_ENDPOINT_IN,
                                     LIBUSB_REQUEST_GET_DESCRIPTOR,
                                     LIBUSB_DT_CONFIG << 8, 0,
                                     HIGH_SPEED_MAX_PACKET_SIZE);
    }
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
     * All USB transfers are only valid after firmware handles the port change
     * interrupt, so device_connect request can only be sent afterwards.
     * Clearing port change detect status is the best way to tell if the
     * firmware has processed the port information, so the device_connect
     * request is sent, if the firmware tries to clear port change status.
     */
    if (udc->running && udc->attached) {
        if (FIELD_EX32(value, USBSTS, PORT_CHANGE_DETECT)) {
            npcm8xx_udc_initiate_usbredir_request(udc, usb_redir_device_connect,
                                                  true);
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

static inline uint8_t ep_address_to_usbredir_ep_index(uint8_t endpoint_address)
{
    return ((endpoint_address & 0x80) >> 3) | (endpoint_address & 0x0f);
}

static inline int read_interface_info(
    struct usb_redir_interface_info_header *interface_info,
    struct libusb_interface_descriptor *interface_desc)
{
    int index = interface_desc->bInterfaceNumber;
    interface_info->interface[index] = index;
    interface_info->interface_class[index] = interface_desc->bInterfaceClass;
    interface_info->interface_subclass[index] =
        interface_desc->bInterfaceSubClass;
    interface_info->interface_protocol[index] =
        interface_desc->bInterfaceProtocol;

    return interface_desc->bLength;
}

static inline int read_ep_info(
    struct usb_redir_ep_info_header *ep_info,
    struct libusb_interface_descriptor *interface_desc,
    struct libusb_endpoint_descriptor *ep_desc)
{
    int read_bytes = 0;

    for (int i = 0; i < interface_desc->bNumEndpoints; i++) {
        int ep_index =
            ep_address_to_usbredir_ep_index(ep_desc[i].bEndpointAddress);
        ep_info->interface[ep_index] = interface_desc->bInterfaceNumber;
        ep_info->type[ep_index] = ep_desc[i].bmAttributes & 0x3;
        ep_info->max_packet_size[ep_index] = ep_desc[i].wMaxPacketSize;
        ep_info->interval[ep_index] = ep_desc[i].bInterval;
        read_bytes += ep_desc->bLength;
    }

    return read_bytes;
}

static inline struct usb_redir_device_connect_header make_device_info(
    struct libusb_device_descriptor *device_desc)
{
    struct usb_redir_device_connect_header device_info;
    device_info.device_class = device_desc->bDeviceClass;
    device_info.device_subclass = device_desc->bDeviceSubClass;
    device_info.device_protocol = device_desc->bDeviceProtocol;
    device_info.device_version_bcd = device_desc->bcdUSB;
    device_info.vendor_id = device_desc->idVendor;
    device_info.product_id = device_desc->idProduct;
    device_info.speed = usb_redir_speed_high;
    return device_info;
}

static inline int usbredir_send_if_and_ep_info(USBRedirectHost *usbredir_host,
                                               uint8_t *data)
{
    struct usb_redir_interface_info_header interface_info;
    struct usb_redir_ep_info_header ep_info;
    uint8_t *init_data_ptr = data;
    for (int i = 0; i < 32; i++) {
        ep_info.type[i] = usb_redir_type_invalid;
    }

    struct libusb_config_descriptor *config_desc = (void *)data;
    interface_info.interface_count = config_desc->bNumInterfaces;
    data += config_desc->bLength;

    for (int i = 0; i < interface_info.interface_count; i++) {
        struct libusb_interface_descriptor *interface_desc = (void *)data;
        data += read_interface_info(&interface_info, interface_desc);
        data += read_ep_info(&ep_info, interface_desc, (void *)data);
    }

    if (usbredir_host_send_ep_info(usbredir_host, &ep_info) != 0) {
        return 0;
    }
    if (usbredir_host_send_interface_info(usbredir_host, &interface_info) !=
        0) {
        return 0;
    }

    return data - init_data_ptr;
}

static inline int usbredir_send_device_connect(USBRedirectHost *usbredir_host,
                                               uint8_t *data)
{
    struct libusb_device_descriptor *device_desc = (void *)data;
    struct usb_redir_device_connect_header device_info =
        make_device_info(device_desc);
    if (usbredir_host_send_device_connect(usbredir_host, &device_info) != 0) {
        return 0;
    }
    return device_desc->bLength;
}

static inline int npcm8xx_udc_send_data_over_usbredir(NPCM8xxUDC *udc,
                                                      uint8_t *data,
                                                      int data_size)
{
    if (!udc->attached) {
        qemu_log_mask(LOG_GUEST_ERROR, "Not ready to send");
    }

    if (udc->usbredir_request.active) {
        if (udc->usbredir_request.require_if_and_ep_info) {
            if (usbredir_send_if_and_ep_info(udc->usbredir_host, data) ==
                data_size) {
                udc->usbredir_request.require_if_and_ep_info = false;

                /* After success switch request type here */
                switch (udc->usbredir_request.request_type) {
                case usb_redir_device_connect:
                    npcm8xx_udc_control_transfer(
                        udc, LIBUSB_ENDPOINT_IN,
                        LIBUSB_REQUEST_GET_DESCRIPTOR,
                        LIBUSB_DT_DEVICE << 8, 0, LIBUSB_DT_DEVICE_SIZE);
                    break;
                }
                return data_size;
            }
        } else {
            switch (udc->usbredir_request.request_type) {
            case usb_redir_device_connect:
                if (usbredir_send_device_connect(udc->usbredir_host, data) ==
                    data_size) {
                    udc->usbredir_request.active = false;
                    return data_size;
                }
                break;
            }
        }
    }

    return 0;
}

static inline void npcm8xx_udc_send_data(NPCM8xxUDC *udc,
                                         TransferDescriptor td_head)
{
    while ((td_head.next_pointer & TD_NEXT_POINTER_VALID_MASK) == 0) {
        TransferDescriptor next_td;
        cpu_physical_memory_read(td_head.next_pointer, &next_td,
                                 sizeof(TransferDescriptor));
        int data_size = (next_td.info & TD_INFO_TOTAL_BYTES_MASK) >>
                        TD_INFO_TOTAL_BYTES_SHIFT;
        int sent_data_size = 0;

        uint8_t *data = g_malloc(data_size);
        cpu_physical_memory_read(next_td.buffer_pointers[0], data, data_size);
        sent_data_size =
            npcm8xx_udc_send_data_over_usbredir(udc, data, data_size);

        g_free(data);

        if (data_size == sent_data_size) {
            /* Clear status if transfer succeeds */
            next_td.info &= ~TD_INFO_STATUS_MASK;
            cpu_physical_memory_write(td_head.next_pointer, &next_td,
                                      sizeof(TransferDescriptor));
        } else {
            error_report("%s: unable to send data via usbredir host.",
                         DEVICE(udc)->canonical_path);
        }

        td_head = next_td;
    }
}

static inline void npcm8xx_udc_write_endptprime(NPCM8xxUDC *udc, uint32_t value)
{
    NPCM8xxUDCRegisters *registers = (NPCM8xxUDCRegisters *)udc->registers;
    QueueHead qh;
    registers->endpoint_prime = value;

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

    uint8_t tx_endpoints = FIELD_EX32(value, ENDPTPRIME, TX_BUFFER);
    if (tx_endpoints) {
        const uint32_t tx_qh_base_address =
            registers->endpoint_list_address + sizeof(QueueHead);
        for (uint8_t tx_endpoint_index = 0; tx_endpoints != 0;
             tx_endpoint_index += 2, tx_endpoints >>= 1) {
            cpu_physical_memory_read(
                tx_qh_base_address + tx_endpoint_index * sizeof(QueueHead), &qh,
                sizeof(QueueHead));
            npcm8xx_udc_send_data(udc, qh.td);
        }
    }

    registers->endpoint_complete = value;
    registers->endpoint_status = value;
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

static void npcm8xx_udc_usbredir_attach(void *opaque)
{
    NPCM8xxUDC *udc = NPCM8XX_UDC(opaque);
    NPCM8xxUDCRegisters *registers = (NPCM8xxUDCRegisters *)udc->registers;

    udc->attached = true;

    if (udc->running) {
        registers->port_control_status |= R_PORTSC1_CURRENT_CONNECT_STATUS_MASK;
        registers->status |= R_USBSTS_PORT_CHANGE_DETECT_MASK;
    }

    npcm8xx_udc_update_irq(udc);
}

static const USBRedirectHostOps npcm8xx_udc_usbredir_ops = {
    .on_attach = npcm8xx_udc_usbredir_attach,
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

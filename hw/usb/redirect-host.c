/*
 * USB Redirector usb-host
 *
 * Copyright (C) 2023 Google, LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 */

#include "qemu/osdep.h"
#include "hw/usb/redirect-host.h"

#include "libusb.h"
#include <stdbool.h>
#include <string.h>
#include <usbredirparser.h>
#include <usbredirproto.h>

#include "chardev/char.h"
#include "chardev/char-fe.h"
#include "hw/qdev-core.h"
#include "hw/qdev-properties-system.h"
#include "qapi/qapi-types-run-state.h"
#include "qemu/error-report.h"
#include "qemu/log-for-trace.h"
#include "qemu/log.h"
#include "qemu/typedefs.h"
#include "qemu/units.h"
#include "system/runstate.h"

#define VERSION "qemu usb-redir host " QEMU_VERSION
#define HS_USB_MAX_PACKET_SIZE 512
#define USB_GET_CONFIGURATION_DATA_SIZE 1

void usbredir_host_attach_complete(USBRedirectHost *usbredir_host)
{
    /* Start the device connect workflow once the device is attached. */
    usbredir_host->request.request_type = usb_redir_device_connect;
    usbredir_host->request.active = true;
    usbredir_host->request.require_if_and_ep_info = true;
    usbredir_host->device_ops->control_transfer(
        usbredir_host->opaque, usbredir_host->control_endpoint_address,
        LIBUSB_ENDPOINT_IN, LIBUSB_REQUEST_GET_DESCRIPTOR,
        LIBUSB_DT_CONFIG << 8, 0, HS_USB_MAX_PACKET_SIZE, NULL, 0);
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

static inline uint8_t ep_address_to_usbredir_ep_index(uint8_t endpoint_address)
{
    return ((endpoint_address & 0x80) >> 3) | (endpoint_address & 0x0f);
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

static inline int send_interface_and_ep_info(struct usbredirparser *parser,
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

    usbredirparser_send_ep_info(parser, &ep_info);

    if (usbredirparser_do_write(parser) != 0) {
        return 0;
    }

    usbredirparser_send_interface_info(parser, &interface_info);

    if (usbredirparser_do_write(parser) != 0) {
        return 0;
    }

    return data - init_data_ptr;
}

static inline struct usb_redir_device_connect_header make_device_connect_header(
    struct libusb_device_descriptor *device_desc)
{
    struct usb_redir_device_connect_header device_connect;
    device_connect.device_class = device_desc->bDeviceClass;
    device_connect.device_subclass = device_desc->bDeviceSubClass;
    device_connect.device_protocol = device_desc->bDeviceProtocol;
    device_connect.device_version_bcd = device_desc->bcdUSB;
    device_connect.vendor_id = device_desc->idVendor;
    device_connect.product_id = device_desc->idProduct;
    device_connect.speed = usb_redir_speed_high;
    return device_connect;
}

static inline int usbredir_host_handle_device_connect(
    USBRedirectHost *usbredir_host, uint8_t *data)
{
    if (usbredir_host->request.require_if_and_ep_info) {
        usbredir_host->request.require_if_and_ep_info = false;
        usbredir_host->device_ops->control_transfer(
            usbredir_host->opaque, usbredir_host->control_endpoint_address,
            LIBUSB_ENDPOINT_IN, LIBUSB_REQUEST_GET_DESCRIPTOR,
            LIBUSB_DT_DEVICE << 8, 0, LIBUSB_DT_DEVICE_SIZE, NULL, 0);
        return send_interface_and_ep_info(usbredir_host->parser, data);
    }

    struct libusb_device_descriptor *device_desc = (void *)data;
    struct usb_redir_device_connect_header device_info =
        make_device_connect_header(device_desc);

    usbredirparser_send_device_connect(usbredir_host->parser, &device_info);

    if (usbredirparser_do_write(usbredir_host->parser) != 0) {
        return 0;
    }

    usbredir_host->request.active = false;
    return device_desc->bLength;
}

static inline int usbredir_host_send_control_packet(
    USBRedirectHost *usbredir_host, uint8_t *data, int data_size)
{
    struct usb_redir_control_packet_header *control_packet =
        (void *)usbredir_host->usbredir_header_cache;

    control_packet->length = data_size;
    control_packet->status = usb_redir_success;

    usbredirparser_send_control_packet(usbredir_host->parser,
                                       usbredir_host->latest_packet_id,
                                       control_packet, data, data_size);

    if (usbredirparser_do_write(usbredir_host->parser) != 0) {
        return 0;
    }

    return data_size;
}

static inline int usbredir_host_handle_configuration_status(
    USBRedirectHost *usbredir_host, uint8_t *data)
{
    if (usbredir_host->request.require_if_and_ep_info) {
        if (usbredir_host->request.requested_config_descriptor) {
            usbredir_host->request.require_if_and_ep_info = false;
            usbredir_host->device_ops->control_transfer(
                usbredir_host->opaque, usbredir_host->control_endpoint_address,
                LIBUSB_ENDPOINT_IN, LIBUSB_REQUEST_GET_CONFIGURATION, 0, 0,
                USB_GET_CONFIGURATION_DATA_SIZE, NULL, 0);
            return send_interface_and_ep_info(usbredir_host->parser, data);
        } else {
            usbredir_host->request.requested_config_descriptor = true;
            usbredir_host->device_ops->control_transfer(
                usbredir_host->opaque, usbredir_host->control_endpoint_address,
                LIBUSB_ENDPOINT_IN, LIBUSB_REQUEST_GET_DESCRIPTOR,
                LIBUSB_DT_CONFIG << 8, 0, HS_USB_MAX_PACKET_SIZE, NULL, 0);
            return 0;
        }
    }

    struct usb_redir_set_configuration_header *set_config =
        (void *)usbredir_host->usbredir_header_cache;
    struct usb_redir_configuration_status_header config_status = {
        .status = set_config->configuration == *data ? usb_redir_success
                                                     : usb_redir_ioerror,
        .configuration = *data,
    };

    usbredirparser_send_configuration_status(
        usbredir_host->parser, usbredir_host->latest_packet_id, &config_status);

    if (usbredirparser_do_write(usbredir_host->parser) != 0) {
        return 0;
    }

    usbredir_host->request.active = false;
    return USB_GET_CONFIGURATION_DATA_SIZE;
}

int usbredir_host_control_transfer_complete(USBRedirectHost *usbredir_host,
                                            uint8_t *data, int data_len)
{
    if (!usbredir_host->request.active) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "BAD! You haven't received control transfer.");
        return 0;
    }

    switch (usbredir_host->request.request_type) {
    case usb_redir_device_connect:
        return usbredir_host_handle_device_connect(usbredir_host, data);
    case usb_redir_control_packet:
        return usbredir_host_send_control_packet(usbredir_host, data,
                                                 data_len);
    case usb_redir_set_configuration:
        return usbredir_host_handle_configuration_status(usbredir_host,
                                                         data);
    }
    return 0;
}

static void usbredir_host_parser_log(void *priv, int level, const char *msg)
{
    switch (level) {
    case usbredirparser_error:
        error_report("[ERROR] usbredir-host: %s", msg);
        break;
    case usbredirparser_warning:
        error_report("[WARNING] usbredir-host: %s", msg);
        break;
    case usbredirparser_info:
        error_report("[INFO] usbredir-host: %s", msg);
        break;
    default:
        qemu_log_mask(LOG_TRACE, "[DEBUG] usbredir-host: %s", msg);
    }
}

static int usbredir_host_parser_read(void *priv, uint8_t *data, int count)
{
    USBRedirectHost *usbredir_host = priv;

    if (!usbredir_host->read_cache) {
        return 0;
    }

    if (usbredir_host->read_size < count) {
        count = usbredir_host->read_size;
    }

    memcpy(data, usbredir_host->read_cache, count);

    usbredir_host->read_size -= count;
    if (usbredir_host->read_size) {
        usbredir_host->read_cache += count;
    } else {
        usbredir_host->read_cache = NULL;
    }

    return count;
}

static gboolean usbredir_host_parser_write_ready(void *_, GIOCondition cond,
                                                 void *opaque)
{
    USBRedirectHost *usbredir_host = opaque;
    usbredir_host->write_ready_watch = 0;
    usbredirparser_do_write(usbredir_host->parser);

    return FALSE;
}

static int usbredir_host_parser_write(void *priv, uint8_t *data, int count)
{
    USBRedirectHost *usbredir_host = priv;
    int written_bytes = 0;

    if (!qemu_chr_fe_backend_open(&usbredir_host->chr)) {
        return 0;
    }

    if (!runstate_check(RUN_STATE_RUNNING)) {
        return 0;
    }

    written_bytes = qemu_chr_fe_write(&usbredir_host->chr, data, count);

    if (written_bytes < count) {
        if (!usbredir_host->write_ready_watch) {
            usbredir_host->write_ready_watch = qemu_chr_fe_add_watch(
                &usbredir_host->chr, G_IO_OUT | G_IO_HUP,
                usbredir_host_parser_write_ready, usbredir_host);
        }
        if (written_bytes < 0) {
            written_bytes = 0;
        }
    }

    return written_bytes;
}

static void usbredir_host_parser_hello(void *priv,
                                       struct usb_redir_hello_header *h)
{
    USBRedirectHost *usbredir_host = priv;
    usbredir_host->device_ops->on_attach(usbredir_host->opaque);
}

static void usbredir_host_parser_reset(void *priv)
{
    USBRedirectHost *usbredir_host = priv;
    usbredir_host->device_ops->reset(usbredir_host->opaque);
}

static void usbredir_host_parser_control_transfer(
    void *priv, uint64_t id,
    struct usb_redir_control_packet_header *control_packet, uint8_t *data,
    int data_len)
{
    USBRedirectHost *usbredir_host = priv;

    if (USBREDIR_HEADER_CACHE_SIZE <
        sizeof(struct usb_redir_control_packet_header)) {
        g_autofree char *path = object_get_canonical_path(
            OBJECT(usbredir_host));
        error_report(
            "%s: usb_redir_control_packet_header overflowed request cache.",
            path);
        return;
    }

    usbredir_host->latest_packet_id = id;
    usbredir_host->request.request_type = usb_redir_control_packet;
    usbredir_host->request.active = true;
    usbredir_host->request.require_if_and_ep_info = false;
    memcpy(usbredir_host->usbredir_header_cache, control_packet,
           sizeof(struct usb_redir_control_packet_header));
    usbredir_host->device_ops->control_transfer(
        usbredir_host->opaque, usbredir_host->control_endpoint_address,
        control_packet->requesttype, control_packet->request,
        control_packet->value, control_packet->index, control_packet->length,
        data, data_len);
}

static void usbredir_host_parser_set_config(
    void *priv, uint64_t id,
    struct usb_redir_set_configuration_header *set_config)
{
    USBRedirectHost *usbredir_host = priv;

    if (USBREDIR_HEADER_CACHE_SIZE <
        sizeof(struct usb_redir_set_configuration_header)) {
        g_autofree char *path = object_get_canonical_path(
            OBJECT(usbredir_host));
        error_report(
            "%s: usb_redir_set_configuration_header overflowed request cache.",
            path);
        return;
    }

    usbredir_host->latest_packet_id = id;
    usbredir_host->request.active = true;
    usbredir_host->request.request_type = usb_redir_set_configuration;
    usbredir_host->request.require_if_and_ep_info = true;
    usbredir_host->request.requested_config_descriptor = false;
    memcpy(usbredir_host->usbredir_header_cache, set_config,
           sizeof(struct usb_redir_set_configuration_header));
    usbredir_host->device_ops->control_transfer(
        usbredir_host->opaque, usbredir_host->control_endpoint_address,
        LIBUSB_ENDPOINT_OUT, LIBUSB_REQUEST_SET_CONFIGURATION,
        set_config->configuration, 0, 0, NULL, 0);
}

static void usbredir_host_create_parser(USBRedirectHost *usbredir_host)
{
    uint32_t caps[USB_REDIR_CAPS_SIZE] = {
        0,
    };

    usbredir_host->parser = usbredirparser_create();

    if (!usbredir_host->parser) {
        g_autofree char *path = object_get_canonical_path(
            OBJECT(usbredir_host));
        error_report("%s: usbredirparser_create() failed", path);
        exit(1);
    }

    usbredir_host->parser->priv = usbredir_host;
    usbredir_host->parser->log_func = usbredir_host_parser_log;
    usbredir_host->parser->read_func = usbredir_host_parser_read;
    usbredir_host->parser->write_func = usbredir_host_parser_write;
    usbredir_host->parser->reset_func = usbredir_host_parser_reset;
    usbredir_host->parser->hello_func = usbredir_host_parser_hello;
    usbredir_host->parser->control_packet_func =
        usbredir_host_parser_control_transfer;
    usbredir_host->parser->set_configuration_func =
        usbredir_host_parser_set_config;

    usbredirparser_caps_set_cap(caps, usb_redir_cap_connect_device_version);
    usbredirparser_caps_set_cap(caps, usb_redir_cap_ep_info_max_packet_size);
    usbredirparser_caps_set_cap(caps, usb_redir_cap_64bits_ids);
    usbredirparser_caps_set_cap(caps, usb_redir_cap_32bits_bulk_length);
    usbredirparser_init(usbredir_host->parser, VERSION, caps,
                        USB_REDIR_CAPS_SIZE, usbredirparser_fl_usb_host);
    usbredirparser_do_write(usbredir_host->parser);
}

static void usbredir_host_destroy_parser(USBRedirectHost *usbredir_host)
{
    if (usbredir_host->parser) {
        usbredirparser_destroy(usbredir_host->parser);
    }
}

static void usbredir_host_chardev_event_handler(void *opaque,
                                                QEMUChrEvent event)
{
    switch (event) {
    case CHR_EVENT_OPENED:
        usbredir_host_create_parser(USB_REDIR_HOST(opaque));
        break;
    case CHR_EVENT_CLOSED:
        usbredir_host_destroy_parser(USB_REDIR_HOST(opaque));
    default:
        break;
    }
}

static void usbredir_host_chardev_read(void *opaque, const uint8_t *buf,
                                       int size)
{
    USBRedirectHost *usbredir_host = USB_REDIR_HOST(opaque);

    usbredir_host->read_cache = buf;
    usbredir_host->read_size = size;

    usbredirparser_do_read(usbredir_host->parser);
}

static int usbredir_host_can_read(void *opaque)
{
    USBRedirectHost *usbredir_host = USB_REDIR_HOST(opaque);

    if (!usbredir_host->parser) {
        return 0;
    }

    if (!runstate_check(RUN_STATE_RUNNING)) {
        return 0;
    }

    return 1 * MiB;
}

static void usbredir_host_realize(DeviceState *dev, Error **errp)
{
    USBRedirectHost *usbredir_host = USB_REDIR_HOST(dev);

    if (qemu_chr_fe_backend_connected(&usbredir_host->chr)) {
        qemu_chr_fe_set_handlers(&usbredir_host->chr, usbredir_host_can_read,
                                 usbredir_host_chardev_read,
                                 usbredir_host_chardev_event_handler, NULL,
                                 usbredir_host, NULL, true);
    } else {
        g_autofree char *path = object_get_canonical_path(
            OBJECT(dev));
        qemu_log_mask(LOG_TRACE, "%s: continuing without chardev", path);
    }
}

static void usbredir_host_unrealize(DeviceState *dev)
{
    USBRedirectHost *usbredir_host = USB_REDIR_HOST(dev);
    qemu_chr_fe_deinit(&usbredir_host->chr, true);
    usbredir_host_destroy_parser(usbredir_host);
}

static const Property usbredir_host_properties[] = {
    DEFINE_PROP_CHR("chardev", USBRedirectHost, chr),
};

static void usbredir_host_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_props(dc, usbredir_host_properties);
    dc->desc = "USB Redirect Host";
    dc->realize = usbredir_host_realize;
    dc->unrealize = usbredir_host_unrealize;
}

static const TypeInfo usbredir_host_info = {
    .name = TYPE_USB_REDIR_HOST,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(USBRedirectHost),
    .class_init = usbredir_host_class_init,
};

static void usbredir_host_register_type(void)
{
    type_register_static(&usbredir_host_info);
}

type_init(usbredir_host_register_type)

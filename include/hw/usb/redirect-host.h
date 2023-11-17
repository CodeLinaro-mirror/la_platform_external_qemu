/*
 * USB Redirector usb-host
 *
 * Copyright (C) 2023 Google, LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 */

#ifndef REDIRECT_HOST_H
#define REDIRECT_HOST_H

#include <usbredirparser.h>
#include <usbredirproto.h>

#include "chardev/char-fe.h"
#include "hw/qdev-core.h"
#include "qemu/typedefs.h"

#define TYPE_USB_REDIR_HOST "usbredir-host"
OBJECT_DECLARE_SIMPLE_TYPE(USBRedirectHost, USB_REDIR_HOST)

/* Callback functions defined by a USB device to handle usbredir event. */
typedef struct USBRedirectHostOps {
    void (*on_attach)(void *opaque);
} USBRedirectHostOps;

typedef struct USBRedirectHost {
    DeviceState parent_obj;
    CharBackend chr;

    const uint8_t *read_cache;
    int read_size;
    gint write_ready_watch;
    struct usbredirparser *parser;
    const USBRedirectHostOps *device_ops;
    void *opaque;
} USBRedirectHost;

/**
 * usbredir_host_set_ops - Sets callback functions that controls a USB device.
 * @usbredir_host - A usbredir host to connect to a USB device
 * @device_ops - Callback functions from the USB device
 * @opaque - The USB device
 */
static inline void usbredir_host_set_ops(USBRedirectHost *usbredir_host,
                                         const USBRedirectHostOps *device_ops,
                                         void *opaque)
{
    usbredir_host->device_ops = device_ops;
    usbredir_host->opaque = opaque;
}

/**
 * usbredir_host_send_ep_info - Sends an endpoint descriptor with usbredir
 * protocol
 * @usbredir_host - A usbredir host to send endpoint descriptor with.
 * @ep_info - The endpoint descriptor
 *
 * Returns 0 on success.
 */
static inline int usbredir_host_send_ep_info(
    USBRedirectHost *usbredir_host, struct usb_redir_ep_info_header *ep_info)
{
    usbredirparser_send_ep_info(usbredir_host->parser, ep_info);
    return usbredirparser_do_write(usbredir_host->parser);
}

/**
 * usbredir_host_send_interface_info - Sends an interface descriptor with
 * usbredir protocol
 * @usbredir_host - A usbredir host to send interface descriptor with.
 * @interface_info - The interface descriptor
 *
 * Returns 0 on success.
 */
static inline int usbredir_host_send_interface_info(
    USBRedirectHost *usbredir_host,
    struct usb_redir_interface_info_header *interface_info)
{
    usbredirparser_send_interface_info(usbredir_host->parser, interface_info);
    return usbredirparser_do_write(usbredir_host->parser);
}

/**
 * usbredir_host_send_device_connect - Sends a device descriptor with usbredir
 * protocol
 * @usbredir_host - A usbredir host to send device descriptor with.
 * @device_info - The device descriptor
 *
 * Returns 0 on success.
 */
static inline int usbredir_host_send_device_connect(
    USBRedirectHost *usbredir_host,
    struct usb_redir_device_connect_header *device_info)
{
    usbredirparser_send_device_connect(usbredir_host->parser, device_info);
    return usbredirparser_do_write(usbredir_host->parser);
}

#endif /* REDIRECT_HOST_H */

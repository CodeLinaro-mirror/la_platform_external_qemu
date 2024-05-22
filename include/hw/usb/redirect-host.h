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
#include "qemu/log.h"
#include "qemu/queue.h"
#include "qemu/typedefs.h"

#define TYPE_USB_REDIR_HOST "usbredir-host"
OBJECT_DECLARE_SIMPLE_TYPE(USBRedirectHost, USB_REDIR_HOST)

/* Callback functions defined by a USB device to handle usbredir event. */
typedef struct USBRedirectHostOps {
    uint8_t (*on_attach)(void *opaque);
    void (*on_detach)(void *opaque);
    void (*reset)(void *opaque);
    void (*control_transfer)(void *opaque, uint8_t endpoint_address,
                             uint8_t request_type, uint8_t request,
                             uint16_t value, uint16_t index, uint16_t length,
                             uint8_t *data, int data_len);
    void (*data_out)(void *opaque, uint8_t endpoint_address, uint8_t *data,
                     int data_len);
} USBRedirectHostOps;

typedef struct UsbRedirRequest {
    bool require_if_and_ep_info;
    bool requested_config_descriptor;
    int request_type;
    bool active;
} UsbRedirRequest;

typedef struct BulkData {
    uint8_t *data;
    int data_len;
    QTAILQ_ENTRY(BulkData) next;
} BulkData;
typedef QTAILQ_HEAD(BulkDataQueue, BulkData) BulkDataQueue;

typedef struct BulkHeader {
    struct usb_redir_bulk_packet_header header;
    uint64_t id;
    QTAILQ_ENTRY(BulkHeader) next;
} BulkHeader;
typedef QTAILQ_HEAD(BulkHeaderQueue, BulkHeader) BulkHeaderQueue;
#define USBREDIR_HEADER_CACHE_SIZE 10

typedef struct USBRedirectHost {
    DeviceState parent_obj;
    CharBackend chr;

    const uint8_t *read_cache;
    int read_size;
    gint write_ready_watch;

    struct usbredirparser *parser;
    uint64_t latest_packet_id;
    const USBRedirectHostOps *device_ops;
    void *opaque;
    uint8_t control_endpoint_address;

    UsbRedirRequest request;
    uint8_t usbredir_header_cache[USBREDIR_HEADER_CACHE_SIZE];

    /* For bulk transfer. */
    BulkDataQueue bulk_in_data_cache;
    BulkDataQueue bulk_out_data_cache;
    BulkHeaderQueue bulk_in_header_cache;
    BulkHeaderQueue bulk_out_header_cache;
} USBRedirectHost;

/**
 * usbredir_host_attach_complete - Notify usbredir host that the attach workflow
 *   has completed.
 * @usbredir_host - The usbredir host to be notified
 */
void usbredir_host_attach_complete(USBRedirectHost *usbredir_host);

/**
 * usbredir_host_control_transfer_complete - Notify usbredir host that the
 *   control transfer has completed and send control data if any.
 * @usbredir_host - The usbredir host to be notified
 * @data - The pointer to the control data
 * @data_len - The length of the control data
 */
int usbredir_host_control_transfer_complete(USBRedirectHost *usbredir_host,
                                            uint8_t *data, int data_len);

/**
 * usbredir_host_data_in_complete - Send data to the usbredir host.
 * @usbredir_host - The usbredir host receiving the data
 * @data - The pointer to the data
 * @data_len - The length of the data
 */
int usbredir_host_data_in_complete(USBRedirectHost *usbredir_host,
                                   uint8_t *data, int data_len);

/**
 * usbredir_host_data_out_complete - Notify the usbredir host that the data
 *   written to the OUT endpoint has completed.
 * @usbredir_host - The usbredir host to be notified
 * @endpoint_address - The endpoint that is notifying
 */
int usbredir_host_data_out_complete(USBRedirectHost *usbredir_host,
                                    uint8_t endpoint_address);

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

#endif /* REDIRECT_HOST_H */

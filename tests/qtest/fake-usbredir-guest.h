/*
 * Fake USB Redirect Guest
 *
 * Copyright (C) 2024 Google, LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 */

#ifndef FAKE_USBREDIR_GUEST_H
#define FAKE_USBREDIR_GUEST_H

#include <libusb-1.0/libusb.h>
#include <semaphore.h>
#include <usbredirparser.h>
#include <usbredirproto.h>

#include "qemu/queue.h"

typedef struct DataPacket {
    uint64_t id;
    bool canceled;
    bool done;
    uint8_t *data;
    uint16_t data_length;
} DataPacket;

typedef struct ControlDataPacket {
    DataPacket packet;
    struct usb_redir_control_packet_header header;
} ControlDataPacket;

typedef struct BulkDataPacket {
    DataPacket packet;
    struct usb_redir_bulk_packet_header header;
    QTAILQ_ENTRY(BulkDataPacket) next;
} BulkDataPacket;
typedef QTAILQ_HEAD(BulkDataPacketQueue, BulkDataPacket) BulkDataPacketQueue;

typedef struct FakeUsbredirGuest {
    struct usbredirparser *parser;
    int fd;
    uint64_t packet_id;

    GThread *parser_thread;
    GMutex flag_mu;

    /* States */
    sem_t helloed_sem;
    sem_t if_info_sem;
    bool received_if_info;
    struct usb_redir_interface_info_header if_info;
    sem_t ep_info_sem;
    bool received_ep_info;
    struct usb_redir_ep_info_header ep_info;
    bool device_connected;
    sem_t device_info_sem;
    struct usb_redir_device_connect_header device_info;
    sem_t control_transfer_sem;
    ControlDataPacket *control_data_packet;
    sem_t bulk_transfer_sem;
    BulkDataPacketQueue bulk_data_packet_queue;
    uint8_t configuration_value;
    uint8_t interface_num;
    uint8_t alt_setting;
} FakeUsbredirGuest;

/**
 * fake_usbredir_guest_init:
 * @faker: Fake usbredir guest.
 * @fd: File descriptor for the socket connection.
 * Initializes the fake usbredir guest.
 */
void fake_usbredir_guest_init(FakeUsbredirGuest *faker, int fd);

/**
 * fake_usbredir_guest_start:
 * @faker: Fake usbredir guest.
 * Starts the fake usbredir guest.
 */
void fake_usbredir_guest_start(FakeUsbredirGuest *faker);

/**
 * fake_usbredir_guest_stop:
 * @faker: Fake usbredir guest.
 * Stops the fake usbredir guest.
 */
void fake_usbredir_guest_stop(FakeUsbredirGuest *faker);

/**
 * fake_usbredir_guest_helloed:
 * @faker: Fake usbredir guest.
 * Returns true if the fake usbredir guest has received hello packet. Otherwise,
 * returns false. This function should only be called once after the fake object
 * has started.
 */
bool fake_usbredir_guest_helloed(FakeUsbredirGuest *faker);

/**
 * fake_usbredir_guest_assert_num_interfaces:
 * @faker: Fake usbredir guest.
 * @expected_num_interfaces: Expected number of interfaces.
 * Assert the actual number of interfaces advertised to the fake usbredir guest
 * with the expected number of interfaces.
 */
void fake_usbredir_guest_assert_num_interfaces(FakeUsbredirGuest *faker,
                                               int expected_num_interfaces);

/**
 * fake_usbredir_guest_assert_contains_interface:
 * @faker: Fake usbredir guest.
 * @if_desc: Expected interface descriptor.
 * Assert the fake usbredir guest contains the expected interface descriptor.
 */
void fake_usbredir_guest_assert_contains_interface(
    FakeUsbredirGuest *faker,
    const struct libusb_interface_descriptor *if_desc);

/**
 * fake_usbredir_guest_assert_num_endpoints:
 * @faker: Fake usbredir guest.
 * @num_endpoints: Expected number of endpoints.
 * Assert the actual number of endpoints advertised to the fake usbredir guest
 * with the expected number of endpoints.
 */
void fake_usbredir_guest_assert_num_endpoints(FakeUsbredirGuest *faker,
                                              int expected_num_endpoints);

/**
 * fake_usbredir_guest_assert_contains_endpoint:
 * @faker: Fake usbredir guest.
 * @ep_desc: Expected endpoint descriptor.
 * Assert the fake usbredir guest contains the expected endpoint descriptor with
 * the corresponding interface number.
 */
void fake_usbredir_guest_assert_contains_endpoint(
    FakeUsbredirGuest *faker, const struct libusb_endpoint_descriptor *ep_desc,
    uint8_t interface_num);

/**
 * fake_usbredir_guest_assert_device:
 * @faker: Fake usbredir guest.
 * @device_desc: Expected device descriptor.
 * Assert the fake usbredir guest has the expected device descriptor.
 */
void fake_usbredir_guest_assert_device(
    FakeUsbredirGuest *faker,
    const struct libusb_device_descriptor *device_desc);

/**
 * fake_usbredir_guest_control_transfer:
 * @faker: Fake usbredir guest.
 * @request_type: USB control transfer request type.
 * @request: USB control transfer request.
 * @value: USB control transfer value.
 * @index: USB control transfer index.
 * @data: USB control transfer data.
 * @length: USB control transfer length.
 * Send the control transfer from the fake usbredir guest to the connected
 * device.
 */
void fake_usbredir_guest_control_transfer(FakeUsbredirGuest *faker,
                                          uint8_t request_type, uint8_t request,
                                          uint16_t value, uint16_t index,
                                          unsigned char *data, uint16_t length);

/**
 * fake_usbredir_guest_bulk_transfer:
 * @faker: Fake usbredir guest.
 * @endpoint: Device endpoint to read/write to.
 * @data: Data to be read/written.
 * @length: Length of the buffer or the data to be written.
 * Send the bulk transfer from the fake usbredir guest to the connected device.
 */
void fake_usbredir_guest_bulk_transfer(FakeUsbredirGuest *faker,
                                       uint8_t endpoint, uint8_t *data,
                                       uint32_t length);

/**
 * fake_usbredir_guest_assert_control_transfer_data:
 * @faker: Fake usbredir guest.
 * @data: Expected control transfer data.
 * @length: Expected data length.
 * Assert the fake usbredir guest has received the control transfer data.
 */
void fake_usbredir_guest_assert_control_transfer_received(
    FakeUsbredirGuest *faker, uint8_t *data, uint16_t length);

/**
 * fake_usbredir_guest_assert_bulk_transfer:
 * @faker: Fake usbredir guest.
 * @data: Expected bulk transfer data to be received by the fake.
 * @length: Expected data length to be received by the fake.
 * Assert the fake usbredir guest has received the expected bulk transfer data.
 * After bulk transfer read, this must be called before the next bulk transfer
 * read in unit testing. Otherwise, the fake usbredir guest will exit because of
 * unchecked data.
 */
void fake_usbredir_guest_assert_bulk_transfer(FakeUsbredirGuest *faker,
                                              uint8_t *data, uint32_t length);

/**
 * fake_usbredir_guest_set_configuration:
 * @faker: Fake usbredir guest.
 * @configuration_num: The configuration number to be set.
 * Sets the test USB device configuration number from the fake usbredir guest.
 */
void fake_usbredir_guest_set_configuration(FakeUsbredirGuest *faker,
                                           uint8_t configuration_value);

/**
 * fake_usbredir_guest_set_alt_interface:
 * @faker: Fake usbredir guest.
 * @interface_num: The interface number to be set.
 * @alt_setting: The alternative setting of ther interface.
 * Sets the test USB device interface from the fake usbredir guest.
 */
void fake_usbredir_guest_set_alt_interface(FakeUsbredirGuest *faker,
                                           uint8_t interface_num,
                                           uint8_t alt_setting);

/**
 * fake_usbredir_guest_cancel_data_packet:
 * @faker: Fake usbredir guest.
 * Cancels the most recently sent data packet.
 */
void fake_usbredir_guest_cancel_transfer(FakeUsbredirGuest *faker);

#endif /* FAKE_USBREDIR_GUEST_H */

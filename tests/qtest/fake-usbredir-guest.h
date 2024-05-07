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

typedef struct FakeUsbredirGuest {
    struct usbredirparser *parser;
    int fd;

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
    sem_t device_info_sem;
    struct usb_redir_device_connect_header device_info;
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

#endif /* FAKE_USBREDIR_GUEST_H */

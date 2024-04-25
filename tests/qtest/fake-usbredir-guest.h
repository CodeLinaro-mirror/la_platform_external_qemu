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

#include <usbredirparser.h>

typedef struct FakeUsbredirGuest {
    struct usbredirparser *parser;
    int fd;

    GThread *parser_thread;

    /* States. */
    bool helloed;
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
 * Returns true if the fake usbredir guest has received hello packet.
 * Otherwise, return false.
 */
bool fake_usbredir_guest_helloed(FakeUsbredirGuest *faker);

#endif /* FAKE_USBREDIR_GUEST_H */

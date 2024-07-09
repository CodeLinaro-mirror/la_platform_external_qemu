/*
 * SBTSI I3C target device.
 * Based on PPR Vol 5 for AMD Family 1Ah Model 02h C0 (9.2 SB-TSI Protocol)
 *
 * Copyright (c) 2024 Google LLC
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef SBTSI_I3C_H_
#define SBTSI_I3C_H_

#include "hw/i3c/i3c.h"
#include "hw/sensor/sbtsi.h"

#define TYPE_SBTSI_I3C_TARGET "sbtsi-i3c-target"
OBJECT_DECLARE_SIMPLE_TYPE(SbtsiI3cTargetState, SBTSI_I3C_TARGET)


struct SbtsiI3cTargetState {
    I3CTarget i3c;

    /* General device state */
    I3CEvent curr_event;
    uint8_t command_code;
    bool command_code_received;

    /* SBTSI temperature sensor */
    SBTSIState sbtsi;

    char *name;
};


#endif  /* SBTSI_I3C_H_ */

/*
 * SBTSI I3C target device.
 * Based on PPR Vol 5 for AMD Family 1Ah Model 02h C0 (9.2 SB-TSI Protocol)
 *
 * Copyright (c) 2024 Google LLC
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

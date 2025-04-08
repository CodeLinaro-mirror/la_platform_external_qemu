/*
 * MIPI HCI I3C controller DCT functionality
 *
 * Copyright (C) 2025 Google, LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HCI_DCT_H
#define HCI_DCT_H

REG32(TARGET_DCT_0, 0x00)
REG32(TARGET_DCT_1, 0X04)
      FIELD(TARGET_DCT_1, TARGET_PID_LO, 0, 16)
REG32(TARGET_DCT_2, 0x08)
    FIELD(TARGET_DCT_2, TARGET_DCR, 0, 8)
    FIELD(TARGET_DCT_2, TARGET_BCR, 8, 8)
REG32(TARGET_DCT_3, 0x0c)
    FIELD(TARGET_DCT_3, TARGET_DYNAMIC_ADDRESS, 0, 8)

#endif  /* HCI_DCT_H */

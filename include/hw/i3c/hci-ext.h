/*
 * MIPI HCI I3C controller extended capabilities
 *
 * Copyright (C) 2025 Google, LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef I3C_HCI_EXT_H_
#define I3C_HCI_EXT_H_

#include "system/memory.h"

typedef struct HCIExtCapState {
  uint32_t num_ext_capabilities;
  uint32_t *ext_capabilities;

  MemoryRegion mmio;
} HCIExtCapState;

#endif  /* I3C_HCI_EXT_H_ */

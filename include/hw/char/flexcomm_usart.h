/*
 * QEMU model for NXP's FLEXCOMM USART
 *
 * Copyright (c) 2024 Google LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_FLEXCOMM_USART_H
#define HW_FLEXCOMM_USART_H

#include "hw/misc/flexcomm_function.h"
#include "chardev/char-fe.h"

#define TYPE_FLEXCOMM_USART "flexcomm-usart"
OBJECT_DECLARE_TYPE(FlexcommUsartState, FlexcommUsartClass, FLEXCOMM_USART);

struct FlexcommUsartState {
    FlexcommFunction parent_obj;

    CharFrontend chr;
};

struct FlexcommUsartClass {
    FlexcommFunctionClass parent_obj;

    FlexcommFunctionSelect select;
};

#endif /* HW_FLEXCOMM_USART_H */

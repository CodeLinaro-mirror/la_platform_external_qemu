/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <stdint.h>

#include "android/utils/compiler.h"

ANDROID_BEGIN_HEADER

/* Merges the updated framebuffer region (x, y, w, h) into the accumulated
 * dirty region (*acc_x, *acc_y, *acc_w, *acc_h), and clamps the result to the
 * current display bounds [0, disp_w) x [0, disp_h).
 *
 * An accumulated region with *acc_w == 0 && *acc_h == 0 is treated as empty
 * and is replaced by the (clamped) update region.
 *
 * Clamping is required because updates are accumulated while a previous frame
 * transfer is in flight. If the display geometry changes in between (e.g.
 * rotation or a foldable resize), the union of the old and new regions can
 * exceed the new framebuffer, which would make the JPEG compressor read past
 * the end of the framebuffer.
 *
 * If the clamped region is empty, all four outputs are set to 0. */
static inline void mt_fb_merge_dirty_rect(int* acc_x,
                                          int* acc_y,
                                          int* acc_w,
                                          int* acc_h,
                                          int x,
                                          int y,
                                          int w,
                                          int h,
                                          int disp_w,
                                          int disp_h) {
    /* Use 64-bit arithmetic so that x + w and y + h cannot overflow. */
    int64_t left = x;
    int64_t top = y;
    int64_t right = (int64_t)x + w;
    int64_t bottom = (int64_t)y + h;

    if (*acc_w != 0 || *acc_h != 0) {
        const int64_t acc_right = (int64_t)*acc_x + *acc_w;
        const int64_t acc_bottom = (int64_t)*acc_y + *acc_h;
        if (*acc_x < left) {
            left = *acc_x;
        }
        if (*acc_y < top) {
            top = *acc_y;
        }
        if (acc_right > right) {
            right = acc_right;
        }
        if (acc_bottom > bottom) {
            bottom = acc_bottom;
        }
    }

    if (left < 0) {
        left = 0;
    }
    if (top < 0) {
        top = 0;
    }
    if (right > disp_w) {
        right = disp_w;
    }
    if (bottom > disp_h) {
        bottom = disp_h;
    }

    if (right <= left || bottom <= top) {
        *acc_x = *acc_y = *acc_w = *acc_h = 0;
        return;
    }

    *acc_x = (int)left;
    *acc_y = (int)top;
    *acc_w = (int)(right - left);
    *acc_h = (int)(bottom - top);
}

ANDROID_END_HEADER

// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "android/multitouch-fb-rect.h"

#include <gtest/gtest.h>

#include <climits>

namespace {

struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

void merge(Rect* acc, const Rect& upd, int dispW, int dispH) {
    mt_fb_merge_dirty_rect(&acc->x, &acc->y, &acc->w, &acc->h, upd.x, upd.y,
                           upd.w, upd.h, dispW, dispH);
}

void expectRect(const Rect& r, int x, int y, int w, int h) {
    EXPECT_EQ(x, r.x);
    EXPECT_EQ(y, r.y);
    EXPECT_EQ(w, r.w);
    EXPECT_EQ(h, r.h);
}

}  // namespace

TEST(MultitouchFbRect, EmptyAccumulatorTakesUpdate) {
    Rect acc;
    merge(&acc, {10, 20, 30, 40}, 100, 100);
    expectRect(acc, 10, 20, 30, 40);
}

TEST(MultitouchFbRect, UnionsWithinBounds) {
    Rect acc{10, 10, 10, 10};
    merge(&acc, {50, 60, 10, 10}, 100, 100);
    expectRect(acc, 10, 10, 50, 60);
}

// Regression test: landscape frame pending while the display rotates to
// portrait. The unclamped union would be 1920x1920 over a 1080x1920 buffer.
TEST(MultitouchFbRect, ClampsUnionAfterRotation) {
    Rect acc{0, 0, 1920, 1080};
    merge(&acc, {0, 0, 1080, 1920}, 1080, 1920);
    expectRect(acc, 0, 0, 1080, 1920);
}

TEST(MultitouchFbRect, ClampsAccumulatorAfterShrink) {
    Rect acc{100, 100, 900, 900};
    merge(&acc, {0, 0, 10, 10}, 500, 400);
    expectRect(acc, 0, 0, 500, 400);
}

TEST(MultitouchFbRect, ClampsNegativeOrigin) {
    Rect acc;
    merge(&acc, {-5, -7, 20, 20}, 100, 100);
    expectRect(acc, 0, 0, 15, 13);
}

TEST(MultitouchFbRect, FullyOutOfBoundsBecomesEmpty) {
    Rect acc;
    merge(&acc, {200, 200, 10, 10}, 100, 100);
    expectRect(acc, 0, 0, 0, 0);
}

TEST(MultitouchFbRect, ZeroDisplayBecomesEmpty) {
    Rect acc{0, 0, 10, 10};
    merge(&acc, {0, 0, 10, 10}, 0, 0);
    expectRect(acc, 0, 0, 0, 0);
}

TEST(MultitouchFbRect, NoIntegerOverflow) {
    Rect acc{INT_MAX - 1, INT_MAX - 1, INT_MAX, INT_MAX};
    merge(&acc, {INT_MAX, INT_MAX, INT_MAX, INT_MAX}, 640, 480);
    // Entire region lies outside [0, 640) x [0, 480).
    expectRect(acc, 0, 0, 0, 0);
}

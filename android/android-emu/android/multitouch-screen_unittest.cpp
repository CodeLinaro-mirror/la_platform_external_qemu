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
#include "android/multitouch-screen.h"

#include <gtest/gtest.h>

#include <climits>
#include <cstring>
#include <thread>
#include <vector>

#include "android/base/testing/TestLooper.h"
#include "android/utils/looper.h"

namespace android {
namespace {

void dummyGetFrameBuffer(int* w,
                         int* h,
                         int* lineSize,
                         int* bytesPerPixel,
                         uint8_t** frameBufferData) {
    if (w)
        *w = 4;
    if (h)
        *h = 4;
    if (lineSize)
        *lineSize = 16;
    if (bytesPerPixel)
        *bytesPerPixel = 4;
    if (frameBufferData)
        *frameBufferData = nullptr;
}

void dummyRegisterUpdateListener(AndroidDisplayUpdateCallback, void*) {}

TEST(MultitouchScreenTest, OpenGlesFbUpdateStagesAndDispatchesToMainLooper) {
    static android::base::TestLooper* sLooper = new android::base::TestLooper();
    android_registerMainLooper(reinterpret_cast<::Looper*>(
            static_cast<android::base::Looper*>(sLooper)));

    QAndroidUserEventAgent userAgent = {};
    QAndroidDisplayAgent displayAgent = {};
    displayAgent.getFrameBuffer = dummyGetFrameBuffer;
    displayAgent.registerUpdateListener = dummyRegisterUpdateListener;

    multitouch_init(nullptr, &userAgent, &displayAgent);
    EXPECT_TRUE(sLooper->activeTimers().empty());

    std::vector<unsigned char> pixels(4 * 4 * 4, 0xAB);

    // Invalid parameters and integer-overflowing widths must be rejected.
    multitouch_opengles_fb_update(nullptr, 0, 0, 4, -1, 0, 0, pixels.data());
    multitouch_opengles_fb_update(nullptr, 0, 4, -1, -1, 0, 0, pixels.data());
    multitouch_opengles_fb_update(nullptr, 0, (INT_MAX / 4) + 1, 1, -1, 0, 0,
                                  pixels.data());
    multitouch_opengles_fb_update(nullptr, 0, 4, 4, -1, 0, 0, nullptr);
    EXPECT_TRUE(sLooper->activeTimers().empty());

    // Valid update from a separate render thread stages the pixel buffer and
    // schedules the main-looper timer even if the caller immediately overwrites
    // the transient buffer.
    std::thread renderThread([&pixels]() {
        multitouch_opengles_fb_update(nullptr, 0, 4, 4, -1, 0, 0,
                                      pixels.data());
        std::memset(pixels.data(), 0, pixels.size());
    });
    renderThread.join();

    EXPECT_EQ(1U, sLooper->activeTimers().size());
    sLooper->runOneIterationWithDeadlineMs(sLooper->nowMs() + 10);
    EXPECT_TRUE(sLooper->activeTimers().empty());

    android_registerMainLooper(nullptr);
}

}  // namespace
}  // namespace android

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
#include "android/gps.h"

#include <gtest/gtest.h>

#include "android/emulation/testing/TestVmLock.h"

namespace android_gps {

TEST(GpsVmLock, SendAndRefreshAcquireVmLock) {
    android::VmLock* prev = android::VmLock::set(nullptr);
    EXPECT_FALSE(android::VmLock::hasInstance());
    android_gps_refresh();
    EXPECT_FALSE(android::VmLock::hasInstance());
    android::VmLock::set(prev);

    android::TestVmLock testLock;
    EXPECT_EQ(0, testLock.mLockCount);
    EXPECT_EQ(0, testLock.mUnlockCount);

    android_gps_send_nmea("$GPGGA,test");
    EXPECT_EQ(1, testLock.mLockCount);
    EXPECT_EQ(1, testLock.mUnlockCount);

    android_gps_send_gnss("test_gnss");
    EXPECT_EQ(2, testLock.mLockCount);
    EXPECT_EQ(2, testLock.mUnlockCount);

    android_gps_refresh();
    EXPECT_EQ(3, testLock.mLockCount);
    EXPECT_EQ(3, testLock.mUnlockCount);
}

}  // namespace android_gps

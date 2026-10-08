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
#include "android/car.h"

#include <gtest/gtest.h>

#include "android/car-cluster.h"
#include "android/emulation/testing/TestVmLock.h"

namespace android {

TEST(CarVmLock, SendCarAndClusterDataAcquireVmLock) {
    VmLock* prev = VmLock::set(nullptr);
    EXPECT_FALSE(VmLock::hasInstance());
    set_car_call_back(nullptr, nullptr);
    set_car_cluster_call_back(nullptr);
    EXPECT_FALSE(VmLock::hasInstance());
    VmLock::set(prev);

    TestVmLock testLock;
    EXPECT_EQ(0, testLock.mLockCount);
    EXPECT_EQ(0, testLock.mUnlockCount);

    const char kDummyMsg[] = "test";
    android_send_car_data(kDummyMsg, sizeof(kDummyMsg));
    EXPECT_EQ(1, testLock.mLockCount);
    EXPECT_EQ(1, testLock.mUnlockCount);

    set_car_call_back(nullptr, nullptr);
    EXPECT_EQ(2, testLock.mLockCount);
    EXPECT_EQ(2, testLock.mUnlockCount);

    android_send_car_cluster_data(reinterpret_cast<const uint8_t*>(kDummyMsg),
                                  sizeof(kDummyMsg));
    EXPECT_EQ(3, testLock.mLockCount);
    EXPECT_EQ(3, testLock.mUnlockCount);

    set_car_cluster_call_back(nullptr);
    EXPECT_EQ(4, testLock.mLockCount);
    EXPECT_EQ(4, testLock.mUnlockCount);
}

}  // namespace android

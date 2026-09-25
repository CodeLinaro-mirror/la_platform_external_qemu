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
#include "android/emulation/android_qemud.h"

#include <gtest/gtest.h>

#include "android/emulation/testing/TestVmLock.h"

namespace android {

TEST(QemudVmLock, SendAndBroadcastAcquireVmLock) {
    TestVmLock myLock;
    EXPECT_EQ(0, myLock.mLockCount);
    EXPECT_EQ(0, myLock.mUnlockCount);

    const uint8_t dummy = 0;
    qemud_client_send(nullptr, &dummy, 1);
    EXPECT_EQ(1, myLock.mLockCount);
    EXPECT_EQ(1, myLock.mUnlockCount);

    qemud_service_broadcast(nullptr, &dummy, 1);
    EXPECT_EQ(2, myLock.mLockCount);
    EXPECT_EQ(2, myLock.mUnlockCount);
}

}  // namespace android

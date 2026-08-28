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

#include <gtest/gtest.h>
#include <vector>

#include "aemu/base/EventNotificationSupport.h"
#include "android/console.h"
#include "host-common/MultiDisplay.h"
#include "host-common/multi_display_agent.h"

namespace android {
namespace emulation {

class MultiDisplayTest : public ::testing::Test {
protected:
    void SetUp() override {
        mAgent = getConsoleAgents()->multi_display;
        ASSERT_NE(nullptr, mAgent);

        // Ensure a test display exists
        mAgent->createDisplay(&mDisplayId);
    }

    const QAndroidMultiDisplayAgent* mAgent = nullptr;
    uint32_t mDisplayId = 0;
};

TEST_F(MultiDisplayTest, GetAndSetDisplayPowerMode) {
    uint32_t mode = 0;
    EXPECT_EQ(0, mAgent->getDisplayPowerMode(mDisplayId, &mode));
    EXPECT_EQ(static_cast<uint32_t>(DisplayPowerMode::ON), mode);

    EXPECT_EQ(0, mAgent->setDisplayPowerMode(
                         mDisplayId,
                         static_cast<uint32_t>(DisplayPowerMode::OFF)));
    EXPECT_EQ(0, mAgent->getDisplayPowerMode(mDisplayId, &mode));
    EXPECT_EQ(static_cast<uint32_t>(DisplayPowerMode::OFF), mode);

    EXPECT_EQ(0, mAgent->setDisplayPowerMode(
                         mDisplayId,
                         static_cast<uint32_t>(DisplayPowerMode::DOZE)));
    EXPECT_EQ(0, mAgent->getDisplayPowerMode(mDisplayId, &mode));
    EXPECT_EQ(static_cast<uint32_t>(DisplayPowerMode::DOZE), mode);

    // Invalid power mode value should fail
    EXPECT_NE(0, mAgent->setDisplayPowerMode(mDisplayId, 999));

    // Invalid displayId should fail
    EXPECT_NE(0, mAgent->setDisplayPowerMode(9999, 0));
}

TEST_F(MultiDisplayTest, DisplayPowerModeEventListener) {
    auto listenerSupport = static_cast<DisplayPowerModeNotificationSupport*>(
            mAgent->getDisplayPowerModeEventListener());
    ASSERT_NE(nullptr, listenerSupport);

    std::vector<DisplayPowerModeChangeEvent> receivedEvents;
    {
        base::RaiiEventListener<DisplayPowerModeNotificationSupport,
                                DisplayPowerModeChangeEvent>
                listener(listenerSupport,
                         [&](const DisplayPowerModeChangeEvent evt) {
                             receivedEvents.push_back(evt);
                         });

        EXPECT_EQ(0,
                  mAgent->setDisplayPowerMode(
                          mDisplayId, static_cast<uint32_t>(
                                              DisplayPowerMode::DOZE_SUSPEND)));

        ASSERT_EQ(1u, receivedEvents.size());
        EXPECT_EQ(mDisplayId, receivedEvents[0].displayId);
        EXPECT_EQ(DisplayPowerMode::DOZE_SUSPEND, receivedEvents[0].powerMode);

        EXPECT_EQ(0, mAgent->setDisplayPowerMode(
                             mDisplayId,
                             static_cast<uint32_t>(DisplayPowerMode::ON)));

        ASSERT_EQ(2u, receivedEvents.size());
        EXPECT_EQ(mDisplayId, receivedEvents[1].displayId);
        EXPECT_EQ(DisplayPowerMode::ON, receivedEvents[1].powerMode);

        // Setting the same power mode again should not fire an additional event
        EXPECT_EQ(0, mAgent->setDisplayPowerMode(
                             mDisplayId,
                             static_cast<uint32_t>(DisplayPowerMode::ON)));

        EXPECT_EQ(2u, receivedEvents.size());
    }
}

}  // namespace emulation
}  // namespace android

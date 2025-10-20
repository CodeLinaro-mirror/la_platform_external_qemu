// Copyright (C) 2025 The Android Open Source Project
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

#include "android/skin/qt/extended-pages/battery-page.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QSlider>
#include <chrono>
#include <thread>
// #include <QTest>

#include "android/cmdline-definitions.h"
#include "host-common/hw-config.h"
#include "android/console.h"
#include "android/emulation/control/battery_agent.h"
#include "android/skin/qt/extended-pages/battery-controller.h"
#include "android/skin/qt/qt-settings.h"
#include "ui_battery-page.h"

using ::testing::_;
using ::testing::Field;
using ::testing::SaveArg;

// MockBatteryController to verify interactions with the backend.
class MockBatteryController : public BatteryController {
public:
    MOCK_METHOD(void, setBattery, (const BatteryState& state), (override));
};

class BatteryPageTest : public ::testing::Test {
protected:
    void SetUp() override {
        // This is required for any QWidget-based unit test.
        if (!QCoreApplication::instance()) {
            static int argc = 1;
            static char* argv[] = {(char*)"test"};
            new QApplication(argc, argv);
        }

        // Isolate QSettings to a test-specific file.
        QCoreApplication::setOrganizationName("GoogleTest");
        QCoreApplication::setApplicationName("BatteryPageTest");

        // Create the page and inject the mock controller.
        AndroidHwConfig& hwCfg = *getConsoleAgents()->settings->hw();
        hwCfg.hw_battery = true;
        // getConsoleAgents()->settings->hw()->hw_battery = true;
        m_page = std::make_unique<BatteryPage>();
        auto mockController = std::make_unique<MockBatteryController>();
        m_mockController = mockController.get();
        m_page->setControllerForTest(std::move(mockController));
    }

    void TearDown() override { m_page.reset(); }

    // Helper to find a widget by its object name.
    template <typename T>
    T* find(const QString& name) {
        return m_page->findChild<T*>(name);
    }

    std::unique_ptr<BatteryPage> m_page;
    MockBatteryController* m_mockController;

private:
    // Mock AndroidHardware and other dependencies needed by the page.
    QAndroidBatteryAgent m_battery_agent = {};
};

// Test that changing the charge level slider updates the state and calls the
// controller.
TEST_F(BatteryPageTest, SliderUpdatesChargeLevel) {
    EXPECT_CALL(*m_mockController, setBattery(_));
    m_page->show();

    BatteryState capturedState;
    EXPECT_CALL(*m_mockController, setBattery(_))
            .WillOnce(SaveArg<0>(&capturedState));

    find<QSlider>("bat_levelSlider")->setValue(75);

    ASSERT_EQ(capturedState.chargeLevel, 75);
}

// Test that changing the charger combo box updates the state and calls the
// controller.
TEST_F(BatteryPageTest, ChargerBoxUpdatesCharger) {
    EXPECT_CALL(*m_mockController, setBattery(_));
    m_page->show();

    BatteryState capturedState;
    EXPECT_CALL(*m_mockController, setBattery(_))
            .WillOnce(SaveArg<0>(&capturedState));

    QComboBox* chargerBox = find<QComboBox>("bat_chargerBox");
    int index = chargerBox->findData(BATTERY_CHARGER_AC);
    chargerBox->setCurrentIndex(index);
    QMetaObject::invokeMethod(m_page.get(), "on_bat_chargerBox_activated",
                              Q_ARG(int, index));

    ASSERT_EQ(capturedState.charger, BATTERY_CHARGER_AC);
}

// Test that changing the health combo box updates the state and calls the
// controller.
TEST_F(BatteryPageTest, HealthBoxUpdatesHealth) {
    EXPECT_CALL(*m_mockController, setBattery(_));
    m_page->show();

    BatteryState capturedState;
    EXPECT_CALL(*m_mockController, setBattery(_))
            .WillOnce(SaveArg<0>(&capturedState));

    QComboBox* healthBox = find<QComboBox>("bat_healthBox");
    int index = healthBox->findData(BATTERY_HEALTH_DEAD);
    healthBox->setCurrentIndex(index);
    QMetaObject::invokeMethod(m_page.get(), "on_bat_healthBox_activated",
                              Q_ARG(int, index));

    ASSERT_EQ(capturedState.health, BATTERY_HEALTH_DEAD);
}

// Test that changing the status combo box updates the state and calls the
// controller.
TEST_F(BatteryPageTest, StatusBoxUpdatesStatus) {
    EXPECT_CALL(*m_mockController, setBattery(_));
    m_page->show();

    BatteryState capturedState;
    EXPECT_CALL(*m_mockController, setBattery(_))
            .WillOnce(SaveArg<0>(&capturedState));

    QComboBox* statusBox = find<QComboBox>("bat_statusBox");
    int index = statusBox->findData(BATTERY_STATUS_CHARGING);
    statusBox->setCurrentIndex(index);
    QMetaObject::invokeMethod(m_page.get(), "on_bat_statusBox_activated",
                              Q_ARG(int, index));

    ASSERT_EQ(capturedState.status, BATTERY_STATUS_CHARGING);
}

// Test that state is correctly saved to and loaded from QSettings.
TEST_F(BatteryPageTest, SavesAndLoadsStateFromSettings) {
    EXPECT_CALL(*m_mockController, setBattery(_));
    m_page->show();

    // 1. Change the UI, which should trigger a save.
    EXPECT_CALL(*m_mockController, setBattery(_));
    find<QSlider>("bat_levelSlider")->setValue(42);

    // 2. Create a new BatteryPage to force a load from the saved settings.
    auto new_page = std::make_unique<BatteryPage>();
    // Inject a new mock controller for the new page.
    auto newMockController = std::make_unique<MockBatteryController>();
    EXPECT_CALL(*newMockController, setBattery(_));
    new_page->setControllerForTest(std::move(newMockController));

    new_page->show();  // Trigger showEvent to load state.

    // 3. Verify the new page loaded the saved state.
    EXPECT_EQ(new_page->findChild<QSlider*>("bat_levelSlider")->value(), 42);
}
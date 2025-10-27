#include "android/skin/qt/extended-pages/cellular-page.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>

#include "android/cmdline-definitions.h"
#include "android/console.h"
#include "android/emulation/control/cellular_agent.h"
#include "android/skin/qt/extended-pages/cellular-controller.h"
#include "android/skin/qt/qt-settings.h"
#include "host-common/hw-config.h"
#include "ui_cellular-page.h"

using ::testing::_;
using ::testing::Field;
using ::testing::SaveArg;

// MockCellularController to verify interactions with the backend.
class MockCellularController : public CellularController {
public:
    MOCK_METHOD(void, setCellular, (const CellularState& state), (override));
};

class CellularPageTest : public ::testing::Test {
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
        QCoreApplication::setApplicationName("CellularPageTest");

        // Create the page and inject the mock controller.
        AndroidHwConfig& hwCfg = *getConsoleAgents()->settings->hw();
        // hwCfg.hw_cellular = true;
        m_page = std::make_unique<CellularPage>();
        auto mockController = std::make_unique<MockCellularController>();
        m_mockController = mockController.get();
        m_page->setControllerForTest(std::move(mockController));

        // Set all combo boxes to index 0
        find<QComboBox>("cell_standardBox")->setCurrentIndex(0);
        find<QComboBox>("cell_signalStatusBox")->setCurrentIndex(0);
        find<QComboBox>("cell_voiceStatusBox")->setCurrentIndex(0);
        find<QComboBox>("cell_dataStatusBox")->setCurrentIndex(0);
        find<QComboBox>("cell_meterStatusBox")->setCurrentIndex(0);
    }

    void TearDown() override { m_page.reset(); }

    // Helper to find a widget by its object name.
    template <typename T>
    T* find(const QString& name) {
        return m_page->findChild<T*>(name);
    }

    std::unique_ptr<CellularPage> m_page;
    MockCellularController* m_mockController;

private:
    // Mock AndroidHardware and other dependencies needed by the page.
    QAndroidCellularAgent m_cellular_agent = {};
};

TEST_F(CellularPageTest, StandardBoxUpdatesNetworkType) {
    EXPECT_CALL(*m_mockController, setCellular(_));
    m_page->show();

    CellularState capturedState;
    EXPECT_CALL(*m_mockController, setCellular(_))
            .WillOnce(SaveArg<0>(&capturedState));

    find<QComboBox>("cell_standardBox")->setCurrentIndex(Cellular_Std_LTE);

    ASSERT_EQ(capturedState.networkType, Cellular_Std_LTE);
}

TEST_F(CellularPageTest, SignalStatusBoxUpdatesSignalStrength) {
    EXPECT_CALL(*m_mockController, setCellular(_));
    m_page->show();

    CellularState capturedState;
    EXPECT_CALL(*m_mockController, setCellular(_))
            .WillOnce(SaveArg<0>(&capturedState));

    find<QComboBox>("cell_signalStatusBox")
            ->setCurrentIndex(Cellular_Signal_Great);

    ASSERT_EQ(capturedState.signalStrength, Cellular_Signal_Great);
}

TEST_F(CellularPageTest, VoiceStatusBoxUpdatesVoiceStatus) {
    EXPECT_CALL(*m_mockController, setCellular(_));
    m_page->show();

    CellularState capturedState;
    EXPECT_CALL(*m_mockController, setCellular(_))
            .WillOnce(SaveArg<0>(&capturedState));

    find<QComboBox>("cell_voiceStatusBox")
            ->setCurrentIndex(Cellular_Stat_Roaming);

    ASSERT_EQ(capturedState.voiceStatus, Cellular_Stat_Roaming);
}

TEST_F(CellularPageTest, DataStatusBoxUpdatesDataStatus) {
    EXPECT_CALL(*m_mockController, setCellular(_));
    m_page->show();

    CellularState capturedState;
    EXPECT_CALL(*m_mockController, setCellular(_))
            .WillOnce(SaveArg<0>(&capturedState));

    find<QComboBox>("cell_dataStatusBox")
            ->setCurrentIndex(Cellular_Stat_Denied);

    ASSERT_EQ(capturedState.dataStatus, Cellular_Stat_Denied);
}

TEST_F(CellularPageTest, MeterStatusBoxUpdatesMeterStatus) {
    EXPECT_CALL(*m_mockController, setCellular(_));
    m_page->show();

    CellularState capturedState;
    EXPECT_CALL(*m_mockController, setCellular(_))
            .WillOnce(SaveArg<0>(&capturedState));

    find<QComboBox>("cell_meterStatusBox")
            ->setCurrentIndex(Cellular_Temporarily_Not_Metered);

    ASSERT_EQ(capturedState.meterStatus, Cellular_Temporarily_Not_Metered);
}

TEST_F(CellularPageTest, SavesAndLoadsStateFromSettings) {
    EXPECT_CALL(*m_mockController, setCellular(_));
    m_page->show();

    // 1. Change the UI, which should trigger a save.
    EXPECT_CALL(*m_mockController, setCellular(_));
    find<QComboBox>("cell_standardBox")->setCurrentIndex(Cellular_Std_GPRS);

    // 2. Create a new CellularPage to force a load from the saved settings.
    auto new_page = std::make_unique<CellularPage>();
    // Inject a new mock controller for the new page.
    auto newMockController = std::make_unique<MockCellularController>();
    EXPECT_CALL(*newMockController, setCellular(_));
    new_page->setControllerForTest(std::move(newMockController));

    new_page->show();  // Trigger showEvent to load state.

    // 3. Verify the new page loaded the saved state.
    EXPECT_EQ(
            new_page->findChild<QComboBox*>("cell_standardBox")->currentIndex(),
            Cellular_Std_GPRS);
}

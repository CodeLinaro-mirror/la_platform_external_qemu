#include "android/skin/qt/extended-pages/legacy-cellular-controller.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <chrono>
#include <thread>

#include "android/base/testing/TestLooper.h"
#include "android/emulation/control/cellular_agent.h"
#include "android/utils/looper.h"

using ::testing::_;

// A mockable struct to hold the agent function pointers.
struct MockCellularAgent {
    MOCK_METHOD(void, setStandard, (CellularStandard std));
    MOCK_METHOD(void, setSignalStrengthProfile, (CellularSignal strength));
    MOCK_METHOD(void, setVoiceStatus, (CellularStatus status));
    MOCK_METHOD(void, setDataStatus, (CellularStatus status));
    MOCK_METHOD(void, setMeterStatus, (CellularMeterStatus status));
};

static constexpr android::base::Looper::Duration kTimeoutMs = 100;

MockCellularAgent* g_mockAgentPtr;

class LegacyCellularControllerTest : public ::testing::Test {
protected:
    void SetUp() override {
        g_mockAgentPtr = &m_mockAgent;
        if (!QCoreApplication::instance()) {
            static int argc = 1;
            static char* argv[] = {(char*)"test"};
            new QApplication(argc, argv);
        }

        mLooper = std::make_unique<android::base::TestLooper>();
        // TestLooper -> base::Looper -> C Looper
        android_registerMainLooper(reinterpret_cast<::Looper*>(
                static_cast<android::base::Looper*>(mLooper.get())));

        // Wire up the QAndroidCellularAgent to call our mock methods.
        m_qAgent.setStandard = [](CellularStandard std) {
            g_mockAgentPtr->setStandard(std);
        };
        m_qAgent.setSignalStrengthProfile = [](CellularSignal strength) {
            g_mockAgentPtr->setSignalStrengthProfile(strength);
        };
        m_qAgent.setVoiceStatus = [](CellularStatus status) {
            g_mockAgentPtr->setVoiceStatus(status);
        };
        m_qAgent.setDataStatus = [](CellularStatus status) {
            g_mockAgentPtr->setDataStatus(status);
        };
        m_qAgent.setMeterStatus = [](CellularMeterStatus status) {
            g_mockAgentPtr->setMeterStatus(status);
        };

        m_controller = std::make_unique<LegacyCellularController>(&m_qAgent);
        testing::Mock::AllowLeak(&m_mockAgent);
    }

    void TearDown() override {
        m_controller.reset();
        android_registerMainLooper(nullptr);
        mLooper.reset();
    }

    void pumpLooper() {
        constexpr android::base::Looper::Duration kStep = 50;  // 50 ms.

        android::base::Looper::Duration current = mLooper->nowMs();
        const android::base::Looper::Duration deadline =
                mLooper->nowMs() + kTimeoutMs;

        while (current + kStep < deadline) {
            mLooper->runOneIterationWithDeadlineMs(current + kStep);
            current = mLooper->nowMs();
        }
    }

    MockCellularAgent m_mockAgent;
    std::unique_ptr<LegacyCellularController> m_controller;
    QAndroidCellularAgent m_qAgent = {};
    std::unique_ptr<android::base::TestLooper> mLooper;
};

TEST_F(LegacyCellularControllerTest, SetCellularCallsAllAgentMethods) {
    CellularState state = {Cellular_Std_LTE,
                           Cellular_Signal_Great,
                           Cellular_Stat_Roaming,
                           Cellular_Stat_Denied,
                           Cellular_Temporarily_Not_Metered};

    EXPECT_CALL(m_mockAgent, setStandard(Cellular_Std_LTE));
    EXPECT_CALL(m_mockAgent, setSignalStrengthProfile(Cellular_Signal_Great));
    EXPECT_CALL(m_mockAgent, setVoiceStatus(Cellular_Stat_Roaming));
    EXPECT_CALL(m_mockAgent, setDataStatus(Cellular_Stat_Denied));
    EXPECT_CALL(m_mockAgent, setMeterStatus(Cellular_Temporarily_Not_Metered));

    m_controller->setCellular(state);
    QCoreApplication::processEvents();  // Process the async calls
    pumpLooper();
}

TEST_F(LegacyCellularControllerTest, SetCellularOnlyCallsChangedMethods) {
    CellularState initialState = {Cellular_Std_LTE,
                                  Cellular_Signal_Great,
                                  Cellular_Stat_Roaming,
                                  Cellular_Stat_Denied,
                                  Cellular_Temporarily_Not_Metered};
    CellularState nextState = initialState;
    nextState.networkType = Cellular_Std_GPRS;  // Only change one field.

    // First call should set everything.
    EXPECT_CALL(m_mockAgent, setStandard(_));
    EXPECT_CALL(m_mockAgent, setSignalStrengthProfile(_));
    EXPECT_CALL(m_mockAgent, setVoiceStatus(_));
    EXPECT_CALL(m_mockAgent, setDataStatus(_));
    EXPECT_CALL(m_mockAgent, setMeterStatus(_));
    m_controller->setCellular(initialState);
    QCoreApplication::processEvents();
    pumpLooper();

    // Second call should only invoke the method for the changed field.
    EXPECT_CALL(m_mockAgent, setStandard(Cellular_Std_GPRS));
    EXPECT_CALL(m_mockAgent, setSignalStrengthProfile(_)).Times(0);
    EXPECT_CALL(m_mockAgent, setVoiceStatus(_)).Times(0);
    EXPECT_CALL(m_mockAgent, setDataStatus(_)).Times(0);
    EXPECT_CALL(m_mockAgent, setMeterStatus(_)).Times(0);
    m_controller->setCellular(nextState);
    QCoreApplication::processEvents();
    pumpLooper();
}

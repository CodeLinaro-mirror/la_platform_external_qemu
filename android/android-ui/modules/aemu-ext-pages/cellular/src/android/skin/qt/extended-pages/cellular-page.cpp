#include "android/skin/qt/extended-pages/cellular-page.h"

#include <qsettings.h>
#include <qstring.h>
#include <QComboBox>
#include <QSettings>
#include <QShowEvent>
#include <QVariant>

#include "aemu/base/async/Looper.h"
#include "aemu/base/async/ThreadLooper.h"
#include "android/avd/util.h"
#include "android/console.h"
#include "android/emulation/control/cellular_agent.h"
#include "android/emulator-window.h"
#include "android/main-common.h"
#include "android/metrics/UiEventTracker.h"
#include "android/skin/qt/qt-settings.h"
#include "android/utils/debug.h"
#include "host-common/VmLock.h"
#include "ui_cellular-page.h"

#include "android/skin/qt/extended-pages/grpc-cellular-controller.h"
#include "android/skin/qt/extended-pages/legacy-cellular-controller.h"

#define DEBUG 0
/* set  >1 for very verbose debugging */
#if DEBUG <= 1
#define DD(...) (void)0
#else
#define DD(...) dinfo(__VA_ARGS__)
#endif

#define STATE(p) \
    case (p):    \
        s = #p;  \
        break;

static std::string translate_idx(CellularStatus value) {
    std::string s = "";
    switch (value) {
        STATE(Cellular_Stat_Home);
        STATE(Cellular_Stat_Roaming);
        STATE(Cellular_Stat_Searching);
        STATE(Cellular_Stat_Denied);
        STATE(Cellular_Stat_Unregistered);
        default:
            derror("%s: Unseen value for cellular status: 0x%x", __func__,
                   value);
            return "Unknown";
    }
    // Chop off "Cellular_"
    return s.substr(9);
}

static std::string translate_idx(CellularStandard value) {
    std::string s = "";
    switch (value) {
        STATE(Cellular_Std_GSM);
        STATE(Cellular_Std_HSCSD);
        STATE(Cellular_Std_GPRS);
        STATE(Cellular_Std_EDGE);
        STATE(Cellular_Std_UMTS);
        STATE(Cellular_Std_HSDPA);
        STATE(Cellular_Std_LTE);
        STATE(Cellular_Std_full);
        STATE(Cellular_Std_5G);
        default:
            derror("%s: Unseen value for cellular standasrd: 0x%x", __func__,
                   value);
            return "Unknown";
    }
    // Chop off "Cellular_"
    return s.substr(9);
}

static std::string translate_idx(CellularSignal value) {
    std::string s = "";
    switch (value) {
        STATE(Cellular_Signal_None);
        STATE(Cellular_Signal_Poor);
        STATE(Cellular_Signal_Moderate);
        STATE(Cellular_Signal_Good);
        STATE(Cellular_Signal_Great);
        default:
            derror("%s: Unseen value for cellular signal: 0x%x", __func__,
                   value);
            return "Unknown";
    }
    // Chop off "Cellular_"
    return s.substr(9);
}

static std::string translate_idx(CellularMeterStatus value) {
    std::string s = "";
    switch (value) {
        STATE(Cellular_Metered);
        STATE(Cellular_Temporarily_Not_Metered);
    }
    return s;
}

CellularPage::CellularPage(QWidget* parent)
    : QWidget(parent),
      mUi(new Ui::CellularPage()),
      mDropDownTracker(new UiEventTracker(
              android_studio::EmulatorUiEvent::OPTION_SELECTED,
              android_studio::EmulatorUiEvent::EXTENDED_CELLULAR_TAB)) {
    mUi->setupUi(this);
    // Restore previous setting values to the UI widgets
    mState.networkType = (CellularStandard)getSavedNetworkType();
    mState.signalStrength = (CellularSignal)getSavedSignalStrength();
    mState.voiceStatus = (CellularStatus)getSavedVoiceStatus();
    mState.dataStatus = (CellularStatus)getSavedDataStatus();
    mState.meterStatus = (CellularMeterStatus)getSavedMeterStatus();
    updateUiFromState();
}

CellularPage::~CellularPage() = default;

void CellularPage::setCellularAgent(const QAndroidCellularAgent* agent) {
    initializeController(agent);
    if (emulator_has_network_option) {
        return;
    }
    saveAndSendState();
}

void CellularPage::setControllerForTest(
        std::unique_ptr<CellularController> controller) {
    mController = std::move(controller);
}

void CellularPage::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    initializeController(getConsoleAgents()->cellular);
    if (!mInitialized) {
        mInitialized = true;

        // Read the stored state, and inform the ui and android.
        mState.networkType = (CellularStandard)getSavedNetworkType();
        mState.signalStrength = (CellularSignal)getSavedSignalStrength();
        mState.voiceStatus = (CellularStatus)getSavedVoiceStatus();
        mState.dataStatus = (CellularStatus)getSavedDataStatus();
        mState.meterStatus = (CellularMeterStatus)getSavedMeterStatus();
        updateUiFromState();
        mController->setCellular(mState);
    }
}

void CellularPage::initializeController(const QAndroidCellularAgent* agent) {
    if (mController) {
        DD("Controller already initialized");
        return;
    }
    if (getConsoleAgents()->settings->android_cmdLineOptions()->grpc_ui) {
        mController = std::make_unique<GrpcCellularController>();
    } else {
        mController = std::make_unique<LegacyCellularController>(agent);
    }
}

void CellularPage::updateUiFromState() {
    mUi->cell_standardBox->setCurrentIndex(mState.networkType);
    mUi->cell_signalStatusBox->setCurrentIndex(mState.signalStrength);
    mUi->cell_voiceStatusBox->setCurrentIndex(mState.voiceStatus);
    mUi->cell_dataStatusBox->setCurrentIndex(mState.dataStatus);
    mUi->cell_meterStatusBox->setCurrentIndex(mState.meterStatus);
}

void CellularPage::saveAndSendState() {
    saveNetworkType(mState.networkType);
    saveSignalStrength(mState.signalStrength);
    saveVoiceStatus(mState.voiceStatus);
    saveDataStatus(mState.dataStatus);
    saveMeterStatus(mState.meterStatus);

    if (mController) {
        DD("Setting cellur state to: %s", mState.toString().c_str());
        mController->setCellular(mState);
    }
}

void CellularPage::on_cell_standardBox_currentIndexChanged(int index) {
    mState.networkType = (CellularStandard)index;
    mDropDownTracker->increment(translate_idx(mState.networkType));
    saveAndSendState();
}

void CellularPage::on_cell_voiceStatusBox_currentIndexChanged(int index) {
    mState.voiceStatus = (CellularStatus)index;
    mDropDownTracker->increment(translate_idx(mState.voiceStatus) + "_VOICE");
    saveAndSendState();
}

void CellularPage::on_cell_meterStatusBox_currentIndexChanged(int index) {
    mState.meterStatus = (CellularMeterStatus)index;
    mDropDownTracker->increment(translate_idx(mState.meterStatus));
    saveAndSendState();
}

void CellularPage::on_cell_dataStatusBox_currentIndexChanged(int index) {
    mState.dataStatus = (CellularStatus)index;
    mDropDownTracker->increment(translate_idx(mState.dataStatus) + "_DATA");
    saveAndSendState();
}

void CellularPage::on_cell_signalStatusBox_currentIndexChanged(int index) {
    mState.signalStrength = (CellularSignal)index;
    mDropDownTracker->increment(translate_idx(mState.signalStrength));
    saveAndSendState();
}

void CellularPage::saveDataStatus(int status) {
    const char* avdPath = path_getAvdContentPath(
            getConsoleAgents()->settings->hw()->avd_name);
    if (avdPath) {
        QString avdSettingsFile =
                avdPath + QString(Ui::Settings::PER_AVD_SETTINGS_NAME);
        QSettings avdSpecificSettings(avdSettingsFile, QSettings::IniFormat);
        avdSpecificSettings.setValue(Ui::Settings::PER_AVD_CELLULAR_DATA_STATUS,
                                     status);
    } else {
        QSettings settings;
        settings.setValue(Ui::Settings::CELLULAR_DATA_STATUS, status);
    }
}

void CellularPage::saveNetworkType(int type) {
    const char* avdPath = path_getAvdContentPath(
            getConsoleAgents()->settings->hw()->avd_name);
    if (avdPath) {
        QString avdSettingsFile =
                avdPath + QString(Ui::Settings::PER_AVD_SETTINGS_NAME);
        QSettings avdSpecificSettings(avdSettingsFile, QSettings::IniFormat);
        avdSpecificSettings.setValue(
                Ui::Settings::PER_AVD_CELLULAR_NETWORK_TYPE, type);
    } else {
        QSettings settings;
        settings.setValue(Ui::Settings::CELLULAR_NETWORK_TYPE, type);
    }
}

void CellularPage::saveSignalStrength(int strength) {
    const char* avdPath = path_getAvdContentPath(
            getConsoleAgents()->settings->hw()->avd_name);
    if (avdPath) {
        QString avdSettingsFile =
                avdPath + QString(Ui::Settings::PER_AVD_SETTINGS_NAME);
        QSettings avdSpecificSettings(avdSettingsFile, QSettings::IniFormat);
        avdSpecificSettings.setValue(
                Ui::Settings::PER_AVD_CELLULAR_SIGNAL_STRENGTH, strength);
    } else {
        QSettings settings;
        settings.setValue(Ui::Settings::CELLULAR_SIGNAL_STRENGTH, strength);
    }
}

void CellularPage::saveVoiceStatus(int status) {
    const char* avdPath = path_getAvdContentPath(
            getConsoleAgents()->settings->hw()->avd_name);
    if (avdPath) {
        QString avdSettingsFile =
                avdPath + QString(Ui::Settings::PER_AVD_SETTINGS_NAME);
        QSettings avdSpecificSettings(avdSettingsFile, QSettings::IniFormat);
        avdSpecificSettings.setValue(
                Ui::Settings::PER_AVD_CELLULAR_VOICE_STATUS, status);
    } else {
        QSettings settings;
        settings.setValue(Ui::Settings::CELLULAR_VOICE_STATUS, status);
    }
}

void CellularPage::saveMeterStatus(int status) {
    const char* avdPath = path_getAvdContentPath(
            getConsoleAgents()->settings->hw()->avd_name);
    if (avdPath) {
        QString avdSettingsFile =
                avdPath + QString(Ui::Settings::PER_AVD_SETTINGS_NAME);
        QSettings avdSpecificSettings(avdSettingsFile, QSettings::IniFormat);
        avdSpecificSettings.setValue(
                Ui::Settings::PER_AVD_CELLULAR_METER_STATUS, status);
    } else {
        QSettings settings;
        settings.setValue(Ui::Settings::CELLULAR_METER_STATUS, status);
    }
}

int CellularPage::getSavedMeterStatus() {
    const char* avdPath = path_getAvdContentPath(
            getConsoleAgents()->settings->hw()->avd_name);
    if (avdPath) {
        QString avdSettingsFile =
                avdPath + QString(Ui::Settings::PER_AVD_SETTINGS_NAME);
        QSettings avdSpecificSettings(avdSettingsFile, QSettings::IniFormat);
        return avdSpecificSettings
                .value(Ui::Settings::PER_AVD_CELLULAR_METER_STATUS,
                       Cellular_Metered)
                .toInt();
    } else {
        QSettings settings;
        return settings
                .value(Ui::Settings::CELLULAR_METER_STATUS, Cellular_Metered)
                .toInt();
    }
}

int CellularPage::getSavedDataStatus() {
    const char* avdPath = path_getAvdContentPath(
            getConsoleAgents()->settings->hw()->avd_name);
    if (avdPath) {
        QString avdSettingsFile =
                avdPath + QString(Ui::Settings::PER_AVD_SETTINGS_NAME);
        QSettings avdSpecificSettings(avdSettingsFile, QSettings::IniFormat);
        return avdSpecificSettings
                .value(Ui::Settings::PER_AVD_CELLULAR_DATA_STATUS,
                       Cellular_Stat_Home)
                .toInt();
    } else {
        QSettings settings;
        return settings
                .value(Ui::Settings::CELLULAR_DATA_STATUS, Cellular_Stat_Home)
                .toInt();
    }
}

int CellularPage::getSavedNetworkType() {
    const char* avdPath = path_getAvdContentPath(
            getConsoleAgents()->settings->hw()->avd_name);
    if (avdPath) {
        QString avdSettingsFile =
                avdPath + QString(Ui::Settings::PER_AVD_SETTINGS_NAME);
        QSettings avdSpecificSettings(avdSettingsFile, QSettings::IniFormat);
        return avdSpecificSettings
                .value(Ui::Settings::PER_AVD_CELLULAR_NETWORK_TYPE,
                       Cellular_Std_full)
                .toInt();
    } else {
        QSettings settings;
        return settings
                .value(Ui::Settings::CELLULAR_NETWORK_TYPE, Cellular_Std_full)
                .toInt();
    }
}

int CellularPage::getSavedSignalStrength() {
    const char* avdPath = path_getAvdContentPath(
            getConsoleAgents()->settings->hw()->avd_name);
    if (avdPath) {
        QString avdSettingsFile =
                avdPath + QString(Ui::Settings::PER_AVD_SETTINGS_NAME);
        QSettings avdSpecificSettings(avdSettingsFile, QSettings::IniFormat);
        return avdSpecificSettings
                .value(Ui::Settings::PER_AVD_CELLULAR_SIGNAL_STRENGTH,
                       Cellular_Signal_Moderate)
                .toInt();
    } else {
        QSettings settings;
        return settings
                .value(Ui::Settings::CELLULAR_SIGNAL_STRENGTH,
                       Cellular_Signal_Moderate)
                .toInt();
    }
}

int CellularPage::getSavedVoiceStatus() {
    const char* avdPath = path_getAvdContentPath(
            getConsoleAgents()->settings->hw()->avd_name);
    if (avdPath) {
        QString avdSettingsFile =
                avdPath + QString(Ui::Settings::PER_AVD_SETTINGS_NAME);
        QSettings avdSpecificSettings(avdSettingsFile, QSettings::IniFormat);
        return avdSpecificSettings
                .value(Ui::Settings::PER_AVD_CELLULAR_VOICE_STATUS,
                       Cellular_Stat_Home)
                .toInt();
    } else {
        QSettings settings;
        return settings
                .value(Ui::Settings::CELLULAR_VOICE_STATUS, Cellular_Stat_Home)
                .toInt();
    }
}

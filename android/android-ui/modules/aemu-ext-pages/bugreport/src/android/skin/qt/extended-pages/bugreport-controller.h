// Copyright 2025 The Android Open Source Project
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

#pragma once

#include <functional>
#include <string>

#include "aemu/base/StringFormat.h"

// A struct to hold all the static system information for a bug report.
struct BugreportInfo {
    std::string emulatorVersion;
    std::string hypervisorVersion;
    std::string androidVersion;
    std::string deviceName;
    std::string hostOsName;
    std::string avdDetails;
    int apiLevel;

    std::string dump() const {
        std::string report;
        android::base::StringAppendFormat(&report, "Emulator Version: %s (%s)\n",
                                          emulatorVersion.c_str(),
                                          hypervisorVersion.c_str());
        android::base::StringAppendFormat(&report, "Android Version: %s\n",
                                          androidVersion.c_str());
        android::base::StringAppendFormat(&report, "Host OS: %s\n",
                                          hostOsName.c_str());
        android::base::StringAppendFormat(&report, "Device Name: %s\n",
                                          deviceName.c_str());
        report += "\n--- AVD DETAILS ---\n";
        report += avdDetails;
        return report;
    }
};

// BugreportController is an abstract base class that defines the interface
// for all backend operations required by the bug report page. This allows the UI
// to be decoupled from the specific implementation (e.g., legacy vs. gRPC).
class BugreportController {
public:
    virtual ~BugreportController() = default;

    // Asynchronously takes a screenshot and saves it to a temporary file.
    // The provided callback is invoked on the UI thread with the path to the
    // screenshot file on success, or an empty string on failure.
    virtual void takeScreenshot(std::function<void(const std::string&)> callback) = 0;

    // Asynchronously checks if the guest system has finished booting.
    // The provided callback is invoked on the UI thread with the result.
    virtual void isBootCompleted(std::function<void(bool)> callback) = 0;

    // Fetches the static system information needed for the bug report.
    virtual BugreportInfo getSystemInfo() = 0;
};

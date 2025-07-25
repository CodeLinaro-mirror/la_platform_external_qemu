// Copyright 2024 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
#include "android/crashreport/CrashpadLogSink.h"
#include "android/base/logging/LogRegistry.h"
#include "android/crashreport/SimpleStringAnnotation.h"

#include "absl/base/macros.h"
#include "absl/strings/str_format.h"
#include "absl/time/time.h"

#include "absl/log/log.h"
#include "absl/log/log_sink_registry.h"

#include <atomic>
#include <cstdint>
#include <iostream>

namespace android {
namespace crashreport {

void CrashpadLogSink::registerSink() {
    static std::atomic_bool initialized = false;
    static CrashpadLogSink crashpadSink;
    if (!initialized.exchange(true)) {
        android::base::registerSink(&crashpadSink);
        LOG(INFO) << "CrashpadLogSink registered, error messages will be "
                     "included in crashreports.";
    }
}

void CrashpadLogSink::Send(const absl::LogEntry& entry) {
    std::string_view msg = entry.text_message_with_newline();

    // In the FATAL case we get 2 calls.
    // The first will contain the log message.
    // The 2nd call will contain the stacktrace, if any.
    // So no need to log the 2nd entry.
    if (entry.log_severity() == absl::LogSeverity::kFatal &&
        !entry.stacktrace().empty()) {
        return;
    }

    switch (entry.log_severity()) {
        case absl::LogSeverity::kInfo:
        case absl::LogSeverity::kWarning:
            return;
        case absl::LogSeverity::kError:
        case absl::LogSeverity::kFatal:
        default:
            mOutputStream << msg;
            mOutputStream.flush();
    }
}

}  // namespace crashreport
}  // namespace android

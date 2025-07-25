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
#pragma once
#include <ostream>

#include "absl/log/log_sink.h"

#include "android/base/logging/LoggingApi.h"
#include "android/crashreport/AnnotationStreambuf.h"

namespace android {
namespace crashreport {

/**
 * @brief A custom log sink that can format messages using ANSI colors.
 *
 * The sink will output to std::cout.
 */
class CrashpadLogSink : public absl::LogSink {
public:
    CrashpadLogSink() = default;
    virtual ~CrashpadLogSink() = default;

    /**
     * @brief Sends a formatted log entry to the output stream.
     *
     * @param entry The log entry to be formatted and sent.
     */
    void Send(const absl::LogEntry& entry) override;

    /**
     * @brief Registers the crash sink with the log system.
     */
    static void registerSink();

private:
    DefaultAnnotationStreambuf mLogBuf{"ERRLOG"};
    std::ostream mOutputStream{&mLogBuf};
};
}  // namespace crashreport
}  // namespace android
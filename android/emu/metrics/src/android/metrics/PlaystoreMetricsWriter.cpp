// Copyright 2019 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
#include "android/metrics/PlaystoreMetricsWriter.h"

#include <assert.h>    // for assert
#include <errno.h>     // for errno
#include <inttypes.h>  // for PRIi64
#include <stdlib.h>    // for size_t
#include <string.h>    // for strerror
#include <chrono>      // for millise...
#include <fstream>     // for ios
#include <memory>
#include <sstream>  // IWYU pragma: keep
#include <utility>  // for move

#include "aemu/base/Log.h"                  // for LOG
#include "aemu/base/Uuid.h"                 // for Uuid
#include "aemu/base/files/GzipStreambuf.h"  // for GzipOut...
#include "aemu/base/files/PathUtils.h"      // for PathUtils
#include "android/base/system/System.h"     // for System
#include "android/curl-support.h"           // for curl_post
#include "android/metrics/FileMetricsWriter.h"
#include "android/metrics/MetricsLogging.h"  // for D
#include "android/metrics/MetricsPaths.h"    // for getSpoolDirectory
#include "android/metrics/StudioConfig.h"
#include "android/metrics/studio_stats_wrapper.pb.h"  // for Android...
#include "android/utils/debug.h"                      // for VERBOSE...
#include "android/utils/eintr_wrapper.h"              // for HANDLE_EINTR
#include "android/utils/file_io.h"                    // for android_unlink
#include "android/utils/path.h"         // for path_mkdir_if_needed
#include "android/version.h"            // for EMULATO...
#include "google_logs_publishing.pb.h"  // for LogEvent

namespace android {
namespace metrics {

using android::base::PathUtils;
using std::chrono::duration_cast;
using std::chrono::milliseconds;
using wireless_android_play_playlog::ClientInfo;
using wireless_android_play_playlog::LogEvent;
using wireless_android_play_playlog::LogRequest;
using wireless_android_play_playlog::LogResponse;

static milliseconds epoch_time_in_ms() {
    std::chrono::microseconds nowUs(base::System::get()->getUnixTimeUs());
    return duration_cast<milliseconds>(nowUs);
}

PlaystoreMetricsWriter::PlaystoreMetricsWriter(const std::string& sessionId,
                                               const std::string& cookieFile,
                                               std::string url)
    : MetricsWriter(sessionId),
      mUrl(std::move(url)),
      mCookieFile(cookieFile),
      mSessionFileLock(
              base::makeCustomScopedPtr<FileLock*>(nullptr, filelock_release)) {
    std::string dirName;
    if (!mCookieFile.empty() &&
        PathUtils::split(mCookieFile.data(), PathUtils::HOST_TYPE, &dirName,
                         nullptr)) {
        mSpoolDir = PathUtils::removeTrailingDirSeparator(dirName.c_str());
    } else {
        mSpoolDir = getSpoolDirectory();
    }
    openSessionFile();

    std::ifstream cookieResponse(PathUtils::asUnicodePath(cookieFile.data()).c_str(),
                                 std::ios::in | std::ios::binary);
    if (cookieResponse.good()) {
        LogResponse response;
        response.ParseFromIstream(&cookieResponse);
        mSendAfterMs = milliseconds(response.next_request_wait_millis());
        D("Read a timeout cookie from %s, wait until %d.", mCookieFile,
          mSendAfterMs.count());
    }
}

PlaystoreMetricsWriter::~PlaystoreMetricsWriter() {
    // The session ended cleanly: remove the crash marker before the final
    // upload, which can block for a long time, so that killing the process
    // during that upload isn't reported as a crash by the next session.
    closeSessionFile();
    commit();
}

void PlaystoreMetricsWriter::openSessionFile() {
    if (mSpoolDir.empty() || sessionId().empty()) {
        return;
    }
    path_mkdir_if_needed(mSpoolDir.c_str(), 0744);
    if (!base::System::get()->pathIsDir(mSpoolDir)) {
        return;
    }

    int counter = 0;
    auto file = FileMetricsWriter::createLockedOpenFile(mSpoolDir, sessionId(),
                                                        &counter);
    if (!file) {
        W("Failed to create Playstore session marker in %s", mSpoolDir.c_str());
        return;
    }
    IGNORE_EINTR(close(file->fd));
    mSessionFilePath = std::move(file->path);
    mSessionFileLock = std::move(file->lock);
    D("Opened Playstore session marker %s", mSessionFilePath.c_str());
}

void PlaystoreMetricsWriter::closeSessionFile() {
    if (!mSessionFilePath.empty()) {
        if (HANDLE_EINTR(android_unlink(mSessionFilePath.c_str())) != 0) {
            W("Failed to delete Playstore session marker '%s': %s",
              mSessionFilePath.c_str(), strerror(errno));
        } else {
            D("Removed Playstore session marker %s", mSessionFilePath.c_str());
        }
        mSessionFilePath.clear();
    }
    mSessionFileLock.reset();
}

PlaystoreMetricsWriter::Ptr PlaystoreMetricsWriter::create(
        const std::string& sessionId,
        const std::string& cookieFile) {
    return std::make_shared<PlaystoreMetricsWriter>(sessionId, cookieFile);
}

void PlaystoreMetricsWriter::write(
        const android_studio::AndroidStudioEvent& asEvent,
        LogEvent* logEvent) {
    D("event time %" PRIi64 " ms", logEvent->event_time_ms());
    D("{ %s }", asEvent.ShortDebugString().c_str());

    LogEvent event(*logEvent);
    asEvent.SerializeToString(event.mutable_source_extension());
    auto messageLength = event.ByteSizeLong();

    // Message is too big to track, drop it.
    if (messageLength > kMaxStorage) {
        D("Message size %lu is larger than max storage %lu. Dropping.",
                messageLength, kMaxStorage)
        return;
    }

    // Erase events if we are over our storage limit..
    while (mBytes + messageLength > kMaxStorage && !mEvents.empty()) {
        mBytes -= mEvents.front().ByteSizeLong();
        mEvents.pop();
        D("Erased an old event to free space for new ones.")
    };

    // empty queue -> 0 storage.
    assert(!mEvents.empty() || mBytes == 0);

    mEvents.push(std::move(event));
    mBytes += messageLength;
    assert(mBytes <= kMaxStorage);
}

static const char* osType() {
    switch (base::System::get()->getOsType()) {
        case base::OsType::Linux:
            return "linux";
        case base::OsType::Mac:
            return "macosx";
        case base::OsType::Windows:
            return "windows";
        default:
            return "unknown";
    }
}

// Sets the OS information and log source, the LogRequest will be similar as to
// what android studio produces on this OS.
static LogRequest buildBaseRequest() {
    LogRequest request;
    request.set_request_time_ms(epoch_time_in_ms().count());
    request.set_log_source(LogRequest::ANDROID_STUDIO);

    auto client = request.mutable_client_info();
    client->set_client_type(ClientInfo::DESKTOP);

    // Setup the os information.
    auto desktop = client->mutable_desktop_client_info();
    desktop->set_application_build(EMULATOR_VERSION_STRING);
    desktop->set_os_full_version(base::System::get()->getOsName());
    desktop->set_os(osType());
    desktop->set_os_major_version(base::System::get()->getMajorOsVersion());
    desktop->set_logging_id(android::studio::getInstallationId());
    return request;
}

// Callback where we collect the received bytes..
static size_t curlWriteCallback(char* contents,
                                size_t size,
                                size_t nmemb,
                                void* userp) {
    auto& proto = *static_cast<std::string*>(userp);
    const size_t total = size * nmemb;

    proto.insert(proto.end(), contents, contents + total);
    return total;
}

void PlaystoreMetricsWriter::writeCookie(std::string proto) {
    LogResponse response;
    if (response.ParseFromString(proto)) {
        // The cookie will contain the earlies timestamp after which we are
        // good to send data.
        mSendAfterMs = milliseconds(response.next_request_wait_millis()) +
                       epoch_time_in_ms();

        response.set_next_request_wait_millis(mSendAfterMs.count());
        std::ofstream cookieResponse(
                PathUtils::asUnicodePath(mCookieFile.data()).c_str(),
                std::ios::out | std::ios::binary | std::ios::trunc);
        response.SerializeToOstream(&cookieResponse);
        cookieResponse.close();
        D("Wrote a timeout cookie for %" PRIi64 " ms to %s",
          response.next_request_wait_millis(), mCookieFile.c_str());
    }
}

void PlaystoreMetricsWriter::commit() {
    if (mEvents.empty() || epoch_time_in_ms() < mSendAfterMs) {
        D("Not reporting %d metrics time: %" PRIi64 ", wait until %" PRIi64,
          mEvents.size(), epoch_time_in_ms().count(), mSendAfterMs.count());
        return;
    }

    // Note that we are not sending meta metrics, as we are not tracking failed
    // uploads.
    auto request = buildBaseRequest();
    D("{ %s }", request.ShortDebugString().c_str());
    D("Upload request contains %d event logs.", mEvents.size());
    while (!mEvents.empty()) {
        request.add_log_event()->CopyFrom(mEvents.front());
        mEvents.pop();
    }
    mBytes = 0;

    // Now gzip the bytes to an in memory stream.
    std::stringbuf buffer;
    std::ostream memoryStream(&buffer);
    base::GzipOutputStream gos(memoryStream);

    if (!request.SerializeToOstream(&gos)) {
        LOG(ERROR) << "Failed to serialize stream.";
        return;
    }
    gos.flush();

    char* error = nullptr;
    const char* encoding = "Content-Encoding: gzip";
    const char* content_type = "Content-Type: application/x-gzip";
    const char* headers[] = {encoding, content_type};
    std::string protobuf_response;

    // Now ship of the bytes.
    const auto data = buffer.str();
    bool success = curl_post(mUrl.c_str(), data.data(), data.size(), headers, 2,
                             curlWriteCallback, &protobuf_response, &error);
    if (success) {
        D("Metrics written to playstore");
        writeCookie(protobuf_response);
    } else {
        LOG(ERROR) << "Failed to send data due to: " << error;
        free(error);
    }
}

}  // namespace metrics
}  // namespace android

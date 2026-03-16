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
#include "fishtank_agents.h"

#include "absl/strings/match.h"
#include "absl/synchronization/notification.h"
#include "aemu/base/files/PathUtils.h"
#include "android/base/system/System.h"
#include "android/utils/debug.h"
#include "host-common/record_screen_agent.h"

#include <fstream>
#include <mutex>
#include <string>

using namespace android::emulation::control;

// Wrapper to own transient data in RecordingInfo for async calls.
// This is necessary because the caller-provided RecordingInfo pointer
// might go out of scope (e.g. if it's on the stack) before the gRPC
// callback fires. The wrapper moves the data to the heap via shared_ptr.
struct RecordingInfoWrapper {
    ::RecordingInfo info;
    std::string fileName;

    RecordingInfoWrapper(const ::RecordingInfo* source) {
        info = *source;
        if (source->fileName) {
            fileName = source->fileName;
            info.fileName = fileName.c_str();
        }
    }
};

// Track current recording info to handle stop requests without displayId
static struct {
    ::RecordingInfo info;
    std::string fileName;
    ::RecorderState state = RECORDER_STOPPED;
    bool active = false;
    std::mutex mtx;
} sCurrentRecording;

static void updateActiveRecording(const ::RecordingInfo* info, bool active, ::RecorderState state) {
    std::lock_guard<std::mutex> lock(sCurrentRecording.mtx);
    if (info) {
        sCurrentRecording.info = *info;
        if (info->fileName) {
            sCurrentRecording.fileName = info->fileName;
            sCurrentRecording.info.fileName = sCurrentRecording.fileName.c_str();
        } else {
            sCurrentRecording.fileName.clear();
            sCurrentRecording.info.fileName = nullptr;
        }
    }
    sCurrentRecording.active = active;
    sCurrentRecording.state = state;
}

static ::RecorderState toAgentState(incubating::RecordingInfo::RecorderState state) {
    switch (state) {
        case incubating::RecordingInfo::RECORDER_STATE_STARTING:
            return RECORDER_STARTING;
        case incubating::RecordingInfo::RECORDER_STATE_RECORDING:
            return RECORDER_RECORDING;
        case incubating::RecordingInfo::RECORDER_STATE_STOPPING:
            return RECORDER_STOPPING;
        case incubating::RecordingInfo::RECORDER_STATE_STOPPED:
        case incubating::RecordingInfo::RECORDER_STATE_START_FAILED:
        case incubating::RecordingInfo::RECORDER_STOP_FAILED:
            return RECORDER_STOPPED;
        default:
            return RECORDER_STOPPED;
    }
}

static incubating::RecordingInfo toProto(const ::RecordingInfo* info) {
    incubating::RecordingInfo proto;
    if (info->fileName) {
        proto.set_file_name(info->fileName);
    }
    proto.set_width(info->width);
    proto.set_height(info->height);
    proto.set_bit_rate(info->videoBitrate);
    proto.set_time_limit(info->timeLimit);
    proto.set_fps(info->fps);
    proto.set_display(info->displayId);
    return proto;
}

static void handleRecordingStatus(const ::RecordingInfo* info,
                                 incubating::RecordingInfo::RecorderState state) {
    if (!info || !info->cb) {
        return;
    }
    RecordingStatus status = RECORD_START_FAILED;
    switch (state) {
        case incubating::RecordingInfo::RECORDER_STATE_STARTING:
            status = RECORD_START_INITIATED;
            break;
        case incubating::RecordingInfo::RECORDER_STATE_RECORDING:
            status = RECORD_STARTED;
            break;
        case incubating::RecordingInfo::RECORDER_STATE_STOPPING:
            status = RECORD_STOP_INITIATED;
            break;
        case incubating::RecordingInfo::RECORDER_STATE_STOPPED:
            status = RECORD_STOPPED;
            break;
        case incubating::RecordingInfo::RECORDER_STATE_START_FAILED:
            status = RECORD_START_FAILED;
            break;
        case incubating::RecordingInfo::RECORDER_STOP_FAILED:
            status = RECORD_STOP_FAILED;
            break;
        default:
            break;
    }
    info->cb(info->opaque, status);
}

static void startRecordingInternal(const ::RecordingInfo* info,
                                   std::function<void(bool)> onResult) {
    auto client = getGlobalRecordingClient();
    if (!client) {
        onResult(false);
        return;
    }

    auto infoCopy = std::make_shared<RecordingInfoWrapper>(info);
    client->startRecordingAsync(
            toProto(info),
            [infoCopy, onResult](absl::StatusOr<incubating::RecordingInfo*> result) {
                bool success = false;
                if (result.ok()) {
                    auto proto = *result;
                    success = proto->state() ==
                              incubating::RecordingInfo::RECORDER_STATE_RECORDING;
                    handleRecordingStatus(&infoCopy->info, proto->state());
                    updateActiveRecording(&infoCopy->info, success,
                                         toAgentState(proto->state()));
                } else {
                    derror("gRPC StartRecording failed: %s",
                           result.status().ToString().c_str());
                    handleRecordingStatus(
                            &infoCopy->info,
                            incubating::RecordingInfo::RECORDER_STATE_START_FAILED);
                    updateActiveRecording(&infoCopy->info, false, RECORDER_STOPPED);
                }
                onResult(success);
            });
}

const QAndroidRecordScreenAgent sFishtankQAndroidRecordScreenAgent = {
        .startRecording =
                [](const ::RecordingInfo* info) {
                    bool success = false;
                    absl::Notification done;
                    startRecordingInternal(info, [&](bool result) {
                        success = result;
                        done.Notify();
                    });
                    done.WaitForNotification();
                    return success;
                },
        .startRecordingAsync =
                [](const ::RecordingInfo* info) {
                    startRecordingInternal(info, [](bool) {});
                    return true;
                },
        .stopRecording =
                []() {
                    auto client = getGlobalRecordingClient();
                    if (!client) {
                        return false;
                    }

                    bool success = false;
                    std::mutex mtx;
                    std::condition_variable cv;
                    bool done = false;

                    ::RecordingInfo currentInfo;
                    {
                        std::lock_guard<std::mutex> lock(sCurrentRecording.mtx);
                        currentInfo = sCurrentRecording.info;
                    }
                    auto infoCopy = std::make_shared<RecordingInfoWrapper>(&currentInfo);

                    client->stopRecordingAsync(
                            toProto(&currentInfo),
                            [&, infoCopy](absl::StatusOr<incubating::RecordingInfo*> result) {
                                std::lock_guard<std::mutex> lock(mtx);
                                if (result.ok()) {
                                    auto proto = *result;
                                    success = proto->state() ==
                                              incubating::RecordingInfo::
                                                      RECORDER_STATE_STOPPED;
                                    handleRecordingStatus(&infoCopy->info, proto->state());
                                    updateActiveRecording(&infoCopy->info, !success, toAgentState(proto->state()));
                                } else {
                                    derror("gRPC StopRecording failed: %s",
                                           result.status().ToString().c_str());
                                    handleRecordingStatus(
                                            &infoCopy->info,
                                            incubating::RecordingInfo::
                                                    RECORDER_STOP_FAILED);
                                    updateActiveRecording(&infoCopy->info, true, RECORDER_RECORDING);
                                }
                                done = true;
                                cv.notify_one();
                            });

                    std::unique_lock<std::mutex> lock(mtx);
                    cv.wait(lock, [&] { return done; });
                    return success;
                },
        .stopRecordingAsync =
                []() {
                    auto client = getGlobalRecordingClient();
                    if (!client) {
                        return false;
                    }

                    ::RecordingInfo currentInfo;
                    {
                        std::lock_guard<std::mutex> lock(sCurrentRecording.mtx);
                        currentInfo = sCurrentRecording.info;
                    }
                    auto infoCopy = std::make_shared<RecordingInfoWrapper>(&currentInfo);

                    client->stopRecordingAsync(
                            toProto(&currentInfo),
                            [infoCopy](absl::StatusOr<incubating::RecordingInfo*> result) {
                                if (result.ok()) {
                                    auto proto = *result;
                                    bool stopped = proto->state() ==
                                              incubating::RecordingInfo::
                                                      RECORDER_STATE_STOPPED;
                                    handleRecordingStatus(&infoCopy->info,
                                                         proto->state());
                                    updateActiveRecording(&infoCopy->info, !stopped, toAgentState(proto->state()));
                                } else {
                                    derror("gRPC StopRecording failed: %s",
                                           result.status().ToString().c_str());
                                    handleRecordingStatus(
                                            &infoCopy->info,
                                            incubating::RecordingInfo::
                                                    RECORDER_STOP_FAILED);
                                    updateActiveRecording(&infoCopy->info, true, RECORDER_RECORDING);
                                }
                            });
                    return true;
                },
        .getRecorderState =
                []() {
                    std::lock_guard<std::mutex> lock(sCurrentRecording.mtx);
                    return RecorderStates{sCurrentRecording.state, sCurrentRecording.info.displayId};
                },
        .doSnap =
                [](const char* name, uint32_t displayId) {
                    auto client = getGlobalControlClient();
                    if (!client) {
                        return false;
                    }

                    android::emulation::control::ImageFormat format;
                    format.set_format(android::emulation::control::ImageFormat::PNG);
                    format.set_display(displayId);

                    bool success = false;
                    absl::Notification done;

                    client->getScreenshotAsync(
                            format,
                            [&](absl::StatusOr<android::emulation::control::Image*> imgOrStatus) {
                        if (imgOrStatus.ok()) {
                            auto img = *imgOrStatus;
                            if (img && !img->image().empty()) {
                                std::string outputFilePath;
                                std::string outputDirectoryPath = name ? name : "";

                                if (absl::EndsWith(outputDirectoryPath, ".png")) {
                                    outputFilePath = outputDirectoryPath;
                                } else {
                                    char fileName[100];
                                    snprintf(fileName, sizeof(fileName), "Screenshot_%lld.png",
                                            (int64_t) android::base::System::get()->getUnixTime());
                                    outputFilePath =
                                            outputDirectoryPath.empty()
                                                    ? fileName
                                                    : android::base::PathUtils::join(
                                                              outputDirectoryPath,
                                                              fileName);
                                }

                                std::ofstream file(
                                        android::base::PathUtils::asUnicodePath(
                                                outputFilePath.c_str())
                                                .c_str(),
                                        std::ios::binary);
                                if (file) {
                                    file.write(img->image().data(),
                                               img->image().size());
                                    success = true;
                                    dinfo("Saved screenshot to %s", outputFilePath.c_str());
                                } else {
                                    derror("Failed to open %s for writing", outputFilePath.c_str());
                                }
                            }
                        } else {
                            derror("gRPC getScreenshot failed: %s",
                                   imgOrStatus.status().ToString().c_str());
                        }
                        done.Notify();
                    });

                    done.WaitForNotification();
                    return success;
                },
        .startSharedMemoryModule = [](int size) -> const char* {
            NOT_IMPLEMENTED("QAndroidRecordScreenAgent.startSharedMemoryModule(size: %d)", size);
            return nullptr;
        },
        .stopSharedMemoryModule =
                []() {
                    NOT_IMPLEMENTED("QAndroidRecordScreenAgent.stopSharedMemoryModule");
                    return false;
                },
};

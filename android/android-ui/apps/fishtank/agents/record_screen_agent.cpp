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
#include "aemu/base/files/PathUtils.h"
#include "android/base/system/System.h"
#include "android/utils/debug.h"
#include "host-common/record_screen_agent.h"

#include <condition_variable>
#include <fstream>
#include <mutex>

const QAndroidRecordScreenAgent sFishtankQAndroidRecordScreenAgent = {
        .startRecording =
                [](const RecordingInfo* info) {
                    NOT_IMPLEMENTED("QAndroidRecordScreenAgent.startRecording(info: %p)", info);
                    return false;
                },
        .startRecordingAsync =
                [](const RecordingInfo* info) {
                    NOT_IMPLEMENTED("QAndroidRecordScreenAgent.startRecordingAsync(info: %p)", info);
                    return false;
                },
        .stopRecording =
                []() {
                    NOT_IMPLEMENTED("QAndroidRecordScreenAgent.stopRecording");
                    return false;
                },
        .stopRecordingAsync =
                []() {
                    NOT_IMPLEMENTED("QAndroidRecordScreenAgent.stopRecordingAsync");
                    return false;
                },
        .getRecorderState =
                []() {
                    NOT_IMPLEMENTED("QAndroidRecordScreenAgent.getRecorderState");
                    return RecorderStates{RECORDER_STOPPED, 0};
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
                    std::mutex mtx;
                    std::condition_variable cv;
                    bool done = false;

                    client->getScreenshotAsync(
                            format,
                            [&](absl::StatusOr<android::emulation::control::Image*> imgOrStatus) {
                        std::lock_guard<std::mutex> lock(mtx);
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
                        done = true;
                        cv.notify_one();
                    });

                    std::unique_lock<std::mutex> lock(mtx);
                    cv.wait(lock, [&] { return done; });
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

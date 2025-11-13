/*
 * Copyright (C) 2023 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace android {
namespace emulation {
namespace control {

// Represents the compatibility status of a snapshot.
enum class SnapshotStatus {
    Compatible,
    Incompatible,
    Loaded,
    Unknown,
};

// A simple struct to hold the details of a snapshot.
struct SnapshotInfo {
    std::string snapshot_id;
    SnapshotStatus status;
    std::chrono::system_clock::time_point creation_time;
    std::string logical_name;
    std::string description;
    uint64_t size;
};

// Represents a screenshot of a snapshot
struct SnapshotScreenshot {
    enum class Format {
        PNG,
        UNSPECIFIED,
    };

    Format format;
    std::vector<char> data;
};

// Interface for managing snapshots in the emulator.
class SnapshotController {
public:
    virtual ~SnapshotController() = default;

    virtual void listSnapshots(
            std::function<void(absl::StatusOr<std::vector<SnapshotInfo>>)>
                    callback) = 0;
    virtual void loadSnapshot(const std::string& snapshotId,
                              const std::string& destination,
                              std::function<void(absl::Status)> callback) = 0;
    virtual void saveSnapshot(const std::string& snapshotId,
                              std::function<void(absl::Status)> callback) = 0;
    virtual void deleteSnapshot(
            const std::string& snapshotId,
            std::function<void(absl::Status)> callback) = 0;
    virtual void updateSnapshot(
            const SnapshotInfo& details,
            std::function<void(absl::Status)> callback) = 0;
    virtual void getScreenshot(
            const std::string& snapshotId,
            std::function<void(absl::StatusOr<SnapshotScreenshot>)>
                    callback) = 0;
};

}  // namespace control
}  // namespace emulation
}  // namespace android

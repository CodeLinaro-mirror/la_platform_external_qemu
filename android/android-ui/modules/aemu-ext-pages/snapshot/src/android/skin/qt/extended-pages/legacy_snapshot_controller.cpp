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
#include "android/skin/qt/extended-pages/legacy_snapshot_controller.h"

#include <chrono>
#include <fstream>
#include <iterator>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "aemu/base/async/ThreadLooper.h"
#include "android/console.h"
#include "android/crashreport/CrashReporter.h"
#include "android/crashreport/HangDetector.h"
#include "android/emulation/control/LineConsumer.h"
#include "android/snapshot/Snapshot.h"
#include "android/snapshot/SnapshotUtils.h"
#include "android/snapshot/Snapshotter.h"
#include "google/protobuf/timestamp.pb.h"
#include "google/protobuf/util/time_util.h"
#include "snapshot.pb.h"

namespace android {
namespace emulation {
namespace control {

namespace {
SnapshotStatus toSnapshotStatus(bool isLoaded, bool isValid) {
    if (isLoaded) {
        return SnapshotStatus::Loaded;
    }
    if (isValid) {
        return SnapshotStatus::Compatible;
    }
    return SnapshotStatus::Incompatible;
}
}  // namespace

LegacySnapshotController::LegacySnapshotController() = default;

LegacySnapshotController::~LegacySnapshotController() = default;

void LegacySnapshotController::listSnapshots(
        std::function<void(absl::StatusOr<std::vector<SnapshotInfo>>)>
                callback) {
    android::base::ThreadLooper::runOnMainLooper([callback] {
        std::vector<SnapshotInfo> snapshots;
        for (auto& snapshot : snapshot::Snapshot::getExistingSnapshots()) {
            auto protobuf = snapshot.getGeneralInfo();
            if (protobuf) {
                google::protobuf::Timestamp creation_time_proto;
                creation_time_proto.set_seconds(protobuf->creation_time());
                creation_time_proto.set_nanos(0);

                snapshots.push_back(
                        {.snapshot_id = std::string(snapshot.name().data()),
                         .status = toSnapshotStatus(snapshot.isLoaded(),
                                                    snapshot.checkValid(false)),
                         .creation_time =
                                 std::chrono::system_clock::from_time_t(
                                         google::protobuf::util::TimeUtil::
                                                 TimestampToTimeT(
                                                         creation_time_proto)),
                         .logical_name = protobuf->logical_name(),
                         .description = protobuf->description(),
                         .size = snapshot.folderSize()});
            }
        }
        callback(snapshots);
    });
}

void LegacySnapshotController::loadSnapshot(
        const std::string& snapshotId,
        const std::string& destination,
        std::function<void(absl::Status)> callback) {
    android::base::ThreadLooper::runOnMainLooper([id = snapshotId,
                                                  callback] {
        auto snapshot = snapshot::Snapshot::getSnapshotById(id);
        if (!snapshot.hasValue()) {
            callback(absl::NotFoundError("Snapshot not found"));
            return;
        }

        LineConsumer slc;
        bool snapshot_success = false;

        // Put an extra pause in hang detector.
        // Snapshotter already calls a hang detector pause. But it is not
        // enough for imported snapshots, because it performs extra steps
        // (rebase snasphot) before the snapshotter pause. So it would
        // require an extra pause here.
        crashreport::CrashReporter::get()->hangDetector().pause(true);
        snapshot_success = getConsoleAgents()->vm->snapshotLoad(
                snapshot->name().data(), slc.opaque(), LineConsumer::Callback);
        crashreport::CrashReporter::get()->hangDetector().pause(false);
        if (!snapshot_success) {
            std::string error = absl::StrJoin(slc.lines(), "\n");
            callback(absl::InternalError(
                    absl::StrCat("Failed to load snapshot due to: ", error)));
        } else {
            callback(absl::OkStatus());
        }
    });
}

void LegacySnapshotController::saveSnapshot(
        const std::string& snapshotId,
        std::function<void(absl::Status)> callback) {
    android::base::ThreadLooper::runOnMainLooper([id = snapshotId, callback] {
        auto snapshot = snapshot::Snapshot::getSnapshotById(id);
        if (snapshot) {
            callback(absl::AlreadyExistsError("Snapshot with id " + id +
                                              " already exists!"));
            return;
        }

        using android::snapshot::Snapshotter;
        Snapshotter::get().stopVulkanAppsIfApplicable();

        LineConsumer slc;
        bool snapshot_success = false;
        snapshot_success = getConsoleAgents()->vm->snapshotSave(
                id.c_str(), slc.opaque(), LineConsumer::Callback);

        if (!snapshot_success) {
            std::string error = absl::StrJoin(slc.lines(), "\n");
            callback(absl::InternalError(
                    absl::StrCat("Failed to save snapshot due to: ", error)));
        } else {
            callback(absl::OkStatus());
        }
    });
}

void LegacySnapshotController::deleteSnapshot(
        const std::string& snapshotId,
        std::function<void(absl::Status)> callback) {
    android::base::ThreadLooper::runOnMainLooper([id = snapshotId, callback] {
        snapshot::Snapshotter::get().deleteSnapshot(id.c_str());
        callback(absl::OkStatus());
    });
}

void LegacySnapshotController::updateSnapshot(
        const SnapshotInfo& details,
        std::function<void(absl::Status)> callback) {
    android::base::ThreadLooper::runOnMainLooper([details, callback] {
        auto snapshot = snapshot::Snapshot::getSnapshotById(details.snapshot_id);
        if (!snapshot.hasValue()) {
            callback(absl::NotFoundError("Snapshot with id " +
                                         details.snapshot_id +
                                         " does not exist."));
            return;
        }

        emulator_snapshot::Snapshot* info =
                const_cast<emulator_snapshot::Snapshot*>(
                        snapshot->getGeneralInfo());
        if (!info) {
            callback(absl::InternalError("Unable to retrieve general info for " +
                                         details.snapshot_id));
            return;
        }

        info->set_description(details.description);
        info->set_logical_name(details.logical_name);

        if (!snapshot->writeSnapshotToDisk()) {
            callback(absl::InternalError("Unable to write snapshot to disk for " +
                                         details.snapshot_id));
            return;
        }

        callback(absl::OkStatus());
    });
}

void LegacySnapshotController::getScreenshot(
        const std::string& snapshotId,
        std::function<void(absl::StatusOr<SnapshotScreenshot>)> callback) {
    android::base::ThreadLooper::runOnMainLooper([id = snapshotId, callback] {
        auto snapshot = snapshot::Snapshot::getSnapshotById(id);
        if (!snapshot.hasValue()) {
            callback(absl::NotFoundError("Snapshot not found"));
            return;
        }
        std::ifstream file(snapshot->screenshot(), std::ios::binary);
        if (!file) {
            callback(absl::InternalError(
                    "Unable to open screenshot for snapshot: " + id));
            return;
        }
        auto screenshot = std::vector<char>(
                (std::istreambuf_iterator<char>(file)),
                std::istreambuf_iterator<char>());
        callback(SnapshotScreenshot{.format = SnapshotScreenshot::Format::PNG,
                                    .data = std::move(screenshot)});
    });
}

}  // namespace control
}  // namespace emulation
}  // namespace android
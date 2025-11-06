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

#include "android/skin/qt/extended-pages/snapshot_controller.h"

namespace android {
namespace emulation {
namespace control {

class LegacySnapshotController : public SnapshotController {
public:
    LegacySnapshotController();
    ~LegacySnapshotController() override;

    void listSnapshots(
            std::function<void(absl::StatusOr<std::vector<SnapshotInfo>>)> callback)
            override;
    void loadSnapshot(const std::string& snapshotId,
                      const std::string& destination,
                      std::function<void(absl::Status)> callback) override;
    void saveSnapshot(const std::string& snapshotId,
                      std::function<void(absl::Status)> callback) override;
    void deleteSnapshot(const std::string& snapshotId,
                        std::function<void(absl::Status)> callback) override;
    void updateSnapshot(const SnapshotInfo& details,
                        std::function<void(absl::Status)> callback) override;
    void getScreenshot(const std::string& snapshotId,
                       std::function<void(absl::StatusOr<SnapshotScreenshot>)> callback)
            override;
};

}  // namespace control
}  // namespace emulation
}  // namespace android

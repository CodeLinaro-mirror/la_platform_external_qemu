// Copyright 2026 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

#include "webcam_source.h"

#include "absl/types/optional.h"

#include <memory>

namespace android {
namespace ver {

class LinuxImpl : public WebcamSource::Impl {
public:
    explicit LinuxImpl(std::shared_ptr<const WebcamSource::WebcamInfo> info)
        : webcam_info_(std::move(info)) {}

    int Start(uint32_t pixel_format,
              int frame_width,
              int frame_height) override {
        return 0;
    }

    int Stop() override { return 0; }

    absl::StatusOr<std::optional<RawImageToken>> UpdateImage(
            int64_t target_time_us,
            std::optional<RawImageToken> token,
            std::function<absl::Status(const RawImageBufferView*)> updater)
            override {
        return std::nullopt;
    }

private:
    std::shared_ptr<const WebcamSource::WebcamInfo> webcam_info_;
};

std::unique_ptr<WebcamSource::Impl> CreatePlatformWebcamImpl(
        std::shared_ptr<const WebcamSource::WebcamInfo> info) {
    return std::make_unique<LinuxImpl>(std::move(info));
}

std::vector<std::shared_ptr<WebcamSource::WebcamInfo>>
WebcamSource::EnumerateWebcams() {
    return {};
}

}  // namespace ver
}  // namespace android
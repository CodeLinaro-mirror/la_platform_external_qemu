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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <array>
#include <memory>
#include <string>
#include <vector>

#include "../fourcc_utils.h"
#include "aemu/base/logging/Log.h"
#include "android/camera/camera-common.h"
#include "ver/virtual_environment_renderer_types.h"

namespace android {
namespace ver {

std::mutex WebcamSource::s_webcam_info_list_lock_;
std::optional<std::vector<std::shared_ptr<WebcamSource::WebcamInfo>>>
        WebcamSource::s_webcam_info_list_;

WebcamSource::WebcamSource(std::shared_ptr<WebcamInfo> info)
    : webcam_info_(std::move(info)),
      impl_(CreatePlatformWebcamImpl(webcam_info_)) {}

WebcamSource::~WebcamSource() {
    if (webcam_info_) {
        bool expected = true;
        if (!webcam_info_->in_use.compare_exchange_strong(expected, false)) {
            derror("Webcam '%s' freed, but was not marked as in use!",
                   webcam_info_->friendly_name.c_str());
        }
    }
}

int WebcamSource::Start(VerImageFormat ver_image_format,
                        int frame_width,
                        int frame_height) {
    if (ver_image_format != VerImageFormat::RGBA8) {
        derror("Scene system currently only supports RGBA8");
        return -1;
    }
    return impl_->Start(VerImageFormatToFourcc(ver_image_format), frame_width,
                        frame_height);
}

absl::StatusOr<std::optional<RawImageToken>> WebcamSource::UpdateImage(
        int64_t target_time_us,
        std::optional<RawImageToken> token,
        std::function<absl::Status(const RawImageBufferView*)> updater) {
    return impl_->UpdateImage(target_time_us, token, updater);
}

int WebcamSource::Stop() {
    return impl_->Stop();
}

int WebcamSource::GetPreferredFormatIndex(
        std::vector<WebcamPixelFormat>& formats) {
    /* Preferred pixel formats arranged from the most to the least desired.
     *
     * More than anything else this array is defined by the existence of format
     * conversion between the camera supported formats, and formats that are
     * supported by camera framework in the guest system. Currently, guest
     * supports only YV12 pixel format for data, and RGB32 for preview. So, this
     * array should contain only those formats, for which converters are
     * implemented. Generally speaking, the order in which entries should be
     * arranged in this array matters only as far as conversion speed is
     * concerned. So, formats with the fastest converters should be put closer
     * to the top of the array, while slower ones should be put closer to the
     * bottom. But as far as functionality is concerned, the order doesn't
     * matter, and any format can be placed anywhere in this array, as long as
     * conversion for it exists.
     */
    static constexpr std::array<uint32_t, 14> _preferred_formats = {
            /* Native format for the emulated camera: no conversion at all. */
            V4L2_PIX_FMT_YUV420, V4L2_PIX_FMT_YVU420,
            /* Continue with YCbCr: less math than with RGB */
            V4L2_PIX_FMT_NV12, V4L2_PIX_FMT_NV21, V4L2_PIX_FMT_YUYV,
            V4L2_PIX_FMT_UYVY, V4L2_PIX_FMT_YUY2, V4L2_PIX_FMT_YUNV,
            V4L2_PIX_FMT_V422,
            /* End with RGB. */
            V4L2_PIX_FMT_RGB32, V4L2_PIX_FMT_RGB24, V4L2_PIX_FMT_ARGB32,
            V4L2_PIX_FMT_BGR32, V4L2_PIX_FMT_BGR24};

    for (auto preferred : _preferred_formats) {
        for (int i = 0; i < (int)formats.size(); i++) {
            if (preferred == formats[i].pixel_format) {
                return i;
            }
        }
    }

    /* No supported format found */
    return -1;
}

std::unique_ptr<WebcamSource> WebcamSource::Create(
        std::string_view camera_arg) {
    std::lock_guard<std::mutex> lock(s_webcam_info_list_lock_);
    if (!s_webcam_info_list_) {
        s_webcam_info_list_ = EnumerateWebcams();
    }
    if (s_webcam_info_list_->empty()) {
        dwarning("No webcams detected");
        return nullptr;
    }
    for (const std::shared_ptr<WebcamInfo>& info : *s_webcam_info_list_) {
        if (info->os_alias == camera_arg || info->os_name == camera_arg) {
            bool expected = false;
            if (info->in_use.compare_exchange_strong(expected, true)) {
                return std::unique_ptr<WebcamSource>(new WebcamSource(info));
            } else {
                derror("Webcam '%s' already in use!",
                       info->friendly_name.c_str());
                return nullptr;
            }
        }
    }
    derror("No webcam found matching '%.*s'!",
           static_cast<int>(camera_arg.length()), camera_arg.data());
    return nullptr;
}

WebcamSource::Resolution
WebcamSource::WebcamPixelFormat::FindBestMatchForResolution(
        WebcamSource::Resolution requested) {
    unsigned int w = 0;
    unsigned int h = 0;
    // If current resolution is greater than
    // threshold_numerator/threshold_denomanator times the requested value user
    // a smaller one instead
    constexpr int threshold_numerator = 3;
    constexpr int threshold_denomanator = 2;

    // The list of resolutions is sorted by number of pixels, so the first value
    // we come across that is larger than the target will be the first one
    // larger than it
    for (const WebcamResolutions& res : this->resolutions) {
        if (res.resolution.height * res.resolution.width *
                    threshold_denomanator >
            threshold_numerator * requested.width * requested.height) {
            // We've gone too large. Use the previous resolution if there is one
            if (w == 0 && h == 0) {
                return {res.resolution.width, res.resolution.height};
            }
            return {w, h};
        }
        if (res.resolution.width >= requested.width &&
            res.resolution.height >= requested.height) {
            // This resolution is suitable to support the target
            return {res.resolution.width, res.resolution.height};
        }

        w = res.resolution.width;
        h = res.resolution.height;
    }

    return {w, h};
}

size_t WebcamSource::GetWebcamCount() {
    std::lock_guard<std::mutex> lock(s_webcam_info_list_lock_);
    if (!s_webcam_info_list_) {
        s_webcam_info_list_ = EnumerateWebcams();
    }
    return s_webcam_info_list_->size();
}

std::shared_ptr<WebcamSource::WebcamInfo> WebcamSource::GetWebcamInfo(
        size_t index) {
    std::lock_guard<std::mutex> lock(s_webcam_info_list_lock_);
    if (!s_webcam_info_list_) {
        s_webcam_info_list_ = EnumerateWebcams();
    }
    if (index >= s_webcam_info_list_->size()) {
        return nullptr;
    }
    return (*s_webcam_info_list_)[index];
}

}  // namespace ver
}  // namespace android

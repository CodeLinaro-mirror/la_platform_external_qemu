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
#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include "absl/status/status.h"
#include "absl/status/statusor.h"

struct RawImageBuffer {
    uint8_t *buffer;
    size_t buffer_size;
    uint32_t pixel_format;
    int width;
    int height;
};

struct RawImageToken {
    int64_t token;
};

class RawImageSource {
public:
    /* The arguments are suggestions, must check Image for resulting values */
    virtual int Start(uint32_t pixel_format, int width, int height) = 0;
    /* Updates the current image using the given updater function.
     * target_time_us is the point within any animation that we would like an
     * image for token should be the previous token provided by this function.
     * It is used to determine if there is a new image available.
     *
     * Returns the error value, or the new token if the image was updated.
     * If the image was not updated, the caller should retain the previous
     * token.
     */
    virtual absl::StatusOr<std::optional<RawImageToken>> UpdateImage(
            int64_t target_time_us,
            std::optional<RawImageToken> token,
            std::function<absl::Status(const RawImageBuffer*)> updater) = 0;
    virtual int Stop() = 0;
    /* This is the rotation that must be applied to the images produced by this
     * source to have them oriented in the natural way.
     */
    virtual int GetBaseRotation() { return 0; }
    /* This returns the length of any animation from the source,
     * or 0 if there is no meaninful finite length
     */
    virtual int64_t GetAnimationLengthUs() { return 0; }
};

// DefaultImageProvider provides a 1x1 magenta image to serve as a default when
// the configuration fails.
class DefaultRawImageProvider : public RawImageSource {
public:
    DefaultRawImageProvider() = default;
    int Start(uint32_t pixel_format, int width, int height) override;
    absl::StatusOr<std::optional<RawImageToken>> UpdateImage(
            int64_t target_time_us,
            std::optional<RawImageToken> token,
            std::function<absl::Status(const RawImageBuffer*)> updater)
            override;
    int Stop() override;
};
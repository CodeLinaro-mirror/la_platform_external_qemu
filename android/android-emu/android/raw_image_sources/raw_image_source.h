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
#include <functional>
#include <memory>
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
    /* The timestamp for the next frame. This is used by the source to infer
     * video control type operations
     */
    virtual void UpdateTime(int64_t nextTimeUs) {};
    /* Checks if there is an update to the image, provided a token. The token is
     * an opaque value provided from a previous call to AccessImage. For the
     * first call, users should provide 0 here.
     */
    virtual bool HasUpdate(RawImageToken token) = 0;
    /* Provides access to an image, to be used only within the provided accessor
     * function. returns a negative value on error, or a token to be provided to
     * HasUpdate
     */
    virtual absl::StatusOr<RawImageToken> AccessImage(
            std::function<absl::Status(RawImageBuffer*)> accessor) = 0;
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
    bool HasUpdate(RawImageToken token) override;
    absl::StatusOr<RawImageToken> AccessImage(
            std::function<absl::Status(RawImageBuffer*)> accessor) override;
    int Stop() override;
};
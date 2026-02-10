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

struct RawImageBuffer {
    uint8_t *buffer;
    size_t buffer_size;
    uint32_t pixel_format;
    int width;
    int height;
};

class RawImageSource {
public:
    /* The arguments are suggestions, must check Image for resulting values */
    virtual int Start(uint32_t pixel_format, int width, int height) = 0;
    /* Image is only guaranteed to be valid within the scope of the accessor
     * function */
    virtual int AccessImage(std::function<int(RawImageBuffer*)> accessor) = 0;
    virtual int Stop() = 0;
};

// DefaultImageProvider provides a 1x1 magenta image to serve as a default when
// the configuration fails.
class DefaultRawImageProvider : public RawImageSource {
public:
    DefaultRawImageProvider() = default;
    int Start(uint32_t pixel_format, int width, int height) override;
    int AccessImage(std::function<int(RawImageBuffer*)> accessor) override;
    int Stop() override;
};
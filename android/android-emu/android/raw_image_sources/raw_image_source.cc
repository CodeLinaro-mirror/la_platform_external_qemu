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
#include "android/raw_image_sources/raw_image_source.h"
#include <cstdint>

#include "android/camera/camera-common.h"

static uint8_t defaultImageData[16] = {
    0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0xFF,
    0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0xFF
};

static RawImageBuffer defaultImage = {defaultImageData, sizeof(defaultImageData),
                             V4L2_PIX_FMT_RGB32, 2, 2};

int DefaultRawImageProvider::Start(uint32_t pixel_format,
                                        int width,
                                        int height) {
    return 0;
}
int DefaultRawImageProvider::AccessImage(std::function<int(RawImageBuffer*)> accessor) {
    return accessor(&defaultImage);
}
int DefaultRawImageProvider::Stop() {
    return 0;
}
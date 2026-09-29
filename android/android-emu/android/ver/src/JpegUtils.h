/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include <jpeglib.h>
}

namespace android {
namespace ver {

enum class JpegOrientation {
    TopDown,
    BottomUp,
};

struct JpegDecompressOptions {
    J_COLOR_SPACE color_space = JCS_RGBA_8888;
    JpegOrientation orientation = JpegOrientation::TopDown;
    bool align_row_stride = false;  // Align row stride to 4-byte boundary
    bool save_app1_marker = false;  // Save APP1 marker data (e.g. EXIF metadata)
};

struct JpegImageInfo {
    int width = 0;
    int height = 0;
    int num_components = 0;
    size_t line_size = 0;
    size_t total_size = 0;
    std::vector<std::vector<uint8_t>> app1_markers;
};

class JpegUtils {
public:
    // Decompresses a JPEG image from an in-memory buffer.
    // Reuses out_buffer if it already has sufficient capacity (zero heap
    // allocations for streaming).
    static bool DecompressFromMemory(const uint8_t* jpeg_data,
                                     size_t jpeg_size,
                                     std::vector<uint8_t>& out_buffer,
                                     JpegImageInfo* out_info = nullptr,
                                     const JpegDecompressOptions& options = {});

    // Decompresses a JPEG image from a file on disk.
    static bool DecompressFromFile(const std::string& filepath,
                                   std::vector<uint8_t>& out_buffer,
                                   JpegImageInfo* out_info = nullptr,
                                   const JpegDecompressOptions& options = {});
};

}  // namespace ver
}  // namespace android

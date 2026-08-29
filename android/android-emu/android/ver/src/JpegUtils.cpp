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

#include "JpegUtils.h"

#include <csetjmp>
#include <cstddef>
#include <limits>
#include <memory>

#include "aemu/base/files/ScopedStdioFile.h"
#include "aemu/base/logging/Log.h"
#include "android/utils/debug.h"
#include "android/utils/file_io.h"

namespace android {
namespace ver {

namespace {

struct JpegErrorManager {
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
};

void jpeg_error_exit(j_common_ptr cinfoPtr) {
    auto* myerr = reinterpret_cast<JpegErrorManager*>(cinfoPtr->err);
    longjmp(myerr->setjmp_buffer, 1);
}

void initSource(j_decompress_ptr cinfo) {}

boolean fillInputBuffer(j_decompress_ptr cinfo) {
    static const JOCTET kEoiBuffer[2] = {0xFF, JPEG_EOI};
    cinfo->src->next_input_byte = kEoiBuffer;
    cinfo->src->bytes_in_buffer = 2;
    return TRUE;
}

void skipInputData(j_decompress_ptr cinfo, long numBytes) {
    if (numBytes > 0) {
        if (static_cast<size_t>(numBytes) > cinfo->src->bytes_in_buffer) {
            cinfo->src->next_input_byte += cinfo->src->bytes_in_buffer;
            cinfo->src->bytes_in_buffer = 0;
        } else {
            cinfo->src->next_input_byte += static_cast<size_t>(numBytes);
            cinfo->src->bytes_in_buffer -= static_cast<size_t>(numBytes);
        }
    }
}

void termSource(j_decompress_ptr cinfo) {}

void jpeg_mem_src_custom(j_decompress_ptr cinfo,
                         const uint8_t* buffer,
                         size_t size) {
    if (cinfo->src == nullptr) {
        cinfo->src =
                static_cast<struct jpeg_source_mgr*>((*cinfo->mem->alloc_small)(
                        reinterpret_cast<j_common_ptr>(cinfo), JPOOL_PERMANENT,
                        sizeof(struct jpeg_source_mgr)));
    }
    cinfo->src->init_source = initSource;
    cinfo->src->fill_input_buffer = fillInputBuffer;
    cinfo->src->skip_input_data = skipInputData;
    cinfo->src->resync_to_restart = jpeg_resync_to_restart;
    cinfo->src->term_source = termSource;
    cinfo->src->bytes_in_buffer = size;
    cinfo->src->next_input_byte = reinterpret_cast<const JOCTET*>(buffer);
}

inline size_t alignTo4Bytes(size_t n) {
    return (n + 3) & ~static_cast<size_t>(3);
}

struct JpegSource {
    const uint8_t* mem_data = nullptr;
    size_t mem_size = 0;
    FILE* file = nullptr;
    const char* filename = nullptr;
};

bool DecompressInternal(const JpegSource& source,
                        std::vector<uint8_t>& out_buffer,
                        JpegImageInfo* out_info,
                        const JpegDecompressOptions& options) {
    struct jpeg_decompress_struct cinfo = {};
    JpegErrorManager jerr;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_exit;

    // All cpp classes must be declared before the setjmp to ensure their
    // destructors are called on error exit.
    std::unique_ptr<jpeg_decompress_struct, decltype(&jpeg_destroy_decompress)>
            cinfo_guard(&cinfo, &jpeg_destroy_decompress);

    if (setjmp(jerr.setjmp_buffer)) {
        char buffer[JMSG_LENGTH_MAX];
        (*cinfo.err->format_message)(
                reinterpret_cast<jpeg_common_struct*>(&cinfo), buffer);
        if (source.filename) {
            derror("%s: JPEG library error for %s: %s", __FUNCTION__,
                   source.filename, buffer);
        } else {
            derror("%s: JPEG library error: %s", __FUNCTION__, buffer);
        }
        return false;
    }

    jpeg_create_decompress(&cinfo);

    // Increase libjpeg memory limit to 256 MB to prevent failures when
    // loading large images.
    cinfo.mem->max_memory_to_use = static_cast<long>(256 * 1024 * 1024);

    if (source.file) {
        jpeg_stdio_src(&cinfo, source.file);
    } else {
        jpeg_mem_src_custom(&cinfo, source.mem_data, source.mem_size);
    }

    if (options.save_app1_marker) {
        jpeg_save_markers(&cinfo, JPEG_APP0 + 1, 0xFFFF);
    }

    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        return false;
    }

    if (options.save_app1_marker && out_info) {
        jpeg_saved_marker_ptr marker = cinfo.marker_list;
        while (marker != nullptr) {
            if (marker->marker == JPEG_APP0 + 1) {
                out_info->app1_markers.emplace_back(
                        marker->data, marker->data + marker->data_length);
            }
            marker = marker->next;
        }
    }

    cinfo.out_color_space = options.color_space;
    if (!jpeg_start_decompress(&cinfo)) {
        return false;
    }

    int width = cinfo.output_width;
    int height = cinfo.output_height;
    int components = cinfo.output_components;

    if (width <= 0 || height <= 0 || width > 16384 || height > 16384 ||
        components <= 0 || components > 4) {
        if (source.filename) {
            dwarning("%s: Invalid JPEG dimensions %dx%d or components %d for %s",
                     __FUNCTION__, width, height, components, source.filename);
        } else {
            dwarning("%s: Invalid JPEG dimensions %dx%d or components %d",
                     __FUNCTION__, width, height, components);
        }
        return false;
    }

    size_t raw_line_size = static_cast<size_t>(width) * components;
    size_t stride = options.align_row_stride ? alignTo4Bytes(raw_line_size)
                                             : raw_line_size;

    if (height > 0 && stride > std::numeric_limits<size_t>::max() / height) {
        if (source.filename) {
            dwarning("%s: Image data allocation size overflow for %s",
                     __FUNCTION__, source.filename);
        } else {
            dwarning("%s: Image data allocation size overflow", __FUNCTION__);
        }
        return false;
    }

    size_t total_bytes = stride * height;
    out_buffer.resize(total_bytes);

    while (cinfo.output_scanline < cinfo.output_height) {
        uint8_t* row_ptr;
        if (options.orientation == JpegOrientation::BottomUp) {
            row_ptr = out_buffer.data() +
                      (height - cinfo.output_scanline - 1) * stride;
        } else {
            row_ptr = out_buffer.data() + cinfo.output_scanline * stride;
        }
        if (jpeg_read_scanlines(&cinfo, &row_ptr, 1) == 0) {
            if (source.filename) {
                dwarning("%s: Stalled scanline read at line %u / %u for %s",
                         __FUNCTION__, cinfo.output_scanline,
                         cinfo.output_height, source.filename);
            } else {
                dwarning("%s: Stalled scanline read at line %u / %u",
                         __FUNCTION__, cinfo.output_scanline,
                         cinfo.output_height);
            }
            return false;
        }
    }

    (void)jpeg_finish_decompress(&cinfo);

    if (out_info) {
        out_info->width = width;
        out_info->height = height;
        out_info->num_components = components;
        out_info->line_size = stride;
        out_info->total_size = total_bytes;
    }

    return true;
}

}  // namespace

bool JpegUtils::DecompressFromMemory(const uint8_t* jpeg_data,
                                     size_t jpeg_size,
                                     std::vector<uint8_t>& out_buffer,
                                     JpegImageInfo* out_info,
                                     const JpegDecompressOptions& options) {
    if (!jpeg_data || jpeg_size == 0) {
        return false;
    }
    JpegSource source;
    source.mem_data = jpeg_data;
    source.mem_size = jpeg_size;
    return DecompressInternal(source, out_buffer, out_info, options);
}

bool JpegUtils::DecompressFromFile(const std::string& filepath,
                                   std::vector<uint8_t>& out_buffer,
                                   JpegImageInfo* out_info,
                                   const JpegDecompressOptions& options) {
    base::ScopedStdioFile fp(android_fopen(filepath.c_str(), "rb"));
    if (!fp) {
        derror("%s: Failed to open file %s", __FUNCTION__, filepath.c_str());
        return false;
    }
    JpegSource source;
    source.file = fp.get();
    source.filename = filepath.c_str();
    return DecompressInternal(source, out_buffer, out_info, options);
}

}  // namespace ver
}  // namespace android

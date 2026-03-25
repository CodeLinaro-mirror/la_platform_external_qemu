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
#include "android/raw_image_sources/image_file/raw_image_file_source.h"

#include <png.h>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>
#include "absl/status/status.h"
#include "aemu/base/logging/Log.h"
#include "android/raw_image_sources/raw_image_source.h"

// jpeglib.h needs to be included. It's a C library.
extern "C" {
#include <jpeglib.h>
}

#include "aemu/base/files/PathUtils.h"
#include "aemu/base/files/ScopedStdioFile.h"
#include "android/camera/camera-common.h"
#include "android/utils/debug.h"
#include "android/utils/file_io.h"

using android::base::Optional;
using android::base::PathUtils;
using android::base::ScopedStdioFile;

static void pngWarningCallback(png_structp readPtr,
                               png_const_charp warningMessage) {
    dprint("%s: %s\n", __FUNCTION__, warningMessage);
}

template <typename T>
static inline T alignRowBytes(T value) {
    return (value + 3) / 4 * 4;
}

std::optional<ImageData> loadPNGImage(std::string& filename) {
    ScopedStdioFile fp(android_fopen(filename.c_str(), "rb"));
    if (!fp) {
        derror("%s: Failed to open file %s", __FUNCTION__, filename);
        return {};
    }

    uint8_t header[8];
    if (fread(header, sizeof(header), 1, fp.get()) != 1) {
        derror("%s: Failed to read header", __FUNCTION__);
        return {};
    }

    if (png_sig_cmp(header, 0, sizeof(header))) {
        derror("%s: header is not a PNG header", __FUNCTION__);
        return {};
    }

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, 0, 0, 0);
    if (!png) {
        derror("%s: Failed to allocate png read struct", __FUNCTION__);
        return {};
    }

    png_infop pngInfo = png_create_info_struct(png);
    if (!pngInfo) {
        derror("%s: Failed to allocate png info struct", __FUNCTION__);
        return {};
    }

    png_set_error_fn(png, nullptr, nullptr, pngWarningCallback);

    if (setjmp(png_jmpbuf(png))) {
        derror("%s: PNG library error", __FUNCTION__);
        png_destroy_read_struct(&png, &pngInfo, 0);
        return {};
    }

    png_init_io(png, fp.get());
    png_set_sig_bytes(png, 8);

    png_read_info(png, pngInfo);

    png_uint_32 width = 0;
    png_uint_32 height = 0;
    int bitDepth = 0;
    int colorType = 0;
    png_get_IHDR(png, pngInfo, &width, &height, &bitDepth, &colorType, nullptr,
                 nullptr, nullptr);
    dprint("%s: Loaded PNG %s, %dx%d (d=%d, c=%d)", __FUNCTION__, filename,
           width, height, bitDepth, colorType);

    // Convert to RGB if required.
    if (colorType == PNG_COLOR_TYPE_PALETTE) {
        png_set_palette_to_rgb(png);
    } else if (colorType == PNG_COLOR_TYPE_GRAY ||
               colorType == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png);
    }

    if (png_get_valid(png, pngInfo, PNG_INFO_tRNS)) {
        // Convert palette and grayscale transparency to full alpha channel.
        png_set_tRNS_to_alpha(png);
    }

    // If we still don't have an alpha channel, add a full alpha channel.
    if (!(colorType & PNG_COLOR_MASK_ALPHA) &&
        !png_get_valid(png, pngInfo, PNG_INFO_tRNS)) {
        png_set_add_alpha(png, 0xFF, PNG_FILLER_AFTER);
    }

    // At this point, the bit depth is either 8 or 16, ensure 8 bpp.
    if (bitDepth == 16) {
        png_set_strip_16(png);
    }

    // Update the pngInfo struct and validate that we have a format that we can
    // handle.
    png_read_update_info(png, pngInfo);

    const int newBitDepth = png_get_bit_depth(png, pngInfo);
    const int newColorType = png_get_color_type(png, pngInfo);
    if (newBitDepth != bitDepth || newColorType != colorType) {
        dprint("%s: Converting PNG to (d=%d, c=%d)", __FUNCTION__, newBitDepth,
               newColorType);
    }

    if (newColorType != PNG_COLOR_TYPE_RGB_ALPHA) {
        derror("%s: Unsupported color type: %d", __FUNCTION__, newColorType);
        png_destroy_read_struct(&png, &pngInfo, 0);
        return {};
    }

    const size_t rowBytes = png_get_rowbytes(png, pngInfo);
    const size_t stride = alignRowBytes(rowBytes);
    std::vector<uint8_t> data(stride * height);
    std::vector<uint8_t*> rowPtrs(height);

    for (size_t i = 0; i < height; i++) {
        rowPtrs[i] = data.data() + stride * i;
    }

    png_read_image(png, rowPtrs.data());
    png_destroy_read_struct(&png, &pngInfo, 0);

    ImageData img;
    img.data = std::move(data);
    img.data_ptr = &img.data[0];
    img.width = width;
    img.height = height;
    img.num_components = 4;
    img.line_size = img.width * img.num_components;

    return img;
}

std::optional<ImageData> loadJPEGImage(std::string& filename) {
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;

    ScopedStdioFile fp(android_fopen(filename.c_str(), "rb"));
    if (!fp) {
        derror("%s: Failed to open file %s", __FUNCTION__, filename);
        return std::nullopt;
    }

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);

    auto destroy_decompress = [](j_decompress_ptr cinfo_ptr) {
        if (cinfo_ptr)
            jpeg_destroy_decompress(cinfo_ptr);
    };
    std::unique_ptr<jpeg_decompress_struct, decltype(destroy_decompress)>
            cinfo_ptr(&cinfo, destroy_decompress);

    jpeg_stdio_src(&cinfo, fp.get());
    (void)jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGBA_8888;  // Force RGBA format.
    (void)jpeg_start_decompress(&cinfo);

    const uint32_t width = cinfo.output_width;
    const uint32_t height = cinfo.output_height;

    ImageData img;
    img.width = cinfo.output_width;
    img.height = cinfo.output_height;
    img.num_components = cinfo.output_components;
    img.line_size = img.width * img.num_components;

    img.data.resize(img.width * img.height * img.num_components);

    while (cinfo.output_scanline < cinfo.output_height) {
        // buffer is a pointer to the start of the current row in our vector
        unsigned char* buffer =
                &img.data[cinfo.output_scanline * img.line_size];
        (void)jpeg_read_scanlines(&cinfo, &buffer, 1);
    }

    (void)jpeg_finish_decompress(&cinfo);
    img.data_ptr = &img.data[0];
    return img;
}

std::optional<ImageData> loadImageFromFile(std::string& filename) {
    const std::string filename_str{filename};
    const std::string_view extension{PathUtils::extension(filename_str)};

    if (strncasecmp(extension.data(), ".png", extension.size()) == 0) {
        return loadPNGImage(filename);
    } else if (strncasecmp(extension.data(), ".jpg", extension.size()) == 0 ||
               strncasecmp(extension.data(), ".jpeg", extension.size()) == 0) {
        return loadJPEGImage(filename);
    } else {
        derror("%s: Unsupported file format %s", __FUNCTION__,
               android::base::c_str(extension).get());
        return std::nullopt;
    }
}

std::unique_ptr<RawImageFileSource> RawImageFileSource::Create(
        std::string filename) {
    std::optional<ImageData> maybeImage = loadImageFromFile(filename);
    if (maybeImage) {
        return std::unique_ptr<RawImageFileSource>(
                new RawImageFileSource(filename, std::move(maybeImage.value())));
    }
    return nullptr;
}

RawImageFileSource::RawImageFileSource(std::string filename, ImageData&& image)
    : file_(std::move(filename)), image_(std::move(image)) {}

int RawImageFileSource::Start(uint32_t pixel_format,
                                     int width,
                                     int height) {
    return 0;
}

bool RawImageFileSource::HasUpdate(RawImageToken token) {
    return token.token != 1;
}

absl::StatusOr<RawImageToken> RawImageFileSource::AccessImage(
        std::function<absl::Status(RawImageBuffer*)> accessor) {
    size_t buffer_size;
    uint32_t pixel_format;
    int width;
    int height;
    struct RawImageBuffer im = {image_.data_ptr,
                       static_cast<size_t>(image_.line_size) * image_.height,
                       V4L2_PIX_FMT_RGB32, image_.width, image_.height};
    absl::Status ret = accessor(&im);
    if (ret.ok()) {
        return RawImageToken{1};
    } else {
        return ret;
    }
}

int RawImageFileSource::Stop() {
    return 0;
}
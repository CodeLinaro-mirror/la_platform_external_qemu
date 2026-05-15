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
#include "raw_image_file_source.h"

#include <png.h>
#include <csetjmp>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>
#include "absl/status/status.h"

// jpeglib.h needs to be included. It's a C library.
extern "C" {
#include <jpeglib.h>
}

#include "aemu/base/Log.h"
#include "aemu/base/files/PathUtils.h"
#include "aemu/base/files/ScopedStdioFile.h"
#include "android/utils/file_io.h"

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
        derror("Failed to open file %s", filename);
        return {};
    }

    uint8_t header[8];
    if (fread(header, sizeof(header), 1, fp.get()) != 1) {
        derror("Failed to read header");
        return {};
    }

    if (png_sig_cmp(header, 0, sizeof(header))) {
        derror("header is not a PNG header");
        return {};
    }

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, 0, 0, 0);
    if (!png) {
        derror("Failed to allocate png read struct");
        return {};
    }

    png_infop pngInfo = png_create_info_struct(png);
    if (!pngInfo) {
        derror("Failed to allocate png info struct");
        return {};
    }

    png_set_error_fn(png, nullptr, nullptr, pngWarningCallback);

    // These are before setjmp to ensure the deconstructors work properly
    std::vector<uint8_t> data;
    std::vector<uint8_t*> rowPtrs;

    if (setjmp(png_jmpbuf(png))) {
        derror("PNG library error");
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
        derror("Unsupported color type: %d", newColorType);
        png_destroy_read_struct(&png, &pngInfo, 0);
        return {};
    }

    const size_t rowBytes = png_get_rowbytes(png, pngInfo);
    const size_t stride = alignRowBytes(rowBytes);
    data.resize(stride * height);
    rowPtrs.resize(height);

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

struct JpegErrorManager {
    struct jpeg_error_mgr pub;  // "public" fields
    jmp_buf setjmp_buffer;      // for return to caller
};

static void jpeg_error_exit(j_common_ptr cinfoPtr) {
    JpegErrorManager* myerr =
            reinterpret_cast<JpegErrorManager*>(cinfoPtr->err);
    longjmp(myerr->setjmp_buffer, 1);
}

std::optional<ImageData> loadJPEGImage(std::string& filename) {
    struct jpeg_decompress_struct cinfo;
    JpegErrorManager jerr;

    // All cpp classes must be declared before the setjmp to ensure thier
    // deconstructors are called.
    ImageData img;
    ScopedStdioFile fp(android_fopen(filename.c_str(), "rb"));
    if (!fp) {
        derror("Failed to open file %s", filename.c_str());
        return std::nullopt;
    }

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_exit;

    std::unique_ptr<jpeg_decompress_struct, decltype(&jpeg_destroy_decompress)>
            cinfo_guard(&cinfo, &jpeg_destroy_decompress);

    if (setjmp(jerr.setjmp_buffer)) {
        char buffer[JMSG_LENGTH_MAX];
        (*cinfo.err->format_message)(
                reinterpret_cast<jpeg_common_struct*>(&cinfo), buffer);
        derror("JPEG library error for %s: %s", filename.c_str(), buffer);
        return std::nullopt;
    }

    jpeg_create_decompress(&cinfo);

    jpeg_stdio_src(&cinfo, fp.get());
    (void)jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGBA_8888;  // Force RGBA format.
    (void)jpeg_start_decompress(&cinfo);

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
        derror("Unsupported file format %s",
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

int RawImageFileSource::Start(VerImageFormat pixel_format,
                                     int width,
                                     int height) {
    return 0;
}

absl::StatusOr<std::optional<RawImageToken>> RawImageFileSource::UpdateImage(
        int64_t target_time_us,
        std::optional<RawImageToken> token,
        std::function<absl::Status(const RawImageBufferView*)> updater) {
    if (token.has_value() && token.value().token == 1) {
        return std::nullopt;
    }
    size_t buffer_size;
    VerImageFormat pixel_format;
    int width;
    int height;
    struct RawImageBufferView im = {
            image_.data_ptr,
            static_cast<size_t>(image_.line_size) * image_.height,
            VerImageFormat::RGBA8, image_.width, image_.height};
    absl::Status ret = updater(&im);
    if (ret.ok()) {
        return RawImageToken{1};
    } else {
        return ret;
    }
}

int RawImageFileSource::Stop() {
    return 0;
}
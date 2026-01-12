/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "android/camera/camera-imagefile.h"

#include <png.h>
#include <optional>
#include <string>
#include <vector>
#include "aemu/base/logging/Log.h"

// jpeglib.h needs to be included. It's a C library.
extern "C" {
#include <jpeglib.h>
}

#include "aemu/base/files/PathUtils.h"
#include "aemu/base/files/ScopedStdioFile.h"
#include "android/camera/camera-format-converters.h"
#include "android/utils/debug.h"
#include "android/utils/file_io.h"

using android::base::Optional;
using android::base::PathUtils;
using android::base::ScopedStdioFile;

struct ImageData {
    unsigned int width;
    unsigned int height;
    int num_components;
    int line_size;
    std::vector<unsigned char> data;
    uint8_t* data_ptr;
};

static void pngWarningCallback(png_structp readPtr,
                               png_const_charp warningMessage) {
    dprint("%s: %s\n", __FUNCTION__, warningMessage);
}

template <typename T>
static inline T alignRowBytes(T value) {
    return (value + 3) / 4 * 4;
}

std::optional<ImageData> loadPNGImage(const char* filename) {
    ScopedStdioFile fp(android_fopen(filename, "rb"));
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

    if (newColorType != PNG_COLOR_TYPE_RGB &&
        newColorType != PNG_COLOR_TYPE_RGB_ALPHA) {
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
    img.num_components = (newColorType == PNG_COLOR_TYPE_RGB) ? 3 : 4;
    img.line_size = img.width * img.num_components;

    return img;
}

std::optional<ImageData> loadJPEGImage(const char* filename) {
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;

    derror("Loading: %s", filename);

    ScopedStdioFile fp(android_fopen(filename, "rb"));
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
    cinfo.out_color_space = JCS_RGB;  // Force RGB format.
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

std::optional<ImageData> loadImageFromFile(const char* filename) {
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

struct ImagefileCameraDevice {
    explicit ImagefileCameraDevice(ImageData image)
        : mImage(std::move(image)) {
        mHeader.opaque = this;
    }

    static CameraDevice* open(const char* args, int) {
        std::optional<ImageData> maybeImage = loadImageFromFile(args);
        if (maybeImage) {
            return &(new ImagefileCameraDevice(std::move(maybeImage.value())))
                            ->mHeader;
        } else {
            return nullptr;
        }
    }

    static int startCapturingStatic(CameraDevice* cd,
                                    uint32_t /*pixelFormat*/,
                                    int width,
                                    int height) {
        return myselfFrom(cd)->startCapturing();
    }

    static int readFrameStatic(CameraDevice* cd,
                               ClientFrame* frame,
                               float rScale,
                               float gScale,
                               float bScale,
                               float expComp,
                               const char* direction,
                               int sensor_orientation) {
        return myselfFrom(cd)->readFrame(*frame, rScale, gScale, bScale,
                                         expComp, direction, sensor_orientation);
    }

    static int stopCapturingStatic(CameraDevice* cd) {
        return myselfFrom(cd)->stopCapturing();
    }

    static void closeStatic(CameraDevice* cd) { delete myselfFrom(cd); }

private:
    int startCapturing() { return 0; }

    int readFrame(ClientFrame& cframe,
                  const float rScale,
                  const float gScale,
                  const float bScale,
                  const float expComp,
                  const char* direction,
                  const int sensor_orientation) {
        const bool backFacing = !strcmp(direction, "back");

        for (uint32_t i = 0; i < cframe.framebuffers_count; ++i) {
            if (const int err = convert_frame(mImage.data_ptr,
                            mImage.num_components == 3 ? V4L2_PIX_FMT_RGB24 : V4L2_PIX_FMT_RGB32,
                            mImage.line_size * mImage.height,
                            mImage.width,
                            mImage.height, &cframe,
                            rScale, gScale, bScale, expComp, direction,
                            get_coarse_orientation(sensor_orientation))) {
                return err;
            }
        }

        return 0;
    }

    int stopCapturing() {
        return 0;
    }

    static ImagefileCameraDevice* myselfFrom(CameraDevice* c) {
        return static_cast<ImagefileCameraDevice*>(c->opaque);
    }

    CameraDevice mHeader;
    ImageData mImage;
};

int camera_imagefile_init_CameraInfo(CameraInfo* ci,
                                     const char* direction,
                                     const char* args) {
    static const CameraInfoVtbl vtbl = {
        .open = &ImagefileCameraDevice::open,
        .start_capturing = &ImagefileCameraDevice::startCapturingStatic,
        .read_frame = &ImagefileCameraDevice::readFrameStatic,
        .stop_capturing = &ImagefileCameraDevice::stopCapturingStatic,
        .close = &ImagefileCameraDevice::closeStatic,
        .camera_source = kImagefile,
    };

    static const CameraFrameDim kDims[] = {
        {640, 480},
        {352, 288},
        {320, 240},
        {176, 144},
        {1280, 720},
        {1280, 960},
    };

    ci->frame_sizes = static_cast<CameraFrameDim*>(::malloc(sizeof(kDims)));
    memcpy(ci->frame_sizes, kDims, sizeof(kDims));
    ci->frame_sizes_num = sizeof(kDims) / sizeof(kDims[0]);
    ci->vtbl = &vtbl;
    ci->display_name = ::strdup(args);
    ci->device_name = ::strdup(args);
    ci->inp_channel = 0;
    ci->pixel_format = V4L2_PIX_FMT_RGB32;
    ci->direction = ::strdup(direction);
    ci->in_use = 0;

    return 0;
}

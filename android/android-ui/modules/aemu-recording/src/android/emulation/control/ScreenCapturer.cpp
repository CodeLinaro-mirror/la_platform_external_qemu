// Copyright (C) 2016 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "android/emulation/control/ScreenCapturer.h"

#include <assert.h>  // for assert
#include <png.h>     // for png_create_info...
#include <stdio.h>   // for NULL, snprintf
#include <string.h>  // for memcpy

#include <fstream>  // for ofstream, basic...
#include <memory>   // for shared_ptr
#include <optional>
#include <string_view>
#include <vector>   // for vector

#include "render-utils/Renderer.h"                    // for Renderer
#include "aemu/base/Log.h"                         // for LOG, LogMessage
#include "aemu/base/files/PathUtils.h"             // for PathUtils
#include "android/base/system/System.h"               // for System
#include "android/console.h"                          // for getConsoleAgents
#include "host-common/display_agent.h"  // for QAndroidDisplay...
#include "host-common/window_agent.h"   // for QAndroidEmulato...
#include "android/emulator-window.h"                  // for emulator_window...
#include "android/loadpng.h"                          // for savepng, write_...
#include "host-common/opengles.h"                         // for android_getOpen...
#include "android/utils/string.h"                     // for str_ends_with
#include "android/utils/debug.h"
#include "observation.pb.h"                           // for Observation
#include "pngconf.h"                                  // for png_byte, png_b...

using android::base::PathUtils;

namespace android {
namespace emulation {

// Global background image data to be applied on the CPU side when taking
// screenshots on transparent displays
static std::mutex sBackgroundImageMutex;
static std::optional<Image> sBackgroundImage;

bool captureScreenshot(const char* outputDirectoryPath,
                       std::string* pOutputFilepath,
                       uint32_t displayId) {
    const auto& renderer = android_getOpenglesRenderer();
    SkinRotation rotation = (SkinRotation)getConsoleAgents()->emu->getRotation();
    if (const auto renderer_ptr = renderer.get()) {
        return captureScreenshot(renderer_ptr, nullptr, rotation,
                                 outputDirectoryPath, pOutputFilepath,
                                 displayId);
    } else {
        // renderer is nullptr in -gpu guest
        if (displayId > 0) {
            return false;
        }
        return captureScreenshot(
                nullptr, getConsoleAgents()->display->getFrameBuffer, rotation,
                outputDirectoryPath, pOutputFilepath);
    }
}

struct RgbColor {
    uint8_t r, g, b;
};

RgbColor bilinearSample(const Image& img,
                        const float u,
                        const float v) {
    auto getPixelSafe = [](const Image& img, int x, int y) {
        x = std::max(0, std::min(img.getWidth() - 1, x));
        y = std::max(0, std::min(img.getHeight() - 1, y));
        const uint32_t pixelIndex =
                (y * img.getWidth() + x) * img.getChannels();
        return RgbColor{img.getPixelBuf()[pixelIndex + 0],
                        img.getPixelBuf()[pixelIndex + 1],
                        img.getPixelBuf()[pixelIndex + 2]};
    };

    auto lerpComponent = [](uint8_t a, uint8_t b, float t) {
        return static_cast<uint8_t>(a + t * (b - a));
    };

    const float x = u * img.getWidth();
    const float y = v * img.getHeight();
    int x0 = static_cast<int>(std::floor(x));
    int y0 = static_cast<int>(std::floor(y));
    int x1 = x0 + 1;
    int y1 = y0 + 1;

    float tx = x - x0;
    float ty = y - y0;

    RgbColor p00 = getPixelSafe(img, x0, y0);
    RgbColor p10 = getPixelSafe(img, x1, y0);
    RgbColor p01 = getPixelSafe(img, x0, y1);
    RgbColor p11 = getPixelSafe(img, x1, y1);

    uint8_t rTop = lerpComponent(p00.r, p10.r, tx);
    uint8_t gTop = lerpComponent(p00.g, p10.g, tx);
    uint8_t bTop = lerpComponent(p00.b, p10.b, tx);

    uint8_t rBottom = lerpComponent(p01.r, p11.r, tx);
    uint8_t gBottom = lerpComponent(p01.g, p11.g, tx);
    uint8_t bBottom = lerpComponent(p01.b, p11.b, tx);

    RgbColor result;
    result.r = lerpComponent(rTop, rBottom, ty);
    result.g = lerpComponent(gTop, gBottom, ty);
    result.b = lerpComponent(bTop, bBottom, ty);

    return result;
}

AEMU_EXPORT bool setScreenshotBackground(const int width,
                                         const int height,
                                         const int numChannels,
                                         const uint8_t* pixelData) {
    std::lock_guard<std::mutex> guard(sBackgroundImageMutex);
    if (pixelData == nullptr) {
        // Can be used to reset
        sBackgroundImage = std::nullopt;
        return true;
    }
    if (width == 0 || height == 0 ||
        ((numChannels != 3) && (numChannels != 4))) {
        // Invalid input
        return false;
    }

    const ImageFormat format =
            (numChannels == 3) ? ImageFormat::RGB888 : ImageFormat::RGBA8888;
    std::vector<uint8_t> pixels(width * height * numChannels);
    memcpy(pixels.data(), pixelData, pixels.size());
    sBackgroundImage =
            Image(width, height, numChannels, format, std::move(pixels));

    return true;
}

Image takeScreenshot(
        ImageFormat desiredFormat,
        SkinRotation rotation,
        gfxstream::Renderer* renderer,
        std::function<void(int* w,
                           int* h,
                           int* lineSize,
                           int* bytesPerPixel,
                           uint8_t** frameBufferData)> getFrameBuffer,
        int displayId,
        int desiredWidth,
        int desiredHeight,
        SkinRect rect) {
    unsigned int nChannels = 4;
    unsigned int width;
    unsigned int height;
    ImageFormat outputFormat = ImageFormat::RGBA8888;
    if (!renderer) {
        derror("Could not take screenshot: no renderer");
        return Image(0, 0, 0, ImageFormat::RGB888, {});
    }

    if (desiredFormat == ImageFormat::RGB888) {
        nChannels = 3;
        outputFormat = ImageFormat::RGB888;
    }
    size_t cPixels = 0;
    int screenshotRes = renderer->getScreenshot(
            nChannels, &width, &height, nullptr, &cPixels,
            displayId, desiredWidth, desiredHeight, rotation,
            {{rect.pos.x, rect.pos.y}, {rect.size.w, rect.size.h}});
    std::vector<uint8_t> pixelBuffer(0);
    if (screenshotRes == gfxstream::Renderer::GET_SCREENSHOT_RESULT_PIXELS_SIZE) {
        pixelBuffer.resize(cPixels);
        screenshotRes = renderer->getScreenshot(
                nChannels, &width, &height, pixelBuffer.data(), &cPixels,
                displayId, desiredWidth, desiredHeight, rotation,
                {{rect.pos.x, rect.pos.y}, {rect.size.w, rect.size.h}});
    }
    if (screenshotRes != 0) {
        derror("Could not take screenshot, error: %d", screenshotRes);
        return Image(0, 0, 0, ImageFormat::RGB888, {});
    }

    // We only convert png/ RGBA8888 -> RBG888 at this time..
    switch (desiredFormat) {
        case ImageFormat::PNG: {
            std::vector<uint8_t> pngData;
            png_structp p = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL,
                                                    NULL, NULL);
            png_infop pi = png_create_info_struct(p);
            png_set_write_fn(
                    p, &pngData,
                    [](png_structp png_ptr, png_bytep data, png_size_t length) {
                        std::vector<uint8_t>* vec =
                                reinterpret_cast<std::vector<uint8_t>*>(
                                        png_get_io_ptr(png_ptr));
                        vec->insert(vec->end(), &data[0], &data[length]);
                    },
                    [](png_structp png_ptr) {});
#if defined(__linux__) && defined(__aarch64__)
            // TODO(b/468252386): fix linux-arm linker settings for png
            // functions
            derror("Cannot use write_png_user_function on linux_aarch64");
            return Image(0, 0, 0, ImageFormat::RGB888, {});
#else
            // already rotated through rendering
            write_png_user_function(p, pi, nChannels, width, height,
                                    SKIN_ROTATION_0, pixelBuffer.data());
            png_destroy_write_struct(&p, &pi);
            return Image((uint16_t)width, (uint16_t)height, nChannels,
                         ImageFormat::PNG, std::move(pngData));
#endif
        }
        case ImageFormat::RGB888: {
            auto img = Image((uint16_t)width, (uint16_t)height, nChannels,
                             outputFormat, std::move(pixelBuffer));
            return img.asRGB888();
        }
        default:
            return Image((uint16_t)width, (uint16_t)height, nChannels,
                         outputFormat, std::move(pixelBuffer));
    }
}

bool captureScreenshot(
        gfxstream::Renderer* renderer,
        std::function<void(int* w,
                           int* h,
                           int* lineSize,
                           int* bytesPerPixel,
                           uint8_t** frameBufferData)> getFrameBuffer,
        SkinRotation rotation,
        const char* outputDirectoryPath,
        std::string* pOutputFilepath,
        int displayId) {
    if (!renderer && !getFrameBuffer) {
        dwarning(
                "Unable to take a screenshot of display: %d. No framebuffer, "
                "or renderer available",
                displayId);
        return false;
    }

    Image img = takeScreenshot(ImageFormat::RAW, rotation, renderer,
                               getFrameBuffer, displayId);

    if (img.getWidth() == 0 || img.getHeight() == 0) {
        derror("Failed to take a screenshot of display: %d, received: (%dx%d)",
               displayId, img.getWidth(), img.getHeight());
        return false;
    }

    // If the given directory path is actually a filename with a protobuf
    // extension, write a serialized Observation instead.
    std::string outputFilePath;
    if (outputDirectoryPath && str_ends_with(outputDirectoryPath, ".pb")) {
        std::ofstream file(PathUtils::asUnicodePath(outputDirectoryPath).c_str(), std::ios_base::binary);
        if (!file) {
            derror("Failed to write a screenshot of display: %d to %s",
                   displayId, outputDirectoryPath);
            return false;
        }

        Observation observation;
        observation.set_timestamp_us(
                android::base::System::get()->getUnixTimeUs());
        Observation::Image* screen = observation.mutable_screen();
        screen->set_width(img.getWidth());
        screen->set_height(img.getHeight());
        screen->set_num_channels(img.getChannels());
        screen->set_data(img.getPixelBuf(), img.getPixelCount());
        observation.SerializeToOstream(&file);
        return true;
    }

    // Custom filename
    if (outputDirectoryPath && str_ends_with(outputDirectoryPath, ".png")) {
        outputFilePath = outputDirectoryPath;
    } else {
        char fileName[100];
        // the file name is ~25 characters
        int fileNameSize = snprintf(
                fileName, sizeof(fileName), "Screenshot_%lld.png",
                (long long int)android::base::System::get()->getUnixTime());
        assert(fileNameSize < sizeof(fileName));
        (void)fileNameSize;

        outputFilePath = outputDirectoryPath ?
                android::base::PathUtils::join(outputDirectoryPath, fileName) : fileName;
    }
    if (pOutputFilepath) {
        *pOutputFilepath = outputFilePath;
    }

#if defined(__linux__) && defined(__aarch64__)
    // TODO(b/468252386): fix linux-arm linker settings for png functions
    derror("Cannot use savepng on linux_aarch64");
    return false;
#else
    dprint("Saving screenshot to file: %s", outputFilePath.c_str());
    // already rotated through rendering
    rotation = renderer ? SKIN_ROTATION_0 : rotation;
    savepng(outputFilePath.c_str(), img.getChannels(), img.getWidth(),
            img.getHeight(), rotation, img.getPixelBuf());
    return true;
#endif
}

// True if we are on a big endian system
static int is_big_endian(void) {
    static const union {
        uint16_t w;
        uint8_t b[2];
    } tmp = {1};
    return (tmp.b[0] != 1);
}

static std::vector<uint8_t>& convert_dma_byte(std::vector<uint8_t>& source,
                                              std::vector<uint8_t>& dest) {
    auto len = source.size() / 4 * 3;
    uint8_t* dst = dest.data();
    const uint8_t* src = source.data();
    const uint8_t* src_end = source.data() + source.size();
    int j = 0;
    while (src < src_end) {
        // Loop invariant when in place assert(dst <= src);
        j++;
        if (j % 4 != 0) {
            *dst = *src;
            dst++;
        }
        src++;
    }
    dest.resize(len);
    return source;
}

static std::vector<uint8_t>& convert_dma_hexlet(std::vector<uint8_t>& source,
                                                std::vector<uint8_t>& dest) {
    // This only works if we have "native" support for uint128_t (which clang
    // has)
    static_assert(sizeof(__uint128_t) == 16);

    // Dest should be large enough to hold what we need.
    assert(dest.size() == source.size() ||
           dest.size() >= source.size() / 4 * 3 + 16);

    // an (uint32_t) ABGR value ends up like this in memory:
    //               |||\- [0] R
    //               ||\-- [1] G
    //               |\--- [2] B
    //               \-----[3] A

    // in a uint64_t  ABGR2 ABRG1
    // in a __uint128 ABGR4 ABGR3 ABGR2 ABRG1

    // The idea is that we are going to mask the Alpha byte
    // and move the bytes over so we turn

    // in a __uint128 ABGR4 ABGR3 ABGR2 ABRG1 --> 0x00000000 BGR4 BGR3 BGR2 BGR1
    // which end up in memory like:  R1,G1,B1,R2,G2,B2,R3,G3,B3,R4,G4,B4,0,0,0,0
    auto final_size = source.size() / 4 * 3;

    // Start & ending pointers.
    const uint8_t* src = source.data();
    const uint8_t* src_end = source.data() + source.size();
    uint8_t* dst = dest.data();

    // Various masks to mask out the alpha channel.
    constexpr __uint128_t RGB1 = 0xFFFFFF, RGB2 = RGB1 << 32, RGB3 = RGB2 << 32,
                          RGB4 = RGB3 << 32;

    // Make sure we can read at least 16 bytes.. (128 bits)
    // This guarantees that we do not access any memory we do not own.
    while (src + 16 < src_end) {
        // Only valid when doing in place: assert(dst <= src);
        const __uint128_t pixel = *((__uint128_t*)src);
        __uint128_t* to_write = (__uint128_t*)dst;
        __uint128_t newValue = ((pixel & RGB4) >> 24) | ((pixel & RGB3) >> 16) |
                               ((pixel & RGB2) >> 8) | (pixel & RGB1);

        // Note that endianness is very important! the most significant bits end
        // up in the address furthest away resulting in 4 zero bytes! which will
        // be filled up in the next round (or will get chopped up in the end).
        *to_write = newValue;

        dst += 12;  // We wrote 12 bytes. (well 16 but the last 4 bytes we don't
                    // care for)
        src += 16;  // We read 16 bytes.
    }

    // Move the last 16 bytes if needed, this happens if we are not aligned to a
    // 128 bit boundary.
    int j = 0;
    while (src < src_end) {
        // assert(dst <= src); only true when doing in place.
        j++;
        if (j % 4 != 0) {
            *dst = *src;
            dst++;
        }
        src++;
    }

    // Shrink our vector
    dest.resize(final_size);
    return dest;
}

void Image::convertPerByte() {
    convert_dma_byte(m_Pixels, m_Pixels);
}

void Image::convertPerHexlet() {
    convert_dma_hexlet(m_Pixels, m_Pixels);
}

Image& Image::asRGB888() {
    // No need to convert an already converted image.
    if (m_Format == ImageFormat::RGB888) {
        return *this;
    }

    m_Format = ImageFormat::RGB888;
    // Let's just use the slow, default approach when
    // When we are not little endian.
    if (is_big_endian() || sizeof(__uint128_t) != 16) {
        convertPerByte();
    } else {
        convertPerHexlet();
    }

    return *this;
}

}  // namespace emulation
}  // namespace android

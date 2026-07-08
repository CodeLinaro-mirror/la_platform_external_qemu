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
#include <cstring>
#include <limits>
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

namespace android {
namespace ver {

static std::optional<size_t> safeMultiply(size_t a, size_t b) {
    if (a > 0 && b > std::numeric_limits<size_t>::max() / a) {
        return std::nullopt;
    }
    return a * b;
}

static void pngWarningCallback(png_structp readPtr,
                               png_const_charp warningMessage) {
    dprint("%s: %s\n", __FUNCTION__, warningMessage);
}

template <typename T>
static inline T alignRowBytes(T value) {
    return (value + 3) / 4 * 4;
}

// Comments are formatted as
// (0th Row-0th Column)
enum class ExifOrientation {
    Normal = 1,          // (Top-Left)
    FlipHorizontal = 2,  // (Top-Right)
    Rotate180 = 3,       // (Bottom-Right)
    FlipVertical = 4,    // (Bottom-Left)
    Transpose = 5,       // (Left-Top)
    Rotate90CW = 6,      // (Right-Top)
    Transverse = 7,      // (Right-Bottom)
    Rotate270CW = 8,     // (Left-Bottom)
};

static std::optional<ExifOrientation> toExifOrientation(int value) {
    if (value >= static_cast<int>(ExifOrientation::Normal) &&
        value <= static_cast<int>(ExifOrientation::Rotate270CW)) {
        return static_cast<ExifOrientation>(value);
    }
    return std::nullopt;
}

static bool exifOrientationSwapsDims(ExifOrientation orientation) {
    return orientation >= ExifOrientation::Transpose &&
           orientation <= ExifOrientation::Rotate270CW;
}

// TIFF Header Constants
constexpr size_t kTiffHeaderSize = 8;
constexpr uint8_t kTiffLittleEndianMarker = 0x49;  // 'I'
constexpr uint8_t kTiffBigEndianMarker = 0x4D;     // 'M'
constexpr uint16_t kTiffMagicNumber = 42;

// TIFF Directory Constants
constexpr size_t kTiffDirNumEntriesSize = 2;
constexpr size_t kTiffDirEntrySize = 12;
constexpr uint16_t kExifOrientationTag = 0x0112;
constexpr uint16_t kTiffTypeShort = 3;

// TIFF Directory Entry Offsets
constexpr size_t kTiffDirEntryTypeOffset = 2;
constexpr size_t kTiffDirEntryCountOffset = 4;
constexpr size_t kTiffDirEntryValueOffset = 8;

static uint16_t read16(const uint8_t* ptr, bool little_endian) {
    if (little_endian) {
        return static_cast<uint32_t>(ptr[0]) |
               (static_cast<uint32_t>(ptr[1]) << 8);
    } else {
        return (static_cast<uint32_t>(ptr[0]) << 8) |
               static_cast<uint32_t>(ptr[1]);
    }
}

static uint32_t read32(const uint8_t* ptr, bool little_endian) {
    if (little_endian) {
        return static_cast<uint32_t>(ptr[0]) |
               (static_cast<uint32_t>(ptr[1]) << 8) |
               (static_cast<uint32_t>(ptr[2]) << 16) |
               (static_cast<uint32_t>(ptr[3]) << 24);
    } else {
        return (static_cast<uint32_t>(ptr[0]) << 24) |
               (static_cast<uint32_t>(ptr[1]) << 16) |
               (static_cast<uint32_t>(ptr[2]) << 8) |
               static_cast<uint32_t>(ptr[3]);
    }
}

static std::optional<ExifOrientation> getOrientationFromTiff(
        const uint8_t* tiff,
        size_t tiff_len) {
    if (tiff_len < kTiffHeaderSize) {
        return std::nullopt;
    }

    bool little_endian = false;
    if (tiff[0] == kTiffLittleEndianMarker &&
        tiff[1] == kTiffLittleEndianMarker) {
        little_endian = true;
    } else if (tiff[0] == kTiffBigEndianMarker &&
               tiff[1] == kTiffBigEndianMarker) {
        little_endian = false;
    } else {
        return std::nullopt;
    }

    uint16_t magic = read16(tiff + 2, little_endian);
    if (magic != kTiffMagicNumber) {
        return std::nullopt;
    }

    uint32_t ifd0_offset = read32(tiff + 4, little_endian);
    if (ifd0_offset > tiff_len ||
        tiff_len - ifd0_offset < kTiffDirNumEntriesSize) {
        return std::nullopt;
    }

    uint16_t num_entries = read16(tiff + ifd0_offset, little_endian);
    size_t entry_offset =
            static_cast<size_t>(ifd0_offset) + kTiffDirNumEntriesSize;

    for (int i = 0; i < num_entries; ++i) {
        if (entry_offset > tiff_len ||
            tiff_len - entry_offset < kTiffDirEntrySize) {
            return std::nullopt;
        }

        uint16_t tag = read16(tiff + entry_offset, little_endian);
        if (tag == kExifOrientationTag) {
            uint16_t type =
                    read16(tiff + entry_offset + kTiffDirEntryTypeOffset,
                           little_endian);
            uint32_t count =
                    read32(tiff + entry_offset + kTiffDirEntryCountOffset,
                           little_endian);
            if (type == kTiffTypeShort && count == 1) {
                uint16_t val =
                        read16(tiff + entry_offset + kTiffDirEntryValueOffset,
                               little_endian);
                return toExifOrientation(val);
            }
        }
        entry_offset += kTiffDirEntrySize;
    }

    return std::nullopt;
}

static std::optional<ExifOrientation> getOrientation(const uint8_t* app1_data,
                                                     size_t length) {
    const uint8_t kExifSignature[] = {'E', 'x', 'i', 'f', 0, 0};
    const size_t kExifSignatureSize = sizeof(kExifSignature);

    if (length < kExifSignatureSize) {
        return std::nullopt;
    }
    if (memcmp(app1_data, kExifSignature, kExifSignatureSize) != 0) {
        return std::nullopt;
    }
    return getOrientationFromTiff(app1_data + kExifSignatureSize,
                                  length - kExifSignatureSize);
}

static void transformImage(ImageData& img, ExifOrientation orientation) {
    if (orientation == ExifOrientation::Normal) {
        return;
    }

    uint32_t src_w = img.width;
    uint32_t src_h = img.height;
    uint32_t dst_w = src_w;
    uint32_t dst_h = src_h;

    if (exifOrientationSwapsDims(orientation)) {
        dst_w = src_h;
        dst_h = src_w;
    }

    auto dst_line_size_opt = safeMultiply(dst_w, img.num_components);
    if (!dst_line_size_opt) {
        derror("Overflow in dst_line_size");
        return;
    }
    if (*dst_line_size_opt > std::numeric_limits<int>::max() - 3) {
        derror("dst_line_size overflows int");
        return;
    }
    size_t dst_line_size = alignRowBytes(*dst_line_size_opt);

    auto dst_size = safeMultiply(dst_line_size, dst_h);
    if (!dst_size) {
        derror("Overflow in dst_data allocation size (stride * height)");
        return;
    }

    std::vector<unsigned char> dst_data(*dst_size);

    auto copy_pixel = [&](uint32_t x, uint32_t y, uint32_t dx, uint32_t dy) {
        const unsigned char* src_pixel =
                &img.data[y * img.line_size + x * img.num_components];
        unsigned char* dst_pixel =
                &dst_data[dy * dst_line_size + dx * img.num_components];
        memcpy(dst_pixel, src_pixel, img.num_components);
    };

    switch (orientation) {
        case ExifOrientation::FlipHorizontal:
            for (uint32_t y = 0; y < src_h; ++y) {
                for (uint32_t x = 0; x < src_w; ++x) {
                    copy_pixel(x, y, src_w - 1 - x, y);
                }
            }
            break;
        case ExifOrientation::Rotate180:
            for (uint32_t y = 0; y < src_h; ++y) {
                for (uint32_t x = 0; x < src_w; ++x) {
                    copy_pixel(x, y, src_w - 1 - x, src_h - 1 - y);
                }
            }
            break;
        case ExifOrientation::FlipVertical:
            for (uint32_t y = 0; y < src_h; ++y) {
                for (uint32_t x = 0; x < src_w; ++x) {
                    copy_pixel(x, y, x, src_h - 1 - y);
                }
            }
            break;
        case ExifOrientation::Transpose:
            for (uint32_t y = 0; y < src_h; ++y) {
                for (uint32_t x = 0; x < src_w; ++x) {
                    copy_pixel(x, y, y, x);
                }
            }
            break;
        case ExifOrientation::Rotate90CW:
            for (uint32_t y = 0; y < src_h; ++y) {
                for (uint32_t x = 0; x < src_w; ++x) {
                    copy_pixel(x, y, src_h - 1 - y, x);
                }
            }
            break;
        case ExifOrientation::Transverse:
            for (uint32_t y = 0; y < src_h; ++y) {
                for (uint32_t x = 0; x < src_w; ++x) {
                    copy_pixel(x, y, src_h - 1 - y, src_w - 1 - x);
                }
            }
            break;
        case ExifOrientation::Rotate270CW:
            for (uint32_t y = 0; y < src_h; ++y) {
                for (uint32_t x = 0; x < src_w; ++x) {
                    copy_pixel(x, y, y, src_w - 1 - x);
                }
            }
            break;
        default:
            // We've already handled ExifOrientation::Normal
            break;
    }

    img.data = std::move(dst_data);
    img.data_ptr = img.data.data();
    img.width = dst_w;
    img.height = dst_h;
    img.line_size = dst_line_size;
}

std::optional<ImageData> loadPNGImage(const std::string& filename) {
    ScopedStdioFile fp(android_fopen(filename.c_str(), "rb"));
    if (!fp) {
        derror("Failed to open file %s", filename.c_str());
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

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr,
                                             nullptr, nullptr);
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
        png_destroy_read_struct(&png, &pngInfo, nullptr);
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
        png_destroy_read_struct(&png, &pngInfo, nullptr);
        return {};
    }

    const size_t rowBytes = png_get_rowbytes(png, pngInfo);
    if (rowBytes > static_cast<size_t>(std::numeric_limits<int>::max()) - 3) {
        derror("PNG rowBytes overflows int");
        png_destroy_read_struct(&png, &pngInfo, nullptr);
        return {};
    }
    const size_t stride = alignRowBytes(rowBytes);
    auto alloc_size = safeMultiply(stride, height);
    if (!alloc_size) {
        derror("Overflow in PNG data allocation size");
        png_destroy_read_struct(&png, &pngInfo, nullptr);
        return {};
    }
    data.resize(*alloc_size);
    rowPtrs.resize(height);

    for (size_t i = 0; i < height; i++) {
        rowPtrs[i] = data.data() + stride * i;
    }

    png_read_image(png, rowPtrs.data());

    png_uint_32 num_exif = 0;
    png_bytep exif_data = nullptr;
    ExifOrientation orientation = ExifOrientation::Normal;
    if (png_get_eXIf_1(png, pngInfo, &num_exif, &exif_data) & PNG_INFO_eXIf) {
        orientation = getOrientationFromTiff(exif_data, num_exif)
                              .value_or(ExifOrientation::Normal);
    }

    png_destroy_read_struct(&png, &pngInfo, nullptr);

    ImageData img;
    img.data = std::move(data);
    img.data_ptr = img.data.data();
    img.width = width;
    img.height = height;
    img.num_components = 4;
    img.line_size = static_cast<int>(stride);

    transformImage(img, orientation);

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

std::optional<ImageData> loadJPEGImage(const std::string& filename) {
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
    // Request to save APP1 marker (contains EXIF)
    jpeg_save_markers(&cinfo, JPEG_APP0 + 1, 0xFFFF);
    (void)jpeg_read_header(&cinfo, TRUE);

    // Parse EXIF orientation early
    ExifOrientation orientation = ExifOrientation::Normal;
    jpeg_saved_marker_ptr marker = cinfo.marker_list;
    while (marker != nullptr) {
        if (marker->marker == JPEG_APP0 + 1) {
            auto res = getOrientation(marker->data, marker->data_length);
            if (res.has_value()) {
                orientation = res.value();
                break;
            }
        }
        marker = marker->next;
    }

    cinfo.out_color_space = JCS_RGBA_8888;  // Force RGBA format.
    (void)jpeg_start_decompress(&cinfo);

    img.width = cinfo.output_width;
    img.height = cinfo.output_height;
    img.num_components = cinfo.output_components;

    if (img.num_components <= 0) {
        derror("Invalid JPEG num_components: %d", img.num_components);
        return std::nullopt;
    }

    auto packed_line_size = safeMultiply(img.width, img.num_components);
    if (!packed_line_size ||
        *packed_line_size >
                static_cast<size_t>(std::numeric_limits<int>::max()) - 3) {
        derror("JPEG line_size overflows int");
        return std::nullopt;
    }
    size_t stride = alignRowBytes(*packed_line_size);
    img.line_size = static_cast<int>(stride);

    auto size = safeMultiply(stride, img.height);
    if (!size) {
        derror("Overflow in JPEG data allocation size (stride * height)");
        return std::nullopt;
    }

    img.data.resize(*size);

    while (cinfo.output_scanline < cinfo.output_height) {
        // buffer is a pointer to the start of the current row in our vector
        unsigned char* buffer =
                &img.data[cinfo.output_scanline * img.line_size];
        (void)jpeg_read_scanlines(&cinfo, &buffer, 1);
    }

    (void)jpeg_finish_decompress(&cinfo);
    img.data_ptr = img.data.data();

    transformImage(img, orientation);

    return img;
}

std::optional<ImageData> loadImageFromFile(const std::string& filename) {
    const std::string_view extension{PathUtils::extension(filename)};

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
        const std::string& filename) {
    std::optional<ImageData> maybeImage = loadImageFromFile(filename);
    if (maybeImage) {
        return std::unique_ptr<RawImageFileSource>(
                new RawImageFileSource(filename, std::move(maybeImage.value())));
    }
    return nullptr;
}

RawImageFileSource::RawImageFileSource(const std::string& filename, ImageData&& image)
    : file_(filename), image_(std::move(image)) {}

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

}  // namespace ver
}  // namespace android
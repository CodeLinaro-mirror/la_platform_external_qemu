// Copyright (C) 2026 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

#include "JpegUtils.h"

#include <gtest/gtest.h>

#include "aemu/base/files/PathUtils.h"
#include "aemu/base/files/ScopedStdioFile.h"
#include "android/base/system/System.h"
#include "android/utils/file_io.h"

namespace android {
namespace ver {

static std::string testdataPathToAbsolute(const char* filename) {
    return base::PathUtils::join(base::System::get()->getProgramDirectory(),
                                 "testdata", filename);
}

TEST(JpegUtilsTest, DecompressFromFile_RGBA) {
    const std::string path = testdataPathToAbsolute("jpeg_rgb24.jpg");
    std::vector<uint8_t> buffer;
    JpegImageInfo info;
    JpegDecompressOptions options;
    options.color_space = JCS_RGBA_8888;
    options.align_row_stride = false;

    ASSERT_TRUE(JpegUtils::DecompressFromFile(path, buffer, &info, options));
    EXPECT_EQ(info.width, 31);
    EXPECT_EQ(info.height, 31);
    EXPECT_EQ(info.num_components, 4);
    EXPECT_EQ(info.line_size, 31 * 4);
    EXPECT_EQ(info.total_size, 31 * 31 * 4);
    EXPECT_EQ(buffer.size(), 31 * 31 * 4);

    // Alpha channel must be 255 for all pixels
    for (int i = 0; i < 31 * 31; ++i) {
        EXPECT_EQ(buffer[i * 4 + 3], 255);
    }
}

TEST(JpegUtilsTest, DecompressFromFile_RGB) {
    const std::string path = testdataPathToAbsolute("jpeg_rgb24.jpg");
    std::vector<uint8_t> buffer;
    JpegImageInfo info;
    JpegDecompressOptions options;
    options.color_space = JCS_RGB;
    options.align_row_stride = true;

    ASSERT_TRUE(JpegUtils::DecompressFromFile(path, buffer, &info, options));
    EXPECT_EQ(info.width, 31);
    EXPECT_EQ(info.height, 31);
    EXPECT_EQ(info.num_components, 3);
    // 31 * 3 = 93 bytes, aligned to 4 bytes is 96 bytes
    EXPECT_EQ(info.line_size, 96);
    EXPECT_EQ(info.total_size, 96 * 31);
    EXPECT_EQ(buffer.size(), 96 * 31);
}

TEST(JpegUtilsTest, DecompressFromMemory_RGBA) {
    const std::string path = testdataPathToAbsolute("jpeg_rgb24.jpg");
    base::ScopedStdioFile fp(android_fopen(path.c_str(), "rb"));
    ASSERT_TRUE(fp);

    fseek(fp.get(), 0, SEEK_END);
    long size = ftell(fp.get());
    fseek(fp.get(), 0, SEEK_SET);
    std::vector<uint8_t> file_bytes(size);
    ASSERT_EQ(size, fread(file_bytes.data(), 1, size, fp.get()));

    std::vector<uint8_t> out_buffer;
    JpegImageInfo info;
    JpegDecompressOptions options;
    options.color_space = JCS_RGBA_8888;
    options.align_row_stride = false;

    ASSERT_TRUE(JpegUtils::DecompressFromMemory(
            file_bytes.data(), file_bytes.size(), out_buffer, &info, options));
    EXPECT_EQ(info.width, 31);
    EXPECT_EQ(info.height, 31);
    EXPECT_EQ(out_buffer.size(), 31 * 31 * 4);
}

TEST(JpegUtilsTest, DecompressFromMemory_BottomUp) {
    const std::string path = testdataPathToAbsolute("jpeg_rgb24.jpg");
    std::vector<uint8_t> top_down_buf;
    std::vector<uint8_t> bottom_up_buf;
    JpegImageInfo info_td;
    JpegImageInfo info_bu;

    JpegDecompressOptions opt_td;
    opt_td.color_space = JCS_RGBA_8888;
    opt_td.orientation = JpegOrientation::TopDown;

    JpegDecompressOptions opt_bu;
    opt_bu.color_space = JCS_RGBA_8888;
    opt_bu.orientation = JpegOrientation::BottomUp;

    ASSERT_TRUE(JpegUtils::DecompressFromFile(path, top_down_buf, &info_td,
                                              opt_td));
    ASSERT_TRUE(JpegUtils::DecompressFromFile(path, bottom_up_buf, &info_bu,
                                              opt_bu));

    EXPECT_EQ(info_td.width, info_bu.width);
    EXPECT_EQ(info_td.height, info_bu.height);

    int height = info_td.height;
    size_t stride = info_td.line_size;

    // The first row of bottom_up should match the last row of top_down
    EXPECT_EQ(0, memcmp(bottom_up_buf.data(),
                        top_down_buf.data() + (height - 1) * stride, stride));
}

TEST(JpegUtilsTest, DecompressFromMemory_BufferReuse) {
    const std::string path = testdataPathToAbsolute("jpeg_rgb24.jpg");
    std::vector<uint8_t> file_bytes;
    base::ScopedStdioFile fp(android_fopen(path.c_str(), "rb"));
    ASSERT_TRUE(fp);

    fseek(fp.get(), 0, SEEK_END);
    long size = ftell(fp.get());
    fseek(fp.get(), 0, SEEK_SET);
    file_bytes.resize(size);
    ASSERT_EQ(size, fread(file_bytes.data(), 1, size, fp.get()));

    // Pre-allocate buffer to simulate streaming buffer reuse
    std::vector<uint8_t> reusable_buffer(64 * 1024);
    const uint8_t* initial_ptr = reusable_buffer.data();

    JpegImageInfo info;
    ASSERT_TRUE(JpegUtils::DecompressFromMemory(
            file_bytes.data(), file_bytes.size(), reusable_buffer, &info));

    // Pointer should remain unchanged since initial capacity was sufficient
    EXPECT_EQ(reusable_buffer.data(), initial_ptr);
}

TEST(JpegUtilsTest, DecompressFromMemory_InvalidData) {
    const std::vector<uint8_t> garbage = {0x12, 0x34, 0x56, 0x78};
    std::vector<uint8_t> out;
    JpegImageInfo info;

    EXPECT_FALSE(JpegUtils::DecompressFromMemory(garbage.data(), garbage.size(),
                                                 out, &info));
}

TEST(JpegUtilsTest, DecompressFromMemory_NullOrEmpty) {
    std::vector<uint8_t> out;
    uint8_t dummy[4] = {0};

    EXPECT_FALSE(JpegUtils::DecompressFromMemory(nullptr, 10, out));
    EXPECT_FALSE(JpegUtils::DecompressFromMemory(dummy, 0, out));
}

TEST(JpegUtilsTest, DecompressFromFile_NonExistent) {
    std::vector<uint8_t> out;
    EXPECT_FALSE(JpegUtils::DecompressFromFile("/non/existent/file.jpg", out));
}

TEST(JpegUtilsTest, DecompressFromFile_SaveApp1Marker) {
    const std::string path =
            testdataPathToAbsolute("ver/exif_rotation/corner_1.jpg");
    std::vector<uint8_t> out;
    JpegImageInfo info;
    JpegDecompressOptions options;
    options.save_app1_marker = true;

    ASSERT_TRUE(JpegUtils::DecompressFromFile(path, out, &info, options));
    EXPECT_FALSE(info.app1_markers.empty());
    // First 4 bytes of EXIF marker are 'E', 'x', 'i', 'f'
    ASSERT_GE(info.app1_markers[0].size(), 6u);
    EXPECT_EQ('E', info.app1_markers[0][0]);
    EXPECT_EQ('x', info.app1_markers[0][1]);
    EXPECT_EQ('i', info.app1_markers[0][2]);
    EXPECT_EQ('f', info.app1_markers[0][3]);
}

}  // namespace ver
}  // namespace android

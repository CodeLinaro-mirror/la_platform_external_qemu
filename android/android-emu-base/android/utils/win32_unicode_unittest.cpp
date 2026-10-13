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

#include "android/utils/win32_unicode.h"

#include <gtest/gtest.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <wchar.h>

#include "aemu/base/files/PathUtils.h"
#include "android/base/testing/TestTempDir.h"
#include "android/utils/file_io.h"
#include "android/utils/path.h"

extern "C" int win32_stat(const char* filepath, struct _stati64* st);

namespace {

struct TestStringPair {
    const char* utf8;
    const wchar_t* utf16;
};

constexpr TestStringPair kTestStrings[] = {
        {"", L""},
        {"Hello World!", L"Hello World!"},
        {"T\xC3\xA9l\xC3\xA9vision", L"T\xE9l\xE9vision"},
        {"foo\xE1\x80\x80 bar", L"foo\x1000 bar"},
        {"fileShareTest\xE4\xB8\xAD\xE6\x96\x87", L"fileShareTest\x4E2D\x6587"},
};

}  // namespace

TEST(Win32Unicode, Utf16ToUtf8Str) {
    EXPECT_EQ(nullptr, win32_utf16_to_utf8_str(nullptr));
    EXPECT_EQ(nullptr, win32_utf16_to_utf8_str(L""));

    for (const auto& item : kTestStrings) {
        if (item.utf16[0] == L'\0') {
            continue;
        }
        char* utf8 = win32_utf16_to_utf8_str(item.utf16);
        ASSERT_NE(nullptr, utf8);
        EXPECT_STREQ(item.utf8, utf8);
        free(utf8);
    }
}

TEST(Win32Unicode, Utf16ToUtf8Buf) {
    for (const auto& item : kTestStrings) {
        char buf[256] = {};
        const int expectedLen = static_cast<int>(strlen(item.utf8)) + 1;
        EXPECT_EQ(expectedLen,
                  win32_utf16_to_utf8_buf(item.utf16, buf, sizeof(buf)));
        EXPECT_STREQ(item.utf8, buf);

        EXPECT_EQ(-1, win32_utf16_to_utf8_buf(item.utf16, buf, expectedLen));
        EXPECT_EQ(expectedLen,
                  win32_utf16_to_utf8_buf(item.utf16, buf, expectedLen + 1));
    }

    char buf[16] = {};
    EXPECT_EQ(-1, win32_utf16_to_utf8_buf(nullptr, buf, sizeof(buf)));
    EXPECT_EQ(-1, win32_utf16_to_utf8_buf(L"test", nullptr, sizeof(buf)));
    EXPECT_EQ(-1, win32_utf16_to_utf8_buf(L"test", buf, 0));
    EXPECT_EQ(-1, win32_utf16_to_utf8_buf(L"test", buf, -1));
}

TEST(Win32Unicode, Utf8ToUtf16Str) {
    EXPECT_EQ(nullptr, win32_utf8_to_utf16_str(nullptr));

    for (const auto& item : kTestStrings) {
        wchar_t* utf16 = win32_utf8_to_utf16_str(item.utf8);
        ASSERT_NE(nullptr, utf16);
        EXPECT_STREQ(item.utf16, utf16);
        free(utf16);
    }
}

TEST(Win32Unicode, Win32Stat) {
    android::base::TestTempDir tempDir("win32_stat_test_\xE4\xB8\xAD\xE6\x96\x87");
    const std::string subDir =
            tempDir.makeSubPath("dir_\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E");
    ASSERT_EQ(0, path_mkdir_if_needed(subDir.c_str(), 0755));

    const std::string filePath =
            android::base::pj(subDir, "file_\xE4\xB8\xAD\xE6\x96\x87.txt");
    struct _stati64 st = {};
    EXPECT_EQ(-1, win32_stat(filePath.c_str(), &st));
    EXPECT_EQ(-1, win32_stat(nullptr, &st));

    const char kPayload[] = "win32_stat test payload";
    FILE* fp = android_fopen(filePath.c_str(), "wb");
    ASSERT_NE(nullptr, fp);
    ASSERT_EQ(sizeof(kPayload), fwrite(kPayload, 1, sizeof(kPayload), fp));
    fclose(fp);

    EXPECT_EQ(0, win32_stat(filePath.c_str(), &st));
    EXPECT_EQ(static_cast<int64_t>(sizeof(kPayload)), st.st_size);
}

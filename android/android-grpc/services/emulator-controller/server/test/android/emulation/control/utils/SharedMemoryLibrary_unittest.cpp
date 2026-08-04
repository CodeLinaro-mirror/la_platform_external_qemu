// Copyright (C) 2026 The Android Open Source Project
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

#include "android/emulation/control/utils/SharedMemoryLibrary.h"

#include <gtest/gtest.h>

namespace android {
namespace emulation {
namespace control {

TEST(SharedMemoryLibraryTest, BorrowBasic) {
    SharedMemoryLibrary lib;
    auto entry = lib.borrow("test_shm_region", 1024);
    ASSERT_NE(entry.get(), nullptr);
    EXPECT_TRUE(entry->isOpen());
    EXPECT_TRUE(entry->isMapped());
    EXPECT_GE(entry->size(), 1024u);
}

TEST(SharedMemoryLibraryTest, BorrowSizeZeroThenGrow) {
    SharedMemoryLibrary lib;
    // Initial borrow with size 0 (e.g. unscaled request)
    auto entry0 = lib.borrow("test_shm_grow", 0);
    ASSERT_NE(entry0.get(), nullptr);

    // Subsequent borrow with actual frame size
    auto entry1 = lib.borrow("test_shm_grow", 4096);
    ASSERT_NE(entry1.get(), nullptr);
    EXPECT_TRUE(entry1->isOpen());
    EXPECT_TRUE(entry1->isMapped());
    EXPECT_GE(entry1->size(), 4096u);
}

TEST(SharedMemoryLibraryTest, BorrowMultipleRefCounts) {
    SharedMemoryLibrary lib;
    auto entry1 = lib.borrow("test_shm_ref", 2048);
    ASSERT_NE(entry1.get(), nullptr);

    auto entry2 = lib.borrow("test_shm_ref", 2048);
    ASSERT_NE(entry2.get(), nullptr);
    EXPECT_EQ(entry1.get(), entry2.get());

    entry1.reset();
    EXPECT_TRUE(entry2->isOpen());
    EXPECT_TRUE(entry2->isMapped());
}

}  // namespace control
}  // namespace emulation
}  // namespace android

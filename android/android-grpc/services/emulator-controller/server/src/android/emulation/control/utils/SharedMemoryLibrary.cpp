// Copyright (C) 2020 The Android Open Source Project
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

#include <cassert>  // for assert
#include <string_view>
#include <utility>  // for move

#include "aemu/base/Log.h"

#include "aemu/base/memory/ScopedPtr.h"     // for makeCustomScopedPtr
#include "aemu/base/memory/SharedMemory.h"  // for SharedMemory, std::string_view

#define assertm(exp, msg) assert(((void)msg, exp))

namespace android {
namespace emulation {
namespace control {

using android::base::SharedMemory;

SharedMemoryLibrary::LibraryEntry SharedMemoryLibrary::borrow(
        std::string handle,
        size_t size) {
    const std::lock_guard<std::mutex> lock(mAccess);

    bool needsCreate = (mHandlesCnt.count(handle) == 0) ||
                       !mMemMap[handle]->isOpen() ||
                       !mMemMap[handle]->isMapped() ||
                       (size > 0 && mMemMap[handle]->size() < size);

    if (needsCreate) {
        auto shm = std::make_unique<SharedMemory>(handle, size);
        if (shm->open(SharedMemory::AccessMode::READ_WRITE) != 0 || !shm->isOpen() || !shm->isMapped()) {
            shm->create(0600);
        }
        if (mHandlesCnt.count(handle) == 0) {
            mHandlesCnt[handle] = 1;
        }
        mMemMap[handle] = std::move(shm);
    } else {
        // Increase the refcount.
        mHandlesCnt[handle]++;
    }

    SharedMemory* shm = mMemMap[handle].get();

    return android::base::makeCustomScopedPtr(
            shm, [this, handle](SharedMemory* shm) { release(handle); });
}

void SharedMemoryLibrary::release(std::string handle) {
    const std::lock_guard<std::mutex> lock(mAccess);
    if (mHandlesCnt.count(handle) == 0) {
        // What? We are releasing a released handle?
        assert(false);
        return;
    }
    mHandlesCnt[handle]--;
    if (mHandlesCnt[handle] == 0) {
        mHandlesCnt.erase(handle);
        mMemMap.erase(handle);
    }
}

}  // namespace control
}  // namespace emulation
}  // namespace android

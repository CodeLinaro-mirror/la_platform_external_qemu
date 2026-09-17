// Copyright 2015 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

#include "aemu/base/files/MemStream.h"

#include "aemu/base/files/StreamSerializing.h"
#include "aemu/base/logging/Log.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace android {
namespace base {

MemStream::MemStream(size_t reserveSize) {
    mData.reserve(reserveSize);
}

MemStream::MemStream(Buffer&& data) : mData(std::move(data)) {}

ssize_t MemStream::read(void* buffer, size_t size) {
    if (!size) {
        return 0;
    }
    if (!buffer) {
        addError("Memory stream cannot read %zu bytes, no buffer", size);
        return -1;
    }
    const size_t readableSize = readSize();
    if (size > readableSize) {
        addError("Memory stream requested read size %zu is bigger than readable size %zu", size,
                 readableSize);
        if (!readableSize) {
            return 0;
        }
        size = readableSize;
    }
    memcpy(buffer, mData.data() + mReadPos, size);
    mReadPos += size;
    return size;
}

ssize_t MemStream::write(const void* buffer, size_t size) {
    if (!size) {
        return 0;
    }
    if (!buffer) {
        addError("Memory stream cannot write %zu bytes, no buffer", size);
        return -1;
    }
    mData.insert(mData.end(), (const char*)buffer, (const char*)buffer + size);
    return size;
}

size_t MemStream::writtenSize() const {
    return mData.size();
}

size_t MemStream::readPos() const {
    return mReadPos;
}

size_t  MemStream::readSize() const {
    if (mData.size() > mReadPos) {
        return mData.size() - mReadPos;
    }
    return 0;
}

void MemStream::save(Stream* stream) const {
    saveBuffer(stream, mData);
}

void MemStream::load(Stream* stream) {
    loadBuffer(stream, &mData);
    mReadPos = 0;
}

void MemStream::rewind() {
    mReadPos = 0;
}

}  // namespace base
}  // namespace android

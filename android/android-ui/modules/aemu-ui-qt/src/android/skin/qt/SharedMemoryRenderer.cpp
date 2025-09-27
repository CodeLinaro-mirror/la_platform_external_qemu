// Copyright 2025 The Android Open Source Project
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

#include "android/skin/qt/SharedMemoryRenderer.h"

#include <QImage>

#include "aemu/base/logging/CLog.h"

SharedMemoryRenderer::SharedMemoryRenderer(const std::string& name,
                                           int width,
                                           int height,
                                           ImgFormat format,
                                           QObject* parent)
    : QObject(parent),
      mSharedMemory(name, 0),  // Size will be set in initialize
      mWidth(width),
      mHeight(height),
      mFormat(format),
      mBytesPerPixel(0) {
    switch (mFormat) {
        case ImgFormat::RGBA8888:
            mBytesPerPixel = 4;
            break;
        case ImgFormat::RGB888:
            mBytesPerPixel = 3;
            break;
        case ImgFormat::PNG:
            // Size is variable for PNG, so we can't pre-calculate it here.
            // The shared memory region must be created by the writer with
            // a sufficient size.
            mBytesPerPixel = 0;
            break;
        default:
            derror("Unsupported image format: %d", static_cast<int>(mFormat));
            break;
    }
}

SharedMemoryRenderer::~SharedMemoryRenderer() = default;

bool SharedMemoryRenderer::initialize() {
    // For raw formats, we can calculate the expected size.
    // For PNG, the size is determined by the writer. We use a default
    // size here which is likely incorrect but allows the SharedMemory
    // object to be constructed. A better mechanism would be needed for
    // robustly handling variable-sized data.
    size_t regionSize =
            (mBytesPerPixel > 0)
                    ? (mWidth * mHeight * mBytesPerPixel)
                    : (mWidth * mHeight * 4);  // Assume max size for PNG

    // Re-construct the SharedMemory object with the calculated size.
    new (&mSharedMemory) android::base::SharedMemory(
            "file://" + mSharedMemory.name(), regionSize);

    if (auto err = mSharedMemory.create(0600) != 0) {
        derror("Failed to open shared memory region: %s (%d, %d) of size: %d",
               mSharedMemory.name().c_str(), err, errno, regionSize);
        return false;
    }

    if (!mSharedMemory.isMapped()) {
        derror("Failed to map shared memory region: %s",
               mSharedMemory.name().c_str());
        return false;
    }

    return true;
}

void SharedMemoryRenderer::update() {
    if (!mSharedMemory.isMapped()) {
        return;
    }

    const uchar* data = static_cast<const uchar*>(mSharedMemory.get());
    QImage image;

    switch (mFormat) {
        case ImgFormat::RGBA8888:
            // QImage will not take ownership of the data, which is correct
            // as it belongs to the shared memory region.
            image = QImage(data, mWidth, mHeight, QImage::Format_RGBA8888);
            break;
        case ImgFormat::RGB888:
            image = QImage(data, mWidth, mHeight, QImage::Format_RGB888);
            break;
        case ImgFormat::PNG:
            // loadFromData will read the PNG data from the buffer and decode
            // it.
            if (!image.loadFromData(data, mSharedMemory.size())) {
                derror("Failed to load PNG from shared memory.");
                return;
            }
            break;
        default:
            // Should not happen if constructor logic is correct.
            return;
    }

    if (image.isNull()) {
        derror("Failed to create QImage from shared memory data.");
        return;
    }

    emit frameReady(image);
}

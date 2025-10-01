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
#pragma once

#include <QImage>
#include <QObject>
#include <string>

#include "aemu/base/memory/SharedMemory.h"

/**
 * @brief Renders images from a shared memory region.
 *
 * This class is designed to read image data from a shared memory region,
 * convert it into a QImage, and emit a signal when a new frame is ready.
 * It is useful for high-performance, low-latency graphics streaming between
 * processes. The shared memory region is identified by a name, and the class
 * supports several image formats.
 */
class SharedMemoryRenderer : public QObject {
    Q_OBJECT

public:
    /**
     * @brief Defines the image formats supported by the renderer.
     */
    enum class ImgFormat {
        /**
         * @brief Portable Network Graphics format.
         * The image data is expected to be a complete PNG file stream.
         */
        PNG = 0,
        /**
         * @brief 32-bit RGBA format, with 8 bits per channel.
         * The data is raw pixel data.
         */
        RGBA8888 = 1,
        /**
         * @brief 24-bit RGB format, with 8 bits per channel.
         * The data is raw pixel data.
         */
        RGB888 = 2,
    };

    /**
     * @brief Constructs a SharedMemoryRenderer object.
     *
     * @param name The handle for the shared memory region. This can be a named
     *             handle for a shared memory object, or a URI for a
     *             memory-mapped file (e.g., "file:///path/to/file").
     * @param width The width of the image in pixels.
     * @param height The height of the image in pixels.
     * @param format The format of the image data in the shared memory.
     * @param parent The parent QObject, for memory management.
     */
    SharedMemoryRenderer(const std::string& name,
                         int width,
                         int height,
                         ImgFormat format,
                         QObject* parent = nullptr);
    ~SharedMemoryRenderer();

    /**
     * @brief Initializes the shared memory region.
     *
     * This method creates and maps the shared memory region. It must be
     * called successfully before any calls to update().
     *
     * @return true if initialization is successful, false otherwise.
     */
    bool initialize();

    /**
     * @brief Reads a new frame from shared memory and emits the frameReady
     * signal.
     *
     * This method is thread-safe and can be called from any thread to trigger
     * a frame update. It reads the current data from the shared memory,
     * creates a QImage, and emits the frameReady() signal with the new image.
     */
    void update();

signals:
    /**
     * @brief Emitted when a new frame has been rendered from shared memory.
     *
     * @param frame A QImage containing the new frame. QImage is an implicitly
     *              shared class, making it efficient to pass by value across
     *              thread boundaries (if the underlying data is not modified).
     */
    void frameReady(const QImage& frame);

private:
    android::base::SharedMemory mSharedMemory;
    int mWidth;
    int mHeight;
    ImgFormat mFormat;
    int mBytesPerPixel;  // Only valid for raw formats
};

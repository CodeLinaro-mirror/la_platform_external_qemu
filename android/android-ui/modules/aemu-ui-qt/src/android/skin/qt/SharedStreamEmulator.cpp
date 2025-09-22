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
#include "android/skin/qt/SharedStreamEmulator.h"

#include "aemu/base/logging/Log.h"

SharedStreamEmulator::SharedStreamEmulator(
        std::string_view handle,
        FrameCallback callback,
        int width,
        int height,
        std::shared_ptr<android::emulation::control::EmulatorGrpcClient> client)
    : mWidth(width),
      mHeight(height),
      mClient(client),
      mHandle(handle),
      mFrameCallback(std::move(callback)) {
    mStub = mClient->stub<android::emulation::control::EmulatorController>();
}

SharedStreamEmulator::~SharedStreamEmulator() {
    stopStream();
}

void SharedStreamEmulator::startStream() {
    if (mIsStreaming) {
        return;
    }

    if (!mClient || !mStub) {
        LOG(ERROR) << "gRPC client not initialized.";
        return;
    }

    mIsStreaming = true;
    mStreamThread = std::thread(&SharedStreamEmulator::streamLoop, this);
}

void SharedStreamEmulator::stopStream() {
    if (!mIsStreaming) {
        return;
    }

    mIsStreaming = false;
    if (mContext) {
        mContext->TryCancel();
    }

    if (mStreamThread.joinable()) {
        mStreamThread.join();
    }
}

void SharedStreamEmulator::streamLoop() {
    mContext = mClient->newContext();
    android::emulation::control::ImageFormat request;
    request.set_format(android::emulation::control::ImageFormat::RGB888);

    request.set_width(mWidth);
    request.set_height(mHeight);

    auto* transport = request.mutable_transport();
    transport->set_channel(android::emulation::control::ImageTransport::MMAP);
    transport->set_handle(mHandle);

    mReader = mStub->streamScreenshot(mContext.get(), request);

    android::emulation::control::Image image;
    while (mIsStreaming && mReader->Read(&image)) {
        if (mFrameCallback) {
            mFrameCallback();
        }
    }

    auto status = mReader->Finish();
    if (!status.ok() && status.error_code() != grpc::StatusCode::CANCELLED) {
        LOG(ERROR) << "Screenshot stream finished with error: "
                   << status.error_code()
                   << ", msg: " << status.error_message();
    }
}

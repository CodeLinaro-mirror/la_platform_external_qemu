// Copyright (C) 2025 The Android Open Source Project
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
#include "fishtank_agents.h"

#include "host-common/display_agent.h"

const QAndroidDisplayAgent sFishtankQAndroidDisplayAgent = {
        .getFrameBuffer =
                [](int* w, int* h, int* bpp, int* bpl, uint8_t** buffer) {
                    NOT_IMPLEMENTED("QAndroidDisplayAgent.getFrameBuffer(w: %p, h: %p, bpp: %p, bpl: %p, buffer: %p)", w, h, bpp, bpl, buffer);
                },
        .registerUpdateListener =
                [](AndroidDisplayUpdateCallback cb, void* opaque) {
                    NOT_IMPLEMENTED("QAndroidDisplayAgent.registerUpdateListener(cb: %p, opaque: %p)", cb, opaque);
                },
        .unregisterUpdateListener =
                [](AndroidDisplayUpdateCallback cb) {
                    NOT_IMPLEMENTED("QAndroidDisplayAgent.unregisterUpdateListener(cb: %p)", cb);
                },
        .initFrameBufferNoWindow =
                [](QFrameBuffer* fb) { NOT_IMPLEMENTED("QAndroidDisplayAgent.initFrameBufferNoWindow(fb: %p)", fb); },
};

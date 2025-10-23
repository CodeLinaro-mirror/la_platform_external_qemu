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

#include "host-common/multi_display_agent.h"

const QAndroidMultiDisplayAgent sFishtankQAndroidMultiDisplayAgent = {
        .notifyDisplayChanges =
                []() {
                    NOT_IMPLEMENTED("QAndroidMultiDisplayAgent.notifyDisplayChanges");
                    return false;
                },
        .setMultiDisplay =
                [](uint32_t id,
                   int32_t x,
                   int32_t y,
                   uint32_t w,
                   uint32_t h,
                   uint32_t dpi,
                   uint32_t flag,
                   bool add) {
                    NOT_IMPLEMENTED("QAndroidMultiDisplayAgent.setMultiDisplay(id: %u, x: %d, y: %d, w: %u, h: %u, dpi: %u, flag: %u, add: %d)", id, x, y, w, h, dpi, flag, add);
                    return 0;
                },
        .getMultiDisplay =
                [](uint32_t id,
                   int32_t* x,
                   int32_t* y,
                   uint32_t* w,
                   uint32_t* h,
                   uint32_t* dpi,
                   uint32_t* flag,
                   bool* enable) {
                    NOT_IMPLEMENTED("QAndroidMultiDisplayAgent.getMultiDisplay(id: %u)", id);
                    return false;
                },
        .getNextMultiDisplay =
                [](int32_t start_id,
                   uint32_t* id,
                   int32_t* x,
                   int32_t* y,
                   uint32_t* w,
                   uint32_t* h,
                   uint32_t* dpi,
                   uint32_t* flag,
                   uint32_t* cb) {
                    NOT_IMPLEMENTED("QAndroidMultiDisplayAgent.getNextMultiDisplay(start_id: %d, id: %p, x: %p, y: %p, w: %p, h: %p, dpi: %p, flag: %p, cb: %p)", start_id, id, x, y, w, h, dpi, flag, cb);
                    return false;
                },
        .isMultiDisplayEnabled =
                []() {
                    NOT_IMPLEMENTED("QAndroidMultiDisplayAgent.isMultiDisplayEnabled");
                    return false;
                },
        .getCombinedDisplaySize =
                [](uint32_t* width, uint32_t* height) {
                    NOT_IMPLEMENTED("QAndroidMultiDisplayAgent.getCombinedDisplaySize(width: %p, height: %p)", width, height);
                },
        .multiDisplayParamValidate =
                [](uint32_t id,
                   uint32_t w,
                   uint32_t h,
                   uint32_t dpi,
                   uint32_t flag) {
                    NOT_IMPLEMENTED("QAndroidMultiDisplayAgent.multiDisplayParamValidate(id: %u, w: %u, h: %u, dpi: %u, flag: %u)", id, w, h, dpi, flag);
                    return false;
                },
        .translateCoordination =
                [](uint32_t* x, uint32_t* y, uint32_t* displayId) {
                    NOT_IMPLEMENTED("QAndroidMultiDisplayAgent.translateCoordination(x: %p, y: %p, displayId: %p)", x, y, displayId);
                    return false;
                },
        .setGpuMode = [](bool isGuestMode,
                         uint32_t w,
                         uint32_t h) { NOT_IMPLEMENTED("QAndroidMultiDisplayAgent.setGpuMode(isGuestMode: %d, w: %u, h: %u)", isGuestMode, w, h); },
        .createDisplay =
                [](uint32_t* displayId) {
                    NOT_IMPLEMENTED("QAndroidMultiDisplayAgent.createDisplay(displayId: %p)", displayId);
                    return 0;
                },
        .destroyDisplay =
                [](uint32_t displayId) {
                    NOT_IMPLEMENTED("QAndroidMultiDisplayAgent.destroyDisplay(displayId: %u)", displayId);
                    return 0;
                },
        .setDisplayPose =
                [](uint32_t displayId,
                   int32_t x,
                   int32_t y,
                   uint32_t w,
                   uint32_t h,
                   uint32_t dpi) {
                    NOT_IMPLEMENTED("QAndroidMultiDisplayAgent.setDisplayPose(displayId: %u, x: %d, y: %d, w: %u, h: %u, dpi: %u)", displayId, x, y, w, h, dpi);
                    return 0;
                },
        .getDisplayPose =
                [](uint32_t displayId,
                   int32_t* x,
                   int32_t* y,
                   uint32_t* w,
                   uint32_t* h) {
                    NOT_IMPLEMENTED("QAndroidMultiDisplayAgent.getDisplayPose(displayId: %u, x: %p, y: %p, w: %p, h: %p)", displayId, x, y, w, h);
                    return 0;
                },
        .setDisplayColorTransform =
                [](uint32_t displayId, const float colorTransformMatrix[16]) {
                    NOT_IMPLEMENTED("QAndroidMultiDisplayAgent.setDisplayColorTransform(displayId: %u, matrix: %p)", displayId, colorTransformMatrix);
                    return 0;
                },
        .getDisplayColorTransform =
                [](uint32_t displayId, float outColorTransformMatrix[16]) {
                    NOT_IMPLEMENTED("QAndroidMultiDisplayAgent.getDisplayColorTransform(displayId: %u, matrix: %p)", displayId, outColorTransformMatrix);
                    return 0;
                },
        .getDisplayColorBuffer =
                [](uint32_t displayId, uint32_t* colorBuffer) {
                    NOT_IMPLEMENTED("QAndroidMultiDisplayAgent.getDisplayColorBuffer(displayId: %u, colorBuffer: %p)", displayId, colorBuffer);
                    return 0;
                },
        .getColorBufferDisplay =
                [](uint32_t colorBuffer, uint32_t* displayId) {
                    NOT_IMPLEMENTED("QAndroidMultiDisplayAgent.getColorBufferDisplay(colorBuffer: %u, displayId: %p)", colorBuffer, displayId);
                    return 0;
                },
        .setDisplayColorBuffer =
                [](uint32_t displayId, uint32_t colorBuffer) {
                    NOT_IMPLEMENTED("QAndroidMultiDisplayAgent.setDisplayColorBuffer(displayId: %u, colorBuffer: %u)", displayId, colorBuffer);
                    return 0;
                },
        .isMultiDisplayWindow =
                []() {
                    NOT_IMPLEMENTED("QAndroidMultiDisplayAgent.isMultiDisplayWindow");
                    return false;
                },
        .performRotation = [](int rot) { NOT_IMPLEMENTED("QAndroidMultiDisplayAgent.performRotation(rot: %d)", rot); },
        .isPixelFold =
                []() {
                    NOT_IMPLEMENTED("QAndroidMultiDisplayAgent.isPixelFold");
                    return false;
                },
};

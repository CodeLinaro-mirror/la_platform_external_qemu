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

#include "android/console.h"
#include "android/emulation/control/utils/EmulatorControlClient.h"
#include "android/emulation/control/utils/EmulatorGrcpClient.h"
#include "host-common/vm_operations.h"

const QAndroidVmOperations sFishtankQAndroidVmOperations = {
        .vmStop =
                []() {
                    NOT_IMPLEMENTED("QAndroidVmOperations.vmStop");
                    return false;
                },
        .vmStart =
                []() {
                    NOT_IMPLEMENTED("QAndroidVmOperations.vmStart");
                    return false;
                },
        .vmReset = []() { NOT_IMPLEMENTED("QAndroidVmOperations.vmReset"); },
        .vmShutdown =
                []() {
                    auto controlClient = getGlobalControlClient();
                    auto opts = getConsoleAgents()->settings->android_cmdLineOptions();
                    if (!opts->qt_hide_window && controlClient && controlClient->service()) {
                        LOG(INFO) << "Terminating emulator via gRPC...";
                        android::emulation::control::VmRunState request;
                        request.set_state(android::emulation::control::VmRunState::SHUTDOWN);

                        google::protobuf::Empty resp;
                        auto context = controlClient->client()->newContext();
                        auto status = controlClient->service()->setVmState(context.get(), request, &resp);
                        if (!status.ok()) {
                            LOG(ERROR) << "Failed to send shutdown request: " << status.error_message();
                        }
                    }
                },
        .vmPause =
                []() {
                    NOT_IMPLEMENTED("QAndroidVmOperations.vmPause");
                    return false;
                },
        .vmResume =
                []() {
                    NOT_IMPLEMENTED("QAndroidVmOperations.vmResume");
                    return false;
                },
        .vmIsRunning =
                []() {
                    NOT_IMPLEMENTED("QAndroidVmOperations.vmIsRunning");
                    return false;
                },
        .snapshotList =
                [](void* opaque,
                   LineConsumerCallback outConsumer,
                   LineConsumerCallback errConsumer) {
                    NOT_IMPLEMENTED("QAndroidVmOperations.snapshotList");
                    return false;
                },
        .snapshotSave =
                [](const char* name,
                   void* opaque,
                   LineConsumerCallback errConsumer) {
                    NOT_IMPLEMENTED("QAndroidVmOperations.snapshotSave");
                    return false;
                },
        .snapshotLoad =
                [](const char* name,
                   void* opaque,
                   LineConsumerCallback errConsumer) {
                    NOT_IMPLEMENTED("QAndroidVmOperations.snapshotLoad");
                    return false;
                },
        .snapshotDelete =
                [](const char* name,
                   void* opaque,
                   LineConsumerCallback errConsumer) {
                    NOT_IMPLEMENTED("QAndroidVmOperations.snapshotDelete");
                    return false;
                },
        .snapshotRemap =
                [](bool shared,
                   void* opaque,
                   LineConsumerCallback errConsumer) {
                    NOT_IMPLEMENTED("QAndroidVmOperations.snapshotRemap");
                    return false;
                },
        .snapshotExport =
                [](const char* snapshot,
                   const char* dest,
                   void* opaque,
                   LineConsumerCallback errConsumer) {
                    NOT_IMPLEMENTED("QAndroidVmOperations.snapshotExport");
                    return false;
                },
        .snapshotLastLoaded =
                [](void* opaque,
                   LineConsumerCallback outConsumer,
                   LineConsumerCallback errConsumer) {
                    NOT_IMPLEMENTED("QAndroidVmOperations.snapshotLastLoaded");
                    return false;
                },
        .setSnapshotCallbacks =
                [](void* opaque, const SnapshotCallbacks* callbacks) {
                    NOT_IMPLEMENTED("QAndroidVmOperations.setSnapshotCallbacks");
                },
        .setSnapshotProtobuf =
                [](void* pb) {
                    NOT_IMPLEMENTED("QAndroidVmOperations.setSnapshotProtobuf");
                },
        .mapUserBackedRam =
                [](uint64_t gpa, void* hva, uint64_t size) {
                    NOT_IMPLEMENTED("QAndroidVmOperations.mapUserBackedRam");
                },
        .unmapUserBackedRam =
                [](uint64_t gpa, uint64_t size) {
                    NOT_IMPLEMENTED("QAndroidVmOperations.unmapUserBackedRam");
                },
        .getVmConfiguration =
                [](VmConfiguration* out) {
                    NOT_IMPLEMENTED("QAndroidVmOperations.getVmConfiguration");
                },
        .setFailureReason =
                [](const char* name, int failureReason) {
                    NOT_IMPLEMENTED("QAndroidVmOperations.setFailureReason");
                },
        .setExiting =
                []() { NOT_IMPLEMENTED("QAndroidVmOperations.setExiting"); },
        .allowRealAudio =
                [](bool allow) {
                    NOT_IMPLEMENTED("QAndroidVmOperations.allowRealAudio");
                },
        .isRealAudioAllowed =
                []() {
                    NOT_IMPLEMENTED("QAndroidVmOperations.isRealAudioAllowed");
                    return false;
                },
        .getRealAudioEventListener = []() -> void* {
            NOT_IMPLEMENTED("QAndroidVmOperations.getRealAudioEventListener");
            return nullptr;
        },
        .setSkipSnapshotSave =
                [](bool used) {
                    NOT_IMPLEMENTED("QAndroidVmOperations.setSkipSnapshotSave");
                },
        .isSnapshotSaveSkipped =
                []() {
                    NOT_IMPLEMENTED("QAndroidVmOperations.isSnapshotSaveSkipped");
                    return false;
                },
        .getRunState =
                []() {
                    NOT_IMPLEMENTED("QAndroidVmOperations.getRunState");
                    return QEMU_RUN_STATE_SHUTDOWN;
                },
        .setDisplay =
                [](int32_t id, int32_t w, int32_t h, uint32_t dpi) {
                    NOT_IMPLEMENTED("QAndroidVmOperations.setDisplay");
                    return false;
                },
        .system_shutdown_request =
                [](QemuShutdownCause reason) {
                    NOT_IMPLEMENTED("QAndroidVmOperations.system_shutdown_request");
                },
        .setSkipSnapshotSaveReason =
                [](uint32_t reason) {
                    NOT_IMPLEMENTED(
                            "QAndroidVmOperations.setSkipSnapshotSaveReason");
                },
        .getSkipSnapshotSaveReason =
                []() {
                    NOT_IMPLEMENTED(
                            "QAndroidVmOperations.getSkipSnapshotSaveReason");
                    return SNAPSHOT_SKIP_UNKNOWN;
                },
        .setStatSnapshotUseVulkan =
                []() {
                    NOT_IMPLEMENTED(
                            "QAndroidVmOperations.setStatSnapshotUseVulkan");
                },
        .snapshotUseVulkan =
                []() {
                    NOT_IMPLEMENTED("QAndroidVmOperations.snapshotUseVulkan");
                    return false;
                }};

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
#include "host-common/MultiDisplay.h"
#include "emulator_controller.pb.h"

#include <algorithm>
#include <vector>

using android::emulation::control::DisplayConfigurations;
using android::emulation::control::DisplayConfiguration;

const QAndroidMultiDisplayAgent sFishtankQAndroidMultiDisplayAgent = {
        .notifyDisplayChanges =
                []() {
                    dinfo("FishtankAgents (MultiDisplay): notifyDisplayChanges called");
                    // notifyDisplayChanges is a local notification trigger.
                    // Since we are a gRPC client, the server will trigger this
                    // automatically when we set configurations.
                    // If we need to trigger it locally, we return true to indicate
                    // we "handled" it (or at least didn't fail).
                    return true;
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
                    dinfo("FishtankAgents (MultiDisplay): setMultiDisplay called: id=%u, x=%d, y=%d, w=%u, h=%u, dpi=%u, flag=%u, add=%d",
                          id, x, y, w, h, dpi, flag, add);
                    auto client = getGlobalControlClient();
                    if (!client) {
                        derror("FishtankAgents (MultiDisplay): setMultiDisplay failed: global control client is null");
                        return -1;
                    }
                    auto context = client->client()->newContext();
                    google::protobuf::Empty request;
                    DisplayConfigurations configs;
                    auto status = client->service()->getDisplayConfigurations(context.get(), request, &configs);
                    if (!status.ok()) {
                        derror("FishtankAgents (MultiDisplay): getDisplayConfigurations failed: %s", status.error_message().c_str());
                        return -1;
                    }

                    // Find if it already exists
                    bool found = false;
                    DisplayConfigurations new_configs;
                    new_configs.set_userconfigurable(configs.userconfigurable());
                    new_configs.set_maxdisplays(configs.maxdisplays());

                    for (const auto& display : configs.displays()) {
                        if (display.display() == 0) {
                            // Skip default display, it cannot be modified/deleted via this RPC
                            continue;
                        }
                        if (display.display() == id) {
                            found = true;
                            if (add) {
                                // Update
                                auto* new_disp = new_configs.add_displays();
                                new_disp->set_display(id);
                                new_disp->set_width(w);
                                new_disp->set_height(h);
                                new_disp->set_dpi(dpi);
                                new_disp->set_flags(flag);
                            }
                            // If !add, we don't add it to new_configs, which effectively deletes it
                        } else {
                            // Keep others
                            auto* new_disp = new_configs.add_displays();
                            new_disp->CopyFrom(display);
                        }
                    }

                    if (!found && add) {
                        // Add new
                        auto* new_disp = new_configs.add_displays();
                        new_disp->set_display(id);
                        new_disp->set_width(w);
                        new_disp->set_height(h);
                        new_disp->set_dpi(dpi);
                        new_disp->set_flags(flag);
                    }

                    dinfo("FishtankAgents (MultiDisplay): setDisplayConfigurations request: user_configurable=%d, max_displays=%u, displays_size=%d",
                          new_configs.userconfigurable(), new_configs.maxdisplays(), new_configs.displays_size());
                    for (int i = 0; i < new_configs.displays_size(); ++i) {
                        const auto& d = new_configs.displays(i);
                        dinfo("  display %d: id=%u, w=%u, h=%u, dpi=%u, flags=%u",
                              i, d.display(), d.width(), d.height(), d.dpi(), d.flags());
                    }

                    auto set_context = client->client()->newContext();
                    DisplayConfigurations response;
                    status = client->service()->setDisplayConfigurations(set_context.get(), new_configs, &response);

                    if (status.ok()) {
                        dinfo("FishtankAgents (MultiDisplay): setDisplayConfigurations gRPC succeeded");
                        if (const auto windowAgent = getFishtankEmulatorWindowAgent()) {
                            dinfo("FishtankAgents (MultiDisplay): Notifying local window agent");
                            if (add) {
                                windowAgent->addMultiDisplayWindow(id, true, w, h);
                            } else {
                                windowAgent->addMultiDisplayWindow(id, false, 0, 0);
                            }
                            windowAgent->updateUIMultiDisplayPage(id);
                        } else {
                            dwarning("FishtankAgents (MultiDisplay): local window agent is null, cannot notify UI");
                        }
                        return 0;
                    }
                    derror("FishtankAgents (MultiDisplay): setDisplayConfigurations gRPC failed: %s", status.error_message().c_str());
                    return -1;
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
                    dinfo("FishtankAgents (MultiDisplay): getMultiDisplay called: id=%u", id);
                    auto client = getGlobalControlClient();
                    if (!client) {
                        derror("FishtankAgents (MultiDisplay): getMultiDisplay failed: global control client is null");
                        if (enable) *enable = false;
                        return false;
                    }
                    auto context = client->client()->newContext();
                    google::protobuf::Empty request;
                    DisplayConfigurations response;
                    auto status = client->service()->getDisplayConfigurations(context.get(), request, &response);
                    if (!status.ok()) {
                        derror("FishtankAgents (MultiDisplay): getDisplayConfigurations failed: %s", status.error_message().c_str());
                        if (enable) *enable = false;
                        return false;
                    }
                    for (const auto& display : response.displays()) {
                        if (display.display() == id) {
                            if (x) *x = 0;
                            if (y) *y = 0;
                            if (w) *w = display.width();
                            if (h) *h = display.height();
                            if (dpi) *dpi = display.dpi();
                            if (flag) *flag = display.flags();
                            if (enable) *enable = true;
                            dinfo("FishtankAgents (MultiDisplay): getMultiDisplay found active display: w=%u, h=%u, dpi=%u, flag=%u",
                                  display.width(), display.height(), display.dpi(), display.flags());
                            return true;
                        }
                    }
                    dinfo("FishtankAgents (MultiDisplay): getMultiDisplay display id=%u not found (disabled)", id);
                    if (enable) *enable = false;
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
                    // getNextMultiDisplay is used to iterate over active displays.
                    // We can implement it by getting all displays and finding the next one after start_id.
                    auto client = getGlobalControlClient();
                    if (!client) {
                        return false;
                    }
                    auto context = client->client()->newContext();
                    google::protobuf::Empty request;
                    DisplayConfigurations response;
                    auto status = client->service()->getDisplayConfigurations(context.get(), request, &response);
                    if (!status.ok()) {
                        return false;
                    }

                    uint32_t next_id = 0xFFFFFFFF;
                    const DisplayConfiguration* next_disp = nullptr;

                    for (const auto& display : response.displays()) {
                        if ((int32_t)display.display() > start_id && display.display() < next_id) {
                            next_id = display.display();
                            next_disp = &display;
                        }
                    }

                    if (next_disp) {
                        if (id) *id = next_disp->display();
                        if (x) *x = 0;
                        if (y) *y = 0;
                        if (w) *w = next_disp->width();
                        if (h) *h = next_disp->height();
                        if (dpi) *dpi = next_disp->dpi();
                        if (flag) *flag = next_disp->flags();
                        if (cb) *cb = 0; // Color buffer not supported via gRPC yet
                        return true;
                    }
                    return false;
                },
        .isMultiDisplayEnabled =
                []() {
                    auto client = getGlobalControlClient();
                    if (!client) {
                        return false;
                    }
                    auto context = client->client()->newContext();
                    google::protobuf::Empty request;
                    DisplayConfigurations response;
                    auto status = client->service()->getDisplayConfigurations(context.get(), request, &response);
                    if (!status.ok()) {
                        return false;
                    }
                    return response.displays_size() > 1;
                },
        .getCombinedDisplaySize =
                [](uint32_t* width, uint32_t* height) {
                    dinfo("FishtankAgents (MultiDisplay): getCombinedDisplaySize called");
                    auto client = getGlobalControlClient();
                    if (!client) {
                        derror("FishtankAgents (MultiDisplay): getCombinedDisplaySize failed: global control client is null");
                        if (width) *width = 0;
                        if (height) *height = 0;
                        return;
                    }
                    auto context = client->client()->newContext();
                    google::protobuf::Empty request;
                    DisplayConfigurations response;
                    auto status = client->service()->getDisplayConfigurations(context.get(), request, &response);
                    if (!status.ok()) {
                        derror("FishtankAgents (MultiDisplay): getDisplayConfigurations failed: %s", status.error_message().c_str());
                        if (width) *width = 0;
                        if (height) *height = 0;
                        return;
                    }
                    uint32_t total_w = 0;
                    uint32_t total_h = 0;
                    for (const auto& display : response.displays()) {
                        total_w += display.width();
                        total_h = std::max(total_h, display.height());
                    }
                    if (width) *width = total_w;
                    if (height) *height = total_h;
                    dinfo("FishtankAgents (MultiDisplay): getCombinedDisplaySize calculated: w=%u, h=%u", total_w, total_h);
                },
        .multiDisplayParamValidate =
                [](uint32_t id,
                   uint32_t w,
                   uint32_t h,
                   uint32_t dpi,
                   uint32_t flag) {
                    return android::MultiDisplay::multiDisplayParamValidate(
                            id, w, h, dpi, flag, nullptr);
                },
        .translateCoordination =
                [](uint32_t* x, uint32_t* y, uint32_t* displayId) {
                    NOT_IMPLEMENTED("QAndroidMultiDisplayAgent.translateCoordination");
                    return false;
                },
        .setGpuMode = [](bool isGuestMode,
                         uint32_t w,
                         uint32_t h) { NOT_IMPLEMENTED("QAndroidMultiDisplayAgent.setGpuMode"); },
        .createDisplay =
                [](uint32_t* displayId) {
                    // We don't support direct display creation without pose.
                    // We can just return a dummy success or not implemented.
                    NOT_IMPLEMENTED("QAndroidMultiDisplayAgent.createDisplay");
                    return -1;
                },
        .destroyDisplay =
                [](uint32_t displayId) {
                    dinfo("FishtankAgents (MultiDisplay): destroyDisplay called: id=%u", displayId);
                    return sFishtankQAndroidMultiDisplayAgent.setMultiDisplay(displayId, -1, -1, 0, 0, 0, 0, false);
                },
        .setDisplayPose =
                [](uint32_t displayId,
                   int32_t x,
                   int32_t y,
                   uint32_t w,
                   uint32_t h,
                   uint32_t dpi) {
                    dinfo("FishtankAgents (MultiDisplay): setDisplayPose called: id=%u, x=%d, y=%d, w=%u, h=%u, dpi=%u",
                          displayId, x, y, w, h, dpi);
                    return sFishtankQAndroidMultiDisplayAgent.setMultiDisplay(displayId, x, y, w, h, dpi, 0, true);
                },
        .getDisplayPose =
                [](uint32_t displayId,
                   int32_t* x,
                   int32_t* y,
                   uint32_t* w,
                   uint32_t* h) {
                    dinfo("FishtankAgents (MultiDisplay): getDisplayPose called: id=%u", displayId);
                    uint32_t dpi = 0;
                    uint32_t flag = 0;
                    bool enabled = false;
                    if (sFishtankQAndroidMultiDisplayAgent.getMultiDisplay(displayId, x, y, w, h, &dpi, &flag, &enabled)) {
                        return 0;
                    }
                    return -1;
                },
        .setDisplayColorTransform =
                [](uint32_t displayId, const float colorTransformMatrix[16]) {
                    NOT_IMPLEMENTED("QAndroidMultiDisplayAgent.setDisplayColorTransform");
                    return -1;
                },
        .getDisplayColorTransform =
                [](uint32_t displayId, float outColorTransformMatrix[16]) {
                    NOT_IMPLEMENTED("QAndroidMultiDisplayAgent.getDisplayColorTransform");
                    return -1;
                },
        .getDisplayColorBuffer =
                [](uint32_t displayId, uint32_t* colorBuffer) {
                    NOT_IMPLEMENTED("QAndroidMultiDisplayAgent.getDisplayColorBuffer");
                    return -1;
                },
        .getColorBufferDisplay =
                [](uint32_t colorBuffer, uint32_t* displayId) {
                    NOT_IMPLEMENTED("QAndroidMultiDisplayAgent.getColorBufferDisplay");
                    return -1;
                },
        .setDisplayColorBuffer =
                [](uint32_t displayId, uint32_t colorBuffer) {
                    NOT_IMPLEMENTED("QAndroidMultiDisplayAgent.setDisplayColorBuffer");
                    return -1;
                },
        .isMultiDisplayWindow =
                []() {
                    // Fishtank usually runs in a multi-display window mode if it's managing multiple windows.
                    // We can return true if we have multiple displays, or just default to false if we don't know.
                    return sFishtankQAndroidMultiDisplayAgent.isMultiDisplayEnabled();
                },
        .performRotation = [](int rot) { NOT_IMPLEMENTED("QAndroidMultiDisplayAgent.performRotation"); },
        .isPixelFold =
                []() {
                    // We don't easily know if it's a Pixel Fold without querying status or features.
                    // But we can assume false for now, or try to query if needed.
                    return false;
                },
};

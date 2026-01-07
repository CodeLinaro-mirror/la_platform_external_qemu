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
#include "aemu/base/logging/LogSeverity.h"
#include "fishtank_agents.h"

#include "android/emulation/control/user_event_agent.h"
#include "android/emulation/control/utils/EmulatorControlClient.h"
#include "android/grpc/utils/SimpleAsyncGrpc.h"
#include "android/skin/event.h"
#include "android/skin/generic-event-buffer.h"
#include "android/utils/debug.h"
#include "emulator_controller.pb.h"

using ::SimpleClientWriter;
using android::emulation::control::EmulatorControlClient;
using android::emulation::control::InputEvent;
using android::emulation::control::KeyboardEvent;
using android::emulation::control::MouseEvent;
using android::emulation::control::Touch;
using android::emulation::control::TouchEvent;
using android::emulation::control::TouchpadEvent;
using android::emulation::control::WheelEvent;

static std::shared_ptr<SimpleClientWriter<InputEvent>> sInputEventWriter;

void initializeGrpcUserEventAgent(EmulatorControlClient* client) {
    if (client) {
        sInputEventWriter = client->asyncInputEventWriter();
    } else {
        LOG(ERROR)
                << "Cannot initialize GrpcUserEventAgent with a null client.";
    }
}

static void grpc_sendKey(unsigned keycode, bool is_down) {
    if (!sInputEventWriter)
        return;

    auto keyEvent = std::make_unique<KeyboardEvent>();
    keyEvent->set_codetype(KeyboardEvent::Evdev);
    keyEvent->set_eventtype(is_down ? KeyboardEvent::keydown
                                    : KeyboardEvent::keyup);
    keyEvent->set_keycode(keycode);

    auto inputEvent = std::make_unique<InputEvent>();
    inputEvent->set_allocated_key_event(keyEvent.release());
    sInputEventWriter->Write(*inputEvent);
}

static void grpc_sendKeyCode(int keycode) {
    if (!sInputEventWriter)
        return;

    auto keyEvent = std::make_unique<KeyboardEvent>();
    keyEvent->set_codetype(KeyboardEvent::Evdev);

    // EvDev keydow/up mask.
    bool down = keycode & 0x400;
    keyEvent->set_eventtype(down ? KeyboardEvent::keydown
                                 : KeyboardEvent::keyup);
    keyEvent->set_keycode(keycode);
    if (VERBOSE_CHECK(keys))
        LOG(INFO) << "Keyboard: " << keyEvent->ShortDebugString();

    auto inputEvent = std::make_unique<InputEvent>();
    inputEvent->set_allocated_key_event(keyEvent.release());
    sInputEventWriter->Write(*inputEvent);
}

static void grpc_sendKeyCodes(int* keycodes, int count) {
    if (!sInputEventWriter)
        return;

    for (int i = 0; i < count; ++i) {
        grpc_sendKeyCode(keycodes[i]);
    }
}

static void setupTouch(Touch* touch,
                       const SkinEvent* const ev) {
    touch->set_x(ev->u.multi_touch_point.x);
    touch->set_y(ev->u.multi_touch_point.y);
    touch->set_identifier(ev->u.multi_touch_point.id);
    touch->set_pressure(ev->u.multi_touch_point.pressure);
    touch->set_touch_major(ev->u.multi_touch_point.touch_major);
    touch->set_touch_minor(ev->u.multi_touch_point.touch_minor);
}

static void grpc_sendTouchEvents(const SkinEvent* const event, int displayId) {
    if (!sInputEventWriter)
        return;

    auto touchEvent = std::make_unique<TouchEvent>();
    Touch* touch = touchEvent->add_touches();
    setupTouch(touch, event);
    if (VERBOSE_CHECK(keys))
        LOG(INFO) << "Touch: " << touch->ShortDebugString();

    auto inputEvent = std::make_unique<InputEvent>();
    inputEvent->set_allocated_touch_event(touchEvent.release());
    sInputEventWriter->Write(*inputEvent);
}

static void grpc_sendTouchpadEvents(const SkinEvent* const event, int touchpadId) {
    if (!sInputEventWriter)
        return;

    auto touchpadEvent = std::make_unique<TouchpadEvent>();
    Touch* touch = touchpadEvent->add_touches();
    touchpadEvent->set_touchpad(touchpadId);
    setupTouch(touch, event);
    if (VERBOSE_CHECK(keys))
        LOG(INFO) << "Touchpad: " << touch->ShortDebugString();

    auto inputEvent = std::make_unique<InputEvent>();
    inputEvent->set_allocated_touchpad_event(touchpadEvent.release());
    sInputEventWriter->Write(*inputEvent);
}

static void grpc_sendMouseEvent(int dx,
                                int dy,
                                int dz,
                                int buttons_state,
                                int display_id,
                                MouseEventMode mode) {
    if (!sInputEventWriter)
        return;

    auto mouseEvent = std::make_unique<MouseEvent>();
    mouseEvent->set_x(dx);
    mouseEvent->set_y(dy);
    mouseEvent->set_buttons(buttons_state);
    mouseEvent->set_display(display_id);
    if (VERBOSE_CHECK(keys))
        LOG(INFO) << "Mouse: " << mouseEvent->ShortDebugString();

    auto inputEvent = std::make_unique<InputEvent>();
    inputEvent->set_allocated_mouse_event(mouseEvent.release());
    sInputEventWriter->Write(*inputEvent);
}

static void grpc_sendMouseWheelEvent(int dx, int dy, int displayId) {
    if (!sInputEventWriter)
        return;

    auto wheelEvent = std::make_unique<WheelEvent>();
    wheelEvent->set_dx(dx);
    wheelEvent->set_dy(dy);
    wheelEvent->set_display(displayId);
    if (VERBOSE_CHECK(keys))
        LOG(INFO) << "Wheel: " << wheelEvent->ShortDebugString();

    auto inputEvent = std::make_unique<InputEvent>();
    inputEvent->set_allocated_wheel_event(wheelEvent.release());

    sInputEventWriter->Write(*inputEvent);
}

static void grpc_sendRotaryEvent(int rotation) {
    // Simulate rotary event as a vertical mouse wheel event.
    grpc_sendMouseWheelEvent(0, rotation, 0);
}

static void grpc_sendGenericEvent(SkinGenericEventCode code) {
    if (!sInputEventWriter)
        return;

    auto keyEvent = std::make_unique<KeyboardEvent>();
    keyEvent->set_eventtype(KeyboardEvent::keypress);

    if (VERBOSE_CHECK(keys))
        LOG(INFO) << "Send generic event code: "
                  << keyEvent->ShortDebugString();
    auto inputEvent = std::make_unique<InputEvent>();
    inputEvent->set_allocated_key_event(keyEvent.release());
    sInputEventWriter->Write(*inputEvent);
}

const QAndroidUserEventAgent sFishtankQAndroidUserEventAgent = {
        .sendKey = grpc_sendKey,
        .sendKeyCode = grpc_sendKeyCode,
        .sendKeyCodes = grpc_sendKeyCodes,
        .sendTouchEvents = grpc_sendTouchEvents,
        .sendTouchpadEvents = grpc_sendTouchpadEvents,
        .sendMouseEvent = grpc_sendMouseEvent,
        .sendPenEvent =
                [](int displayId,
                   int type,
                   const SkinEvent* events,
                   int count,
                   int buttons_state) {
                    NOT_IMPLEMENTED("QAndroidUserEventAgent.sendPenEvent");
                },
        .sendMouseWheelEvent = grpc_sendMouseWheelEvent,
        .sendRotaryEvent = grpc_sendRotaryEvent,
        .sendGenericEvent = grpc_sendGenericEvent,
        .sendGenericEvents =
                [](SkinGenericEventCode* codes, int count) {
                    for (int i = 0; i < count; ++i) {
                        grpc_sendGenericEvent(codes[i]);
                    }
                },
        .onNewUserEvent = []() { /* No-op for gRPC */ },
        .eventsDropped =
                []() {
                    // We don't have a good way to check this with the async
                    // writer.
                    return 0;
                },
};

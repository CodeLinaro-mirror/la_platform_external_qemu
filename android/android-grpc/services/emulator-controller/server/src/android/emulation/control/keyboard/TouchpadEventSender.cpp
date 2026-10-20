#include "android/emulation/control/keyboard/TouchpadEventSender.h"

#include <chrono>
#include <vector>

#include "aemu/base/Log.h"

#include "android/emulation/control/user_event_agent.h"
#include "android/skin/event.h"
#include "android/skin/generic-event-buffer.h"
#include "emulator_controller.pb.h"
#include "google/protobuf/repeated_field.h"

namespace android {
namespace emulation {
namespace control {

void TouchpadEventSender::doSend(const TouchpadEvent request) {
    // Obtain display width, height for the given display id.
    auto touchpadId = request.touchpad();

    // Sends a sequence of touch events to the linux kernel using "Protocol B"
    std::vector<SkinGenericEventCode> events;

    // Let's expunge expired events..
    // This alleviates problems where a developer never closes out a slot.
    auto now = std::chrono::system_clock::now();
    std::chrono::seconds epoch =
            std::chrono::duration_cast<std::chrono::seconds>(
                    now.time_since_epoch());

    SkinEvent skin_event;
    for (auto it = mIdLastUsedEpoch.begin(); it != mIdLastUsedEpoch.end();) {
        if (it->second < epoch) {
            LOG(WARNING) << "Expiring outdated touchpad event id: "
                         << it->first;

            // First create an up event, otherwise android kernel might get
            // confused
            skin_event = createSkinEvent(kEventTouchEnd);
            skin_event.u.multi_touch_point.id = it->first;
            skin_event.u.multi_touch_point.skip_sync = true;
            mAgents->user_event->sendTouchpadEvents(&skin_event, touchpadId);

            it = mIdLastUsedEpoch.erase(it);
        } else {
            ++it;
        }
    }

    bool event_set = false;

    for (auto touch : request.touches()) {
        int slot = 0;
        // We only need to send with a sync for the final reported finger
        // After the loop, we will send the final event, which will also
        // trigger an EV_SYN
        if (event_set) {
            skin_event.u.multi_touch_point.skip_sync = true;
            mAgents->user_event->sendTouchpadEvents(&skin_event, touchpadId);
        }

        if (touch.pressure() > 0) {
            if (mIdLastUsedEpoch.count(touch.identifier()) == 0)
                skin_event = createSkinEvent(kEventTouchBegin);
            else
                skin_event = createSkinEvent(kEventTouchUpdate);
            skin_event.u.multi_touch_point.x = touch.x();
            skin_event.u.multi_touch_point.y = touch.y();
            skin_event.u.multi_touch_point.id = touch.identifier();
            skin_event.u.multi_touch_point.pressure = touch.pressure();

            if (touch.expiration() == Touch::NEVER_EXPIRE) {
                mIdLastUsedEpoch[touch.identifier()] =
                        std::chrono::seconds(LONG_MAX);
            } else {
                mIdLastUsedEpoch[touch.identifier()] =
                        epoch + kTOUCH_EXPIRE_AFTER_120S;
            }
        } else {
            // Clean up if pressure is 0. Ignoring the possibility that this is
            // negative for now.
            skin_event = createSkinEvent(kEventTouchEnd);
            skin_event.u.multi_touch_point.id = touch.identifier();

            mIdLastUsedEpoch.erase(touch.identifier());
        }

        // If we have more events to send, this one will be sent at the start of
        // the next loop
        event_set = true;
    }

    if (event_set) {
        mAgents->user_event->sendTouchpadEvents(&skin_event, touchpadId);
    }
}
}  // namespace control
}  // namespace emulation
}  // namespace android

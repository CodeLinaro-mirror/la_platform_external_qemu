// Copyright (C) 2023 The Android Open Source Project
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
#include <cassert>
#include <memory>
#include <utility>

#include "aemu/base/async/ThreadLooper.h"
#include "android/utils/debug.h"

// Only a pointer is stored; users that dereference mAgents must include
// "android/console.h" themselves.
typedef struct AndroidConsoleAgents AndroidConsoleAgents;

namespace android {
namespace emulation {
namespace control {

// Base class for senders that forward input events to the emulator on the
// main looper.
//
// Instances MUST be owned by a std::shared_ptr (use std::make_shared). send()
// dispatches asynchronously, and the queued closure holds a strong reference
// to the sender so that the complete derived object outlives the dispatch,
// even if the owner is destroyed on another thread in the meantime.
template <typename T>
class EventSender : public std::enable_shared_from_this<EventSender<T>> {
public:
    EventSender(const AndroidConsoleAgents* const agents) : mAgents(agents) {}
    virtual ~EventSender() = default;

    // Sends the event to the emulator on the main looper. Events sent from an
    // instance that is not owned by a std::shared_ptr are dropped, since there
    // is no way to guarantee that the instance is alive when the closure runs.
    void send(const T event) {
        auto self = this->weak_from_this().lock();
        if (!self) {
            assert(false && "EventSender must be owned by a std::shared_ptr");
            derror("%s: EventSender is not shared-owned, dropping event",
                   __func__);
            return;
        }
        android::base::ThreadLooper::runOnMainLooper(
                [self = std::move(self), event] { self->doSend(event); });
    }

protected:
    virtual void doSend(const T event) = 0;
    const AndroidConsoleAgents* const mAgents;
};

}  // namespace control
}  // namespace emulation
}  // namespace android

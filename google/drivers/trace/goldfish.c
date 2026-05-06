// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "qemu/osdep.h"
#include "trace/goldfish.h"


static void goldfish_trace_stub(const GoldfishTraceData* data) {
    // Do nothing.
    (void)data;
}

GoldfishTraceCallback goldfish_trace_callback = goldfish_trace_stub;

GoldfishTraceCallback qemu_goldfish_set_trace_callback(GoldfishTraceCallback cb) {
    if (!cb) {
        cb = goldfish_trace_stub;
    }
    GoldfishTraceCallback prev = goldfish_trace_callback;
    goldfish_trace_callback = cb;
    return prev;
}

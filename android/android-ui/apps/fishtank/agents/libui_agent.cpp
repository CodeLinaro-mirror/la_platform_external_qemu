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

#include "aemu/base/logging/CLog.h"
#include "android/emulation/control/libui_agent.h"
#include "android/skin/charmap.h"
#include "android/skin/keycode-buffer.h"
#include "android/skin/winsys.h"

static int utf8_next(const unsigned char** pp, const unsigned char* end) {
    const unsigned char* p = *pp;
    int result = -1;

    if (p < end) {
        int c = *p++;
        if (c >= 128) {
            if ((c & 0xe0) == 0xc0)
                c &= 0x1f;
            else if ((c & 0xf0) == 0xe0)
                c &= 0x0f;
            else
                c &= 0x07;

            while (p < end && (p[0] & 0xc0) == 0x80) {
                c = (c << 6) | (p[0] & 0x3f);
                p++;
            }
        }
        result = c;
        *pp = p;
    }
    return result;
}

const QAndroidLibuiAgent sFishtankQAndroidLibuiAgent = {
        // convertUtf8ToKeyCodeEvents
        [](const unsigned char* text,
           int len,
           LibuiKeyCodeSendFunc sendFunc,
           void* context) -> bool {
            const auto charmap = skin_charmap_get();
            if (!charmap) {
                return false;
            }

            SkinKeycodeBuffer keycodes;
            skin_keycode_buffer_init(&keycodes, (SkinKeyCodeFlushFunc)sendFunc);
            keycodes.context = context;

            const auto end = text + len;
            while (text < end) {
                const int c = utf8_next(&text, end);
                if (c <= 0)
                    break;

                skin_charmap_reverse_map_unicode(charmap, (unsigned)c, 1,
                                                 &keycodes);
                skin_charmap_reverse_map_unicode(charmap, (unsigned)c, 0,
                                                 &keycodes);
                skin_keycode_buffer_flush(&keycodes);
            }
            return true;
        },
        // requestExit
        [](int exitCode, const char* message) {
            // Unfortunately we don't have any way of passing code/message now.
            skin_winsys_quit_request();
        },
        // requestRestart
        [](int exitCode, const char* message) {
                // LOG(FATAL) << "Cannot restart yet";
        //     android::base::restartEmulator();
        },
};

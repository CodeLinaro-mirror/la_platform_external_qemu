/* Copyright (C) 2026 The Android Open Source Project
**
** This software is licensed under the terms of the GNU General Public
** License version 2, as published by the Free Software Foundation, and
** may be copied, distributed, and modified under those terms.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
*/


#include "aemu/base/utils/stream.h"
#include "aemu/base/EventNotificationSupport.h"  // for EventNotifi...
#include "android/emulation/android_qemud.h"
#include "lights_conn.pb.h"
#include "xr_emulator_conn.pb.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>


// used to initialize the xr light status support
extern void android_hw_xrlights_init();

typedef enum {
    WORLD = 0,
    USER = 1,
} LightFacing;

struct HwXrLightStatus {
    // world facing led light color, packed in ARGB order.
    uint32_t world_color = 0;

    // user facing led light color, packed in ARGB order.
    uint32_t user_color = 0;
};

class HwXrLights {

public:
    HwXrLights();
    QemudClient* initializeQemudClient(int channel, const char* client_param);
    void qemudClientRecv(uint8_t* msg, int msglen);
    uint32_t getLightStatus(uint32_t id);
    void setLightStatus(uint32_t id, uint32_t color);

private:
    QemudService* mService = nullptr;
    HwXrLightStatus mHwXrLightStatus = {};
};

class HwXrLightsClient {

public:
    explicit HwXrLightsClient(HwXrLights* hwXrLights) : mHwXrLights(hwXrLights) {}
    static void close(void* opaque);
    static void recv(void* opaque,
                     uint8_t* msg,
                     int msglen,
                     QemudClient* qemudClient);
    void setQemudClient(QemudClient* client) {mQemudClient = client;}

private:
    HwXrLights* mHwXrLights = nullptr;
    QemudClient* mQemudClient = nullptr;
};
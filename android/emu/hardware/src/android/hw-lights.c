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

#include "android/hw-lights.h"

#include "android/emulation/android_qemud.h"
#include "android/console.h"
#include "android/utils/debug.h"
#include "android/utils/misc.h"
#include "aemu/base/utils/stream.h"
#include "android/utils/system.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define  E(...)    derror(__VA_ARGS__)
#define  W(...)    dwarning(__VA_ARGS__)
#define  D(...)  VERBOSE_PRINT(init,__VA_ARGS__)
#define  V(...)  VERBOSE_PRINT(init,__VA_ARGS__)

/***********************************************************************************

            All the declarations

***********************************************************************************/

typedef struct HwLightsClient   HwLightsClient;

typedef struct {
    QemudService*       qemu_listen_service;
    HwLightsClient*     lights_clients;
} HwLightsService;

struct HwLightsClient {
    HwLightsClient*     next;
    HwLightsService*    lights;
    QemudClient*        qemu_client;
};

 static void
_hwLightsClient_recv(void* opaque, uint8_t* msg, int msglen,
                      QemudClient*  client );

static void
_hwLightsClient_close(void* opaque);

static HwLightsClient*
_hwLightsClient_new( HwLightsService* lights);

/* the only static variable */
static HwLightsService _lightsState[1];

static QemudClient*
_hwLights_connect(void*  opaque,
                  QemudService*  service,
                  int  channel,
                  const char* client_param);
static void
_hwLightsClient_removeFromList(HwLightsClient** lhead,
        HwLightsClient* target);

/***********************************************************************************

            All the public methods

***********************************************************************************/

void
android_hw_lights_init( void )
{
    HwLightsService* lights = _lightsState;

    if (lights->qemu_listen_service == NULL) {
        lights->qemu_listen_service = qemud_service_register("lightsservice", 0, lights,
                _hwLights_connect,
                NULL, /* no save */
                NULL /* no load */);
        D("%s: lights qemud listen service initialized", __FUNCTION__);
    }
}

/***********************************************************************************

            All the static methods

***********************************************************************************/

static QemudClient*
_hwLights_connect(void*  opaque,
                  QemudService*  service,
                  int  channel,
                  const char* client_param)
{
    HwLightsService* lights = opaque;
    HwLightsClient* lights_client = _hwLightsClient_new(lights);
    QemudClient* client  = qemud_client_new(service, channel, client_param, lights_client,
                                                _hwLightsClient_recv,
                                                _hwLightsClient_close,
                                                NULL, /* no save */
                                                NULL /* no load */ );
    qemud_client_set_framing(client, 1);
    lights_client->qemu_client = client;
    D("%s: connect lights qemud is called", __FUNCTION__);
    return client;
}

// The current message format is pure string, only for log and test purpose.
// TODO(b/504670848): refine the message format.
// The color is in ARGB with the alpha channel expected to be 255, but ignored.
static void
_hwLightsClient_recv(void* opaque, uint8_t* msg, int msglen,
                      QemudClient*  client )
{
    D("%s: received light status message: %s", __FUNCTION__, msg);
}

// Insert the new HwLightsClient into the current clients list. Make it the new list head.
static HwLightsClient*
_hwLightsClient_new(HwLightsService* lights)
{
    HwLightsClient*  lights_client;
    ANEW0(lights_client);
    lights_client->lights = lights;
    lights_client->next = lights->lights_clients;
    lights->lights_clients = lights_client;
    return lights_client;
}

static void
_hwLightsClient_close(void* opaque)
{
    HwLightsClient*      lights_client = opaque;
    if (lights_client->lights) {
        HwLightsClient** pnode = &lights_client->lights->lights_clients;
        _hwLightsClient_removeFromList(pnode, lights_client);
        lights_client->next = NULL;
        lights_client->lights = NULL;
    }
    AFREE(lights_client);
}

static void
_hwLightsClient_removeFromList(HwLightsClient** phead, HwLightsClient* target)
{
    for (;;) {
        HwLightsClient* node = *phead;
        if (node == NULL)
            break;
        if (node == target) {
            *phead = target->next;
            break;
        }
        phead = &node->next;
    }
}

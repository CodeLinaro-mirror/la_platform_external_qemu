/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>

#include <stdio.h>
#include <stdlib.h>

#include "android/camera/camera-service.h"

#include "android/boot-properties.h"
#include "android/camera/camera-capture.h"
#include "android/camera/camera-format-converters.h"
#include "android/camera/camera-metrics.h"
#include "android/camera/camera-videoplayback.h"
#include "android/camera/camera-virtualscene.h"
#include "host-common/address_space_device.h"
#include "android/emulation/android_qemud.h"
#include "host-common/feature_control.h"
#include "host-common/hw-config.h"
#include "host-common/hw-config-helper.h"
#include "android/avd/info.h"
#include "android/console.h" /* for android_hw */
#include "android/hw-sensors.h"
#include "android/utils/debug.h"
#include "android/utils/looper.h"
#include "android/utils/misc.h"
#include "android/utils/system.h"
#include "host-common/hw-config.h"

#define  E(...)    derror(__VA_ARGS__)
#define  W(...)    dwarning(__VA_ARGS__)

/* Camera service version 1 */
#define V1 ((avdInfo_getApiLevel(getConsoleAgents()->settings->avdInfo()) > 29) && \
            !feature_is_enabled(kFeature_Minigbm))

static constexpr size_t MAX_CAMERA = 8;
static constexpr size_t MAX_QUERY_MESSAGE_SIZE = 8092;

struct CameraServiceDesc {
    CameraInfo  camera_info[MAX_CAMERA];
    int         camera_count;
};

struct CameraCallbackDesc {
    void* context = nullptr;
    camera_callback_t callback = nullptr;
    CameraSourceType source;
};

static CameraServiceDesc  g_cameraServiceDesc;
static CameraCallbackDesc g_cameraCallbackDesc;

using namespace std::literals;

static int getTokenValue(const char* params, const char* name,
                         char* value, int val_size) {
    const char* val_end;
    int len = strlen(name);
    const char* par_end = params + strlen(params);
    const char* par_start = strstr(params, name);

    /* Search for 'name=' */
    while (par_start != nullptr) {
        /* Make sure that we're within the parameters buffer. */
        if ((par_end - par_start) < len) {
            par_start = nullptr;
            break;
        }
        /* Make sure that par_start starts at the beginning of <name>, and only
         * then check for '=' value separator. */
        if ((par_start == params || (*(par_start - 1) == ' ')) &&
                par_start[len] == '=') {
            break;
        }
        par_start = strstr(par_start + 1, name);
    }
    if (par_start == nullptr) {
        return -1;
    }

    par_start += len + 1;
    val_end = strchr(par_start, ' ');
    if (val_end == nullptr) {
        val_end = par_start + strlen(par_start);
    }
    len = val_end - par_start;

    if ((len + 1) <= val_size) {
        memcpy(value, par_start, len);
        value[len] = '\0';
        return 0;
    } else {
        return len + 1;
    }
}

static int getTokenValueAlloc(const char* params,
                              const char* name, char** value) {
    char tmp;
    int res;

    const int val_size = getTokenValue(params, name, &tmp, 0);
    if (val_size < 0) {
        *value = nullptr;
        return val_size;
    }

    *value = (char*)malloc(val_size);
    if (*value == nullptr) {
        return -2;
    }
    res = getTokenValue(params, name, *value, val_size);
    if (res) {
        free(*value);
        *value = nullptr;
    }

    return res;
}

static int getTokenValueInt(const char* params,
                            const char* name, int* value) {
    char val_str[64];   // Should be enough for all numeric values.
    if (!getTokenValue(params, name, val_str, sizeof(val_str))) {
        errno = 0;
        *value = strtoi(val_str, (char**)nullptr, 10);
        if (errno) {
            return -2;
        } else {
            return 0;
        }
    } else {
        return -1;
    }
}

static std::pair<std::string_view, std::string_view> _parse_query(const std::string_view request) {
    const size_t separator = request.find(' ');
    if (separator != request.npos) {
        return {request.substr(0, separator), request.substr(separator + 1)};
    } else if (request.empty()) {
        return {{}, {}};
    } else if (request.back() == 0) {
        return {request.substr(0, request.size() - 1), {}};
    } else {
        return {request, {}};
    }
}

static std::pair<std::string_view, std::string_view> _parse_query(const void* data, size_t size) {
    return _parse_query(std::string_view(static_cast<const char*>(data), size));
}

static std::string cameraInfoToString(const CameraInfo& ci) {
    if (ci.frame_sizes_num == 0) {
        return {};
    }

    char buf[256];
    int len = ::snprintf(buf, sizeof(buf), "name=%s channel=%u pix=%u "
                         "dir=%s framedims=%ux%u", ci.device_name,
                         ci.inp_channel, ci.pixel_format, ci.direction,
                         ci.frame_sizes[0].width, ci.frame_sizes[0].height);

    std::string info(buf, len);

    for (int i = 1; i < ci.frame_sizes_num; ++i) {
        len = ::snprintf(buf, sizeof(buf), ",%ux%u",
                         ci.frame_sizes[i].width,
                         ci.frame_sizes[i].height);
        info.append(buf, len);
    }

    info.push_back('\n');

    return info;
}

static CameraInfo* cameraInfoGetByDisplayName(const char* disp_name,
                                              CameraInfo* arr,
                                              int num) {
    int n;
    for (n = 0; n < num; n++) {
        if (!arr[n].in_use && arr[n].display_name != nullptr &&
            !strcmp(arr[n].display_name, disp_name)) {
            return &arr[n];
        }
    }
    return nullptr;
}

static int cameraClientGetMaxResolution(const CameraInfo* info,
                                        int* width, int* height) {
    if (!info || !width || !height) {
        return -1;
    }

    const CameraFrameDim *maxDim = info->frame_sizes;
    if (!maxDim) {
        return -1;
    }
    const int frameSizesNum = info->frame_sizes_num;
    if (frameSizesNum <= 0) {
        return -1;
    }

    using MaxSoFar = std::pair<const CameraFrameDim *, int>;

    maxDim = std::accumulate(maxDim + 1, maxDim + frameSizesNum,
        std::make_pair(maxDim, maxDim->width * maxDim->height),
        [](const MaxSoFar maxSoFar, const CameraFrameDim &dim) -> MaxSoFar {
            const int area = dim.width * dim.height;
            return (area > maxSoFar.second) ? std::make_pair(&dim, area) : maxSoFar;
        }).first;

    *width = maxDim->width;
    *height = maxDim->height;
    return 0;
}

static void virtualscenecameraSetup(CameraServiceDesc* csd) {
    static const CameraInfoVtbl vtbl = {
        .open = &camera_virtualscene_open,
        .start_capturing = &camera_virtualscene_start_capturing,
        .read_frame = &camera_virtualscene_read_frame,
        .stop_capturing = &camera_virtualscene_stop_capturing,
        .close = &camera_virtualscene_close,
        .camera_source = kVirtualScene,
    };

    static const CameraFrameDim kEmulateDims[] = {
            {640, 480},
            {352, 288},
            {320, 240},
            {176, 144},
            {1280, 720},
            {1280, 960}};

    CameraInfo& ci = csd->camera_info[csd->camera_count];
    ci.frame_sizes = (CameraFrameDim*)malloc(sizeof(kEmulateDims));
    if (!ci.frame_sizes) {
        return;
    }

    ci.vtbl = &vtbl;

    memcpy(ci.frame_sizes, kEmulateDims, sizeof(kEmulateDims));
    ci.frame_sizes_num = sizeof(kEmulateDims) / sizeof(*kEmulateDims);

    ci.display_name = ASTRDUP("virtualscene");
    ci.device_name = ASTRDUP("virtualscene");

    ci.inp_channel = 0;
    ci.pixel_format = camera_virtualscene_preferred_format();
    ci.direction = ASTRDUP("back");
    ci.in_use = 0;

    csd->camera_count++;
}

static void videoplaybackcameraSetup(CameraServiceDesc* csd, const char* dir) {
    static const CameraInfoVtbl vtbl = {
        .open = &camera_videoplayback_open,
        .start_capturing = &camera_videoplayback_start_capturing,
        .read_frame = &camera_videoplayback_read_frame,
        .stop_capturing = &camera_videoplayback_stop_capturing,
        .close = &camera_videoplayback_close,
        .camera_source = kVideoPlayback,
    };

    static const CameraFrameDim kEmulateDims[] = {
            {640, 480},
            {352, 288},
            {320, 240},
            {176, 144},
            {1280, 720},
            {1280, 960}};

    CameraInfo& ci = csd->camera_info[csd->camera_count];
    ci.frame_sizes = (CameraFrameDim*)malloc(sizeof(kEmulateDims));
    if (!ci.frame_sizes) {
        return;
    }

    ci.vtbl = &vtbl;

    memcpy(ci.frame_sizes, kEmulateDims, sizeof(kEmulateDims));
    ci.frame_sizes_num = sizeof(kEmulateDims) / sizeof(*kEmulateDims);

    ci.display_name = ASTRDUP("videoplayback");
    ci.device_name = ASTRDUP("videoplayback");

    ci.inp_channel = 0;
    ci.pixel_format = camera_videoplayback_preferred_format();
    ci.direction = ASTRDUP(dir);
    ci.in_use = 0;

    csd->camera_count++;
}

static void webcamSetup(CameraServiceDesc* csd,
                        const char* disp_name,
                        const char* dir,
                        CameraInfo* webcams,
                        int webcams_cnt) {
    static const CameraInfoVtbl vtbl = {
        .open = &camera_device_open,
        .start_capturing = &camera_device_start_capturing,
        .read_frame = &camera_device_read_frame,
        .stop_capturing = &camera_device_stop_capturing,
        .close = &camera_device_close,
        .camera_source = kWebcam,
    };

    CameraInfo* srcCi =
        cameraInfoGetByDisplayName(disp_name, webcams, webcams_cnt);
    if (srcCi == nullptr) {
        W("Camera name '%s' is not found in the list of connected cameras.\n"
          "Use '-webcam-list' emulator option to obtain the list of connected camera names.\n",
          disp_name);
        return;
    }

    CameraInfo& dstCi = csd->camera_info[csd->camera_count];

    srcCi->in_use = 1;
    camera_info_copy(&dstCi, srcCi);
    dstCi.vtbl = &vtbl;

    if (dstCi.direction != nullptr) {
        free(dstCi.direction);
    }

    dstCi.direction = ASTRDUP(dir);

    csd->camera_count++;
}

static void cameraServiceInit(CameraServiceDesc* csd) {
    memset(csd->camera_info, 0, sizeof(csd->camera_info));
    csd->camera_count = 0;
    set_coarse_orientation_getter(
        (GetCoarseOrientation)android_sensors_get_coarse_orientation);

    if (androidHwConfig_hasVirtualSceneCamera(getConsoleAgents()->settings->hw())) {
        virtualscenecameraSetup(csd);
    }

    if (androidHwConfig_hasVideoPlaybackBackCamera(getConsoleAgents()->settings->hw())) {
        videoplaybackcameraSetup(csd, "back");
    }

    if (androidHwConfig_hasVideoPlaybackFrontCamera(getConsoleAgents()->settings->hw())) {
        videoplaybackcameraSetup(csd, "front");
    }

    /* Lets see if HW config uses emulated cameras. */
    if (!strncmp(getConsoleAgents()->settings->hw()->hw_camera_back, "webcam", 6) ||
        !strncmp(getConsoleAgents()->settings->hw()->hw_camera_front, "webcam", 6)) {
        int connected_cnt = 0;
        CameraInfo ci[MAX_CAMERA] = {};

        /* Enumerate web cameras connected to the host. */
        connected_cnt = camera_enumerate_devices(ci, MAX_CAMERA);
        if (connected_cnt <= 0) {
            /* Nothing is connected - nothing to emulate. */
            return;
        }

        /* Set up back camera emulation. */
        if (!strncmp(getConsoleAgents()->settings->hw()->hw_camera_back, "webcam", 6)) {
            webcamSetup(csd, getConsoleAgents()->settings->hw()->hw_camera_back, "back", ci,
                        connected_cnt);
        }

        /* Set up front camera emulation. */
        if (!strncmp(getConsoleAgents()->settings->hw()->hw_camera_front, "webcam", 6)) {
            webcamSetup(csd, getConsoleAgents()->settings->hw()->hw_camera_front, "front", ci,
                        connected_cnt);
        }

        int i;
        for (i = 0; i < connected_cnt; ++i) {
            camera_info_done(&ci[i]);
        }
    }
}

static void sendPayloadSize(QemudClient* qc, const size_t size) {
    char str[9];
    ::snprintf(str, sizeof(str), "%08zx", size);
    qemud_client_send(qc, reinterpret_cast<const uint8_t*>(str), 8);
}

static constexpr size_t kReplyPrefixSize = 3;
static constexpr uint8_t kOkReplyData[kReplyPrefixSize] = {'o', 'k', ':'};

static void qemuClientReply(QemudClient* qc, const bool okko,
                            const void* data,
                            const size_t dataSize) {
    static constexpr uint8_t kOkReply[kReplyPrefixSize] = {'o', 'k', 0};
    static constexpr uint8_t kKoReply[kReplyPrefixSize] = {'k', 'o', 0};
    static constexpr uint8_t kKoReplyData[kReplyPrefixSize] = {'k', 'o', ':'};

    const uint8_t* okkoStr = dataSize ?
        (okko ? kOkReplyData : kKoReplyData) :
        (okko ? kOkReply : kKoReply);

    sendPayloadSize(qc, kReplyPrefixSize + dataSize);
    qemud_client_send(qc, okkoStr, kReplyPrefixSize);
    if (dataSize) {
        qemud_client_send(qc, static_cast<const uint8_t*>(data),
                          dataSize);
    }
}

static void qemuClientReply(QemudClient* qc, const bool okko,
                            const std::string_view str = {}) {
    qemuClientReply(qc, okko, str.data(), str.size());
}

static void qemuClientReplyASCIZ(QemudClient* qc, const bool okko,
                                 const char* str) {
    qemuClientReply(qc, okko, str, ::strlen(str));
}

/********************************************************************************
 * Camera Factory API
 *******************************************************************************/

/* Handles 'list' query received from the Factory client.
 * Response to this query is a string that represents each connected camera in
 * this format: 'name=devname framedims=widh1xheight1,widh2xheight2,widhNxheightN\n'
 * Strings, representing each camera are separated with EOL symbol.
 * Param:
 *  csd, client - Factory serivice, and client.
 * Return:
 *  0 on success, or != 0 on failure.
 */
static int factoryClientListCameras(CameraServiceDesc* csd, QemudClient* client) {
    if (csd->camera_count == 0) {
        /* No cameras connected to the host. Reply with "\n" */
        qemuClientReply(client, true, "\n"sv);
        return 0;
    }

    std::string reply;
    for (int n = 0; n < csd->camera_count; n++) {
        reply += cameraInfoToString(csd->camera_info[n]);
    }

    qemuClientReply(client, true, reply);
    return 0;
}

/* Handles a message received from the emulated camera factory client.
 * Queries received here are represented as strings:
 *  'list' - Queries list of cameras connected to the host.
 * Param:
 *  opaque - Camera service descriptor.
 *  msg, msglen - Message received from the camera factory client.
 *  client - Camera factory client pipe.
 */
static void factoryClientRecv(void*         opaque,
                              uint8_t*      msg,
                              int           msglen,
                              QemudClient*  client) {
    using namespace std::literals;

    static constexpr std::string_view _query_list = "list"sv;

    CameraServiceDesc* csd = (CameraServiceDesc*)opaque;

    const auto [query_name, query_param] = _parse_query(msg, msglen);
    if (query_name.empty()) {
        qemuClientReply(client, false, "Invalid query format"sv);
        return;
    }

    if (query_name == _query_list) {
        factoryClientListCameras(csd, client);
    } else {
        qemuClientReply(client, false, "Unknown query name"sv);
    }
}

static void factoryClientClose(void*) {
}

struct CameraClient {
    CameraClient(CameraInfo* ci, uint32_t inp_channel1)
            : camera_info(ci)
            , inp_channel(inp_channel1) {
        ci->in_use = 1;
    }

    CameraInfo* const   camera_info;
    CameraDevice*       camera = nullptr;

    /* Buffer allocated for video frames.
     * Note that memory allocated for this buffer also contains preview
     * framebuffer and i420 staging framebuffer. */
    uint8_t*            video_frame = nullptr;
    /* Preview frame buffer.
     * This address points inside the 'video_frame' buffer. */
    uint8_t*            preview_frame = nullptr;
    /* Byte size of the videoframe buffer. */
    size_t              video_frame_size = 0;
    /* Byte size of the preview frame buffer. */
    size_t              preview_frame_size = 0;
    /* Staging framebuffer, used as an intermediate buffer for libyuv. */
    uint8_t*            staging_framebuffer = nullptr;
    /* Staging framebuffer size. */
    size_t              staging_framebuffer_size = 0;
    /* Input channel to use to connect to the camera. */
    const uint32_t      inp_channel = 0;
    /* Pixel format required by the guest. */
    uint32_t            pixel_format = 0;
    /* Frame width. */
    int                 width = 0;
    /* Frame height. */
    int                 height = 0;

    /* Queries being sent from the guest can be interrupted, resulting in the camera receiving
       the partial text of a query.  (This can be detected by the query not ending with a
       terminating 0 character.)  In that case, the partial command is stored in command_buffer,
       and command_buffer_offset records the length of the messages received so far, and thus where
       the next segment should be written.
       */
    char command_buffer[MAX_QUERY_MESSAGE_SIZE];
    int  command_buffer_offset = 0;
    uint64_t            frame_count = 0;
    bool                started = false;

    ~CameraClient() {
        if (camera != nullptr) {
            (camera_info->vtbl->close)(camera);
        }
        if (video_frame != nullptr) {
            free(video_frame);
        }
        camera_info->in_use = 0;
    };
};

static CameraClient* cameraClientCreate(CameraServiceDesc* csd, const char* param) {

    char* device_name = nullptr;
    if (getTokenValueAlloc(param, "name", &device_name)) {
        return nullptr;
    }

    int inp_channel;
    int res = getTokenValueInt(param, "inp_channel", &inp_channel);
    if (res != 0) {
        if (res == -1) {
            /* 'inp_channel' parameter has been ommited. Use default input
             * channel, which is zero. */
            inp_channel = 0;
        } else {
            ::free(device_name);
            return nullptr;
        }
    }

    CameraInfo* ci = std::find_if(csd->camera_info, &csd->camera_info[csd->camera_count],
                                  [device_name](const CameraInfo& ci){
                                        return ci.device_name &&
                                               !strcmp(ci.device_name, device_name);
                                  });
    if (ci == &csd->camera_info[csd->camera_count]) {
        ::free(device_name);
        return nullptr;
    }
    ::free(device_name);

    if (ci->in_use) {
        return nullptr;
    }

    return new CameraClient(ci, inp_channel);
}

static int64_t getTimestamp(void) {
    struct timeval t;
    gettimeofday(&t, nullptr);
    return int64_t(t.tv_sec) * 1000000L + t.tv_usec;
}

static void cameraSleep(const int64_t millisec) {
    int64_t toSleep = millisec * 1000L;
    const int64_t wakeAt = getTimestamp() + toSleep;

    while (toSleep > 0) {
        const lldiv_t parts = ::lldiv(toSleep, 1000000L);

        struct timeval interval = {
            .tv_sec = parts.quot,
            .tv_usec = parts.rem,
        };

        if ((select(0, nullptr, nullptr, nullptr, &interval) < 0)
                && (errno == EINTR)) {
            toSleep = wakeAt - getTimestamp();
        } else {
            break;
        }
    }
}

static void cameraClientQueryConnect(CameraClient* cc, QemudClient* qc, const char* param) {
    const CameraInfo& ci = *cc->camera_info;

    if (cc->camera != nullptr) {
        qemuClientReply(qc, true, "Camera is already connected"sv);
        return;
    }

    cc->camera = (ci.vtbl->open)(ci.device_name, cc->inp_channel);
    if (cc->camera == nullptr) {
        qemuClientReply(qc, false, "Unable to open camera device."sv);
        return;
    }

    if ((ci.vtbl->camera_source == g_cameraCallbackDesc.source) &&
            g_cameraCallbackDesc.callback) {
        g_cameraCallbackDesc.callback(g_cameraCallbackDesc.context, true);
    }
    qemuClientReply(qc, true);
}

static void cameraClientQueryDisconnect(CameraClient* cc, QemudClient* qc, const char* param) {
    const CameraInfo& ci = *cc->camera_info;

    if (cc->camera == nullptr) {
        qemuClientReply(qc, true, "Camera is not connected"sv);
        return;
    }

    if ((!V1 && cc->video_frame != nullptr) || (V1 && cc->started)) {
        qemuClientReply(qc, false, "Camera is not stopped"sv);
        return;
    }

    (ci.vtbl->close)(cc->camera);
    cc->camera = nullptr;

    qemuClientReply(qc, true);
}

static ClientStartResult cameraClientStart(CameraClient* cc,
                                           int width,
                                           int height,
                                           int pix_format) {
    const CameraInfo& ci = *cc->camera_info;

    camera_metrics_report_start_session(ci.vtbl->camera_source, ci.direction, width,
                                        height, pix_format);

    if ((!V1 && cc->video_frame != nullptr) || (V1 && cc->started)) {
        if (cc->pixel_format == (uint32_t)pix_format && cc->width == width &&
            cc->height == height) {
            return CLIENT_START_RESULT_ALREADY_STARTED;
        } else {
            return CLIENT_START_RESULT_PARAMETER_MISMATCH;
        }
    }

    cc->pixel_format = pix_format;
    cc->width = width;
    cc->height = height;
    cc->frame_count = 0;
    cc->staging_framebuffer = nullptr;
    cc->staging_framebuffer_size = 0;

    if (V1) {
        if (!has_converter(ci.pixel_format, cc->pixel_format)) {
            return CLIENT_START_RESULT_NO_PIXEL_CONVERSION;
        }
    } else {
        /* Make sure that pixel format is known, and calculate video/preview
         * framebuffer size along the lines. */
        if (!calculate_framebuffer_size(cc->pixel_format, cc->width, cc->height,
                                        &cc->video_frame_size)) {
            return CLIENT_START_RESULT_UNKNOWN_PIXEL_FORMAT;
        }

        /* TODO: At the moment camera framework in the emulator requires RGB32 pixel
         * format for preview window. So, we need to keep two framebuffers here: one
         * for the video, and another for the preview window. Watch out when this
         * changes (if changes). */
        cc->preview_frame_size = cc->width * cc->height * 4;

        /* Make sure that we have a converters between the original camera pixel
         * format and the one that the client expects. Also a converter must exist
         * for the preview window pixel format (RGB32) */
        if (!has_converter(ci.pixel_format, cc->pixel_format) ||
            !has_converter(ci.pixel_format, V4L2_PIX_FMT_RGB32)) {
            return CLIENT_START_RESULT_NO_PIXEL_CONVERSION;
        }

        /* Allocate buffer large enough to contain both, video and preview
         * framebuffers. */
        cc->video_frame =
                (uint8_t*)malloc(cc->video_frame_size + cc->preview_frame_size);
        if (cc->video_frame == nullptr) {
            return CLIENT_START_RESULT_OUT_OF_MEMORY;
        }

        /* Set framebuffer pointers. */
        cc->preview_frame = cc->video_frame + cc->video_frame_size;
    }

    if ((ci.vtbl->start_capturing)(cc->camera, ci.pixel_format,
                                   cc->width, cc->height)) {
        if (cc->video_frame) {
            free(cc->video_frame);
            cc->video_frame = nullptr;
        }
        return CLIENT_START_RESULT_FAILED;
    }

    if (V1) {
        cc->started = true;
    }

    return CLIENT_START_RESULT_SUCCESS;
}

static void cameraClientQueryStart(CameraClient* cc, QemudClient* qc, const char* param) {
    char* w;
    char dim[64];
    int width, height, pix_format;

    if (cc->camera == nullptr) {
        qemuClientReply(qc, false, "Camera is not connected"sv);
        return;
    }

    if (param == nullptr) {
        qemuClientReply(qc, false, "Missing parameters for the query"sv);
        return;
    }

    if (getTokenValue(param, "dim", dim, sizeof(dim))) {
        qemuClientReply(qc, false, "Invalid or missing 'dim' parameter"sv);
        return;
    }

    if (getTokenValueInt(param, "pix", &pix_format)) {
        qemuClientReply(qc, false, "Invalid or missing 'pix' parameter"sv);
        return;
    }

    w = strchr(dim, 'x');
    if (w == nullptr || w[1] == '\0') {
        qemuClientReply(qc, false, "Invalid 'dim' parameter");
        return;
    }
    *w = '\0'; w++;
    errno = 0;
    width = strtoi(dim, nullptr, 10);
    height = strtoi(w, nullptr, 10);
    if (errno) {
        qemuClientReply(qc, false, "Invalid 'dim' parameter");
        return;
    }

    ClientStartResult result =
            cameraClientStart(cc, width, height, pix_format);
    camera_metrics_report_start_result(result);

    if (result < 0) {
        camera_metrics_report_stop_session(0);
    }

    switch (result) {
    case CLIENT_START_RESULT_SUCCESS:
        qemuClientReply(qc, true);
        break;
    case CLIENT_START_RESULT_ALREADY_STARTED:
        qemuClientReply(qc, true, "Camera is already started"sv);
        break;
    case CLIENT_START_RESULT_PARAMETER_MISMATCH:
        qemuClientReply(qc, false, "Camera is already started with different capturing parameters"sv);
        break;
    case CLIENT_START_RESULT_UNKNOWN_PIXEL_FORMAT:
        qemuClientReply(qc, false, "Pixel format is unknown"sv);
        break;
    case CLIENT_START_RESULT_NO_PIXEL_CONVERSION:
        qemuClientReply(qc, false, "No conversion exist for the requested pixel format"sv);
        break;
    case CLIENT_START_RESULT_OUT_OF_MEMORY:
        qemuClientReply(qc, false, "Out of memory"sv);
        break;
    default:
        E("%s: Unexpected capture result '%d'", __func__, result);
        [[fallthrough]];
    case CLIENT_START_RESULT_FAILED:
        qemuClientReply(qc, false, "Cannot start the camera"sv);
        break;
    }
}

static void cameraClientQueryStartV1(CameraClient* cc, QemudClient* qc,
                                     const char* param) {
    const CameraInfo& ci = *cc->camera_info;

    char* w;
    char dim[64];
    int width, height, pix_format;

    if (cc->camera == nullptr) {
        qemuClientReply(qc, false, "Camera is not connected"sv);
        return;
    }

    if (param == nullptr) {
        if (cameraClientGetMaxResolution(&ci, &width, &height)) {
            qemuClientReply(qc, false, "Failed to get default resolution"sv);
            return;
        }
        pix_format = ci.pixel_format;
    } else {
        if (getTokenValue(param, "dim", dim, sizeof(dim))) {
            if (cameraClientGetMaxResolution(&ci, &width, &height)) {
                qemuClientReply(qc, false, "Failed to get default resolution"sv);
                return;
            }
        } else {
            w = strchr(dim, 'x');
            if (w == nullptr || w[1] == '\0') {
                qemuClientReply(qc, false, "Invalid 'dim' parameter"sv);
                return;
            }
            *w = '\0'; w++;
            errno = 0;
            width = strtoi(dim, nullptr, 10);
            height = strtoi(w, nullptr, 10);
            if (errno) {
                qemuClientReply(qc, false, "Invalid 'dim' parameter"sv);
                return;
            }
        }
        if (getTokenValueInt(param, "pix", &pix_format)) {
            pix_format = ci.pixel_format;
        }
    }

    ClientStartResult result =
            cameraClientStart(cc, width, height, pix_format);
    camera_metrics_report_start_result(result);

    if (result < 0) {
        camera_metrics_report_stop_session(0);
    }

    switch (result) {
    case CLIENT_START_RESULT_SUCCESS:
        qemuClientReply(qc, true);
        break;
    case CLIENT_START_RESULT_ALREADY_STARTED:
        qemuClientReply(qc, true, "Camera is already started"sv);
        break;
    case CLIENT_START_RESULT_PARAMETER_MISMATCH:
        qemuClientReply(qc, false, "Camera is already started with "
                                   "different capturing parameters"sv);
        break;
    case CLIENT_START_RESULT_UNKNOWN_PIXEL_FORMAT:
        qemuClientReply(qc, false, "Pixel format is unknown"sv);
        break;
    case CLIENT_START_RESULT_NO_PIXEL_CONVERSION:
        qemuClientReply(qc, false, "No conversion exist for the "
                                   "requested pixel format"sv);
        break;
    case CLIENT_START_RESULT_OUT_OF_MEMORY:
        qemuClientReply(qc, false, "Out of memory"sv);
        break;
    default:
        E("%s: Unexpected capture result '%d'", __func__, result);
        [[fallthrough]];
    case CLIENT_START_RESULT_FAILED:
        qemuClientReply(qc, false, "Cannot start the camera");
        break;
    }
}

static void cameraClientQueryStop(CameraClient* cc, QemudClient* qc, const char* param) {
    const CameraInfo& ci = *cc->camera_info;

    if ((!V1 && cc->video_frame == nullptr) || (V1 && !cc->started)) {
        qemuClientReply(qc, true, "Camera is not started"sv);
        return;
    }

    if ((ci.vtbl->stop_capturing)(cc->camera)) {
        qemuClientReply(qc, false, "Cannot stop camera device"sv);
        return;
    }

    if (cc->video_frame) {
        free(cc->video_frame);
        cc->video_frame = nullptr;
    }

    if (V1) {
        cc->started = false;
    }

    free(cc->staging_framebuffer);
    cc->staging_framebuffer = nullptr;

    camera_metrics_report_stop_session(cc->frame_count);

    if ((ci.vtbl->camera_source == g_cameraCallbackDesc.source) &&
            g_cameraCallbackDesc.callback) {
        g_cameraCallbackDesc.callback(g_cameraCallbackDesc.context, false);
    }

    qemuClientReply(qc, true);
}

static int readFrameImpl(CameraClient* cc, QemudClient* qc, ClientFrame* frame,
                         const float r_scale, const float g_scale, const float b_scale,
                         const float expComp) {
    const CameraInfo& ci = *cc->camera_info;
    const auto readFrame = ci.vtbl->read_frame;

    int retry = readFrame(cc->camera, frame,
                          r_scale, g_scale, b_scale,
                          expComp, ci.direction);
    if (!retry) {
        return 0;
    }

    const int64_t timeout = getTimestamp() + 2000000U;
    do {
        cameraSleep(10);

        retry = readFrame(cc->camera, frame,
                          r_scale, g_scale, b_scale,
                          expComp, ci.direction);
    } while ((retry > 0) && (getTimestamp() < timeout));

    if (retry > 0) {
        qemuClientReply(qc, false, "Unable to obtain video frame "
                                   "from the camera"sv);
    } else if (retry < 0) {
        qemuClientReplyASCIZ(qc, false, strerror(errno));
    }

    return retry;
}

static void cameraClientQueryFrame(CameraClient* cc, QemudClient* qc, const char* param) {
    const CameraInfo& ci = *cc->camera_info;

    int video_size = 0;
    int preview_size = 0;
    ClientFrameBuffer fbs[2];
    int fbs_num = 0;
    float r_scale = 1.0f, g_scale = 1.0f, b_scale = 1.0f, exp_comp = 1.0f;
    char tmp[256];
    int send_frame_time = 0;
    ClientFrame frame = {};

    if (cc->video_frame == nullptr) {
        qemuClientReply(qc, false, "Invalid 'video' parameter"sv);
        return;
    }

    if (getTokenValueInt(param, "video", &video_size) ||
        getTokenValueInt(param, "preview", &preview_size)) {
        qemuClientReply(qc, false, "Invalid or missing 'video', or 'preview' parameter"sv);
        return;
    }

    if (!getTokenValue(param, "whiteb", tmp, sizeof(tmp))) {
        if (sscanf(tmp, "%g,%g,%g", &r_scale, &g_scale, &b_scale) != 3) {
            r_scale = g_scale = b_scale = 1.0f;
        }
    }

    if (!getTokenValue(param, "expcomp", tmp, sizeof(tmp))) {
        if (sscanf(tmp, "%g", &exp_comp) != 1) {
            exp_comp = 1.0f;
        }
    }

    if (getTokenValueInt(param, "time", &send_frame_time) < 0) {
        send_frame_time = 0;
    }

    if ((video_size != 0 && cc->video_frame_size != (size_t)video_size) ||
        (preview_size != 0 && cc->preview_frame_size != (size_t)preview_size)) {
        qemuClientReply(qc, false, "Frame size mismatch"sv);
        return;
    }


    if (video_size) {
        fbs[fbs_num].pixel_format = cc->pixel_format;
        fbs[fbs_num].width = cc->width;
        fbs[fbs_num].height = cc->height;
        fbs[fbs_num].framebuffer = cc->video_frame;
        fbs_num++;
    }
    if (preview_size) {
        /* TODO: Watch out for preview format changes! */
        fbs[fbs_num].pixel_format = V4L2_PIX_FMT_RGB32;
        fbs[fbs_num].width = cc->width;
        fbs[fbs_num].height = cc->height;
        fbs[fbs_num].framebuffer = cc->preview_frame;
        fbs_num++;
    }

    frame.framebuffers_count = fbs_num;
    frame.framebuffers = fbs;
    frame.staging_framebuffer = &cc->staging_framebuffer;
    frame.staging_framebuffer_size = &cc->staging_framebuffer_size;
    frame.frame_time =
            looper_nowNsWithClock(looper_getForThread(), LOOPER_CLOCK_VIRTUAL);

    if (readFrameImpl(cc, qc, &frame, r_scale, g_scale, b_scale, exp_comp)) {
        return;
    }

    const size_t payload_size = kReplyPrefixSize +
        (send_frame_time ? sizeof(int64_t) : 0) + video_size + preview_size;

    if (payload_size > kReplyPrefixSize) {
        sendPayloadSize(qc, payload_size);
        qemud_client_send(qc, kOkReplyData, kReplyPrefixSize);

        if (video_size) {
            qemud_client_send(qc, cc->video_frame, video_size);
        }
        if (preview_size) {
            qemud_client_send(qc, cc->preview_frame, preview_size);
        }
        if (send_frame_time) {
            const int64_t adjusted_time = frame.frame_time +
                    android_sensors_get_time_offset();

            qemud_client_send(qc, (const uint8_t*) &adjusted_time,
                              sizeof(adjusted_time));
        }
    } else {
        qemuClientReply(qc, true);
    }

    ++cc->frame_count;
}

static void cameraClientQueryFrameV1(CameraClient* cc, QemudClient* qc,
                                     const char* param) {
    const CameraInfo& ci = *cc->camera_info;

    char* w;
    int format, width, height;
    uint64_t offset;
    float r_scale = 1.0f, g_scale = 1.0f, b_scale = 1.0f, exp_comp = 1.0f;
    char tmp[256];
    int send_frame_time = 0;
    ClientFrame frame = {};

    if (!cc->started) {
        qemuClientReply(qc, "Camera is not started");
        return;
    }

    if (getTokenValue(param, "dim", tmp, sizeof(tmp))) {
        qemuClientReply(qc, false, "Invalid or missing 'dim' parameter"sv);
        return;
    } else {
        w = strchr(tmp, 'x');
        if (w == nullptr || w[1] == '\0') {
            qemuClientReply(qc, false, "Invalid 'dim' parameter"sv);
            return;
        }
        *w = '\0'; w++;
        errno = 0;
        width = strtoi(tmp, nullptr, 10);
        height = strtoi(w, nullptr, 10);
        if (errno) {
            qemuClientReply(qc, false, "Invalid 'dim' parameter"sv);
            return;
        }
    }

    if (getTokenValueInt(param, "pix", &format)) {
        qemuClientReply(qc, false, "Invalid or missing 'pix' parameter"sv);
        return;
    }

    if (getTokenValue(param, "offset", tmp, sizeof(tmp))) {
        qemuClientReply(qc, false, "Invalid or missing 'offset' parameter"sv);
        return;
    } else {
        if (sscanf(tmp, "%" PRIu64, &offset) != 1) {
            qemuClientReply(qc, false, "not a decimal number for 'offset'"sv);
            return;
        }
    }

    if (!getTokenValue(param, "whiteb", tmp, sizeof(tmp))) {
        if (sscanf(tmp, "%g,%g,%g", &r_scale, &g_scale, &b_scale) != 3) {
            r_scale = g_scale = b_scale = 1.0f;
        }
    }

    if (!getTokenValue(param, "expcomp", tmp, sizeof(tmp))) {
        if (sscanf(tmp, "%g", &exp_comp) != 1) {
            exp_comp = 1.0f;
        }
    }

    if (getTokenValueInt(param, "time", &send_frame_time) < 0) {
        send_frame_time = 0;
    }


    const ClientFrameBuffer fb = {
        .pixel_format = format,
        .width = width,
        .height = height,
        .framebuffer =
            get_address_space_device_control_ops()->get_host_ptr(
                get_address_space_device_hw_funcs()->getPhysAddrStart() +
                    offset),
    };

    frame.framebuffers_count = 1;
    frame.framebuffers = &fb;
    frame.staging_framebuffer = &cc->staging_framebuffer;
    frame.staging_framebuffer_size = &cc->staging_framebuffer_size;
    frame.frame_time =
            looper_nowNsWithClock(looper_getForThread(), LOOPER_CLOCK_VIRTUAL);

    if (readFrameImpl(cc, qc, &frame, r_scale, g_scale, b_scale, exp_comp)) {
        return;
    }

    ++cc->frame_count;

    if (send_frame_time) {
        static const uint8_t kOkColon[] = { 'o', 'k', ':' };
        const int64_t adjusted_time = frame.frame_time +
                android_sensors_get_time_offset();

        qemuClientReply(qc, true, &adjusted_time, sizeof(adjusted_time));
    } else {
        qemuClientReply(qc, true);
    }
}

static void cameraClientHandleEvent(CameraClient*  cc,
                                    uint8_t*       msg,
                                    int            msglen,
                                    QemudClient*   client) {
    using namespace std::literals;

    if (msglen <= 0) {
        return;
    }

    if (cc->command_buffer_offset + msglen >= MAX_QUERY_MESSAGE_SIZE) {
        cc->command_buffer_offset = 0;
        qemuClientReply(client, false, "query too long"sv);
        return;
    }
    memcpy(cc->command_buffer + cc->command_buffer_offset, msg, msglen);
    cc->command_buffer_offset += msglen;

    if (cc->command_buffer[cc->command_buffer_offset - 1] != '\0') {
        return;
    }

    static constexpr std::string_view _query_connect    = "connect"sv;
    static constexpr std::string_view _query_disconnect = "disconnect"sv;
    static constexpr std::string_view _query_start      = "start"sv;
    static constexpr std::string_view _query_stop       = "stop"sv;
    static constexpr std::string_view _query_frame      = "frame"sv;

    const auto [query_name, query_param] =
        _parse_query(cc->command_buffer, cc->command_buffer_offset);
    cc->command_buffer_offset = 0;

    if (query_name.empty()) {
        qemuClientReply(client, false, "Invalid query"sv);
        return;
    }

    if (query_name == _query_frame) {
        if (V1) {
            cameraClientQueryFrameV1(cc, client, query_param.data());
        } else {
            cameraClientQueryFrame(cc, client, query_param.data());
        }
    } else if (query_name == _query_connect) {
        cameraClientQueryConnect(cc, client, query_param.data());
    } else if (query_name == _query_disconnect) {
        cameraClientQueryDisconnect(cc, client, query_param.data());
    } else if (query_name == _query_start) {
        if (V1) {
            cameraClientQueryStartV1(cc, client, query_param.data());
        } else {
            cameraClientQueryStart(cc, client, query_param.data());
        }
    } else if (query_name == _query_stop) {
        cameraClientQueryStop(cc, client, query_param.data());
    } else {
        qemuClientReply(client, false, "Unknown query"sv);
    }
}

static void cameraClientRecv(void*         opaque,
                             uint8_t*      msg,
                             int           msglen,
                             QemudClient*  client) {
    CameraClient* cc = (CameraClient*)opaque;
    cameraClientHandleEvent(cc, msg, msglen, client);
}

/* Emulated camera client has been disconnected from the service. */
static void cameraClientClose(void* opaque) {
    CameraClient* cc = static_cast<CameraClient*>(opaque);


    delete cc;
}

/* Saves the state of the camera client.
 *
 * This simply saves whether the camera is currently connected, so that it can
 * reconnect on load.
 */
static void cameraClientSave(Stream* f, QemudClient* client, void* opaque) {
    CameraClient* cc = (CameraClient*)opaque;

    stream_put_be32(f, cc->camera != nullptr ? 1 : 0);
    if (V1) {
        stream_put_be32(f, cc->started ? 1: 0);
    } else {
        stream_put_be32(f, cc->video_frame != nullptr ? 1 : 0);
    }
    if ((!V1 && cc->video_frame != nullptr) || (V1 && cc->started)) {
        stream_put_be32(f, cc->pixel_format);
        stream_put_be32(f, cc->width);
        stream_put_be32(f, cc->height);
    }
}

static int cameraClientLoad(Stream* f, QemudClient* client, void* opaque) {
    CameraClient* cc = (CameraClient*)opaque;
    const CameraInfo& ci = *cc->camera_info;

    int is_camera_connected = stream_get_be32(f);
    if (is_camera_connected && cc->camera == nullptr) {
        cc->camera = (ci.vtbl->open)(ci.device_name, cc->inp_channel);
        if (cc->camera == nullptr) {
            return -EIO;
        }
    }

    // Try to stop the camera if it is already started in order to avoid a frame
    // size or format mismatch.
    if ((!V1 &&cc->video_frame != nullptr) || (V1 && cc->started)) {
        if ((ci.vtbl->stop_capturing)(cc->camera) == 0) {
            if (cc->video_frame) {
                free(cc->video_frame);
                cc->video_frame = nullptr;
            }
            if (V1) {
                cc->started = false;
            }
        } else {
            return -EIO;
        }
    }

    int is_camera_started = stream_get_be32(f);
    if (is_camera_started) {
        int pixel_format = stream_get_be32(f);
        int width = stream_get_be32(f);
        int height = stream_get_be32(f);

        ClientStartResult result = cameraClientStart(cc, width, height,
                                                     pixel_format);
        camera_metrics_report_start_result(result);
        if (result < 0) {
            camera_metrics_report_stop_session(0);
            return -EIO;
        }
    }
    if ((ci.vtbl->camera_source == g_cameraCallbackDesc.source) &&
            g_cameraCallbackDesc.callback)
        g_cameraCallbackDesc.callback(g_cameraCallbackDesc.context, is_camera_started);

    return 0;
}

/********************************************************************************
 * Camera service API
 *******************************************************************************/

/* Connects a client to the camera service.
 * There are two classes of the client that can connect to the service:
 *  - Camera factory that is interested only in listing camera devices attached
 *    to the host.
 *  - Camera device emulators that attach to the actual camera devices.
 * The distinction between these two classes is made by looking at extra
 * parameters passed in client_param variable. If it's nullptr, or empty, the
 * client connects to a camera factory. Otherwise, parameters describe the
 * camera device the client wants to connect to.
 */
static QemudClient* cameraServiceConnect(void*          opaque,
                                         QemudService*  serv,
                                         int            channel,
                                         const char*    client_param) {
    QemudClient*  client = nullptr;
    CameraServiceDesc* csd = (CameraServiceDesc*)opaque;

    if (client_param == nullptr || *client_param == '\0') {
        /* This is an emulated camera factory client. */
        client = qemud_client_new(serv, channel, client_param, csd,
                                  factoryClientRecv, factoryClientClose,
                                  nullptr, nullptr);
    } else {
        /* This is an emulated camera client. */
        CameraClient* cc = cameraClientCreate(csd, client_param);
        if (cc != nullptr) {
            client = qemud_client_new(serv, channel, client_param, cc,
                                      cameraClientRecv, cameraClientClose,
                                      cameraClientSave, cameraClientLoad);
        }
    }

    return client;
}

void register_camera_status_change_callback(camera_callback_t cb,
                                            void* ctx,
                                            CameraSourceType src) {
    g_cameraCallbackDesc.callback = cb;
    g_cameraCallbackDesc.context = ctx;
    g_cameraCallbackDesc.source = src;
}

void android_camera_service_init(void) {
    static constexpr char kServiceCamera[] = "camera";

    static int _inited = 0;

    if (!_inited) {
        cameraServiceInit(&g_cameraServiceDesc);

        QemudService* serv = qemud_service_register(kServiceCamera, 0,
                &g_cameraServiceDesc, &cameraServiceConnect,
                nullptr, nullptr);
        if (serv == nullptr) {
            derror("%s: Could not register '%s' service",
                    __func__, kServiceCamera);
            return;
        }

        if (strcmp(getConsoleAgents()->settings->hw()->hw_camera_back, "emulated") &&
                strcmp(getConsoleAgents()->settings->hw()->hw_camera_front, "emulated")) {
            /* Fake camera is not used for camera emulation. */
            boot_property_add_qemu_sf_fake_camera("none");
        } else {
            if(!strcmp(getConsoleAgents()->settings->hw()->hw_camera_back, "emulated") &&
                    !strcmp(getConsoleAgents()->settings->hw()->hw_camera_front, "emulated")) {
                /* Fake camera is used for both, front and back camera emulation. */
                boot_property_add_qemu_sf_fake_camera("both");
            } else if (!strcmp(getConsoleAgents()->settings->hw()->hw_camera_back, "emulated")) {
                boot_property_add_qemu_sf_fake_camera("back");
            } else {
                boot_property_add_qemu_sf_fake_camera("front");
            }
        }
    }
}

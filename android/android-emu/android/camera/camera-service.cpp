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
#include <charconv>
#include <memory>
#include <optional>
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

/* Camera service version 1 */
#define V1 ((avdInfo_getApiLevel(getConsoleAgents()->settings->avdInfo()) > 29) && \
            !feature_is_enabled(kFeature_Minigbm))

static constexpr size_t MAX_CAMERA = 8;

struct CameraServiceDesc {
    CameraInfo  camera_info[MAX_CAMERA];
    int         camera_count;
};

struct CameraCallbackDesc {
    void set(camera_callback_t cb, void* ctx, CameraSourceType src) {
        callback = cb;
        context = ctx;
        source = src;
    }

    void operator()(CameraSourceType src, bool value) const {
        if (callback && (source == src)) {
            callback(context, value);
        }
    }

    camera_callback_t callback = nullptr;
    void* context = nullptr;
    CameraSourceType source = {};
};

struct WhiteBalance {
    float red, green, blue;
};

using namespace std::literals;

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

static std::optional<std::string_view> getTokenValueStr(const std::string_view params,
                                                        const std::string_view name) {
    const size_t paramsSize = params.size();
    const size_t nameSize = name.size();

    size_t i = 0;
    while ((i = params.find(name, i)) != params.npos) {
        const size_t nameEnd = i + nameSize;
        if (nameEnd >= paramsSize) {
            return std::nullopt;
        } else if (params[nameEnd] == '=') {
            const size_t valueBegin = nameEnd + 1;
            const size_t valueEnd = params.find(' ', valueBegin);

            return (valueEnd == params.npos) ?
                params.substr(valueBegin) :
                params.substr(valueBegin, valueEnd - valueBegin);
        } else {
            ++i;
        }
    }

    return std::nullopt;
}

template <class T, class P> bool getParamValue(T& destination,
                                               const std::string_view params,
                                               const std::string_view name,
                                               const P valueParser) {
    const std::optional<std::string_view> maybeValueStr =
        getTokenValueStr(params, name);
    if (!maybeValueStr) {
        return false;
    }

    auto maybeValue = valueParser(maybeValueStr.value());
    if (!maybeValue) {
        return false;
    }

    destination = std::move(maybeValue.value());
    return true;
}

template <class T, class P> bool getParamValueV(T& destination,
                                                const std::string_view params,
                                                const std::string_view name,
                                                const P valueParser,
                                                const T& defValue) {
    const std::optional<std::string_view> maybeValueStr =
        getTokenValueStr(params, name);
    if (!maybeValueStr) {
        destination = defValue;
        return true;
    }

    auto maybeValue = valueParser(maybeValueStr.value());
    if (!maybeValue) {
        return false;
    }

    destination = std::move(maybeValue.value());
    return true;
}

template <class T, class P, class F> bool getParamValueF(T& destination,
                                                         const std::string_view params,
                                                         const std::string_view name,
                                                         const P valueParser,
                                                         const F& getDefValue) {
    const std::optional<std::string_view> maybeValueStr =
        getTokenValueStr(params, name);
    if (!maybeValueStr) {
        destination = getDefValue();
        return true;
    }

    auto maybeValue = valueParser(maybeValueStr.value());
    if (!maybeValue) {
        return false;
    }

    destination = std::move(maybeValue.value());
    return true;
}

static std::optional<bool> parseBool(const std::string_view str) {
    if (str == "0"sv) {
        return false;
    } else if (str == "1"sv) {
        return true;
    } else {
        return std::nullopt;
    }
}

template <class T> static std::optional<T> parseInt(const std::string_view str,
                                                    const unsigned base = 10) {
    if (str.empty()) {
        return std::nullopt;
    }

    T value;
    const auto [ptr, ec] =
        std::from_chars(&*str.begin(), &*str.end(), value, base);
    if ((ec != std::errc()) || (ptr != &*str.end())) {
        return std::nullopt;
    }

    return value;
}

template <class T, class F>
static std::optional<T> parseIntValidated(const std::string_view str,
                                          const F& isValid,
                                          const unsigned base = 10) {
    const auto maybeValue = parseInt<T>(str, base);
    if (maybeValue && isValid(maybeValue.value())) {
        return maybeValue;
    } else {
        return std::nullopt;
    }
}

template <class F> static std::optional<float>parseFloatValidated(const std::string_view str,
                                                                  const F& isValid) {
    if (str.empty()) {
        return std::nullopt;
    }

    char* end = const_cast<char*>(&*str.end());
    const float value = std::strtof(&*str.begin(), &end);
    if (end != &*str.end()) {
        return std::nullopt;
    }

    if (isValid(value)) {
        return value;
    } else {
        return std::nullopt;
    }
}

static std::optional<size_t> parseSize(const std::string_view str) {
    return parseInt<size_t>(str);
}

static std::optional<uint64_t> parseOffset(const std::string_view str) {
    return parseInt<uint64_t>(str);
}

static std::optional<uint32_t> parsePix(const std::string_view str) {
    return parseInt<uint32_t>(str);
}

static std::optional<uint32_t> parseInpChannel(const std::string_view str) {
    return parseInt<uint32_t>(str);
}

static std::optional<std::pair<uint32_t, uint32_t>>
parseDim(const std::string_view str) {
    static constexpr auto parseDim1 =
        [](const std::string_view str){
            static constexpr auto isValidDimValue =
                [](const uint32_t value){ return value > 0; };

            return parseIntValidated<uint32_t>(str, isValidDimValue);
        };

    const size_t xpos = str.find('x');
    if (xpos == str.npos) {
        return std::nullopt;
    }

    std::optional<uint32_t> maybeWidth = parseDim1(str.substr(0, xpos));
    if (!maybeWidth) {
        return std::nullopt;
    }

    std::optional<uint32_t> maybeHeight = parseDim1(str.substr(xpos + 1));
    if (!maybeHeight) {
        return std::nullopt;
    }

    return std::make_pair(maybeWidth.value(), maybeHeight.value());
}

static std::optional<WhiteBalance> parseWhiteBalance(const std::string_view str) {
    static constexpr auto parseWhiteBalance1 =
        [](const std::string_view str){
            static constexpr auto isValidWhiteBalanceValue =
                [](const float value){
                    return value > 0.0f;
                };

            return parseFloatValidated(str, isValidWhiteBalanceValue);
        };

    WhiteBalance whiteBalance;
    std::optional<float> maybeValue;

    const size_t comma1 = str.find(',');
    if (comma1 == str.npos) {
        return std::nullopt;
    }

    maybeValue = parseWhiteBalance1(str.substr(0, comma1));
    if (!maybeValue) {
        return std::nullopt;
    }

    whiteBalance.red = maybeValue.value();

    const size_t comma2 = str.find(',', comma1 + 1);
    if (comma2 == str.npos) {
        return std::nullopt;
    }

    maybeValue = parseWhiteBalance1(str.substr(comma1 + 1, comma2 - comma1 - 1));
    if (!maybeValue) {
        return std::nullopt;
    }

    whiteBalance.green = maybeValue.value();

    maybeValue = parseWhiteBalance1(str.substr(comma2 + 1));
    if (!maybeValue) {
        return std::nullopt;
    }

    whiteBalance.blue = maybeValue.value();
    return whiteBalance;
}

static std::optional<float> parseExpComp(const std::string_view str) {
    return parseFloatValidated(str, [](const float value){ return value > 0.0f; });
}

static std::pair<std::string_view, std::string_view>
parseQuery(const std::string_view query) {
    const size_t separator = query.find(' ');
    if (separator != query.npos) {
        return {query.substr(0, separator), query.substr(separator + 1)};
    } else {
        return {query, {}};
    }
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
    for (int n = 0; n < num; n++) {
        if (!arr[n].in_use && arr[n].display_name &&
            !strcmp(arr[n].display_name, disp_name)) {
            return &arr[n];
        }
    }
    return nullptr;
}

static std::pair<uint32_t, uint32_t>
cameraClientGetMaxResolution(const CameraInfo& info) {
    const CameraFrameDim *maxDim = info.frame_sizes;
    if (!maxDim) {
        return {0, 0};
    }
    const int frameSizesNum = info.frame_sizes_num;
    if (frameSizesNum <= 0) {
        return {0, 0};
    }

    using MaxSoFar = std::pair<const CameraFrameDim *, int>;

    maxDim = std::accumulate(maxDim + 1, maxDim + frameSizesNum,
        std::make_pair(maxDim, maxDim->width * maxDim->height),
        [](const MaxSoFar maxSoFar, const CameraFrameDim &dim) -> MaxSoFar {
            const int area = dim.width * dim.height;
            return (area > maxSoFar.second) ? std::make_pair(&dim, area) : maxSoFar;
        }).first;

    return {maxDim->width, maxDim->height};
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
    if (!srcCi) {
        dwarning("Camera name '%s' is not found in the list of connected cameras.\n"
                "Use '-webcam-list' emulator option to obtain the list of connected "
                "camera names.\n", disp_name);
        return;
    }

    CameraInfo& dstCi = csd->camera_info[csd->camera_count];

    srcCi->in_use = 1;
    camera_info_copy(&dstCi, srcCi);
    dstCi.vtbl = &vtbl;

    if (dstCi.direction) {
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
static void factoryClientListCameras(const CameraServiceDesc& csd, QemudClient* client) {
    if (!csd.camera_count) {
        /* No cameras connected to the host. Reply with "\n" */
        qemuClientReply(client, true, "\n"sv);
    }

    std::string reply;
    for (int i = 0; i < csd.camera_count; i++) {
        reply += cameraInfoToString(csd.camera_info[i]);
    }

    qemuClientReply(client, true, reply);
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
                              const int     msglen,
                              QemudClient*  client) {
    static constexpr std::string_view kQueryList = "list"sv;

    if (msglen <= 1) {
        return;
    }

    const auto [queryName, queryParams] =
        parseQuery(std::string_view(reinterpret_cast<const char*>(msg),
                                    msglen - 1));

    if (queryName == kQueryList) {
        factoryClientListCameras(*static_cast<CameraServiceDesc*>(opaque),
                                 client);
    } else if (queryName.empty()) {
        qemuClientReply(client, false, "Empty query"sv);
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

    /* to parse quesries that arrive in parts */
    std::vector<char>   queryBuffer;

    /* Buffer allocated for video frames.
     * Note that memory allocated for this buffer also contains preview
     * framebuffer and i420 staging framebuffer. */
    uint8_t*            video_frame = nullptr;
    /* Preview frame buffer.
     * This address points inside the 'video_frame' buffer. */
    uint8_t*            preview_frame = nullptr;
    /* Staging framebuffer, used as an intermediate buffer for libyuv. */
    uint8_t*            staging_framebuffer = nullptr;

    /* Byte size of the videoframe buffer. */
    size_t              video_frame_size = 0;
    /* Byte size of the preview frame buffer. */
    size_t              preview_frame_size = 0;
    /* Staging framebuffer size. */
    size_t              staging_framebuffer_size = 0;

    /* Total number of frames rendered, used for metrics. */
    uint64_t            frame_count = 0;

    /* Input channel to use to connect to the camera. */
    const uint32_t      inp_channel = 0;
    /* Pixel format required by the guest. */
    uint32_t            pixel_format = 0;
    /* Frame width. */
    uint32_t            width = 0;
    /* Frame height. */
    uint32_t            height = 0;

    bool                started = false;

    ~CameraClient() {
        if (camera) {
            (camera_info->vtbl->close)(camera);
        }

        ::free(video_frame);
        ::free(staging_framebuffer);

        camera_info->in_use = 0;
    };
};

CameraCallbackDesc g_cameraCallbackDesc;

static CameraClient* cameraClientCreate(CameraServiceDesc& csd,
                                        const std::string_view params) {
    static constexpr std::string_view kParamName       = "name"sv;
    static constexpr std::string_view kParamInpChannel = "inp_channel"sv;

    std::optional<std::string_view> maybeDeviceName =
        getTokenValueStr(params, kParamName);
    if (!maybeDeviceName) {
        dwarning("Missing the '%s' parameter.", kParamName);
        return nullptr;
    }

    const std::string_view deviceName = std::move(maybeDeviceName.value());
    CameraInfo* ci = std::find_if(
            csd.camera_info, &csd.camera_info[csd.camera_count],
            [&deviceName](const CameraInfo& ci){
                return ci.device_name &&
                       !strncmp(ci.device_name, deviceName.data(), deviceName.size()) &&
                       !ci.device_name[deviceName.size()];
            });

    if (ci == &csd.camera_info[csd.camera_count]) {
        dwarning("Camera name '%s' is not found in the list of "
                 "connected cameras.", deviceName);
        return nullptr;
    }
    if (ci->in_use) {
        dwarning("Can't open the '%s' camera, it is still in use.", deviceName);
        return nullptr;
    }
    if (!ci->frame_sizes_num || !ci->frame_sizes) {
        dwarning("Camera '%s' has no supported frame dimensions.", deviceName);
        return nullptr;
    }

    uint32_t inpChannel;
    if (!getParamValueV(inpChannel, params, kParamInpChannel, parseInpChannel, 0U)) {
        dwarning("Invalid '%s' parameter.", kParamInpChannel);
        return nullptr;
    }

    return new CameraClient(ci, inpChannel);
}

static void cameraClientQueryConnect(CameraClient* cc, QemudClient* qc) {
    if (cc->camera) {
        qemuClientReply(qc, true, "Camera is already connected"sv);
        return;
    }

    const CameraInfo& ci = *cc->camera_info;
    cc->camera = (ci.vtbl->open)(ci.device_name, cc->inp_channel);
    if (!cc->camera) {
        qemuClientReply(qc, false, "Unable to open camera device."sv);
        return;
    }

    qemuClientReply(qc, true);
}

static void cameraClientQueryDisconnect(CameraClient* cc, QemudClient* qc) {
    if (!cc->camera) {
        qemuClientReply(qc, true, "Camera is not connected"sv);
        return;
    }

    if ((!V1 && cc->video_frame) || (V1 && cc->started)) {
        qemuClientReply(qc, false, "Camera is not stopped"sv);
        return;
    }

    (cc->camera_info->vtbl->close)(cc->camera);
    cc->camera = nullptr;

    qemuClientReply(qc, true);
}

static ClientStartResult cameraClientStart(CameraClient* cc,
                                           const uint32_t width, const uint32_t height,
                                           const uint32_t pixFormat) {
    const CameraInfo& ci = *cc->camera_info;

    if ((!V1 && cc->video_frame) || (V1 && cc->started)) {
        if (cc->pixel_format == pixFormat && cc->width == width &&
            cc->height == height) {
            return CLIENT_START_RESULT_ALREADY_STARTED;
        } else {
            return CLIENT_START_RESULT_PARAMETER_MISMATCH;
        }
    }

    cc->pixel_format = pixFormat;
    cc->width = width;
    cc->height = height;
    cc->frame_count = 0;

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
        if (!cc->video_frame) {
            return CLIENT_START_RESULT_OUT_OF_MEMORY;
        }

        /* Set framebuffer pointers. */
        cc->preview_frame = cc->video_frame + cc->video_frame_size;
    }

    if ((ci.vtbl->start_capturing)(cc->camera, ci.pixel_format, cc->width, cc->height)) {
        if (cc->video_frame) {
            free(cc->video_frame);
            cc->video_frame = nullptr;
        }
        return CLIENT_START_RESULT_FAILED;
    }

    if (V1) {
        cc->started = true;
    }

    camera_metrics_report_start_session(ci.vtbl->camera_source,
                                        ci.direction, width,
                                        height, pixFormat);

    g_cameraCallbackDesc(ci.vtbl->camera_source, true);

    return CLIENT_START_RESULT_SUCCESS;
}

static constexpr std::string_view kParamDim             = "dim"sv;
static constexpr std::string_view kParamPix             = "pix"sv;
static constexpr std::string_view kParamOffset          = "offset"sv;
static constexpr std::string_view kParamPreviewSize     = "preview"sv;
static constexpr std::string_view kParamVideoSize       = "video"sv;
static constexpr std::string_view kParamSendFrameTime   = "time"sv;
static constexpr std::string_view kParamWhiteBalance    = "whiteb"sv;
static constexpr std::string_view kParamExpComp         = "expcomp"sv;

static constexpr WhiteBalance kDefaultWhiteBalance = { 1.0f, 1.0f, 1.0f };

static void cameraClientQueryStart(CameraClient* cc, QemudClient* qc,
                                   const std::string_view params,
                                   const bool allowDefaults) {
    if (!cc->camera) {
        qemuClientReply(qc, false, "Camera is not connected"sv);
        return;
    }

    const CameraInfo& ci = *cc->camera_info;

    uint32_t pixFormat;
    if (allowDefaults) {
        if (!getParamValueF(pixFormat, params, kParamPix, parsePix,
                            [&ci](){
                                return ci.pixel_format;
                            })) {
badPix:     qemuClientReply(qc, false, "Invalid or missing 'pix' parameter"sv);
            return;
        }

    } else {
        if (!getParamValue(pixFormat, params, kParamPix, parsePix)) {
            goto badPix;
        }
    }

    uint32_t width;
    uint32_t height;
    auto rect = std::tie(width, height);
    if (allowDefaults) {
        if (!getParamValueF(rect, params, kParamDim, parseDim,
                            [&ci](){
                                return cameraClientGetMaxResolution(ci);
                            })) {
badDim:     qemuClientReply(qc, false, "Invalid or missing 'dim' parameter"sv);
            return;
        } else if (!width || !height) {
            goto badDim;
        }
    } else {
        if (!getParamValue(rect, params, kParamDim, parseDim)) {
            goto badDim;
        }
    }

    const ClientStartResult result =
            cameraClientStart(cc, width, height, pixFormat);

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
        derror("%s: Unexpected capture result '%d'", __func__, result);
        [[fallthrough]];
    case CLIENT_START_RESULT_FAILED:
        qemuClientReply(qc, false, "Cannot start the camera");
        break;
    }
}

static void cameraClientQueryStop(CameraClient* cc, QemudClient* qc) {
    if ((!V1 && !cc->video_frame) || (V1 && !cc->started)) {
        qemuClientReply(qc, true, "Camera is not started"sv);
        return;
    }

    const CameraInfo& ci = *cc->camera_info;

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

    camera_metrics_report_stop_session(cc->frame_count);

    g_cameraCallbackDesc(ci.vtbl->camera_source, false);
    qemuClientReply(qc, true);
}

static int readFrameImpl(CameraClient* cc, QemudClient* qc, ClientFrame* frame,
                         const WhiteBalance& whiteBalance, const float expComp) {
    const CameraInfo& ci = *cc->camera_info;
    const auto readFrame = ci.vtbl->read_frame;

    int retry = readFrame(cc->camera, frame,
                          whiteBalance.red, whiteBalance.green, whiteBalance.blue,
                          expComp, ci.direction);
    if (!retry) {
        return 0;
    }

    const int64_t timeout = getTimestamp() + 2000000U;
    do {
        cameraSleep(10);

        retry = readFrame(cc->camera, frame,
                          whiteBalance.red, whiteBalance.green, whiteBalance.blue,
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

static void cameraClientQueryFrame(CameraClient* cc, QemudClient* qc,
                                   const std::string_view params) {
    constexpr size_t kZero = 0;

    if (!cc->camera) {
        return;
    }

    size_t videoSize;
    if (!getParamValueV(videoSize, params, kParamVideoSize,
                        parseSize, kZero)) {
        qemuClientReply(qc, false, "Invalid 'video' parameter"sv);
        return;
    }

    size_t previewSize;
    if (!getParamValueV(previewSize, params, kParamPreviewSize,
                        parseSize, kZero)) {
        qemuClientReply(qc, false, "Invalid 'preview' parameter"sv);
        return;
    }

    if (!videoSize && !previewSize) {
        qemuClientReply(qc, false, "Nothing requested"sv);
        return;
    }

    if ((videoSize && (cc->video_frame_size != videoSize)) ||
            (previewSize && (cc->preview_frame_size != previewSize))) {
        qemuClientReply(qc, false, "Frame size mismatch"sv);
        return;
    }

    bool sendFrameTime;
    if (!getParamValueV(sendFrameTime, params, kParamSendFrameTime,
                        parseBool, false)) {
        qemuClientReply(qc, false, "Invalid 'time' parameter"sv);
        return;
    }

    WhiteBalance whiteBalance;
    if (!getParamValueV(whiteBalance, params, kParamWhiteBalance,
                        parseWhiteBalance, kDefaultWhiteBalance)) {
        qemuClientReply(qc, false, "Invalid 'whiteb' parameter"sv);
        return;
    }

    float expComp;
    if (!getParamValueV(expComp, params, kParamExpComp,
                        parseExpComp, 1.0f)) {
        qemuClientReply(qc, false, "Invalid 'expcomp' parameter"sv);
        return;
    }

    ClientFrameBuffer fbs[2];
    int fbs_num = 0;
    if (videoSize) {
        fbs[fbs_num].pixel_format = cc->pixel_format;
        fbs[fbs_num].width = cc->width;
        fbs[fbs_num].height = cc->height;
        fbs[fbs_num].framebuffer = cc->video_frame;
        fbs_num++;
    }
    if (previewSize) {
        /* TODO: Watch out for preview format changes! */
        fbs[fbs_num].pixel_format = V4L2_PIX_FMT_RGB32;
        fbs[fbs_num].width = cc->width;
        fbs[fbs_num].height = cc->height;
        fbs[fbs_num].framebuffer = cc->preview_frame;
        fbs_num++;
    }

    ClientFrame frame = {
        .framebuffers_count = fbs_num,
        .framebuffers = fbs,
        .staging_framebuffer = &cc->staging_framebuffer,
        .staging_framebuffer_size = &cc->staging_framebuffer_size,
        .frame_time =
                looper_nowNsWithClock(looper_getForThread(),
                                      LOOPER_CLOCK_VIRTUAL),
    };

    if (readFrameImpl(cc, qc, &frame, whiteBalance, expComp)) {
        return;
    }

    const size_t payloadSize = kReplyPrefixSize +
        (sendFrameTime ? sizeof(int64_t) : 0) + videoSize + previewSize;

    if (payloadSize > kReplyPrefixSize) {
        sendPayloadSize(qc, payloadSize);
        qemud_client_send(qc, kOkReplyData, kReplyPrefixSize);

        if (videoSize) {
            qemud_client_send(qc, cc->video_frame, videoSize);
        }
        if (previewSize) {
            qemud_client_send(qc, cc->preview_frame, previewSize);
        }
        if (sendFrameTime) {
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
                                     const std::string_view params) {
    if (!cc->started) {
        qemuClientReply(qc, "Camera is not started");
        return;
    }

    uint32_t width;
    uint32_t height;
    auto rect = std::tie(width, height);
    if (!getParamValue(rect, params, kParamDim, parseDim)) {
        qemuClientReply(qc, false, "Invalid or missing 'dim' parameter"sv);
        return;
    }

    uint32_t pixFormat;
    if (!getParamValue(pixFormat, params, kParamPix, parsePix)) {
        qemuClientReply(qc, false, "Invalid or missing 'pix' parameter"sv);
        return;
    }

    uint64_t offset;
    if (!getParamValue(offset, params, kParamOffset, parseOffset)) {
        qemuClientReply(qc, false, "Invalid or missing 'offset' parameter"sv);
        return;
    }

    bool sendFrameTime;
    if (!getParamValueV(sendFrameTime, params, kParamSendFrameTime,
                        parseBool, false)) {
        qemuClientReply(qc, false, "Invalid 'time' parameter"sv);
        return;
    }

    WhiteBalance whiteBalance;
    if (!getParamValueV(whiteBalance, params, kParamWhiteBalance,
                        parseWhiteBalance, kDefaultWhiteBalance)) {
        qemuClientReply(qc, false, "Invalid or missing 'whiteb' parameter"sv);
        return;
    }

    float expComp;
    if (!getParamValueV(expComp, params, kParamExpComp,
                        parseExpComp, 1.0f)) {
        qemuClientReply(qc, false, "Invalid or missing 'expcomp' parameter"sv);
        return;
    }

    const ClientFrameBuffer fb = {
        .pixel_format = pixFormat,
        .width = width,
        .height = height,
        .framebuffer =
            get_address_space_device_control_ops()->get_host_ptr(
                get_address_space_device_hw_funcs()->getPhysAddrStart() +
                    offset),
    };

    ClientFrame frame = {
        .framebuffers_count = 1,
        .framebuffers = &fb,
        .staging_framebuffer = &cc->staging_framebuffer,
        .staging_framebuffer_size = &cc->staging_framebuffer_size,
        .frame_time =
                looper_nowNsWithClock(looper_getForThread(),
                                      LOOPER_CLOCK_VIRTUAL),
    };

    if (readFrameImpl(cc, qc, &frame, whiteBalance, expComp)) {
        return;
    }

    if (sendFrameTime) {
        const int64_t adjusted_time = frame.frame_time +
                android_sensors_get_time_offset();
        qemuClientReply(qc, true, &adjusted_time, sizeof(adjusted_time));
    } else {
        qemuClientReply(qc, true);
    }

    ++cc->frame_count;
}

static void cameraClientHandleQuery(CameraClient*  cc,
                                    QemudClient*   client,
                                    const std::string_view query) {
    static constexpr std::string_view kQueryConnect         = "connect"sv;
    static constexpr std::string_view kQueryStart           = "start"sv;
    static constexpr std::string_view kQueryFrame           = "frame"sv;
    static constexpr std::string_view kQueryStop            = "stop"sv;
    static constexpr std::string_view kQueryDisconnect      = "disconnect"sv;

    const auto [queryName, queryParams] = parseQuery(query);

    if (queryName == kQueryFrame) {
        if (V1) {
            cameraClientQueryFrameV1(cc, client, queryParams);
        } else {
            cameraClientQueryFrame(cc, client, queryParams);
        }
    } else if (queryName == kQueryConnect) {
        cameraClientQueryConnect(cc, client);
    } else if (queryName == kQueryStart) {
        if (V1) {
            cameraClientQueryStart(cc, client, queryParams, true);
        } else {
            cameraClientQueryStart(cc, client, queryParams, false);
        }
    } else if (queryName == kQueryStop) {
        cameraClientQueryStop(cc, client);
    } else if (queryName == kQueryDisconnect) {
        cameraClientQueryDisconnect(cc, client);
    } else if (queryName.empty()) {
        qemuClientReply(client, false, "Empty query"sv);
    } else {
        qemuClientReply(client, false, "Unknown query"sv);
    }
}

static void cameraClientHandleEvent(CameraClient*  cc,
                                    QemudClient*   client,
                                    const std::string_view msg) {
    constexpr char kQuerySeparator = 0;

    auto& queryBuffer = cc->queryBuffer;

    const size_t separator = msg.find(kQuerySeparator);
    if (separator == msg.npos) {
        queryBuffer.insert(cc->queryBuffer.end(),
                           msg.begin(), msg.end());
        return;
    }

    if (queryBuffer.empty()) {
        cameraClientHandleQuery(cc, client, msg.substr(0, separator));
    } else {
        const size_t querySize = queryBuffer.size() + separator;

        queryBuffer.insert(queryBuffer.end(),
                           msg.begin(), msg.begin() + separator);

        cameraClientHandleQuery(cc, client,
                                std::string_view(queryBuffer.data(),
                                                 querySize));
    }

    queryBuffer.assign(msg.begin() + separator + 1, msg.end());
}

static void cameraClientRecv(void*         opaque,
                             uint8_t*      msg,
                             const int     msglen,
                             QemudClient*  client) {
    if (msglen <= 0) {
        return;
    }

    cameraClientHandleEvent(static_cast<CameraClient*>(opaque), client,
                            std::string_view(reinterpret_cast<const char*>(msg),
                                             msglen));
}

/* Emulated camera client has been disconnected from the service. */
static void cameraClientClose(void* opaque) {
    delete static_cast<CameraClient*>(opaque);
}

/* Saves the state of the camera client.
 *
 * This simply saves whether the camera is currently connected, so that it can
 * reconnect on load.
 */
static void cameraClientSave(Stream* f, QemudClient* client, void* opaque) {
    CameraClient* cc = static_cast<CameraClient*>(opaque);

    stream_put_be32(f, cc->camera ? 1 : 0);
    if (V1) {
        stream_put_be32(f, cc->started ? 1: 0);
    } else {
        stream_put_be32(f, cc->video_frame ? 1 : 0);
    }
    if ((!V1 && cc->video_frame) || (V1 && cc->started)) {
        stream_put_be32(f, cc->pixel_format);
        stream_put_be32(f, cc->width);
        stream_put_be32(f, cc->height);
    }
}

static int cameraClientLoad(Stream* f, QemudClient* client, void* opaque) {
    CameraClient* cc = static_cast<CameraClient*>(opaque);
    const CameraInfo& ci = *cc->camera_info;

    int is_camera_connected = stream_get_be32(f);
    if (is_camera_connected && !cc->camera) {
        cc->camera = (ci.vtbl->open)(ci.device_name, cc->inp_channel);
        if (!cc->camera) {
            return -EIO;
        }
    }

    // Try to stop the camera if it is already started in order to avoid a frame
    // size or format mismatch.
    if ((!V1 && cc->video_frame) || (V1 && cc->started)) {
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

    g_cameraCallbackDesc(ci.vtbl->camera_source, is_camera_started);
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
                                         const int      channel,
                                         const char*    clientParams) {
    CameraServiceDesc* csd = static_cast<CameraServiceDesc*>(opaque);

    if (!clientParams || !*clientParams) {
        /* This is an emulated camera factory client. */
        return qemud_client_new(serv, channel, clientParams, csd,
                                &factoryClientRecv, &factoryClientClose,
                                nullptr, nullptr);
    } else {
        /* This is an emulated camera client. */
        CameraClient* cc = cameraClientCreate(*csd, clientParams);
        if (cc) {
            return qemud_client_new(serv, channel, clientParams, cc,
                                    &cameraClientRecv, &cameraClientClose,
                                    &cameraClientSave, &cameraClientLoad);
        }
    }

    return nullptr;
}

// TODO: remove this function and g_cameraCallbackDesc and call
// the callback from camera_XYZ_(start|stop)_capturing.
void register_camera_status_change_callback(camera_callback_t cb,
                                            void* ctx,
                                            CameraSourceType src) {
    g_cameraCallbackDesc.set(cb, ctx, src);
}

void android_camera_service_init(void) {
    static constexpr char kServiceCamera[] = "camera";
    static CameraServiceDesc  s_cameraServiceDesc;
    static bool s_inited = false;

    if (!s_inited) {
        cameraServiceInit(&s_cameraServiceDesc);
        s_inited = true;

        QemudService* serv = qemud_service_register(kServiceCamera, 0,
                &s_cameraServiceDesc, &cameraServiceConnect,
                nullptr, nullptr);
        if (!serv) {
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

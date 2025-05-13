/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "android/camera/camera-videofile.h"

#include <cstring>
#include <memory>
#include <optional>
#include <tuple>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}  // extern "C"

#include "android/utils/debug.h"

namespace {
struct AVFormatContextDeleter {
    void operator()(AVFormatContext* p) const {
        ::avformat_close_input(&p);
    }
};

using AVFormatContextPtr = std::unique_ptr<AVFormatContext, AVFormatContextDeleter>;

struct AVCodecContextDeleter {
    void operator()(AVCodecContext* p) const {
        ::avcodec_close(p);
        ::avcodec_free_context(&p);
    }
};

using AVCodecContextPtr = std::unique_ptr<AVCodecContext, AVCodecContextDeleter>;

struct AVFrameDeleter {
    void operator()(AVFrame* p) const {
        ::av_frame_free(&p);
    }
};

using AVFramePtr = std::unique_ptr<AVFrame, AVFrameDeleter>;

struct SwsContextDeleter {
    void operator()(SwsContext* p) const {
        ::sws_freeContext(p);
    }
};

using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;

struct VideoFile {
    AVFormatContextPtr formatCtx;
    AVCodecContextPtr codecCtx;
    unsigned videoStreamIndex;
};

int getVideoStreamIndex(const AVFormatContext& fmtctx) {
    for (unsigned i = 0; i < fmtctx.nb_streams; ++i) {
        if (fmtctx.streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            return i;
        }
    }

    return -1;
}

std::optional<VideoFile> openVideoFile(const char* filename) {
    int err;

    AVFormatContext* fmtCtxWeak = nullptr;
    err = avformat_open_input(&fmtCtxWeak, filename, nullptr, nullptr);
    if (err < 0) {
        derror("Could not open the '%s' file.", filename);
        return std::nullopt;
    }
    AVFormatContextPtr fmtCtx(fmtCtxWeak);

    err = avformat_find_stream_info(fmtCtx.get(), nullptr);
    if (err < 0) {
        derror("avformat_find_stream_info failed with %d for '%s'", err, filename);
        return std::nullopt;
    }

    const int videoStreamIndex = getVideoStreamIndex(*fmtCtx);
    if (videoStreamIndex < 0) {
        derror("Can't find the video stream in '%s'", filename);
        return std::nullopt;
    }

    const AVCodecParameters *codecParams = fmtCtx->streams[videoStreamIndex]->codecpar;
    const AVCodec *codec = ::avcodec_find_decoder(codecParams->codec_id);
    if (!codec) {
        derror("Unsupported codec_id=%d in '%s'", codecParams->codec_id, filename);
        return std::nullopt;
    }

    AVCodecContext *codecCtxWeak = ::avcodec_alloc_context3(codec);
    if (!codecCtxWeak) {
        derror("avcodec_alloc_context3 failed for codec_id=%d for '%s'",
               codecParams->codec_id, filename);
        return std::nullopt;
    }

    err = ::avcodec_parameters_to_context(codecCtxWeak, codecParams);
    if (err < 0) {
        derror("avcodec_parameters_to_context failed with %d for '%s'", err, filename);
        ::avcodec_free_context(&codecCtxWeak);
        return std::nullopt;
    }

    err = avcodec_open2(codecCtxWeak, codec, nullptr);
    if (err < 0) {
        derror("Could not open the codec (id=%d) for '%s'", err, filename);
        ::avcodec_free_context(&codecCtxWeak);
        return std::nullopt;
    }

    AVCodecContextPtr codecCtx(codecCtxWeak);

    VideoFile videoFile;

    videoFile.formatCtx = std::move(fmtCtx);
    videoFile.codecCtx = std::move(codecCtx);
    videoFile.videoStreamIndex = videoStreamIndex;

    return videoFile;
}

struct VideofileCameraDevice {
    VideofileCameraDevice(VideoFile videoFile) : mVideoFile(std::move(videoFile)) {
        mHeader.opaque = this;
    }

    static CameraDevice* open(const char* args, int) {
        std::optional<VideoFile> maybeVideofile = openVideoFile(args);
        if (maybeVideofile) {
            return &(new VideofileCameraDevice(std::move(maybeVideofile.value())))->mHeader;
        } else {
            return nullptr;
        }
    }

    static int startCapturingStatic(CameraDevice* cd, uint32_t /*pixelFormat*/,
                                    int /*width*/, int /*height*/) {
        return myselfFrom(cd)->startCapturing();
    }

    static int readFrameStatic(CameraDevice* cd, ClientFrame* frame,
                               float rScale, float gScale, float bScale, float expComp,
                               const char* direction) {
        return myselfFrom(cd)->readFrame(*frame, rScale, gScale, bScale, expComp, direction);
    }

    static int stopCapturingStatic(CameraDevice* cd) {
        return myselfFrom(cd)->stopCapturing();
    }

    static void closeStatic(CameraDevice* cd) {
        delete myselfFrom(cd);
    }

private:
    int startCapturing() {
        const int err = ::av_seek_frame(mVideoFile.formatCtx.get(), -1, 0, AVSEEK_FLAG_BACKWARD);
        if (err >= 0) {
            mConverterCache.clear();
            mFrameCache.reset(::av_frame_alloc());
            return mFrameCache ? 0 : -1;
        } else {
            derror("%s:%d av_seek_frame: err=%d", __func__, __LINE__, err);
            return err;
        }
    }

    int readFrame(ClientFrame& cframe,
                  const float rScale, const float gScale, const float bScale,
                  const float expComp, const char* direction) {
        if (const AVFrame* avFrame = decodeNextFrame()) {
            const bool backFacing = !strcmp(direction, "back");

            for (uint32_t i = 0; i < cframe.framebuffers_count; ++i) {
                if (const int err = fillCFB(cframe.framebuffers[i], *avFrame, backFacing)) {
                    return err;
                }
            }

            return 0;
        } else {
            derror("Could not decode a frame");
            return -1;
        }
    }

    int stopCapturing() {
        mFrameCache.reset();
        mConverterCache.clear();
        return 0;
    }

    struct FrameBufferInfo {
        uint8_t* planes[3];
        int strides[3];
        AVPixelFormat format;
    };

    static FrameBufferInfo getFrameBufferInfo(const ClientFrameBuffer& cfb) {
        const size_t w = cfb.width;
        const size_t h = cfb.height;

        FrameBufferInfo fbi = {};
        fbi.planes[0] = static_cast<uint8_t*>(cfb.framebuffer);

        switch (cfb.pixel_format) {
        case V4L2_PIX_FMT_RGB32:
            fbi.strides[0] = w * 4U;
            fbi.format = AV_PIX_FMT_RGBA;
            break;

        case V4L2_PIX_FMT_YUV420:
            fbi.planes[1] = fbi.planes[0] + (w * h);
            fbi.planes[2] = fbi.planes[1] + (w * h) / 4;
            fbi.strides[0] = w;
            fbi.strides[1] = w / 2;
            fbi.strides[2] = w / 2;
            fbi.format = AV_PIX_FMT_YUV420P;
            break;

        case V4L2_PIX_FMT_NV12:
            fbi.planes[1] = fbi.planes[0] + (w * h);
            fbi.strides[0] = w;
            fbi.strides[1] = w;
            fbi.format = AV_PIX_FMT_NV12;
            break;

        default:
            fbi.format = AV_PIX_FMT_NONE;
            break;
        }

        return fbi;
    }

    int fillCFB(const ClientFrameBuffer& cfb, const AVFrame& avFrame,
                const bool backFacing) {
        const FrameBufferInfo fbi = getFrameBufferInfo(cfb);
        if (fbi.format == AV_PIX_FMT_NONE) {
            const uint32_t fmt = cfb.pixel_format;
            derror("Unexpected format: %c%c%c%c",
                (fmt & 0xFF), ((fmt >> 8) & 0xFF),
                ((fmt >> 16) & 0xFF), ((fmt >> 24) & 0xFF));
            return -1;
        }

        SwsContext* swsCtx = getSwsContext(
                avFrame.width, avFrame.height, static_cast<AVPixelFormat>(avFrame.format),
                cfb.width, cfb.height, fbi.format);
        if (swsCtx) {
            ::sws_scale(swsCtx, avFrame.data, avFrame.linesize, 0, avFrame.height,
                        fbi.planes, fbi.strides);
            return 0;
        } else {
            derror("Could not allocate SwsContext for src={ %dx%d, fmt=%d }, "
                   "dst={ %dx%d, fmt=%d }",
                   avFrame.width, avFrame.height, avFrame.format,
                   cfb.width, cfb.height, fbi.format);
            return -1;
        }
    }

    const AVFrame* decodeNextFrame() {
        AVPacket packet;
        av_init_packet(&packet);
        packet.data = nullptr;
        packet.size = 0;

        for (int n = 100; n > 0; --n) {
            int err = ::av_read_frame(mVideoFile.formatCtx.get(), &packet);
            if (err < 0) {
                if (err == AVERROR_EOF) {
                    err = ::av_seek_frame(mVideoFile.formatCtx.get(), -1, 0, AVSEEK_FLAG_BACKWARD);
                    if (err >= 0) {
                        continue;
                    } else {
                        derror("%s:%d av_seek_frame: err=%d", __func__, __LINE__, err);
                        return nullptr;
                    }
                } else if (err == AVERROR(EAGAIN)) {
                    continue;
                } else {
                    derror("%s:%d av_read_frame: err=%d", __func__, __LINE__, err);
                    continue;
                }
            }

            if (packet.stream_index != mVideoFile.videoStreamIndex) {
                ::av_packet_unref(&packet);
                continue;
            }

            err = ::avcodec_send_packet(mVideoFile.codecCtx.get(), &packet);
            if (err < 0) {
                derror("%s:%d avcodec_send_packet: err=%d", __func__, __LINE__, err);
                ::av_packet_unref(&packet);
                continue;
            }

            err = ::avcodec_receive_frame(mVideoFile.codecCtx.get(), mFrameCache.get());
            ::av_packet_unref(&packet);
            if (err >= 0) {
                return mFrameCache.get();
            } else if (err != AVERROR(EAGAIN)) {
                derror("%s:%d avcodec_receive_frame: err=%d", __func__, __LINE__, err);
            }
        }

        return nullptr;
    }

    struct ConversionKey {
        int srcWidth;
        int srcHeight;
        AVPixelFormat srcFmt;
        int dstWidth;
        int dstHeight;
        AVPixelFormat dstFmt;

        bool operator==(const ConversionKey& rhs) const {
            return (srcWidth == rhs.srcWidth) &&
                   (srcHeight == rhs.srcHeight) &&
                   (srcFmt == rhs.srcFmt) &&
                   (dstWidth == rhs.dstWidth) &&
                   (dstHeight == rhs.dstHeight) &&
                   (dstFmt == rhs.dstFmt);
        }
    };

    SwsContext* getSwsContext(const int srcWidth, const int srcHeight, const AVPixelFormat srcFmt,
                              const int dstWidth, const int dstHeight, const AVPixelFormat dstFmt) {
        ConversionKey conv = {
            .srcWidth = srcWidth,
            .srcHeight = srcHeight,
            .srcFmt = srcFmt,
            .dstWidth = dstWidth,
            .dstHeight = dstHeight,
            .dstFmt = dstFmt,
        };

        const auto i = std::find_if(mConverterCache.begin(), mConverterCache.end(),
                                    [&conv](const std::pair<ConversionKey, SwsContextPtr>& kv) {
                                        return conv == kv.first;
                                    });
        if (i != mConverterCache.end()) {
            return i->second.get();
        }

        SwsContext* ctx = ::sws_getContext(srcWidth, srcHeight, srcFmt,
                                           dstWidth, dstHeight, dstFmt,
                                           SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if (ctx) {
            mConverterCache.push_back({std::move(conv), SwsContextPtr(ctx)});
        }

        return ctx;
    }

    static VideofileCameraDevice* myselfFrom(CameraDevice* c) {
        return static_cast<VideofileCameraDevice*>(c->opaque);
    }

    CameraDevice mHeader;
    VideoFile mVideoFile;
    AVFramePtr mFrameCache;
    std::vector<std::pair<ConversionKey, SwsContextPtr>> mConverterCache;
};

}  // namespace

int camera_videofile_init_CameraInfo(CameraInfo* ci, const char* direction,
                                     const char* args) {
    static const CameraInfoVtbl vtbl = {
        .open = &VideofileCameraDevice::open,
        .start_capturing = &VideofileCameraDevice::startCapturingStatic,
        .read_frame = &VideofileCameraDevice::readFrameStatic,
        .stop_capturing = &VideofileCameraDevice::stopCapturingStatic,
        .close = &VideofileCameraDevice::closeStatic,
        .camera_source = kVideofile,
    };

    static const CameraFrameDim kDims[] = {
        {640, 480},
        {352, 288},
        {320, 240},
        {176, 144},
        {1280, 720},
        {1280, 960},
    };

    ci->frame_sizes = static_cast<CameraFrameDim*>(::malloc(sizeof(kDims)));
    memcpy(ci->frame_sizes, kDims, sizeof(kDims));
    ci->frame_sizes_num = sizeof(kDims) / sizeof(kDims[0]);
    ci->vtbl = &vtbl;
    ci->display_name = ::strdup(args);
    ci->device_name = ::strdup(args);
    ci->inp_channel = 0;
    ci->pixel_format = V4L2_PIX_FMT_RGB32;
    ci->direction = ::strdup(direction);
    ci->in_use = 0;

    return 0;
}

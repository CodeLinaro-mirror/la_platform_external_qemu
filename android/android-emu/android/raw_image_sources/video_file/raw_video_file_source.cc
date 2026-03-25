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

#include "android/raw_image_sources/video_file/raw_video_file_source.h"

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "android/base/system/System.h"
#include "android/raw_image_sources/raw_image_source.h"

#include "android/camera/camera-common.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <tuple>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}  // extern "C"

#include "android/utils/debug.h"

namespace {

int getVideoStreamIndex(const AVFormatContext& fmtctx) {
    for (unsigned i = 0; i < fmtctx.nb_streams; ++i) {
        if (fmtctx.streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            return i;
        }
    }

    return -1;
}

int getVideoStreamRotation(const AVFormatContext& fmtctx, int streamIndex) {
    int display_matrix_size;
    uint8_t* display_matrix = av_stream_get_side_data(
            fmtctx.streams[streamIndex], AV_PKT_DATA_DISPLAYMATRIX,
            &display_matrix_size);

    double raw_rotation = 0;

    // Handle rotations in the modern display matrix way
    if (display_matrix && display_matrix_size >= 9 * sizeof(int32_t)) {
        // Currently ignoring the possibilities of flips
        raw_rotation = av_display_rotation_get(
                reinterpret_cast<int32_t*>(display_matrix));

        // av_display_rotation_get returns in [-180.0, 180.0], or NaN
        if (std::isnan(raw_rotation)) {
            raw_rotation = 0;
        }
    } else {
        // Handle rotations via stream metadata
        AVDictionaryEntry* tag = av_dict_get(
                fmtctx.streams[streamIndex]->metadata, "rotate", nullptr, 0);
        if (!tag) {
            // If that's not present, check container metadata
            tag = av_dict_get(fmtctx.metadata, "rotate", nullptr, 0);
        }

        if (tag) {
            raw_rotation = atof(tag->value);
            if (std::isnan(raw_rotation)) {
                raw_rotation = 0;
            }
        } else {
            return 0;
        }
    }

    // We only care about 90 degree intervals
    int rotation = static_cast<int>(std::round((raw_rotation) / 90.0) * 90.0);

    rotation %= 360;
    if (rotation < 0) {
        rotation += 360;
    }
    return rotation;
}
}  // namespace

std::optional<RawVideofileSource::VideoFile> RawVideofileSource::openVideoFile(
        const char* filename) {
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
        derror("avformat_find_stream_info failed with %d for '%s'", err,
               filename);
        return std::nullopt;
    }

    const int videoStreamIndex = getVideoStreamIndex(*fmtCtx);
    if (videoStreamIndex < 0) {
        derror("Can't find the video stream in '%s'", filename);
        return std::nullopt;
    }

    const AVCodecParameters* codecParams =
            fmtCtx->streams[videoStreamIndex]->codecpar;
    const AVCodec* codec = ::avcodec_find_decoder(codecParams->codec_id);
    if (!codec) {
        derror("Unsupported codec_id=%d in '%s'", codecParams->codec_id,
               filename);
        return std::nullopt;
    }

    AVCodecContext* codecCtxWeak = ::avcodec_alloc_context3(codec);
    if (!codecCtxWeak) {
        derror("avcodec_alloc_context3 failed for codec_id=%d for '%s'",
               codecParams->codec_id, filename);
        return std::nullopt;
    }

    err = ::avcodec_parameters_to_context(codecCtxWeak, codecParams);
    if (err < 0) {
        derror("avcodec_parameters_to_context failed with %d for '%s'", err,
               filename);
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

    videoFile.baseRotation = getVideoStreamRotation(*fmtCtx, videoStreamIndex);
    videoFile.formatCtx = std::move(fmtCtx);
    videoFile.codecCtx = std::move(codecCtx);
    videoFile.videoStreamIndex = videoStreamIndex;

    return videoFile;
}

RawVideofileSource::RawVideofileSource(RawVideofileSource::VideoFile videoFile)
    : mVideoFile(std::move(videoFile)) {
    // Assuming 30 fps for the moment
    us_per_frame_ = 33333;
}

std::unique_ptr<RawVideofileSource> RawVideofileSource::Create(
        std::string filename) {
    std::optional<VideoFile> maybeVideofile = openVideoFile(filename.c_str());
    if (maybeVideofile) {
        return std::unique_ptr<RawVideofileSource>(
                new RawVideofileSource(std::move(maybeVideofile.value())));
    } else {
        return nullptr;
    }
}

int RawVideofileSource::Start(uint32_t pixel_format, int width, int height) {
    const int err = ::av_seek_frame(mVideoFile.formatCtx.get(), -1, 0,
                                    AVSEEK_FLAG_BACKWARD);
    if (err >= 0) {
        mConverterCache.clear();
        mFrameCache.reset(::av_frame_alloc());
        mConvertedFrameCache.reset(::av_frame_alloc());
        return (mFrameCache || mConvertedFrameCache) ? 0 : -1;
    } else {
        derror("%s:%d av_seek_frame: err=%d", __func__, __LINE__, err);
        return err;
    }
};

bool RawVideofileSource::HasUpdate(RawImageToken token) {
    int64_t current_time = android::base::System::get()->getUnixTimeUs();
    return std::abs(current_time - token.token) > us_per_frame_;
}

absl::StatusOr<RawImageToken> RawVideofileSource::AccessImage(
        std::function<absl::Status(RawImageBuffer*)> accessor) {
    if (const AVFrame* avFrame = decodeNextFrame()) {
        avFrame = convertFrameToRGBA(*avFrame);
        if (!avFrame) {
            derror("Could not convert frame");
            return absl::UnavailableError("Could not decode the frame");
        }

        RawImageBuffer img;
        img.buffer = avFrame->data[0];
        img.buffer_size = avFrame->width * avFrame->height * 4;
        img.width = avFrame->width;
        img.height = avFrame->height;
        img.pixel_format = V4L2_PIX_FMT_RGB32;

        int64_t frame_time = android::base::System::get()->getUnixTimeUs();
        absl::Status ret = accessor(&img);
        if (ret.ok()) {
            return RawImageToken{frame_time};
        } else {
            return ret;
        }
    } else {
        return absl::UnavailableError("Could not decode the frame");
    }
}

int RawVideofileSource::Stop() {
    mFrameCache.reset();
    mConvertedFrameCache.reset();
    mConverterCache.clear();
    return 0;
};

int RawVideofileSource::GetBaseRotation() {
    return mVideoFile.baseRotation;
}

const AVFrame* RawVideofileSource::decodeNextFrame() {
    AVPacket packet;
    av_init_packet(&packet);
    packet.data = nullptr;
    packet.size = 0;

    for (int n = 100; n > 0; --n) {
        int err = ::av_read_frame(mVideoFile.formatCtx.get(), &packet);
        if (err < 0) {
            if (err == AVERROR_EOF) {
                err = ::av_seek_frame(mVideoFile.formatCtx.get(), -1, 0,
                                      AVSEEK_FLAG_BACKWARD);
                if (err >= 0) {
                    continue;
                } else {
                    derror("%s:%d av_seek_frame: err=%d", __func__, __LINE__,
                           err);
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
            derror("%s:%d avcodec_send_packet: err=%d", __func__, __LINE__,
                   err);
            ::av_packet_unref(&packet);
            continue;
        }

        err = ::avcodec_receive_frame(mVideoFile.codecCtx.get(),
                                      mFrameCache.get());
        ::av_packet_unref(&packet);
        if (err >= 0) {
            return mFrameCache.get();
        } else if (err != AVERROR(EAGAIN)) {
            derror("%s:%d avcodec_receive_frame: err=%d", __func__, __LINE__,
                   err);
        }
    }

    return nullptr;
}

const AVFrame* RawVideofileSource::convertFrameToRGBA(const AVFrame& avFrame) {
    SwsContext* swsCtx =
            getSwsContext(avFrame.width, avFrame.height,
                          static_cast<AVPixelFormat>(avFrame.format),
                          avFrame.width, avFrame.height, AV_PIX_FMT_RGBA);
    if (!swsCtx) {
        derror("Could not allocate SwsContext for src={ %dx%d, fmt=%d }, "
               "dst={ %dx%d, fmt=%d }",
               avFrame.width, avFrame.height, avFrame.format, avFrame.width,
               avFrame.height, AV_PIX_FMT_RGBA);
        return nullptr;
    }
    if (!mConvertedFrameCache || mConvertedFrameCache->width != avFrame.width ||
        mConvertedFrameCache->height != avFrame.height) {
        mConvertedFrameCache.reset(::av_frame_alloc());
        mConvertedFrameCache->format = AV_PIX_FMT_RGBA;
        mConvertedFrameCache->width = avFrame.width;
        mConvertedFrameCache->height = avFrame.height;
        int bufferSize = av_image_get_buffer_size(
                AV_PIX_FMT_RGBA, avFrame.width, avFrame.height, 1);
        mConvertedFrameBuffer.resize(bufferSize);
        if (av_image_fill_arrays(mConvertedFrameCache->data,
                                 mConvertedFrameCache->linesize,
                                 mConvertedFrameBuffer.data(), AV_PIX_FMT_RGBA,
                                 avFrame.width, avFrame.height, 1) < 0) {
            return nullptr;
        }
    }

    ::sws_scale(swsCtx, avFrame.data, avFrame.linesize, 0, avFrame.height,
                mConvertedFrameCache->data, mConvertedFrameCache->linesize);
    return mConvertedFrameCache.get();
}

SwsContext* RawVideofileSource::getSwsContext(const int srcWidth,
                                              const int srcHeight,
                                              const AVPixelFormat srcFmt,
                                              const int dstWidth,
                                              const int dstHeight,
                                              const AVPixelFormat dstFmt) {
    ConversionKey conv = {
            .srcWidth = srcWidth,
            .srcHeight = srcHeight,
            .srcFmt = srcFmt,
            .dstWidth = dstWidth,
            .dstHeight = dstHeight,
            .dstFmt = dstFmt,
    };

    const auto i = std::find_if(
            mConverterCache.begin(), mConverterCache.end(),
            [&conv](const std::pair<ConversionKey, SwsContextPtr>& kv) {
                return conv == kv.first;
            });
    if (i != mConverterCache.end()) {
        return i->second.get();
    }

    SwsContext* ctx = ::sws_getContext(srcWidth, srcHeight, srcFmt, dstWidth,
                                       dstHeight, dstFmt, SWS_FAST_BILINEAR,
                                       nullptr, nullptr, nullptr);
    if (ctx) {
        mConverterCache.push_back({std::move(conv), SwsContextPtr(ctx)});
    }

    return ctx;
}
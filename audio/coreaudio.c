/*
 * QEMU OS X CoreAudio audio driver
 *
 * Copyright (c) 2005 Mike Kronenberg
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <pthread.h>            /* pthread_X */

#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "audio.h"
#include "audio_int.h"

#if 0
#define ASSERT(XYZ) assert(XYZ)
#else
#define ASSERT(XYZ)
#endif

static int g_conf_period_frames = 512;
static int g_conf_num_periods = 4;

typedef enum coreaudioVoiceRunningState {
    CA_VOICE_STOPPED = 0,
    CA_VOICE_STOPPING = 1,
    CA_VOICE_RUNNING = 2,
} CoreaudioVoiceRunningState;

typedef struct coreaudioVoice {
    union {
        HWVoiceOut out;
        HWVoiceIn in;
    } hw;
    AudioDeviceIOProcID ioprocid;
    AudioConverterRef converter;
    pthread_mutex_t buf_mutex;
    void *buf_emul;
    AudioDeviceID device_id;
    uint32_t hw_channels : 10;
    uint32_t hw_frame_size : 16;
    uint32_t running_state : 2;
    uint32_t is_output : 1;

    uint32_t size_emul;
    uint32_t pos_emul;
    uint32_t pending_emul;
} CoreaudioVoice;

static const char* ca_OSStatus_str(OSStatus status)
{
#define HANDLE_STATUS(S, TXT) case S: return TXT;
    switch (status) {
    HANDLE_STATUS(kAudioHardwareNoError, "success");
    HANDLE_STATUS(kAudioHardwareBadObjectError, "bad object");
    HANDLE_STATUS(kAudioHardwareUnspecifiedError, "unspecified");
    HANDLE_STATUS(kAudioHardwareNotRunningError, "not running");
    HANDLE_STATUS(kAudioHardwareUnknownPropertyError, "unknown property");
    HANDLE_STATUS(kAudioHardwareBadPropertySizeError, "bad property size");
    HANDLE_STATUS(kAudioHardwareIllegalOperationError, "illegal operation");
    HANDLE_STATUS(kAudioHardwareBadDeviceError, "invalid device");
    HANDLE_STATUS(kAudioHardwareBadStreamError, "invalid stream");
    HANDLE_STATUS(kAudioHardwareUnsupportedOperationError, "unsupported operation");
    HANDLE_STATUS(kAudioDeviceUnsupportedFormatError, "unsupported format");
    HANDLE_STATUS(kAudioDevicePermissionsError, "denied");
    default: return "???";
    }
#undef HANDLE_STATUS
}

#define ca_logwrn(fmt, ...) warn_report("coreaudio: " fmt, __VA_ARGS__)
#define ca_logwrn2(is_output, status, fmt, ...) \
    warn_report("coreaudio(%s, %s): " fmt, \
                 (is_output ? "output" : "input"), \
                 ca_OSStatus_str(status), \
                 __VA_ARGS__)

#define ca_logerr(fmt, ...) error_report("coreaudio: " fmt, __VA_ARGS__)
#define ca_logerr2(is_output, status, fmt, ...) \
    error_report("coreaudio(%s, %s): " fmt, \
                 (is_output ? "output" : "input"), \
                 ca_OSStatus_str(status), \
                 __VA_ARGS__)

static AudioObjectPropertyScope ca_get_os_dir_scope(bool is_output)
{
    return is_output ? kAudioDevicePropertyScopeOutput
                     : kAudioDevicePropertyScopeInput;
}

static const AudioObjectPropertyAddress *ca_get_os_device_addr(
        bool is_output)
{
    static const AudioObjectPropertyAddress out_addr = {
        kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    static const AudioObjectPropertyAddress in_addr = {
        kAudioHardwarePropertyDefaultInputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    return is_output ? &out_addr : &in_addr;
}

static OSStatus ca_get_os_device(bool is_output, AudioDeviceID *id)
{
    UInt32 size = sizeof(*id);
    return AudioObjectGetPropertyData(
            kAudioObjectSystemObject,
            ca_get_os_device_addr(is_output),
            0, NULL,
            &size, id);
}

static OSStatus cs_is_aggregate_device(AudioDeviceID id, bool *result)
{
    static const AudioObjectPropertyAddress addr = {
        kAudioAggregateDevicePropertyActiveSubDeviceList,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    UInt32 size = 0;
    OSStatus status = AudioObjectGetPropertyDataSize(id, &addr, 0, NULL, &size);
    switch (status) {
    case kAudioHardwareNoError:
        *result = (size > sizeof(AudioObjectID));
        break;
    case kAudioHardwareUnknownPropertyError:
        *result = false;
        status = kAudioHardwareNoError;
        break;
    }
    return status;
}

static OSStatus ca_get_device_period_size_frames(bool is_output,
                                                 AudioDeviceID id,
                                                 UInt32 *period_sz)
{
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyBufferFrameSize,
        ca_get_os_dir_scope(is_output),
        kAudioObjectPropertyElementMain
    };

    UInt32 size = sizeof(*period_sz);
    return AudioObjectGetPropertyData(id, &addr,
                                      0, NULL,
                                      &size, period_sz);
}

static OSStatus ca_get_device_stream_format(bool is_output,
                                            AudioDeviceID id,
                                            AudioStreamBasicDescription *d)
{
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyStreamFormat,
        ca_get_os_dir_scope(is_output),
        kAudioObjectPropertyElementMain
    };

    UInt32 size = sizeof(*d);
    return AudioObjectGetPropertyData(id, &addr,
                                      0, NULL,
                                      &size, d);
}

static void ca_voice_lock_impl(CoreaudioVoice *core,
                               const char *fn_name)
{
    int err = pthread_mutex_lock(&core->buf_mutex);
    if (err) {
        ca_logerr("Could not lock voice for %s: %s",
                  fn_name, strerror(err));
        abort();
    }
}

static void ca_voice_unlock_impl(CoreaudioVoice *core,
                                 const char *fn_name)
{
    int err = pthread_mutex_unlock(&core->buf_mutex);
    if (err) {
        ca_logerr("Could not unlock voice for %s: %s",
                  fn_name, strerror(err));
        abort();
    }
}

#define ca_voice_lock(core) ca_voice_lock_impl(core, __func__)
#define ca_voice_unlock(core) ca_voice_unlock_impl(core, __func__)

static inline size_t audio_ring_posb(size_t pos, size_t pending, size_t size)
{
    return (pos >= pending) ? (pos - pending) : (pos + size - pending);
}

static bool ca_update_voice_running_state_locked(CoreaudioVoice *core,
                                                 const bool enable)
{
    if (core->device_id == kAudioDeviceUnknown) {
        ASSERT(!core->ioprocid);
        return false;
    }
    ASSERT(core->ioprocid);

    while (core->running_state == CA_VOICE_STOPPING) {
        ca_voice_unlock(core);
        usleep(1000);
        ca_voice_lock(core);
    }

    if (core->running_state ==
            (enable ? CA_VOICE_RUNNING : CA_VOICE_STOPPED)) {
        return true;
    }

    OSStatus status;

    if (enable) {
        status = AudioDeviceStart(core->device_id, core->ioprocid);
        if (status == kAudioHardwareNoError) {
            core->running_state = CA_VOICE_RUNNING;
        }
    } else {
        core->running_state = CA_VOICE_STOPPING;
        ca_voice_unlock(core);
        status = AudioDeviceStop(core->device_id, core->ioprocid);
        ca_voice_lock(core);
        core->running_state = CA_VOICE_STOPPED;
    }

    if ((status != kAudioHardwareNoError) &&
            (status != kAudioHardwareBadDeviceError)) {
        ca_logerr2(core->is_output, status, "Could not %s the device",
                   (enable ? "resume" : "pause"));
    }

    return status == kAudioHardwareNoError;
}

static bool ca_init_voice_locked(CoreaudioVoice *core,
                                 const struct audsettings *sw_as);
static void ca_fini_voice_locked(CoreaudioVoice *core);

/* This handles both device and format changes */
static OSStatus ca_handle_voice_change(
    AudioObjectID in_object_id,
    UInt32 in_number_addresses,
    const AudioObjectPropertyAddress *in_addresses,
    void *in_client_data)
{
    CoreaudioVoice *core = (CoreaudioVoice *)in_client_data;

    ca_voice_lock(core);
    bool is_running = false;
    if (core->device_id) {
        is_running = (core->running_state == CA_VOICE_RUNNING);
        ca_fini_voice_locked(core);
    }

    if (ca_init_voice_locked(core, NULL)) {
        if (is_running) {
            ca_update_voice_running_state_locked(core, true);
        }
    }
    ca_voice_unlock(core);

    return kAudioHardwareNoError;
}

static OSStatus ca_out_device_ioproc(
    AudioDeviceID inDevice,
    const AudioTimeStamp *inNow,
    const AudioBufferList *inInputData,
    const AudioTimeStamp *inInputTime,
    AudioBufferList *outOutputData,
    const AudioTimeStamp *inOutputTime,
    void *hwptr)
{
    ASSERT(outOutputData->mNumberBuffers == 1);
    if (!outOutputData->mBuffers[0].mDataByteSize) {
        return kAudioHardwareNoError;
    }
    char *out8 = outOutputData->mBuffers[0].mData;

    CoreaudioVoice *core = hwptr;
    ca_voice_lock(core);
    ASSERT(core->is_output);
    ASSERT(core->device_id != kAudioDeviceUnknown);
    ASSERT(!core->converter);
    if (inDevice != core->device_id) {
        memset(out8, 0, outOutputData->mBuffers[0].mDataByteSize);
        ca_voice_unlock(core);
        return kAudioHardwareNoError;
    }

    const unsigned bytes_per_sample = core->hw.out.info.bits / 8;
    const unsigned q_frame_size =
            core->hw.out.info.nchannels * bytes_per_sample;
    ASSERT(q_frame_size > 0);
    const unsigned h_frame_size =
            core->hw_channels * bytes_per_sample;
    ASSERT(h_frame_size > 0);

    size_t len = outOutputData->mBuffers[0].mDataByteSize / h_frame_size * q_frame_size;
    size_t pending_emul = core->pending_emul;
    if (pending_emul < len) {
        const size_t hw_size = pending_emul / q_frame_size * h_frame_size;
        memset(out8 + hw_size, 0,
               outOutputData->mBuffers[0].mDataByteSize - hw_size);
        len = pending_emul;
    }

    const size_t size_emul = core->size_emul;
    ASSERT(size_emul > 0);
    const size_t pos_emul = core->pos_emul;
    ASSERT(pos_emul < size_emul);
    const char *const buf_emul8 = core->buf_emul;
    ASSERT(buf_emul8);

    while (len) {
        const size_t start = audio_ring_posb(pos_emul, pending_emul, size_emul);
        ASSERT(start < size_emul);
        const size_t write_len = MIN(MIN(pending_emul, len), size_emul - start);
        ASSERT(!(write_len % q_frame_size));
        const char *input8 = (const char *)(buf_emul8 + start);

        if (q_frame_size == h_frame_size) {
            memcpy(out8, input8, write_len);
            out8 += write_len;
        } else if (q_frame_size < h_frame_size) {
            const unsigned zeroes = h_frame_size - q_frame_size;
            size_t frames = write_len / q_frame_size;
            for (; frames; --frames) {
                memcpy(out8, input8, q_frame_size);
                input8 += q_frame_size;
                out8 += q_frame_size;
                memset(out8, 0, zeroes);
                out8 += zeroes;
            }
        } else {  /* q_frame_size > h_frame_size */
            size_t frames = write_len / q_frame_size;
            for (; frames; --frames) {
                memcpy(out8, input8, h_frame_size);
                input8 += q_frame_size;
                out8 += h_frame_size;
            }
        }

        pending_emul -= write_len;
        len -= write_len;
    }

    core->pending_emul = pending_emul;
    ca_voice_unlock(core);
    return kAudioHardwareNoError;
}

static OSStatus ca_out_conv_proc_locked(AudioConverterRef converter,
                                        UInt32* frames_to_consume,
                                        AudioBufferList* data_to_consume,
                                        AudioStreamPacketDescription** spd,
                                        void* hwptr)
{
    if (spd) {
        /* not required for PCM, QEMU only works with PCM. */
        *spd = NULL;
    }

    CoreaudioVoice *core = hwptr;
    HWVoiceOut *q_voice = &core->hw.out;
    const size_t q_frame_size = q_voice->info.nchannels * (q_voice->info.bits / 8);
    ASSERT(core->size_emul > 0);
    ASSERT(core->pending_emul <= core->size_emul);
    ASSERT(core->pos_emul < core->size_emul);

    const size_t q_read_pos = audio_ring_posb(core->pos_emul,
                                              core->pending_emul,
                                              core->size_emul);
    ASSERT(q_read_pos < core->size_emul);

    const size_t q_read_len = MIN(MIN(core->pending_emul, core->size_emul - q_read_pos),
                                  *frames_to_consume * q_frame_size);
    ASSERT(!(q_read_len % q_frame_size));

    data_to_consume->mNumberBuffers = 1;
    data_to_consume->mBuffers[0].mData = ((char*)core->buf_emul) + q_read_pos;
    data_to_consume->mBuffers[0].mDataByteSize = q_read_len;
    core->pending_emul -= q_read_len;
    *frames_to_consume = q_read_len / q_frame_size;

    return kAudioHardwareNoError;
}

static OSStatus ca_out_device_wconv_ioproc(
    AudioDeviceID inDevice,
    const AudioTimeStamp *inNow,
    const AudioBufferList *inInputData,
    const AudioTimeStamp *inInputTime,
    AudioBufferList *outOutputData,
    const AudioTimeStamp *inOutputTime,
    void *hwptr)
{
    ASSERT(outOutputData->mNumberBuffers == 1);
    if (!outOutputData->mBuffers[0].mDataByteSize ||
            !outOutputData->mBuffers[0].mData) {
        return kAudioHardwareNoError;
    }

    CoreaudioVoice *core = hwptr;
    ca_voice_lock(core);
    ASSERT(core->is_output);
    ASSERT(core->device_id != kAudioDeviceUnknown);
    ASSERT(core->converter);
    ASSERT(core->hw_frame_size > 0);

    if (inDevice != core->device_id) {
        ca_voice_unlock(core);
        memset(outOutputData->mBuffers[0].mData, 0,
               outOutputData->mBuffers[0].mDataByteSize);
        return kAudioHardwareNoError;
    }

    const UInt32 requested_size_frames =
            outOutputData->mBuffers[0].mDataByteSize /
            core->hw_frame_size;
    UInt32 output_size_frames = requested_size_frames;

    OSStatus status =
            AudioConverterFillComplexBuffer(core->converter,
                                            ca_out_conv_proc_locked,
                                            core,
                                            &output_size_frames,
                                            outOutputData,
                                            NULL);
    ca_voice_unlock(core);

    if ((status != kAudioHardwareNoError) ||
            (output_size_frames < requested_size_frames)) {
        const size_t filled_bytes = output_size_frames * core->hw_frame_size;
        ASSERT(filled_bytes <= outOutputData->mBuffers[0].mDataByteSize);

        memset(((char *)outOutputData->mBuffers[0].mData) + filled_bytes, 0,
               outOutputData->mBuffers[0].mDataByteSize - filled_bytes);
    }

    return kAudioHardwareNoError;
}

static OSStatus ca_in_device_ioproc(
    AudioDeviceID inDevice,
    const AudioTimeStamp *inNow,
    const AudioBufferList *inInputData,
    const AudioTimeStamp *inInputTime,
    AudioBufferList *outOutputData,
    const AudioTimeStamp *inOutputTime,
    void *hwptr)
{
    ASSERT(inInputData->mNumberBuffers == 1);
    if (!inInputData->mBuffers[0].mDataByteSize ||
            !inInputData->mBuffers[0].mData) {
        return kAudioHardwareNoError;
    }

    CoreaudioVoice *core = hwptr;
    ca_voice_lock(core);
    ASSERT(!core->is_output);
    ASSERT(core->device_id != kAudioDeviceUnknown);
    ASSERT(!core->converter);
    if (inDevice != core->device_id) {
        ca_voice_unlock(core);
        return kAudioHardwareNoError;
    }

    const unsigned bytes_per_sample = core->hw.in.info.bits / 8;
    const unsigned q_frame_size =
            core->hw.in.info.nchannels * bytes_per_sample;
    ASSERT(q_frame_size > 0);
    const unsigned h_frame_size =
            core->hw_channels * bytes_per_sample;
    ASSERT(h_frame_size > 0);

    size_t len = inInputData->mBuffers[0].mDataByteSize / h_frame_size * q_frame_size;
    const size_t size_emul = core->size_emul;
    ASSERT(size_emul > 0);

    size_t pending_emul = core->pending_emul;
    ASSERT(pending_emul <= size_emul);

    size_t pos_emul = core->pos_emul;
    ASSERT(pos_emul < size_emul);

    size_t space_in_hw = size_emul - pending_emul;
    if (len > space_in_hw) {
        len = space_in_hw;
    }

    char *const buf_emul8 = core->buf_emul;
    ASSERT(buf_emul8);

    const char *input8 = inInputData->mBuffers[0].mData;
    ASSERT(input8);

    while (len) {
        const size_t write_len = MIN(len, size_emul - pos_emul);
        ASSERT(!(write_len % q_frame_size));
        char *out8 = buf_emul8 + pos_emul;

        if (q_frame_size == h_frame_size) {
            memcpy(out8, input8, write_len);
            input8 += write_len;
        } else if (q_frame_size < h_frame_size) {
            size_t frames = write_len / q_frame_size;
            for (; frames; --frames) {
                memcpy(out8, input8, q_frame_size);
                out8 += q_frame_size;
                input8 += h_frame_size;
            }
        } else {  /* q_frame_size > h_frame_size */
            const unsigned zeroes = q_frame_size - h_frame_size;
            size_t frames = write_len / q_frame_size;
            for (; frames; --frames) {
                memcpy(out8, input8, h_frame_size);
                input8 += h_frame_size;
                out8 += h_frame_size;
                memset(out8, 0, zeroes);
                out8 += zeroes;
            }
        }

        pos_emul = (pos_emul + write_len) % size_emul;
        pending_emul += write_len;
        len -= write_len;
    }

    core->pos_emul = pos_emul % size_emul;
    core->pending_emul = pending_emul;
    ca_voice_unlock(core);
    return kAudioHardwareNoError;
}

static OSStatus ca_in_conv_proc_locked(AudioConverterRef converter,
                                       UInt32 *frames_to_consume,
                                       AudioBufferList *data_to_consume,
                                       AudioStreamPacketDescription **spd,
                                       void *input_data_raw)
{
    if (spd) {
        *spd = NULL;
    }

    AudioBufferList *input_data = input_data_raw;
    const uint32_t available_frames = input_data->mBuffers[0].mDataByteSize;
    const uint32_t hacked_channels = input_data->mBuffers[0].mNumberChannels;
    const uint32_t frame_size = hacked_channels >> 10;
    const size_t num_frames = MIN(*frames_to_consume, available_frames);

    *data_to_consume = *input_data;
    data_to_consume->mBuffers[0].mDataByteSize = num_frames * frame_size;
    data_to_consume->mBuffers[0].mNumberChannels = hacked_channels & 0x3FF;
    *frames_to_consume = num_frames;

    input_data->mBuffers[0].mData =
            ((char*)input_data->mBuffers[0].mData) + num_frames * frame_size;
    input_data->mBuffers[0].mDataByteSize -= num_frames;
    return kAudioHardwareNoError;
}

static OSStatus ca_in_device_wconv_ioproc(
    AudioDeviceID inDevice,
    const AudioTimeStamp *inNow,
    const AudioBufferList *inInputData,
    const AudioTimeStamp *inInputTime,
    AudioBufferList *outOutputData,
    const AudioTimeStamp *inOutputTime,
    void *hwptr)
{
    ASSERT(inInputData->mNumberBuffers == 1);
    if (!inInputData->mBuffers[0].mDataByteSize ||
            !inInputData->mBuffers[0].mData) {
        return kAudioHardwareNoError;
    }

    CoreaudioVoice *core = hwptr;
    ca_voice_lock(core);
    ASSERT(!core->is_output);
    ASSERT(core->device_id != kAudioDeviceUnknown);
    ASSERT(core->converter);
    ASSERT(core->hw_frame_size > 0);

    if (inDevice != core->device_id) {
        ca_voice_unlock(core);
        return kAudioHardwareNoError;
    }

    UInt32 requested_size_frames =
            inInputData->mBuffers[0].mDataByteSize /
            core->hw_frame_size;

    AudioBufferList input_data = *inInputData;
    input_data.mBuffers[0].mDataByteSize = requested_size_frames;
    input_data.mBuffers[0].mNumberChannels =
            (core->hw_frame_size << 10) |
            inInputData->mBuffers[0].mNumberChannels;

    HWVoiceIn *q_voice = &core->hw.in;
    const size_t q_frame_size = q_voice->info.nchannels * (q_voice->info.bits / 8);
    ASSERT(q_frame_size > 0);
    const size_t size_emul = core->size_emul;
    ASSERT(size_emul > 0);
    size_t pending_emul = core->pending_emul;
    ASSERT(pending_emul <= size_emul);
    size_t pos_emul = core->pos_emul;
    ASSERT(pos_emul < size_emul);
    char* buf_emul8 = core->buf_emul;
    ASSERT(buf_emul8);

    while (requested_size_frames) {
        UInt32 size_frames =
                MIN(requested_size_frames,
                    MIN(size_emul - pending_emul, size_emul - pos_emul) /
                            q_frame_size);

        AudioBufferList output_data;
        output_data.mNumberBuffers = 1;
        output_data.mBuffers[0].mData = buf_emul8 + pos_emul;
        output_data.mBuffers[0].mDataByteSize = size_frames * q_frame_size;
        output_data.mBuffers[0].mNumberChannels = q_voice->info.nchannels;

        OSStatus status =
                AudioConverterFillComplexBuffer(core->converter,
                                                ca_in_conv_proc_locked,
                                                &input_data,
                                                &size_frames,
                                                &output_data,
                                                NULL);
        if (status || !size_frames) {
            break;
        }

        const size_t q_size = size_frames * q_frame_size;
        pending_emul += q_size;
        pos_emul = (pos_emul + q_size) % size_emul;

        requested_size_frames -= size_frames;
    }

    core->pending_emul = pending_emul;
    core->pos_emul = pos_emul;
    ca_voice_unlock(core);

    return kAudioHardwareNoError;
}

static void ca_unlisten_dev_change_locked(CoreaudioVoice *core)
{
    OSStatus status;

    ca_voice_unlock(core);
    status = AudioObjectRemovePropertyListener(
            kAudioObjectSystemObject,
            ca_get_os_device_addr(core->is_output),
            ca_handle_voice_change,
            core);
    if ((status != kAudioHardwareNoError) &&
            (status != kAudioHardwareBadObjectError)) {
        ca_logerr2(core->is_output, status, "%s",
                   "Could not remove the device change callback");
    }
    ca_voice_lock(core);
}

static const AudioObjectPropertySelector ca_fmt_change_selectors[] = {
    kAudioDevicePropertyNominalSampleRate,
    kAudioDevicePropertyStreamFormat,
    kAudioDevicePropertyStreamConfiguration,
    kAudioDevicePropertyStreams,
};

static void ca_unlisten_fmt_change_locked_impl(const bool is_output,
                                               const AudioDeviceID device_id,
                                               CoreaudioVoice *core,
                                               unsigned i)
{
    ASSERT(device_id != kAudioDeviceUnknown);
    ASSERT(i > 0);
    const AudioObjectPropertyScope scope = ca_get_os_dir_scope(is_output);
    OSStatus status;

    ca_voice_unlock(core);
    for (; i; --i) {
        const AudioObjectPropertyAddress addr = {
            ca_fmt_change_selectors[i - 1],
            scope,
            kAudioObjectPropertyElementMain
        };

        status = AudioObjectRemovePropertyListener(
                device_id, &addr,
                ca_handle_voice_change, core);
        if ((status != kAudioHardwareNoError) &&
                (status != kAudioHardwareBadObjectError)) {
            ca_logerr2(is_output, status,
                       "Could not remove a format change property "
                       "listener (selector=%" PRIu32 ")",
                       addr.mSelector);
        }
    }
    ca_voice_lock(core);
}

static OSStatus ca_listen_fmt_change_locked(const bool is_output,
                                            const AudioDeviceID device_id,
                                            CoreaudioVoice *core)
{
    ASSERT(device_id != kAudioDeviceUnknown);
    const AudioObjectPropertyScope scope = ca_get_os_dir_scope(is_output);

    for (unsigned i = 0; i < sizeof(ca_fmt_change_selectors) /
                             sizeof(ca_fmt_change_selectors[0]); ++i) {
        const AudioObjectPropertyAddress addr = {
            ca_fmt_change_selectors[i],
            scope,
            kAudioObjectPropertyElementMain
        };

        OSStatus status = AudioObjectAddPropertyListener(device_id, &addr,
                                                         ca_handle_voice_change,
                                                         core);
        if (status != kAudioHardwareNoError) {
            /* if the device changes we cannot unlisten */
            if (status != kAudioHardwareBadObjectError) {
                ca_logerr2(is_output, status,
                           "Could not add a format change property "
                           "listener (selector=%" PRIu32 ")",
                           addr.mSelector);

                if (i > 0) {
                    ca_unlisten_fmt_change_locked_impl(is_output,
                                                       device_id,
                                                       core,
                                                       i);
                }
            }

            return status;
        }
    }

    return kAudioHardwareNoError;
}

static void ca_unlisten_fmt_change_locked(const AudioDeviceID device_id,
                                          CoreaudioVoice *core)
{
    ca_unlisten_fmt_change_locked_impl(
            core->is_output, device_id, core,
            sizeof(ca_fmt_change_selectors) /
                    sizeof(ca_fmt_change_selectors[0]));
}

static bool ca_get_sample_format(const AudioStreamBasicDescription *hw,
                                 audfmt_e *af)
{
    if (!(hw->mFormatFlags & kAudioFormatFlagIsPacked) ||
            (hw->mFormatFlags & kAudioFormatFlagIsNonInterleaved) ||
            (hw->mFramesPerPacket != 1) ||
            (hw->mBytesPerFrame != hw->mBytesPerPacket) ||
            (hw->mFormatID != kAudioFormatLinearPCM)) {
        return false;
    }

    if (hw->mFormatFlags & kAudioFormatFlagIsSignedInteger) {
        switch (hw->mBitsPerChannel) {
        case sizeof(int8_t) * CHAR_BIT:
            *af = AUD_FMT_S8;
            return true;
        case sizeof(int16_t) * CHAR_BIT:
            *af = AUD_FMT_S16;
            return true;
        case sizeof(int32_t) * CHAR_BIT:
            *af = AUD_FMT_S32;
            return true;
        default:
            return false;
        }
    } else {
        return false;
    }
}

static audfmt_e ca_to_safe_audfmt(const audfmt_e fmt)
{
    switch (fmt) {
    case AUD_FMT_S32:
    case AUD_FMT_U32:
        return AUD_FMT_S32;

    default:
        return AUD_FMT_S16;
    }
}

static audfmt_e ca_to_audfmt_e(const struct audio_pcm_info *sw)
{
    switch (sw->bits) {
    case (CHAR_BIT * sizeof(int8_t)):
        return sw->sign ? AUD_FMT_S8 : AUD_FMT_U8;

    case (CHAR_BIT * sizeof(int16_t)):
        return sw->sign ? AUD_FMT_S16 : AUD_FMT_U16;

    default:
        return sw->sign ? AUD_FMT_S32 : AUD_FMT_U32;
    }
}

static bool ca_is_converter_required(const AudioStreamBasicDescription *hw,
                                     const struct audio_pcm_info *sw)
{
    audfmt_e hw_af;
    return (fabs(hw->mSampleRate - sw->freq) > (sw->freq * 0.0005)) ||
           !ca_get_sample_format(hw, &hw_af) ||
           (hw_af != ca_to_audfmt_e(sw));
}

static AudioStreamBasicDescription
ca_to_AudioStreamBasicDescription(const size_t freq,
                                  const size_t nchannels,
                                  const audfmt_e fmt)
{
    size_t bytes_per_sample;
    AudioFormatFlags flags;

    switch (fmt) {
    case AUD_FMT_S8:
        flags = kAudioFormatFlagIsPacked | kAudioFormatFlagIsSignedInteger;
        bytes_per_sample = sizeof(int8_t);
        break;

    case AUD_FMT_S16:
        flags = kAudioFormatFlagIsPacked | kAudioFormatFlagIsSignedInteger;
        bytes_per_sample = sizeof(int16_t);
        break;

    case AUD_FMT_S32:
    default:
        flags = kAudioFormatFlagIsPacked | kAudioFormatFlagIsSignedInteger;
        bytes_per_sample = sizeof(int32_t);
        break;
    }

    AudioStreamBasicDescription result = {
        .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = flags,
        .mSampleRate = freq,
        .mBitsPerChannel = bytes_per_sample * CHAR_BIT,
        .mBytesPerFrame = bytes_per_sample * nchannels,
        .mChannelsPerFrame = nchannels,
        .mBytesPerPacket = bytes_per_sample * nchannels,
        .mFramesPerPacket = 1,
    };

    return result;
}

static size_t ca_get_periods_count(const CoreaudioVoice *core)
{
    return g_conf_num_periods > 0 ? g_conf_num_periods : 4;
}

static bool ca_init_voice_locked(CoreaudioVoice *core,
                                 const struct audsettings *sw_as)
{
    ASSERT(core->device_id == kAudioDeviceUnknown);
    ASSERT(!core->ioprocid);
    ASSERT(!core->converter);

    for (unsigned retry = 10; retry; --retry) {
        OSStatus status;
        AudioDeviceID device_id;

        status = ca_get_os_device(core->is_output, &device_id);
        if (status != kAudioHardwareNoError) {
no_voice:   ca_logerr2(core->is_output, status, "%s",
                       "Could not get the default device");
            return false;
        } else if (device_id == kAudioDeviceUnknown) {
            status = kAudioHardwareUnspecifiedError;
            goto no_voice;
        }

        bool is_aggregate_device;
        status = cs_is_aggregate_device(device_id, &is_aggregate_device);
        if (status != kAudioHardwareNoError) {
            if (status == kAudioHardwareBadObjectError) {
                continue;
            }
            ca_logerr2(core->is_output, status, "%s",
                       "Could not detect if an aggregate device");
            return false;
        } else if (is_aggregate_device) {
            ca_logerr2(core->is_output, status, "%s",
                       "Aggregate devices are not supported");
            return false;
        }

        status = ca_listen_fmt_change_locked(core->is_output, device_id, core);
        if (status != kAudioHardwareNoError) {
            if (status == kAudioHardwareBadObjectError) {
                continue;
            } else {
                return false;
            }
        }

        AudioStreamBasicDescription hw_stream_fmt;
        status = ca_get_device_stream_format(core->is_output, device_id,
                                             &hw_stream_fmt);
        if (status != kAudioHardwareNoError) {
            ca_unlisten_fmt_change_locked(device_id, core);
            if (status == kAudioHardwareBadObjectError) {
                continue;
            }

            ca_logerr2(core->is_output, status, "%s",
                       "Could not get device stream format");
            return false;
        } else if ((hw_stream_fmt.mSampleRate < 8000) ||
                !hw_stream_fmt.mChannelsPerFrame) {
            ca_unlisten_fmt_change_locked(device_id, core);
            ca_logerr2(core->is_output, status, "Bad audio device: "
                       "freq=%fHz nChannels=%" PRIu32,
                       hw_stream_fmt.mSampleRate,
                       hw_stream_fmt.mChannelsPerFrame);
            return false;
        }

        UInt32 hw_period_size_frames;
        status = ca_get_device_period_size_frames(core->is_output, device_id,
                                                  &hw_period_size_frames);
        if (status != kAudioHardwareNoError) {
            ca_unlisten_fmt_change_locked(device_id, core);
            if (status == kAudioHardwareBadObjectError) {
                continue;
            }

            ca_logerr2(core->is_output, status, "%s",
                       "Could not get device period size");
            return false;
        }

        struct audio_pcm_info *info = core->is_output ? &core->hw.out.info
                                                      : &core->hw.in.info;
        if (sw_as) {
            ASSERT(sw_as->freq > 0);
            ASSERT(sw_as->nchannels > 0);

            /*
             * This is a new voice, try using the sound card
             * values to avoid double conversion.
             */
            struct audsettings as = {
                /*
                 * This accommodates 5 channel guest streams on
                 * a 16 channel sound card in MacOS.
                 */
                .nchannels = MIN(sw_as->nchannels,
                                 (int)hw_stream_fmt.mChannelsPerFrame),
                .endianness = AUDIO_HOST_ENDIANNESS,
            };

            if (ca_get_sample_format(&hw_stream_fmt, &as.fmt)) {
                as.freq = hw_stream_fmt.mSampleRate;
            } else {
                /*
                 * A converter will be created anyway, use
                 * what QEMU suggested.
                 */
                as.freq = sw_as->freq;
                as.fmt = ca_to_safe_audfmt(sw_as->fmt);
            }

            audio_pcm_init_info(info, &as);
        }

        uint32_t qemu_period_size_frames = hw_period_size_frames;
        AudioConverterRef converter = NULL;
        if (ca_is_converter_required(&hw_stream_fmt, info)) {
            const audfmt_e sw_af = ca_to_audfmt_e(info);
            const AudioStreamBasicDescription sw_stream_fmt =
                    ca_to_AudioStreamBasicDescription(info->freq,
                                                      info->nchannels,
                                                      sw_af);
            /* AudioConverterNew(src, dst, &converter) */
            if (core->is_output) {
                status = AudioConverterNew(&sw_stream_fmt, &hw_stream_fmt,
                                           &converter);
            } else {
                status = AudioConverterNew(&hw_stream_fmt, &sw_stream_fmt,
                                           &converter);
            }

            if ((status != kAudioHardwareNoError) || !converter) {
                ca_unlisten_fmt_change_locked(device_id, core);
                ca_logerr2(core->is_output, status, "%s",
                           "Could not create AudioConverter");
                return false;
            }

            qemu_period_size_frames =
                    ((uint64_t)hw_period_size_frames) * ((uint64_t)info->freq) /
                    hw_stream_fmt.mSampleRate;
        }

        static const AudioDeviceIOProc ioprocs[2][2] = {
            {ca_in_device_ioproc, ca_in_device_wconv_ioproc},
            {ca_out_device_ioproc, ca_out_device_wconv_ioproc},
        };

        /*
         * set Callback.
         *
         * On macOS 11.3.1, Core Audio calls AudioDeviceIOProc after calling an
         * internal function named HALB_Mutex::Lock(), which locks a mutex in
         * HALB_IOThread::Entry(void*). HALB_Mutex::Lock() is also called in
         * AudioObjectGetPropertyData, which is called by coreaudio driver.
         * Therefore, the specified callback must be designed to avoid a deadlock
         * with the callers of AudioObjectGetPropertyData.
         */
        AudioDeviceIOProcID ioprocid = NULL;
        status = AudioDeviceCreateIOProcID(device_id,
                                           ioprocs[core->is_output][converter != NULL],
                                           core,
                                           &ioprocid);
        if ((status != kAudioHardwareNoError) || !ioprocid) {
            if (converter) {
                AudioConverterDispose(converter);
            }

            ca_unlisten_fmt_change_locked(device_id, core);
            if (status == kAudioHardwareBadObjectError) {
                continue;
            }

            ca_logerr2(core->is_output, status, "%s", "Could not set IOProc");
            return false;
        }

        core->device_id = device_id;
        core->ioprocid = ioprocid;
        core->converter = converter;
        core->hw_channels = hw_stream_fmt.mChannelsPerFrame;
        core->hw_frame_size = hw_stream_fmt.mBytesPerFrame;

        const size_t q_frame_size = info->nchannels * (info->bits / 8);
        const size_t buffer_size_samples =
                ca_get_periods_count(core) * qemu_period_size_frames;
        ASSERT(buffer_size_samples > 0);

        if (sw_as) {
            if (core->is_output) {
                core->hw.out.samples = buffer_size_samples;
            } else {
                core->hw.in.samples = buffer_size_samples;
            }
        }

        core->size_emul = buffer_size_samples * q_frame_size;
        core->buf_emul = g_malloc0(core->size_emul);
        core->pos_emul = 0;
        core->pending_emul = 0;

        return true;
    }

    ca_logerr2(core->is_output, kAudioHardwareBadObjectError, "%s",
               "Ran out of retries to get the voice object");
    return false;
}

static int coreaudio_init_impl(const bool is_output,
                               CoreaudioVoice *core,
                               struct audsettings *sw_as)
{
    OSStatus status;
    int err;

    core->is_output = is_output ? 1 : 0;
    core->running_state = CA_VOICE_STOPPED;

    err = pthread_mutex_init(&core->buf_mutex, NULL);
    if (err) {
        error_report("coreaudio: Could not create mutex: %s", strerror(err));
        return -1;
    }

    ca_voice_lock(core);
    status = AudioObjectAddPropertyListener(kAudioObjectSystemObject,
                                            ca_get_os_device_addr(is_output),
                                            ca_handle_voice_change,
                                            core);
    if (status != kAudioHardwareNoError) {
        ca_voice_unlock(core);
        pthread_mutex_destroy(&core->buf_mutex);
        ca_logerr2(is_output, status, "%s",
                   "Could not listen to device property change");
        return -1;
    }

    if (!ca_init_voice_locked(core, sw_as)) {
        ca_unlisten_dev_change_locked(core);
        ca_voice_unlock(core);
        pthread_mutex_destroy(&core->buf_mutex);
        return -1;
    }

    ca_voice_unlock(core);
    return 0;
}

static void ca_fini_voice_locked(CoreaudioVoice *core)
{
    ASSERT(core->device_id != kAudioDeviceUnknown);
    ASSERT(core->ioprocid);

    OSStatus status;

    ca_update_voice_running_state_locked(core, false);
    ca_unlisten_fmt_change_locked(core->device_id, core);

    status = AudioDeviceDestroyIOProcID(core->device_id, core->ioprocid);
    core->ioprocid = NULL;
    if ((status != kAudioHardwareNoError) &&
            (status != kAudioHardwareBadDeviceError)) {
        ca_logerr2(core->is_output, status, "%s", "Could not remove IOProc");
    }

    if (core->converter) {
        AudioConverterDispose(core->converter);
        core->converter = NULL;
    }

    g_free(core->buf_emul);
    core->buf_emul = NULL;
    core->size_emul = 0;
    core->pos_emul = 0;
    core->pending_emul = 0;

    core->device_id = kAudioDeviceUnknown;
}

static void coreaudio_fini_impl(CoreaudioVoice *core)
{
    ca_voice_lock(core);
    ca_unlisten_dev_change_locked(core);
    if (core->device_id != kAudioDeviceUnknown) {
        ca_fini_voice_locked(core);
    }
    ca_voice_unlock(core);
    pthread_mutex_destroy(&core->buf_mutex);
}

static void coreaudio_enable_impl(CoreaudioVoice *core,
                                  const bool enable)
{
    ca_voice_lock(core);
    ca_update_voice_running_state_locked(core, enable);
    ca_voice_unlock(core);
}

static int coreaudio_init_out(HWVoiceOut *hw, struct audsettings *as, void *drv_opaque)
{
    (void)drv_opaque;
    return coreaudio_init_impl(true, (CoreaudioVoice *)hw, as);
}

static void coreaudio_fini_out(HWVoiceOut *hw)
{
    coreaudio_fini_impl((CoreaudioVoice *)hw);
}

static int coreaudio_run_out(HWVoiceOut *hw, int live)
{
    if (live <= 0) {
        return 0;
    }

    CoreaudioVoice *core = (CoreaudioVoice *)hw;

    ca_voice_lock(core);
    const size_t q_frame_size = hw->info.nchannels * (hw->info.bits / 8);
    if (!core->buf_emul || !core->size_emul) {
        ca_voice_unlock(core);
        return 0;
    }

    size_t free_bytes = core->size_emul - core->pending_emul;
    size_t free_samples = free_bytes / q_frame_size;
    size_t to_write = MIN((size_t)live, free_samples);
    size_t written = 0;

    while (written < to_write) {
        size_t samples_left = to_write - written;
        size_t mix_chunk = MIN(samples_left, (size_t)(hw->samples - hw->rpos));
        size_t emul_chunk_samples = (core->size_emul - core->pos_emul) / q_frame_size;
        size_t chunk = MIN(mix_chunk, emul_chunk_samples);

        struct st_sample *src = hw->mix_buf + hw->rpos;
        char *dst = ((char *)core->buf_emul) + core->pos_emul;

        hw->clip(dst, src, chunk);

        hw->rpos = (hw->rpos + chunk) % hw->samples;
        core->pos_emul = (core->pos_emul + chunk * q_frame_size) % core->size_emul;
        core->pending_emul += chunk * q_frame_size;
        written += chunk;
    }

    ca_voice_unlock(core);
    return written;
}

static int coreaudio_write(SWVoiceOut *sw, void *buf, int size)
{
    return audio_pcm_sw_write(sw, buf, size);
}

static int coreaudio_ctl_out(HWVoiceOut *hw, int cmd, ...)
{
    CoreaudioVoice *core = (CoreaudioVoice *)hw;
    switch (cmd) {
    case VOICE_ENABLE:
        coreaudio_enable_impl(core, true);
        break;
    case VOICE_DISABLE:
        coreaudio_enable_impl(core, false);
        break;
    }
    return 0;
}

static int coreaudio_init_in(HWVoiceIn *hw, struct audsettings *as, void *drv_opaque)
{
    (void)drv_opaque;
    return coreaudio_init_impl(false, (CoreaudioVoice *)hw, as);
}

static void coreaudio_fini_in(HWVoiceIn *hw)
{
    coreaudio_fini_impl((CoreaudioVoice *)hw);
}

static int coreaudio_run_in(HWVoiceIn *hw)
{
    CoreaudioVoice *core = (CoreaudioVoice *)hw;

    ca_voice_lock(core);
    const size_t q_frame_size = hw->info.nchannels * (hw->info.bits / 8);

    if (!core->buf_emul || !core->size_emul) {
        ca_voice_unlock(core);
        return 0;
    }

    size_t avail_samples = core->pending_emul / q_frame_size;
    size_t free_samples = hw->samples - hw->total_samples_captured;
    size_t to_read = MIN(avail_samples, free_samples);
    size_t read_samples = 0;

    while (read_samples < to_read) {
        size_t samples_left = to_read - read_samples;
        size_t conv_chunk = MIN(samples_left, (size_t)(hw->samples - hw->wpos));
        size_t q_read_pos = audio_ring_posb(core->pos_emul, core->pending_emul, core->size_emul);
        size_t emul_chunk_samples = (core->size_emul - q_read_pos) / q_frame_size;
        size_t chunk = MIN(conv_chunk, emul_chunk_samples);

        const char *src = ((const char *)core->buf_emul) + q_read_pos;
        struct st_sample *dst = hw->conv_buf + hw->wpos;

        hw->conv(dst, src, chunk);

        hw->wpos = (hw->wpos + chunk) % hw->samples;
        core->pending_emul -= chunk * q_frame_size;
        read_samples += chunk;
    }

    ca_voice_unlock(core);
    return read_samples;
}

static int coreaudio_read(SWVoiceIn *sw, void *buf, int size)
{
    return audio_pcm_sw_read(sw, buf, size);
}

static int coreaudio_ctl_in(HWVoiceIn *hw, int cmd, ...)
{
    CoreaudioVoice *core = (CoreaudioVoice *)hw;
    switch (cmd) {
    case VOICE_ENABLE:
        coreaudio_enable_impl(core, true);
        break;
    case VOICE_DISABLE:
        coreaudio_enable_impl(core, false);
        break;
    }
    return 0;
}

static void *coreaudio_audio_init(void)
{
    return &g_conf_num_periods;
}

static void coreaudio_audio_fini(void *opaque)
{
    (void)opaque;
}

static struct audio_option coreaudio_options[] = {
    {
        .name  = "BUFFER_SIZE",
        .tag   = AUD_OPT_INT,
        .valp  = &g_conf_period_frames,
        .descr = "Size of the buffer in frames"
    },
    {
        .name  = "BUFFER_COUNT",
        .tag   = AUD_OPT_INT,
        .valp  = &g_conf_num_periods,
        .descr = "Number of buffers"
    },
    { /* End of list */ }
};

static struct audio_pcm_ops coreaudio_pcm_ops = {
    .init_out = coreaudio_init_out,
    .fini_out = coreaudio_fini_out,
    .run_out  = coreaudio_run_out,
    .write    = coreaudio_write,
    .ctl_out  = coreaudio_ctl_out,

    .init_in  = coreaudio_init_in,
    .fini_in  = coreaudio_fini_in,
    .run_in   = coreaudio_run_in,
    .read     = coreaudio_read,
    .ctl_in   = coreaudio_ctl_in,
};

static struct audio_driver coreaudio_audio_driver = {
    .name           = "coreaudio",
    .descr          = "CoreAudio http://developer.apple.com/audio/coreaudio.html",
    .options        = coreaudio_options,
    .init           = coreaudio_audio_init,
    .fini           = coreaudio_audio_fini,
    .pcm_ops        = &coreaudio_pcm_ops,
    .can_be_default = 1,
    .max_voices_out = 1,
    .max_voices_in  = 1,
    .voice_size_out = sizeof(CoreaudioVoice),
    .voice_size_in  = sizeof(CoreaudioVoice),
};

static void register_audio_coreaudio(void)
{
    audio_driver_register(&coreaudio_audio_driver);
}
type_init(register_audio_coreaudio);

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
#include "qemu/audio.h"
#include "qom/object.h"
#include "audio_int.h"

#if 1
#define ASSERT(XYZ) assert(XYZ)
#else
#define ASSERT(XYZ)
#endif

#define TYPE_AUDIO_COREAUDIO "audio-coreaudio"
OBJECT_DECLARE_SIMPLE_TYPE(AudioCoreaudio, AUDIO_COREAUDIO)

struct AudioCoreaudio {
    AudioMixengBackend parent_obj;
};

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
    AudioDeviceID device_id;
    uint32_t hw_channels : 10;
    uint32_t hw_frame_size : 16;
    uint32_t running_state : 2;
    uint32_t is_output : 1;
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

#define COREAUDIO_WRAPPER_FUNC(name, ret_type, args_decl, args)       \
    static ret_type glue(coreaudio_, name)args_decl                   \
    {                                                                 \
        CoreaudioVoice *core = (CoreaudioVoice *)hw;            \
        ret_type ret;                                                 \
        ca_voice_lock(core);                                   \
        ret = glue(audio_generic_, name)args;                         \
        ca_voice_unlock(core);                                 \
        return ret;                                                   \
    }
COREAUDIO_WRAPPER_FUNC(buffer_get_free, size_t, (HWVoiceOut *hw), (hw))
COREAUDIO_WRAPPER_FUNC(get_buffer_out, void *, (HWVoiceOut *hw, size_t *size),
                       (hw, size))
COREAUDIO_WRAPPER_FUNC(put_buffer_out, size_t,
                       (HWVoiceOut *hw, void *buf, size_t size),
                       (hw, buf, size))
COREAUDIO_WRAPPER_FUNC(write, size_t, (HWVoiceOut *hw, void *buf, size_t size),
                       (hw, buf, size))
#undef COREAUDIO_WRAPPER_FUNC

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
                                 const struct audsettings *mixeng_as);
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

    const unsigned q_frame_size =
            core->hw.out.info.nchannels * core->hw.out.info.bytes_per_frame;
    ASSERT(q_frame_size > 0);
    const unsigned h_frame_size =
            core->hw_channels * core->hw.out.info.bytes_per_frame;
    ASSERT(h_frame_size > 0);

    size_t len = outOutputData->mBuffers[0].mDataByteSize / h_frame_size * q_frame_size;
    size_t pending_emul = core->hw.out.pending_emul;
    if (pending_emul < len) {
        const size_t hw_size = pending_emul / q_frame_size * h_frame_size;
        memset(out8 + hw_size, 0,
               outOutputData->mBuffers[0].mDataByteSize - hw_size);
        len = pending_emul;
    }

    const size_t size_emul = core->hw.out.size_emul;
    ASSERT(size_emul > 0);
    const size_t pos_emul = core->hw.out.pos_emul;
    ASSERT(pos_emul < size_emul);
    const char *const buf_emul8 = core->hw.out.buf_emul;
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

    core->hw.out.pending_emul = pending_emul;
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

    HWVoiceOut *q_voice = hwptr;
    ASSERT(q_voice->size_emul > 0);
    ASSERT(q_voice->pending_emul <= q_voice->size_emul);
    ASSERT(q_voice->pos_emul < q_voice->size_emul);

    const size_t q_read_pos = audio_ring_posb(q_voice->pos_emul,
                                              q_voice->pending_emul,
                                              q_voice->size_emul);
    ASSERT(q_read_pos < q_voice->size_emul);

    const size_t q_read_len = MIN(MIN(q_voice->pending_emul, q_voice->size_emul - q_read_pos),
                                  *frames_to_consume * q_voice->info.bytes_per_frame);
    ASSERT(!(q_read_len % q_voice->info.bytes_per_frame));

    data_to_consume->mNumberBuffers = 1;
    data_to_consume->mBuffers[0].mData = ((char*)q_voice->buf_emul) + q_read_pos;
    data_to_consume->mBuffers[0].mDataByteSize = q_read_len;
    q_voice->pending_emul -= q_read_len;
    *frames_to_consume = q_read_len / q_voice->info.bytes_per_frame;

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
                                            &ca_out_conv_proc_locked,
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

    const unsigned q_frame_size =
            core->hw.in.info.nchannels * core->hw.in.info.bytes_per_frame;
    ASSERT(q_frame_size > 0);
    const unsigned h_frame_size =
            core->hw_channels * core->hw.in.info.bytes_per_frame;
    ASSERT(h_frame_size > 0);

    size_t len = inInputData->mBuffers[0].mDataByteSize / h_frame_size * q_frame_size;
    const size_t size_emul = core->hw.in.size_emul;
    ASSERT(size_emul > 0);

    size_t pending_emul = core->hw.in.pending_emul;
    ASSERT(pending_emul <= size_emul);

    size_t pos_emul = core->hw.in.pos_emul;
    ASSERT(pos_emul < size_emul);

    size_t space_in_hw = size_emul - pending_emul;
    if (len > space_in_hw) {
        len = space_in_hw;
    }

    char *const buf_emul8 = core->hw.out.buf_emul;
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

    core->hw.in.pos_emul = pos_emul % size_emul;
    core->hw.in.pending_emul = pending_emul;
    ca_voice_unlock(core);
    return kAudioHardwareNoError;
}

typedef struct caInputConverterContext {
    const char *data;
    uint32_t available_frames;
    uint32_t num_channels : 10;
    uint32_t frame_size : 22;
} CaInputConverterContext;

static OSStatus ca_in_conv_proc_locked(AudioConverterRef converter,
                                       UInt32 *frames_to_consume,
                                       AudioBufferList *data_to_consume,
                                       AudioStreamPacketDescription **spd,
                                       void *input_context_raw)
{
    if (spd) {
        *spd = NULL;
    }

    CaInputConverterContext *input_context = input_context_raw;

    const size_t num_frames = MIN(*frames_to_consume, input_context->available_frames);

    data_to_consume->mNumberBuffers = 1;
    data_to_consume->mBuffers[0].mData = (char*)input_context->data;
    data_to_consume->mBuffers[0].mNumberChannels = input_context->num_channels;
    data_to_consume->mBuffers[0].mDataByteSize = num_frames * input_context->frame_size;
    input_context->data += data_to_consume->mBuffers[0].mDataByteSize;
    input_context->available_frames -= num_frames;
    *frames_to_consume = num_frames;
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

    HWVoiceIn *q_voice = &core->hw.in;
    const size_t q_bytes_per_frame = q_voice->info.bytes_per_frame;
    ASSERT(q_bytes_per_frame > 0);
    const size_t size_emul = q_voice->size_emul;
    ASSERT(size_emul > 0);
    size_t pending_emul = q_voice->pending_emul;
    ASSERT(pending_emul <= size_emul);
    size_t pos_emul = q_voice->pos_emul;
    ASSERT(pos_emul < size_emul);
    char* buf_emul8 = q_voice->buf_emul;
    ASSERT(buf_emul8);

    ASSERT(core->hw_channels == inInputData->mBuffers[0].mNumberChannels);
    CaInputConverterContext input_context = {
        .data = inInputData->mBuffers[0].mData,
        .available_frames = requested_size_frames,
        .num_channels = core->hw_channels,
        .frame_size = core->hw_frame_size,
    };

    while (requested_size_frames) {
        UInt32 size_frames =
                MIN(requested_size_frames,
                    MIN(size_emul - pending_emul, size_emul - pos_emul) /
                            q_bytes_per_frame);

        AudioBufferList output_data;
        output_data.mNumberBuffers = 1;
        output_data.mBuffers[0].mData = buf_emul8 + pos_emul;
        output_data.mBuffers[0].mDataByteSize = size_frames * q_bytes_per_frame;
        output_data.mBuffers[0].mNumberChannels = q_voice->info.nchannels;

        OSStatus status =
                AudioConverterFillComplexBuffer(core->converter,
                                                &ca_in_conv_proc_locked,
                                                &input_context,
                                                &size_frames,
                                                &output_data,
                                                NULL);
        if (status || !size_frames) {
            break;
        }

        const size_t q_size = size_frames * q_bytes_per_frame;
        pending_emul += q_size;
        pos_emul = (pos_emul + q_size) % size_emul;

        requested_size_frames -= size_frames;
    }

    q_voice->pending_emul = pending_emul;
    q_voice->pos_emul = pos_emul;
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
                                 AudioFormat* af)
{
    if (!(hw->mFormatFlags & kAudioFormatFlagIsPacked) ||
            (hw->mFormatFlags & kAudioFormatFlagIsNonInterleaved) ||
            (hw->mFramesPerPacket != 1) ||
            (hw->mBytesPerFrame != hw->mBytesPerPacket) ||
            (hw->mFormatID != kAudioFormatLinearPCM)) {
        return false;
    }

    if (hw->mFormatFlags & kAudioFormatFlagIsFloat) {
        switch (hw->mBitsPerChannel) {
        case sizeof(float) * CHAR_BIT:
            *af = AUDIO_FORMAT_F32;
            return true;
        default:
            return false;
        }
    } else if (hw->mFormatFlags & kAudioFormatFlagIsSignedInteger) {
        switch (hw->mBitsPerChannel) {
        case sizeof(int8_t) * CHAR_BIT:
            *af = AUDIO_FORMAT_S8;
            return true;
        case sizeof(int16_t) * CHAR_BIT:
            *af = AUDIO_FORMAT_S16;
            return true;
        case sizeof(int32_t) * CHAR_BIT:
            *af = AUDIO_FORMAT_S32;
            return true;
        default:
            return false;
        }
    } else {
        return false;
    }
}

static bool ca_is_converter_required(const AudioStreamBasicDescription *hw,
                                     const struct audio_pcm_info* sw)
{
    AudioFormat hw_af;
    return (fabs(hw->mSampleRate - sw->freq) > (sw->freq * 0.0005)) ||
           !ca_get_sample_format(hw, &hw_af) ||
           (hw_af != sw->af);
}

static AudioStreamBasicDescription
ca_to_AudioStreamBasicDescription(const size_t freq,
                                  const size_t nchannels,
                                  const AudioFormat fmt)
{
    size_t bytes_per_sample;
    AudioFormatFlags flags;

    switch (fmt) {
    case AUDIO_FORMAT_S8:
        flags = kAudioFormatFlagIsPacked | kAudioFormatFlagIsSignedInteger;
        bytes_per_sample = sizeof(int8_t);
        break;

    case AUDIO_FORMAT_S16:
        flags = kAudioFormatFlagIsPacked | kAudioFormatFlagIsSignedInteger;
        bytes_per_sample = sizeof(int16_t);
        break;

    case AUDIO_FORMAT_S32:
        flags = kAudioFormatFlagIsPacked | kAudioFormatFlagIsSignedInteger;
        bytes_per_sample = sizeof(int32_t);
        break;

    default:
        ASSERT(fmt == AUDIO_FORMAT_F32);
        flags = kAudioFormatFlagIsPacked | kAudioFormatFlagIsFloat;
        bytes_per_sample = sizeof(float);
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

static AudioFormat ca_to_safe_AudioFormat(const AudioFormat fmt)
{
    switch (fmt) {
    case AUDIO_FORMAT_S8:   /* avoid using 8bit samples */
    case AUDIO_FORMAT_U8:   /* avoid using 8bit samples */
    case AUDIO_FORMAT_S16:
    case AUDIO_FORMAT_U16:
        return AUDIO_FORMAT_S16;

    case AUDIO_FORMAT_S32:
    case AUDIO_FORMAT_U32:
        return AUDIO_FORMAT_S32;

    default:
        return AUDIO_FORMAT_F32;
    }
}

/* This value fomes from the command line. */
static size_t ca_get_periods_count(const CoreaudioVoice *core)
{
    const AudiodevCoreaudioPerDirectionOptions *cpdo;
    if (core->is_output) {
        cpdo = core->hw.out.s->dev->u.coreaudio.out;
    } else {
        cpdo = core->hw.in.s->dev->u.coreaudio.in;
    }

    return cpdo->has_buffer_count ? cpdo->buffer_count : 4;
}

static bool ca_init_voice_locked(CoreaudioVoice *core,
                                 const struct audsettings *mixeng_as)
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
        if (mixeng_as) {
            ASSERT(mixeng_as->freq > 0);
            ASSERT(mixeng_as->nchannels > 0);

            /*
             * This is a new voice, try using the sound card
             * values to avoid double conversion.
             */
            struct audsettings as = {
                /*
                 * This accomodates 5 channel guest streams on
                 * a 16 channel sound card in MacOS.
                 */
                .nchannels = MIN(mixeng_as->nchannels,
                                 hw_stream_fmt.mChannelsPerFrame),
                .big_endian = false,
            };

            if (ca_get_sample_format(&hw_stream_fmt, &as.fmt)) {
                as.freq = hw_stream_fmt.mSampleRate;
            } else {
                /*
                 * A converter will be created anyway, use
                 * the mixeng settings.
                 */
                as.freq = mixeng_as->freq;
                as.fmt = ca_to_safe_AudioFormat(mixeng_as->fmt);
            }

            audio_pcm_init_info(info, &as);
        }

        uint32_t qemu_period_size_frames = hw_period_size_frames;
        AudioConverterRef converter = NULL;
        if (ca_is_converter_required(&hw_stream_fmt, info)) {
            const AudioStreamBasicDescription sw_stream_fmt =
                    ca_to_AudioStreamBasicDescription(info->freq,
                                                      info->nchannels,
                                                      info->af);
            /* AudioConverterNew(src, dst, ) */
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

        const size_t buffer_size_samples =
                ca_get_periods_count(core) * qemu_period_size_frames;
        ASSERT(buffer_size_samples > 0);
        if (core->is_output) {
            HWVoiceOut *hw = &core->hw.out;

            ASSERT(hw->samples == 0);
            hw->samples = buffer_size_samples;
            audio_generic_initialize_buffer_out(hw);
        } else {
            HWVoiceIn *hw = &core->hw.in;

            ASSERT(hw->samples == 0);
            hw->samples = buffer_size_samples;
            hw->size_emul = hw->samples * info->bytes_per_frame;
            hw->buf_emul = g_malloc(hw->size_emul);
            hw->pos_emul = 0;
            hw->pending_emul = 0;
        }
        return true;
    }

    ca_logerr2(core->is_output, kAudioHardwareBadObjectError, "%s",
               "Ran out of retries to get the voice object");
    return false;
}

static int coreaudio_init_impl(const bool is_output,
                               CoreaudioVoice *core,
                               struct audsettings *mixeng_as)
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

    if (!ca_init_voice_locked(core, mixeng_as)) {
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

    if (core->is_output) {
        HWVoiceOut *hw = &core->hw.out;

        g_free(hw->buf_emul);
        hw->buf_emul = NULL;
        hw->size_emul = 0;
        hw->samples = 0;
    } else {
        HWVoiceIn *hw = &core->hw.in;

        g_free(hw->buf_emul);
        hw->buf_emul = NULL;
        hw->size_emul = 0;
        hw->samples = 0;
    }

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

static int coreaudio_init_out(HWVoiceOut *hw, struct audsettings *mixeng_as)
{
    return coreaudio_init_impl(true, (CoreaudioVoice *)hw, mixeng_as);
}

static void coreaudio_fini_out(HWVoiceOut *hw)
{
    coreaudio_fini_impl((CoreaudioVoice *)hw);
}

static void coreaudio_enable_out(HWVoiceOut *hw, bool enable)
{
    coreaudio_enable_impl((CoreaudioVoice *)hw, enable);
}

static int coreaudio_init_in(HWVoiceIn *hw, struct audsettings *mixeng_as)
{
    return coreaudio_init_impl(false, (CoreaudioVoice *)hw, mixeng_as);
}

static void coreaudio_fini_in(HWVoiceIn *hw)
{
    coreaudio_fini_impl((CoreaudioVoice *)hw);
}

static void coreaudio_enable_in(HWVoiceIn *hw, bool enable)
{
    coreaudio_enable_impl((CoreaudioVoice *)hw, enable);
}

static void *coreaudio_get_buffer_in(HWVoiceIn *hw, size_t *size)
{
    CoreaudioVoice *core = (CoreaudioVoice *)hw;
    ca_voice_lock(core);
    void* ret = audio_generic_get_buffer_in(hw, size);
    ca_voice_unlock(core);
    return ret;
}

static void coreaudio_put_buffer_in(HWVoiceIn *hw, void *buf, size_t size)
{
    CoreaudioVoice *core = (CoreaudioVoice *)hw;
    ca_voice_lock(core);
    audio_generic_put_buffer_in(hw, buf, size);
    ca_voice_unlock(core);
}

static void audio_coreaudio_class_init(ObjectClass *klass, const void *data)
{
    AudioMixengBackendClass *k = AUDIO_MIXENG_BACKEND_CLASS(klass);

    k->max_voices_out = 1;
    k->max_voices_in = 1;
    k->voice_size_out = sizeof(CoreaudioVoice);
    k->voice_size_in = sizeof(CoreaudioVoice);

    k->init_out = coreaudio_init_out;
    k->fini_out = coreaudio_fini_out;
    /* wrapper for audio_generic_write */
    k->write = coreaudio_write;
    /* wrapper for audio_generic_buffer_get_free */
    k->buffer_get_free = coreaudio_buffer_get_free;
    /* wrapper for audio_generic_get_buffer_out */
    k->get_buffer_out = coreaudio_get_buffer_out;
    /* wrapper for audio_generic_put_buffer_out */
    k->put_buffer_out = coreaudio_put_buffer_out;
    k->enable_out = coreaudio_enable_out;

    /*
     * The ring buffer (`hw.buf_emul`) is initialized in `init_in_device`.
     * The audio callback (`audioDeviceIOProcIn`) writes into the ring
     * buffer directly (under a lock, `buf_mutex`), therefore we don't
     * need `run_buffer_in`.
     *
     * `coreaudio_get_buffer_in` and `coreaudio_put_buffer_in` are lock
     * aware wrappers for QEMU ring buffer functions.
     *
     * The `read` call is the default QEMU function built with
     * `get_buffer_in` and `put_buffer_in`.
     */
    k->init_in = coreaudio_init_in;
    k->fini_in = coreaudio_fini_in;
    k->read    = audio_generic_read;
    k->run_buffer_in = NULL;
    k->get_buffer_in = coreaudio_get_buffer_in;
    k->put_buffer_in = coreaudio_put_buffer_in;
    k->enable_in = coreaudio_enable_in;
}

static const TypeInfo audio_types[] = {
    {
        .name = TYPE_AUDIO_COREAUDIO,
        .parent = TYPE_AUDIO_MIXENG_BACKEND,
        .instance_size = sizeof(AudioCoreaudio),
        .class_init = audio_coreaudio_class_init,
    },
};

DEFINE_TYPES(audio_types)
module_obj(TYPE_AUDIO_COREAUDIO);

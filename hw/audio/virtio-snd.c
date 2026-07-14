/*
 * VIRTIO Sound Device conforming to
 *
 * "Virtual I/O Device (VIRTIO) Version 1.2
 * Committee Specification Draft 01
 * 09 May 2022"
 *
 * <https://docs.oasis-open.org/virtio/virtio/v1.2/csd01/virtio-v1.2-csd01.html#x1-52900014>
 *
 * Copyright (c) 2023 Emmanouil Pitsidianakis <manos.pitsidianakis@linaro.org>
 * Copyright (C) 2019 OpenSynergy GmbH
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * (at your option) any later version.  See the COPYING file in the
 * top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/iov.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/lockable.h"
#include "system/runstate.h"
#include "migration/qemu-file-types.h"
#include "trace.h"
#include "qapi/error.h"
#include "hw/audio/virtio-snd.h"

#define VIRTIO_SOUND_VM_VERSION 1
#define VIRTIO_SOUND_JACK_DEFAULT 0
#define VIRTIO_SOUND_STREAM_DEFAULT 2
#define VIRTIO_SOUND_CHMAP_DEFAULT 0
#define VIRTIO_SOUND_HDA_FN_NID 0

static void virtio_snd_pcm_out_cb(void *data, int available);
static void virtio_snd_pcm_flush(VirtIOSoundPCMStream *stream);
static void virtio_snd_pcm_in_cb(void *data, int available);
static void virtio_snd_unrealize(DeviceState *dev);

static const uint32_t supported_formats = BIT(VIRTIO_SND_PCM_FMT_S8)
                                        | BIT(VIRTIO_SND_PCM_FMT_U8)
                                        | BIT(VIRTIO_SND_PCM_FMT_S16)
                                        | BIT(VIRTIO_SND_PCM_FMT_U16)
                                        | BIT(VIRTIO_SND_PCM_FMT_S32)
                                        | BIT(VIRTIO_SND_PCM_FMT_U32)
                                        | BIT(VIRTIO_SND_PCM_FMT_FLOAT);

static const uint32_t supported_rates = BIT(VIRTIO_SND_PCM_RATE_5512)
                                      | BIT(VIRTIO_SND_PCM_RATE_8000)
                                      | BIT(VIRTIO_SND_PCM_RATE_11025)
                                      | BIT(VIRTIO_SND_PCM_RATE_16000)
                                      | BIT(VIRTIO_SND_PCM_RATE_22050)
                                      | BIT(VIRTIO_SND_PCM_RATE_32000)
                                      | BIT(VIRTIO_SND_PCM_RATE_44100)
                                      | BIT(VIRTIO_SND_PCM_RATE_48000)
                                      | BIT(VIRTIO_SND_PCM_RATE_64000)
                                      | BIT(VIRTIO_SND_PCM_RATE_88200)
                                      | BIT(VIRTIO_SND_PCM_RATE_96000)
                                      | BIT(VIRTIO_SND_PCM_RATE_176400)
                                      | BIT(VIRTIO_SND_PCM_RATE_192000)
                                      | BIT(VIRTIO_SND_PCM_RATE_384000);

static const struct virtio_snd_pcm_info stream_default_info = {
    .hdr = {
        .hda_fn_nid = VIRTIO_SOUND_HDA_FN_NID,
    },
    .features = 0,
    .formats = supported_formats,
    .rates = supported_rates,
    .direction = -1,  /* overwritten in virtio_snd_create_new_stream */
    .channels_min = 1,
    .channels_max = 2,
};

static const Property virtio_snd_properties[] = {
    DEFINE_AUDIO_PROPERTIES(VirtIOSound, audio_be),
    DEFINE_PROP_UINT32("jacks", VirtIOSound, snd_conf.jacks,
                       VIRTIO_SOUND_JACK_DEFAULT),
    DEFINE_PROP_UINT32("streams", VirtIOSound, snd_conf.streams,
                       VIRTIO_SOUND_STREAM_DEFAULT),
    DEFINE_PROP_UINT32("chmaps", VirtIOSound, snd_conf.chmaps,
                       VIRTIO_SOUND_CHMAP_DEFAULT),
};

static void
virtio_snd_get_config(VirtIODevice *vdev, uint8_t *config)
{
    VirtIOSound *s = VIRTIO_SND(vdev);
    virtio_snd_config *sndconfig =
        (virtio_snd_config *)config;
    trace_virtio_snd_get_config(vdev,
                                s->snd_conf.jacks,
                                s->snd_conf.streams,
                                s->snd_conf.chmaps);

    memcpy(sndconfig, &s->snd_conf, sizeof(s->snd_conf));
    cpu_to_le32s(&sndconfig->jacks);
    cpu_to_le32s(&sndconfig->streams);
    cpu_to_le32s(&sndconfig->chmaps);

}

static void
virtio_snd_pcm_buffer_free(VirtIOSoundPCMBuffer *buffer)
{
    g_free(buffer->elem);
    g_free(buffer);
}

/*
 * Get a specific stream from the virtio sound card device.
 * Returns NULL if @stream_id is invalid or not allocated.
 *
 * @s: VirtIOSound device
 * @stream_id: stream id
 */
static VirtIOSoundPCMStream *virtio_snd_pcm_get_stream(VirtIOSound *s,
                                                       uint32_t stream_id)
{
    return stream_id >= s->snd_conf.streams ? NULL :
        s->pcm_items[stream_id].stream;
}

/*
 * Get params for a specific stream.
 *
 * @s: VirtIOSound device
 * @stream_id: stream id
 */
static VirtIOPcmParams *virtio_snd_pcm_get_params(VirtIOSound *s,
                                                  uint32_t stream_id)
{
    return stream_id >= s->snd_conf.streams ? NULL
        : &s->pcm_items[stream_id].params;
}

/*
 * Handle the VIRTIO_SND_R_PCM_INFO request.
 * The function writes the info structs to the request element.
 *
 * @s: VirtIOSound device
 * @cmd: The request command queue element from VirtIOSound cmdq field
 */
static void virtio_snd_handle_pcm_info(VirtIOSound *s,
                                       virtio_snd_ctrl_command *cmd)
{
    uint32_t stream_id, start_id, count, size, tmp;
    virtio_snd_pcm_info val;
    virtio_snd_query_info req;
    VirtIOSoundPCMStream *stream = NULL;
    g_autofree virtio_snd_pcm_info *pcm_info = NULL;
    size_t msg_sz = iov_to_buf(cmd->elem->out_sg,
                               cmd->elem->out_num,
                               0,
                               &req,
                               sizeof(virtio_snd_query_info));

    if (msg_sz != sizeof(virtio_snd_query_info)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                "%s: virtio-snd command size incorrect %zu vs \
                %zu\n", __func__, msg_sz, sizeof(virtio_snd_query_info));
        cmd->resp.code = cpu_to_le32(VIRTIO_SND_S_BAD_MSG);
        return;
    }

    start_id = le32_to_cpu(req.start_id);
    count = le32_to_cpu(req.count);
    size = le32_to_cpu(req.size);

    /*
     * 5.14.6.2 Driver Requirements: Item Information Request
     * "The driver MUST NOT set start_id and count such that start_id + count
     * is greater than the total number of particular items that is indicated
     * in the device configuration space."
     */
    if (start_id > s->snd_conf.streams
        || !g_uint_checked_add(&tmp, start_id, count)
        || start_id + count > s->snd_conf.streams) {
        error_report("pcm info: start_id + count is greater than the total "
                     "number of streams, got: start_id = %u, count = %u",
                     start_id, count);
        cmd->resp.code = cpu_to_le32(VIRTIO_SND_S_BAD_MSG);
        return;
    }

    /*
     * 5.14.6.2 Driver Requirements: Item Information Request
     * "The driver MUST provide a buffer of sizeof(struct virtio_snd_hdr) +
     * count * size bytes for the response."
     */
    if (!g_uint_checked_mul(&tmp, size, count)
        || !g_uint_checked_add(&tmp, tmp, sizeof(virtio_snd_hdr))
        || iov_size(cmd->elem->in_sg, cmd->elem->in_num) <
           sizeof(virtio_snd_hdr) + size * count) {
        error_report("pcm info: buffer too small, got: %zu, needed: %zu",
                iov_size(cmd->elem->in_sg, cmd->elem->in_num),
                sizeof(virtio_snd_pcm_info) * count);
        cmd->resp.code = cpu_to_le32(VIRTIO_SND_S_BAD_MSG);
        return;
    }

    pcm_info = g_new0(virtio_snd_pcm_info, count);
    for (uint32_t i = 0; i < count; i++) {
        stream_id = i + start_id;
        trace_virtio_snd_handle_pcm_info(stream_id);
        stream = virtio_snd_pcm_get_stream(s, stream_id);
        if (!stream) {
            error_report("Invalid stream id: %"PRIu32, stream_id);
            cmd->resp.code = cpu_to_le32(VIRTIO_SND_S_BAD_MSG);
            return;
        }
        val = stream_default_info;
        val.direction = stream->is_output ?
                VIRTIO_SND_D_OUTPUT : VIRTIO_SND_D_INPUT;
        /*
         * 5.14.6.6.2.1 Device Requirements: Stream Information The device MUST
         * NOT set undefined feature, format, rate and direction values. The
         * device MUST initialize the padding bytes to 0.
         */
        pcm_info[i] = val;
        memset(&pcm_info[i].padding, 0, 5);
    }

    cmd->payload_size = sizeof(virtio_snd_pcm_info) * count;
    cmd->resp.code = cpu_to_le32(VIRTIO_SND_S_OK);
    iov_from_buf(cmd->elem->in_sg,
                 cmd->elem->in_num,
                 sizeof(virtio_snd_hdr),
                 pcm_info,
                 cmd->payload_size);
}

/*
 * Get a QEMU Audiosystem compatible format value from a VIRTIO_SND_PCM_FMT_*
 */
static int virtio_snd_get_qemu_format(AudioFormat *dst, uint32_t format)
{
    #define CASE(FMT)               \
    case VIRTIO_SND_PCM_FMT_##FMT:  \
        *dst = AUDIO_FORMAT_##FMT;  \
        return 0;                   \

    switch (format) {
    CASE(U8)
    CASE(S8)
    CASE(U16)
    CASE(S16)
    CASE(U32)
    CASE(S32)
    case VIRTIO_SND_PCM_FMT_FLOAT:
        *dst = AUDIO_FORMAT_F32;
        return 0;

    default:
        return -1;
    }

    #undef CASE
}

/*
 * Get a QEMU Audiosystem compatible frequency value from a
 * VIRTIO_SND_PCM_RATE_*
 */
static int32_t virtio_snd_get_qemu_freq(uint32_t rate)
{
    #define CASE(RATE)               \
    case VIRTIO_SND_PCM_RATE_##RATE: \
        return RATE;

    switch (rate) {
    CASE(5512)
    CASE(8000)
    CASE(11025)
    CASE(16000)
    CASE(22050)
    CASE(32000)
    CASE(44100)
    CASE(48000)
    CASE(64000)
    CASE(88200)
    CASE(96000)
    CASE(176400)
    CASE(192000)
    CASE(384000)
    default:
        return -1;
    }

    #undef CASE
}

/*
 * Get QEMU Audiosystem compatible audsettings from virtio based pcm stream
 * params.
 */
static int virtio_snd_get_qemu_audsettings(audsettings *as,
                                           uint8_t channels,
                                           uint8_t format,
                                           uint8_t rate)
{
    int freq;

    if ((channels < 1) || (channels > AUDIO_MAX_CHANNELS)) {
        error_report("Invalid number of channels: %"PRIu8, channels);
        return -1;
    }
    as->nchannels = channels;

    if (virtio_snd_get_qemu_format(&as->fmt, format)) {
        error_report("Unsupported format: %"PRIu8, format);
        return -1;
    }

    freq = virtio_snd_get_qemu_freq(rate);
    if (freq <= 0) {
        error_report("Unsupported rate: %"PRIu8, rate);
        return -1;
    }
    as->freq = freq;

    as->big_endian = false; /* Conforming to VIRTIO 1.0: always little endian. */
    return 0;
}

/*
 * Set the given stream params.
 * Called by both virtio_snd_handle_pcm_set_params and during device
 * initialization.
 * Returns the response status code. (VIRTIO_SND_S_*).
 *
 * @s: VirtIOSound device
 * @params: The PCM params as defined in the virtio specification
 */
static
uint32_t virtio_snd_set_pcm_params(VirtIOSound *s,
                                   uint32_t stream_id,
                                   virtio_snd_pcm_set_params *params)
{
    audsettings as_ignored;
    VirtIOPcmParams *st_params;

    if (stream_id >= s->snd_conf.streams || s->pcm_items == NULL) {
        virtio_error(VIRTIO_DEVICE(s), "Streams have not been initialized.\n");
        return cpu_to_le32(VIRTIO_SND_S_BAD_MSG);
    }

    if (virtio_snd_get_qemu_audsettings(
            &as_ignored, params->channels, params->format, params->rate)) {
        return cpu_to_le32(VIRTIO_SND_S_NOT_SUPP);
    }

    st_params = virtio_snd_pcm_get_params(s, stream_id);

    st_params->buffer_bytes = le32_to_cpu(params->buffer_bytes);
    st_params->period_bytes = le32_to_cpu(params->period_bytes);
    st_params->features = le32_to_cpu(params->features);
    /* the following are uint8_t, so there's no need to bswap the values. */
    st_params->channels = params->channels;
    st_params->format = params->format;
    st_params->rate = params->rate;

    return cpu_to_le32(VIRTIO_SND_S_OK);
}

/*
 * Handles the VIRTIO_SND_R_PCM_SET_PARAMS request.
 *
 * @s: VirtIOSound device
 * @cmd: The request command queue element from VirtIOSound cmdq field
 */
static void virtio_snd_handle_pcm_set_params(VirtIOSound *s,
                                             virtio_snd_ctrl_command *cmd)
{
    virtio_snd_pcm_set_params req = { 0 };
    uint32_t stream_id;
    size_t msg_sz = iov_to_buf(cmd->elem->out_sg,
                               cmd->elem->out_num,
                               0,
                               &req,
                               sizeof(virtio_snd_pcm_set_params));

    if (msg_sz != sizeof(virtio_snd_pcm_set_params)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                "%s: virtio-snd command size incorrect %zu vs \
                %zu\n", __func__, msg_sz, sizeof(virtio_snd_pcm_set_params));
        cmd->resp.code = cpu_to_le32(VIRTIO_SND_S_BAD_MSG);
        return;
    }
    stream_id = le32_to_cpu(req.hdr.stream_id);
    trace_virtio_snd_handle_pcm_set_params(stream_id);
    cmd->resp.code = virtio_snd_set_pcm_params(s, stream_id, &req);
}

/*
 * Creates a new blank stream with a reference to VirtIOSound.
 *
 * NOTE: the id field is zero and must be set outside.
 *
 * @s: VirtIOSound *s
 */
static VirtIOSoundPCMStream *virtio_snd_create_new_stream(VirtIOSound *s,
                                                          uint32_t id,
                                                          bool is_output)
{
    VirtIOSoundPCMStream *stream = g_new0(VirtIOSoundPCMStream, 1);
    if (!stream) {
        return NULL;
    }

    stream->s = s;
    stream->id = id;
    stream->is_output = is_output;

    stream->active = false;
    qemu_mutex_init(&stream->mtx);
    QSIMPLEQ_INIT(&stream->queue);

    return stream;
}

/*
 * Initializes the voice field in VirtIOSoundPCMStream and sets
 * the VirtIOPcmParams in the channel.
 *
 * @stream: VirtIOSoundPCMStream *stream
 * @const VirtIOPcmParams *params
 */
static int virtio_snd_init_stream_voice_locked(VirtIOSoundPCMStream *stream,
                                               const VirtIOPcmParams *params)
{
    audsettings as;

    g_assert(stream);
    g_assert(params);
    g_assert(stream->s);
    g_assert(stream->s->audio_be);

    if (virtio_snd_get_qemu_audsettings(
            &as, params->channels, params->format, params->rate)) {
        return -1;
    }

    if (stream->is_output) {
        stream->voice.out = audio_be_open_out(stream->s->audio_be,
                                         stream->voice.out,
                                         "virtio-sound.out",
                                         stream,
                                         virtio_snd_pcm_out_cb,
                                         &as);
        if (!stream->voice.out) {
            return -1;
        }

        audio_be_set_volume_out_lr(stream->s->audio_be, stream->voice.out, 0, 255, 255);
    } else {
        stream->voice.in = audio_be_open_in(stream->s->audio_be,
                                        stream->voice.in,
                                        "virtio-sound.in",
                                        stream,
                                        virtio_snd_pcm_in_cb,
                                        &as);
        if (!stream->voice.in) {
            return -1;
        }

        audio_be_set_volume_in_lr(stream->s->audio_be, stream->voice.in, 0, 255, 255);
    }

    stream->effective_params = *params;
    return 0;
}

/*
 * Closes the voice field in VirtIOSoundPCMStream and clears
 * `stream->as.nchannels` to mark that there is no voice on
 * the channel (e.g. when a stream is loaded from a snapshot).
 *
 * @stream: VirtIOSoundPCMStream *stream
 */
static void virtio_snd_close_stream_voice(VirtIOSoundPCMStream *stream) {
    g_assert(stream);

    if (stream->voice.raw) {
        g_assert(stream->effective_params.channels);

        if (stream->is_output) {
            audio_be_close_out(stream->s->audio_be, stream->voice.out);
        } else {
            audio_be_close_in(stream->s->audio_be, stream->voice.in);
        }

        stream->voice.raw = NULL;
    }

    stream->effective_params.channels = 0;
}

/*
 * Releases all resources owned by VirtIOSoundPCMStream and
 * frees its memory. The stream pointer must be non-NULL.
 *
 * @stream: VirtIOSoundPCMStream *stream
 */
static void virtio_snd_destroy_stream(VirtIOSoundPCMStream *stream)
{
    g_assert(stream);

    virtio_snd_pcm_flush(stream);
    virtio_snd_close_stream_voice(stream);

    qemu_mutex_destroy(&stream->mtx);
    g_free(stream);
}

/*
 * Prepares a VirtIOSound card stream.
 * Returns the response status code. (VIRTIO_SND_S_*).
 *
 * @s: VirtIOSound device
 * @stream_id: stream id
 */
static uint32_t virtio_snd_pcm_prepare(VirtIOSound *s, uint32_t stream_id)
{
    VirtIOPcmParams *params;
    VirtIOSoundPCMStream *stream;

    if (s->pcm_items == NULL ||
        stream_id >= s->snd_conf.streams) {
        return cpu_to_le32(VIRTIO_SND_S_BAD_MSG);
    }

    params = virtio_snd_pcm_get_params(s, stream_id);
    if (params == NULL) {
        return cpu_to_le32(VIRTIO_SND_S_BAD_MSG);
    }

    stream = virtio_snd_pcm_get_stream(s, stream_id);
    if (stream == NULL) {
        return cpu_to_le32(VIRTIO_SND_S_BAD_MSG);
    }

    WITH_QEMU_LOCK_GUARD(&stream->mtx) {
        if (virtio_snd_init_stream_voice_locked(stream, params)) {
            return cpu_to_le32(VIRTIO_SND_S_BAD_MSG);
        }
    }

    return cpu_to_le32(VIRTIO_SND_S_OK);
}

static const char *print_code(uint32_t code)
{
    #define CASE(CODE)            \
    case VIRTIO_SND_R_##CODE:     \
        return "VIRTIO_SND_R_"#CODE

    switch (code) {
    CASE(JACK_INFO);
    CASE(JACK_REMAP);
    CASE(PCM_INFO);
    CASE(PCM_SET_PARAMS);
    CASE(PCM_PREPARE);
    CASE(PCM_RELEASE);
    CASE(PCM_START);
    CASE(PCM_STOP);
    CASE(CHMAP_INFO);
    default:
        return "invalid code";
    }

    #undef CASE
};

/*
 * Handles VIRTIO_SND_R_PCM_PREPARE.
 *
 * @s: VirtIOSound device
 * @cmd: The request command queue element from VirtIOSound cmdq field
 */
static void virtio_snd_handle_pcm_prepare(VirtIOSound *s,
                                          virtio_snd_ctrl_command *cmd)
{
    uint32_t stream_id;
    size_t msg_sz = iov_to_buf(cmd->elem->out_sg,
                               cmd->elem->out_num,
                               sizeof(virtio_snd_hdr),
                               &stream_id,
                               sizeof(stream_id));

    stream_id = le32_to_cpu(stream_id);
    cmd->resp.code = msg_sz == sizeof(stream_id)
                   ? virtio_snd_pcm_prepare(s, stream_id)
                   : cpu_to_le32(VIRTIO_SND_S_BAD_MSG);
}

/*
 * Handles VIRTIO_SND_R_PCM_START.
 *
 * @s: VirtIOSound device
 * @cmd: The request command queue element from VirtIOSound cmdq field
 * @start: whether to start or stop the device
 */
static void virtio_snd_handle_pcm_start_stop(VirtIOSound *s,
                                             virtio_snd_ctrl_command *cmd,
                                             bool start)
{
    VirtIOSoundPCMStream *stream;
    virtio_snd_pcm_hdr req;
    uint32_t stream_id;
    size_t msg_sz = iov_to_buf(cmd->elem->out_sg,
                               cmd->elem->out_num,
                               0,
                               &req,
                               sizeof(virtio_snd_pcm_hdr));

    if (msg_sz != sizeof(virtio_snd_pcm_hdr)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                "%s: virtio-snd command size incorrect %zu vs \
                %zu\n", __func__, msg_sz, sizeof(virtio_snd_pcm_hdr));
        cmd->resp.code = cpu_to_le32(VIRTIO_SND_S_BAD_MSG);
        return;
    }

    stream_id = le32_to_cpu(req.stream_id);
    cmd->resp.code = cpu_to_le32(VIRTIO_SND_S_OK);
    trace_virtio_snd_handle_pcm_start_stop(start ? "VIRTIO_SND_R_PCM_START" :
            "VIRTIO_SND_R_PCM_STOP", stream_id);

    stream = virtio_snd_pcm_get_stream(s, stream_id);
    if (!stream) {
        error_report("Invalid stream id: %"PRIu32, stream_id);
        cmd->resp.code = cpu_to_le32(VIRTIO_SND_S_BAD_MSG);
        return;
    }

    WITH_QEMU_LOCK_GUARD(&stream->mtx) {
        if (stream->is_output) {
            audio_be_set_active_out(s->audio_be, stream->voice.out, start);
        } else {
            audio_be_set_active_in(s->audio_be, stream->voice.in, start);
        }

        stream->active = start;
    }
}

/*
 * Returns the number of I/O messages that are being processed.
 *
 * @stream: VirtIOSoundPCMStream
 */
static size_t virtio_snd_pcm_get_io_msgs_count(VirtIOSoundPCMStream *stream)
{
    VirtIOSoundPCMBuffer *buffer, *next;
    size_t count = 0;

    WITH_QEMU_LOCK_GUARD(&stream->mtx) {
        QSIMPLEQ_FOREACH_SAFE(buffer, &stream->queue, entry, next) {
            count += 1;
        }
    }
    return count;
}

/*
 * Handles VIRTIO_SND_R_PCM_RELEASE.
 *
 * @s: VirtIOSound device
 * @cmd: The request command queue element from VirtIOSound cmdq field
 */
static void virtio_snd_handle_pcm_release(VirtIOSound *s,
                                          virtio_snd_ctrl_command *cmd)
{
    uint32_t stream_id;
    VirtIOSoundPCMStream *stream;
    size_t msg_sz = iov_to_buf(cmd->elem->out_sg,
                               cmd->elem->out_num,
                               sizeof(virtio_snd_hdr),
                               &stream_id,
                               sizeof(stream_id));

    if (msg_sz != sizeof(stream_id)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                "%s: virtio-snd command size incorrect %zu vs \
                %zu\n", __func__, msg_sz, sizeof(stream_id));
        cmd->resp.code = cpu_to_le32(VIRTIO_SND_S_BAD_MSG);
        return;
    }

    stream_id = le32_to_cpu(stream_id);
    trace_virtio_snd_handle_pcm_release(stream_id);
    stream = virtio_snd_pcm_get_stream(s, stream_id);
    if (stream == NULL) {
        error_report("already released stream %"PRIu32, stream_id);
        virtio_error(VIRTIO_DEVICE(s),
                     "already released stream %"PRIu32,
                     stream_id);
        cmd->resp.code = cpu_to_le32(VIRTIO_SND_S_BAD_MSG);
        return;
    }

    if (virtio_snd_pcm_get_io_msgs_count(stream)) {
        /*
         * virtio-v1.2-csd01, 5.14.6.6.5.1,
         * Device Requirements: Stream Release
         *
         * - The device MUST complete all pending I/O messages for the
         *   specified stream ID.
         * - The device MUST NOT complete the control request while there
         *   are pending I/O messages for the specified stream ID.
         */
        trace_virtio_snd_pcm_stream_flush(stream_id);
        virtio_snd_pcm_flush(stream);
    }

    cmd->resp.code = cpu_to_le32(VIRTIO_SND_S_OK);
}

/*
 * Processes the ctrl queue events.
 *
 * @s: VirtIOSound device
 * @cmd: control command request
 */
static inline void
process_cmd(VirtIOSound *s, virtio_snd_ctrl_command *cmd, VirtQueue *vq)
{
    uint32_t code;
    size_t msg_sz = iov_to_buf(cmd->elem->out_sg,
                               cmd->elem->out_num,
                               0,
                               &cmd->ctrl,
                               sizeof(virtio_snd_hdr));

    if (msg_sz != sizeof(virtio_snd_hdr)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                "%s: virtio-snd command size incorrect %zu vs \
                %zu\n", __func__, msg_sz, sizeof(virtio_snd_hdr));
        return;
    }

    code = le32_to_cpu(cmd->ctrl.code);

    trace_virtio_snd_handle_code(code, print_code(code));

    switch (code) {
    case VIRTIO_SND_R_JACK_INFO:
    case VIRTIO_SND_R_JACK_REMAP:
        qemu_log_mask(LOG_UNIMP,
                     "virtio_snd: jack functionality is unimplemented.\n");
        cmd->resp.code = cpu_to_le32(VIRTIO_SND_S_NOT_SUPP);
        break;
    case VIRTIO_SND_R_PCM_INFO:
        virtio_snd_handle_pcm_info(s, cmd);
        break;
    case VIRTIO_SND_R_PCM_START:
        virtio_snd_handle_pcm_start_stop(s, cmd, true);
        break;
    case VIRTIO_SND_R_PCM_STOP:
        virtio_snd_handle_pcm_start_stop(s, cmd, false);
        break;
    case VIRTIO_SND_R_PCM_SET_PARAMS:
        virtio_snd_handle_pcm_set_params(s, cmd);
        break;
    case VIRTIO_SND_R_PCM_PREPARE:
        virtio_snd_handle_pcm_prepare(s, cmd);
        break;
    case VIRTIO_SND_R_PCM_RELEASE:
        virtio_snd_handle_pcm_release(s, cmd);
        break;
    case VIRTIO_SND_R_CHMAP_INFO:
        qemu_log_mask(LOG_UNIMP,
                     "virtio_snd: chmap info functionality is unimplemented.\n");
        trace_virtio_snd_handle_chmap_info();
        cmd->resp.code = cpu_to_le32(VIRTIO_SND_S_NOT_SUPP);
        break;
    default:
        /* error */
        error_report("virtio snd header not recognized: %"PRIu32, code);
        cmd->resp.code = cpu_to_le32(VIRTIO_SND_S_BAD_MSG);
    }

    iov_from_buf(cmd->elem->in_sg,
                 cmd->elem->in_num,
                 0,
                 &cmd->resp,
                 sizeof(virtio_snd_hdr));
    virtqueue_push(vq, cmd->elem,
                   sizeof(virtio_snd_hdr) + cmd->payload_size);
}

/*
 * The control message handler.
 *
 * @vdev: VirtIOSound device
 * @vq: Control virtqueue
 */
static void virtio_snd_handle_ctrl(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOSound *s = VIRTIO_SND(vdev);
    VirtQueueElement *elem;
    virtio_snd_ctrl_command cmd;

    trace_virtio_snd_handle_ctrl(vdev, vq);

    if (!virtio_queue_ready(vq)) {
        return;
    }

    elem = virtqueue_pop(vq, sizeof(VirtQueueElement));
    while (elem) {
        memset(&cmd, 0, sizeof(cmd));
        cmd.elem = elem;
        cmd.resp.code = cpu_to_le32(VIRTIO_SND_S_OK);

        process_cmd(s, &cmd, vq);
        g_free(elem);

        elem = virtqueue_pop(vq, sizeof(VirtQueueElement));
    }

    virtio_notify(vdev, vq);
}

/*
 * The event virtqueue handler.
 * Not implemented yet.
 *
 * @vdev: VirtIOSound device
 * @vq: event vq
 */
static void virtio_snd_handle_event(VirtIODevice *vdev, VirtQueue *vq)
{
    qemu_log_mask(LOG_UNIMP, "virtio_snd: event queue is unimplemented.\n");
    trace_virtio_snd_handle_event();
}

/*
 * Must only be called if `invalid` is not empty.
 */
static inline void empty_invalid_queue(VirtIODevice *vdev, VirtQueue *vq,
                                       VirtIOSoundPCMBufferQueue *invalid)
{
    VirtIOSoundPCMBuffer *buffer = NULL;
    virtio_snd_pcm_status resp = { 0 };

    g_assert(!QSIMPLEQ_EMPTY(invalid));

    while (!QSIMPLEQ_EMPTY(invalid)) {
        buffer = QSIMPLEQ_FIRST(invalid);
        /* If buffer->vq != vq, our logic is fundamentally wrong, so bail out */
        g_assert(buffer->vq == vq);

        resp.status = cpu_to_le32(VIRTIO_SND_S_BAD_MSG);
        iov_from_buf(buffer->elem->in_sg,
                     buffer->elem->in_num,
                     0,
                     &resp,
                     sizeof(virtio_snd_pcm_status));
        virtqueue_push(vq,
                       buffer->elem,
                       sizeof(virtio_snd_pcm_status));
        QSIMPLEQ_REMOVE_HEAD(invalid, entry);
        virtio_snd_pcm_buffer_free(buffer);
    }
    /* Notify vq about virtio_snd_pcm_status responses. */
    virtio_notify(vdev, vq);
}

/*
 * The tx virtqueue handler. Makes the buffers available to their respective
 * streams for consumption.
 *
 * @vdev: VirtIOSound device
 * @vq: tx virtqueue
 */
static void virtio_snd_handle_tx_xfer(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOSound *vsnd = VIRTIO_SND(vdev);
    VirtIOSoundPCMBuffer *buffer;
    VirtQueueElement *elem;
    size_t msg_sz, size;
    virtio_snd_pcm_xfer hdr;
    uint32_t stream_id;
    /*
     * If any of the I/O messages are invalid, put them in `invalid` and
     * return them after the for loop.
     */
    VirtIOSoundPCMBufferQueue invalid;
    QSIMPLEQ_INIT(&invalid);

    if (!virtio_queue_ready(vq)) {
        return;
    }
    trace_virtio_snd_handle_tx_xfer();

    for (;;) {
        VirtIOSoundPCMStream *stream;

        elem = virtqueue_pop(vq, sizeof(VirtQueueElement));
        if (!elem) {
            break;
        }
        /* get the message hdr object */
        msg_sz = iov_to_buf(elem->out_sg,
                            elem->out_num,
                            0,
                            &hdr,
                            sizeof(virtio_snd_pcm_xfer));
        if (msg_sz != sizeof(virtio_snd_pcm_xfer)) {
            goto tx_err;
        }
        stream_id = le32_to_cpu(hdr.stream_id);

        if (stream_id >= vsnd->snd_conf.streams
            || vsnd->pcm_items[stream_id].stream == NULL) {
            goto tx_err;
        }

        stream = vsnd->pcm_items[stream_id].stream;
        if (!stream || !stream->is_output) {
            goto tx_err;
        }

        size = iov_size(elem->out_sg, elem->out_num) - msg_sz;
        buffer = g_malloc0(sizeof(VirtIOSoundPCMBuffer) + size);
        buffer->elem = elem;
        buffer->populated = false;
        buffer->vq = vq;
        buffer->size = size;
        buffer->offset = 0;

        WITH_QEMU_LOCK_GUARD(&stream->mtx) {
            stream->latency_bytes += size;
            QSIMPLEQ_INSERT_TAIL(&stream->queue, buffer, entry);
        }
        continue;

tx_err:
        buffer = g_malloc0(sizeof(VirtIOSoundPCMBuffer));
        buffer->elem = elem;
        buffer->vq = vq;
        QSIMPLEQ_INSERT_TAIL(&invalid, buffer, entry);
    }

    if (!QSIMPLEQ_EMPTY(&invalid)) {
        empty_invalid_queue(vdev, vq, &invalid);
    }
}

/*
 * The rx virtqueue handler. Makes the buffers available to their respective
 * streams for consumption.
 *
 * @vdev: VirtIOSound device
 * @vq: rx virtqueue
 */
static void virtio_snd_handle_rx_xfer(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOSound *vsnd = VIRTIO_SND(vdev);
    VirtIOSoundPCMBuffer *buffer;
    VirtQueueElement *elem;
    size_t msg_sz, size;
    virtio_snd_pcm_xfer hdr;
    uint32_t stream_id;
    /*
     * If any of the I/O messages are invalid, put them in `invalid` and
     * return them after the for loop.
     */
    VirtIOSoundPCMBufferQueue invalid;
    QSIMPLEQ_INIT(&invalid);

    if (!virtio_queue_ready(vq)) {
        return;
    }
    trace_virtio_snd_handle_rx_xfer();

    for (;;) {
        VirtIOSoundPCMStream *stream;

        elem = virtqueue_pop(vq, sizeof(VirtQueueElement));
        if (!elem) {
            break;
        }
        /* get the message hdr object */
        msg_sz = iov_to_buf(elem->out_sg,
                            elem->out_num,
                            0,
                            &hdr,
                            sizeof(virtio_snd_pcm_xfer));
        if (msg_sz != sizeof(virtio_snd_pcm_xfer)) {
            goto rx_err;
        }
        stream_id = le32_to_cpu(hdr.stream_id);

        if (stream_id >= vsnd->snd_conf.streams
            || !vsnd->pcm_items[stream_id].stream) {
            goto rx_err;
        }

        stream = vsnd->pcm_items[stream_id].stream;
        if (!stream || stream->is_output) {
            goto rx_err;
        }

        size = iov_size(elem->in_sg, elem->in_num) -
            sizeof(virtio_snd_pcm_status);
        buffer = g_malloc0(sizeof(VirtIOSoundPCMBuffer) + size);
        buffer->elem = elem;
        buffer->vq = vq;
        buffer->size = 0;
        buffer->offset = 0;

        WITH_QEMU_LOCK_GUARD(&stream->mtx) {
            QSIMPLEQ_INSERT_TAIL(&stream->queue, buffer, entry);
        }
        continue;

rx_err:
        buffer = g_malloc0(sizeof(VirtIOSoundPCMBuffer));
        buffer->elem = elem;
        buffer->vq = vq;
        QSIMPLEQ_INSERT_TAIL(&invalid, buffer, entry);
    }

    if (!QSIMPLEQ_EMPTY(&invalid)) {
        empty_invalid_queue(vdev, vq, &invalid);
    }
}

static uint64_t get_features(VirtIODevice *vdev, uint64_t features,
                             Error **errp)
{
    /*
     * virtio-v1.2-csd01, 5.14.3,
     * Feature Bits
     * None currently defined.
     */
    VirtIOSound *s = VIRTIO_SND(vdev);
    features |= s->features;

    trace_virtio_snd_get_features(vdev, features);

    return features;
}

static void
virtio_snd_vm_state_change(void *opaque, bool running,
                                       RunState state)
{
    if (running) {
        trace_virtio_snd_vm_state_running();
    } else {
        trace_virtio_snd_vm_state_stopped();
    }
}

static void virtio_snd_realize(DeviceState *dev, Error **errp)
{
    ERRP_GUARD();
    VirtIOSound *vsnd = VIRTIO_SND(dev);
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    virtio_snd_pcm_set_params default_params = { 0 };
    uint32_t status;
    uint32_t num_output_streams;

    trace_virtio_snd_realize(vsnd);

    /* check number of jacks and streams */
    if (vsnd->snd_conf.jacks > 8) {
        error_setg(errp,
                   "Invalid number of jacks: %"PRIu32,
                   vsnd->snd_conf.jacks);
        return;
    }
    if (vsnd->snd_conf.streams < 1 || vsnd->snd_conf.streams > 10) {
        error_setg(errp,
                   "Invalid number of streams: %"PRIu32,
                    vsnd->snd_conf.streams);
        return;
    }

    if (vsnd->snd_conf.chmaps > VIRTIO_SND_CHMAP_MAX_SIZE) {
        error_setg(errp,
                   "Invalid number of channel maps: %"PRIu32,
                   vsnd->snd_conf.chmaps);
        return;
    }

    if (!audio_be_check(&vsnd->audio_be, errp)) {
        return;
    }

    vsnd->vmstate =
        qemu_add_vm_change_state_handler(virtio_snd_vm_state_change, vsnd);

    vsnd->pcm_items =
        g_new0(VirtIOSoundPCMItem, vsnd->snd_conf.streams);

    virtio_init(vdev, VIRTIO_ID_SOUND, sizeof(virtio_snd_config));
    virtio_add_feature(&vsnd->features, VIRTIO_F_VERSION_1);

    vsnd->queues[VIRTIO_SND_VQ_CONTROL] =
        virtio_add_queue(vdev, 64, virtio_snd_handle_ctrl);
    vsnd->queues[VIRTIO_SND_VQ_EVENT] =
        virtio_add_queue(vdev, 64, virtio_snd_handle_event);
    vsnd->queues[VIRTIO_SND_VQ_TX] =
        virtio_add_queue(vdev, 64, virtio_snd_handle_tx_xfer);
    vsnd->queues[VIRTIO_SND_VQ_RX] =
        virtio_add_queue(vdev, 64, virtio_snd_handle_rx_xfer);

    /* set default params for all streams */
    default_params.features = stream_default_info.features;
    default_params.buffer_bytes = cpu_to_le32(8192);
    default_params.period_bytes = cpu_to_le32(2048);
    default_params.channels = stream_default_info.channels_min;
    default_params.format = VIRTIO_SND_PCM_FMT_S16;
    default_params.rate = VIRTIO_SND_PCM_RATE_48000;

    num_output_streams = (vsnd->snd_conf.streams + 1U) / 2;

    for (uint32_t i = 0; i < vsnd->snd_conf.streams; i++) {
        VirtIOSoundPCMStream* stream;

        status = virtio_snd_set_pcm_params(vsnd, i, &default_params);
        if (status != cpu_to_le32(VIRTIO_SND_S_OK)) {
            error_setg(errp,
                       "Can't initialize stream params, device responded with %s.",
                       print_code(status));
            goto error_cleanup;
        }

        vsnd->pcm_items[i].stream = virtio_snd_create_new_stream(
            vsnd, i, (i < num_output_streams));
    }

    return;

error_cleanup:
    virtio_snd_unrealize(dev);
}

static inline void update_latency(VirtIOSoundPCMStream *s, size_t used)
{
    s->latency_bytes = s->latency_bytes > used ?
                       s->latency_bytes - used : 0;
}

static inline void return_tx_buffer(VirtIOSoundPCMStream *stream,
                                    VirtIOSoundPCMBuffer *buffer)
{
    virtio_snd_pcm_status resp = { 0 };
    resp.status = cpu_to_le32(VIRTIO_SND_S_OK);
    update_latency(stream, buffer->size);
    resp.latency_bytes = cpu_to_le32(stream->latency_bytes);
    iov_from_buf(buffer->elem->in_sg,
                 buffer->elem->in_num,
                 0,
                 &resp,
                 sizeof(virtio_snd_pcm_status));
    virtqueue_push(buffer->vq,
                   buffer->elem,
                   sizeof(virtio_snd_pcm_status));
    virtio_notify(VIRTIO_DEVICE(stream->s), buffer->vq);
    QSIMPLEQ_REMOVE(&stream->queue,
                    buffer,
                    VirtIOSoundPCMBuffer,
                    entry);
    virtio_snd_pcm_buffer_free(buffer);
}

/*
 * audio_be_* output callback.
 *
 * @data: VirtIOSoundPCMStream stream
 * @available: number of bytes that can be written with audio_be_write()
 */
static void virtio_snd_pcm_out_cb(void *data, int available)
{
    VirtIOSoundPCMStream *stream = data;
    VirtIOSoundPCMBuffer *buffer;
    size_t size;

    WITH_QEMU_LOCK_GUARD(&stream->mtx) {
        while (!QSIMPLEQ_EMPTY(&stream->queue)) {
            buffer = QSIMPLEQ_FIRST(&stream->queue);
            if (!virtio_queue_ready(buffer->vq)) {
                return;
            }
            if (!stream->active) {
                /* Stream has stopped, so do not perform audio_be_write. */
                return_tx_buffer(stream, buffer);
                continue;
            }
            if (!buffer->populated) {
                iov_to_buf(buffer->elem->out_sg,
                           buffer->elem->out_num,
                           sizeof(virtio_snd_pcm_xfer),
                           buffer->data,
                           buffer->size);
                buffer->populated = true;
            }
            for (;;) {
                size = audio_be_write(stream->s->audio_be,
                                 stream->voice.out,
                                 buffer->data + buffer->offset,
                                 MIN(buffer->size, available));
                assert(size <= MIN(buffer->size, available));
                if (size == 0) {
                    /* break out of both loops */
                    available = 0;
                    break;
                }
                buffer->size -= size;
                buffer->offset += size;
                available -= size;
                update_latency(stream, size);
                if (buffer->size < 1) {
                    return_tx_buffer(stream, buffer);
                    break;
                }
                if (!available) {
                    break;
                }
            }
            if (!available) {
                break;
            }
        }
    }
}

/*
 * Flush all buffer data from this input stream's queue into the driver's
 * virtual queue.
 *
 * @stream: VirtIOSoundPCMStream *stream
 */
static inline void return_rx_buffer(VirtIOSoundPCMStream *stream,
                                    VirtIOSoundPCMBuffer *buffer)
{
    virtio_snd_pcm_status resp = { 0 };
    resp.status = cpu_to_le32(VIRTIO_SND_S_OK);
    resp.latency_bytes = 0;
    /* Copy data -if any- to guest */
    iov_from_buf(buffer->elem->in_sg,
                 buffer->elem->in_num,
                 0,
                 buffer->data,
                 buffer->size);
    iov_from_buf(buffer->elem->in_sg,
                 buffer->elem->in_num,
                 buffer->size,
                 &resp,
                 sizeof(virtio_snd_pcm_status));
    virtqueue_push(buffer->vq,
                   buffer->elem,
                   sizeof(virtio_snd_pcm_status) + buffer->size);
    virtio_notify(VIRTIO_DEVICE(stream->s), buffer->vq);
    QSIMPLEQ_REMOVE(&stream->queue,
                    buffer,
                    VirtIOSoundPCMBuffer,
                    entry);
    virtio_snd_pcm_buffer_free(buffer);
}


/*
 * audio_be_* input callback.
 *
 * @data: VirtIOSoundPCMStream stream
 * @available: number of bytes that can be read with audio_be_read()
 */
static void virtio_snd_pcm_in_cb(void *data, int available)
{
    VirtIOSoundPCMStream *stream = data;
    VirtIOSoundPCMBuffer *buffer;
    size_t size, max_size, to_read;

    WITH_QEMU_LOCK_GUARD(&stream->mtx) {
        const size_t period_bytes = stream->effective_params.period_bytes;

        while (!QSIMPLEQ_EMPTY(&stream->queue)) {
            buffer = QSIMPLEQ_FIRST(&stream->queue);
            if (!virtio_queue_ready(buffer->vq)) {
                return;
            }
            if (!stream->active) {
                /* Stream has stopped, so do not perform audio_be_read. */
                return_rx_buffer(stream, buffer);
                continue;
            }

            max_size = iov_size(buffer->elem->in_sg, buffer->elem->in_num);
            if (max_size <= sizeof(virtio_snd_pcm_status)) {
                return_rx_buffer(stream, buffer);
                continue;
            }
            max_size -= sizeof(virtio_snd_pcm_status);

            for (;;) {
                if (buffer->size >= max_size) {
                    return_rx_buffer(stream, buffer);
                    break;
                }
                to_read = period_bytes - buffer->size;
                to_read = MIN(to_read, available);
                to_read = MIN(to_read, max_size - buffer->size);
                size = audio_be_read(stream->s->audio_be,
                                     stream->voice.in,
                                     buffer->data + buffer->size,
                                     to_read);
                if (!size) {
                    available = 0;
                    break;
                }
                buffer->size += size;
                available -= size;
                if (buffer->size >= period_bytes) {
                    return_rx_buffer(stream, buffer);
                    break;
                }
                if (!available) {
                    break;
                }
            }
            if (!available) {
                break;
            }
        }
    }
}

/*
 * Flush all buffer data from this output stream's queue into the driver's
 * virtual queue.
 *
 * @stream: VirtIOSoundPCMStream *stream
 */
static inline void virtio_snd_pcm_flush(VirtIOSoundPCMStream *stream)
{
    VirtIOSoundPCMBuffer *buffer;
    void (*cb)(VirtIOSoundPCMStream *, VirtIOSoundPCMBuffer *) =
        stream->is_output ? return_tx_buffer : return_rx_buffer;

    WITH_QEMU_LOCK_GUARD(&stream->mtx) {
        while (!QSIMPLEQ_EMPTY(&stream->queue)) {
            buffer = QSIMPLEQ_FIRST(&stream->queue);
            cb(stream, buffer);
        }
    }
}

static void virtio_snd_unrealize(DeviceState *dev)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOSound *vsnd = VIRTIO_SND(dev);

    qemu_del_vm_change_state_handler(vsnd->vmstate);
    trace_virtio_snd_unrealize(vsnd);

    if (vsnd->pcm_items) {
        for (uint32_t i = 0; i < vsnd->snd_conf.streams; i++) {
            VirtIOSoundPCMStream *stream = vsnd->pcm_items[i].stream;
            if (stream) {
                virtio_snd_destroy_stream(stream);
            }
        }
        g_free(vsnd->pcm_items);
    }

    virtio_delete_queue(vsnd->queues[VIRTIO_SND_VQ_CONTROL]);
    virtio_delete_queue(vsnd->queues[VIRTIO_SND_VQ_EVENT]);
    virtio_delete_queue(vsnd->queues[VIRTIO_SND_VQ_TX]);
    virtio_delete_queue(vsnd->queues[VIRTIO_SND_VQ_RX]);
    virtio_cleanup(vdev);
}

static VirtIOSoundPCMBuffer
*virtio_snd_VirtIOSoundPCMBuffer_get(QEMUFile *f, VirtIOSound *s)
{
    size_t size = qemu_get_be64(f);
    VirtIOSoundPCMBuffer *buffer =
            g_malloc0(sizeof(VirtIOSoundPCMBuffer) + size);
    unsigned vq_index;

    buffer->size = size;
    buffer->offset = qemu_get_be64(f);
    buffer->populated = qemu_get_byte(f);
    vq_index = qemu_get_be16(f);
    if (vq_index >= VIRTIO_SND_VQ_MAX) {
        g_free(buffer);
        return NULL;
    }

    buffer->vq = s->queues[vq_index];
    g_assert(buffer->vq);

    buffer->elem = qemu_get_virtqueue_element(&s->parent_obj, f,
                                              sizeof(VirtQueueElement));
    if (!buffer->elem) {
        g_free(buffer);
        return NULL;
    }

    if (qemu_get_buffer(f, &buffer->data[0], buffer->size) != buffer->size) {
        g_free(buffer->elem);
        g_free(buffer);
        return NULL;
    }

    return buffer;
}

static void
virtio_snd_VirtIOSoundPCMBuffer_put(QEMUFile *f,
                                    VirtIOSoundPCMBuffer *buffer,
                                    VirtIOSound *s)
{
    qemu_put_be64(f, buffer->size);
    qemu_put_be64(f, buffer->offset);
    qemu_put_byte(f, buffer->populated);
    qemu_put_be16(f, virtio_get_queue_index(buffer->vq));
    qemu_put_virtqueue_element(&s->parent_obj, f, buffer->elem);
    qemu_put_buffer(f, &buffer->data[0], buffer->size);
}

static int
virtio_snd_VirtIOSoundPCMBufferQueue_get(QEMUFile *f,
                                         VirtIOSoundPCMBufferQueue *bq,
                                         VirtIOSound *s)
{
    g_assert(QSIMPLEQ_EMPTY(bq));

    uint32_t queue_size = qemu_get_be32(f);
    for (; queue_size; --queue_size) {
        VirtIOSoundPCMBuffer *buffer =
                virtio_snd_VirtIOSoundPCMBuffer_get(f, s);
        if (!buffer) {
            return -EINVAL;
        }

        QSIMPLEQ_INSERT_TAIL(bq, buffer, entry);
    }

    return 0;
}

static void
virtio_snd_VirtIOSoundPCMBufferQueue_put(QEMUFile *f,
                                         VirtIOSoundPCMBufferQueue *bq,
                                         VirtIOSound *s)
{
    VirtIOSoundPCMBuffer *buffer;
    uint32_t queue_size = 0;
    QSIMPLEQ_FOREACH(buffer, bq, entry) {
        ++queue_size;
    }

    qemu_put_be32(f, queue_size);
    QSIMPLEQ_FOREACH(buffer, bq, entry) {
        virtio_snd_VirtIOSoundPCMBuffer_put(f, buffer, s);
    }
}

static const VMStateDescription vmstate_VirtIOPcmParams = {
    .name = "VirtIOPcmParams",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(buffer_bytes, VirtIOPcmParams),
        VMSTATE_UINT32(period_bytes, VirtIOPcmParams),
        VMSTATE_UINT32(features, VirtIOPcmParams),
        VMSTATE_UINT8(channels, VirtIOPcmParams),
        VMSTATE_UINT8(format, VirtIOPcmParams),
        VMSTATE_UINT8(rate, VirtIOPcmParams),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_VirtIOSoundPCMStream = {
    .name = "VirtIOSoundPCMStream",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(effective_params, VirtIOSoundPCMStream, 1,
                       vmstate_VirtIOPcmParams, VirtIOPcmParams),
        VMSTATE_UINT32(id, VirtIOSoundPCMStream),
        VMSTATE_BOOL(is_output, VirtIOSoundPCMStream),
        VMSTATE_BOOL(active, VirtIOSoundPCMStream),
        /*
         * NOTE: stream->queue is handled manually in
         * the virtio_snd_pcm_VirtIOSoundPCMStream_put/get functions.
         */
        VMSTATE_END_OF_LIST()
    }
};

static VirtIOSoundPCMStream
*virtio_snd_pcm_VirtIOSoundPCMStream_get(QEMUFile *f,
                                         VirtIOSound *s)
{
    VirtIOSoundPCMStream* stream = virtio_snd_create_new_stream(s, 0, true);
    Error *err = NULL;
    int ret;

    ret = vmstate_load_state(f, &vmstate_VirtIOSoundPCMStream, stream, 1, &err);
    if (ret < 0) {
        error_report_err(err);
        virtio_snd_destroy_stream(stream);
        return NULL;
    }

    /*
     * Locking `mtx` is not required because the stream
     * was just created here.
     */
    ret = virtio_snd_VirtIOSoundPCMBufferQueue_get(f, &stream->queue, s);
    if (ret < 0) {
        virtio_snd_destroy_stream(stream);
        return NULL;
    }

    if (stream->effective_params.channels) {
        virtio_snd_init_stream_voice_locked(stream, &stream->effective_params);
    }

    return stream;
}

static int
virtio_snd_pcm_VirtIOSoundPCMStream_put(QEMUFile *f,
                                        JSONWriter *vmdes,
                                        VirtIOSoundPCMStream *stream)
{
    Error *err = NULL;
    int ret;

    ret = vmstate_save_state(f, &vmstate_VirtIOSoundPCMStream,
                             stream, vmdes, &err);
    if (ret < 0) {
        error_report_err(err);
        return ret;
    }

    WITH_QEMU_LOCK_GUARD(&stream->mtx) {
        virtio_snd_VirtIOSoundPCMBufferQueue_put(f, &stream->queue, stream->s);
    }

    return 0;
}

static int
virtio_snd_device_remaining_get(QEMUFile *f,
                                void *pv,
                                size_t size,
                                const VMStateField *field)
{
    VirtIOSound *s = pv;
    VirtIOSoundPCMStream *stream;
    uint32_t i;

    for (i = 0; i < s->snd_conf.streams; ++i) {
        g_assert(!s->pcm_items[i].stream);

        if (qemu_get_byte(f)) {
            stream = virtio_snd_pcm_VirtIOSoundPCMStream_get(f, s);
            if (!stream) {
                return -EINVAL;
            }

            s->pcm_items[i].stream = stream;
        }
    }

    return 0;
}

static int
virtio_snd_device_remaining_put(QEMUFile *f,
                                void *pv,
                                size_t size,
                                const VMStateField *field,
                                JSONWriter *vmdes) {
    VirtIOSound *s = pv;
    uint32_t i;
    int ret;

    for (i = 0; i < s->snd_conf.streams; ++i) {
        VirtIOSoundPCMStream *stream = s->pcm_items[i].stream;

        if (stream) {
            qemu_put_byte(f, 1);
            ret = virtio_snd_pcm_VirtIOSoundPCMStream_put(f, vmdes, stream);
            if (ret < 0) {
                return ret;
            }
        } else {
            qemu_put_byte(f, 0);
        }
    }

    return 0;
}

static const VMStateInfo vmstate_virtio_snd_device_remaining = {
    .name = "virtio_snd_device_remaining",
    .get  = virtio_snd_device_remaining_get,
    .put  = virtio_snd_device_remaining_put,
};

static int virtio_snd_device_pre_load(void *opaque)
{
    VirtIOSound *s = opaque;
    uint32_t i;

    for (i = 0; i < s->snd_conf.streams; ++i) {
        VirtIOSoundPCMStream *stream = s->pcm_items[i].stream;
        if (stream) {
            virtio_snd_destroy_stream(stream);
            s->pcm_items[i].stream = NULL;
        }
    }

    return 0;
}

static int virtio_snd_device_post_load(void *opaque, int version_id)
{
    VirtIOSound *s = opaque;
    uint32_t i;

    for (i = 0; i < s->snd_conf.streams; ++i) {
        VirtIOSoundPCMStream *stream = s->pcm_items[i].stream;
        if (stream) {
            WITH_QEMU_LOCK_GUARD(&stream->mtx) {
                if (stream->active) {
                    if (stream->is_output) {
                        audio_be_set_active_out(stream->s->audio_be,
                                                stream->voice.out, 1);
                    } else {
                        audio_be_set_active_in(stream->s->audio_be,
                                               stream->voice.in, 1);
                    }
                }
            }
        }
    }

    return 0;
}

static const VMStateDescription vmstate_VirtIOSoundPCMItem = {
    .name = "VirtIOSoundPCMItem",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(params, VirtIOSoundPCMItem, 1,
                       vmstate_VirtIOPcmParams, VirtIOPcmParams),
        /*
         * NOTE: the `stream` field is not saved/loaded here,
         * see `vmstate_virtio_snd_device_remaining`
         */
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_virtio_snd_config = {
    .name = "virtio_snd_config",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_EQUAL(jacks, virtio_snd_config, NULL),
        VMSTATE_UINT32_EQUAL(streams, virtio_snd_config, NULL),
        VMSTATE_UINT32_EQUAL(chmaps, virtio_snd_config, NULL),
        VMSTATE_UINT32_EQUAL(controls, virtio_snd_config, NULL),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_virtio_snd = {
    .name = TYPE_VIRTIO_SND "-base",
    .version_id = VIRTIO_SOUND_VM_VERSION,
    .minimum_version_id = VIRTIO_SOUND_VM_VERSION,
    .fields = (const VMStateField[]) {
        VMSTATE_VIRTIO_DEVICE,
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_virtio_snd_device = {
    .name = TYPE_VIRTIO_SND,
    .version_id = VIRTIO_SOUND_VM_VERSION,
    .minimum_version_id = VIRTIO_SOUND_VM_VERSION,
    .pre_load = virtio_snd_device_pre_load,
    .post_load = virtio_snd_device_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64_EQUAL(features, VirtIOSound, NULL),
        VMSTATE_STRUCT(snd_conf, VirtIOSound, 1,
                       vmstate_virtio_snd_config, virtio_snd_config),
        VMSTATE_STRUCT_VARRAY_POINTER_UINT32(
                pcm_items, VirtIOSound, snd_conf.streams,
                vmstate_VirtIOSoundPCMItem, VirtIOSoundPCMItem),
        {
            .name = "remaining",
            .info = &vmstate_virtio_snd_device_remaining,
            .flags = VMS_SINGLE,
        },
        VMSTATE_END_OF_LIST()
    },
};

static void virtio_snd_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);


    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
    device_class_set_props(dc, virtio_snd_properties);

    dc->vmsd = &vmstate_virtio_snd;
    vdc->vmsd = &vmstate_virtio_snd_device;
    vdc->realize = virtio_snd_realize;
    vdc->unrealize = virtio_snd_unrealize;
    vdc->get_config = virtio_snd_get_config;
    vdc->get_features = get_features;
    vdc->legacy_features = 0;
}

static const TypeInfo virtio_snd_types[] = {
    {
      .name          = TYPE_VIRTIO_SND,
      .parent        = TYPE_VIRTIO_DEVICE,
      .instance_size = sizeof(VirtIOSound),
      .class_init    = virtio_snd_class_init,
    }
};

DEFINE_TYPES(virtio_snd_types)

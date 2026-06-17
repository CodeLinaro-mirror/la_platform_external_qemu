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
#define STREAM_AS_ENDIANNESS 0 /* Conforming to VIRTIO 1.0: always little endian. */

static void virtio_snd_aud_out_cb(void *data, int available);
static void virtio_snd_aud_in_cb(void *data, int available);

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
    DEFINE_AUDIO_PROPERTIES(VirtIOSound, card),
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

static void vqelems_alloc(VirtIOSoundVqElems* vqe, size_t capacity)
{
    g_assert(vqe);
    g_assert(capacity > 0);
    g_assert(!vqe->data);
    g_assert(!vqe->capacity);
    g_assert(!vqe->size);

    vqe->data = g_new0(VirtQueueElement *, capacity);
    g_assert(vqe->data);
    vqe->capacity = capacity;
    vqe->producer_pos = 0;
    vqe->consumer_pos = 0;
}

static void vqelems_free(VirtIOSoundVqElems* vqe)
{
    g_assert(vqe);
    g_assert(!vqe->size);  /* we cannot leak VirtQueueElement */
    g_free(vqe->data);
    vqe->data = NULL;
    vqe->capacity = 0;
}

static bool vqelems_push(VirtIOSoundVqElems* vqe, VirtQueueElement *elem)
{
    size_t capacity;
    size_t size;
    size_t producer_pos;

    g_assert(vqe);
    g_assert(elem);
    g_assert(vqe->capacity > 0);
    g_assert(vqe->size <= vqe->capacity);
    g_assert(vqe->producer_pos < vqe->capacity);
    g_assert(vqe->data);

    capacity = vqe->capacity;
    size = vqe->size;
    if (size >= capacity) {
        return false;
    }

    producer_pos = vqe->producer_pos;
    vqe->data[producer_pos] = elem;
    vqe->producer_pos = (producer_pos + 1) % capacity;
    vqe->size = size + 1;
    return true;
}

static VirtQueueElement *vqelems_pop(VirtIOSoundVqElems* vqe)
{
    size_t capacity;
    size_t size;
    size_t consumer_pos;

    g_assert(vqe);
    g_assert(vqe->capacity > 0);
    g_assert(vqe->size <= vqe->capacity);
    g_assert(vqe->consumer_pos < vqe->capacity);
    g_assert(vqe->data);

    size = vqe->size;
    if (!size) {
        return NULL;
    }

    consumer_pos = vqe->consumer_pos;
    vqe->consumer_pos = (consumer_pos + 1U) % vqe->capacity;
    vqe->size = size - 1U;
    return vqe->data[consumer_pos];
}

static size_t vqelems_capacity(const VirtIOSoundVqElems* vqe)
{
    g_assert(vqe);
    return vqe->capacity;
}

static size_t vqelems_size(const VirtIOSoundVqElems* vqe)
{
    g_assert(vqe);
    g_assert(vqe->size <= vqe->capacity);
    return vqe->size;
}

static VirtQueueElement *vqelems_peek(const VirtIOSoundVqElems* vqe,
                                      size_t index)
{
    g_assert(vqe);
    g_assert(vqe->capacity > 0);
    g_assert(vqe->size <= vqe->capacity);
    g_assert(vqe->consumer_pos < vqe->capacity);
    g_assert(vqe->data);
    g_assert(index < vqe->size);

    return vqe->data[(vqe->consumer_pos + index) % vqe->capacity];
}

static void pcm_ring_buffer_alloc(VirtIOSoundPcmRingBuf *rb, size_t capacity)
{
    g_assert(capacity > 0);
    g_assert(rb);
    g_assert(!rb->data);
    g_assert(!rb->capacity);

    rb->data = g_new0(uint8_t, capacity);
    g_assert(rb->data);
    rb->capacity = capacity;
    rb->size = 0;
    rb->producer_pos = 0;
    rb->consumer_pos = 0;
}

static void pcm_ring_buffer_free(VirtIOSoundPcmRingBuf *rb)
{
    g_free(rb->data);
    rb->data = NULL;
    rb->capacity = 0;
    rb->size = 0;
}

static void* pcm_ring_buffer_get_consume_chunk(VirtIOSoundPcmRingBuf *rb,
                                               size_t *chunk_size)
{
    g_assert(rb);
    g_assert(rb->data);
    g_assert(rb->capacity > 0);
    g_assert(rb->size >= 0);
    g_assert(rb->size <= rb->capacity);
    g_assert(rb->consumer_pos < rb->capacity);
    g_assert(rb->producer_pos < rb->capacity);

    const uint32_t consumer_pos = rb->consumer_pos;
    const uint32_t producer_pos = rb->producer_pos;
    const uint32_t size = rb->size;
    if (producer_pos > consumer_pos) {
        *chunk_size = MIN(producer_pos - consumer_pos, size);
    } else {
        *chunk_size = MIN(rb->capacity - consumer_pos, size);
    }

    return &rb->data[consumer_pos];
}

static size_t pcm_ring_buffer_consume(VirtIOSoundPcmRingBuf *rb,
                                      size_t chunk_size)
{
    size_t new_size;

    g_assert(rb);
    g_assert(rb->data);
    g_assert(rb->capacity > 0);
    g_assert(rb->size >= 0);
    g_assert(rb->size <= rb->capacity);
    g_assert(rb->consumer_pos < rb->capacity);
    g_assert(rb->producer_pos < rb->capacity);
    g_assert(chunk_size <= rb->size);
    g_assert((rb->consumer_pos + chunk_size) <= rb->capacity);

    rb->consumer_pos = (rb->consumer_pos + chunk_size) % rb->capacity;

    new_size = rb->size - chunk_size;
    rb->size = new_size;
    return new_size;
}

static void* pcm_ring_buffer_get_produce_chunk(VirtIOSoundPcmRingBuf *rb,
                                               size_t want_free_space,
                                               size_t *chunk_size)
{
    size_t capacity;
    size_t size;
    size_t consumer_pos;
    size_t producer_pos;
    size_t available_free;

    g_assert(rb);
    g_assert(rb->data);
    g_assert(rb->capacity > 0);
    g_assert(rb->size >= 0);
    g_assert(rb->size <= rb->capacity);
    g_assert(rb->consumer_pos < rb->capacity);
    g_assert(rb->producer_pos < rb->capacity);
    g_assert(want_free_space > 0);

    capacity = rb->capacity;
    size = rb->size;
    consumer_pos = rb->consumer_pos;

    want_free_space = MIN(want_free_space, capacity);
    available_free = capacity - size;
    if (available_free < want_free_space) {
        const size_t to_drop = want_free_space - available_free;
        g_assert(to_drop <= size);

        size -= to_drop;
        consumer_pos = (consumer_pos + to_drop) % capacity;
        available_free += to_drop;

        rb->size = size;
        rb->consumer_pos = consumer_pos;
    }

    producer_pos = rb->producer_pos;
    if (producer_pos >= consumer_pos) {
        *chunk_size = MIN(want_free_space,
                          MIN(capacity - producer_pos, available_free));
    } else {
        *chunk_size = MIN(want_free_space,
                          MIN(consumer_pos - producer_pos, available_free));
    }

    return &rb->data[rb->producer_pos];
}

static uint32_t pcm_ring_buffer_produce(VirtIOSoundPcmRingBuf *rb,
                                        size_t chunk_size)
{
    size_t new_size;

    g_assert(rb);
    g_assert(rb->data);
    g_assert(rb->capacity > 0);
    g_assert(rb->size <= rb->capacity);
    g_assert(rb->consumer_pos < rb->capacity);
    g_assert(rb->producer_pos < rb->capacity);
    g_assert((rb->size + chunk_size) <= rb->capacity);
    g_assert((rb->producer_pos + chunk_size) <= rb->capacity);

    rb->producer_pos = (rb->producer_pos + chunk_size) % rb->capacity;
    new_size = rb->size + chunk_size;
    rb->size = new_size;
    return new_size;
}

static uint32_t pcm_ring_buffer_size(const VirtIOSoundPcmRingBuf *rb)
{
    g_assert(rb);
    g_assert(rb->capacity > 0);
    g_assert(rb->size <= rb->capacity);
    return rb->size;
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

static size_t virtio_snd_get_sample_size(AudioFormat fmt)
{
    switch (fmt) {
    case AUDIO_FORMAT_U8:
    case AUDIO_FORMAT_S8:
        return 1;

    case AUDIO_FORMAT_U16:
    case AUDIO_FORMAT_S16:
        return 2;

    case AUDIO_FORMAT_U32:
    case AUDIO_FORMAT_S32:
    case AUDIO_FORMAT_F32:
        return 4;

    default:
        return 0;
    }
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

    as->endianness = STREAM_AS_ENDIANNESS;
    return 0;
}

static uint32_t virtio_snd_get_period_us(size_t period_bytes,
                                         AudioFormat fmt,
                                         size_t nchannels,
                                         size_t freq_hz)
{
    size_t frame_bytes = virtio_snd_get_sample_size(fmt) * nchannels;
    g_assert(period_bytes > 0);
    g_assert(frame_bytes > 0);
    g_assert(freq_hz > 0);
    return ((period_bytes / frame_bytes) * 1000000U) / freq_hz;
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
}

static void virtio_snd_virtqueue_consume_elem(VirtQueue *vq,
                                              VirtQueueElement *elem,
                                              size_t data_size)
{
    g_assert(vq);
    g_assert(elem);
    virtqueue_push(vq, elem, data_size);
    g_free(elem);
}

static VirtQueue *virtio_snd_stream_get_vq(const VirtIOSoundPCMStream *stream)
{
    g_assert(stream);
    return stream->s->queues[stream->is_output ? VIRTIO_SND_VQ_TX
                                               : VIRTIO_SND_VQ_RX];
}

static size_t virtio_snd_stream_period_tx_elem(VirtIOSoundPCMStream *stream,
                                               VirtQueueElement *elem,
                                               bool main_path)
{
    virtio_snd_pcm_status resp;

    g_assert(elem);

    if (main_path) {
        size_t data_size;
        size_t offset;

        data_size = iov_size(elem->out_sg, elem->out_num);
        g_assert(data_size >= sizeof(virtio_snd_pcm_xfer));

        data_size -= sizeof(virtio_snd_pcm_xfer);
        if (data_size > stream->params.period_bytes) {
            data_size = stream->params.period_bytes;
        }

        offset = sizeof(virtio_snd_pcm_xfer);
        while (data_size > 0) {
            size_t chunk_size;
            void *chunk = pcm_ring_buffer_get_produce_chunk(
                    &stream->pcm, data_size, &chunk_size);

            chunk_size = MIN(chunk_size, data_size);
            g_assert(chunk_size > 0);

            iov_to_buf(elem->out_sg, elem->out_num,
                    offset, chunk, chunk_size);

            pcm_ring_buffer_produce(&stream->pcm, chunk_size);

            data_size -= chunk_size;
            offset += chunk_size;
        }
    }

    resp.status = cpu_to_le32(VIRTIO_SND_S_OK);
    resp.latency_bytes = cpu_to_le32(pcm_ring_buffer_size(&stream->pcm));
    iov_from_buf(elem->in_sg, elem->in_num, 0, &resp, sizeof(resp));
    return sizeof(resp);
}

static size_t virtio_snd_stream_period_rx_elem(VirtIOSoundPCMStream *stream,
                                               VirtQueueElement *elem,
                                               bool main_path)
{
    virtio_snd_pcm_status resp;
    size_t data_size;
    size_t offset;

    g_assert(elem);

    data_size = stream->params.period_bytes;
    offset = 0;
    while (data_size > 0) {
        size_t chunk_size;
        void *chunk = pcm_ring_buffer_get_consume_chunk(
                &stream->pcm, &chunk_size);

        if (!chunk_size) {
            if (main_path) {
                iov_from_buf(elem->in_sg, elem->in_num,
                             offset, stream->silence_buf, data_size);
                offset += data_size;
                data_size = 0;
            }
            break;
        }

        chunk_size = MIN(chunk_size, data_size);
        g_assert(chunk_size > 0);

        iov_from_buf(elem->in_sg, elem->in_num,
                     offset, chunk, chunk_size);

        pcm_ring_buffer_consume(&stream->pcm, chunk_size);

        offset += chunk_size;
        data_size -= chunk_size;
    }

    resp.status = cpu_to_le32(VIRTIO_SND_S_OK);
    resp.latency_bytes = cpu_to_le32(pcm_ring_buffer_size(&stream->pcm));
    iov_from_buf(elem->in_sg, elem->in_num, offset, &resp, sizeof(resp));
    return offset + sizeof(resp);

}

static size_t virtio_snd_stream_period_elem(VirtIOSoundPCMStream *stream,
                                            VirtQueueElement *elem,
                                            bool main_path)
{
    g_assert(elem);

    if (stream->is_output) {
        return virtio_snd_stream_period_tx_elem(stream, elem, main_path);
    } else {
        return virtio_snd_stream_period_rx_elem(stream, elem, main_path);
    }
}

static void virtio_snd_stream_period_cb(void *opaque)
{
    VirtIOSoundPCMStream *stream = opaque;
    uint64_t next_period_us;

    do {
        VirtQueue *vq;
        VirtQueueElement *elem;
        size_t data_size;

        WITH_QEMU_LOCK_GUARD(&stream->mtx) {
            elem = vqelems_pop(&stream->vqelems);
            if (elem) {
                data_size = virtio_snd_stream_period_elem(stream, elem, true);
                vq = virtio_snd_stream_get_vq(stream);
            } else {
                stream->num_missed_periods =
                        MIN(stream->num_missed_periods + 1U,
                            vqelems_capacity(&stream->vqelems));
            }

            g_assert(stream->next_period_us > 0);
            g_assert(stream->period_us > 0);
            next_period_us = stream->next_period_us + stream->period_us;
            stream->next_period_us = next_period_us;
        }

        if (elem) {
            virtio_snd_virtqueue_consume_elem(vq, elem, data_size);
            virtio_notify(&stream->s->parent_obj, vq);
        }
    } while (next_period_us <= qemu_clock_get_us(QEMU_CLOCK_VIRTUAL));

    timer_mod(&stream->period_timer, next_period_us);
}

static uint32_t virtio_snd_stream_start(VirtIOSoundPCMStream *stream)
{
    VirtQueueElement *elem = NULL;
    size_t data_size;
    uint64_t next_period_us;

    WITH_QEMU_LOCK_GUARD(&stream->mtx) {
        if (stream->next_period_us) {
            return VIRTIO_SND_S_OK;
        }

        g_assert(stream->period_us > 0);
        stream->num_missed_periods = 0;
        next_period_us =
                qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) + stream->period_us;
        stream->next_period_us = next_period_us;

        g_assert(stream->voice.raw);
        if (stream->is_output) {
            elem = vqelems_pop(&stream->vqelems);
            if (elem) {
                data_size = virtio_snd_stream_period_tx_elem(
                        stream, elem, true);
            }

            AUD_set_active_out(stream->voice.out, 1);
        } else {
            AUD_set_active_in(stream->voice.in, 1);
        }
    }

    if (elem) {
        VirtQueue *vq = virtio_snd_stream_get_vq(stream);
        virtio_snd_virtqueue_consume_elem(vq, elem, data_size);
        virtio_notify(&stream->s->parent_obj, vq);
    }

    timer_mod(&stream->period_timer, next_period_us);

    return VIRTIO_SND_S_OK;
}

static uint32_t virtio_snd_stream_stop(VirtIOSoundPCMStream *stream)
{
    WITH_QEMU_LOCK_GUARD(&stream->mtx) {
        if (!stream->next_period_us) {
            return VIRTIO_SND_S_OK;
        }

        timer_del(&stream->period_timer);
        stream->num_missed_periods = 0;
        stream->next_period_us = 0;

        g_assert(stream->voice.raw);
        if (stream->is_output) {
            AUD_set_active_out(stream->voice.out, 0);
        } else {
            AUD_set_active_in(stream->voice.in, 0);
        }
    }

    return VIRTIO_SND_S_OK;
}

static void virtio_snd_stream_post_load(VirtIOSoundPCMStream *stream)
{
    g_assert(stream);

    WITH_QEMU_LOCK_GUARD(&stream->mtx) {
        if (stream->next_period_us) {
            timer_mod(&stream->period_timer, stream->next_period_us);

            if (stream->is_output) {
                AUD_set_active_out(stream->voice.out, 1);
            } else {
                AUD_set_active_in(stream->voice.in, 1);
            }
        }
    }
}

static VirtIOSoundPCMStream *virtio_snd_stream_create(
        VirtIOSound *s,
        const VirtIOPcmParams *params,
        bool is_output)
{
    VirtIOSoundPCMStream *stream;
    audsettings as;
    size_t num_periods;

    g_assert(s);
    g_assert(params);
    g_assert(params->buffer_bytes > 0);
    g_assert(params->period_bytes > 0);

    if (virtio_snd_get_qemu_audsettings(
            &as, params->channels, params->format, params->rate)) {
        return NULL;
    }

    stream = g_new0(VirtIOSoundPCMStream, 1);
    if (!stream) {
        return NULL;
    }

    if (is_output) {
        stream->voice.out = AUD_open_out(&s->card,
                                         NULL,
                                         "virtio-sound.out",
                                         stream,
                                         virtio_snd_aud_out_cb,
                                         &as);
        if (!stream->voice.out) {
err:        g_free(stream);
            return NULL;
        }

        AUD_set_volume_out(stream->voice.out, 0, 255, 255);
    } else {
        stream->voice.in = AUD_open_in(&s->card,
                                       NULL,
                                       "virtio-sound.in",
                                       stream,
                                       virtio_snd_aud_in_cb,
                                       &as);
        if (!stream->voice.in) {
            goto err;
        }

        AUD_set_volume_in(stream->voice.in, 0, 255, 255);
    }

    stream->s = s;
    stream->is_output = is_output;
    stream->params = *params;
    stream->period_us = virtio_snd_get_period_us(
            params->period_bytes, as.fmt, as.nchannels, as.freq);

    pcm_ring_buffer_alloc(&stream->pcm, params->buffer_bytes);
    stream->silence_buf = g_new0(char, params->period_bytes);

    num_periods =
            (params->buffer_bytes + params->period_bytes - 1) / params->period_bytes;
    vqelems_alloc(&stream->vqelems, num_periods * 2U);

    timer_init_us(&stream->period_timer, QEMU_CLOCK_VIRTUAL,
                  virtio_snd_stream_period_cb, stream);

    qemu_mutex_init(&stream->mtx);
    return stream;
}

static void virtio_snd_stream_destroy(VirtIOSoundPCMStream *stream)
{
    VirtQueue *vq;
    bool need_notify = false;
    g_assert(stream);

    WITH_QEMU_LOCK_GUARD(&stream->mtx) {
        timer_del(&stream->period_timer);

        g_assert(stream->voice.raw);
        if (stream->is_output) {
            AUD_close_out(&stream->s->card, stream->voice.out);
        } else {
            AUD_close_in(&stream->s->card, stream->voice.in);
        }
    }

    /* with the callbacks stopped above we no longer need the lock */
    vq = virtio_snd_stream_get_vq(stream);
    while (true) {
        VirtQueueElement *elem = vqelems_pop(&stream->vqelems);
        size_t data_size;
        if (!elem) {
            break;
        }

        data_size = virtio_snd_stream_period_elem(stream, elem, false);
        virtio_snd_virtqueue_consume_elem(vq, elem, data_size);
        need_notify = true;
    }

    g_assert(stream->silence_buf);

    vqelems_free(&stream->vqelems);
    pcm_ring_buffer_free(&stream->pcm);
    g_free(stream->silence_buf);
    qemu_mutex_destroy(&stream->mtx);

    if (need_notify) {
        virtio_notify(&stream->s->parent_obj, vq);
    }

    g_free(stream);
}

static bool virtio_snd_is_output_stream(VirtIOSound *s, uint32_t stream_id)
{
    return stream_id < ((s->snd_conf.streams + 1U) / 2U);
}

static uint32_t virtio_snd_handle_pcm_info(VirtIOSound *s,
                                           VirtQueueElement *elem,
                                           size_t *payload_size)
{
    uint32_t start_id, count, size;
    virtio_snd_query_info req;
    g_autofree virtio_snd_pcm_info *pcm_info = NULL;
    size_t msg_sz = iov_to_buf(elem->out_sg, elem->out_num,
                               0, &req, sizeof(req));

    if (msg_sz != sizeof(req)) {
        /*
         * TODO: do we need to set DEVICE_NEEDS_RESET?
         */
        qemu_log_mask(LOG_GUEST_ERROR,
                "%s: virtio-snd command size incorrect %zu vs \
                %zu\n", __func__, msg_sz, sizeof(req));
        return VIRTIO_SND_S_BAD_MSG;
    }

    start_id = le32_to_cpu(req.start_id);
    count = le32_to_cpu(req.count);
    if ((start_id + count) > s->snd_conf.streams) {
        error_report("pcm info: requested a stream outside of bounds,"
                " start_id=%" PRIu32 " count=%" PRIu32
                " snd_conf.streams=%" PRIu32,
                start_id, count, s->snd_conf.streams);
        return VIRTIO_SND_S_BAD_MSG;
    }

    size = le32_to_cpu(req.size);

    if (iov_size(elem->in_sg, elem->in_num) <
        sizeof(virtio_snd_hdr) + size * count) {
        /*
         * TODO: do we need to set DEVICE_NEEDS_RESET?
         */
        error_report("pcm info: buffer too small, got: %zu, needed: %zu",
                iov_size(elem->in_sg, elem->in_num),
                sizeof(virtio_snd_pcm_info));
        return VIRTIO_SND_S_BAD_MSG;
    }

    pcm_info = g_new0(virtio_snd_pcm_info, count);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t stream_id = i + start_id;
        virtio_snd_pcm_info* val = &pcm_info[i];
        trace_virtio_snd_handle_pcm_info(stream_id);
        *val = stream_default_info;
        val->direction = virtio_snd_is_output_stream(s, stream_id) ?
                VIRTIO_SND_D_OUTPUT : VIRTIO_SND_D_INPUT;
        /*
         * 5.14.6.6.2.1 Device Requirements: Stream Information The device MUST
         * NOT set undefined feature, format, rate and direction values. The
         * device MUST initialize the padding bytes to 0.
         */
        memset(&val->padding, 0, 5);
    }

    *payload_size = sizeof(virtio_snd_pcm_info) * count;
    iov_from_buf(elem->in_sg, elem->in_num,
                 sizeof(virtio_snd_hdr),
                 pcm_info, *payload_size);

    return VIRTIO_SND_S_OK;
}

static uint32_t virtio_snd_handle_pcm_set_params(VirtIOSound *s,
                                                 VirtQueueElement *elem)
{
    VirtIOPcmParams *st_params;
    virtio_snd_pcm_set_params req;
    uint32_t stream_id;
    audsettings as_ignored;

    size_t msg_sz = iov_to_buf(elem->out_sg, elem->out_num,
                               0, &req, sizeof(req));

    if (msg_sz != sizeof(req)) {
        /*
         * TODO: do we need to set DEVICE_NEEDS_RESET?
         */
        qemu_log_mask(LOG_GUEST_ERROR,
                "%s: virtio-snd command size incorrect %zu vs \
                %zu\n", __func__, msg_sz, sizeof(virtio_snd_pcm_set_params));
        return VIRTIO_SND_S_BAD_MSG;
    }

    stream_id = le32_to_cpu(req.hdr.stream_id);
    trace_virtio_snd_handle_pcm_set_params(stream_id);

    if (stream_id > s->snd_conf.streams) {
        error_report("Invalid stream id: %"PRIu32, stream_id);
        return VIRTIO_SND_S_BAD_MSG;
    }

    if (virtio_snd_get_qemu_audsettings(
            &as_ignored, req.channels, req.format, req.rate)) {
        return VIRTIO_SND_S_NOT_SUPP;
    }

    st_params = &s->pcm_items[stream_id].params;

    st_params->buffer_bytes = le32_to_cpu(req.buffer_bytes);
    st_params->period_bytes = le32_to_cpu(req.period_bytes);
    st_params->features = le32_to_cpu(req.features);
    /* the following are uint8_t, so there's no need to bswap the values. */
    st_params->channels = req.channels;
    st_params->format = req.format;
    st_params->rate = req.rate;

    return VIRTIO_SND_S_OK;
}

static uint32_t virtio_snd_handle_pcm_prepare(VirtIOSound *s,
                                              VirtQueueElement *elem)
{
    VirtIOSoundPCMItem *item;
    uint32_t stream_id;
    size_t msg_sz = iov_to_buf(elem->out_sg, elem->out_num,
                               sizeof(virtio_snd_hdr),
                               &stream_id,
                               sizeof(stream_id));
    if (msg_sz != sizeof(stream_id)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                "%s: virtio-snd command size incorrect %zu vs \
                %zu\n", __func__, msg_sz, sizeof(stream_id));
        return VIRTIO_SND_S_BAD_MSG;
    }

    stream_id = le32_to_cpu(stream_id);
    if (stream_id > s->snd_conf.streams) {
        error_report("Invalid stream id: %"PRIu32, stream_id);
        return VIRTIO_SND_S_BAD_MSG;
    }

    item = &s->pcm_items[stream_id];
    if (item->stream) {
        virtio_snd_stream_destroy(item->stream);
        item->stream = NULL;
    }

    item->stream = virtio_snd_stream_create(
            s, &item->params, virtio_snd_is_output_stream(s, stream_id));
    if (!item->stream) {
        return VIRTIO_SND_S_BAD_MSG;
    }

    return VIRTIO_SND_S_OK;
}

static uint32_t virtio_snd_handle_pcm_release(VirtIOSound *s,
                                              VirtQueueElement *elem)
{
    VirtIOSoundPCMItem *item;
    uint32_t stream_id;
    size_t msg_sz = iov_to_buf(elem->out_sg, elem->out_num,
                               sizeof(virtio_snd_hdr),
                               &stream_id,
                               sizeof(stream_id));
    if (msg_sz != sizeof(stream_id)) {
        /*
         * TODO: do we need to set DEVICE_NEEDS_RESET?
         */
        qemu_log_mask(LOG_GUEST_ERROR,
                "%s: virtio-snd command size incorrect %zu vs \
                %zu\n", __func__, msg_sz, sizeof(stream_id));
        return VIRTIO_SND_S_BAD_MSG;
    }

    stream_id = le32_to_cpu(stream_id);
    trace_virtio_snd_handle_pcm_release(stream_id);

    if (stream_id > s->snd_conf.streams) {
        error_report("Invalid stream id: %"PRIu32, stream_id);
        return VIRTIO_SND_S_BAD_MSG;
    }

    item = &s->pcm_items[stream_id];
    if (item->stream) {
        virtio_snd_stream_destroy(item->stream);
        item->stream = NULL;
    }

    return VIRTIO_SND_S_OK;
}

static uint32_t virtio_snd_handle_pcm_start_stop(VirtIOSound *s,
                                                 VirtQueueElement *elem,
                                                 bool start)
{
    VirtIOSoundPCMStream *stream;
    virtio_snd_pcm_hdr req;
    uint32_t stream_id;
    size_t msg_sz = iov_to_buf(elem->out_sg, elem->out_num,
                               0, &req, sizeof(req));

    if (msg_sz != sizeof(req)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                "%s: virtio-snd command size incorrect %zu vs \
                %zu\n", __func__, msg_sz, sizeof(req));
        return VIRTIO_SND_S_BAD_MSG;
    }

    stream_id = le32_to_cpu(req.stream_id);
    trace_virtio_snd_handle_pcm_start_stop(start ? "VIRTIO_SND_R_PCM_START" :
            "VIRTIO_SND_R_PCM_STOP", req.stream_id);

    if (stream_id > s->snd_conf.streams) {
        error_report("Invalid stream id: %"PRIu32, stream_id);
        return VIRTIO_SND_S_BAD_MSG;
    }

    stream = s->pcm_items[stream_id].stream;
    if (!stream) {
        error_report("Unprepared stream id: %"PRIu32, stream_id);
        return VIRTIO_SND_S_BAD_MSG;
    }

    return start ?
            virtio_snd_stream_start(stream) :
            virtio_snd_stream_stop(stream);
}

/*
 * Processes a command from the VIRTIO_SND_VQ_CONTROL queue.
 *
 * @s: VirtIOSound device
 * @cmd: control command request
 */
static size_t
process_cmd(VirtIOSound *s, VirtQueueElement *elem)
{
    virtio_snd_hdr ctrl;
    virtio_snd_hdr resp;
    size_t payload_size = 0;
    uint32_t code;
    uint32_t resp_code;
    size_t msg_sz = iov_to_buf(elem->out_sg, elem->out_num,
                               0, &ctrl, sizeof(ctrl));

    if (msg_sz != sizeof(ctrl)) {
        /*
         * TODO: do we need to set DEVICE_NEEDS_RESET?
         */
        qemu_log_mask(LOG_GUEST_ERROR,
                "%s: virtio-snd command size incorrect %zu vs \
                %zu\n", __func__, msg_sz, sizeof(ctrl));
        return 0;
    }

    code = le32_to_cpu(ctrl.code);

    trace_virtio_snd_handle_code(code, print_code(code));

    switch (code) {
    case VIRTIO_SND_R_JACK_INFO:
    case VIRTIO_SND_R_JACK_REMAP:
        qemu_log_mask(LOG_UNIMP,
                     "virtio_snd: jack functionality is unimplemented.\n");
        resp_code = VIRTIO_SND_S_NOT_SUPP;
        break;
    case VIRTIO_SND_R_PCM_INFO:
        resp_code = virtio_snd_handle_pcm_info(s, elem, &payload_size);
        break;
    case VIRTIO_SND_R_PCM_START:
        resp_code = virtio_snd_handle_pcm_start_stop(s, elem, true);
        break;
    case VIRTIO_SND_R_PCM_STOP:
        resp_code = virtio_snd_handle_pcm_start_stop(s, elem, false);
        break;
    case VIRTIO_SND_R_PCM_SET_PARAMS:
        resp_code = virtio_snd_handle_pcm_set_params(s, elem);
        break;
    case VIRTIO_SND_R_PCM_PREPARE:
        resp_code = virtio_snd_handle_pcm_prepare(s, elem);
        break;
    case VIRTIO_SND_R_PCM_RELEASE:
        resp_code = virtio_snd_handle_pcm_release(s, elem);
        break;
    case VIRTIO_SND_R_CHMAP_INFO:
        qemu_log_mask(LOG_UNIMP,
                     "virtio_snd: chmap info functionality is unimplemented.\n");
        trace_virtio_snd_handle_chmap_info();
        resp_code = VIRTIO_SND_S_NOT_SUPP;
        break;
    default:
        /* error */
        error_report("virtio snd header not recognized: %"PRIu32, code);
        resp_code = VIRTIO_SND_S_BAD_MSG;
    }

    resp.code = cpu_to_le32(resp_code);

    iov_from_buf(elem->in_sg, elem->in_num,
                 0, &resp, sizeof(resp));

    return sizeof(resp) + payload_size;
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
    trace_virtio_snd_handle_ctrl(vdev, vq);

    if (!virtio_queue_ready(vq)) {
        return;
    }

    while (true) {
        VirtQueueElement *elem = virtqueue_pop(vq, sizeof(VirtQueueElement));
        if (elem) {
            virtio_snd_virtqueue_consume_elem(vq, elem, process_cmd(s, elem));
        } else {
            break;
        }
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

static void virtio_snd_handle_xfer(VirtIOSound *vsnd, VirtQueue *vq)
{
    bool need_notify = false;

    if (!virtio_queue_ready(vq)) {
        return;
    }

    for (;;) {
        VirtQueueElement *elem;
        virtio_snd_pcm_xfer hdr;
        size_t msg_sz;
        size_t data_size;

        elem = virtqueue_pop(vq, sizeof(VirtQueueElement));
        if (!elem) {
            break;
        }

        /* get the message hdr object */
        msg_sz = iov_to_buf(elem->out_sg, elem->out_num,
                            0, &hdr, sizeof(hdr));
        if (msg_sz != sizeof(hdr)) {
            virtio_snd_virtqueue_consume_elem(vq, elem, 0);
            need_notify = true;
        } else {
            VirtIOSoundPCMStream *stream;
            virtio_snd_pcm_status err_resp;
            uint32_t stream_id = le32_to_cpu(hdr.stream_id);
            if (stream_id > vsnd->snd_conf.streams) {
err:            err_resp.status = cpu_to_le32(VIRTIO_SND_S_BAD_MSG);
                err_resp.latency_bytes = 0;

                iov_from_buf(elem->in_sg, elem->in_num,
                             0, &err_resp, sizeof(err_resp));
                virtio_snd_virtqueue_consume_elem(vq, elem, sizeof(err_resp));
                need_notify = true;
                continue;
            }

            stream = vsnd->pcm_items[stream_id].stream;
            if (!stream) {
                goto err;
            }

            WITH_QEMU_LOCK_GUARD(&stream->mtx) {
                if (stream->num_missed_periods) {
                    data_size = virtio_snd_stream_period_elem(stream, elem, true);
                    --stream->num_missed_periods;
                } else if (vqelems_push(&stream->vqelems, elem)) {
                    elem = NULL;
                } else {
                    /*
                     * `stream->vqelems` should not overflow (it is allocated
                     * with double capacity), but if does then process
                     * the `elem` right away.
                     */
                    data_size = virtio_snd_stream_period_elem(stream, elem, true);
                }
            }

            if (elem) {
                virtio_snd_virtqueue_consume_elem(vq, elem, data_size);
                need_notify = true;
            }
        }
    }

    if (need_notify) {
        virtio_notify(&vsnd->parent_obj, vq);
    }
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
    trace_virtio_snd_handle_tx_xfer();
    virtio_snd_handle_xfer(VIRTIO_SND(vdev), vq);
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
    trace_virtio_snd_handle_rx_xfer();
    virtio_snd_handle_xfer(VIRTIO_SND(vdev), vq);
}

/*
 * AUD_* output callback.
 *
 * @data: VirtIOSoundPCMStream stream
 * @available: number of bytes that can be written with AUD_write()
 */
static void virtio_snd_aud_out_cb(void *data, int available_bytes)
{
    VirtIOSoundPCMStream *stream = data;

    WITH_QEMU_LOCK_GUARD(&stream->mtx) {
        while (available_bytes > 0) {
            size_t chunk_size;
            size_t written;
            void* chunk_data = pcm_ring_buffer_get_consume_chunk(
                    &stream->pcm, &chunk_size);

            if (!chunk_size) {
                /* insert silence to fill up available_bytes */
                const size_t period_size = stream->params.period_bytes;
                g_assert(period_size > 0);

                do {
                    size_t chunk_size =
                            MIN((size_t)available_bytes, period_size);

                    written = AUD_write(stream->voice.out, stream->silence_buf,
                                        chunk_size);
                    g_assert(written <= chunk_size);
                    if (!written) {
                        return;
                    }

                    available_bytes -= written;
                } while (available_bytes > 0);

                return;
            } else if (chunk_size > available_bytes) {
                chunk_size = available_bytes;
            }

            written = AUD_write(stream->voice.out, chunk_data, chunk_size);
            g_assert(written <= chunk_size);
            if (!written) {
                return;
            }

            pcm_ring_buffer_consume(&stream->pcm, written);
            available_bytes -= written;
        }
    }
}

/*
 * AUD_* input callback.
 *
 * @data: VirtIOSoundPCMStream stream
 * @available: number of bytes that can be read with AUD_read()
 */
static void virtio_snd_aud_in_cb(void *data, int available_bytes)
{
    VirtIOSoundPCMStream *stream = data;

    WITH_QEMU_LOCK_GUARD(&stream->mtx) {
        const size_t period_size = stream->params.period_bytes;

        while (available_bytes > 0) {
            size_t chunk_size;
            size_t read;
            void* chunk_data = pcm_ring_buffer_get_produce_chunk(
                    &stream->pcm, period_size, &chunk_size);

            g_assert(chunk_size);
            if (chunk_size > available_bytes) {
                chunk_size = available_bytes;
            }

            read = AUD_read(stream->voice.in, chunk_data, chunk_size);
            g_assert(read <= chunk_size);
            if (read) {
                pcm_ring_buffer_produce(&stream->pcm, read);
                available_bytes -= read;
            } else {
                return;
            }
        }
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

    if (!AUD_register_card("virtio-sound", &vsnd->card, errp)) {
        return;
    }

    vsnd->vmstate =
        qemu_add_vm_change_state_handler(virtio_snd_vm_state_change, vsnd);

    vsnd->pcm_items = g_new0(VirtIOSoundPCMItem, vsnd->snd_conf.streams);

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
}

static void virtio_snd_unrealize(DeviceState *dev)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOSound *vsnd = VIRTIO_SND(dev);

    qemu_del_vm_change_state_handler(vsnd->vmstate);
    trace_virtio_snd_unrealize(vsnd);

    if (vsnd->pcm_items) {
        for (uint32_t i = 0; i < vsnd->snd_conf.streams; i++) {
            VirtIOSoundPCMStream* stream = vsnd->pcm_items[i].stream;
            if (stream) {
                virtio_snd_stream_destroy(stream);
            }
        }
        g_free(vsnd->pcm_items);
        vsnd->pcm_items = NULL;
    }

    AUD_remove_card(&vsnd->card);
    virtio_delete_queue(vsnd->queues[VIRTIO_SND_VQ_CONTROL]);
    virtio_delete_queue(vsnd->queues[VIRTIO_SND_VQ_EVENT]);
    virtio_delete_queue(vsnd->queues[VIRTIO_SND_VQ_TX]);
    virtio_delete_queue(vsnd->queues[VIRTIO_SND_VQ_RX]);
    virtio_cleanup(vdev);
}

static int virtio_snd_device_pre_load(void *opaque)
{
    VirtIOSound *s = opaque;
    uint32_t i;

    for (i = 0; i < s->snd_conf.streams; ++i) {
        VirtIOSoundPCMStream* stream = s->pcm_items[i].stream;
        if (stream) {
            virtio_snd_stream_destroy(stream);
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
        VirtIOSoundPCMStream* stream = s->pcm_items[i].stream;
        if (stream) {
            virtio_snd_stream_post_load(stream);
        }
    }

    return 0;
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

static VirtIOSoundPCMStream *virtio_snd_stream_load(VirtIOSound *s,
                                                 uint32_t stream_id,
                                                 QEMUFile *f)
{
    VirtIOPcmParams params;
    VirtIOSoundPCMStream *stream;
    size_t num_elems;

    if (vmstate_load_state(f, &vmstate_VirtIOPcmParams, &params, 1)) {
        return NULL;
    }

    stream = virtio_snd_stream_create(
            s, &params, virtio_snd_is_output_stream(s, stream_id));
    if (!stream) {
        return NULL;
    }

    stream->next_period_us = qemu_get_be64(f);
    stream->num_missed_periods = qemu_get_be16(f);

    num_elems = qemu_get_be16(f);
    for (; num_elems; --num_elems) {
        VirtQueueElement *elem = qemu_get_virtqueue_element(
                &s->parent_obj, f, sizeof(VirtQueueElement));
        if (!elem) {
            virtio_snd_stream_destroy(stream);
            return NULL;
        }
        if (!vqelems_push(&stream->vqelems, elem)) {
            VirtQueue *vq = s->queues[
                    stream->is_output ? VIRTIO_SND_VQ_TX : VIRTIO_SND_VQ_RX];
            virtqueue_detach_element(vq, elem, 0);
            g_free(elem);
            virtio_snd_stream_destroy(stream);
            return NULL;
        }
    }

    return stream;
}

static void virtio_snd_stream_save(VirtIOSoundPCMStream *stream,
                                   QEMUFile *f,
                                   JSONWriter *vmdes)
{
    WITH_QEMU_LOCK_GUARD(&stream->mtx) {
        size_t i;
        size_t num_elems;

        vmstate_save_state(f, &vmstate_VirtIOPcmParams,
                           &stream->params, vmdes);
        qemu_put_be64(f, stream->next_period_us);
        qemu_put_be16(f, stream->num_missed_periods);

        num_elems = vqelems_size(&stream->vqelems);
        qemu_put_be16(f, num_elems);
        for (i = 0; i < num_elems; ++i) {
            qemu_put_virtqueue_element(
                    &stream->s->parent_obj, f,
                    vqelems_peek(&stream->vqelems, i));
        }
    }
}

static int
vmstate_virtio_snd_device_streams_get(QEMUFile *f,
                                      void *pv,
                                      size_t size,
                                      const VMStateField *field)
{
    VirtIOSound *s = pv;
    uint32_t stream_id;

    for (stream_id = 0; stream_id < s->snd_conf.streams; ++stream_id) {
        VirtIOSoundPCMStream *stream;
        g_assert(!s->pcm_items[stream_id].stream);

        switch (qemu_get_byte(f)) {
        case 0xAB:
            stream = virtio_snd_stream_load(s, stream_id, f);
            if (!stream) {
                return -EINVAL;
            }

            s->pcm_items[stream_id].stream = stream;
            break;

        case 0xBA:
            break;

        default:
            return -EINVAL;
        }
    }

    return 0;
}

static int
vmstate_virtio_snd_device_streams_put(QEMUFile *f,
                                      void *pv,
                                      size_t size,
                                      const VMStateField *field,
                                      JSONWriter *vmdes) {
    VirtIOSound *s = pv;
    uint32_t i;

    for (i = 0; i < s->snd_conf.streams; ++i) {
        VirtIOSoundPCMStream *stream = s->pcm_items[i].stream;
        if (stream) {
            qemu_put_byte(f, 0xAB);
            virtio_snd_stream_save(stream, f, vmdes);
        } else {
            qemu_put_byte(f, 0xBA);
        }
    }

    return 0;
}

static const VMStateInfo vmstate_virtio_snd_device_streams = {
    .name = "vmstate_virtio_snd_device_streams",
    .get  = vmstate_virtio_snd_device_streams_get,
    .put  = vmstate_virtio_snd_device_streams_put,
};

static const VMStateDescription vmstate_VirtIOSoundPCMItem = {
    .name = "vmstate_VirtIOSoundPCMItem",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(params, VirtIOSoundPCMItem, 1,
                       vmstate_VirtIOPcmParams, VirtIOPcmParams),
        /*
         * The `stream` field is handled in `vmstate_virtio_snd_device_streams`
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
            .name = "streams",
            .info = &vmstate_virtio_snd_device_streams,
            .flags = VMS_SINGLE,
        },
        VMSTATE_END_OF_LIST()
    },
};

static void virtio_snd_class_init(ObjectClass *klass, void *data)
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

/*
 * VIRTIO Sound Device conforming to
 *
 * "Virtual I/O Device (VIRTIO) Version 1.2
 * Committee Specification Draft 01
 * 09 May 2022"
 *
 * Copyright (c) 2023 Emmanouil Pitsidianakis <manos.pitsidianakis@linaro.org>
 * Copyright (C) 2019 OpenSynergy GmbH
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * (at your option) any later version.  See the COPYING file in the
 * top-level directory.
 */

#ifndef QEMU_VIRTIO_SOUND_H
#define QEMU_VIRTIO_SOUND_H

#include "qemu/audio.h"
#include "qemu/timer.h"

#include "hw/virtio/virtio.h"
#include "standard-headers/linux/virtio_ids.h"
#include "standard-headers/linux/virtio_snd.h"

#define TYPE_VIRTIO_SND "virtio-sound-device"
#define VIRTIO_SND(obj) \
        OBJECT_CHECK(VirtIOSound, (obj), TYPE_VIRTIO_SND)

/* CONFIGURATION SPACE */

typedef struct virtio_snd_config virtio_snd_config;

/* COMMON DEFINITIONS */

/* common header for request/response*/
typedef struct virtio_snd_hdr virtio_snd_hdr;

/* event notification */
typedef struct virtio_snd_event virtio_snd_event;

/* common control request to query an item information */
typedef struct virtio_snd_query_info virtio_snd_query_info;

/* JACK CONTROL MESSAGES */

typedef struct virtio_snd_jack_hdr virtio_snd_jack_hdr;

/* jack information structure */
typedef struct virtio_snd_jack_info virtio_snd_jack_info;

/* jack remapping control request */
typedef struct virtio_snd_jack_remap virtio_snd_jack_remap;

/*
 * PCM CONTROL MESSAGES
 */
typedef struct virtio_snd_pcm_hdr virtio_snd_pcm_hdr;

/* PCM stream info structure */
typedef struct virtio_snd_pcm_info virtio_snd_pcm_info;

/* set PCM stream params */
typedef struct virtio_snd_pcm_set_params virtio_snd_pcm_set_params;

/* I/O request header */
typedef struct virtio_snd_pcm_xfer virtio_snd_pcm_xfer;

/* I/O request status */
typedef struct virtio_snd_pcm_status virtio_snd_pcm_status;

/* device structs */

typedef struct VirtIOSound VirtIOSound;

typedef struct VirtIOPcmParams VirtIOPcmParams;

typedef struct VirtIOSoundPCMStream VirtIOSoundPCMStream;

typedef struct VirtIOSoundPCMItem VirtIOSoundPCMItem;

typedef struct VirtIOSoundVqElems VirtIOSoundVqElems;

typedef struct VirtIOSoundPcmRingBuf VirtIOSoundPcmRingBuf;

struct VirtIOPcmParams {
    uint32_t buffer_bytes;
    uint32_t period_bytes;
    uint32_t features;
    uint8_t channels;
    uint8_t format;
    uint8_t rate;
};

struct VirtIOSoundVqElems {
    VirtQueueElement **data;
    uint16_t capacity;
    uint16_t size;
    uint16_t producer_pos;
    uint16_t consumer_pos;
};

struct VirtIOSoundPcmRingBuf {
    uint8_t *data;
    uint32_t capacity;
    uint32_t size;
    uint32_t producer_pos;
    uint32_t consumer_pos;
};

struct VirtIOSoundPCMStream {
    VirtIOSound *s;
    union {
        SWVoiceIn *in;
        SWVoiceOut *out;
        void *raw;
    } voice;
    void *silence_buf;  /* params.period_bytes of silence */
    VirtIOSoundPcmRingBuf pcm;
    QEMUTimer period_timer;
    uint32_t period_us;
    bool is_output;
    QemuMutex mtx;

    /* All the fields below are migratable. */
    VirtIOPcmParams params;
    VirtIOSoundVqElems vqelems;
    uint64_t next_period_us;
    uint16_t num_missed_periods;
};

struct VirtIOSoundPCMItem {
    /*
     * Set by VIRTIO_SND_R_PCM_SET_PARAMS.
     * The `stream` is not required to be populated.
     */
    VirtIOPcmParams params;

    /*
     * Created by *_PREPARE and destroyed by *_RELEASE.
     * Must be populated for *_START and *_STOP.
     */
    VirtIOSoundPCMStream *stream;
};

/*
 * PCM stream state machine.
 * -------------------------
 *
 * 5.14.6.6.1 PCM Command Lifecycle
 * ================================
 *
 * A PCM stream has the following command lifecycle:
 * - `SET PARAMETERS`
 *   The driver negotiates the stream parameters (format, transport, etc) with
 *   the device.
 *   Possible valid transitions: `SET PARAMETERS`, `PREPARE`.
 * - `PREPARE`
 *   The device prepares the stream (allocates resources, etc).
 *   Possible valid transitions: `SET PARAMETERS`, `PREPARE`, `START`,
 *   `RELEASE`. Output only: the driver transfers data for pre-buffing.
 * - `START`
 *   The device starts the stream (unmute, putting into running state, etc).
 *   Possible valid transitions: `STOP`.
 *   The driver transfers data to/from the stream.
 * - `STOP`
 *   The device stops the stream (mute, putting into non-running state, etc).
 *   Possible valid transitions: `START`, `RELEASE`.
 * - `RELEASE`
 *   The device releases the stream (frees resources, etc).
 *   Possible valid transitions: `SET PARAMETERS`, `PREPARE`.
 *
 * +---------------+ +---------+ +---------+ +-------+ +-------+
 * | SetParameters | | Prepare | | Release | | Start | | Stop  |
 * +---------------+ +---------+ +---------+ +-------+ +-------+
 *         |-             |           |          |         |
 *         ||             |           |          |         |
 *         |<             |           |          |         |
 *         |------------->|           |          |         |
 *         |<-------------|           |          |         |
 *         |              |-          |          |         |
 *         |              ||          |          |         |
 *         |              |<          |          |         |
 *         |              |--------------------->|         |
 *         |              |---------->|          |         |
 *         |              |           |          |-------->|
 *         |              |           |          |<--------|
 *         |              |           |<-------------------|
 *         |<-------------------------|          |         |
 *         |              |<----------|          |         |
 *
 * CTRL in the VirtIOSound device
 * ==============================
 *
 * The control messages that affect the state of a stream arrive in the
 * `virtio_snd_handle_ctrl()` queue callback and are of type `struct
 * virtio_snd_ctrl_command`. They are stored in a queue field in the device
 * type, `VirtIOSound`. This allows deferring the CTRL request completion if
 * it's not immediately possible due to locking/state reasons.
 *
 * The CTRL message is finally handled in `process_cmd()`.
 */
struct VirtIOSound {
    VirtIODevice parent_obj;

    VirtQueue *queues[VIRTIO_SND_VQ_MAX];
    uint64_t features;
    VirtIOSoundPCMItem *pcm_items;
    AudioBackend *audio_be;
    VMChangeStateEntry *vmstate;
    virtio_snd_config snd_conf;
};

#endif

/*
 * Copyright © 2016-2017 The TokTok team.
 * Copyright © 2014 Tox project.
 *
 * This file is part of Tox, the free peer to peer instant messenger.
 *
 * Tox is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Tox is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Tox.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#include "groupav.h"

#include <stdlib.h>
#include <string.h>

#include "../toxcore/logger.h"
#include "../toxcore/mono_time.h"
#include "../toxcore/util.h"

#define GROUP_JBUF_SIZE 6
#define GROUP_JBUF_DEAD_SECONDS 4

typedef struct {
    uint16_t sequnum;
    uint16_t length;
    uint8_t data[];
} Group_Audio_Packet;

typedef struct {
    Group_Audio_Packet **queue;
    uint32_t size;
    uint32_t capacity;
    uint16_t bottom;
    uint16_t top;
    uint64_t last_queued_time;
} Group_JitterBuffer;

static Group_JitterBuffer *create_queue(unsigned int capacity)
{
    unsigned int size = 1;

    while (size <= capacity) {
        size *= 2;
    }

    Group_JitterBuffer *q;

    if (!(q = (Group_JitterBuffer *)calloc(sizeof(Group_JitterBuffer), 1))) {
        return nullptr;
    }

    if (!(q->queue = (Group_Audio_Packet **)calloc(sizeof(Group_Audio_Packet *), size))) {
        free(q);
        return nullptr;
    }

    q->size = size;
    q->capacity = capacity;
    return q;
}

static void clear_queue(Group_JitterBuffer *q)
{
    for (; q->bottom != q->top; ++q->bottom) {
        if (q->queue[q->bottom % q->size]) {
            free(q->queue[q->bottom % q->size]);
            q->queue[q->bottom % q->size] = nullptr;
        }
    }
}

static void terminate_queue(Group_JitterBuffer *q)
{
    if (!q) {
        return;
    }

    clear_queue(q);
    free(q->queue);
    free(q);
}

/* Return 0 if packet was queued, -1 if it wasn't.
 */
static int queue(Group_JitterBuffer *q, Group_Audio_Packet *pk)
{
    uint16_t sequnum = pk->sequnum;

    unsigned int num = sequnum % q->size;

    if (!is_timeout(q->last_queued_time, GROUP_JBUF_DEAD_SECONDS)) {
        if ((uint32_t)(sequnum - q->bottom) > (1 << 15)) {
            /* Drop old packet. */
            return -1;
        }
    }

    if ((uint32_t)(sequnum - q->bottom) > q->size) {
        clear_queue(q);
        q->bottom = sequnum - q->capacity;
        q->queue[num] = pk;
        q->top = sequnum + 1;
        q->last_queued_time = unix_time();
        return 0;
    }

    if (q->queue[num]) {
        return -1;
    }

    q->queue[num] = pk;

    if ((sequnum - q->bottom) >= (q->top - q->bottom)) {
        q->top = sequnum + 1;
    }

    q->last_queued_time = unix_time();
    return 0;
}

/* success is 0 when there is nothing to dequeue, 1 when there's a good packet, 2 when there's a lost packet */
static Group_Audio_Packet *dequeue(Group_JitterBuffer *q, int *success)
{
    if (q->top == q->bottom) {
        *success = 0;
        return nullptr;
    }

    unsigned int num = q->bottom % q->size;

    if (q->queue[num]) {
        Group_Audio_Packet *ret = q->queue[num];
        q->queue[num] = nullptr;
        ++q->bottom;
        *success = 1;
        return ret;
    }

    if ((uint32_t)(q->top - q->bottom) > q->capacity) {
        ++q->bottom;
        *success = 2;
        return nullptr;
    }

    *success = 0;
    return nullptr;
}

typedef struct {
    const Logger *log;
    Group_Chats *g_c;
    OpusEncoder *audio_encoder;

    unsigned int audio_channels, audio_sample_rate, audio_bitrate;

    uint16_t audio_sequnum;

    void (*audio_data)(Messenger *m, uint32_t groupnumber, uint32_t peernumber, const int16_t *pcm, uint32_t samples,
                       uint8_t channels, unsigned int sample_rate, void *userdata);
    void *userdata;
} Group_AV;

typedef struct {
    Group_JitterBuffer *buffer;

    OpusDecoder *audio_decoder;
    int decoder_channels;
    unsigned int last_packet_samples;
} Group_Peer_AV;

static void kill_group_av(Group_AV *group_av)
{
    if (group_av->audio_encoder) {
        opus_encoder_destroy(group_av->audio_encoder);
    }

    free(group_av);
}

static int recreate_encoder(Group_AV *group_av)
{
    if (group_av->audio_encoder) {
        opus_encoder_destroy(group_av->audio_encoder);
        group_av->audio_encoder = nullptr;
    }

    int rc = OPUS_OK;
    group_av->audio_encoder = opus_encoder_create(group_av->audio_sample_rate, group_av->audio_channels,
                              OPUS_APPLICATION_AUDIO, &rc);

    if (rc != OPUS_OK) {
        LOGGER_ERROR(group_av->log, "Error while starting audio encoder: %s", opus_strerror(rc));
        group_av->audio_encoder = nullptr;
        return -1;
    }

    rc = opus_encoder_ctl(group_av->audio_encoder, OPUS_SET_BITRATE(group_av->audio_bitrate));

    if (rc != OPUS_OK) {
        LOGGER_ERROR(group_av->log, "Error while setting encoder ctl: %s", opus_strerror(rc));
        opus_encoder_destroy(group_av->audio_encoder);
        group_av->audio_encoder = nullptr;
        return -1;
    }

    rc = opus_encoder_ctl(group_av->audio_encoder, OPUS_SET_COMPLEXITY(10));

    if (rc != OPUS_OK) {
        LOGGER_ERROR(group_av->log, "Error while setting encoder ctl: %s", opus_strerror(rc));
        opus_encoder_destroy(group_av->audio_encoder);
        group_av->audio_encoder = nullptr;
        return -1;
    }

    return 0;
}

static Group_AV *new_group_av(const Logger *log, Group_Chats *g_c, void (*audio_callback)(Messenger *, uint32_t,
                              uint32_t, const int16_t *, unsigned int, uint8_t, uint32_t, void *), void *userdata)
{
    if (!g_c) {
        return nullptr;
    }

    Group_AV *group_av = (Group_AV *)calloc(1, sizeof(Group_AV));

    if (!group_av) {
        return nullptr;
    }

    group_av->log = log;
    group_av->g_c = g_c;

    group_av->audio_data = audio_callback;
    group_av->userdata = userdata;

    return group_av;
}

static void group_av_peer_new(void *object, uint32_t groupnumber, uint32_t friendgroupnumber)
{
    Group_AV *group_av = (Group_AV *)object;
    Group_Peer_AV *peer_av = (Group_Peer_AV *)calloc(1, sizeof(Group_Peer_AV));

    if (!peer_av) {
        return;
    }

    peer_av->buffer = create_queue(GROUP_JBUF_SIZE);
    group_peer_set_object(group_av->g_c, groupnumber, friendgroupnumber, peer_av);
}

static void group_av_peer_delete(void *object, uint32_t groupnumber, void *peer_object)
{
    Group_Peer_AV *peer_av = (Group_Peer_AV *)peer_object;

    if (!peer_av) {
        return;
    }

    if (peer_av->audio_decoder) {
        opus_decoder_destroy(peer_av->audio_decoder);
    }

    terminate_queue(peer_av->buffer);
    free(peer_object);
}

static void group_av_groupchat_delete(void *object, uint32_t groupnumber)
{
    if (object) {
        kill_group_av((Group_AV *)object);
    }
}

static int decode_audio_packet(Group_AV *group_av, Group_Peer_AV *peer_av, uint32_t groupnumber,
                               uint32_t friendgroupnumber)
{
    if (!group_av || !peer_av) {
        return -1;
    }

    int success;
    Group_Audio_Packet *pk = dequeue(peer_av->buffer, &success);

    if (success == 0) {
        return -1;
    }

    int16_t *out_audio = nullptr;
    int out_audio_samples = 0;

    unsigned int sample_rate = 48000;

    if (success == 1) {
        int channels = opus_packet_get_nb_channels(pk->data);

        if (channels == OPUS_INVALID_PACKET) {
            free(pk);
            return -1;
        }

        if (channels != 1 && channels != 2) {
            free(pk);
            return -1;
        }

        if (channels != peer_av->decoder_channels) {
            if (peer_av->audio_decoder) {
                opus_decoder_destroy(peer_av->audio_decoder);
                peer_av->audio_decoder = nullptr;
            }

            int rc;
            peer_av->audio_decoder = opus_decoder_create(sample_rate, channels, &rc);

            if (rc != OPUS_OK) {
                LOGGER_ERROR(group_av->log, "Error while starting audio decoder: %s", opus_strerror(rc));
                free(pk);
                return -1;
            }

            peer_av->decoder_channels = channels;
        }

        int num_samples = opus_decoder_get_nb_samples(peer_av->audio_decoder, pk->data, pk->length);

        out_audio = (int16_t *)malloc(num_samples * peer_av->decoder_channels * sizeof(int16_t));

        if (!out_audio) {
            free(pk);
            return -1;
        }

        out_audio_samples = opus_decode(peer_av->audio_decoder, pk->data, pk->length, out_audio, num_samples, 0);
        free(pk);

        if (out_audio_samples <= 0) {
            free(out_audio);
            return -1;
        }

        peer_av->last_packet_samples = out_audio_samples;
    } else {
        if (!peer_av->audio_decoder) {
            return -1;
        }

        if (!peer_av->last_packet_samples) {
            return -1;
        }

        out_audio = (int16_t *)malloc(peer_av->last_packet_samples * peer_av->decoder_channels * sizeof(int16_t));

        if (!out_audio) {
            free(pk);
            return -1;
        }

        out_audio_samples = opus_decode(peer_av->audio_decoder, nullptr, 0, out_audio, peer_av->last_packet_samples, 1);

        if (out_audio_samples <= 0) {
            free(out_audio);
            return -1;
        }
    }

    if (out_audio) {

        if (group_av->audio_data) {
            group_av->audio_data(group_av->g_c->m, groupnumber, friendgroupnumber, out_audio, out_audio_samples,
                                 peer_av->decoder_channels, sample_rate, group_av->userdata);
        }

        free(out_audio);
        return 0;
    }

    return -1;
}

static int handle_group_audio_packet(void *object, uint32_t groupnumber, uint32_t friendgroupnumber, void *peer_object,
                                     const uint8_t *packet, uint16_t length)
{
    if (!peer_object || !object || length <= sizeof(uint16_t)) {
        return -1;
    }

    Group_Peer_AV *peer_av = (Group_Peer_AV *)peer_object;

    Group_Audio_Packet *pk = (Group_Audio_Packet *)calloc(1, sizeof(Group_Audio_Packet) + (length - sizeof(uint16_t)));

    if (!pk) {
        return -1;
    }

    net_unpack_u16(packet, &pk->sequnum);
    pk->length = length - sizeof(uint16_t);
    memcpy(pk->data, packet + sizeof(uint16_t), pk->length);

    if (queue(peer_av->buffer, pk) == -1) {
        free(pk);
        return -1;
    }

    while (decode_audio_packet((Group_AV *)object, peer_av, groupnumber, friendgroupnumber) == 0) {
        ;
    }

    return 0;
}

/* Convert groupchat to an A/V groupchat.
 *
 * return 0 on success.
 * return -1 on failure.
 */
static int groupchat_enable_av(const Logger *log, Group_Chats *g_c, uint32_t groupnumber,
                               void (*audio_callback)(Messenger *,
                                       uint32_t, uint32_t, const int16_t *, unsigned int, uint8_t, uint32_t, void *), void *userdata)
{
    Group_AV *group_av = new_group_av(log, g_c, audio_callback, userdata);

    if (group_av == nullptr) {
        return -1;
    }

    if (group_set_object(g_c, groupnumber, group_av) == -1
            || callback_groupchat_peer_new(g_c, groupnumber, group_av_peer_new) == -1
            || callback_groupchat_peer_delete(g_c, groupnumber, group_av_peer_delete) == -1
            || callback_groupchat_delete(g_c, groupnumber, group_av_groupchat_delete) == -1) {
        kill_group_av(group_av);
        return -1;
    }

    group_lossy_packet_registerhandler(g_c, GROUP_AUDIO_PACKET_ID, &handle_group_audio_packet);
    return 0;
}

/* Create a new toxav group.
 *
 * return group number on success.
 * return -1 on failure.
 */
int add_av_groupchat(const Logger *log, Group_Chats *g_c, void (*audio_callback)(Messenger *, uint32_t, uint32_t,
                     const int16_t *, unsigned int, uint8_t, uint32_t, void *), void *userdata)
{
    int groupnumber = add_groupchat(g_c, GROUPCHAT_TYPE_AV);

    if (groupnumber == -1) {
        return -1;
    }

    if (groupchat_enable_av(log, g_c, groupnumber, audio_callback, userdata) == -1) {
        del_groupchat(g_c, groupnumber);
        return -1;
    }

    return groupnumber;
}

/* Join a AV group (you need to have been invited first.)
 *
 * returns group number on success
 * returns -1 on failure.
 */
int join_av_groupchat(const Logger *log, Group_Chats *g_c, uint32_t friendnumber, const uint8_t *data, uint16_t length,
                      void (*audio_callback)(Messenger *, uint32_t, uint32_t, const int16_t *, unsigned int, uint8_t, uint32_t, void *),
                      void *userdata)
{
    int groupnumber = join_groupchat(g_c, friendnumber, GROUPCHAT_TYPE_AV, data, length);

    if (groupnumber == -1) {
        return -1;
    }

    if (groupchat_enable_av(log, g_c, groupnumber, audio_callback, userdata) == -1) {
        del_groupchat(g_c, groupnumber);
        return -1;
    }

    return groupnumber;
}

/* Send an encoded audio packet to the group chat.
 *
 * return 0 on success.
 * return -1 on failure.
 */
static int send_audio_packet(Group_Chats *g_c, uint32_t groupnumber, uint8_t *packet, uint16_t length)
{
    if (length == 0 || length > MAX_CRYPTO_DATA_SIZE - 1 - sizeof(uint16_t)) {
        return -1;
    }

    const uint16_t plen = 1 + sizeof(uint16_t) + length;

    Group_AV *const group_av = (Group_AV *)group_get_object(g_c, groupnumber);

    if (!group_av) {
        return -1;
    }

    uint8_t data[MAX_CRYPTO_DATA_SIZE];
    uint8_t *ptr = data;
    *ptr++ = GROUP_AUDIO_PACKET_ID;

    ptr += net_pack_u16(ptr, group_av->audio_sequnum);
    memcpy(ptr, packet, length);

    if (send_group_lossy_packet(g_c, groupnumber, data, plen) == -1) {
        return -1;
    }

    ++group_av->audio_sequnum;
    return 0;
}

/* Send audio to the group chat.
 *
 * return 0 on success.
 * return -1 on failure.
 */
int group_send_audio(Group_Chats *g_c, uint32_t groupnumber, const int16_t *pcm, unsigned int samples, uint8_t channels,
                     uint32_t sample_rate)
{
    Group_AV *group_av = (Group_AV *)group_get_object(g_c, groupnumber);

    if (!group_av) {
        return -1;
    }

    if (channels != 1 && channels != 2) {
        return -1;
    }

    if (sample_rate != 8000 && sample_rate != 12000 && sample_rate != 16000 && sample_rate != 24000
            && sample_rate != 48000) {
        return -1;
    }

    if (!group_av->audio_encoder || group_av->audio_channels != channels || group_av->audio_sample_rate != sample_rate) {
        group_av->audio_channels = channels;
        group_av->audio_sample_rate = sample_rate;

        if (channels == 1) {
            group_av->audio_bitrate = 32000; // TODO(mannol): add way of adjusting bitrate
        } else {
            group_av->audio_bitrate = 64000; // TODO(mannol): add way of adjusting bitrate
        }

        if (recreate_encoder(group_av) == -1) {
            return -1;
        }
    }

    uint8_t encoded[1024];
    int32_t size = opus_encode(group_av->audio_encoder, pcm, samples, encoded, sizeof(encoded));

    if (size <= 0) {
        return -1;
    }

    return send_audio_packet(g_c, groupnumber, encoded, size);
}

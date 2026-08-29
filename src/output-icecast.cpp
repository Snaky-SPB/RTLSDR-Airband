/*
 * output-icecast.cpp
 * Icecast (libshout) output: connection setup, mp3 encode + send, reconnect
 *
 * Copyright (c) 2015-2021 Tomasz Lemiech <szpajder@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 */
#include <shout/shout.h>
#include <stdio.h>
#include <unistd.h>
#include <syslog.h>
#include <lame/lame.h>

// SHOUTERR_RETRY is available since libshout 2.4.0.
// Set it to an impossible value if it's not there.
#ifndef SHOUTERR_RETRY
#define SHOUTERR_RETRY (-255)
#endif /* SHOUTERR_RETRY */

#include "config.h"
#include "output-icecast.h"
#include "rtl_airband.h"

void shout_setup(icecast_data* icecast, mix_modes mixmode) {
    int ret;
    shout_t* shouttemp = shout_new();
    if (shouttemp == NULL) {
        printf("cannot allocate\n");
    }
    if (shout_set_host(shouttemp, icecast->hostname) != SHOUTERR_SUCCESS) {
        shout_free(shouttemp);
        return;
    }
    if (shout_set_protocol(shouttemp, SHOUT_PROTOCOL_HTTP) != SHOUTERR_SUCCESS) {
        shout_free(shouttemp);
        return;
    }
    if (shout_set_port(shouttemp, icecast->port) != SHOUTERR_SUCCESS) {
        shout_free(shouttemp);
        return;
    }
#ifdef LIBSHOUT_HAS_TLS
    if (shout_set_tls(shouttemp, icecast->tls_mode) != SHOUTERR_SUCCESS) {
        shout_free(shouttemp);
        return;
    }
#endif /* LIBSHOUT_HAS_TLS */
    char mp[100];
    sprintf(mp, "/%s", icecast->mountpoint);
    if (shout_set_mount(shouttemp, mp) != SHOUTERR_SUCCESS) {
        shout_free(shouttemp);
        return;
    }
    if (shout_set_user(shouttemp, icecast->username) != SHOUTERR_SUCCESS) {
        shout_free(shouttemp);
        return;
    }
    if (shout_set_password(shouttemp, icecast->password) != SHOUTERR_SUCCESS) {
        shout_free(shouttemp);
        return;
    }
#ifdef LIBSHOUT_HAS_CONTENT_FORMAT
    if (shout_set_content_format(shouttemp, SHOUT_FORMAT_MP3, SHOUT_USAGE_AUDIO, NULL) != SHOUTERR_SUCCESS) {
#else
    if (shout_set_format(shouttemp, SHOUT_FORMAT_MP3) != SHOUTERR_SUCCESS) {
#endif /* LIBSHOUT_HAS_CONTENT_FORMAT */
        shout_free(shouttemp);
        return;
    }
    if (icecast->name && shout_set_meta(shouttemp, SHOUT_META_NAME, icecast->name) != SHOUTERR_SUCCESS) {
        shout_free(shouttemp);
        return;
    }
    if (icecast->genre && shout_set_meta(shouttemp, SHOUT_META_GENRE, icecast->genre) != SHOUTERR_SUCCESS) {
        shout_free(shouttemp);
        return;
    }
    if (icecast->description && shout_set_meta(shouttemp, SHOUT_META_DESCRIPTION, icecast->description) != SHOUTERR_SUCCESS) {
        shout_free(shouttemp);
        return;
    }
    char samplerates[20];
    sprintf(samplerates, "%d", MP3_RATE);
    shout_set_audio_info(shouttemp, SHOUT_AI_SAMPLERATE, samplerates);
    shout_set_audio_info(shouttemp, SHOUT_AI_CHANNELS, (mixmode == MM_STEREO ? "2" : "1"));

    if (shout_set_nonblocking(shouttemp, 1) != SHOUTERR_SUCCESS) {
        log(LOG_ERR, "Error setting non-blocking mode: %s\n", shout_get_error(shouttemp));
        return;
    }
    ret = shout_open(shouttemp);
    if (ret == SHOUTERR_SUCCESS)
        ret = SHOUTERR_CONNECTED;

    if (ret == SHOUTERR_BUSY || ret == SHOUTERR_RETRY)
        log(LOG_NOTICE, "Connecting to %s:%d/%s...\n", icecast->hostname, icecast->port, icecast->mountpoint);

    int shout_timeout = 30 * 5;  // 30 * 5 * 200ms = 30s
    while ((ret == SHOUTERR_BUSY || ret == SHOUTERR_RETRY) && shout_timeout-- > 0) {
        SLEEP(200);
        ret = shout_get_connected(shouttemp);
    }

    if (ret == SHOUTERR_CONNECTED) {
        log(LOG_NOTICE, "Connected to %s:%d/%s\n", icecast->hostname, icecast->port, icecast->mountpoint);
        SLEEP(100);
        icecast->shout = shouttemp;
    } else {
        log(LOG_WARNING, "Could not connect to %s:%d/%s: %s\n", icecast->hostname, icecast->port, icecast->mountpoint, shout_get_error(shouttemp));
        shout_close(shouttemp);
        shout_free(shouttemp);
        return;
    }
}

void icecast_write(channel_t* channel, output_t* output, int cur_scan_freq) {
    icecast_data* icecast = (icecast_data*)(output->data);
    if (icecast->shout == NULL)
        return;

    // encode and send mp3 to shoutcast output
    const auto& lame = output->lame;
    const auto& lamebuf = output->lamebuf;
    int mp3_bytes = lame_encode_buffer_ieee_float(lame, channel->waveout, (channel->mode == MM_STEREO ? channel->waveout_r : NULL), WAVE_BATCH, lamebuf, LAMEBUF_SIZE);
    if (mp3_bytes < 0) {
        log(LOG_WARNING, "lame_encode_buffer_ieee_float: %d\n", mp3_bytes);
    }

    if (mp3_bytes == 0) {
        return;
    }

    int ret = shout_send(icecast->shout, output->lamebuf, mp3_bytes);

    if (ret != SHOUTERR_SUCCESS || shout_queuelen(icecast->shout) > MAX_SHOUT_QUEUELEN) {
        if (shout_queuelen(icecast->shout) > MAX_SHOUT_QUEUELEN)
            log(LOG_WARNING, "Exceeded max backlog for %s:%d/%s, disconnecting\n", icecast->hostname, icecast->port, icecast->mountpoint);
        // reset connection
        log(LOG_WARNING, "Lost connection to %s:%d/%s\n", icecast->hostname, icecast->port, icecast->mountpoint);
        shout_close(icecast->shout);
        shout_free(icecast->shout);
        icecast->shout = NULL;
    } else if (icecast->send_scan_freq_tags && cur_scan_freq >= 0) {
        shout_metadata_t* meta = shout_metadata_new();
        char description[32];
        if (channel->freqlist[channel->freq_idx].label != NULL) {
            if (shout_metadata_add(meta, "song", channel->freqlist[channel->freq_idx].label) != SHOUTERR_SUCCESS) {
                log(LOG_WARNING, "Failed to add shout metadata\n");
            }
        } else {
            snprintf(description, sizeof(description), "%.3f MHz", channel->freqlist[channel->freq_idx].frequency / 1000000.0);
            if (shout_metadata_add(meta, "song", description) != SHOUTERR_SUCCESS) {
                log(LOG_WARNING, "Failed to add shout metadata\n");
            }
        }
        if (SHOUT_SET_METADATA(icecast->shout, meta) != SHOUTERR_SUCCESS) {
            log(LOG_WARNING, "Failed to add shout metadata\n");
        }
        shout_metadata_free(meta);
    }
}

void icecast_close(icecast_data* icecast) {
    if (icecast->shout == NULL)
        return;
    log(LOG_WARNING, "Closing connection to %s:%d/%s\n", icecast->hostname, icecast->port, icecast->mountpoint);
    shout_close(icecast->shout);
    shout_free(icecast->shout);
    icecast->shout = NULL;
}

void icecast_check(icecast_data* icecast, input_state_t state, mix_modes mode, int device_idx) {
    if (state == INPUT_FAILED) {
        if (icecast->shout) {
            log(LOG_WARNING, "Device #%d failed, disconnecting stream %s:%d/%s\n", device_idx, icecast->hostname, icecast->port, icecast->mountpoint);
            shout_close(icecast->shout);
            shout_free(icecast->shout);
            icecast->shout = NULL;
        }
    } else if (state == INPUT_RUNNING) {
        if (icecast->shout == NULL) {
            log(LOG_NOTICE, "Trying to reconnect to %s:%d/%s...\n", icecast->hostname, icecast->port, icecast->mountpoint);
            shout_setup(icecast, mode);
        }
    }
}

/*
 * output.cpp
 * Output related routines
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
#include <math.h>
#include <ogg/ogg.h>
#include <shout/shout.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <vorbis/vorbisenc.h>

// SHOUTERR_RETRY is available since libshout 2.4.0.
// Set it to an impossible value if it's not there.
#ifndef SHOUTERR_RETRY
#define SHOUTERR_RETRY (-255)
#endif /* SHOUTERR_RETRY */

#include <lame/lame.h>

#ifdef WITH_PULSEAUDIO
#include <pulse/pulseaudio.h>
#endif /* WITH_PULSEAUDIO */

#include <syslog.h>
#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sstream>
#include <string>
#include "config.h"
#include "file_output.h"
#include "helper_functions.h"
#include "input-common.h"
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

// Create all the output for a particular channel.
void process_outputs(channel_t* channel, int cur_scan_freq) {
    for (int k = 0; k < channel->output_count; k++) {
        if (channel->outputs[k].enabled == false)
            continue;
        if (channel->outputs[k].type == O_ICECAST) {
            icecast_data* icecast = (icecast_data*)(channel->outputs[k].data);
            if (icecast->shout == NULL)
                continue;

            // encode and send mp3 to shoutcast output
            const auto& lame = channel->outputs[k].lame;
            const auto& lamebuf = channel->outputs[k].lamebuf;
            int mp3_bytes = lame_encode_buffer_ieee_float(lame, channel->waveout, (channel->mode == MM_STEREO ? channel->waveout_r : NULL), WAVE_BATCH, lamebuf, LAMEBUF_SIZE);
            if (mp3_bytes < 0) {
                log(LOG_WARNING, "lame_encode_buffer_ieee_float: %d\n", mp3_bytes);
            }

            if (mp3_bytes == 0) {
                continue;
            }

            int ret = shout_send(icecast->shout, channel->outputs[k].lamebuf, mp3_bytes);

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
        } else if (channel->outputs[k].type == O_FILE || channel->outputs[k].type == O_RAWFILE) {
            file_write(channel, &channel->outputs[k]);
        } else if (channel->outputs[k].type == O_MIXER) {
            mixer_data* mdata = (mixer_data*)(channel->outputs[k].data);
            mixer_put_samples(mdata->mixer, mdata->input, channel->waveout, channel->axcindicate != NO_SIGNAL, WAVE_BATCH);
        } else if (channel->outputs[k].type == O_UDP_STREAM) {
            udp_stream_data* sdata = (udp_stream_data*)channel->outputs[k].data;

            if (sdata->continuous == false && channel->axcindicate == NO_SIGNAL) {
                continue;
            }

            if (channel->mode == MM_MONO) {
                udp_stream_write(sdata, channel->waveout, (size_t)WAVE_BATCH * sizeof(float));
            } else {
                udp_stream_write(sdata, channel->waveout, channel->waveout_r, (size_t)WAVE_BATCH * sizeof(float));
            }

#ifdef WITH_PULSEAUDIO
        } else if (channel->outputs[k].type == O_PULSE) {
            pulse_data* pdata = (pulse_data*)(channel->outputs[k].data);
            if (pdata->continuous == false && channel->axcindicate == NO_SIGNAL)
                continue;

            pulse_write_stream(pdata, channel->mode, channel->waveout, channel->waveout_r, (size_t)WAVE_BATCH * sizeof(float));
#endif /* WITH_PULSEAUDIO */
        }
    }
}

void disable_channel_outputs(channel_t* channel) {
    for (int k = 0; k < channel->output_count; k++) {
        output_t* output = channel->outputs + k;
        output->enabled = false;
        if (output->type == O_ICECAST) {
            icecast_data* icecast = (icecast_data*)(channel->outputs[k].data);
            if (icecast->shout == NULL)
                continue;
            log(LOG_WARNING, "Closing connection to %s:%d/%s\n", icecast->hostname, icecast->port, icecast->mountpoint);
            shout_close(icecast->shout);
            shout_free(icecast->shout);
            icecast->shout = NULL;
        } else if (output->type == O_FILE || output->type == O_RAWFILE) {
            close_file(&channel->outputs[k]);
        } else if (output->type == O_MIXER) {
            mixer_data* mdata = (mixer_data*)(output->data);
            mixer_disable_input(mdata->mixer, mdata->input);
        } else if (output->type == O_UDP_STREAM) {
            udp_stream_data* sdata = (udp_stream_data*)output->data;
            udp_stream_shutdown(sdata);
#ifdef WITH_PULSEAUDIO
        } else if (output->type == O_PULSE) {
            pulse_data* pdata = (pulse_data*)(output->data);
            pulse_shutdown(pdata);
#endif /* WITH_PULSEAUDIO */
        }
    }
}

void disable_device_outputs(device_t* dev) {
    log(LOG_INFO, "Disabling device outputs\n");
    for (int j = 0; j < dev->channel_count; j++) {
        disable_channel_outputs(dev->channels + j);
    }
}

static void print_channel_metric(FILE* f, char const* name, float freq, char* label) {
    fprintf(f, "%s{freq=\"%.3f\"", name, freq / 1000000.0);
    if (label != NULL) {
        fprintf(f, ",label=\"%s\"", label);
    }
    fprintf(f, "}");
}

static void output_channel_noise_levels(FILE* f) {
    fprintf(f,
            "# HELP channel_noise_level Raw squelch noise_level.\n"
            "# TYPE channel_noise_level gauge\n");

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = devices[i].channels + j;
            for (int k = 0; k < channel->freq_count; k++) {
                print_channel_metric(f, "channel_noise_level", channel->freqlist[k].frequency, channel->freqlist[k].label);
                fprintf(f, "\t%.3f\n", channel->freqlist[k].squelch.noise_level());
            }
        }
    }
    fprintf(f, "\n");
}

static void output_channel_dbfs_noise_levels(FILE* f) {
    fprintf(f,
            "# HELP channel_dbfs_noise_level Squelch noise_level as dBFS.\n"
            "# TYPE channel_dbfs_noise_level gauge\n");

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = devices[i].channels + j;
            for (int k = 0; k < channel->freq_count; k++) {
                print_channel_metric(f, "channel_dbfs_noise_level", channel->freqlist[k].frequency, channel->freqlist[k].label);
                fprintf(f, "\t%.3f\n", level_to_dBFS(channel->freqlist[k].squelch.noise_level()));
            }
        }
    }
    fprintf(f, "\n");
}

static void output_channel_signal_levels(FILE* f) {
    fprintf(f,
            "# HELP channel_signal_level Raw squelch signal_level.\n"
            "# TYPE channel_signal_level gauge\n");

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = devices[i].channels + j;
            for (int k = 0; k < channel->freq_count; k++) {
                print_channel_metric(f, "channel_signal_level", channel->freqlist[k].frequency, channel->freqlist[k].label);
                fprintf(f, "\t%.3f\n", channel->freqlist[k].squelch.signal_level());
            }
        }
    }
    fprintf(f, "\n");
}

static void output_channel_dbfs_signal_levels(FILE* f) {
    fprintf(f,
            "# HELP channel_dbfs_signal_level Squelch signal_level as dBFS.\n"
            "# TYPE channel_dbfs_signal_level gauge\n");

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = devices[i].channels + j;
            for (int k = 0; k < channel->freq_count; k++) {
                print_channel_metric(f, "channel_dbfs_signal_level", channel->freqlist[k].frequency, channel->freqlist[k].label);
                fprintf(f, "\t%.3f\n", level_to_dBFS(channel->freqlist[k].squelch.signal_level()));
            }
        }
    }
    fprintf(f, "\n");
}

static void output_channel_squelch_levels(FILE* f) {
    fprintf(f,
            "# HELP channel_squelch_level Squelch squelch_level.\n"
            "# TYPE channel_squelch_level gauge\n");

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = devices[i].channels + j;
            for (int k = 0; k < channel->freq_count; k++) {
                print_channel_metric(f, "channel_squelch_level", channel->freqlist[k].frequency, channel->freqlist[k].label);
                fprintf(f, "\t%.3f\n", channel->freqlist[k].squelch.squelch_level());
            }
        }
    }
    fprintf(f, "\n");
}

static void output_channel_squelch_counter(FILE* f) {
    fprintf(f,
            "# HELP channel_squelch_counter Squelch open_count.\n"
            "# TYPE channel_squelch_counter counter\n");

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = devices[i].channels + j;
            for (int k = 0; k < channel->freq_count; k++) {
                print_channel_metric(f, "channel_squelch_counter", channel->freqlist[k].frequency, channel->freqlist[k].label);
                fprintf(f, "\t%zu\n", channel->freqlist[k].squelch.open_count());
            }
        }
    }
    fprintf(f, "\n");
}

static void output_channel_flappy_counter(FILE* f) {
    fprintf(f,
            "# HELP channel_flappy_counter Squelch flappy_count.\n"
            "# TYPE channel_flappy_counter counter\n");

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = devices[i].channels + j;
            for (int k = 0; k < channel->freq_count; k++) {
                print_channel_metric(f, "channel_flappy_counter", channel->freqlist[k].frequency, channel->freqlist[k].label);
                fprintf(f, "\t%zu\n", channel->freqlist[k].squelch.flappy_count());
            }
        }
    }
    fprintf(f, "\n");
}

static void output_channel_ctcss_counter(FILE* f) {
    fprintf(f,
            "# HELP channel_ctcss_counter count of windows with CTCSS detected.\n"
            "# TYPE channel_ctcss_counter counter\n");

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = devices[i].channels + j;
            for (int k = 0; k < channel->freq_count; k++) {
                print_channel_metric(f, "channel_ctcss_counter", channel->freqlist[k].frequency, channel->freqlist[k].label);
                fprintf(f, "\t%zu\n", channel->freqlist[k].squelch.ctcss_count());
            }
        }
    }
    fprintf(f, "\n");
}

static void output_channel_no_ctcss_counter(FILE* f) {
    fprintf(f,
            "# HELP channel_no_ctcss_counter count of windows without CTCSS detected.\n"
            "# TYPE channel_no_ctcss_counter counter\n");

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = devices[i].channels + j;
            for (int k = 0; k < channel->freq_count; k++) {
                print_channel_metric(f, "channel_no_ctcss_counter", channel->freqlist[k].frequency, channel->freqlist[k].label);
                fprintf(f, "\t%zu\n", channel->freqlist[k].squelch.no_ctcss_count());
            }
        }
    }
    fprintf(f, "\n");
}

static void output_channel_activity_counters(FILE* f) {
    fprintf(f,
            "# HELP channel_activity_counter Loops of output_thread with frequency active.\n"
            "# TYPE channel_activity_counter counter\n");

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = devices[i].channels + j;
            for (int k = 0; k < channel->freq_count; k++) {
                print_channel_metric(f, "channel_activity_counter", channel->freqlist[k].frequency, channel->freqlist[k].label);
                fprintf(f, "\t%zu\n", channel->freqlist[k].active_counter);
            }
        }
    }
    fprintf(f, "\n");
}

static void output_device_buffer_overflows(FILE* f) {
    fprintf(f,
            "# HELP buffer_overflow_count Number of times a device's buffer has overflowed.\n"
            "# TYPE buffer_overflow_count counter\n");

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        fprintf(f, "buffer_overflow_count{device=\"%d\"}\t%zu\n", i, dev->input->overflow_count);
    }
    fprintf(f, "\n");
}

static void output_output_overruns(FILE* f) {
    fprintf(f,
            "# HELP output_overrun_count Number of times a device or mixer output has overrun.\n"
            "# TYPE output_overrun_count counter\n");

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        fprintf(f, "output_overrun_count{device=\"%d\"}\t%zu\n", i, dev->output_overrun_count);
    }
    for (int i = 0; i < mixer_count; i++) {
        mixer_t* mixer = mixers + i;
        fprintf(f, "output_overrun_count{mixer=\"%d\"}\t%zu\n", i, mixer->output_overrun_count);
    }
    fprintf(f, "\n");
}

static void output_input_overruns(FILE* f) {
    if (mixer_count == 0) {
        return;
    }

    fprintf(f,
            "# HELP input_overrun_count Number of times mixer input has overrun.\n"
            "# TYPE input_overrun_count counter\n");

    for (int i = 0; i < mixer_count; i++) {
        mixer_t* mixer = mixers + i;
        for (int j = 0; j < mixer->input_count; j++) {
            mixinput_t* input = mixer->inputs + j;
            fprintf(f, "input_overrun_count{mixer=\"%d\",input=\"%d\"}\t%zu\n", i, j, input->input_overrun_count);
        }
    }
    fprintf(f, "\n");
}

void write_stats_file(timeval* last_stats_write) {
    if (!stats_filepath) {
        return;
    }

    timeval current_time;
    gettimeofday(&current_time, NULL);

    static const double STATS_FILE_TIMING = 15.0;
    if (!do_exit && delta_sec(last_stats_write, &current_time) < STATS_FILE_TIMING) {
        return;
    }

    *last_stats_write = current_time;

    FILE* file = fopen(stats_filepath, "w");
    if (!file) {
        log(LOG_WARNING, "Cannot open output file %s (%s)\n", stats_filepath, strerror(errno));
        return;
    }

    output_channel_activity_counters(file);
    output_channel_noise_levels(file);
    output_channel_dbfs_noise_levels(file);
    output_channel_signal_levels(file);
    output_channel_dbfs_signal_levels(file);
    output_channel_squelch_counter(file);
    output_channel_squelch_levels(file);
    output_channel_flappy_counter(file);
    output_channel_ctcss_counter(file);
    output_channel_no_ctcss_counter(file);
    output_device_buffer_overflows(file);
    output_output_overruns(file);
    output_input_overruns(file);

    fclose(file);
}

void* output_thread(void* param) {
    assert(param != NULL);
    output_params_t* output_param = (output_params_t*)param;
    struct freq_tag tag;
    struct timeval tv;
    int new_freq = -1;
    timeval last_stats_write = {0, 0};

    debug_print("Starting output thread, devices %d:%d, mixers %d:%d, signal %p\n", output_param->device_start, output_param->device_end, output_param->mixer_start, output_param->mixer_end,
                output_param->mp3_signal);

#ifdef DEBUG
    timeval ts, te;
    gettimeofday(&ts, NULL);
#endif /* DEBUG */
    while (!do_exit) {
        output_param->mp3_signal->wait();
        for (int i = output_param->mixer_start; i < output_param->mixer_end; i++) {
            if (mixers[i].enabled == false)
                continue;
            channel_t* channel = &mixers[i].channel;
            if (channel->state == CH_READY) {
                process_outputs(channel, -1);
                channel->state = CH_DIRTY;
            }
        }
#ifdef DEBUG
        gettimeofday(&te, NULL);
        debug_bulk_print("mixeroutput: %lu.%lu %lu\n", te.tv_sec, (unsigned long)te.tv_usec, (te.tv_sec - ts.tv_sec) * 1000000UL + te.tv_usec - ts.tv_usec);
        ts.tv_sec = te.tv_sec;
        ts.tv_usec = te.tv_usec;
#endif /* DEBUG */
        for (int i = output_param->device_start; i < output_param->device_end; i++) {
            device_t* dev = devices + i;
            if (dev->waveavail) {
                if (dev->mode == R_SCAN) {
                    tag_queue_get(dev, &tag);
                    if (tag.freq >= 0) {
                        tag.tv.tv_sec += shout_metadata_delay;
                        gettimeofday(&tv, NULL);
                        if (tag.tv.tv_sec < tv.tv_sec || (tag.tv.tv_sec == tv.tv_sec && tag.tv.tv_usec <= tv.tv_usec)) {
                            new_freq = tag.freq;
                            tag_queue_advance(dev);
                        }
                    }
                }
                for (int j = 0; j < dev->channel_count; j++) {
                    channel_t* channel = devices[i].channels + j;
                    process_outputs(channel, new_freq);
                    memcpy(channel->waveout, channel->waveout + WAVE_BATCH, AGC_EXTRA * 4);
                }
                dev->waveavail = 0;
            }
            // make sure we don't carry new_freq value to the next receiver which might be working
            // in multichannel mode
            new_freq = -1;
        }
        if (output_param->device_start == 0) {
            write_stats_file(&last_stats_write);
        }
    }

    // waveavail=1 set by the demod just before do_exit may have been missed if
    // the final pthread_cond_signal fired while the output thread was busy (signal
    // lost) or if do_exit was checked before the signal was consumed.
    for (int i = output_param->device_start; i < output_param->device_end; i++) {
        device_t* dev = devices + i;
        if (!dev->waveavail)
            continue;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = devices[i].channels + j;
            process_outputs(channel, -1);
            memcpy(channel->waveout, channel->waveout + WAVE_BATCH, AGC_EXTRA * 4);
        }
        dev->waveavail = 0;
    }

    // Final stats flush: do_exit=1 bypasses the 15s throttle, so this always
    // writes regardless of run duration. Called after the post-loop flush above
    // so counters reflect all processed output.
    if (output_param->device_start == 0) {
        write_stats_file(&last_stats_write);
    }

    return 0;
}

// reconnect as required
void* output_check_thread(void*) {
    while (!do_exit) {
        SLEEP(10000);
        for (int i = 0; i < device_count; i++) {
            device_t* dev = devices + i;
            for (int j = 0; j < dev->channel_count; j++) {
                for (int k = 0; k < dev->channels[j].output_count; k++) {
                    if (dev->channels[j].outputs[k].type == O_ICECAST) {
                        icecast_data* icecast = (icecast_data*)(dev->channels[j].outputs[k].data);
                        if (dev->input->state == INPUT_FAILED) {
                            if (icecast->shout) {
                                log(LOG_WARNING, "Device #%d failed, disconnecting stream %s:%d/%s\n", i, icecast->hostname, icecast->port, icecast->mountpoint);
                                shout_close(icecast->shout);
                                shout_free(icecast->shout);
                                icecast->shout = NULL;
                            }
                        } else if (dev->input->state == INPUT_RUNNING) {
                            if (icecast->shout == NULL) {
                                log(LOG_NOTICE, "Trying to reconnect to %s:%d/%s...\n", icecast->hostname, icecast->port, icecast->mountpoint);
                                shout_setup(icecast, dev->channels[j].mode);
                            }
                        }
                    } else if (dev->channels[j].outputs[k].type == O_UDP_STREAM) {
                        udp_stream_data* sdata = (udp_stream_data*)dev->channels[j].outputs[k].data;

                        if (dev->input->state == INPUT_FAILED) {
                            udp_stream_shutdown(sdata);
                        }
#ifdef WITH_PULSEAUDIO
                    } else if (dev->channels[j].outputs[k].type == O_PULSE) {
                        pulse_data* pdata = (pulse_data*)(dev->channels[j].outputs[k].data);
                        if (dev->input->state == INPUT_FAILED) {
                            if (pdata->context) {
                                pulse_shutdown(pdata);
                            }
                        } else if (dev->input->state == INPUT_RUNNING) {
                            if (pdata->context == NULL) {
                                pulse_setup(pdata, dev->channels[j].mode);
                            }
                        }
#endif /* WITH_PULSEAUDIO */
                    }
                }
            }
        }
        for (int i = 0; i < mixer_count; i++) {
            if (mixers[i].enabled == false)
                continue;
            for (int k = 0; k < mixers[i].channel.output_count; k++) {
                if (mixers[i].channel.outputs[k].enabled == false)
                    continue;
                if (mixers[i].channel.outputs[k].type == O_ICECAST) {
                    icecast_data* icecast = (icecast_data*)(mixers[i].channel.outputs[k].data);
                    if (icecast->shout == NULL) {
                        log(LOG_NOTICE, "Trying to reconnect to %s:%d/%s...\n", icecast->hostname, icecast->port, icecast->mountpoint);
                        shout_setup(icecast, mixers[i].channel.mode);
                    }
#ifdef WITH_PULSEAUDIO
                } else if (mixers[i].channel.outputs[k].type == O_PULSE) {
                    pulse_data* pdata = (pulse_data*)(mixers[i].channel.outputs[k].data);
                    if (pdata->context == NULL) {
                        pulse_setup(pdata, mixers[i].channel.mode);
                    }
#endif /* WITH_PULSEAUDIO */
                }
            }
        }
    }
    return 0;
}

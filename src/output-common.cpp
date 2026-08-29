/*
 * output-common.cpp
 * Common output routines: per-type dispatch, output threads, stats file
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
#include <stdio.h>
#include <sys/time.h>
#include <unistd.h>
#include <syslog.h>
#include <cassert>
#include <cerrno>
#include <cstring>
#include "config.h"
#include "input-common.h"
#include "output-common.h"
#include "output-file.h"
#include "output-icecast.h"
#include "output-pulse.h"
#include "output-udp.h"
#include "rtl_airband.h"

// Create all the output for a particular channel.
void process_outputs(channel_t* channel, int cur_scan_freq) {
    for (int k = 0; k < channel->output_count; k++) {
        if (channel->outputs[k].enabled == false)
            continue;
        if (channel->outputs[k].type == O_ICECAST) {
            icecast_write(channel, &channel->outputs[k], cur_scan_freq);
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
            icecast_close((icecast_data*)output->data);
        } else if (output->type == O_FILE || output->type == O_RAWFILE) {
            close_file(output);
        } else if (output->type == O_MIXER) {
            mixer_data* mdata = (mixer_data*)(output->data);
            mixer_disable_input(mdata->mixer, mdata->input);
        } else if (output->type == O_UDP_STREAM) {
            udp_stream_shutdown((udp_stream_data*)output->data);
#ifdef WITH_PULSEAUDIO
        } else if (output->type == O_PULSE) {
            pulse_shutdown((pulse_data*)output->data);
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
                channel_t* channel = dev->channels + j;
                for (int k = 0; k < channel->output_count; k++) {
                    output_t* output = channel->outputs + k;
                    if (output->type == O_ICECAST) {
                        icecast_check((icecast_data*)output->data, dev->input->state, channel->mode, i);
                    } else if (output->type == O_UDP_STREAM) {
                        udp_stream_check((udp_stream_data*)output->data, dev->input->state);
#ifdef WITH_PULSEAUDIO
                    } else if (output->type == O_PULSE) {
                        pulse_check((pulse_data*)output->data, dev->input->state, channel->mode);
#endif /* WITH_PULSEAUDIO */
                    }
                }
            }
        }
        for (int i = 0; i < mixer_count; i++) {
            if (mixers[i].enabled == false)
                continue;
            channel_t* channel = &mixers[i].channel;
            for (int k = 0; k < channel->output_count; k++) {
                output_t* output = channel->outputs + k;
                if (output->enabled == false)
                    continue;
                if (output->type == O_ICECAST) {
                    icecast_check((icecast_data*)output->data, INPUT_RUNNING, channel->mode, -1);
#ifdef WITH_PULSEAUDIO
                } else if (output->type == O_PULSE) {
                    pulse_check((pulse_data*)output->data, INPUT_RUNNING, channel->mode);
#endif /* WITH_PULSEAUDIO */
                }
            }
        }
    }
    return 0;
}

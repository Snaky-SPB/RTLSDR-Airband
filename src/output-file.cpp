/*
 * output-file.cpp
 * File output (mp3/raw) open/append support
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
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sstream>
#include <string>

#include <lame/lame.h>

#include "config.h"
#include "output-file.h"
#include "helper_functions.h"

lame_t airlame_init(mix_modes mixmode, int highpass, int lowpass) {
    lame_t lame = lame_init();
    if (!lame) {
        log(LOG_WARNING, "lame_init failed\n");
        return NULL;
    }

    lame_set_in_samplerate(lame, WAVE_RATE);
    lame_set_VBR(lame, vbr_mtrh);
    lame_set_brate(lame, 16);
    lame_set_quality(lame, 7);
    lame_set_lowpassfreq(lame, lowpass);
    lame_set_highpassfreq(lame, highpass);
    lame_set_out_samplerate(lame, MP3_RATE);
    if (mixmode == MM_STEREO) {
        lame_set_num_channels(lame, 2);
        lame_set_mode(lame, JOINT_STEREO);
    } else {
        lame_set_num_channels(lame, 1);
        lame_set_mode(lame, MONO);
    }
    debug_print("lame init with mixmode=%s\n", mixmode == MM_STEREO ? "MM_STEREO" : "MM_MONO");
    lame_init_params(lame);
    return lame;
}

class LameTone {
    unsigned char* _data;
    int _bytes;

   public:
    LameTone(mix_modes mixmode, int msec, unsigned int hz = 0) : _data(NULL), _bytes(0) {
        _data = (unsigned char*)XCALLOC(1, LAMEBUF_SIZE);

        int samples = (msec * WAVE_RATE) / 1000;
        float* buf = (float*)XCALLOC(samples, sizeof(float));

        debug_print("LameTone with mixmode=%s msec=%d hz=%u\n", mixmode == MM_STEREO ? "MM_STEREO" : "MM_MONO", msec, hz);
        if (hz > 0) {
            const float period = 1.0 / (float)hz;
            const float sample_time = 1.0 / (float)WAVE_RATE;
            float t = 0;
            for (int i = 0; i < samples; ++i, t += sample_time) {
                buf[i] = 0.9 * sinf(t * 2.0 * M_PI / period);
            }
        } else
            memset(buf, 0, samples * sizeof(float));
        lame_t lame = airlame_init(mixmode, 0, 0);
        if (lame) {
            _bytes = lame_encode_buffer_ieee_float(lame, buf, (mixmode == MM_STEREO ? buf : NULL), samples, _data, LAMEBUF_SIZE);
            if (_bytes > 0) {
                int flush_ofs = _bytes;
                if (flush_ofs & 0x1f)
                    flush_ofs += 0x20 - (flush_ofs & 0x1f);
                if (flush_ofs < LAMEBUF_SIZE) {
                    int flush_bytes = lame_encode_flush(lame, _data + flush_ofs, LAMEBUF_SIZE - flush_ofs);
                    if (flush_bytes > 0) {
                        memmove(_data + _bytes, _data + flush_ofs, flush_bytes);
                        _bytes += flush_bytes;
                    }
                }
            } else
                log(LOG_WARNING, "lame_encode_buffer_ieee_float: %d\n", _bytes);
            lame_close(lame);
        }
        free(buf);
    }

    ~LameTone() {
        if (_data)
            free(_data);
    }

    int write(FILE* f) {
        if (!_data || _bytes <= 0)
            return 1;

        if (fwrite(_data, 1, _bytes, f) != (unsigned int)_bytes) {
            log(LOG_WARNING, "LameTone: failed to write %d bytes\n", _bytes);
            return -1;
        }

        return 0;
    }
};

int rename_if_exists(char const* oldpath, char const* newpath) {
    int ret = rename(oldpath, newpath);
    if (ret < 0) {
        if (errno == ENOENT) {
            return 0;
        } else {
            log(LOG_ERR, "Could not rename %s to %s: %s\n", oldpath, newpath, strerror(errno));
        }
    }
    return ret;
}

/*
 * Open output file (mp3 or raw IQ) for append or initial write.
 * If appending to an audio file, insert discontinuity indictor tones
 * as well as the appropriate amount of silence when in continuous mode.
 */
int open_file(file_data* fdata, mix_modes mixmode, int is_audio) {
    int rename_result = rename_if_exists(fdata->file_path.c_str(), fdata->file_path_tmp.c_str());
    if (fdata->append) {
        // Open without O_APPEND so fseek+fwrite works correctly (e.g. for the lametag).
        // Try r+ (existing file) first; fall back to w (new file) if it doesn't exist yet.
        fdata->f = fopen(fdata->file_path_tmp.c_str(), "r+");
        if (fdata->f == NULL) {
            fdata->f = fopen(fdata->file_path_tmp.c_str(), "w");
        } else {
            fseek(fdata->f, 0, SEEK_END);
        }
    } else {
        fdata->f = fopen(fdata->file_path_tmp.c_str(), "w");
    }
    if (fdata->f == NULL) {
        return -1;
    }

    struct stat st = {};
    if (!fdata->append || fstat(fileno(fdata->f), &st) != 0 || st.st_size == 0) {
        if (!fdata->split_on_transmission) {
            log(LOG_INFO, "Writing to %s\n", fdata->file_path.c_str());
        } else {
            debug_print("Writing to %s\n", fdata->file_path_tmp.c_str());
        }
        return 0;
    }
    if (rename_result < 0) {
        log(LOG_INFO, "Writing to %s\n", fdata->file_path.c_str());
        debug_print("Writing to %s\n", fdata->file_path_tmp.c_str());
    } else {
        log(LOG_INFO, "Appending from pos %llu to %s\n", (unsigned long long)st.st_size, fdata->file_path.c_str());
        debug_print("Appending from pos %llu to %s\n", (unsigned long long)st.st_size, fdata->file_path_tmp.c_str());
    }

    if (is_audio) {
        // fill missing space with marker tones
        LameTone lt_a(mixmode, 120, 2222);
        LameTone lt_b(mixmode, 120, 1111);
        LameTone lt_c(mixmode, 120, 555);

        int r = 0;
        if (fdata->discontinuity_tone) {
            r = lt_a.write(fdata->f);
            if (r == 0)
                r = lt_b.write(fdata->f);
            if (r == 0)
                r = lt_c.write(fdata->f);
        }

        // fill in time delta with silence if continuous output mode
        if (fdata->continuous) {
            time_t now = time(NULL);
            if (now > st.st_mtime) {
                time_t delta = now - st.st_mtime;
                if (delta > 3600) {
                    log(LOG_WARNING, "Too big time difference: %llu sec, limiting to one hour\n", (unsigned long long)delta);
                    delta = 3600;
                }
                LameTone lt_silence(mixmode, 1000);
                for (; (r == 0 && delta > 1); --delta)
                    r = lt_silence.write(fdata->f);
            }
        }

        if (fdata->discontinuity_tone) {
            if (r == 0)
                r = lt_c.write(fdata->f);
            if (r == 0)
                r = lt_b.write(fdata->f);
            if (r == 0)
                r = lt_a.write(fdata->f);
        }

        if (r < 0)
            fseek(fdata->f, st.st_size, SEEK_SET);
    }
    return 0;
}

void close_file(output_t* output) {
    file_data* fdata = (file_data*)(output->data);
    if (!fdata) {
        return;
    }

    // close all mp3 files for every output that has a lame context
    if (fdata->type == O_FILE && fdata->f && output->lame) {
        int encoded = lame_encode_flush_nogap(output->lame, output->lamebuf, LAMEBUF_SIZE);
        debug_print("closing file %s flushed %d\n", fdata->file_path.c_str(), encoded);

        if (encoded > 0) {
            size_t written = fwrite((void*)output->lamebuf, 1, (size_t)encoded, fdata->f);
            if (written == 0 || written < (size_t)encoded)
                log(LOG_WARNING, "Problem writing %s (%s)\n", fdata->file_path.c_str(), strerror(errno));
        }

        // write the lametag to the beginning of the file
        const int lametag_size = lame_get_lametag_frame(output->lame, output->lamebuf, LAMEBUF_SIZE);
        fseek(fdata->f, 0, SEEK_SET);
        fwrite(output->lamebuf, 1, lametag_size, fdata->f);

        // Reset the encoder bitstream so the frame count is correct for the next file
        lame_init_bitstream(output->lame);
    }

    if (fdata->f) {
        fclose(fdata->f);
        fdata->f = NULL;
        rename_if_exists(fdata->file_path_tmp.c_str(), fdata->file_path.c_str());
    }
    fdata->file_path.clear();
    fdata->file_path_tmp.clear();
}

/*
 * Close the current output file if the hour boundary was crossed (append/continuous
 * mode). Split mode close conditions are handled in file_write.
 */
void close_if_necessary(output_t* output) {
    file_data* fdata = (file_data*)(output->data);

    if (!fdata || !fdata->f) {
        return;
    }

    timeval current_time;
    gettimeofday(&current_time, NULL);

    // Check if the hour boundary was just crossed.  NOTE: Actual hour number doesn't matter but still
    // need to use localtime if enabled (some timezones have partial hour offsets)
    int start_hour;
    int current_hour;
    if (use_localtime) {
        start_hour = localtime(&(fdata->open_time.tv_sec))->tm_hour;
        current_hour = localtime(&current_time.tv_sec)->tm_hour;
    } else {
        start_hour = gmtime(&(fdata->open_time.tv_sec))->tm_hour;
        current_hour = gmtime(&current_time.tv_sec)->tm_hour;
    }

    if (start_hour != current_hour) {
        debug_print("closing file %s after crossing hour boundary\n", fdata->file_path.c_str());
        close_file(output);
    }
}

/*
 * For a particular channel file output, check if there is a file currently open.
 * If so, that file may need to be flushed and closed.
 *
 * If the existing open file is good for continued use, return true.
 * Otherwise, create a file name based on the current timestamp and
 * open that new file.  If that file open succeeded, return true.
 */
bool output_file_ready(channel_t* channel, output_t* output) {
    file_data* fdata = (file_data*)(output->data);
    if (!fdata) {
        return false;
    }

    close_if_necessary(output);

    if (fdata->f) {  // still open
        return true;
    }

    timeval current_time;
    gettimeofday(&current_time, NULL);
    // split files are named after the activity start, which precedes file creation by
    // split_min_file_time (the buffered interval)
    const timeval& name_time = (fdata->split_on_transmission) ? fdata->activity_start : current_time;
    struct tm* time;
    if (use_localtime) {
        time = localtime(&name_time.tv_sec);
    } else {
        time = gmtime(&name_time.tv_sec);
    }

    char timestamp[32];
    if (strftime(timestamp, sizeof(timestamp), fdata->split_on_transmission ? "_%Y%m%d_%H%M%S" : "_%Y%m%d_%H", time) == 0) {
        log(LOG_NOTICE, "strftime returned 0\n");
        return false;
    }

    std::string output_dir;
    if (fdata->dated_subdirectories) {
        output_dir = make_dated_subdirs(fdata->basedir, time);
        if (output_dir.empty()) {
            log(LOG_ERR, "Failed to create dated subdirectory\n");
            return false;
        }
    } else {
        output_dir = fdata->basedir;
        make_dir(output_dir);
    }

    // use a string stream to build the output filepath
    std::stringstream ss;
    ss << output_dir << '/' << fdata->basename << timestamp;
    if (fdata->include_freq) {
        ss << '_' << channel->freqlist[channel->freq_idx].frequency;
    }
    ss << fdata->suffix;
    fdata->file_path = ss.str();

    fdata->file_path_tmp = fdata->file_path + ".tmp";

    fdata->open_time = fdata->last_write_time = current_time;

    const int is_audio = output->type == O_RAWFILE ? 0 : 1;
    if (open_file(fdata, channel->mode, is_audio) < 0) {
        log(LOG_WARNING, "Cannot open output file %s (%s)\n", fdata->file_path_tmp.c_str(), strerror(errno));
        return false;
    }

    return true;
}

/*
 * Encode (mp3) and write one batch of samples to the open output file.
 * `samples` is the batch: waveout for O_FILE (left channel; `right` is waveout_r for
 * stereo, NULL otherwise) or iq_out for O_RAWFILE.
 * Returns 0 on success, -1 if the output was disabled due to an error.
 */
static int write_batch(output_t* output, const float* samples, const float* right) {
    file_data* fdata = (file_data*)(output->data);

    // encode mp3 bytes if O_FILE
    int mp3_bytes = 0;
    if (output->type == O_FILE) {
        mp3_bytes = lame_encode_buffer_ieee_float(output->lame, samples, right, WAVE_BATCH, output->lamebuf, LAMEBUF_SIZE);
        if (mp3_bytes < 0) {
            log(LOG_WARNING, "lame_encode_buffer_ieee_float: %d\n", mp3_bytes);
        }

        if (mp3_bytes <= 0) {
            return 0;
        }
    }

    size_t buflen = 0, written = 0;
    if (output->type == O_FILE) {
        buflen = (size_t)mp3_bytes;
        written = fwrite(output->lamebuf, 1, buflen, fdata->f);
    } else {
        buflen = 2 * sizeof(float) * WAVE_BATCH;
        written = fwrite(samples, 1, buflen, fdata->f);
    }
    if (written < buflen) {
        if (ferror(fdata->f))
            log(LOG_WARNING, "Cannot write to %s (%s), output disabled\n", fdata->file_path.c_str(), strerror(errno));
        else
            log(LOG_WARNING, "Short write on %s, output disabled\n", fdata->file_path.c_str());
        close_file(output);
        output->enabled = false;
        return -1;
    }
    return 0;
}

/*
 * Create the split output file (named after the activity start) and write the
 * buffered audio into it.
 * Returns 0 on success, -1 if the output was disabled due to an error.
 */
static int split_open_and_flush(channel_t* channel, output_t* output) {
    file_data* fdata = (file_data*)(output->data);

    if (!output_file_ready(channel, output)) {
        log(LOG_WARNING, "Output disabled\n");
        output->enabled = false;
        return -1;
    }

    const size_t per_batch = (output->type == O_FILE && channel->mode == MM_MONO) ? WAVE_BATCH : 2 * WAVE_BATCH;
    for (size_t off = 0; off < fdata->audio_buf.size(); off += per_batch) {
        // in the buffer stereo channels are stored back to back
        const float* right = (output->type == O_FILE && channel->mode == MM_STEREO) ? (&fdata->audio_buf[off + WAVE_BATCH]) : NULL;
        int rc = write_batch(output, &fdata->audio_buf[off], right);
        if (rc < 0) {
            return rc;
        }
    }
    fdata->audio_buf.clear();
    return 0;
}

/*
 * Write one batch of samples to a file output (mp3 or raw IQ).
 *
 * In split_on_transmission mode the audio of an activity is buffered until it outlives
 * split_min_file_time, so that short bursts (clicks, interference) do not create files:
 *   - audio batch, no file yet: buffer; create the file once the buffer reaches min
 *   - audio batch, file open: write; rotate the file when it outlives max
 *   - silence beyond split_max_idle_time: end of activity - close the file if it was
 *     created, otherwise discard the buffered short activity
 * Returns 0 on success, -1 if the output was disabled due to an error.
 */
int file_write(channel_t* channel, output_t* output) {
    file_data* fdata = (file_data*)(output->data);
    timeval now;
    gettimeofday(&now, NULL);

    if (channel->freqlist[channel->freq_idx].frequency == 0) {
        // no carrier on this (wideband) slot: nothing to record, drop the activity state
        if (fdata->f) {
            close_file(output);
        }
        fdata->audio_buf.clear();
        fdata->activity_active = false;
        output->active = false;
        return 0;
    }

    if (!fdata->split_on_transmission) {
        if (fdata->continuous == false && channel->axcindicate == NO_SIGNAL && output->active == false) {
            close_if_necessary(output);
            return 0;
        }

        if (!output_file_ready(channel, output)) {
            log(LOG_WARNING, "Output disabled\n");
            output->enabled = false;
            return -1;
        }

        const float* batch = (output->type == O_FILE) ? channel->waveout : channel->iq_out;
        const float* right = (output->type == O_FILE && channel->mode == MM_STEREO) ? channel->waveout_r : NULL;
        if (write_batch(output, batch, right) < 0) {
            return -1;
        }
        output->active = (channel->axcindicate != NO_SIGNAL);
        fdata->last_write_time = now;
        return 0;
    }

    if (channel->axcindicate != NO_SIGNAL) {
        if (!fdata->activity_active) {
            fdata->activity_active = true;
            fdata->activity_start = now;
        }
        fdata->last_write_time = now;
        output->active = true;

        const float* batch = (output->type == O_FILE) ? channel->waveout : channel->iq_out;
        const float* right = (output->type == O_FILE && channel->mode == MM_STEREO) ? channel->waveout_r : NULL;

        if (!fdata->f) {
            // no file yet: buffer the batch until the activity outlives split_min_file_time,
            // then create the file and flush the buffer (which includes this batch)
            const size_t left_len = (output->type == O_RAWFILE) ? 2 * WAVE_BATCH : WAVE_BATCH;
            const size_t per_batch = left_len + (right ? WAVE_BATCH : 0);
            fdata->audio_buf.insert(fdata->audio_buf.end(), batch, batch + left_len);
            if (right) {
                fdata->audio_buf.insert(fdata->audio_buf.end(), right, right + WAVE_BATCH);
            }
            double buf_sec = (double)(fdata->audio_buf.size() / per_batch) * WAVE_BATCH / WAVE_RATE;
            if (buf_sec >= fdata->split_min_file_time) {
                if (split_open_and_flush(channel, output) < 0) {
                    return -1;
                }
            }
            return 0;
        }

        // file is open: write directly, no buffering
        // rotation: a file outliving split_max_file_time is replaced by a new one
        if (delta_sec(&fdata->open_time, &now) > fdata->split_max_file_time) {
            close_file(output);
            fdata->activity_start = now;
            if (!output_file_ready(channel, output)) {
                log(LOG_WARNING, "Output disabled\n");
                output->enabled = false;
                return -1;
            }
        }
        return write_batch(output, batch, right);
    }

    if (fdata->activity_active && delta_sec(&fdata->last_write_time, &now) > fdata->split_max_idle_time) {
        if (fdata->f) {
            debug_print("closing file %s after idle %f sec\n", fdata->file_path.c_str(), delta_sec(&fdata->last_write_time, &now));
            close_file(output);
        }
        fdata->audio_buf.clear();
        fdata->activity_active = false;
    }
    output->active = false;
    return 0;
}

void close_channel_file_outputs(channel_t* channel) {
    for (int k = 0; k < channel->output_count; k++) {
        if (channel->outputs[k].type == O_FILE || channel->outputs[k].type == O_RAWFILE) {
            close_file(&channel->outputs[k]);
            channel->outputs[k].active = false;
            file_data* fdata = (file_data*)(channel->outputs[k].data);
            if (fdata) {
                // slot is being retuned or released: drop the in-progress activity
                fdata->audio_buf.clear();
                fdata->activity_active = false;
            }
        }
    }
}

/*
 * test_output_file.cpp
 *
 * Copyright (C) 2026 rtl-airband contributors
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

#include <sys/stat.h>
#ifdef __APPLE__
#include <sys/time.h>  // struct utimbuf, utime()
#else
#include <utime.h>  // struct utimbuf, utime()
#endif

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

#include "output-file.h"
#include "test_base_class.h"

class FileOutputTest : public TestBaseClass {
   protected:
    file_data make_fdata(const std::string& name) {
        file_data fdata = {};
        fdata.basedir = temp_dir;
        fdata.basename = name;
        fdata.suffix = ".mp3";
        fdata.file_path = temp_dir + "/" + name + ".mp3";
        fdata.file_path_tmp = temp_dir + "/" + name + ".mp3.tmp";
        fdata.continuous = false;
        fdata.append = true;
        fdata.split_on_transmission = false;
        fdata.split_min_file_time = 1.0;
        fdata.split_max_file_time = 3600.0;
        fdata.split_max_idle_time = 0.5;
        fdata.discontinuity_tone = true;
        fdata.type = O_FILE;
        return fdata;
    }

    void rewind_timeval(timeval* tv, double seconds) {
        tv->tv_sec -= (long)seconds;
    }

    void feed_raw_batch(channel_t* channel, int batch_no) {
        for (int i = 0; i < 2 * WAVE_BATCH; i++)
            channel->iq_out[i] = (float)(batch_no * 100000 + i);
    }

    void write_file(const std::string& path, size_t size) {
        FILE* f = fopen(path.c_str(), "wb");
        ASSERT_NE(f, (FILE*)NULL);
        const unsigned char buf[256] = {0};
        size_t written = 0;
        while (written < size) {
            size_t chunk = size - written < sizeof(buf) ? size - written : sizeof(buf);
            ASSERT_EQ(fwrite(buf, 1, chunk, f), chunk);
            written += chunk;
        }
        fclose(f);
    }

    long file_size(const std::string& path) {
        struct stat st;
        if (stat(path.c_str(), &st) != 0) {
            return -1;
        }
        return (long)st.st_size;
    }

    void set_mtime_past(const std::string& path, time_t seconds) {
        struct utimbuf times;
        times.actime = time(NULL);
        times.modtime = time(NULL) - seconds;
        ASSERT_EQ(utime(path.c_str(), &times), 0);
    }

    channel_t make_channel(int freq_hz) {
        channel_t channel = {};
        channel.mode = MM_MONO;
        channel.freq_count = 1;
        channel.freq_idx = 0;
        channel.axcindicate = SIGNAL;
        channel.freqlist = (freq_t*)calloc(1, sizeof(freq_t));
        channel.freqlist[0].frequency = freq_hz;
        return channel;
    }

    output_t make_output(file_data* fdata, output_type type) {
        output_t output = {};
        output.type = type;
        output.enabled = true;
        output.data = fdata;
        return output;
    }
};

TEST_F(FileOutputTest, new_file_created_empty) {
    file_data fdata = make_fdata("scan");
    ASSERT_EQ(open_file(&fdata, MM_MONO, 1), 0);
    // no existing data: nothing to mark, empty file at tmp name
    EXPECT_EQ(file_size(fdata.file_path), -1);
    EXPECT_EQ(file_size(fdata.file_path_tmp), 0);
    ASSERT_NE(fdata.f, (FILE*)NULL);
    fclose(fdata.f);
}

TEST_F(FileOutputTest, existing_file_discontinuity_tone_written) {
    file_data fdata = make_fdata("scan");
    write_file(fdata.file_path, 4000);
    ASSERT_EQ(open_file(&fdata, MM_MONO, 1), 0);
    // file was renamed to tmp name and marker tones appended
    EXPECT_EQ(file_size(fdata.file_path), -1);
    EXPECT_GT(file_size(fdata.file_path_tmp), 4000);
    ASSERT_NE(fdata.f, (FILE*)NULL);
    fclose(fdata.f);
}

TEST_F(FileOutputTest, existing_file_no_tone_when_disabled) {
    file_data fdata = make_fdata("scan");
    fdata.discontinuity_tone = false;
    write_file(fdata.file_path, 4000);
    ASSERT_EQ(open_file(&fdata, MM_MONO, 1), 0);
    // file was renamed to tmp name, no bytes appended
    EXPECT_EQ(file_size(fdata.file_path), -1);
    EXPECT_EQ(file_size(fdata.file_path_tmp), 4000);
    ASSERT_NE(fdata.f, (FILE*)NULL);
    fclose(fdata.f);
}

TEST_F(FileOutputTest, no_append_truncates_existing_file) {
    file_data fdata = make_fdata("scan");
    fdata.append = false;
    write_file(fdata.file_path, 4000);
    ASSERT_EQ(open_file(&fdata, MM_MONO, 1), 0);
    EXPECT_EQ(file_size(fdata.file_path), -1);
    EXPECT_EQ(file_size(fdata.file_path_tmp), 0);
    ASSERT_NE(fdata.f, (FILE*)NULL);
    fclose(fdata.f);
}

TEST_F(FileOutputTest, non_audio_file_no_tone) {
    file_data fdata = make_fdata("scan");
    write_file(fdata.file_path, 4000);
    ASSERT_EQ(open_file(&fdata, MM_MONO, 0), 0);
    // raw IQ file: appended as-is, no marker tones
    EXPECT_EQ(file_size(fdata.file_path), -1);
    EXPECT_EQ(file_size(fdata.file_path_tmp), 4000);
    ASSERT_NE(fdata.f, (FILE*)NULL);
    fclose(fdata.f);
}

TEST_F(FileOutputTest, continuous_mode_tone_and_silence) {
    // with tones: 4000 + 6 tones + silence
    file_data fdata_tone = make_fdata("tone");
    fdata_tone.continuous = true;
    write_file(fdata_tone.file_path, 4000);
    set_mtime_past(fdata_tone.file_path, 10);
    ASSERT_EQ(open_file(&fdata_tone, MM_MONO, 1), 0);
    long tone_size = file_size(fdata_tone.file_path_tmp);
    ASSERT_NE(fdata_tone.f, (FILE*)NULL);
    fclose(fdata_tone.f);

    // without tones: 4000 + silence only
    file_data fdata_notone = make_fdata("notone");
    fdata_notone.continuous = true;
    fdata_notone.discontinuity_tone = false;
    write_file(fdata_notone.file_path, 4000);
    set_mtime_past(fdata_notone.file_path, 10);
    ASSERT_EQ(open_file(&fdata_notone, MM_MONO, 1), 0);
    long notone_size = file_size(fdata_notone.file_path_tmp);
    ASSERT_NE(fdata_notone.f, (FILE*)NULL);
    fclose(fdata_notone.f);

    // silence still fills the gap in both cases; tones add on top
    EXPECT_GT(notone_size, 4000);
    EXPECT_GT(tone_size, notone_size);
}

TEST_F(FileOutputTest, output_file_ready_creates_hourly_file) {
    channel_t channel = make_channel(172800000);
    file_data fdata = make_fdata("scan");
    output_t output = make_output(&fdata, O_FILE);

    ASSERT_TRUE(output_file_ready(&channel, &output));
    ASSERT_NE(fdata.f, (FILE*)NULL);

    // <basename>_<YYYYmmdd>_<HH><suffix> in basedir, writing goes to the .tmp name
    const std::string prefix = temp_dir + "/scan_";
    ASSERT_EQ(fdata.file_path.compare(0, prefix.size(), prefix), 0);
    EXPECT_EQ(fdata.file_path.size(), prefix.size() + 11 + fdata.suffix.size());
    EXPECT_EQ(fdata.file_path_tmp, fdata.file_path + ".tmp");
    EXPECT_EQ(file_size(fdata.file_path), -1);
    EXPECT_EQ(file_size(fdata.file_path_tmp), 0);

    close_file(&output);
}

TEST_F(FileOutputTest, output_file_ready_includes_freq) {
    channel_t channel = make_channel(172800000);
    file_data fdata = make_fdata("scan");
    fdata.include_freq = true;
    output_t output = make_output(&fdata, O_FILE);

    ASSERT_TRUE(output_file_ready(&channel, &output));
    ASSERT_NE(fdata.file_path.find("_172800000.mp3"), std::string::npos);

    close_file(&output);
}

TEST_F(FileOutputTest, output_file_ready_reuses_open_file) {
    channel_t channel = make_channel(172800000);
    file_data fdata = make_fdata("scan");
    output_t output = make_output(&fdata, O_FILE);

    ASSERT_TRUE(output_file_ready(&channel, &output));
    const std::string first_path = fdata.file_path;
    ASSERT_TRUE(output_file_ready(&channel, &output));
    EXPECT_EQ(fdata.file_path, first_path);
    ASSERT_NE(fdata.f, (FILE*)NULL);

    close_file(&output);
}

TEST_F(FileOutputTest, close_if_necessary_closes_after_hour_change) {
    channel_t channel = make_channel(172800000);
    file_data fdata = make_fdata("scan");
    output_t output = make_output(&fdata, O_FILE);

    ASSERT_TRUE(output_file_ready(&channel, &output));
    const std::string final_path = fdata.file_path;
    fdata.open_time.tv_sec -= 3600;  // file was opened during the previous hour
    close_if_necessary(&output);

    EXPECT_EQ(fdata.f, (FILE*)NULL);
    EXPECT_EQ(file_size(final_path), 0);  // tmp renamed to the final name
    EXPECT_EQ(file_size(final_path + ".tmp"), -1);
    EXPECT_EQ(fdata.file_path, "");
}

TEST_F(FileOutputTest, close_if_necessary_keeps_file_within_same_hour) {
    channel_t channel = make_channel(172800000);
    file_data fdata = make_fdata("scan");
    output_t output = make_output(&fdata, O_FILE);

    ASSERT_TRUE(output_file_ready(&channel, &output));
    close_if_necessary(&output);
    ASSERT_NE(fdata.f, (FILE*)NULL);

    close_file(&output);
}

TEST_F(FileOutputTest, file_write_appends_mp3) {
    channel_t channel = make_channel(172800000);
    file_data fdata = make_fdata("scan");
    output_t output = make_output(&fdata, O_FILE);
    output.lame = airlame_init(MM_MONO, 0, 0);
    ASSERT_NE(output.lame, (lame_t)NULL);
    output.lamebuf = (unsigned char*)calloc(1, LAMEBUF_SIZE);
    ASSERT_NE(output.lamebuf, (void*)NULL);

    ASSERT_TRUE(output_file_ready(&channel, &output));
    // one batch is shorter than an mp3 frame and may be absorbed by lame's internal buffer,
    // so write several batches until frames are produced
    for (int i = 0; i < 16; i++) {
        ASSERT_EQ(file_write(&channel, &output), 0);
    }

    fflush(fdata.f);  // stdio may still hold the encoded bytes
    EXPECT_GT(file_size(fdata.file_path_tmp), 0);
    EXPECT_TRUE(output.active);

    close_file(&output);
    lame_close(output.lame);
    free(output.lamebuf);
}

TEST_F(FileOutputTest, file_write_appends_raw_iq) {
    channel_t channel = make_channel(172800000);
    for (int i = 0; i < 2 * WAVE_BATCH; i++) {
        channel.iq_out[i] = (float)i;
    }
    file_data fdata = make_fdata("scan");
    fdata.suffix = ".cf32";
    output_t output = make_output(&fdata, O_RAWFILE);

    ASSERT_TRUE(output_file_ready(&channel, &output));
    ASSERT_EQ(file_write(&channel, &output), 0);

    fflush(fdata.f);  // stdio may still hold the written samples
    EXPECT_EQ(file_size(fdata.file_path_tmp), 2 * sizeof(float) * WAVE_BATCH);

    close_file(&output);
}

TEST_F(FileOutputTest, close_channel_file_outputs_closes_open_file) {
    channel_t channel = make_channel(172800000);
    file_data fdata = make_fdata("scan");
    output_t output = make_output(&fdata, O_FILE);
    channel.outputs = &output;
    channel.output_count = 1;

    ASSERT_TRUE(output_file_ready(&channel, &output));
    close_channel_file_outputs(&channel);

    EXPECT_EQ(fdata.f, (FILE*)NULL);
    EXPECT_FALSE(output.active);
}

TEST_F(FileOutputTest, split_short_activity_does_not_create_file) {
    channel_t channel = make_channel(172800000);
    file_data fdata = make_fdata("scan");
    fdata.suffix = ".cf32";
    fdata.type = O_RAWFILE;
    fdata.split_on_transmission = true;
    output_t output = make_output(&fdata, O_RAWFILE);

    // 4 batches = 0.5s < split_min_file_time (1.0s): buffered only, no file yet
    for (int b = 0; b < 4; b++) {
        feed_raw_batch(&channel, b);
        ASSERT_EQ(file_write(&channel, &output), 0);
    }
    ASSERT_TRUE(fdata.activity_active);
    ASSERT_FALSE(fdata.audio_buf.empty());
    ASSERT_EQ(fdata.f, (FILE*)NULL);

    // silence beyond split_max_idle_time ends the activity: buffer discarded, no file created
    channel.axcindicate = NO_SIGNAL;
    rewind_timeval(&fdata.last_write_time, 2.0);
    ASSERT_EQ(file_write(&channel, &output), 0);

    EXPECT_FALSE(fdata.activity_active);
    EXPECT_TRUE(fdata.audio_buf.empty());

    char ts[32];
    ASSERT_GT(strftime(ts, sizeof(ts), "_%Y%m%d_%H%M%S", gmtime(&fdata.activity_start.tv_sec)), 0);
    EXPECT_EQ(file_size(temp_dir + "/scan" + ts + ".cf32"), -1);
}

TEST_F(FileOutputTest, split_long_activity_file_starts_at_activity) {
    channel_t channel = make_channel(172800000);
    file_data fdata = make_fdata("scan");
    fdata.suffix = ".cf32";
    fdata.type = O_RAWFILE;
    fdata.split_on_transmission = true;
    output_t output = make_output(&fdata, O_RAWFILE);

    const int batches = 20;  // 2.5s > split_min_file_time (1.0s)
    for (int b = 0; b < batches; b++) {
        feed_raw_batch(&channel, b);
        ASSERT_EQ(file_write(&channel, &output), 0);
    }
    ASSERT_NE(fdata.f, (FILE*)NULL);

    // file name carries the activity start time, not the file creation time
    char ts[32];
    ASSERT_GT(strftime(ts, sizeof(ts), "_%Y%m%d_%H%M%S", gmtime(&fdata.activity_start.tv_sec)), 0);
    EXPECT_EQ(fdata.file_path, temp_dir + "/scan" + ts + ".cf32");
    std::string final_path = fdata.file_path;

    // silence beyond split_max_idle_time closes the file (clears file_path)
    channel.axcindicate = NO_SIGNAL;
    rewind_timeval(&fdata.last_write_time, 2.0);
    ASSERT_EQ(file_write(&channel, &output), 0);
    ASSERT_EQ(fdata.f, (FILE*)NULL);
    EXPECT_FALSE(fdata.activity_active);

    // file contains all batches from the first one (buffered prefix included)
    const size_t expected_samples = 2 * (size_t)WAVE_BATCH * batches;
    FILE* rf = fopen(final_path.c_str(), "rb");
    ASSERT_NE(rf, (FILE*)NULL);
    std::vector<float> content(expected_samples);
    ASSERT_EQ(fread(content.data(), sizeof(float), expected_samples, rf), expected_samples);
    fclose(rf);
    for (size_t b = 0; b < (size_t)batches; b++) {
        for (int i = 0; i < 2 * WAVE_BATCH; i++) {
            EXPECT_FLOAT_EQ(content[b * 2 * WAVE_BATCH + i], (float)(b * 100000 + i));
        }
    }
}

TEST_F(FileOutputTest, split_rotation_creates_new_file_at_max) {
    channel_t channel = make_channel(172800000);
    file_data fdata = make_fdata("scan");
    fdata.suffix = ".cf32";
    fdata.type = O_RAWFILE;
    fdata.split_on_transmission = true;
    fdata.split_max_file_time = 1.0;
    output_t output = make_output(&fdata, O_RAWFILE);

    for (int b = 0; b < 10; b++) {  // file is created when the buffer crosses min (8 batches)
        feed_raw_batch(&channel, b);
        ASSERT_EQ(file_write(&channel, &output), 0);
    }
    ASSERT_NE(fdata.f, (FILE*)NULL);
    timeval created = fdata.open_time;

    // force rotation: file now older than split_max_file_time
    rewind_timeval(&fdata.open_time, 2.0);
    feed_raw_batch(&channel, 10);
    ASSERT_EQ(file_write(&channel, &output), 0);

    // rotation: old file closed, new file open with open_time reset
    ASSERT_NE(fdata.f, (FILE*)NULL);
    EXPECT_GE(fdata.open_time.tv_sec, created.tv_sec);
    EXPECT_GT(file_size(fdata.file_path_tmp), 0);
    std::string second_path = fdata.file_path;

    // silence ends the activity, closing the second file
    channel.axcindicate = NO_SIGNAL;
    rewind_timeval(&fdata.last_write_time, 2.0);
    ASSERT_EQ(file_write(&channel, &output), 0);
    EXPECT_EQ(fdata.f, (FILE*)NULL);
    EXPECT_FALSE(fdata.activity_active);
    EXPECT_GT(file_size(second_path), 0);
}

TEST_F(FileOutputTest, split_idle_closes_open_file) {
    channel_t channel = make_channel(172800000);
    file_data fdata = make_fdata("scan");
    fdata.suffix = ".cf32";
    fdata.type = O_RAWFILE;
    fdata.split_on_transmission = true;
    output_t output = make_output(&fdata, O_RAWFILE);

    for (int b = 0; b < 10; b++) {
        feed_raw_batch(&channel, b);
        ASSERT_EQ(file_write(&channel, &output), 0);
    }
    ASSERT_NE(fdata.f, (FILE*)NULL);
    std::string final_path = fdata.file_path;

    channel.axcindicate = NO_SIGNAL;
    rewind_timeval(&fdata.last_write_time, 2.0);
    ASSERT_EQ(file_write(&channel, &output), 0);

    EXPECT_EQ(fdata.f, (FILE*)NULL);
    EXPECT_FALSE(fdata.activity_active);
    EXPECT_GT(file_size(final_path), 0);
}

TEST_F(FileOutputTest, split_mp3_buffer_flushed_to_file) {
    channel_t channel = make_channel(172800000);
    file_data fdata = make_fdata("scan");
    fdata.split_on_transmission = true;
    output_t output = make_output(&fdata, O_FILE);
    output.lame = airlame_init(MM_MONO, 0, 0);
    ASSERT_NE(output.lame, (lame_t)NULL);
    output.lamebuf = (unsigned char*)calloc(1, LAMEBUF_SIZE);
    ASSERT_NE(output.lamebuf, (void*)NULL);

    // 16 batches = 2s > split_min_file_time (1.0s): first 8 batches are buffered,
    // then the file is created and the buffer is flushed into it
    for (int b = 0; b < 16; b++) {
        ASSERT_EQ(file_write(&channel, &output), 0);
    }
    ASSERT_NE(fdata.f, (FILE*)NULL);
    EXPECT_TRUE(fdata.audio_buf.empty());
    std::string final_path = fdata.file_path;

    channel.axcindicate = NO_SIGNAL;
    rewind_timeval(&fdata.last_write_time, 2.0);
    ASSERT_EQ(file_write(&channel, &output), 0);

    EXPECT_EQ(fdata.f, (FILE*)NULL);
    EXPECT_FALSE(fdata.activity_active);
    EXPECT_GT(file_size(final_path), 0);

    lame_close(output.lame);
    free(output.lamebuf);
}

TEST_F(FileOutputTest, split_slot_release_resets_buffer) {
    channel_t channel = make_channel(172800000);
    file_data fdata = make_fdata("scan");
    fdata.suffix = ".cf32";
    fdata.type = O_RAWFILE;
    fdata.split_on_transmission = true;
    output_t output = make_output(&fdata, O_RAWFILE);
    channel.outputs = &output;
    channel.output_count = 1;

    for (int b = 0; b < 4; b++) {  // 0.5s < min: buffered, no file
        feed_raw_batch(&channel, b);
        ASSERT_EQ(file_write(&channel, &output), 0);
    }
    ASSERT_TRUE(fdata.activity_active);
    ASSERT_FALSE(fdata.audio_buf.empty());

    close_channel_file_outputs(&channel);

    EXPECT_FALSE(fdata.activity_active);
    EXPECT_TRUE(fdata.audio_buf.empty());
    EXPECT_EQ(fdata.f, (FILE*)NULL);
}

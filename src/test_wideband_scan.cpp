/*
 * test_wideband_scan.cpp
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

#include "test_base_class.h"

#include "wideband_scan.h"

#include <array>
#include <vector>

class WidebandScanTest : public TestBaseClass {
   protected:
    void SetUp(void) { TestBaseClass::SetUp(); }

    void TearDown(void) { TestBaseClass::TearDown(); }
};

TEST_F(WidebandScanTest, sample_rate) {
    EXPECT_EQ(0, wideband_scan_sample_rate(0, 16000));
    EXPECT_EQ(0, wideband_scan_sample_rate(1000000, 0));
    EXPECT_EQ(32000, wideband_scan_sample_rate(1000, 16000));
    EXPECT_EQ(1120000, wideband_scan_sample_rate(1000000, 16000));
    EXPECT_EQ(2400000, wideband_scan_sample_rate(2160000, 16000));
    EXPECT_EQ(0, wideband_scan_sample_rate(2170000, 16000));
}

TEST_F(WidebandScanTest, frequency_to_bin) {
    const int sample_rate = 2400000;
    const int centerfreq = 100000000;
    const size_t fft_size = 512;

    // Same convention as parse_channels(): DC (centerfreq) is bin fft_size - 1,
    // positive offsets start at bin 0, negative offsets wrap from fft_size - 1.
    EXPECT_EQ(fft_size - 1, wideband_frequency_to_bin(centerfreq, centerfreq, sample_rate, fft_size));
    EXPECT_EQ(0, wideband_frequency_to_bin(centerfreq + 1, centerfreq, sample_rate, fft_size));
    EXPECT_EQ(fft_size - 1, wideband_frequency_to_bin(centerfreq - 1, centerfreq, sample_rate, fft_size));
    EXPECT_EQ(1, wideband_frequency_to_bin(centerfreq + 4688, centerfreq, sample_rate, fft_size));
    EXPECT_EQ(fft_size - 2, wideband_frequency_to_bin(centerfreq - 4688, centerfreq, sample_rate, fft_size));
}

TEST_F(WidebandScanTest, new_state_validation) {
    EXPECT_EQ(nullptr, wideband_scan_new(0, 1000000, 12.5, 1, 2400000, 500000, 512, 0.0f));
    EXPECT_EQ(nullptr, wideband_scan_new(1000000, 1000000, 12.5, 1, 2400000, 500000, 512, 0.0f));
    EXPECT_EQ(nullptr, wideband_scan_new(1000000, 2000000, 0.0, 1, 2400000, 1500000, 512, 0.0f));
    EXPECT_EQ(nullptr, wideband_scan_new(1000000, 2000000, 12.5, 0, 2400000, 1500000, 512, 0.0f));
    EXPECT_EQ(nullptr, wideband_scan_new(1000000, 2000000, 12.5, 1, 0, 1500000, 512, 0.0f));
    EXPECT_EQ(nullptr, wideband_scan_new(1000000, 2000000, 12.5, 1, 2400000, 0, 512, 0.0f));
    EXPECT_EQ(nullptr, wideband_scan_new(1000000, 2000000, 12.5, 1, 2400000, 1500000, 0, 0.0f));
    EXPECT_EQ(nullptr, wideband_scan_new(1000000, 2000000, 12.5, 1, 2400000, 1500000, 512, -1.0f));
}

TEST_F(WidebandScanTest, grid_construction) {
    wideband_scan_state* state = wideband_scan_new(100000000, 100037500, 12.5, 2, 2400000, 100018750, 512, 0.0f);
    ASSERT_NE(nullptr, state);
    ASSERT_EQ(4, wideband_scan_grid_count(state));
    ASSERT_EQ(2, wideband_scan_slot_count(state));

    const wideband_grid_point* grid = wideband_scan_grid(state);
    EXPECT_EQ(100000000, grid[0].freq);
    EXPECT_EQ(100012500, grid[1].freq);
    EXPECT_EQ(100025000, grid[2].freq);
    EXPECT_EQ(100037500, grid[3].freq);

    wideband_scan_free(state);
}

TEST_F(WidebandScanTest, carrier_detection_and_slot_assignment) {
    wideband_scan_state* state = wideband_scan_new(100000000, 100037500, 12.5, 2, 2400000, 100018750, 512, 10.0f);
    ASSERT_NE(nullptr, state);
    ASSERT_EQ(4, wideband_scan_grid_count(state));

    std::vector<float> powers(4, 1.0f);
    powers[0] = 10.0f;
    powers[2] = 100.0f;

    // not assigned before the min_above debounce is met
    wideband_scan_update(state, powers.data());
    EXPECT_EQ(0, wideband_scan_slots(state)[0]);
    EXPECT_EQ(0, wideband_scan_slots(state)[1]);

    for (int update = 0; update < 2; update++) {
        wideband_scan_update(state, powers.data());
    }

    const wideband_grid_point* grid = wideband_scan_grid(state);
    const int* slots = wideband_scan_slots(state);
    EXPECT_EQ(grid[2].freq, slots[0]);
    EXPECT_EQ(grid[0].freq, slots[1]);

    wideband_scan_free(state);
}

TEST_F(WidebandScanTest, carrier_holds_for_limited_misses) {
    wideband_scan_state* state = wideband_scan_new(100000000, 100037500, 12.5, 2, 2400000, 100018750, 512, 10.0f);
    ASSERT_NE(nullptr, state);
    ASSERT_EQ(4, wideband_scan_grid_count(state));

    std::vector<float> signal(4, 1.0f);
    signal[0] = 10.0f;
    signal[2] = 100.0f;

    for (int update = 0; update < 3; update++) {
        wideband_scan_update(state, signal.data());
    }

    const wideband_grid_point* grid = wideband_scan_grid(state);
    EXPECT_EQ(grid[2].freq, wideband_scan_slots(state)[0]);
    EXPECT_EQ(grid[0].freq, wideband_scan_slots(state)[1]);

    std::vector<float> noise(4, 1.0f);
    for (int update = 0; update < 10; update++) {
        wideband_scan_update(state, noise.data());
        EXPECT_EQ(grid[2].freq, wideband_scan_slots(state)[0]) << "carrier 2 should be held during update " << update;
        EXPECT_EQ(grid[0].freq, wideband_scan_slots(state)[1]) << "carrier 0 should be held during update " << update;
    }

    wideband_scan_update(state, noise.data());
    EXPECT_EQ(0, wideband_scan_slots(state)[0]);
    EXPECT_EQ(0, wideband_scan_slots(state)[1]);

    wideband_scan_free(state);
}

TEST_F(WidebandScanTest, update_from_fft) {
    wideband_scan_state* state = wideband_scan_new(100000000, 100012499, 12.5, 1, 2400000, 100006249, 512, 0.0f);
    ASSERT_NE(nullptr, state);
    ASSERT_EQ(2, wideband_scan_grid_count(state));

    std::vector<std::array<float, 2>> fftout(512, std::array<float, 2>{0.0f, 0.0f});
    const wideband_grid_point* grid = wideband_scan_grid(state);
    fftout[grid[0].bin][0] = 2.0f;
    fftout[grid[0].bin][1] = 3.0f;
    fftout[grid[1].bin][0] = 1.0f;  // baseline power so the noise floor is non-zero

    for (int update = 0; update < 3; update++) {
        wideband_scan_update_from_fft(state, reinterpret_cast<const float(*)[2]>(fftout.data()));
    }

    EXPECT_EQ(grid[0].freq, wideband_scan_slots(state)[0]);

    wideband_scan_free(state);
}

TEST_F(WidebandScanTest, noise_power) {
    EXPECT_FLOAT_EQ(0.0f, wideband_scan_noise_power(nullptr));

    wideband_scan_state* state = wideband_scan_new(100000000, 100037500, 12.5, 2, 2400000, 100018750, 512, 10.0f);
    ASSERT_NE(nullptr, state);
    ASSERT_EQ(4, wideband_scan_grid_count(state));

    EXPECT_FLOAT_EQ(0.0f, wideband_scan_noise_power(state));

    std::vector<float> powers(4, 1.0f);
    powers[2] = 100.0f;

    wideband_scan_update(state, powers.data());
    // 25th percentile of [1, 1, 1, 100] is 1.0, unaffected by the carrier
    EXPECT_FLOAT_EQ(1.0f, wideband_scan_noise_power(state));

    wideband_scan_free(state);
}

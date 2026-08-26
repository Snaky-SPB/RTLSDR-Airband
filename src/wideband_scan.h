/*
 * wideband_scan.h
 * Wideband carrier-detection scanning support
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

#ifndef _WIDEBAND_SCAN_H
#define _WIDEBAND_SCAN_H

#include <cstddef>  // size_t

struct wideband_grid_point {
    int freq;           // frequency in Hz
    size_t bin;         // FFT bin for this frequency
    float power;        // last observed bin power
    int above_count;    // consecutive updates above the detection threshold
    int missing_count;  // consecutive updates where the carrier was below threshold or not assigned
    bool above;         // above the detection threshold on the last update
    bool excluded;      // blacklisted: never detected, never assigned to a slot
};

struct wideband_scan_state {
    int freq_from;
    int freq_to;
    double step_hz;
    int grid_count;
    int slot_count;
    int max_active;
    float snr_threshold_db;
    int min_above;
    int max_missing;
    int sample_rate;
    int centerfreq;
    size_t fft_size;
    float noise_power;  // 25th percentile of the grid bin powers on the last update
    wideband_grid_point* grid;
    int* slot_freqs;
    int* new_slot_freqs;
    float* powers;
    float* sorted;
    int* selected;
    int* candidates;
};

wideband_scan_state* wideband_scan_new(int freq_from_hz, int freq_to_hz, double step_khz, int max_active, int sample_rate, int centerfreq, size_t fft_size, float snr_threshold_db);
void wideband_scan_free(wideband_scan_state* state);
int wideband_scan_exclude(wideband_scan_state* state, int freq_hz);

int wideband_scan_sample_rate(int range_hz, int wave_rate);
size_t wideband_frequency_to_bin(int freq_hz, int centerfreq_hz, int sample_rate, size_t fft_size);

void wideband_scan_update(wideband_scan_state* state, const float* powers);
void wideband_scan_update_from_fft(wideband_scan_state* state, const float (*fftout)[2]);
const int* wideband_scan_slots(const wideband_scan_state* state);
int wideband_scan_slot_count(const wideband_scan_state* state);
int wideband_scan_grid_count(const wideband_scan_state* state);
const wideband_grid_point* wideband_scan_grid(const wideband_scan_state* state);
float wideband_scan_noise_power(const wideband_scan_state* state);

#endif /* _WIDEBAND_SCAN_H */

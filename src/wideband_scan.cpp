/*
 * wideband_scan.cpp
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

#include "wideband_scan.h"

#include <algorithm>  // nth_element, sort
#include <climits>    // INT_MAX
#include <cmath>      // ceil, pow, round
#include <cstdlib>    // calloc, free
#include <vector>

static int find_grid_index(const wideband_scan_state* state, int freq) {
    for (int i = 0; i < state->grid_count; i++) {
        if (state->grid[i].freq == freq)
            return i;
    }
    return -1;
}

int wideband_scan_sample_rate(int range_hz, int wave_rate) {
    static const int MAX_SAMPLE_RATE = 2400000;
    if (range_hz <= 0 || wave_rate <= 0)
        return 0;

    // Leave 10% of the Nyquist band as guard so the scan range is well inside the usable bandwidth.
    double required = (double)range_hz / 0.9;
    int sample_rate = (int)std::ceil(required / (double)wave_rate) * wave_rate;
    if (sample_rate < 2 * wave_rate)
        sample_rate = 2 * wave_rate;
    if (sample_rate > MAX_SAMPLE_RATE)
        return 0;
    return sample_rate;
}

size_t wideband_frequency_to_bin(int freq_hz, int centerfreq_hz, int sample_rate, size_t fft_size) {
    if (sample_rate <= 0 || fft_size == 0)
        return 0;

    // Keep the same FFT bin convention as the fixed-channel path:
    // DC is bin (fft_size - 1), positive offsets wrap to bin 0.
    double bin_hz = (double)sample_rate / (double)fft_size;
    long long bin = (long long)std::ceil(((double)freq_hz + (double)sample_rate - (double)centerfreq_hz) / bin_hz - 1.0);
    bin %= (long long)fft_size;
    if (bin < 0)
        bin += fft_size;
    return (size_t)bin;
}

wideband_scan_state* wideband_scan_new(int freq_from_hz, int freq_to_hz, double step_khz, int max_active, int sample_rate, int centerfreq, size_t fft_size, float snr_threshold_db) {
    if (freq_from_hz <= 0 || freq_to_hz <= freq_from_hz || step_khz <= 0.0 || max_active < 1 || sample_rate <= 0 || centerfreq <= 0 || fft_size == 0 || snr_threshold_db < 0.0f)
        return NULL;

    double step_hz = step_khz * 1000.0;
    int grid_count = (int)std::round(((double)freq_to_hz - (double)freq_from_hz) / step_hz) + 1;
    if (grid_count < 1)
        return NULL;

    wideband_scan_state* state = (wideband_scan_state*)calloc(1, sizeof(wideband_scan_state));
    if (!state)
        return NULL;

    state->freq_from = freq_from_hz;
    state->freq_to = freq_to_hz;
    state->step_hz = step_hz;
    state->grid_count = grid_count;
    state->slot_count = max_active;
    state->max_active = max_active;
    state->snr_threshold_db = snr_threshold_db;
    state->min_above = 3;
    state->max_missing = 10;
    state->sample_rate = sample_rate;
    state->centerfreq = centerfreq;
    state->fft_size = fft_size;

    state->grid = (wideband_grid_point*)calloc((size_t)grid_count, sizeof(wideband_grid_point));
    state->slot_freqs = (int*)calloc((size_t)max_active, sizeof(int));
    state->new_slot_freqs = (int*)calloc((size_t)max_active, sizeof(int));
    state->powers = (float*)calloc((size_t)grid_count, sizeof(float));
    state->sorted = (float*)calloc((size_t)grid_count, sizeof(float));
    state->selected = (int*)calloc((size_t)grid_count, sizeof(int));
    state->candidates = (int*)calloc((size_t)grid_count, sizeof(int));
    if (!state->grid || !state->slot_freqs || !state->new_slot_freqs || !state->powers || !state->sorted || !state->selected || !state->candidates) {
        wideband_scan_free(state);
        return NULL;
    }

    for (int i = 0; i < grid_count; i++) {
        double freq = (double)freq_from_hz + ((double)i * step_hz);
        if (freq > (double)freq_to_hz + 0.5)
            freq = (double)freq_to_hz;
        state->grid[i].freq = (int)std::round(freq);
        state->grid[i].bin = wideband_frequency_to_bin(state->grid[i].freq, centerfreq, sample_rate, fft_size);
    }

    return state;
}

void wideband_scan_free(wideband_scan_state* state) {
    if (!state)
        return;
    free(state->grid);
    free(state->slot_freqs);
    free(state->new_slot_freqs);
    free(state->powers);
    free(state->sorted);
    free(state->selected);
    free(state->candidates);
    free(state);
}

void wideband_scan_update(wideband_scan_state* state, const float* powers) {
    if (!state || !powers || state->grid_count < 1 || state->slot_count < 1)
        return;

    std::copy(powers, powers + state->grid_count, state->sorted);
    size_t noise_idx = (size_t)state->grid_count / 4;
    std::nth_element(state->sorted, state->sorted + noise_idx, state->sorted + state->grid_count);
    float noise = state->sorted[noise_idx];
    float threshold = noise * (float)std::pow(10.0, state->snr_threshold_db / 10.0);

    for (int i = 0; i < state->grid_count; i++) {
        state->grid[i].power = powers[i];
        bool above = (threshold > 0.0f && powers[i] >= threshold);
        state->grid[i].above = above;
        state->grid[i].above_count = above ? state->grid[i].above_count + 1 : 0;
    }

    std::fill(state->new_slot_freqs, state->new_slot_freqs + state->slot_count, 0);
    std::fill(state->selected, state->selected + state->grid_count, 0);

    // Preserve frequencies that are already assigned to a slot.  This avoids churn when a carrier
    // briefly dips below the detection threshold.
    for (int s = 0; s < state->slot_count; s++) {
        int freq = state->slot_freqs[s];
        if (freq == 0)
            continue;
        int gi = find_grid_index(state, freq);
        if (gi >= 0 && !state->selected[gi] && (state->grid[gi].above || state->grid[gi].missing_count < state->max_missing)) {
            state->selected[gi] = 1;
            state->new_slot_freqs[s] = freq;
        }
    }

    // Fill remaining slots with the strongest new carriers that have been above threshold long enough.
    int candidate_count = 0;
    for (int gi = 0; gi < state->grid_count; gi++) {
        if (!state->selected[gi] && state->grid[gi].above && state->grid[gi].above_count >= state->min_above)
            state->candidates[candidate_count++] = gi;
    }
    std::sort(state->candidates, state->candidates + candidate_count, [powers](int a, int b) { return powers[a] > powers[b]; });

    int candidate_idx = 0;
    for (int s = 0; s < state->slot_count && candidate_idx < candidate_count; s++) {
        if (state->new_slot_freqs[s] != 0)
            continue;
        int gi = state->candidates[candidate_idx++];
        if (state->selected[gi])
            continue;
        state->selected[gi] = 1;
        state->new_slot_freqs[s] = state->grid[gi].freq;
    }

    for (int gi = 0; gi < state->grid_count; gi++) {
        if (state->selected[gi] && state->grid[gi].above)
            state->grid[gi].missing_count = 0;
        else if (state->grid[gi].missing_count < INT_MAX)
            state->grid[gi].missing_count++;
    }

    std::swap(state->slot_freqs, state->new_slot_freqs);
}

void wideband_scan_update_from_fft(wideband_scan_state* state, const float (*fftout)[2]) {
    if (!state || !fftout || !state->powers)
        return;

    for (int i = 0; i < state->grid_count; i++) {
        const float* bin = fftout[state->grid[i].bin];
        // power, not amplitude, so the dB SNR threshold (a power ratio) applies correctly
        state->powers[i] = bin[0] * bin[0] + bin[1] * bin[1];
    }
    wideband_scan_update(state, state->powers);
}

const int* wideband_scan_slots(const wideband_scan_state* state) {
    return state ? state->slot_freqs : NULL;
}

int wideband_scan_slot_count(const wideband_scan_state* state) {
    return state ? state->slot_count : 0;
}

int wideband_scan_grid_count(const wideband_scan_state* state) {
    return state ? state->grid_count : 0;
}

const wideband_grid_point* wideband_scan_grid(const wideband_scan_state* state) {
    return state ? state->grid : NULL;
}

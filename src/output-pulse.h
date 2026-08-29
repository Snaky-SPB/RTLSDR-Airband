/*
 * output-pulse.h
 * PulseAudio output (built only with PULSEAUDIO)
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

#ifndef _OUTPUT_PULSE_H
#define _OUTPUT_PULSE_H 1

#include "config.h"
#ifdef WITH_PULSEAUDIO
#include "rtl_airband.h"

void pulse_init();
int pulse_setup(pulse_data* pdata, mix_modes mixmode);
void pulse_start();
void pulse_shutdown(pulse_data* pdata);
void pulse_write_stream(pulse_data* pdata, mix_modes mode, const float* data_left, const float* data_right, size_t len);
void pulse_check(pulse_data* pdata, input_state_t state, mix_modes mode);
#endif /* WITH_PULSEAUDIO */

#endif /* _OUTPUT_PULSE_H */

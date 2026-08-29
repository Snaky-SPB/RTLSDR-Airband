/*
 * output-icecast.h
 * Icecast (libshout) output: connection setup, mp3 encode + send, reconnect
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

#ifndef _OUTPUT_ICECAST_H
#define _OUTPUT_ICECAST_H 1

#include "rtl_airband.h"

#define MAX_SHOUT_QUEUELEN 32768

void shout_setup(icecast_data* icecast, mix_modes mixmode);
void icecast_write(channel_t* channel, output_t* output, int cur_scan_freq);
void icecast_close(icecast_data* icecast);
void icecast_check(icecast_data* icecast, input_state_t state, mix_modes mode, int device_idx);

#endif /* _OUTPUT_ICECAST_H */

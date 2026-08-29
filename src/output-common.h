/*
 * output-common.h
 * Common output routines: per-type dispatch, output threads
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

#ifndef _OUTPUT_COMMON_H
#define _OUTPUT_COMMON_H 1

#include "rtl_airband.h"

// Create all the output for a particular channel.
void process_outputs(channel_t* channel, int cur_scan_freq);
void disable_channel_outputs(channel_t* channel);
void disable_device_outputs(device_t* dev);
void* output_check_thread(void* params);
void* output_thread(void* params);

#endif /* _OUTPUT_COMMON_H */

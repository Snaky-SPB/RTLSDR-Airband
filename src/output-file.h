/*
 * output-file.h
 * File output (mp3/raw) lifecycle: filename, open/append, write, close
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

#ifndef _OUTPUT_FILE_H
#define _OUTPUT_FILE_H 1

#include <stdio.h>

#include "rtl_airband.h"

// low-level helpers
int open_file(file_data* fdata, mix_modes mixmode, int is_audio);
int rename_if_exists(char const* oldpath, char const* newpath);

// file output lifecycle
lame_t airlame_init(mix_modes mixmode, int highpass, int lowpass);
bool output_file_ready(channel_t* channel, output_t* output);
int file_write(channel_t* channel, output_t* output);
void close_file(output_t* output);
void close_if_necessary(output_t* output);
void close_channel_file_outputs(channel_t* channel);

#endif /* _OUTPUT_FILE_H */

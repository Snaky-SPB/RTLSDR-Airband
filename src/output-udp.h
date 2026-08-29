/*
 * output-udp.h
 * UDP stream output: raw 32-bit float audio to a remote host
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

#ifndef _OUTPUT_UDP_H
#define _OUTPUT_UDP_H 1

#include "rtl_airband.h"

bool udp_stream_init(udp_stream_data* sdata, mix_modes mode, size_t len);
void udp_stream_write(udp_stream_data* sdata, const float* data, size_t len);
void udp_stream_write(udp_stream_data* sdata, const float* data_left, const float* data_right, size_t len);
void udp_stream_shutdown(udp_stream_data* sdata);
void udp_stream_check(udp_stream_data* sdata, input_state_t state);

#endif /* _OUTPUT_UDP_H */

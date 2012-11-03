/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef MPV_LIBAV_COMPAT_H
#define MPV_LIBAV_COMPAT_H

#include <libavutil/version.h>
#include <libavutil/cpu.h>

#ifdef AV_CPU_FLAG_MMXEXT
#define AV_CPU_FLAG_MMX2 AV_CPU_FLAG_MMXEXT
#endif

#if LIBAVUTIL_VERSION_MICRO < 100
#define AV_CODEC_ID_SUBRIP CODEC_ID_TEXT
#endif

#endif /* MPV_LIBAV_COMPAT_H */

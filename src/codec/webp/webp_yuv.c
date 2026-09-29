/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Image.
 *
 * Ghoti.io Image is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Image is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * Portions derived from libwebp 1.5.0 (Copyright (c) 2010, Google Inc.),
 * originally under a BSD-style license; adapted for VP8 keyframe decode.
 */

/** @file Fancy 4:2:0 upsample + fixed-point YUV→RGB (libwebp). */
#define HAVE_CONFIG_H
#include "vp8ref/src/webp/config.h"
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wbad-function-cast"
#endif
#include "vp8ref/src/dsp/yuv.inc"
#include "vp8ref/src/dsp/upsampling.inc"
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

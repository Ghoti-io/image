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
 */

/**
 * @file
 *
 * Compile-time debug and trace controls for the JPEG codec. Default all to 0;
 * enable for debug builds via -DGIMG_JPEG_DEBUG_LOAD=1 (or equivalent) or by
 * defining a set of them in a debug build.
 *
 * The behavior-altering option recover_stuff_zero remains
 * runtime (getenv) for field debugging; see comments below.
 */

#ifndef GHOTI_IO_GIMG_SRC_CODEC_JPEG_JPEG_DEBUG_INTERNAL_H
#define GHOTI_IO_GIMG_SRC_CODEC_JPEG_JPEG_DEBUG_INTERNAL_H

#include <ghoti.io/image/macros.h>

// Default all to 0 if not already defined (e.g. -DGIMG_JPEG_DEBUG_LOAD=1).
#ifndef GIMG_JPEG_DEBUG_LOAD
#define GIMG_JPEG_DEBUG_LOAD 0
#endif
/** When GIMG_JPEG_DEBUG_PROG_SYNC=1, log encoder/decoder step sync (used by
 * jpeg_entropy.c and jpeg_block.c). */
#define PROG_SYNC_DEBUG() (GIMG_JPEG_DEBUG_PROG_SYNC)
#ifndef GIMG_JPEG_DEBUG_RST_DEC
#define GIMG_JPEG_DEBUG_RST_DEC 0
#endif
#ifndef GIMG_JPEG_DEBUG_SKIP_FF
#define GIMG_JPEG_DEBUG_SKIP_FF 0
#endif
#ifndef GIMG_JPEG_DEBUG_DHT_DC
#define GIMG_JPEG_DEBUG_DHT_DC 0
#endif
#ifndef GIMG_JPEG_DEBUG_BASELINE_FAIL
#define GIMG_JPEG_DEBUG_BASELINE_FAIL 0
#endif
#ifndef GIMG_JPEG_DEBUG_SCAN_LOADED
#define GIMG_JPEG_DEBUG_SCAN_LOADED 0
#endif
#ifndef GIMG_JPEG_DEBUG_BIT_POS
#define GIMG_JPEG_DEBUG_BIT_POS 0
#endif
#ifndef GIMG_JPEG_AC_LONGEST_MATCH
#define GIMG_JPEG_AC_LONGEST_MATCH 1
#endif
#ifndef GIMG_JPEG_TRACE_BASELINE_SYNC
#define GIMG_JPEG_TRACE_BASELINE_SYNC 0
#endif
#ifndef GIMG_JPEG_TRACE_FIRST_CB
#define GIMG_JPEG_TRACE_FIRST_CB 0
#endif
#ifndef GIMG_JPEG_DUMP_JPEG_COMPONENTS
#define GIMG_JPEG_DUMP_JPEG_COMPONENTS 0
#endif
#ifndef GIMG_JPEG_DUMP_FIRST_MCU_COEF
#define GIMG_JPEG_DUMP_FIRST_MCU_COEF 0
#endif
#ifndef GIMG_JPEG_DUMP_FIRST_BLOCK_COEF
#define GIMG_JPEG_DUMP_FIRST_BLOCK_COEF 0
#endif
#ifndef GIMG_JPEG_DUMP_FIRST_N_BLOCKS_COEF
#define GIMG_JPEG_DUMP_FIRST_N_BLOCKS_COEF 0
#endif
#ifndef GIMG_JPEG_DUMP_SCAN_BASELINE
#define GIMG_JPEG_DUMP_SCAN_BASELINE 0
#endif
#ifndef GIMG_JPEG_TRACE_BASELINE_BIT_POS
#define GIMG_JPEG_TRACE_BASELINE_BIT_POS 0
#endif
#ifndef GIMG_JPEG_TRACE_ENTROPY
#define GIMG_JPEG_TRACE_ENTROPY 0
#endif

// recover_stuff_zero: kept as a runtime switch (GIMG_JPEG_RECOVER_STUFF_ZERO)
// for field debugging of truncated streams. Not from T.81; opt-in only. See
// gimg_jpeg_bitstream_t in jpeg_internal.h.

#endif // GHOTI_IO_GIMG_SRC_CODEC_JPEG_JPEG_DEBUG_INTERNAL_H

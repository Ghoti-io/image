/**
 * @file
 *
 * Compile-time debug and trace controls for the JPEG codec. Default all to 0;
 * enable for debug builds via -DGIMG_JPEG_DEBUG_LOAD=1 (or equivalent) or by
 * defining a set of them in a debug build.
 *
 * Behaviour-altering options recover_stuff_zero and pad_at_eob remain
 * runtime (getenv) for field debugging; see comments below.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GIMG_JPEG_DEBUG_INTERNAL_H
#define GHOTI_IO_GIMG_JPEG_DEBUG_INTERNAL_H

// Default all to 0 if not already defined (e.g. -DGIMG_JPEG_DEBUG_LOAD=1).
#ifndef GIMG_JPEG_DEBUG_LOAD
#define GIMG_JPEG_DEBUG_LOAD 0
#endif
#ifndef GIMG_JPEG_DEBUG_PROG_SYNC
#define GIMG_JPEG_DEBUG_PROG_SYNC 0
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
#ifndef GIMG_JPEG_DEBUG_DHT_AC
#define GIMG_JPEG_DEBUG_DHT_AC 0
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
#ifndef GIMG_JPEG_PROGRESSIVE_DEBUG
#define GIMG_JPEG_PROGRESSIVE_DEBUG 0
#endif
#ifndef GIMG_JPEG_TRACE_DC
#define GIMG_JPEG_TRACE_DC 0
#endif
#ifndef GIMG_JPEG_TRACE_HUFF_BITS
#define GIMG_JPEG_TRACE_HUFF_BITS 0
#endif
#ifndef GIMG_JPEG_TRACE_ALL
#define GIMG_JPEG_TRACE_ALL 0
#endif
#ifndef GIMG_JPEG_TRACE_AC_FAIL
#define GIMG_JPEG_TRACE_AC_FAIL 0
#endif
#ifndef GIMG_JPEG_AC_INITIAL_SYM_LOG
#define GIMG_JPEG_AC_INITIAL_SYM_LOG 0
#endif
#ifndef GIMG_JPEG_TRACE_DC_MATCH
#define GIMG_JPEG_TRACE_DC_MATCH 0
#endif
#ifndef GIMG_JPEG_AC_LONGEST_MATCH
#define GIMG_JPEG_AC_LONGEST_MATCH 1
#endif
#ifndef GIMG_JPEG_TRACE_DC_BLOCK
#define GIMG_JPEG_TRACE_DC_BLOCK 0
#endif
#ifndef GIMG_JPEG_DUMP_AC_INITIAL_FULL
#define GIMG_JPEG_DUMP_AC_INITIAL_FULL 0
#endif
#ifndef GIMG_JPEG_TRACE_PROG_FIRST_AC
#define GIMG_JPEG_TRACE_PROG_FIRST_AC 0
#endif
#ifndef GIMG_JPEG_TRACE_AC_COMPARE
#define GIMG_JPEG_TRACE_AC_COMPARE 0
#endif
#ifndef GIMG_JPEG_DUMP_AC_REFINE_FULL
#define GIMG_JPEG_DUMP_AC_REFINE_FULL 0
#endif
#ifndef GIMG_JPEG_TRACE_BASELINE_SYNC
#define GIMG_JPEG_TRACE_BASELINE_SYNC 0
#endif
#ifndef GIMG_JPEG_TRACE_DEC_BLOCK_POS
#define GIMG_JPEG_TRACE_DEC_BLOCK_POS 0
#endif
#ifndef GIMG_JPEG_TRACE_FIRST_BLOCK
#define GIMG_JPEG_TRACE_FIRST_BLOCK 0
#endif
#ifndef GIMG_JPEG_TRACE_FIRST_CB
#define GIMG_JPEG_TRACE_FIRST_CB 0
#endif
#ifndef GIMG_JPEG_TRACE_BASELINE_FIRST_MCU
#define GIMG_JPEG_TRACE_BASELINE_FIRST_MCU 0
#endif
#ifndef GIMG_JPEG_DUMP_JPEG_COMPONENTS
#define GIMG_JPEG_DUMP_JPEG_COMPONENTS 0
#endif
#ifndef GIMG_JPEG_TRACE_PROG_FIRST_DC
#define GIMG_JPEG_TRACE_PROG_FIRST_DC 0
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
#ifndef GIMG_JPEG_AC_REFINE_CORRECTION_K
#define GIMG_JPEG_AC_REFINE_CORRECTION_K 0
#endif

// recover_stuff_zero and pad_at_eob: kept as runtime (getenv) for field
// debugging of truncated or non-byte-aligned streams. Not from T.81; opt-in
// only. See gimg_jpeg_bitstream_t in jpeg_entropy.c.

#endif // GHOTI_IO_GIMG_JPEG_DEBUG_INTERNAL_H

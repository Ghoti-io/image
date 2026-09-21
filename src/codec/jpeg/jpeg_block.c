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
 * Decode one 8×8 block: baseline DC/AC, progressive DC initial/refinement,
 * progressive AC initial/refinement. Calls bitstream for symbols; outputs
 * coefficient block. No dequant or IDCT. Used by jpeg_entropy.c.
 */

#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/codec.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "jpeg_debug_internal.h"
#include "jpeg_internal.h"

GIMG_Result jpeg_decode_block(gimg_jpeg_bitstream_t * bs,
    const gimg_jpeg_huff_table_t * dc_tbl,
    const gimg_jpeg_huff_table_t * ac_tbl, int16_t * block,
    int16_t * dc_predictor, int is_last_block) {
  jpeg_bitstream_align_skip_rst(bs);
  memset(block, 0, 64 * sizeof(int16_t));
  int sym = jpeg_huff_decode(bs, dc_tbl, 0, 0, 0);
  if (sym < 0) {
    if (GIMG_JPEG_DEBUG_BASELINE_FAIL) {
      (void)fprintf(stderr,
          "BASELINE_FAIL_STEP dc_sym byte_off=%zu bit_off=%u\n",
          (size_t)bs->byte_off, (unsigned)bs->bit_off);
      (void)fflush(stderr);
    }
    return GIMG_ERR_CORRUPT;
  }
  // T.81 F.2.1.3.1: the DC predictions of *every* component are reset at a
  // restart, not just the one whose block happens to follow the marker.  This
  // used to zero only *dc_predictor and then clear the flag, so in a 4:2:0
  // image the two chroma predictors carried across the restart and every
  // chroma block after the first interval was decoded against a stale
  // prediction.  The caller now performs the reset, for all components at once,
  // where it can see them all.
  // T.81 F.1.2.1 Table F.1: a DC difference category is 0..11 at 8-bit and
  // 0..15 at 12-bit precision.  The category comes straight out of the Huffman
  // table, so a crafted DHT can put any byte here; reject it rather than use it
  // as a bit count.
  int nbits = sym;
  if (nbits > 15) {
    return GIMG_ERR_CORRUPT;
  }
  int diff = 0;
  if (nbits > 0) {
    diff = jpeg_bitstream_read_bits(bs, nbits);
    if (diff < 0) {
      if (GIMG_JPEG_DEBUG_BASELINE_FAIL) {
        (void)fprintf(stderr,
            "BASELINE_FAIL_STEP dc_extra nbits=%d byte_off=%zu\n", nbits,
            (size_t)bs->byte_off);
        (void)fflush(stderr);
      }
      return GIMG_ERR_CORRUPT;
    }
    diff = jpeg_extend(diff, nbits);
  }
  *dc_predictor += diff;
  block[0] = *dc_predictor;

  {
    size_t pos_after_dc = (size_t)bs->byte_off * 8u + (unsigned)bs->bit_off;
    if (GIMG_JPEG_TRACE_DC_BLOCK && pos_after_dc >= 960u &&
        pos_after_dc <= 995u) {
      (void)fprintf(stderr,
          "DEC after_dc pos_bits=%zu (byte=%zu bit=%u) dc_cat=%d\n",
          pos_after_dc, (size_t)bs->byte_off, (unsigned)bs->bit_off, sym);
      (void)fflush(stderr);
    }
  }

  for (int k = 1; k < 64; k++) {
    // T.81 Annex F Figure F.16: baseline AC uses first-match only (no
    // longest-match).
    sym = jpeg_huff_decode(bs, ac_tbl, 0, 1, 1);
    if (sym < 0) {
      if (is_last_block) {
        // Segment ended before byte boundary (e.g. 0-bit padding); treat as
        // EOB.
        sym = 0;
      }
      else {
        if (GIMG_JPEG_DEBUG_BASELINE_FAIL) {
          (void)fprintf(stderr,
              "BASELINE_FAIL_STEP ac_sym k=%d byte_off=%zu bit_off=%u\n", k,
              (size_t)bs->byte_off, (unsigned)bs->bit_off);
          (void)fflush(stderr);
        }
        return GIMG_ERR_CORRUPT;
      }
    }
    if (sym == 0) {
      // T.81 Annex F: EOB (0,0); remaining coefficients in zigzag order are
      // zero.
      for (; k < 64; k++) {
        block[k] = 0;
      }
      break;
    }
    int run = sym >> 4;
    int size = sym & 0x0F;
    k += run;
    // T.81 Annex F: (run, size) with k+run >= 64 is invalid.
    if (k >= 64) {
      if (GIMG_JPEG_DEBUG_BASELINE_FAIL) {
        (void)fprintf(stderr,
            "BASELINE_FAIL_STEP ac_run_overflow k=%d run=%d byte_off=%zu "
            "bit_off=%u\n",
            k, run, (size_t)bs->byte_off, (unsigned)bs->bit_off);
        (void)fflush(stderr);
      }
      return GIMG_ERR_CORRUPT;
    }
    int ac = 0;
    if (size > 0) {
      ac = jpeg_bitstream_read_bits(bs, size);
      if (ac < 0) {
        if (is_last_block) {
          // T.81 B.2.2: decoders must not interpret padding as data; treat
          // underflow as EOB.
          for (; k < 64; k++) {
            block[k] = 0;
          }
          return GIMG_OK;
        }
        if (GIMG_JPEG_DEBUG_BASELINE_FAIL) {
          (void)fprintf(stderr,
              "BASELINE_FAIL_STEP ac_extra size=%d byte_off=%zu\n", size,
              (size_t)bs->byte_off);
          (void)fflush(stderr);
        }
        return GIMG_ERR_CORRUPT;
      }
      ac = jpeg_extend(ac, size);
    }
    block[k] = (int16_t)ac;
  }
  return GIMG_OK;
}

/** Progressive: decode DC only (Ss=0, Se=0). Ah=0: initial (size + bits).
 *  T.81 Annex G.1.1.1: point transform Pt = Al; decoded value is DIFF with
 *  low Al bits zero; we store (diff << al) so refinement scans can add low
 * bits. If out_sym and out_diff are non-NULL, they receive the decoded symbol
 * and diff (for TRACE_JPEG_DC_SYMBOLS). When trace_all, log every step
 * (position, sym, bits, computation). is_last_block: when 1, underflow is
 * treated as DC size 0 (T.81 B.2.2 does not specify padding value; supports 0-
 * or 1-bit padding from third-party encoders). */
GIMG_Result jpeg_decode_block_progressive_dc(gimg_jpeg_bitstream_t * bs,
    const gimg_jpeg_huff_table_t * dc_tbl, int16_t * block,
    int16_t * dc_predictor, int al, int * out_sym, int * out_diff,
    int trace_all, int is_last_block) {
  if (trace_all) {
    (void)fprintf(stderr, "DC_HUFF_ENTER byte_off=%zu bit_off=%u\n",
        (size_t)bs->byte_off, (unsigned)bs->bit_off);
    (void)fflush(stderr);
  }
  int sym = jpeg_huff_decode(bs, dc_tbl, 0, 0, 0);
  if (sym < 0) {
    if (is_last_block) {
      sym = 0; // Segment ended before byte boundary; treat as DC size 0 (no
               // extra bits).
    }
    else {
      return GIMG_ERR_CORRUPT;
    }
  }
  // Same bound as the sequential decoder: the DC category comes from the
  // Huffman table and must be a category, not an arbitrary byte
  // (T.81 F.1.2.1 Table F.1).
  int nbits = sym;
  if (nbits > 15) {
    return GIMG_ERR_CORRUPT;
  }
  int diff = 0;
  if (trace_all) {
    (void)fprintf(stderr,
        "DC_HUFF_EXIT sym=%d nbits=%d byte_off=%zu bit_off=%u\n", sym, nbits,
        (size_t)bs->byte_off, (unsigned)bs->bit_off);
    (void)fflush(stderr);
  }
  if (nbits > 0) {
    diff = jpeg_bitstream_read_bits(bs, nbits);
    if (diff < 0) {
      if (is_last_block) {
        diff = 0; // Padding / end of segment; treat as 0.
      }
      else {
        return GIMG_ERR_CORRUPT;
      }
    }
    if (trace_all) {
      (void)fprintf(stderr,
          "DC_EXTRA_BITS nbits=%d raw=%d byte_off=%zu bit_off=%u\n", nbits,
          diff, (size_t)bs->byte_off, (unsigned)bs->bit_off);
      (void)fflush(stderr);
    }
    diff = jpeg_extend(diff, nbits);
    if (trace_all) {
      (void)fprintf(stderr, "DC_EXTEND diff=%d\n", diff);
      (void)fflush(stderr);
    }
  }
  if (out_sym) {
    *out_sym = sym;
  }
  if (out_diff) {
    *out_diff = diff;
  }
  *dc_predictor += (int16_t)GIMG_JPEG_LSHIFT(diff, al);
  block[0] = *dc_predictor;
  if (trace_all) {
    (void)fprintf(stderr,
        "DC_VALUE predictor=%d block[0]=%d (diff<<al=%d) byte_off=%zu "
        "bit_off=%u\n",
        (int)*dc_predictor, (int)block[0], diff << al, (size_t)bs->byte_off,
        (unsigned)bs->bit_off);
    (void)fflush(stderr);
  }
  return GIMG_OK;
}

/** Progressive DC refinement (Ss=0, Se=0, Ah>0): one bit per block.
 *  T.81 Annex G.1.1.2.1: refinement scan codes "the least significant bit of
 * the point transformed DC coefficients" — the bit at position Al in the scan
 * header. Decoder places that bit into the coefficient: block[0] |= (bit <<
 * Al). (Al is 0-based in the scan; for first refinement pass Ah=1, Al=0 we set
 * bit 0.) When trace_all, log DC_REFINE_ENTER, DC_REFINE_BIT, DC_REFINE_VALUE.
 *  is_last_block: when 1, underflow treated as 0 (T.81 B.2.2 padding
 * unspecified). */
GIMG_Result jpeg_decode_block_progressive_dc_refine(gimg_jpeg_bitstream_t * bs,
    int16_t * block, int16_t * dc_predictor, unsigned int al, int * out_bit,
    int trace_all, int is_last_block) {
  if (trace_all) {
    (void)fprintf(stderr,
        "DC_REFINE_ENTER byte_off=%zu bit_off=%u block[0]=%d al=%u\n",
        (size_t)bs->byte_off, (unsigned)bs->bit_off, (int)block[0],
        (unsigned)al);
    (void)fflush(stderr);
  }
  int b = jpeg_bitstream_read_bit(bs);
  if (b < 0) {
    if (is_last_block) {
      b = 0; // Segment ended; treat as 0 (do not assume padding value).
    }
    else {
      return GIMG_ERR_CORRUPT;
    }
  }
  if (trace_all) {
    (void)fprintf(stderr, "DC_REFINE_BIT bit=%d byte_off=%zu bit_off=%u\n",
        b & 1, (size_t)bs->byte_off, (unsigned)bs->bit_off);
    (void)fflush(stderr);
  }
  if (out_bit) {
    *out_bit = b & 1;
  }
  if (al <= 15u && (b & 1)) {
    block[0] = (int16_t)((uint16_t)(unsigned)block[0] | (1u << al));
  }
  *dc_predictor = block[0];
  if (trace_all) {
    (void)fprintf(stderr,
        "DC_REFINE_VALUE block[0]=%d byte_off=%zu bit_off=%u\n", (int)block[0],
        (size_t)bs->byte_off, (unsigned)bs->bit_off);
    (void)fflush(stderr);
  }
  return GIMG_OK;
}

/** Progressive AC initial (Ah=0): decode band [ss, se], store (value << al).
 * Algorithm: Huffman (run,size), EOB/(r,0)/ZRL; extend value, place at k;
 * repeat. T.81 Annex G: (0,0)=EOB; (15,0)=ZRL (16 zero coeffs); (r,0) r=1..14 =
 * EOB with run length EOBRUN = 2^r + next r bits (then skip EOBRUN-1 following
 * blocks). If out_eobrun is non-NULL we implement EOBRUN; else (r,0) r!=15 is
 * error. If do_trace, emit TRACE_JPEG_AC_SYMBOLS [block=N] run=X size=Y val=Z
 * or EOB. trace_block_id: when >= 0 and do_trace, prefix lines with "block=N ".
 *  trace_scan_idx: when do_trace and TRACE_AC_COMPARE=1, emit canonical
 *  "AC_INITIAL scan=N block=B ..." for compare_progressive_trace.py.
 *  When trace_all, log every step: AC_INITIAL_HUFF_ENTER/EXIT,
 * EOB/ZRL/EOBRUN/COEFF. is_last_block: when 1, underflow treated as EOB (T.81
 * B.2.2 padding unspecified). */
GIMG_Result jpeg_decode_block_progressive_ac_initial(gimg_jpeg_bitstream_t * bs,
    const gimg_jpeg_huff_table_t * ac_tbl, int16_t * block, int ss, int se,
    int al, int do_trace, int trace_block_id, unsigned int trace_scan_idx,
    unsigned int * out_eobrun, int trace_all, int is_last_block) {
  // Full step dump for harmonization with ref
  // (GIMG_JPEG_DUMP_AC_INITIAL_FULL=1).
#if GIMG_JPEG_DUMP_AC_INITIAL_FULL
  const int dump_ac_initial_env = 1;
#else
  const int dump_ac_initial_env = 0;
#endif
  const int dump_full = (trace_block_id >= 0 && dump_ac_initial_env);
  int k = ss;
  // T.81 Annex G: at start of block, if EOBRUN > 0 this block is all-zero in
  // band.
  if (out_eobrun && *out_eobrun > 0) {
    if (trace_all) {
      (void)fprintf(stderr,
          "AC_INITIAL_EOBRUN_SKIP block=%d eobrun=%u (block all-zero) "
          "byte_off=%zu bit_off=%u\n",
          trace_block_id, *out_eobrun, (size_t)bs->byte_off,
          (unsigned)bs->bit_off);
      (void)fflush(stderr);
    }
    if (dump_full) {
      (void)fprintf(stderr,
          "OUR_AC_INITIAL_STEP scan=%u block=%d byte=%zu bit=%u op=EOBRUN_SKIP "
          "eobrun=%u\n",
          trace_scan_idx, trace_block_id, (size_t)bs->byte_off,
          (unsigned)bs->bit_off, *out_eobrun);
      (void)fflush(stderr);
    }
    for (k = ss; k <= se; k++) {
      block[k] = 0;
    }
    (*out_eobrun)--;
    return GIMG_OK;
  }
  while (k <= se) {
    if (trace_all) {
      (void)fprintf(stderr,
          "AC_INITIAL_HUFF_ENTER block=%d k=%d byte_off=%zu bit_off=%u\n",
          trace_block_id, k, (size_t)bs->byte_off, (unsigned)bs->bit_off);
      (void)fflush(stderr);
    }
    // Use first_match_only=0 (longest-match) for progressive AC initial: the
    // standard table can have EOB as a prefix of a longer codeword;
    // longest-match reads the full codeword (T.81 Annex G).
    int sym = jpeg_huff_decode(bs, ac_tbl, 0, 1, 0);
    if (sym < 0) {
      if (is_last_block) {
        sym =
            0; // Segment ended; treat as EOB (T.81 B.2.2 padding unspecified).
      }
      else {
        return GIMG_ERR_CORRUPT;
      }
    }
    int run = sym >> 4;
    int size = sym & 0x0F;
    if (dump_full) {
      (void)fprintf(stderr,
          "OUR_AC_INITIAL_STEP scan=%u block=%d byte=%zu bit=%u op=HUFF "
          "sym=0x%02x\n",
          trace_scan_idx, trace_block_id, (size_t)bs->byte_off,
          (unsigned)bs->bit_off, (unsigned)sym);
      (void)fflush(stderr);
    }
    if (trace_all) {
      (void)fprintf(stderr,
          "AC_INITIAL_HUFF_EXIT block=%d sym=0x%02x run=%d size=%d "
          "byte_off=%zu bit_off=%u\n",
          trace_block_id, (unsigned)sym, run, size, (size_t)bs->byte_off,
          (unsigned)bs->bit_off);
      (void)fflush(stderr);
    }
    if (PROG_SYNC_DEBUG() && trace_block_id >= 0 && sym != 0) {
      (void)fprintf(stderr,
          "PROG_SYNC_DEC scan=%u block=%d byte=%zu bit=%u op=HUFF sym=0x%02x "
          "run=%d size=%d\n",
          trace_scan_idx, trace_block_id, (size_t)bs->byte_off,
          (unsigned)bs->bit_off, (unsigned)sym, run, size);
      (void)fflush(stderr);
    }
    if (sym == 0) {
      if (trace_block_id == 0 && GIMG_JPEG_TRACE_PROG_FIRST_AC) {
        (void)fprintf(stderr, "PROG_DEC_AC block=0 EOB\n");
        (void)fflush(stderr);
      }
      if (dump_full) {
        (void)fprintf(stderr,
            "OUR_AC_INITIAL_STEP scan=%u block=%d byte=%zu bit=%u op=EOB\n",
            trace_scan_idx, trace_block_id, (size_t)bs->byte_off,
            (unsigned)bs->bit_off);
        (void)fflush(stderr);
      }
      if (trace_all) {
        (void)fprintf(stderr,
            "AC_INITIAL_EOB block=%d byte_off=%zu bit_off=%u\n", trace_block_id,
            (size_t)bs->byte_off, (unsigned)bs->bit_off);
        (void)fflush(stderr);
      }
      if (PROG_SYNC_DEBUG() && trace_block_id >= 0) {
        (void)fprintf(stderr,
            "PROG_SYNC_DEC scan=%u block=%d byte=%zu bit=%u op=EOB\n",
            trace_scan_idx, trace_block_id, (size_t)bs->byte_off,
            (unsigned)bs->bit_off);
        (void)fflush(stderr);
      }
      if (do_trace) {
        if (trace_block_id >= 0)
          (void)fprintf(
              stderr, "TRACE_JPEG_AC_SYMBOLS block=%d EOB\n", trace_block_id);
        else
          (void)fprintf(stderr, "TRACE_JPEG_AC_SYMBOLS EOB\n");
        if (GIMG_JPEG_TRACE_AC_COMPARE && trace_block_id >= 0) {
          (void)fprintf(stderr, "AC_INITIAL scan=%u block=%d EOB\n",
              trace_scan_idx, trace_block_id);
          (void)fflush(stderr);
        }
        (void)fflush(stderr);
      }
      for (; k <= se; k++) {
        block[k] = 0;
      }
      break;
    }
    // T.81 Annex G: (0,0)=EOB; (15,0)=ZRL; (r,0) r=1..14 = EOB with run 2^r + r
    // bits.
    if (size == 0) {
      if (run != 15) {
        if (out_eobrun && run >= 1 && run <= 14) {
          // EOB run: EOBRUN = 2^r + next r bits (T.81 Annex G).
          unsigned int eobrun = 1u << (unsigned)run;
          if (run > 0) {
            int rbits = jpeg_bitstream_read_bits(bs, run);
            if (rbits < 0) {
              if (is_last_block) {
                rbits = 0; // T.81 B.2.2: segment end; treat as 0.
              }
              else {
                return GIMG_ERR_CORRUPT;
              }
            }
            eobrun += (unsigned int)rbits;
            if (dump_full) {
              (void)fprintf(stderr,
                  "OUR_AC_INITIAL_STEP scan=%u block=%d byte=%zu bit=%u "
                  "op=EOBRUN r=%d eobrun=%u\n",
                  trace_scan_idx, trace_block_id, (size_t)bs->byte_off,
                  (unsigned)bs->bit_off, run, eobrun);
              (void)fflush(stderr);
            }
            if (trace_all) {
              (void)fprintf(stderr,
                  "AC_INITIAL_EOBRUN_BITS block=%d run=%d rbits=%d eobrun=%u "
                  "byte_off=%zu bit_off=%u\n",
                  trace_block_id, run, rbits, eobrun, (size_t)bs->byte_off,
                  (unsigned)bs->bit_off);
              (void)fflush(stderr);
            }
            if (PROG_SYNC_DEBUG() && trace_block_id >= 0) {
              (void)fprintf(stderr,
                  "PROG_SYNC_DEC scan=%u block=%d byte=%zu bit=%u op=EOBRUN "
                  "r=%d eobrun=%u\n",
                  trace_scan_idx, trace_block_id, (size_t)bs->byte_off,
                  (unsigned)bs->bit_off, run, eobrun);
              (void)fflush(stderr);
            }
          }
          else {
            if (dump_full) {
              (void)fprintf(stderr,
                  "OUR_AC_INITIAL_STEP scan=%u block=%d byte=%zu bit=%u "
                  "op=EOBRUN r=0 eobrun=%u\n",
                  trace_scan_idx, trace_block_id, (size_t)bs->byte_off,
                  (unsigned)bs->bit_off, eobrun);
              (void)fflush(stderr);
            }
            if (trace_all) {
              (void)fprintf(stderr,
                  "AC_INITIAL_EOBRUN block=%d eobrun=%u byte_off=%zu "
                  "bit_off=%u\n",
                  trace_block_id, eobrun, (size_t)bs->byte_off,
                  (unsigned)bs->bit_off);
              (void)fflush(stderr);
            }
          }
          // T.81 G.1.2.2: EOBRUN counts the blocks the run covers *including*
          // this one, which is zeroed just below.  What carries to the blocks
          // that follow is therefore one less.  Storing the full count zeroed
          // one block too many for every EOB run, and the symbols belonging to
          // that block were never read - the scan then ended with bits to
          // spare and every later scan, which depends on these coefficients,
          // decoded against the wrong block state.
          *out_eobrun = eobrun - 1u;
          for (; k <= se; k++) {
            block[k] = 0;
          }
          break;
        }
        return GIMG_ERR_CORRUPT; // (r,0) r!=15 without EOBRUN support.
      }
      // ZRL: 16 zero coefficients (T.81 Annex G).
      if (dump_full) {
        (void)fprintf(stderr,
            "OUR_AC_INITIAL_STEP scan=%u block=%d byte=%zu bit=%u op=ZRL\n",
            trace_scan_idx, trace_block_id, (size_t)bs->byte_off,
            (unsigned)bs->bit_off);
        (void)fflush(stderr);
      }
      if (trace_all) {
        (void)fprintf(stderr,
            "AC_INITIAL_ZRL block=%d k=%d (skip 16 zeros) byte_off=%zu "
            "bit_off=%u\n",
            trace_block_id, k, (size_t)bs->byte_off, (unsigned)bs->bit_off);
        (void)fflush(stderr);
      }
      if (PROG_SYNC_DEBUG() && trace_block_id >= 0) {
        (void)fprintf(stderr,
            "PROG_SYNC_DEC scan=%u block=%d byte=%zu bit=%u op=ZRL\n",
            trace_scan_idx, trace_block_id, (size_t)bs->byte_off,
            (unsigned)bs->bit_off);
        (void)fflush(stderr);
      }
      k += 16;
      if (k > se) {
        for (int i = k - 16; i <= se; i++) {
          block[i] = 0;
        }
        break;
      }
      continue;
    }
    k += run;
    if (k > se) {
      for (int i = k - run; i <= se; i++) {
        block[i] = 0;
      }
      if (size > 0 && jpeg_bitstream_read_bits(bs, size) < 0) {
        if (!is_last_block) {
          return GIMG_ERR_CORRUPT;
        }
      }
      break;
    }
    int ac = 0;
    if (size > 0) {
      ac = jpeg_bitstream_read_bits(bs, size);
      if (ac < 0) {
        if (is_last_block) {
          ac = 0; // T.81 B.2.2: segment end; treat as 0.
        }
        else {
          return GIMG_ERR_CORRUPT;
        }
      }
      if (trace_all) {
        (void)fprintf(stderr,
            "AC_INITIAL_EXTRA_BITS block=%d size=%d raw=%d byte_off=%zu "
            "bit_off=%u\n",
            trace_block_id, size, ac, (size_t)bs->byte_off,
            (unsigned)bs->bit_off);
        (void)fflush(stderr);
      }
      if (PROG_SYNC_DEBUG() && trace_block_id >= 0) {
        (void)fprintf(stderr,
            "PROG_SYNC_DEC scan=%u block=%d byte=%zu bit=%u op=EXTRA n=%d "
            "val=%d\n",
            trace_scan_idx, trace_block_id, (size_t)bs->byte_off,
            (unsigned)bs->bit_off, size, ac);
        (void)fflush(stderr);
      }
      ac = jpeg_extend(ac, size);
    }
    block[k] = (int16_t)GIMG_JPEG_LSHIFT(ac, al);
    if (trace_block_id == 0 && GIMG_JPEG_TRACE_PROG_FIRST_AC) {
      (void)fprintf(stderr, "PROG_DEC_AC block=0 run=%d size=%d val=%d k=%d\n",
          run, size, ac << al, k);
      (void)fflush(stderr);
    }
    if (dump_full) {
      (void)fprintf(stderr,
          "OUR_AC_INITIAL_STEP scan=%u block=%d byte=%zu bit=%u op=COEFF "
          "run=%d size=%d k=%d val=%d\n",
          trace_scan_idx, trace_block_id, (size_t)bs->byte_off,
          (unsigned)bs->bit_off, run, size, k, (int)(ac << al));
      (void)fflush(stderr);
    }
    if (trace_all) {
      (void)fprintf(stderr,
          "AC_INITIAL_COEFF block=%d k=%d val=%d (ac<<al) byte_off=%zu "
          "bit_off=%u\n",
          trace_block_id, k, ac << al, (size_t)bs->byte_off,
          (unsigned)bs->bit_off);
      (void)fflush(stderr);
    }
    if (do_trace) {
      if (trace_block_id >= 0)
        (void)fprintf(stderr,
            "TRACE_JPEG_AC_SYMBOLS block=%d run=%d size=%d val=%d k=%d\n",
            trace_block_id, run, size, ac << al, k);
      else
        (void)fprintf(stderr,
            "TRACE_JPEG_AC_SYMBOLS run=%d size=%d val=%d k=%d\n", run, size,
            ac << al, k);
      if (GIMG_JPEG_TRACE_AC_COMPARE && trace_block_id >= 0) {
        (void)fprintf(stderr,
            "AC_INITIAL scan=%u block=%d run=%d size=%d val=%d k=%d\n",
            trace_scan_idx, trace_block_id, run, size, ac << al, k);
        (void)fflush(stderr);
      }
      (void)fflush(stderr);
    }
    k++;
  }
  return GIMG_OK;
}

/** Progressive AC refinement (Ah!=0). Decoding per ITU-T T.81 | ISO/IEC 10918-1
 *  Annex G.1.2.2 and Figure G.7.
 *
 *  Figure G.7: decode (run, size=1) symbol, read one refinement bit for the
 *  newly-nonzero coefficient, then advance (skip run zeros; for each
 *  already-nonzero read one correction bit). Bitstream order:
 *  [run code][refinement bit][correction bits for already-nz in run], then next
 *  symbol or EOB. G.1.2.2: newly nonzero — bit is sign (1 = +, 0 = −),
 * magnitude at Al is 1; already-nonzero — correction bit 1 means the Al-th bit
 * of the magnitude is 1; only add 2^Al when that bit is not already set
 * (successive approximation). 0 = leave unchanged. After EOB, read one
 * correction bit per already-nonzero from current k to end of band (G.1.2.2).
 *  When do_trace and trace_block_id >= 0, prefix lines with "block=N " for ref.
 *  When trace_all is 1, log every decoding step.
 *  is_last_block: when 1, underflow treated as EOB or 0 (T.81 B.2.2 padding
 * unspecified).
 * Summary: one refinement bit per coefficient in band [ss,se]; (r,0)+EOBRUN for
 * skipped blocks; then correction bits for already-nonzero coeffs (T.81
 * G.1.2.2). */
GIMG_Result jpeg_decode_block_progressive_ac_refine(gimg_jpeg_bitstream_t * bs,
    const gimg_jpeg_huff_table_t * ac_tbl, int16_t * block, int ss, int se,
    int al, int do_trace, int trace_block_id, int log_sanity, int trace_all,
    int trace_scan_idx, unsigned int * out_eobrun, int is_last_block) {
  // Full dump for compare with ref: every byte/bit/block
  // (GIMG_JPEG_DUMP_AC_REFINE_FULL=1).
  const int dump_full = GIMG_JPEG_DUMP_AC_REFINE_FULL;
  // T.81: AC band is [Ss, Se] with 1 <= Ss <= Se <= 63. Clamp to prevent
  // overrun.
  if (se > 63) {
    se = 63;
  }
  if (ss < 1) {
    ss = 1;
  }
  int bitpos = (al >= 0 && al <= 15) ? al : 0;
  int k = ss;

  // T.81 G.1.2.3: an EOB run spans whole blocks, exactly as it does in an
  // AC-initial scan.  A block covered by a running EOB still carries one
  // correction bit for each coefficient already nonzero in the band - the run
  // says "no new coefficients here", not "no bits here".  This decoder used to
  // read the run length and throw it away ("EOBRUN value not needed for
  // single-block decode"), which is only ever right when every run has length
  // one.  Any longer run left the following blocks reading bits that belonged
  // to a later block, and the scan desynchronized from there on.
  if (out_eobrun && *out_eobrun > 0) {
    (*out_eobrun)--;
    for (; k <= se; k++) {
      if (block[k] == 0) {
        continue;
      }
      int rbit = jpeg_bitstream_read_bit(bs);
      if (rbit < 0) {
        if (is_last_block || bs->recover_stuff_zero) {
          rbit = 0; // T.81 B.2.2: padding bit value is unspecified.
        }
        else {
          return GIMG_ERR_CORRUPT;
        }
      }
      if ((rbit & 1) && (block[k] & (1 << bitpos)) == 0) {
        int16_t delta =
            (int16_t)(block[k] >= 0 ? (1 << bitpos) : -(1 << bitpos));
        block[k] += delta;
      }
    }
    return GIMG_OK;
  }

  while (k <= se) {
    int k_at_iter_start = k;
    // Break-the-circle: log start of first two blocks (position + nz count).
    if ((trace_block_id >= 0 && trace_block_id < 2 && k == ss) || trace_all ||
        dump_full) {
      int nz_count = 0;
      for (int i = ss; i <= se; i++) {
        if (block[i] != 0)
          nz_count++;
      }
      (void)fprintf(stderr,
          "AC_REFINE_BLOCK_START block=%d byte_off=%zu bit_off=%u "
          "nz_in_band=%d ptr=%p\n",
          trace_block_id, (size_t)bs->byte_off, (unsigned)bs->bit_off, nz_count,
          (void *)block);
      (void)fflush(stderr);
    }
    if (trace_all) {
      (void)fprintf(stderr,
          "AC_REFINE_HUFF_ENTER block=%d k=%d byte_off=%zu bit_off=%u\n",
          trace_block_id, k, (size_t)bs->byte_off, (unsigned)bs->bit_off);
      (void)fflush(stderr);
    }
    // T.81 Annex F / Table K.6: AC refinement uses 17-symbol table; decode
    // first matching codeword only (no longest-match, no EOB peeking).
    int sym = jpeg_huff_decode(bs, ac_tbl, 1, 1, 1);
    if (sym < 0) {
      if (is_last_block) {
        break; // Segment ended; treat as EOB (T.81 B.2.2 padding unspecified).
      }
      if (bs->recover_stuff_zero) {
        break; // Opt-in recovery only (e.g. truncated stream).
      }
      // T.81 B.2.4: entropy-coded segment ends at the next marker.
      if (GIMG_JPEG_PROGRESSIVE_DEBUG)
        (void)fprintf(stderr,
            " ac_refine underflow: huff_decode at k=%d byte_off=%zu "
            "bit_off=%u\n",
            k, (size_t)bs->byte_off, (unsigned)bs->bit_off);
      return GIMG_ERR_CORRUPT;
    }
    if (dump_full) {
      (void)fprintf(stderr,
          "OUR_AC_REFINE_HUFF sym=0x%02x run=%d size=%d byte_off=%zu "
          "bit_off=%u\n",
          (unsigned)sym, sym >> 4, sym & 15, (size_t)bs->byte_off,
          (unsigned)bs->bit_off);
      (void)fflush(stderr);
    }
    if (sym == 0) {
      if (trace_all) {
        (void)fprintf(stderr,
            "AC_REFINE_HUFF_EXIT block=%d sym=EOB(0) byte_off=%zu bit_off=%u\n",
            trace_block_id, (size_t)bs->byte_off, (unsigned)bs->bit_off);
        (void)fflush(stderr);
      }
      if (PROG_SYNC_DEBUG() && trace_scan_idx >= 0 && trace_block_id >= 0) {
        (void)fprintf(stderr,
            "PROG_SYNC_DEC scan=%d block=%d byte=%zu bit=%u op=EOB\n",
            trace_scan_idx, trace_block_id, (size_t)bs->byte_off,
            (unsigned)bs->bit_off);
        (void)fflush(stderr);
      }
      // T.81 Annex G.1.2.2 / libjpeg jdphuff: after EOB we set EOBRUN and break
      // from the symbol loop; then read one correction bit per already-nonzero
      // coefficient in the band [k,Se]. So we read correction bits here.
      for (; k <= se; k++) {
        if (block[k] != 0) {
          int rbit = jpeg_bitstream_read_bit(bs);
          if (rbit < 0) {
            if (is_last_block || bs->recover_stuff_zero) {
              rbit = 0; // T.81 B.2.2 / recovery: treat as 0.
            }
            else {
              if (GIMG_JPEG_PROGRESSIVE_DEBUG)
                (void)fprintf(stderr,
                    " ac_refine underflow: correction_bit at EOB k=%d "
                    "byte_off=%zu\n",
                    k, (size_t)bs->byte_off);
              return GIMG_ERR_CORRUPT;
            }
          }
          int16_t old_val = block[k];
          if (dump_full) {
            (void)fprintf(stderr,
                "OUR_AC_REFINE_CORRECTION k=%d bit=%d byte_off=%zu "
                "bit_off=%u\n",
                k, rbit & 1, (size_t)bs->byte_off, (unsigned)bs->bit_off);
            (void)fflush(stderr);
          }
          if (rbit & 1) {
            // T.81 Annex G.1.2.2: correction bit 1 = Al-th bit of magnitude is
            // 1; only add when that bit is not already set (successive
            // approximation).
            if ((block[k] & (1 << bitpos)) == 0) {
              int16_t delta =
                  (int16_t)(block[k] >= 0 ? (1 << bitpos) : -(1 << bitpos));
              block[k] += delta;
            }
            if (trace_all) {
              (void)fprintf(stderr,
                  "AC_REFINE_CORRECTION_BIT block=%d k=%d bit=%d delta=%d "
                  "old=%d new=%d byte_off=%zu bit_off=%u (after EOB)\n",
                  trace_block_id, k, rbit & 1, (int)(block[k] - old_val),
                  (int)old_val, (int)block[k], (size_t)bs->byte_off,
                  (unsigned)bs->bit_off);
              (void)fflush(stderr);
            }
          }
          else if (trace_all) {
            (void)fprintf(stderr,
                "AC_REFINE_CORRECTION_BIT block=%d k=%d bit=%d (no change) "
                "byte_off=%zu bit_off=%u (after EOB)\n",
                trace_block_id, k, rbit & 1, (size_t)bs->byte_off,
                (unsigned)bs->bit_off);
            (void)fflush(stderr);
          }
          if (PROG_SYNC_DEBUG() && trace_scan_idx >= 0 && trace_block_id >= 0) {
            (void)fprintf(stderr,
                "PROG_SYNC_DEC scan=%d block=%d byte=%zu bit=%u op=CORR k=%d "
                "bit=%d\n",
                trace_scan_idx, trace_block_id, (size_t)bs->byte_off,
                (unsigned)bs->bit_off, k, rbit & 1);
            (void)fflush(stderr);
          }
        }
      }
      if (trace_block_id == 0) {
        (void)fprintf(stderr, "AC_REFINE_EOB block=0 byte_off=%zu bit_off=%u\n",
            (size_t)bs->byte_off, (unsigned)bs->bit_off);
        (void)fflush(stderr);
      }
      if (do_trace) {
        if (trace_block_id >= 0)
          (void)fprintf(
              stderr, "TRACE_JPEG_AC_REFINE block=%d EOB\n", trace_block_id);
        else
          (void)fprintf(stderr, "TRACE_JPEG_AC_REFINE EOB\n");
        (void)fflush(stderr);
      }
      if (log_sanity && trace_block_id >= 0 && k == ss) {
        (void)fprintf(
            stderr, "SANITY_OUR block=%d first_sym EOB\n", trace_block_id);
        (void)fprintf(stderr,
            "SANITY_OUR block=%d after_first_sym byte_off=%zu bit_off=%u\n",
            trace_block_id, (size_t)bs->byte_off, (unsigned)bs->bit_off);
        (void)fflush(stderr);
      }
      break;
    }
    int run = sym >> 4;
    int size = sym & 15;
    if (trace_all) {
      (void)fprintf(stderr,
          "AC_REFINE_HUFF_EXIT block=%d sym=0x%02x run=%d size=%d byte_off=%zu "
          "bit_off=%u\n",
          trace_block_id, (unsigned)sym, run, size, (size_t)bs->byte_off,
          (unsigned)bs->bit_off);
      (void)fflush(stderr);
    }
    if (PROG_SYNC_DEBUG() && trace_scan_idx >= 0 && trace_block_id >= 0) {
      (void)fprintf(stderr,
          "PROG_SYNC_DEC scan=%d block=%d byte=%zu bit=%u op=HUFF run=%d "
          "size=%d\n",
          trace_scan_idx, trace_block_id, (size_t)bs->byte_off,
          (unsigned)bs->bit_off, run, size);
      (void)fflush(stderr);
    }
    if (do_trace && trace_block_id >= 0 && k == ss) {
      (void)fprintf(stderr, "TRACE_JPEG_AC_REFINE block=%d run=%d size=%d\n",
          trace_block_id, run, size);
      (void)fflush(stderr);
    }
    if (log_sanity && trace_block_id >= 0 && k == ss) {
      (void)fprintf(stderr, "SANITY_OUR block=%d first_sym run=%d size=%d\n",
          trace_block_id, run, size);
      (void)fflush(stderr);
    }
    if (size != 0) {
      int b = jpeg_bitstream_read_bit(bs);
      if (b < 0) {
        if (is_last_block || bs->recover_stuff_zero) {
          b = 0; // T.81 B.2.2 / recovery: treat as 0.
        }
        else {
          if (GIMG_JPEG_PROGRESSIVE_DEBUG)
            (void)fprintf(stderr,
                " ac_refine underflow: refinement_bit at k=%d byte_off=%zu\n",
                k, (size_t)bs->byte_off);
          return GIMG_ERR_CORRUPT;
        }
      }
      if (dump_full) {
        (void)fprintf(stderr,
            "OUR_AC_REFINE_REFINEMENT_BIT bit=%d byte_off=%zu bit_off=%u\n",
            b & 1, (size_t)bs->byte_off, (unsigned)bs->bit_off);
        (void)fflush(stderr);
      }
      if (trace_all) {
        (void)fprintf(stderr,
            "AC_REFINE_REFINEMENT_BIT block=%d bit=%d byte_off=%zu "
            "bit_off=%u\n",
            trace_block_id, b & 1, (size_t)bs->byte_off, (unsigned)bs->bit_off);
        (void)fflush(stderr);
      }
      if (PROG_SYNC_DEBUG() && trace_scan_idx >= 0 && trace_block_id >= 0) {
        (void)fprintf(stderr,
            "PROG_SYNC_DEC scan=%d block=%d byte=%zu bit=%u op=REFINE bit=%d\n",
            trace_scan_idx, trace_block_id, (size_t)bs->byte_off,
            (unsigned)bs->bit_off, b & 1);
        (void)fflush(stderr);
      }
      if (log_sanity && trace_block_id >= 0 && k == ss) {
        (void)fprintf(stderr,
            "SANITY_OUR block=%d after_first_sym byte_off=%zu bit_off=%u\n",
            trace_block_id, (size_t)bs->byte_off, (unsigned)bs->bit_off);
        (void)fflush(stderr);
      }
      // Advance from k: skip run zeros; for each already-nonzero read one
      // correction bit.
      while (run >= 0 && k <= se) {
        if (trace_all) {
          (void)fprintf(stderr,
              "AC_REFINE_STEP block=%d k=%d block_k=%d %s run_left=%d\n",
              trace_block_id, k, (int)block[k], block[k] == 0 ? "zero" : "nz",
              run);
          (void)fflush(stderr);
        }
        if (block[k] == 0) {
          if (--run < 0) {
            break;
          }
        }
        else {
          int rbit = jpeg_bitstream_read_bit(bs);
          if (dump_full) {
            (void)fprintf(stderr,
                "OUR_AC_REFINE_CORRECTION k=%d bit=%d byte_off=%zu "
                "bit_off=%u\n",
                k, rbit >= 0 ? (rbit & 1) : -1, (size_t)bs->byte_off,
                (unsigned)bs->bit_off);
            (void)fflush(stderr);
          }
          if (rbit < 0) {
            if (is_last_block || bs->recover_stuff_zero) {
              rbit = 0; // T.81 B.2.2 / recovery: treat as 0.
            }
            else {
              if (GIMG_JPEG_PROGRESSIVE_DEBUG)
                (void)fprintf(stderr,
                    " ac_refine underflow: correction_bit at k=%d "
                    "byte_off=%zu\n",
                    k, (size_t)bs->byte_off);
              return GIMG_ERR_CORRUPT;
            }
          }
          int16_t old_val = block[k];
          // T.81 Annex G.1.2.2: correction bit 1 = Al-th bit of magnitude is 1;
          // only add when that bit is not already set (successive
          // approximation).
          if (rbit & 1) {
            if ((block[k] & (1 << bitpos)) == 0) {
              int16_t delta =
                  (int16_t)(block[k] >= 0 ? (1 << bitpos) : -(1 << bitpos));
              block[k] += delta;
            }
            if (trace_all) {
              (void)fprintf(stderr,
                  "AC_REFINE_CORRECTION_BIT block=%d k=%d bit=%d delta=%d "
                  "old=%d new=%d byte_off=%zu bit_off=%u\n",
                  trace_block_id, k, rbit & 1, (int)(block[k] - old_val),
                  (int)old_val, (int)block[k], (size_t)bs->byte_off,
                  (unsigned)bs->bit_off);
              (void)fflush(stderr);
            }
          }
          else if (trace_all) {
            (void)fprintf(stderr,
                "AC_REFINE_CORRECTION_BIT block=%d k=%d bit=%d (no change) "
                "byte_off=%zu bit_off=%u\n",
                trace_block_id, k, rbit & 1, (size_t)bs->byte_off,
                (unsigned)bs->bit_off);
            (void)fflush(stderr);
          }
          if (PROG_SYNC_DEBUG() && trace_scan_idx >= 0 && trace_block_id >= 0) {
            (void)fprintf(stderr,
                "PROG_SYNC_DEC scan=%d block=%d byte=%zu bit=%u op=CORR k=%d "
                "bit=%d\n",
                trace_scan_idx, trace_block_id, (size_t)bs->byte_off,
                (unsigned)bs->bit_off, k, rbit & 1);
            (void)fflush(stderr);
          }
          if (trace_block_id == 0 && GIMG_JPEG_AC_REFINE_CORRECTION_K &&
              !trace_all) {
            (void)fprintf(stderr,
                "AC_REFINE_CORRECTION_K block=0 k=%d block[k]=%d\n", k,
                (int)block[k]);
            (void)fflush(stderr);
          }
          if (do_trace && !trace_all) {
            if (trace_block_id >= 0)
              (void)fprintf(stderr,
                  "TRACE_JPEG_AC_REFINE block=%d k=%d (already nz) bit=%d\n",
                  trace_block_id, k, rbit & 1);
            else
              (void)fprintf(stderr,
                  "TRACE_JPEG_AC_REFINE k=%d (already nz) bit=%d\n", k,
                  rbit & 1);
            (void)fflush(stderr);
          }
        }
        k++;
      }
      if (k > se) {
        // The run asked for more still-zero coefficients than the band holds.
        // libjpeg tolerates this (its zigzag table is padded so the index
        // clamps to 63) rather than rejecting the file, so do the same: put the
        // new coefficient in the last position of the band and carry on.
        k = se;
      }
      if (do_trace) {
        if (trace_block_id >= 0)
          (void)fprintf(stderr,
              "TRACE_JPEG_AC_REFINE block=%d k=%d bit=%d (bitpos=%d)\n",
              trace_block_id, k, b & 1, bitpos);
        else
          (void)fprintf(stderr,
              "TRACE_JPEG_AC_REFINE k=%d bit=%d (bitpos=%d)\n", k, b & 1,
              bitpos);
        (void)fflush(stderr);
      }
      // Newly nonzero: refinement bit is the sign (1 = +, 0 = −); magnitude at
      // Al is 1.
      if (k > se || k < ss) {
        if (GIMG_JPEG_PROGRESSIVE_DEBUG)
          (void)fprintf(
              stderr, " ac_refine k=%d out of band [%d,%d]\n", k, ss, se);
        return GIMG_ERR_CORRUPT;
      }
      {
        int16_t val = (int16_t)((b & 1) ? (1 << bitpos) : -(1 << bitpos));
        block[k] = val;
        if (trace_all) {
          (void)fprintf(stderr,
              "AC_REFINE_NEW_NZ block=%d k=%d val=%d (bitpos=%d sign=%d) "
              "byte_off=%zu bit_off=%u\n",
              trace_block_id, k, (int)val, bitpos, (b & 1) ? 1 : 0,
              (size_t)bs->byte_off, (unsigned)bs->bit_off);
          (void)fflush(stderr);
        }
      }
      k++;
    }
    else {
      // size==0: T.81 Annex G.1.2.2 — (r,0) is either EOB run (r < 15) or ZRL
      // (r == 15). Spec defines (r,0) for r < 15 as EOB run with run length 2^r
      // + (r appended bits).
      if (run != 15) {
        // EOB run: consume r appended bits, then correction bits for nonzero in
        // band.
        // EOBRUN = 2^r + the r appended bits (T.81 G.1.2.3).  This block is
        // the first of the run, so the rest carries to later blocks.
        unsigned int eobrun_extra = 0u;
        int eobrun_bits = run;
        if (eobrun_bits > 0) {
          for (int bi = 0; bi < eobrun_bits; bi++) {
            int b = jpeg_bitstream_read_bit(bs);
            if (dump_full && bi == eobrun_bits - 1) {
              (void)fprintf(stderr,
                  "OUR_AC_REFINE_EOB_RUN r=%d byte_off=%zu bit_off=%u\n", run,
                  (size_t)bs->byte_off, (unsigned)bs->bit_off);
              (void)fflush(stderr);
            }
            if (b < 0) {
              if (bs->recover_stuff_zero) {
                b = 0;
              }
              else {
                if (GIMG_JPEG_PROGRESSIVE_DEBUG)
                  (void)fprintf(stderr,
                      " ac_refine underflow: eobrun bits at run=%d "
                      "byte_off=%zu\n",
                      run, (size_t)bs->byte_off);
                return GIMG_ERR_CORRUPT;
              }
            }
            eobrun_extra = (eobrun_extra << 1) | (unsigned int)(b & 1);
          }
        }
        if (out_eobrun) {
          unsigned int eobrun_val =
              (1u << (unsigned int)run) + eobrun_extra;
          *out_eobrun = eobrun_val - 1u; // this block is the first of the run
        }
        for (; k <= se; k++) {
          if (block[k] != 0) {
            int rbit = jpeg_bitstream_read_bit(bs);
            if (dump_full) {
              (void)fprintf(stderr,
                  "OUR_AC_REFINE_CORRECTION k=%d bit=%d byte_off=%zu "
                  "bit_off=%u\n",
                  k, rbit >= 0 ? (rbit & 1) : -1, (size_t)bs->byte_off,
                  (unsigned)bs->bit_off);
              (void)fflush(stderr);
            }
            if (rbit < 0) {
              if (is_last_block || bs->recover_stuff_zero) {
                rbit = 0; // T.81 B.2.2 / recovery: treat as 0.
              }
              else {
                if (GIMG_JPEG_PROGRESSIVE_DEBUG)
                  (void)fprintf(stderr,
                      " ac_refine underflow: correction_bit (eob) k=%d "
                      "byte_off=%zu\n",
                      k, (size_t)bs->byte_off);
                return GIMG_ERR_CORRUPT;
              }
            }
            if (rbit & 1) {
              if ((block[k] & (1 << bitpos)) == 0) {
                int16_t delta =
                    (int16_t)(block[k] >= 0 ? (1 << bitpos) : -(1 << bitpos));
                block[k] += delta;
              }
            }
          }
        }
        break; // EOB: done with this block
      }
      // run == 15: ZRL.  T.81 G.1.2.3: skip 16 coefficients that are still
      // zero at this precision - and read one correction bit for every
      // already-nonzero coefficient passed along the way, exactly as the
      // run-skip above does.  libjpeg routes ZRL through that same loop for
      // this reason.  Skipping the nonzeroes silently, as this did, drops one
      // bit per nonzero coefficient and desynchronizes the rest of the scan;
      // chroma refinement, which is dense in ZRL, never survived it.
      int left = 16;
      while (left > 0 && k <= se) {
        if (block[k] == 0) {
          left--;
        }
        else {
          int rbit = jpeg_bitstream_read_bit(bs);
          if (rbit < 0) {
            if (is_last_block || bs->recover_stuff_zero) {
              rbit = 0; // T.81 B.2.2: padding bit value is unspecified.
            }
            else {
              return GIMG_ERR_CORRUPT;
            }
          }
          if ((rbit & 1) && (block[k] & (1 << bitpos)) == 0) {
            int16_t delta =
                (int16_t)(block[k] >= 0 ? (1 << bitpos) : -(1 << bitpos));
            block[k] += delta;
          }
        }
        k++;
      }
    }
    // Break-the-circle: log position after first symbol of block 0 (before next
    // Huffman decode). If (0,5) then next bit is 6th; if (0,4) we're one bit
    // short.
    if (trace_block_id == 0 && k_at_iter_start == ss) {
      (void)fprintf(stderr,
          "AC_REFINE_AFTER_FIRST_SYM block=0 byte_off=%zu bit_off=%u\n",
          (size_t)bs->byte_off, (unsigned)bs->bit_off);
      (void)fflush(stderr);
    }
  }
  return GIMG_OK;
}

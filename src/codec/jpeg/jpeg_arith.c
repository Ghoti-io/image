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
 * Arithmetic entropy coding for JPEG: the adaptive binary arithmetic decoder of
 * ITU-T T.81 Annex D, and the DC and AC decoding procedures of F.2.4 built on
 * top of it.  Used by jpeg_entropy.c for SOF9 and SOF10 frames, exactly as
 * jpeg_bitstream.c and jpeg_block.c are used for the Huffman-coded ones.
 *
 * Arithmetic coding is the other entropy coder T.81 defines.  It is not an
 * extension or a variant: Annex D is normative, and a frame that uses it is as
 * much a JPEG as a Huffman-coded one.  It is rare in the wild only because the
 * patents that covered it (now long expired) kept it out of the early
 * implementations that everything else copied.
 */

#include <string.h>

#include <ghoti.io/image/macros.h>
#include "jpeg_internal.h"

/**
 * Table D.2 - Qe values and probability estimation state machine.
 *
 * This is the conditional exchange table from ITU-T T.81 (1992) Table D.2,
 * which is the same table as Table 24 of T.82 (JBIG).  Each state gives the
 * estimated probability of the less probable symbol (Qe, as a 16-bit binary
 * fraction), the state to move to after coding an MPS or an LPS, and whether
 * an LPS at that state also exchanges the sense of the MPS.
 *
 * Index 113 is not a state the machine can reach; it terminates the table so
 * that a corrupt index read from a statistics bin cannot run off the end.
 */
typedef struct {
  uint16_t qe;      /**< Qe: LPS probability estimate, Q15 */
  uint8_t nmps;     /**< next index after coding the more probable symbol */
  uint8_t nlps;     /**< next index after coding the less probable symbol */
  uint8_t switch_mps; /**< 1: an LPS here also exchanges MPS and LPS */
} jpeg_arith_state_t;

static const jpeg_arith_state_t jpeg_arith_qe[114] = {
    {0x5A1D,   1,   1, 1},
    {0x2586,   2,  14, 0},
    {0x1114,   3,  16, 0},
    {0x080B,   4,  18, 0},
    {0x03D8,   5,  20, 0},
    {0x01DA,   6,  23, 0},
    {0x00E5,   7,  25, 0},
    {0x006F,   8,  28, 0},
    {0x0036,   9,  30, 0},
    {0x001A,  10,  33, 0},
    {0x000D,  11,  35, 0},
    {0x0006,  12,   9, 0},
    {0x0003,  13,  10, 0},
    {0x0001,  13,  12, 0},
    {0x5A7F,  15,  15, 1},
    {0x3F25,  16,  36, 0},
    {0x2CF2,  17,  38, 0},
    {0x207C,  18,  39, 0},
    {0x17B9,  19,  40, 0},
    {0x1182,  20,  42, 0},
    {0x0CEF,  21,  43, 0},
    {0x09A1,  22,  45, 0},
    {0x072F,  23,  46, 0},
    {0x055C,  24,  48, 0},
    {0x0406,  25,  49, 0},
    {0x0303,  26,  51, 0},
    {0x0240,  27,  52, 0},
    {0x01B1,  28,  54, 0},
    {0x0144,  29,  56, 0},
    {0x00F5,  30,  57, 0},
    {0x00B7,  31,  59, 0},
    {0x008A,  32,  60, 0},
    {0x0068,  33,  62, 0},
    {0x004E,  34,  63, 0},
    {0x003B,  35,  32, 0},
    {0x002C,   9,  33, 0},
    {0x5AE1,  37,  37, 1},
    {0x484C,  38,  64, 0},
    {0x3A0D,  39,  65, 0},
    {0x2EF1,  40,  67, 0},
    {0x261F,  41,  68, 0},
    {0x1F33,  42,  69, 0},
    {0x19A8,  43,  70, 0},
    {0x1518,  44,  72, 0},
    {0x1177,  45,  73, 0},
    {0x0E74,  46,  74, 0},
    {0x0BFB,  47,  75, 0},
    {0x09F8,  48,  77, 0},
    {0x0861,  49,  78, 0},
    {0x0706,  50,  79, 0},
    {0x05CD,  51,  48, 0},
    {0x04DE,  52,  50, 0},
    {0x040F,  53,  50, 0},
    {0x0363,  54,  51, 0},
    {0x02D4,  55,  52, 0},
    {0x025C,  56,  53, 0},
    {0x01F8,  57,  54, 0},
    {0x01A4,  58,  55, 0},
    {0x0160,  59,  56, 0},
    {0x0125,  60,  57, 0},
    {0x00F6,  61,  58, 0},
    {0x00CB,  62,  59, 0},
    {0x00AB,  63,  61, 0},
    {0x008F,  32,  61, 0},
    {0x5B12,  65,  65, 1},
    {0x4D04,  66,  80, 0},
    {0x412C,  67,  81, 0},
    {0x37D8,  68,  82, 0},
    {0x2FE8,  69,  83, 0},
    {0x293C,  70,  84, 0},
    {0x2379,  71,  86, 0},
    {0x1EDF,  72,  87, 0},
    {0x1AA9,  73,  87, 0},
    {0x174E,  74,  72, 0},
    {0x1424,  75,  72, 0},
    {0x119C,  76,  74, 0},
    {0x0F6B,  77,  74, 0},
    {0x0D51,  78,  75, 0},
    {0x0BB6,  79,  77, 0},
    {0x0A40,  48,  77, 0},
    {0x5832,  81,  80, 1},
    {0x4D1C,  82,  88, 0},
    {0x438E,  83,  89, 0},
    {0x3BDD,  84,  90, 0},
    {0x34EE,  85,  91, 0},
    {0x2EAE,  86,  92, 0},
    {0x299A,  87,  93, 0},
    {0x2516,  71,  86, 0},
    {0x5570,  89,  88, 1},
    {0x4CA9,  90,  95, 0},
    {0x44D9,  91,  96, 0},
    {0x3E22,  92,  97, 0},
    {0x3824,  93,  99, 0},
    {0x32B4,  94,  99, 0},
    {0x2E17,  86,  93, 0},
    {0x56A8,  96,  95, 1},
    {0x4F46,  97, 101, 0},
    {0x47E5,  98, 102, 0},
    {0x41CF,  99, 103, 0},
    {0x3C3D, 100, 104, 0},
    {0x375E,  93,  99, 0},
    {0x5231, 102, 105, 0},
    {0x4C0F, 103, 106, 0},
    {0x4639, 104, 107, 0},
    {0x415E,  99, 103, 0},
    {0x5627, 106, 105, 1},
    {0x50E7, 107, 108, 0},
    {0x4B85, 103, 109, 0},
    {0x5597, 109, 110, 0},
    {0x504F, 107, 111, 0},
    {0x5A10, 111, 110, 1},
    {0x5522, 109, 112, 0},
    {0x59EB, 111, 112, 1},
    {0x5A1D, 113, 113, 0},};

/**
 * INITDEC (T.81 D.2.8, Figure D.20).
 *
 * The spec primes the C register with two bytes and sets A to 0x8000.  The
 * equivalent formulation used here, and in libjpeg, starts with A and C at zero
 * and CT at -16, so that the renormalization loop at the head of the decode
 * procedure performs the priming: it is the same sequence of byte fetches, with
 * one place that reads input instead of two.
 */
void jpeg_arith_decoder_init(jpeg_arith_decoder_t * d,
    const unsigned char * data, size_t size) {
  memset(d, 0, sizeof(*d));
  d->data = data;
  d->size = size;
  // CT starts at -16 so that the renormalization loop reads the two priming
  // bytes before A becomes meaningful.  At 0 the loop would shift a zero A for
  // ever, because nothing would ever set it to 0x8000.
  d->ct = -16;
}

/**
 * BYTEIN (T.81 D.2.9, Figure D.19) - fetch the next compressed byte.
 *
 * B.1.1.5 byte stuffing applies: a 0xFF in the entropy-coded segment is
 * followed by a stuffed 0x00, which is discarded.  A 0xFF followed by anything
 * else is a marker, and - unlike the Huffman case, where reaching a marker
 * early means the data is broken - T.81 D.2.9 makes this normal: the decoder
 * keeps going and is fed zero bytes until it has produced every coefficient the
 * scan header promised.  The marker is remembered for the caller.
 */
static int jpeg_arith_bytein(jpeg_arith_decoder_t * d) {
  if (d->marker) {
    return 0; // past the end of the segment: supply zeros (D.2.9)
  }
  if (d->pos >= d->size) {
    d->marker = 0xD9; // treat running out like reaching EOI
    return 0;
  }
  int b = d->data[d->pos++];
  if (b != 0xFF) {
    return b;
  }
  // Skip the 0xFF fill bytes B.1.1.2 permits before a marker.
  while (d->pos < d->size && d->data[d->pos] == 0xFF) {
    d->pos++;
  }
  if (d->pos >= d->size) {
    d->marker = 0xD9;
    return 0;
  }
  int next = d->data[d->pos++];
  if (next == 0x00) {
    return 0xFF; // stuffed zero: the 0xFF was data (B.1.1.5)
  }
  d->marker = (uint8_t)next;
  return 0;
}

/**
 * DECODE (T.81 D.2.4, Figure D.15), with MPS_EXCHANGE (Figure D.16),
 * LPS_EXCHANGE (Figure D.17) and RENORMD (Figure D.18).
 *
 * @param st One statistics bin: bit 7 is MPS(S), bits 0..6 are Index(S).
 * @return The decoded binary decision, 0 or 1.
 *
 * T.81 gives the MPS the lower subinterval [0, A-Qe) and the LPS the upper
 * one - the opposite of the otherwise identical coder in T.88 and JPEG 2000,
 * which is worth stating because the two are easy to confuse.  The comparison
 * below is against (A - Qe) shifted left by CT rather than against Chigh,
 * which avoids keeping the high and low halves of C apart; it is the same test.
 */
int jpeg_arith_decode(jpeg_arith_decoder_t * d, uint8_t * st) {
  // RENORMD (Figure D.18), inlined at the head so that priming and
  // renormalization share one path.
  while (d->a < 0x8000) {
    if (--d->ct < 0) {
      int data = jpeg_arith_bytein(d);
      d->c = (d->c << 8) | (uint32_t)data;
      d->ct += 8;
      if (d->ct < 0) {
        // Still priming: two bytes are needed before A is meaningful.
        if (++d->ct == 0) {
          d->a = 0x8000; // becomes 0x10000 when the shift below runs
        }
      }
    }
    d->a = (int32_t)((uint32_t)d->a << 1);
  }

  uint8_t sv = *st;
  const jpeg_arith_state_t * s = &jpeg_arith_qe[sv & 0x7Fu];
  int32_t qe = (int32_t)s->qe;

  int32_t below = d->a - qe; // the MPS subinterval
  d->a = below;
  int32_t threshold = (int32_t)((uint32_t)below << d->ct);

  if (d->c >= (uint32_t)threshold) {
    // Upper subinterval: LPS_EXCHANGE then RENORMD (next call).
    d->c -= (uint32_t)threshold;
    d->a = qe;
    if (below < qe) {
      // Conditional exchange: the "less probable" symbol was in fact the more
      // probable one here, so the decision is the MPS after all.
      *st = (uint8_t)((sv & 0x80u) | s->nmps);
    }
    else {
      *st = (uint8_t)((sv & 0x80u) | s->nlps);
      if (s->switch_mps) {
        *st ^= 0x80u;
      }
      sv ^= 0x80u; // the decision is 1 - MPS(S)
    }
  }
  else if (d->a < 0x8000) {
    // Lower subinterval, but A has fallen below 0x8000: MPS_EXCHANGE.
    if (below < qe) {
      *st = (uint8_t)((sv & 0x80u) | s->nlps);
      if (s->switch_mps) {
        *st ^= 0x80u;
      }
      sv ^= 0x80u;
    }
    else {
      *st = (uint8_t)((sv & 0x80u) | s->nmps);
    }
  }
  // Otherwise the interval is still large enough and the state does not move.

  return sv >> 7;
}

/**
 * The conditioning bounds this library never writes.
 *
 * T.81 F.1.4.4.1.2 classifies a DC difference as small or large by comparing
 * its magnitude against `(1 << L) >> 1`, where L comes from the DAC segment
 * (B.2.4.3).  The default L is 0, which makes that bound 0 - and a magnitude
 * is never negative, so with the default conditioning the "small" arm cannot
 * be taken at all.  It appears three times: twice in the decoder and once in
 * the encoder, and none of the three has ever run.
 *
 * That is not an accident of the fixtures.  jpeg_save.c writes B.2.4.3's
 * defaults on every arithmetic frame and offers no way to ask for anything
 * else, so no file this library produces can reach those arms, and no
 * round trip can test them.  Reaching them needs a file from an encoder that
 * states a non-zero L, and the corpus has none - every DAC in it is the
 * default pair.  jpeg_load.c does read L and store it, so the arms are live
 * for such a file; they are simply not reachable from here.
 *
 * Recorded so the next reader does not spend an afternoon looking for a
 * fixture that cannot be built with what is in this repository.
 */
void jpeg_arith_cond_defaults(jpeg_arith_cond_t * cond) {
  // T.81 B.2.4.3: absent a DAC segment, the conditioning is L = 0, U = 1 for
  // the DC tables and Kx = 5 for the AC tables.
  for (int i = 0; i < GIMG_JPEG_ARITH_TABLES; i++) {
    cond->dc_l[i] = 0;
    cond->dc_u[i] = 1;
    cond->ac_k[i] = 5;
  }
}

void jpeg_arith_stats_reset(jpeg_arith_stats_t * s) {
  // T.81 F.2.4.1: every statistics area starts in state 0 with an MPS of 0, and
  // the DC predictors and conditioning categories start at zero.  This happens
  // at the start of a scan and again after each restart marker.
  memset(s, 0, sizeof(*s));
  // The exception is the bin used for AC signs (F.1.4.4.2), which is not
  // adaptive at all: a coefficient is as likely to be negative as positive, so
  // it is coded at a fixed probability of one half.  Index 113 is the state
  // that provides it - Qe is about a half and both transitions lead back to
  // itself, so the estimate never moves.  Leaving this at 0 gives a bin with
  // the same initial Qe that then adapts, which decodes the first few signs
  // correctly and drifts apart from the encoder after that.
  s->fixed = 113;
}

/**
 * Decode the magnitude bits of a non-zero value (T.81 Figure F.24).
 *
 * @p m is the leading bit of the magnitude, already established; the remaining
 * bits are read from successive bins below @p st.  The value returned is the
 * magnitude, which the caller signs.
 */
static int32_t jpeg_arith_decode_magnitude_bits(
    jpeg_arith_decoder_t * d, uint8_t * st, int32_t m) {
  int32_t v = m;
  while ((m >>= 1) != 0) {
    if (jpeg_arith_decode(d, st)) {
      v |= m;
    }
  }
  return v + 1;
}

/**
 * Decode a DC difference (T.81 F.2.4.2, Figures F.19 through F.24).
 *
 * The DC model is conditioned on how large the previous difference in this
 * component was (F.1.4.4.1.2): zero, small, or large, each with a sign, giving
 * the five-way classification that selects which group of bins to use next.
 * That is why dc_context is per component and survives from block to block.
 */
static GIMG_Result jpeg_arith_decode_dc(jpeg_arith_decoder_t * d,
    jpeg_arith_stats_t * stats, const jpeg_arith_cond_t * cond, uint8_t comp,
    uint8_t dc_tbl, int16_t * block) {
  uint8_t * area = stats->dc[dc_tbl];
  uint8_t * st = area + stats->dc_context[comp];

  // Figure F.19: is the difference zero?
  if (jpeg_arith_decode(d, st) == 0) {
    stats->dc_context[comp] = 0;
    block[0] = (int16_t)stats->dc_pred[comp];
    return GIMG_OK;
  }

  // Figure F.22: the sign.
  int sign = jpeg_arith_decode(d, st + 1);
  st += 2 + (size_t)sign;

  // Figure F.23: the magnitude category, as a run of decisions each saying
  // "the magnitude needs another bit".
  int32_t m = jpeg_arith_decode(d, st);
  if (m != 0) {
    st = area + 20; // Table F.4: the magnitude chain starts at X1 = 20
    while (jpeg_arith_decode(d, st)) {
      m <<= 1;
      if (m == 0x8000) {
        // A magnitude this large cannot be represented; the data is corrupt.
        return GIMG_ERR_CORRUPT;
      }
      st += 1;
    }
  }

  // F.1.4.4.1.2: classify this difference for the next block's context.  L and
  // U come from the DAC segment and say where "small" ends and "large" begins.
  if (m < ((int32_t)1 << cond->dc_l[dc_tbl]) >> 1) {
    stats->dc_context[comp] = 0;
  }
  else if (m > ((int32_t)1 << cond->dc_u[dc_tbl]) >> 1) {
    stats->dc_context[comp] = 12 + (sign * 4);
  }
  else {
    stats->dc_context[comp] = 4 + (sign * 4);
  }

  int32_t v = jpeg_arith_decode_magnitude_bits(d, st + 14, m);
  if (sign) {
    v = -v;
  }
  stats->dc_pred[comp] += (int)v;
  block[0] = (int16_t)stats->dc_pred[comp];
  return GIMG_OK;
}

/**
 * Decode the AC coefficients of a block (T.81 F.2.4.3, Figures F.20 to F.25).
 *
 * Unlike the Huffman coder, which spends a symbol on each run of zeros, the
 * arithmetic coder asks two questions at each position: "is this the end of the
 * block?" and "is this coefficient zero?".  Each position has its own bins, so
 * the model learns where in a block coefficients tend to stop.
 */
static GIMG_Result jpeg_arith_decode_ac(jpeg_arith_decoder_t * d,
    jpeg_arith_stats_t * stats, const jpeg_arith_cond_t * cond, uint8_t ac_tbl,
    int se, int16_t * block) {
  uint8_t * area = stats->ac[ac_tbl];
  uint8_t kx = cond->ac_k[ac_tbl];

  for (int k = 1; k <= se; k++) {
    uint8_t * st = area + 3 * (k - 1);
    if (jpeg_arith_decode(d, st)) {
      break; // Figure F.20: end of block
    }
    while (jpeg_arith_decode(d, st + 1) == 0) {
      st += 3;
      k++;
      if (k > se) {
        return GIMG_ERR_CORRUPT; // ran past the last coefficient of the block
      }
    }

    // F.1.4.4.2: the AC sign is coded with a fixed probability of one half
    // rather than an adaptive bin.
    int sign = jpeg_arith_decode(d, &stats->fixed);
    st += 2;

    int32_t m = jpeg_arith_decode(d, st);
    if (m != 0) {
      if (jpeg_arith_decode(d, st)) {
        m <<= 1;
        // Table F.5: which magnitude chain to use depends on whether this
        // coefficient sits before or after Kx in the block.  Positions past Kx
        // hold high-frequency detail and have their own statistics.
        st = area + (k <= (int)kx ? 189 : 217);
        while (jpeg_arith_decode(d, st)) {
          m <<= 1;
          if (m == 0x8000) {
            return GIMG_ERR_CORRUPT;
          }
          st += 1;
        }
      }
    }

    int32_t v = jpeg_arith_decode_magnitude_bits(d, st + 14, m);
    if (sign) {
      v = -v;
    }
    block[k] = (int16_t)v;
  }
  return GIMG_OK;
}

GIMG_Result jpeg_arith_decode_block_sequential(jpeg_arith_decoder_t * d,
    jpeg_arith_stats_t * stats, const jpeg_arith_cond_t * cond, uint8_t comp,
    uint8_t dc_tbl, uint8_t ac_tbl, int se, int16_t * block) {
  memset(block, 0, 64 * sizeof(int16_t));
  GIMG_Result r = jpeg_arith_decode_dc(d, stats, cond, comp, dc_tbl, block);
  if (r != GIMG_OK) {
    return r;
  }
  return jpeg_arith_decode_ac(d, stats, cond, ac_tbl, se, block);
}

/**
 * Resynchronize at a restart marker (T.81 F.2.4.1 and B.2.1).
 *
 * The arithmetic coder handles restarts quite differently from the Huffman one.
 * There is no bit alignment to do, because the coder's output is a byte stream
 * already; instead the decoder is discarded and started again from scratch
 * after the marker, and the adaptive statistics and the DC predictors go back
 * to their initial state.  That is what makes a restart interval a genuine
 * resynchronization point: everything the decoder had learned is forgotten, so
 * a later interval can be decoded without the earlier ones.
 *
 * The decoder may or may not already have run into the marker - it stops
 * producing output as soon as it has the coefficients it was asked for, which
 * can be a byte or two early - so scan forward for it either way.
 */
//
// Lossless arithmetic coding (T.81 Annex H, SOF11).
//
// H.1.2.3: the DC model of F.1.4.4.1 generalized to two dimensions.  A DCT DC
// difference is conditioned on the one difference that preceded it in the same
// component; a lossless difference is conditioned on two - the sample to the
// left and the sample above - because the data is a raster of samples rather
// than a sequence of blocks, and both neighbors say something about how
// active this part of the image is.  Each of the two is classified into the
// same five categories (H.1.2.3.1), and the pair selects one of 25 states.
//

void jpeg_arith_lossless_stats_reset(jpeg_arith_lossless_stats_t * s) {
  memset(s->ll, 0, sizeof(s->ll));
}

/**
 * Classify a difference for use as conditioning (T.81 H.1.2.3.1).
 *
 * @p m is the leading bit of the magnitude, as the coding procedure
 * established it, and @p sign is non-zero for a negative difference.  L and U
 * come from the DAC segment (H.1.2.3.3 gives the defaults L = 0, U = 1) and
 * say where "small" ends and "large" begins.
 */
static int jpeg_arith_lossless_classify(
    int32_t m, int sign, const jpeg_arith_cond_t * cond, uint8_t tbl) {
  if (m < ((int32_t)1 << cond->dc_l[tbl]) >> 1) {
    return JPEG_LL_CAT_ZERO;
  }
  if (m > ((int32_t)1 << cond->dc_u[tbl]) >> 1) {
    return sign ? JPEG_LL_CAT_LARGE_NEG : JPEG_LL_CAT_LARGE_POS;
  }
  return sign ? JPEG_LL_CAT_SMALL_NEG : JPEG_LL_CAT_SMALL_POS;
}

/** T.81 H.1.2.3.2: S0 = L_Context(Da,Db), read out of the Figure H.2 array -
 * five rows of five entries, four bins apiece. */
static size_t jpeg_arith_lossless_context(int da_cat, int db_cat) {
  return (size_t)(20 * da_cat + 4 * db_cat);
}

/** T.81 H.1.2.3.2: X1_Context(Db).  The magnitude chain sits at 100 when the
 * difference above was zero or small, and at 129 when it was large. */
static size_t jpeg_arith_lossless_x1(int db_cat) {
  return (db_cat == JPEG_LL_CAT_LARGE_POS || db_cat == JPEG_LL_CAT_LARGE_NEG)
      ? 129u
      : 100u;
}

GIMG_Result jpeg_arith_lossless_decode_diff(jpeg_arith_decoder_t * d,
    jpeg_arith_lossless_stats_t * stats, const jpeg_arith_cond_t * cond,
    uint8_t tbl, int da_cat, int db_cat, int32_t * out_diff, int * out_cat) {
  uint8_t * area = stats->ll[tbl];
  uint8_t * st = area + jpeg_arith_lossless_context(da_cat, db_cat);

  // Table H.3, S0: is the difference zero?
  if (jpeg_arith_decode(d, st) == 0) {
    *out_diff = 0;
    *out_cat = JPEG_LL_CAT_ZERO;
    return GIMG_OK;
  }

  // Table H.3, SS = S0 + 1: the sign, which then picks SP or SN.
  int sign = jpeg_arith_decode(d, st + 1);
  uint8_t * stm = st + 2 + (size_t)sign;

  // Table H.3, X1..X15: the magnitude category, as a run of decisions each
  // saying "the magnitude needs another bit".
  int32_t m = jpeg_arith_decode(d, stm);
  if (m != 0) {
    stm = area + jpeg_arith_lossless_x1(db_cat);
    int k = 0;
    while (jpeg_arith_decode(d, stm)) {
      m <<= 1;
      // The chain ends at X15, so a magnitude needing a sixteenth doubling is
      // outside anything Table H.3 can express and the data is corrupt.  The
      // bound also keeps stm inside the 158-bin area: X15 + 14 is M15, the
      // last bin there is.
      if (++k > 14) {
        return GIMG_ERR_CORRUPT;
      }
      stm += 1;
    }
  }

  *out_cat = jpeg_arith_lossless_classify(m, sign, cond, tbl);

  // Table H.3, M2..M15: the remaining magnitude bits, 14 bins above the
  // category bin that named them.
  int32_t v = jpeg_arith_decode_magnitude_bits(d, stm + 14, m);
  *out_diff = sign ? -v : v;
  return GIMG_OK;
}

void jpeg_arith_lossless_encode_diff(jpeg_arith_encoder_t * e,
    jpeg_arith_lossless_stats_t * stats, const jpeg_arith_cond_t * cond,
    uint8_t tbl, int da_cat, int db_cat, int32_t diff, int * out_cat) {
  uint8_t * area = stats->ll[tbl];
  uint8_t * st = area + jpeg_arith_lossless_context(da_cat, db_cat);

  if (diff == 0) {
    jpeg_arith_encode(e, st, 0);
    *out_cat = JPEG_LL_CAT_ZERO;
    return;
  }
  jpeg_arith_encode(e, st, 1);

  int sign = (diff < 0);
  jpeg_arith_encode(e, st + 1, sign);
  int32_t v = sign ? -diff : diff;
  uint8_t * stm = st + 2 + (size_t)sign;

  int32_t m = 0;
  v -= 1; // Sz = |V| - 1, as in F.1.4.4.1.
  if (v != 0) {
    jpeg_arith_encode(e, stm, 1);
    m = 1;
    int32_t v2 = v;
    stm = area + jpeg_arith_lossless_x1(db_cat);
    while ((v2 >>= 1) != 0) {
      jpeg_arith_encode(e, stm, 1);
      m <<= 1;
      stm += 1;
    }
  }
  jpeg_arith_encode(e, stm, 0);

  *out_cat = jpeg_arith_lossless_classify(m, sign, cond, tbl);

  stm += 14;
  while ((m >>= 1) != 0) {
    jpeg_arith_encode(e, stm, (m & v) ? 1 : 0);
  }
}

GIMG_Result jpeg_arith_lossless_restart(
    jpeg_arith_decoder_t * d, jpeg_arith_lossless_stats_t * stats) {
  size_t p = d->pos;
  if (d->marker >= 0xD0u && d->marker <= 0xD7u) {
    d->marker = 0;
  }
  else {
    while (p + 1u < d->size) {
      if (d->data[p] == 0xFFu && d->data[p + 1u] >= 0xD0u &&
          d->data[p + 1u] <= 0xD7u) {
        break;
      }
      p++;
    }
    if (p + 1u >= d->size) {
      return GIMG_ERR_CORRUPT;
    }
    d->pos = p + 2u;
    d->marker = 0;
  }
  d->c = 0;
  d->a = 0;
  d->ct = -16;
  jpeg_arith_lossless_stats_reset(stats);
  return GIMG_OK;
}

GIMG_Result jpeg_arith_restart(
    jpeg_arith_decoder_t * d, jpeg_arith_stats_t * stats) {
  size_t p = d->pos;
  if (d->marker >= 0xD0u && d->marker <= 0xD7u) {
    // Already consumed by BYTEIN; d->pos is just past it.
    d->marker = 0;
  }
  else {
    while (p + 1u < d->size) {
      if (d->data[p] == 0xFFu && d->data[p + 1u] >= 0xD0u &&
          d->data[p + 1u] <= 0xD7u) {
        break;
      }
      p++;
    }
    if (p + 1u >= d->size) {
      return GIMG_ERR_CORRUPT; // no restart marker where one was promised
    }
    d->pos = p + 2u;
    d->marker = 0;
  }
  d->c = 0;
  d->a = 0;
  d->ct = -16; // prime again, exactly as at the start of the scan
  jpeg_arith_stats_reset(stats);
  return GIMG_OK;
}

/**
 * Progressive arithmetic decoding (T.81 G.2).
 *
 * The four procedures below are the arithmetic counterparts of the four
 * Huffman ones in jpeg_block.c, and they map onto the same four cases: the DC
 * coefficient's first scan and its refinements, and the AC coefficients' first
 * scan and theirs.
 *
 * One difference is worth pointing out, because it removes a whole class of
 * bookkeeping.  The Huffman progressive coder compresses long stretches of
 * all-zero blocks into an EOB run spanning many blocks, which the decoder has
 * to carry between blocks and reset at restarts (G.1.2.3).  The arithmetic
 * coder has no EOB run at all: each block gets its own end-of-block decision,
 * and the model learns that those decisions are nearly always the same.  There
 * is no eobrun state here because there is nothing to keep.
 */

/** DC coefficient, first scan of a component (T.81 G.2, Figure G.4). */
GIMG_Result jpeg_arith_decode_block_prog_dc_first(jpeg_arith_decoder_t * d,
    jpeg_arith_stats_t * stats, const jpeg_arith_cond_t * cond, uint8_t comp,
    uint8_t dc_tbl, int al, int16_t * block) {
  int16_t tmp[64];
  memset(tmp, 0, sizeof(tmp));
  GIMG_Result r = jpeg_arith_decode_dc(d, stats, cond, comp, dc_tbl, tmp);
  if (r != GIMG_OK) {
    return r;
  }
  // G.1.1.1.2: a first scan sends the point transform of the coefficient, so
  // the value belongs at bit position Al.
  block[0] = (int16_t)GIMG_JPEG_LSHIFT(stats->dc_pred[comp], al);
  return GIMG_OK;
}

/** DC coefficient, refinement scan (T.81 G.2, Figure G.5). */
GIMG_Result jpeg_arith_decode_block_prog_dc_refine(
    jpeg_arith_decoder_t * d, jpeg_arith_stats_t * stats, int al,
    int16_t * block) {
  // G.1.2.1: a DC refinement sends one bit of the coefficient and nothing else.
  // It is coded against the fixed-probability bin, because a refinement bit
  // carries no bias worth modeling.
  if (jpeg_arith_decode(d, &stats->fixed)) {
    block[0] = (int16_t)(block[0] | GIMG_JPEG_LSHIFT(1, al));
  }
  return GIMG_OK;
}

/** AC coefficients, first scan of a band (T.81 G.2, Figure G.6). */
GIMG_Result jpeg_arith_decode_block_prog_ac_first(jpeg_arith_decoder_t * d,
    jpeg_arith_stats_t * stats, const jpeg_arith_cond_t * cond, uint8_t ac_tbl,
    int ss, int se, int al, int16_t * block) {
  uint8_t * area = stats->ac[ac_tbl];
  uint8_t kx = cond->ac_k[ac_tbl];

  for (int k = ss; k <= se; k++) {
    uint8_t * st = area + 3 * (k - 1);
    if (jpeg_arith_decode(d, st)) {
      break; // end of block
    }
    while (jpeg_arith_decode(d, st + 1) == 0) {
      st += 3;
      k++;
      if (k > se) {
        return GIMG_ERR_CORRUPT;
      }
    }
    int sign = jpeg_arith_decode(d, &stats->fixed);
    st += 2;
    int32_t m = jpeg_arith_decode(d, st);
    if (m != 0) {
      if (jpeg_arith_decode(d, st)) {
        m <<= 1;
        st = area + (k <= (int)kx ? 189 : 217);
        while (jpeg_arith_decode(d, st)) {
          m <<= 1;
          if (m == 0x8000) {
            return GIMG_ERR_CORRUPT;
          }
          st += 1;
        }
      }
    }
    int32_t v = jpeg_arith_decode_magnitude_bits(d, st + 14, m);
    if (sign) {
      v = -v;
    }
    block[k] = (int16_t)GIMG_JPEG_LSHIFT(v, al);
  }
  return GIMG_OK;
}

/** AC coefficients, refinement scan (T.81 G.2, Figure G.7). */
GIMG_Result jpeg_arith_decode_block_prog_ac_refine(jpeg_arith_decoder_t * d,
    jpeg_arith_stats_t * stats, uint8_t ac_tbl, int ss, int se, int al,
    int16_t * block) {
  uint8_t * area = stats->ac[ac_tbl];
  int32_t p1 = GIMG_JPEG_LSHIFT(1, al);  // a 1 in the bit being refined
  int32_t m1 = -GIMG_JPEG_LSHIFT(1, al); // and a -1 there

  // G.1.2.3: the end-of-block decision is only sent for positions beyond the
  // last coefficient that is already non-zero, because the coefficients before
  // it must each be refined whether or not the band ends here.
  int kex = se;
  for (; kex > 0; kex--) {
    if (block[kex] != 0) {
      break;
    }
  }

  for (int k = ss; k <= se; k++) {
    uint8_t * st = area + 3 * (k - 1);
    if (k > kex && jpeg_arith_decode(d, st)) {
      break;
    }
    for (;;) {
      if (block[k] != 0) {
        // Already non-zero: one bit says whether it grows in this pass.
        if (jpeg_arith_decode(d, st + 2)) {
          block[k] = (int16_t)(block[k] + (block[k] < 0 ? m1 : p1));
        }
        break;
      }
      if (jpeg_arith_decode(d, st + 1)) {
        // Becomes non-zero in this pass; its sign follows, at fixed odds.
        block[k] = (int16_t)(jpeg_arith_decode(d, &stats->fixed) ? m1 : p1);
        break;
      }
      st += 3;
      k++;
      if (k > se) {
        return GIMG_ERR_CORRUPT;
      }
    }
  }
  return GIMG_OK;
}

/* ------------------------------------------------------------------------
 * Encoder (T.81 D.1)
 * ------------------------------------------------------------------------ */

/**
 * Hand one finished byte to the output.
 *
 * The encoder does its own byte stuffing, because it has to: B.1.1.5 requires a
 * 0x00 after any 0xFF in the entropy-coded data, and the arithmetic coder
 * cannot know whether a 0xFF it has produced is final until it knows whether a
 * carry will reach it.  So the sink here takes bytes literally and the stuffing
 * is emitted below, unlike the Huffman writer, which can stuff as it goes.
 */
static void jpeg_arith_emit(jpeg_arith_encoder_t * e, unsigned char b) {
  e->emit(e->ctx, b);
}

/** Release the pending zero bytes that were being held back for a carry. */
static void jpeg_arith_flush_zeros(jpeg_arith_encoder_t * e) {
  while (e->zc) {
    jpeg_arith_emit(e, 0x00);
    e->zc--;
  }
}

/**
 * Deal with the byte that has just fallen out of the C register.
 *
 * A carry out of C has to be added to bytes already produced, so a byte cannot
 * be released the moment it is formed.  One byte is held in `buffer`, runs of
 * 0xFF are counted in `sc` rather than emitted (a carry would turn every one of
 * them into 0x00), and runs of 0x00 are counted in `zc` (they only need to be
 * written once something after them is settled).  This is the bookkeeping T.81
 * D.1.6 describes and that the Pennebaker and Mitchell book works through.
 */
static void jpeg_arith_byteout(jpeg_arith_encoder_t * e, int32_t temp) {
  if (temp > 0xFF) {
    // A carry: the held byte increments, and every stacked 0xFF becomes 0x00.
    if (e->buffer >= 0) {
      jpeg_arith_flush_zeros(e);
      jpeg_arith_emit(e, (unsigned char)(e->buffer + 1));
      if (e->buffer + 1 == 0xFF) {
        jpeg_arith_emit(e, 0x00); // B.1.1.5 stuffing
      }
    }
    e->zc += e->sc;
    e->sc = 0;
    // The three spacer bits in C guarantee the new byte cannot itself be 0xFF.
    e->buffer = (int)(temp & 0xFF);
  }
  else if (temp == 0xFF) {
    e->sc++; // stack it: a later carry may still turn it into 0x00
  }
  else {
    // Nothing after this can carry into the stacked bytes, so release them.
    if (e->buffer == 0) {
      e->zc++;
    }
    else if (e->buffer > 0) {
      jpeg_arith_flush_zeros(e);
      jpeg_arith_emit(e, (unsigned char)e->buffer);
    }
    if (e->sc) {
      jpeg_arith_flush_zeros(e);
      while (e->sc) {
        jpeg_arith_emit(e, 0xFF);
        jpeg_arith_emit(e, 0x00); // B.1.1.5 stuffing
        e->sc--;
      }
    }
    e->buffer = (int)(temp & 0xFF);
  }
}

/** INITENC (T.81 D.1.7, Figure D.11). */
void jpeg_arith_encoder_init(
    jpeg_arith_encoder_t * e, jpeg_arith_emit_fn emit, void * ctx) {
  memset(e, 0, sizeof(*e));
  e->emit = emit;
  e->ctx = ctx;
  e->a = 0x10000;
  e->ct = 11;
  e->buffer = -1; // nothing held yet
}

/**
 * ENCODE (T.81 D.1.3), with CODELPS (D.1.4), CODEMPS (D.1.5) and RENORME
 * (D.1.6).
 *
 * The mirror of jpeg_arith_decode: the MPS takes the lower subinterval
 * [0, A-Qe) and the LPS the upper one, and the same conditional exchange
 * applies when the LPS subinterval turns out to be the larger of the two.
 */
void jpeg_arith_encode(jpeg_arith_encoder_t * e, uint8_t * st, int val) {
  uint8_t sv = *st;
  const jpeg_arith_state_t * s = &jpeg_arith_qe[sv & 0x7Fu];
  int32_t qe = (int32_t)s->qe;

  e->a -= qe;
  if (val != (sv >> 7)) {
    // CODELPS.
    if (e->a >= qe) {
      // Conditional exchange: the LPS subinterval is the larger one.
      e->c += (uint32_t)e->a;
      e->a = qe;
    }
    *st = (uint8_t)((sv & 0x80u) | s->nlps);
    if (s->switch_mps) {
      *st ^= 0x80u;
    }
  }
  else {
    // CODEMPS.
    if (e->a >= 0x8000) {
      return; // the interval is still large enough; nothing to renormalize
    }
    if (e->a < qe) {
      e->c += (uint32_t)e->a;
      e->a = qe;
    }
    *st = (uint8_t)((sv & 0x80u) | s->nmps);
  }

  // RENORME.
  do {
    e->a = (int32_t)((uint32_t)e->a << 1);
    e->c <<= 1;
    if (--e->ct == 0) {
      jpeg_arith_byteout(e, (int32_t)(e->c >> 19));
      e->c &= 0x7FFFFu;
      e->ct += 8;
    }
  } while (e->a < 0x8000);
}

/**
 * FLUSH (T.81 D.1.8, Figure D.13) - terminate the entropy-coded segment.
 *
 * Choose the value in the remaining coding interval with the most trailing zero
 * bits, so that as few bytes as possible have to be written, then release
 * everything still held for a possible carry.  Trailing zero bytes are dropped:
 * a decoder that runs past the end supplies zeros anyway (D.2.9), which is why
 * an all-more-probable-symbol scan can end up with no bytes at all.
 */
void jpeg_arith_encoder_flush(jpeg_arith_encoder_t * e) {
  uint32_t temp = (uint32_t)(e->a - 1 + (int32_t)e->c) & 0xFFFF0000u;
  if (temp < e->c) {
    e->c = temp + 0x8000u;
  }
  else {
    e->c = temp;
  }
  e->c <<= e->ct;

  if (e->c & 0xF8000000u) {
    // One last carry to propagate.
    if (e->buffer >= 0) {
      jpeg_arith_flush_zeros(e);
      jpeg_arith_emit(e, (unsigned char)(e->buffer + 1));
      if (e->buffer + 1 == 0xFF) {
        jpeg_arith_emit(e, 0x00);
      }
    }
    e->zc += e->sc;
    e->sc = 0;
  }
  else {
    if (e->buffer == 0) {
      e->zc++;
    }
    else if (e->buffer > 0) {
      jpeg_arith_flush_zeros(e);
      jpeg_arith_emit(e, (unsigned char)e->buffer);
    }
    if (e->sc) {
      jpeg_arith_flush_zeros(e);
      while (e->sc) {
        jpeg_arith_emit(e, 0xFF);
        jpeg_arith_emit(e, 0x00);
        e->sc--;
      }
    }
  }

  // Write the final bytes, but only the ones that are not zero.
  if (e->c & 0x7FFF800u) {
    jpeg_arith_flush_zeros(e);
    unsigned char b = (unsigned char)((e->c >> 19) & 0xFFu);
    jpeg_arith_emit(e, b);
    if (b == 0xFF) {
      jpeg_arith_emit(e, 0x00);
    }
    if (e->c & 0x7F800u) {
      b = (unsigned char)((e->c >> 11) & 0xFFu);
      jpeg_arith_emit(e, b);
      if (b == 0xFF) {
        jpeg_arith_emit(e, 0x00);
      }
    }
  }
  // Whatever zero bytes are still pending are dropped on purpose.
  e->zc = 0;
}

/**
 * Encode a DC difference (T.81 F.1.4.1 and F.1.4.4.1, Figures F.4 and F.6-F.9).
 *
 * The mirror of jpeg_arith_decode_dc, including the conditioning category that
 * is carried to the next block of this component.
 */
static void jpeg_arith_encode_dc(jpeg_arith_encoder_t * e,
    jpeg_arith_stats_t * stats, const jpeg_arith_cond_t * cond, uint8_t comp,
    uint8_t dc_tbl, int coef) {
  uint8_t * area = stats->dc[dc_tbl];
  uint8_t * st = area + stats->dc_context[comp];

  int32_t v = coef - stats->dc_pred[comp];
  if (v == 0) {
    jpeg_arith_encode(e, st, 0);
    stats->dc_context[comp] = 0;
    return;
  }
  stats->dc_pred[comp] = coef;
  jpeg_arith_encode(e, st, 1);

  // Figure F.7: the sign, which also picks the group of bins used below.
  if (v > 0) {
    jpeg_arith_encode(e, st + 1, 0);
    st += 2; // Table F.4: SP = S0 + 2
    stats->dc_context[comp] = 4;
  }
  else {
    v = -v;
    jpeg_arith_encode(e, st + 1, 1);
    st += 3; // Table F.4: SN = S0 + 3
    stats->dc_context[comp] = 8;
  }

  // Figure F.8: the magnitude category, as a run of "one more bit" decisions.
  int32_t m = 0;
  v -= 1;
  if (v != 0) {
    jpeg_arith_encode(e, st, 1);
    m = 1;
    int32_t v2 = v;
    st = area + 20; // Table F.4: X1 = 20
    while ((v2 >>= 1) != 0) {
      jpeg_arith_encode(e, st, 1);
      m <<= 1;
      st += 1;
    }
  }
  jpeg_arith_encode(e, st, 0);

  // F.1.4.4.1.2: classify this difference for the next block's context.
  if (m < ((int32_t)1 << cond->dc_l[dc_tbl]) >> 1) {
    stats->dc_context[comp] = 0;
  }
  else if (m > ((int32_t)1 << cond->dc_u[dc_tbl]) >> 1) {
    stats->dc_context[comp] += 8;
  }

  // Figure F.9: the remaining magnitude bits.
  st += 14;
  while ((m >>= 1) != 0) {
    jpeg_arith_encode(e, st, (m & v) ? 1 : 0);
  }
}

/**
 * Encode the AC coefficients of a block (T.81 F.1.4.2, Figure F.5).
 *
 * @param block 64 coefficients in zigzag order.
 */
static void jpeg_arith_encode_ac(jpeg_arith_encoder_t * e,
    jpeg_arith_stats_t * stats, const jpeg_arith_cond_t * cond, uint8_t ac_tbl,
    int ss, int se, const int16_t * block) {
  uint8_t * area = stats->ac[ac_tbl];
  uint8_t kx = cond->ac_k[ac_tbl];

  // The last coefficient that is not zero; everything past it is the end of
  // block, which is sent once rather than as a run of zeros.
  int ke = se;
  for (; ke > 0; ke--) {
    if (block[ke] != 0) {
      break;
    }
  }

  int k = ss;
  for (; k <= ke; k++) {
    uint8_t * st = area + 3 * (k - 1);
    jpeg_arith_encode(e, st, 0); // not the end of the block
    int32_t temp = block[k];
    while (temp == 0) {
      jpeg_arith_encode(e, st + 1, 0); // this coefficient is zero
      st += 3;
      k++;
      temp = block[k];
    }
    jpeg_arith_encode(e, st + 1, 1);

    // F.1.4.4.2: the sign goes to the fixed-probability bin.
    if (temp < 0) {
      temp = -temp;
      jpeg_arith_encode(e, &stats->fixed, 1);
    }
    else {
      jpeg_arith_encode(e, &stats->fixed, 0);
    }
    st += 2;

    int32_t m = 0;
    temp -= 1;
    if (temp != 0) {
      jpeg_arith_encode(e, st, 1);
      m = 1;
      int32_t v = temp;
      if ((v >>= 1) != 0) {
        jpeg_arith_encode(e, st, 1);
        m <<= 1;
        // Table F.5: which magnitude chain depends on the block position.
        st = area + (k <= (int)kx ? 189 : 217);
        while ((v >>= 1) != 0) {
          jpeg_arith_encode(e, st, 1);
          m <<= 1;
          st += 1;
        }
      }
    }
    jpeg_arith_encode(e, st, 0);

    st += 14;
    while ((m >>= 1) != 0) {
      jpeg_arith_encode(e, st, (m & temp) ? 1 : 0);
    }
  }

  // The end-of-block decision, unless the last coefficient of the band was
  // itself non-zero, in which case there is nothing left to end.
  if (k <= se) {
    jpeg_arith_encode(e, area + 3 * (k - 1), 1);
  }
}

void jpeg_arith_encode_block_sequential(jpeg_arith_encoder_t * e,
    jpeg_arith_stats_t * stats, const jpeg_arith_cond_t * cond, uint8_t comp,
    uint8_t dc_tbl, uint8_t ac_tbl, int se, const int16_t * block) {
  jpeg_arith_encode_dc(e, stats, cond, comp, dc_tbl, (int)block[0]);
  jpeg_arith_encode_ac(e, stats, cond, ac_tbl, 1, se, block);
}

/**
 * Progressive arithmetic encoding (T.81 G.2).
 *
 * These need no record of what the previous scan sent: the point transform of
 * G.1.1.1.2 is a function of the coefficient and of Ah and Al alone, so "what
 * this scan has to say about coefficient k" can be answered from the
 * coefficient itself.  The Huffman progressive encoder used to carry a buffer
 * of the state after the previous scan, on the grounds that its refinement
 * symbols encode runs of newly-non-zero coefficients; that was never
 * necessary for the same reason, and the buffer is gone.
 */

/**
 * The AC point transform of T.81 G.1.1.1.2: divide the magnitude, keep the
 * sign, which truncates toward zero and for a negative value is not the same
 * as a shift of the value.
 */
static int32_t jpeg_arith_point_transform(int32_t coef, int al) {
  if (coef >= 0) {
    return coef >> al;
  }
  return -((-coef) >> al);
}

/**
 * The DC point transform of the same clause, which is a different operation:
 * an arithmetic shift, so it floors.
 *
 * The two used to be one function, and the DC first scan used the AC rule.
 * For a negative DC that sends one more than it should, and the decoder has
 * no way to take it back: it reconstructs by placing the value at bit Al and
 * OR-ing the refinement bits in above it (see
 * jpeg_arith_decode_block_prog_dc_first), which only lands on the original
 * coefficient if the low Al bits were discarded downward. Measured on a
 * 32x32 image at quality 90: a maximum error of 3 and a mean of 0.34 against
 * the same image written in one pass, where an exact match is the only right
 * answer - successive approximation reorders bits, it does not change them.
 * Small enough to read as quantization noise, which is why it survived.
 *
 * Written out rather than left as `coef >> al` because a right shift of a
 * negative int is implementation-defined in C.
 */
static int32_t jpeg_arith_dc_point_transform(int32_t coef, int al) {
  return (coef < 0) ? ~((~coef) >> al) : (coef >> al);
}

/** DC coefficient, first scan of a component (T.81 G.2, Figure G.4). */
void jpeg_arith_encode_block_prog_dc_first(jpeg_arith_encoder_t * e,
    jpeg_arith_stats_t * stats, const jpeg_arith_cond_t * cond, uint8_t comp,
    uint8_t dc_tbl, int al, const int16_t * block) {
  jpeg_arith_encode_dc(e, stats, cond, comp, dc_tbl,
      (int)jpeg_arith_dc_point_transform((int32_t)block[0], al));
}

/** DC coefficient, refinement scan (T.81 G.2, Figure G.5): one bit, at fixed
 * odds. */
void jpeg_arith_encode_block_prog_dc_refine(jpeg_arith_encoder_t * e,
    jpeg_arith_stats_t * stats, int al, const int16_t * block) {
  jpeg_arith_encode(e, &stats->fixed,
      (int)(jpeg_arith_dc_point_transform((int32_t)block[0], al) & 1));
}

/** AC coefficients, first scan of a band (T.81 G.2, Figure G.6). */
void jpeg_arith_encode_block_prog_ac_first(jpeg_arith_encoder_t * e,
    jpeg_arith_stats_t * stats, const jpeg_arith_cond_t * cond, uint8_t ac_tbl,
    int ss, int se, int al, const int16_t * block) {
  uint8_t * area = stats->ac[ac_tbl];
  uint8_t kx = cond->ac_k[ac_tbl];

  // The last coefficient this scan has anything to say about, after the point
  // transform has discarded the bits below Al.
  int ke = se;
  for (; ke > 0; ke--) {
    if (jpeg_arith_point_transform((int32_t)block[ke], al) != 0) {
      break;
    }
  }

  int k = ss;
  for (; k <= ke; k++) {
    uint8_t * st = area + 3 * (k - 1);
    jpeg_arith_encode(e, st, 0); // not the end of the band
    int32_t temp = jpeg_arith_point_transform((int32_t)block[k], al);
    while (temp == 0) {
      jpeg_arith_encode(e, st + 1, 0);
      st += 3;
      k++;
      temp = jpeg_arith_point_transform((int32_t)block[k], al);
    }
    jpeg_arith_encode(e, st + 1, 1);

    if (temp < 0) {
      temp = -temp;
      jpeg_arith_encode(e, &stats->fixed, 1);
    }
    else {
      jpeg_arith_encode(e, &stats->fixed, 0);
    }
    st += 2;

    int32_t m = 0;
    temp -= 1;
    if (temp != 0) {
      jpeg_arith_encode(e, st, 1);
      m = 1;
      int32_t v = temp;
      if ((v >>= 1) != 0) {
        jpeg_arith_encode(e, st, 1);
        m <<= 1;
        st = area + (k <= (int)kx ? 189 : 217);
        while ((v >>= 1) != 0) {
          jpeg_arith_encode(e, st, 1);
          m <<= 1;
          st += 1;
        }
      }
    }
    jpeg_arith_encode(e, st, 0);
    st += 14;
    while ((m >>= 1) != 0) {
      jpeg_arith_encode(e, st, (m & temp) ? 1 : 0);
    }
  }
  if (k <= se) {
    jpeg_arith_encode(e, area + 3 * (k - 1), 1); // end of the band
  }
}

/** AC coefficients, refinement scan (T.81 G.2, Figure G.7). */
void jpeg_arith_encode_block_prog_ac_refine(jpeg_arith_encoder_t * e,
    jpeg_arith_stats_t * stats, uint8_t ac_tbl, int ss, int se, int ah, int al,
    const int16_t * block) {
  uint8_t * area = stats->ac[ac_tbl];

  // The last coefficient this scan says anything about...
  int ke = se;
  for (; ke > 0; ke--) {
    if (jpeg_arith_point_transform((int32_t)block[ke], al) != 0) {
      break;
    }
  }
  // ...and the last one the previous scans had already made non-zero.  The
  // end-of-band decision is only sent beyond that point, because every
  // coefficient before it has to be refined whether or not the band ends here.
  int kex = ke;
  for (; kex > 0; kex--) {
    if (jpeg_arith_point_transform((int32_t)block[kex], ah) != 0) {
      break;
    }
  }

  int k = ss;
  for (; k <= ke; k++) {
    uint8_t * st = area + 3 * (k - 1);
    if (k > kex) {
      jpeg_arith_encode(e, st, 0);
    }
    for (;;) {
      int32_t temp = jpeg_arith_point_transform((int32_t)block[k], al);
      int32_t mag = temp < 0 ? -temp : temp;
      if (mag != 0) {
        if (mag >> 1) {
          // Already non-zero before this scan: send the next bit of it.
          jpeg_arith_encode(e, st + 2, (int)(mag & 1));
        }
        else {
          // Becomes non-zero in this scan; its sign follows, at fixed odds.
          jpeg_arith_encode(e, st + 1, 1);
          jpeg_arith_encode(e, &stats->fixed, temp < 0 ? 1 : 0);
        }
        break;
      }
      jpeg_arith_encode(e, st + 1, 0);
      st += 3;
      k++;
    }
  }
  if (k <= se) {
    jpeg_arith_encode(e, area + 3 * (k - 1), 1);
  }
}

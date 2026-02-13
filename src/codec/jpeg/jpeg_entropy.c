/**
 * @file
 *
 * JPEG entropy decoding (Huffman), dezigzag, dequantise, inverse DCT.
 * Used for baseline decode.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/color.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <inttypes.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/alloc_internal.h"
#include "../../core/safe_math_internal.h"
#include "../../raster/raster_internal.h"
#include "jpeg_internal.h"

// Disable clang-format for this block of code.
// clang-format off

// Zigzag order (stream index -> 8x8 row-major position). ITU-T T.81.
const uint8_t gimg_jpeg_zigzag[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

// Row-major index -> zigzag (DQT) index. DQT is stored in zigzag order.
static const uint8_t gimg_jpeg_inv_zigzag[64] = {
    0,  1,  5,  6,  14, 15, 27, 28, 2,  4,  7,  13, 16, 26, 29, 42,
    3,  8,  12, 17, 25, 30, 41, 43, 9,  11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54, 20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61, 35, 36, 48, 49, 57, 58, 62, 63,
};

// clang-format on

/** When GIMG_JPEG_TRACE_ALL=1, dump block[0..63] (zigzag order) to stderr as
 * 64 space-separated values in natural order. Prefix with label (e.g. "BLOCK_BEFORE"). */
static void jpeg_trace_all_dump_block(
    const char * label, unsigned int scan_idx, uint32_t mcu_x, uint32_t mcu_y,
    unsigned int comp_idx, size_t block_in_mcu, size_t byte_off,
    unsigned int bit_off, const int16_t * block) {
  (void)fprintf(stderr, "%s scan%u mcu=(%u,%u) comp=%u block=%zu byte_off=%zu bit_off=%u coeffs:",
      label, scan_idx, (unsigned)mcu_x, (unsigned)mcu_y, comp_idx, block_in_mcu,
      (size_t)byte_off, (unsigned)bit_off);
  for (int nat = 0; nat < 64; nat++) {
    (void)fprintf(stderr, " %d", (int)block[gimg_jpeg_inv_zigzag[nat]]);
  }
  (void)fprintf(stderr, "\n");
  (void)fflush(stderr);
}

/** Fancy chroma upsampling (triangle filter, 2h2v). ISO/IEC 10918-1 does not
 * mandate a specific upsampling method—multiple implementations are valid and
 * produce different pixel values. This implements the common triangle filter
 * to match libjpeg-turbo (jdsample.c h2v2_fancy_upsample) and Wuffs (same
 * algorithm by design). See also:
 * https://nigeltao.github.io/blog/2024/jpeg-chroma-upsampling.html
 * Output grid: x=0 and x=2*cw-1 use vertical-only (3*cur+other)/4 with bias 8/7;
 * odd x use 2x2 center (9,3,3,1)/16 (bias 7); even x in between use center
 * (bias 8). Call only when width==cw*2 and height==ch*2. */
static int jpeg_chroma_sample_fancy_2h2v(const unsigned char * buf,
    size_t stride, uint32_t cw, uint32_t ch, uint32_t x, uint32_t y) {
  uint32_t ri = y >> 1;
  uint32_t ri_other = (y & 1u) ? (ri + 1 < ch ? ri + 1 : ri) : (ri ? ri - 1 : 0);
  if (x == 0) {
    int cur = (int)buf[ri * stride + 0];
    int other = (int)buf[ri_other * stride + 0];
    return ((cur * 3 + other) * 4 + 8) >> 4;
  }
  /* Last pixel (odd x at right edge): vertical-only at input column cw-1. */
  if (x == 2u * cw - 1u) {
    int cur = (int)buf[ri * stride + (cw - 1)];
    int other = (int)buf[ri_other * stride + (cw - 1)];
    return ((cur * 3 + other) * 4 + 7) >> 4;
  }
  if (x & 1u) {
    uint32_t ci = (x - 1) >> 1;
    uint32_t ci1 = (ci + 1 < cw) ? ci + 1 : ci;
    int tl = (int)buf[ri * stride + ci];
    int tr = (int)buf[ri * stride + ci1];
    int bl = (int)buf[ri_other * stride + ci];
    int br = (int)buf[ri_other * stride + ci1];
    int thiscolsum = tl * 3 + bl;
    int nextcolsum = tr * 3 + br;
    return (thiscolsum * 3 + nextcolsum + 7) >> 4;
  }
  uint32_t ci = x >> 1;
  uint32_t cim1 = ci - 1;
  int tl = (int)buf[ri * stride + ci];
  int tr = (int)buf[ri * stride + cim1];
  int bl = (int)buf[ri_other * stride + ci];
  int br = (int)buf[ri_other * stride + cim1];
  int thiscolsum = tl * 3 + bl;
  int lastcolsum = tr * 3 + br;
  return (thiscolsum * 3 + lastcolsum + 8) >> 4;
}

/** Huffman decode table: for each code length L, min/max code and symbol base.
 */
typedef struct {
  uint16_t min_code[17]; ///< Min code value for length 1..16 (L bits).
  uint16_t max_code[17]; ///< Max code value for length 1..16.
  uint16_t
      base_index[17];  ///< Index into values for first symbol at this length.
  uint8_t values[256]; ///< Symbol values in code length order.
  int num_values;
} gimg_jpeg_huff_table_t;

/** Bitstream over scan data (MSB first; 0xFF 0x00 is data). */
typedef struct {
  const unsigned char * data;
  size_t size;
  size_t byte_off;
  int bit_off; ///< 0..7; next bit is at (data[byte_off] >> (7 - bit_off)) & 1.
  /** One bit pushback for Huffman longest-match; -1 = none. */
  int pushback;
  /** LIFO pushback stack for longest-match (read_bit returns top first). */
  unsigned char pushback_buf[16];
  unsigned int pushback_n;
  /** When set, read_bit returns 0 instead of -1 at end (encoder omitted
   * stuffing). */
  int pad_at_eob;
  /** When set (GIMG_JPEG_RECOVER_STUFF_ZERO=1), at end of segment stuff zero
   * bits and continue, matching libjpeg's JWRN_HIT_MARKER recovery. For
   * debugging only. */
  int recover_stuff_zero;
  /** One-time warning for recover_stuff_zero. */
  int stuffed_any;
} gimg_jpeg_bitstream_t;

static void jpeg_bitstream_init(
    gimg_jpeg_bitstream_t * bs, const unsigned char * data, size_t size) {
  bs->data = data;
  bs->size = size;
  bs->byte_off = 0;
  bs->bit_off = 0;
  bs->pushback = -1;
  bs->pushback_n = 0;
  bs->pad_at_eob = 0;
  bs->recover_stuff_zero = 0;
  bs->stuffed_any = 0;
}

/** Return 1 if marker m has no length/payload (SOI, EOI, RST0..RST7). */
static int jpeg_marker_no_length(unsigned char m) {
  if (m == GIMG_JPEG_MARKER_SOI || m == GIMG_JPEG_MARKER_EOI) {
    return 1;
  }
  if (m >= 0xD0 && m <= 0xD7) {
    return 1;  // RST0..RST7
  }
  return 0;
}

/** Skip marker at current position; current byte must be 0xFF. Skips 0xFF and
 * the marker byte; for markers with length, skips length bytes too. Return 1
 * if we skipped, 0 if next byte is 0x00 or 0xFF (entropy data, do not skip). */
static int jpeg_bitstream_skip_marker_at_ff(gimg_jpeg_bitstream_t * bs) {
  if (bs->byte_off >= bs->size || bs->data[bs->byte_off] != 0xFF) {
    return 0;
  }
  if (bs->byte_off + 1 >= bs->size) {
    return 0;
  }
  unsigned char m = bs->data[bs->byte_off + 1];
  if (m == 0x00 || m == 0xFF) {
    // Byte stuffing or entropy data; do not skip.
    return 0;
  }
  bs->byte_off += 2;  // skip 0xFF and marker byte
  if (jpeg_marker_no_length(m)) {
    return 1;
  }
  // Marker with 2-byte length (DHT, DQT, SOS, etc.). Skip only if the full
  // segment is in the buffer; otherwise leave position unchanged and do not
  // skip (caller will treat 0xFF as data — buffer should not be truncated).
  if (bs->byte_off + 2 > bs->size) {
    bs->byte_off -= 2;  // undo skip
    return 0;
  }
  uint16_t seg_len =
      (uint16_t)((bs->data[bs->byte_off] << 8) | bs->data[bs->byte_off + 1]);
  if (seg_len < 2 || bs->byte_off + (size_t)seg_len > bs->size) {
    bs->byte_off -= 2;
    return 0;
  }
  bs->byte_off += (size_t)seg_len;  // skip length bytes and payload
  return 1;
}

/** Skip marker segments and stuffing after 0xFF (T.81 B.2.2). Call only when
 * byte_off was just advanced past a 0xFF byte (data[byte_off - 1] == 0xFF).
 * Skip 0x00 (stuffing) or RST/marker bytes; do not skip entropy data. We do
 * not consume bytes beyond the current block/phase — each scan has its own
 * data buffer; within a block we only advance by bits we read. */
static void jpeg_bitstream_skip_after_ff(gimg_jpeg_bitstream_t * bs) {
  const char * trace = getenv("GIMG_JPEG_DEBUG_SKIP_FF");
  while (bs->byte_off < bs->size) {
    unsigned char m = bs->data[bs->byte_off];
    if (m == 0x00) {
      // Byte stuffing: 0xFF 0x00 is data 0xFF; skip the 0x00.
      if (trace && trace[0] == '1') {
        (void)fprintf(stderr, "SKIP_FF skipping stuffing 0x00 at byte_off=%zu -> %zu\n",
            (size_t)bs->byte_off, (size_t)(bs->byte_off + 1));
        (void)fflush(stderr);
      }
      bs->byte_off++;
      // Only a single 0x00 after 0xFF is stuffing. If another 0x00 follows,
      // it is real entropy data and must not be skipped again (T.81 B.2.2).
      break;
    }
    if (m == 0xFF) {
      // Entropy data; do not skip.
      break;
    }
    if (m >= 0xD0 && m <= 0xD7) {
      // RST: skip the marker byte (we already passed the 0xFF).
      bs->byte_off++;
      continue;
    }
    // Marker with length (DHT, DQT, SOS, etc.): skip segment. Need 3 bytes
    // (one marker byte we skip + 2 length bytes) so after ++ we can read two.
    if (bs->byte_off + 3 > bs->size) {
      break;
    }
    bs->byte_off++;
    uint16_t seg_len =
        (uint16_t)((bs->data[bs->byte_off] << 8) | bs->data[bs->byte_off + 1]);
    bs->byte_off += 2;
    if (seg_len >= 2 && bs->byte_off + (size_t)(seg_len - 2) <= bs->size) {
      bs->byte_off += (size_t)(seg_len - 2);
    }
    else {
      break;
    }
  }
}

/** Read one bit; return 0 or 1, or -1 on underflow. */
static int jpeg_bitstream_read_bit(gimg_jpeg_bitstream_t * bs) {
  if (bs->pushback_n > 0) {
    return (int)bs->pushback_buf[--bs->pushback_n];
  }
  if (bs->pushback >= 0) {
    int b = bs->pushback;
    bs->pushback = -1;
    return b;
  }
  // When at 0xFF, skip RST markers (0xFF 0xD0..0xD7) so we never feed them
  // as entropy data. Skip other markers (DHT etc.) so 0xFF starting a
  // marker is not fed as data.
  while (bs->byte_off < bs->size && bs->data[bs->byte_off] == 0xFF) {
    if (jpeg_bitstream_skip_marker_at_ff(bs)) {
      continue;
    }
    break;
  }
  if (bs->byte_off >= bs->size) {
    if (bs->recover_stuff_zero) {
      if (!bs->stuffed_any) {
        (void)fprintf(stderr,
            "Corrupt JPEG data: premature end of data segment (stuffing zero bits)\n");
        (void)fflush(stderr);
        bs->stuffed_any = 1;
      }
      return 0;  // Match libjpeg JWRN_HIT_MARKER recovery for debugging.
    }
    if (bs->pad_at_eob) {
      return 0;  // Spec: encoder should emit byte stuffing; some omit it.
    }
    return -1;
  }
  unsigned char b = bs->data[bs->byte_off];
  int bit = (b >> (7 - bs->bit_off)) & 1;
  bs->bit_off++;
  if (bs->bit_off == 8) {
    bs->bit_off = 0;
    bs->byte_off++;
    if (bs->byte_off > 0 && bs->byte_off <= bs->size &&
        bs->data[bs->byte_off - 1] == 0xFF) {
      jpeg_bitstream_skip_after_ff(bs);
    }
  }
  return bit;
}

/** Read n bits (1..16), MSB first. Return value or -1 on underflow. */
static int jpeg_bitstream_read_bits(gimg_jpeg_bitstream_t * bs, int n) {
  int v = 0;
  for (int i = 0; i < n; i++) {
    int b = jpeg_bitstream_read_bit(bs);
    if (b < 0) {
      return -1;
    }
    v = (v << 1) | b;
  }
  return v;
}

/** Build Huffman decode table from DHT payload (TcTh byte + 16 counts +
 * symbols). */
static int jpeg_build_huff_table(
    const unsigned char * dht, size_t dht_len, gimg_jpeg_huff_table_t * tbl) {
  if (dht_len < 17) {
    return -1;
  }
  const unsigned char * bits = dht + 1;
  size_t num_syms = 0;
  for (int i = 0; i < 16; i++) {
    num_syms += bits[i];
  }
  if (dht_len < 17 + num_syms) {
    return -1;
  }
  const unsigned char * vals = dht + 17;
  tbl->num_values = (int)num_syms;

  // Canonical code assignment.
  uint32_t code = 0;
  uint16_t base = 0;
  for (int len = 1; len <= 16; len++) {
    uint8_t count = bits[len - 1];
    tbl->min_code[len] = (uint16_t)code;
    if (count > 0) {
      tbl->max_code[len] = (uint16_t)(code + count - 1);
      tbl->base_index[len] = base;
      for (int k = 0; k < count; k++) {
        tbl->values[base + k] = vals[base + k];
      }
      base += count;
    }
    else {
      tbl->max_code[len] = 0;
      tbl->min_code[len] = 1;
      tbl->base_index[len] = 0;
    }
    code = (code + count) << 1;
  }
  return 0;
}

/** Default AC refinement DHT payload (T.81 Table K.6): Tc=1 Th=0 + 16 bits + 17 vals.
 * Used when scan is AC refinement (Ah!=0) but no 17-symbol DHT is in the file. */
static const unsigned char jpeg_default_ac_refine_dht[1 + 16 + 17] = {
    0x10,
    0, 0, 1, 0, 2, 14, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0x00, 0x01, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70,
    0x80, 0x90, 0xa0, 0xb0, 0xc0, 0xd0, 0xe0, 0xf0,
};

/** Default AC table (T.81 K.4) for first AC-initial scan when no DHT before
 * first SOS. Implemented after jpeg_std_vals_ac_lum. */
static const unsigned char * jpeg_default_ac_dht_payload(size_t * out_len);

/** Pillow/libjpeg compat: build AC table for first AC-initial scan when DHT
 * was between scan 0 and 1. Encoder uses 00=(0,4), 10=EOB (non-canonical; canonical
 * would give 00, 01). Fills tbl so decode accepts code 0 and 2 at length 2. */
static void jpeg_build_pillow_compat_ac_scan1_table(gimg_jpeg_huff_table_t * tbl);

/** Decode one symbol using the given Huffman table. Return symbol or -1.
 * When ac_prefer_eob is 1 and we match (0,1) or (0,2) at 3 bits (code 010/011),
 * we read one more bit: if 0 we have 0100/0100 and return EOB (0); if 1 we
 * push back the bit and return the 3-bit symbol. This allows our encoder’s
 * 4-bit EOB (0100) to decode correctly while still decoding external 3-bit
 * (0,1) (010) correctly. */
static int jpeg_huff_decode(
    gimg_jpeg_bitstream_t * bs, const gimg_jpeg_huff_table_t * tbl,
    int ac_prefer_eob) {
  uint16_t code = 0;
  const char * trace_dc = getenv("GIMG_JPEG_TRACE_DC");
  const char * trace_bits_env = getenv("GIMG_JPEG_TRACE_HUFF_BITS");
  const char * trace_all_env = getenv("GIMG_JPEG_TRACE_ALL");
  const int trace_bits = (trace_bits_env && trace_bits_env[0] == '1') ||
      (trace_all_env && trace_all_env[0] == '1');
  for (int len = 1; len <= 16; len++) {
    size_t pre_byte = bs->byte_off;
    int pre_bit = bs->bit_off;
    int b = jpeg_bitstream_read_bit(bs);
    if (b < 0) {
      if (bs->pad_at_eob) {
        while (bs->byte_off < bs->size) {
          (void)jpeg_bitstream_read_bit(bs);
        }
      }
      return -1;
    }
    code = (code << 1) | (uint16_t)b;
    if (trace_bits) {
      (void)fprintf(stderr, "HUFF_BIT len=%d bit=%d code=0x%x byte_off=%zu bit_off=%u\n",
          len, b, (unsigned)code, (size_t)bs->byte_off, (unsigned)bs->bit_off);
      (void)fflush(stderr);
    }
    if (trace_dc && !ac_prefer_eob) {
      (void)fprintf(stderr,
          "TRACE_DC len=%d bit=%d code=0x%x pre=(byte=%zu,bit=%d) "
          "post=(byte=%zu,bit=%d)\n",
          len, b, (unsigned)code,
          (size_t)pre_byte, pre_bit,
          (size_t)bs->byte_off, bs->bit_off);
      (void)fflush(stderr);
    }
    if (code >= tbl->min_code[len] && code <= tbl->max_code[len]) {
      uint16_t idx = tbl->base_index[len] + (code - tbl->min_code[len]);
      int sym = (int)tbl->values[idx];
      if (trace_bits) {
        (void)fprintf(stderr, "HUFF_MATCH len=%d code=0x%x sym=0x%02x byte_off=%zu bit_off=%u\n",
            len, (unsigned)code, (unsigned)sym, (size_t)bs->byte_off, (unsigned)bs->bit_off);
        (void)fflush(stderr);
      }
      if (ac_prefer_eob && len == 3 && (sym == 0x01 || sym == 0x02) &&
          tbl->max_code[4] != 0) {
        int next_b = jpeg_bitstream_read_bit(bs);
        if (next_b == 0) {
          code = (code << 1) | 0;
          if (code >= tbl->min_code[4] && code <= tbl->max_code[4]) {
            uint16_t idx4 =
                tbl->base_index[4] + (uint16_t)(code - tbl->min_code[4]);
            if (tbl->values[idx4] == 0) {
              return 0;
            }
          }
        }
        if (next_b >= 0 && bs->pushback_n < 16) {
          bs->pushback_buf[bs->pushback_n++] = (unsigned char)next_b;
        }
      }
      /* Longest match for AC: read up to 16-len more bits and take the
       * longest valid code so we don't return (0,0) EOB when the stream has
       * (r,0) EOBRUN. Use when ac_prefer_eob is 0; set GIMG_JPEG_AC_LONGEST_MATCH=0
       * to disable. */
      {
        const char * ac_longest = getenv("GIMG_JPEG_AC_LONGEST_MATCH");
        int do_longest = (!ac_longest || ac_longest[0] != '0') && !ac_prefer_eob;
        if (do_longest && len < 16) {
          int peek_bits[16];
          int n_read = 0;
          int best_len = len;
          int best_sym = sym;
          uint16_t code2 = code;
          while (len + n_read < 16) {
            int b = jpeg_bitstream_read_bit(bs);
            if (b < 0) {
              break;
            }
            peek_bits[n_read++] = b;
            code2 = code;
            for (int i = 0; i < n_read; i++) {
              code2 = (code2 << 1) | (uint16_t)peek_bits[i];
            }
            {
              int len2 = len + n_read;
              if (code2 >= tbl->min_code[len2] &&
                  code2 <= tbl->max_code[len2]) {
                best_len = len2;
                best_sym = (int)tbl->values[tbl->base_index[len2] +
                    (code2 - tbl->min_code[len2])];
              }
            }
          }
          /* Push back bits not consumed by the longest match. */
          {
            int used = best_len - len;
            for (int i = n_read - 1; i >= used && bs->pushback_n < 16; i--) {
              bs->pushback_buf[bs->pushback_n++] = (unsigned char)peek_bits[i];
            }
          }
          return best_sym;
        }
      }
      return sym;
    }
  }
  if (bs->pad_at_eob) {
    while (bs->byte_off < bs->size) {
      (void)jpeg_bitstream_read_bit(bs);
    }
  }
  if (bs->recover_stuff_zero) {
    return 0;
  }
  return -1;
}

/** Extend sign for n-bit value. Per T.81 Annex F (Figure F.12, Table K.2):
 * if V < 2^(n-1) then negative: V - (2^n - 1); else V unchanged.
 * So 0..(2^(n-1)-1) map to -(2^n-1)..-1; 2^(n-1)..(2^n-1) stay positive.
 * IJG/libjpeg use the same convention (Table K.2). */
static int16_t jpeg_extend(int val, int n) {
  if (n == 0) {
    return 0;
  }
  int half = 1 << (n - 1);
  if (val < half) {
    return (int16_t)(val - ((1 << n) - 1));
  }
  return (int16_t)val;
}

/** Decode one 8x8 block (DC + AC). Block is 64 int16_t in zigzag order.
 * T.81 Annex F: DC diff then AC run/size (or EOB). */
static GIMG_Result jpeg_decode_block(gimg_jpeg_bitstream_t * bs,
    const gimg_jpeg_huff_table_t * dc_tbl,
    const gimg_jpeg_huff_table_t * ac_tbl, int16_t * block,
    int16_t * dc_predictor) {
  memset(block, 0, 64 * sizeof(int16_t));
  int sym = jpeg_huff_decode(bs, dc_tbl, 0);
  if (sym < 0) {
    if (getenv("GIMG_JPEG_DEBUG_BASELINE_FAIL")) {
      (void)fprintf(stderr, "BASELINE_FAIL_STEP dc_sym byte_off=%zu bit_off=%u\n",
          (size_t)bs->byte_off, (unsigned)bs->bit_off);
      (void)fflush(stderr);
    }
    return GIMG_ERR_CORRUPT;
  }
  int nbits = sym;
  int diff = 0;
  if (nbits > 0) {
    diff = jpeg_bitstream_read_bits(bs, nbits);
    if (diff < 0) {
      if (getenv("GIMG_JPEG_DEBUG_BASELINE_FAIL")) {
        (void)fprintf(stderr, "BASELINE_FAIL_STEP dc_extra nbits=%d byte_off=%zu\n",
            nbits, (size_t)bs->byte_off);
        (void)fflush(stderr);
      }
      return GIMG_ERR_CORRUPT;
    }
    diff = jpeg_extend(diff, nbits);
  }
  *dc_predictor += diff;
  block[0] = *dc_predictor;

  for (int k = 1; k < 64; k++) {
    sym = jpeg_huff_decode(bs, ac_tbl, 1);
    if (sym < 0) {
      if (getenv("GIMG_JPEG_DEBUG_BASELINE_FAIL")) {
        (void)fprintf(stderr, "BASELINE_FAIL_STEP ac_sym k=%d byte_off=%zu bit_off=%u\n",
            k, (size_t)bs->byte_off, (unsigned)bs->bit_off);
        (void)fflush(stderr);
      }
      return GIMG_ERR_CORRUPT;
    }
    if (sym == 0) {
      // EOB: rest are zero (stream order).
      for (; k < 64; k++) {
        block[k] = 0;
      }
      break;
    }
    int run = sym >> 4;
    int size = sym & 0x0F;
    k += run;
    if (k >= 64) {
      return GIMG_ERR_CORRUPT;
    }
    int ac = 0;
    if (size > 0) {
      ac = jpeg_bitstream_read_bits(bs, size);
      if (ac < 0) {
        if (getenv("GIMG_JPEG_DEBUG_BASELINE_FAIL")) {
          (void)fprintf(stderr, "BASELINE_FAIL_STEP ac_extra size=%d byte_off=%zu\n",
              size, (size_t)bs->byte_off);
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
 *  low Al bits zero; we store (diff << al) so refinement scans can add low bits.
 *  If out_sym and out_diff are non-NULL, they receive the decoded symbol and
 * diff (for TRACE_JPEG_DC_SYMBOLS). When trace_all, log every step (position, sym, bits, computation). */
static GIMG_Result jpeg_decode_block_progressive_dc(gimg_jpeg_bitstream_t * bs,
    const gimg_jpeg_huff_table_t * dc_tbl, int16_t * block,
    int16_t * dc_predictor, int al, int * out_sym, int * out_diff, int trace_all) {
  if (trace_all) {
    (void)fprintf(stderr, "DC_HUFF_ENTER byte_off=%zu bit_off=%u\n",
        (size_t)bs->byte_off, (unsigned)bs->bit_off);
    (void)fflush(stderr);
  }
  int sym = jpeg_huff_decode(bs, dc_tbl, 0);
  if (sym < 0) {
    return GIMG_ERR_CORRUPT;
  }
  int nbits = sym;
  int diff = 0;
  if (trace_all) {
    (void)fprintf(stderr, "DC_HUFF_EXIT sym=%d nbits=%d byte_off=%zu bit_off=%u\n",
        sym, nbits, (size_t)bs->byte_off, (unsigned)bs->bit_off);
    (void)fflush(stderr);
  }
  if (nbits > 0) {
    diff = jpeg_bitstream_read_bits(bs, nbits);
    if (diff < 0) {
      return GIMG_ERR_CORRUPT;
    }
    if (trace_all) {
      (void)fprintf(stderr, "DC_EXTRA_BITS nbits=%d raw=%d byte_off=%zu bit_off=%u\n",
          nbits, diff, (size_t)bs->byte_off, (unsigned)bs->bit_off);
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
  *dc_predictor += (diff << al);
  block[0] = *dc_predictor;
  if (trace_all) {
    (void)fprintf(stderr, "DC_VALUE predictor=%d block[0]=%d (diff<<al=%d) byte_off=%zu bit_off=%u\n",
        (int)*dc_predictor, (int)block[0], diff << al, (size_t)bs->byte_off, (unsigned)bs->bit_off);
    (void)fflush(stderr);
  }
  return GIMG_OK;
}

/** Progressive DC refinement (Ss=0, Se=0, Ah>0): one bit per block.
 *  T.81 Annex G.1.1.2.1: refinement scan codes "the least significant bit of the
 *  point transformed DC coefficients" — the bit at position Al in the scan header.
 *  Decoder places that bit into the coefficient: block[0] |= (bit << Al).
 *  (Al is 0-based in the scan; for first refinement pass Ah=1, Al=0 we set bit 0.)
 *  When trace_all, log DC_REFINE_ENTER, DC_REFINE_BIT, DC_REFINE_VALUE. */
static GIMG_Result jpeg_decode_block_progressive_dc_refine(
    gimg_jpeg_bitstream_t * bs, int16_t * block, int16_t * dc_predictor,
    unsigned int al, int * out_bit, int trace_all) {
  if (trace_all) {
    (void)fprintf(stderr, "DC_REFINE_ENTER byte_off=%zu bit_off=%u block[0]=%d al=%u\n",
        (size_t)bs->byte_off, (unsigned)bs->bit_off, (int)block[0], (unsigned)al);
    (void)fflush(stderr);
  }
  int b = jpeg_bitstream_read_bit(bs);
  if (b < 0) {
    return GIMG_ERR_CORRUPT;
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
    (void)fprintf(stderr, "DC_REFINE_VALUE block[0]=%d byte_off=%zu bit_off=%u\n",
        (int)block[0], (size_t)bs->byte_off, (unsigned)bs->bit_off);
    (void)fflush(stderr);
  }
  return GIMG_OK;
}

/** Progressive AC initial (Ah=0): decode band [ss, se], store (value << al).
 *  T.81 Annex G: (0,0)=EOB; (15,0)=ZRL (16 zero coeffs); (r,0) r=1..14 = EOB with
 *  run length EOBRUN = 2^r + next r bits (then skip EOBRUN-1 following blocks).
 *  If out_eobrun is non-NULL we implement EOBRUN; else (r,0) r!=15 is error.
 *  If do_trace, emit TRACE_JPEG_AC_SYMBOLS [block=N] run=X size=Y val=Z or EOB.
 *  trace_block_id: when >= 0 and do_trace, prefix lines with "block=N ".
 *  trace_scan_idx: when do_trace and TRACE_AC_COMPARE=1, emit canonical
 *  "AC_INITIAL scan=N block=B ..." for compare_progressive_trace.py.
 *  When trace_all, log every step: AC_INITIAL_HUFF_ENTER/EXIT, EOB/ZRL/EOBRUN/COEFF. */
static GIMG_Result jpeg_decode_block_progressive_ac_initial(
    gimg_jpeg_bitstream_t * bs, const gimg_jpeg_huff_table_t * ac_tbl,
    int16_t * block, int ss, int se, int al, int do_trace, int trace_block_id,
    unsigned int trace_scan_idx, unsigned int * out_eobrun, int trace_all) {
  int k = ss;
  /* T.81 Annex G: at start of block, if EOBRUN > 0 this block is all-zero in band. */
  if (out_eobrun && *out_eobrun > 0) {
    if (trace_all) {
      (void)fprintf(stderr, "AC_INITIAL_EOBRUN_SKIP block=%d eobrun=%u (block all-zero) byte_off=%zu bit_off=%u\n",
          trace_block_id, *out_eobrun, (size_t)bs->byte_off, (unsigned)bs->bit_off);
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
      (void)fprintf(stderr, "AC_INITIAL_HUFF_ENTER block=%d k=%d byte_off=%zu bit_off=%u\n",
          trace_block_id, k, (size_t)bs->byte_off, (unsigned)bs->bit_off);
      (void)fflush(stderr);
    }
    /* Use ac_prefer_eob=0: external fixtures (e.g. Pillow) may use different
     * Huffman code for EOB; prefer-eob can mis-decode (0,1)/(0,2) as EOB. */
    int sym = jpeg_huff_decode(bs, ac_tbl, 0);
    if (sym < 0) {
      return GIMG_ERR_CORRUPT;
    }
    int run = sym >> 4;
    int size = sym & 0x0F;
    if (trace_all) {
      (void)fprintf(stderr, "AC_INITIAL_HUFF_EXIT block=%d sym=0x%02x run=%d size=%d byte_off=%zu bit_off=%u\n",
          trace_block_id, (unsigned)sym, run, size, (size_t)bs->byte_off, (unsigned)bs->bit_off);
      (void)fflush(stderr);
    }
    if (sym == 0) {
      if (trace_all) {
        (void)fprintf(stderr, "AC_INITIAL_EOB block=%d byte_off=%zu bit_off=%u\n",
            trace_block_id, (size_t)bs->byte_off, (unsigned)bs->bit_off);
        (void)fflush(stderr);
      }
      if (do_trace) {
        if (trace_block_id >= 0)
          (void)fprintf(stderr, "TRACE_JPEG_AC_SYMBOLS block=%d EOB\n",
              trace_block_id);
        else
          (void)fprintf(stderr, "TRACE_JPEG_AC_SYMBOLS EOB\n");
        if (getenv("TRACE_AC_COMPARE") && getenv("TRACE_AC_COMPARE")[0] == '1' &&
            trace_block_id >= 0) {
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
    /* T.81 Annex G: (0,0)=EOB; (15,0)=ZRL; (r,0) r=1..14 = EOB with run 2^r + r bits. */
    if (size == 0) {
      if (run != 15) {
        if (out_eobrun && run >= 1 && run <= 14) {
          /* EOB run: EOBRUN = 2^r + next r bits (T.81 Annex G). */
          unsigned int eobrun = 1u << (unsigned)run;
          if (run > 0) {
            int rbits = jpeg_bitstream_read_bits(bs, run);
            if (rbits < 0) {
              return GIMG_ERR_CORRUPT;
            }
            eobrun += (unsigned int)rbits;
            if (trace_all) {
              (void)fprintf(stderr, "AC_INITIAL_EOBRUN_BITS block=%d run=%d rbits=%d eobrun=%u byte_off=%zu bit_off=%u\n",
                  trace_block_id, run, rbits, eobrun, (size_t)bs->byte_off, (unsigned)bs->bit_off);
              (void)fflush(stderr);
            }
          } else if (trace_all) {
            (void)fprintf(stderr, "AC_INITIAL_EOBRUN block=%d eobrun=%u byte_off=%zu bit_off=%u\n",
                trace_block_id, eobrun, (size_t)bs->byte_off, (unsigned)bs->bit_off);
            (void)fflush(stderr);
          }
          *out_eobrun = eobrun;
          for (; k <= se; k++) {
            block[k] = 0;
          }
          break;
        }
        return GIMG_ERR_CORRUPT;  /* (r,0) r!=15 without EOBRUN support. */
      }
      /* ZRL: 16 zero coefficients (T.81 Annex G). */
      if (trace_all) {
        (void)fprintf(stderr, "AC_INITIAL_ZRL block=%d k=%d (skip 16 zeros) byte_off=%zu bit_off=%u\n",
            trace_block_id, k, (size_t)bs->byte_off, (unsigned)bs->bit_off);
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
        return GIMG_ERR_CORRUPT;
      }
      break;
    }
    int ac = 0;
    if (size > 0) {
      ac = jpeg_bitstream_read_bits(bs, size);
      if (ac < 0) {
        return GIMG_ERR_CORRUPT;
      }
      if (trace_all) {
        (void)fprintf(stderr, "AC_INITIAL_EXTRA_BITS block=%d size=%d raw=%d byte_off=%zu bit_off=%u\n",
            trace_block_id, size, ac, (size_t)bs->byte_off, (unsigned)bs->bit_off);
        (void)fflush(stderr);
      }
      ac = jpeg_extend(ac, size);
    }
    block[k] = (int16_t)(ac << al);
    if (trace_all) {
      (void)fprintf(stderr, "AC_INITIAL_COEFF block=%d k=%d val=%d (ac<<al) byte_off=%zu bit_off=%u\n",
          trace_block_id, k, ac << al, (size_t)bs->byte_off, (unsigned)bs->bit_off);
      (void)fflush(stderr);
    }
    if (do_trace) {
      if (trace_block_id >= 0)
        (void)fprintf(stderr, "TRACE_JPEG_AC_SYMBOLS block=%d run=%d size=%d val=%d k=%d\n",
            trace_block_id, run, size, ac << al, k);
      else
        (void)fprintf(stderr, "TRACE_JPEG_AC_SYMBOLS run=%d size=%d val=%d k=%d\n",
            run, size, ac << al, k);
      if (getenv("TRACE_AC_COMPARE") && getenv("TRACE_AC_COMPARE")[0] == '1' &&
          trace_block_id >= 0) {
        (void)fprintf(stderr, "AC_INITIAL scan=%u block=%d run=%d size=%d val=%d k=%d\n",
            trace_scan_idx, trace_block_id, run, size, ac << al, k);
        (void)fflush(stderr);
      }
      (void)fflush(stderr);
    }
    k++;
  }
  return GIMG_OK;
}

/** Progressive AC refinement (Ah!=0): interleaved [run code][refinement bit][correction bits].
 *
 *  T.81 Annex G, Figure G.7: for each (run, size=1) decode run-length symbol,
 *  read one refinement bit for the newly-nonzero coefficient, then advance
 *  (skip run zeros; for each already-nonzero read one correction bit). Bitstream:
 *  [run code][refinement bit for new][correction bits for already-nz in run].
 *
 *  Symbol (run<<4)|size: size=0 for EOB or ZRL (run=15 = 16 zeros); size=1 for
 *  one refinement bit. T.81 Annex G: newly nonzero — bit is sign (1 = +, 0 = −),
 *  magnitude at Al is 1. Already-nonzero: correction bit 1 = set bit at Al
 *  (increase magnitude); 0 = leave unchanged. Bit position is Al (0-based).
 *  When do_trace and trace_block_id >= 0, prefix lines with "block=N " for ref compare.
 *  When trace_all is 1, log every decoding step (position, symbol, bits read, computations).
 */
static GIMG_Result jpeg_decode_block_progressive_ac_refine(
    gimg_jpeg_bitstream_t * bs, const gimg_jpeg_huff_table_t * ac_tbl,
    int16_t * block, int ss, int se, int al, int do_trace, int trace_block_id,
    int log_sanity, int trace_all) {
  /* T.81: AC band is [Ss, Se] with 1 <= Ss <= Se <= 63. Clamp to prevent overrun. */
  if (se > 63) {
    se = 63;
  }
  if (ss < 1) {
    ss = 1;
  }
  int bitpos = (al >= 0 && al <= 15) ? al : 0;
  int k = ss;
  while (k <= se) {
    int k_at_iter_start = k;
    /* Break-the-circle: log start of first two blocks (position + nz count). */
    if ((trace_block_id >= 0 && trace_block_id < 2 && k == ss) || trace_all) {
      int nz_count = 0;
      for (int i = ss; i <= se; i++) {
        if (block[i] != 0) nz_count++;
      }
      (void)fprintf(stderr,
          "AC_REFINE_BLOCK_START block=%d byte_off=%zu bit_off=%u nz_in_band=%d\n",
          trace_block_id, (size_t)bs->byte_off, (unsigned)bs->bit_off, nz_count);
      (void)fflush(stderr);
    }
    if (trace_all) {
      (void)fprintf(stderr, "AC_REFINE_HUFF_ENTER block=%d k=%d byte_off=%zu bit_off=%u\n",
          trace_block_id, k, (size_t)bs->byte_off, (unsigned)bs->bit_off);
      (void)fflush(stderr);
    }
    /* ac_prefer_eob=1 to disable longest-match: refinement uses 2-symbol table
     * (EOB + (r,s)); longest-match would read 15 extra bits and push back,
     * so refinement/correction bits would be read in wrong order and we'd
     * consume the whole scan in block 0. */
    int sym = jpeg_huff_decode(bs, ac_tbl, 1);
    if (sym < 0) {
      if (bs->recover_stuff_zero) {
        break;  /* Treat underflow as EOB. */
      }
      /* T.81 does not define "insufficient data" mid-scan; libjpeg treats it as
       * suspend and leaves remaining blocks unchanged. When we underflow at the
       * very start of a block (k == ss), no coefficients have been updated yet,
       * so treat as EOB for this block and succeed (match libjpeg behavior). */
      if (k == ss) {
        if (getenv("GIMG_JPEG_PROGRESSIVE_DEBUG") && getenv("GIMG_JPEG_PROGRESSIVE_DEBUG")[0] == '1')
          (void)fprintf(stderr, " ac_refine insufficient_data at block start k=%d byte_off=%zu (treat as EOB)\n",
              k, (size_t)bs->byte_off);
        break;
      }
      if (getenv("GIMG_JPEG_PROGRESSIVE_DEBUG") && getenv("GIMG_JPEG_PROGRESSIVE_DEBUG")[0] == '1')
        (void)fprintf(stderr, " ac_refine underflow: huff_decode at k=%d byte_off=%zu\n",
            k, (size_t)bs->byte_off);
      return GIMG_ERR_CORRUPT;
    }
    if (sym == 0) {
      if (trace_all) {
        (void)fprintf(stderr, "AC_REFINE_HUFF_EXIT block=%d sym=EOB(0) byte_off=%zu bit_off=%u\n",
            trace_block_id, (size_t)bs->byte_off, (unsigned)bs->bit_off);
        (void)fflush(stderr);
      }
      /* T.81 Annex G.1.2.2 (ISO/IEC 10918-1): after decoding EOB, the decoder
       * must still read one correction bit for each already-nonzero coefficient
       * from the current position k to the end of the band [Ss,Se]. The bitstream
       * order is: EOB code, then correction bits for remaining nonzero coeffs.
       * libjpeg implements this in its EOB run block (for (; k <= Se; k++) ...).
       * Without this we would be one bit short and the next block would mis-decode. */
      for (; k <= se; k++) {
        if (block[k] != 0) {
          int rbit = jpeg_bitstream_read_bit(bs);
          if (rbit < 0) {
            if (bs->recover_stuff_zero) {
              rbit = 0;
            } else {
              if (getenv("GIMG_JPEG_PROGRESSIVE_DEBUG") && getenv("GIMG_JPEG_PROGRESSIVE_DEBUG")[0] == '1')
                (void)fprintf(stderr, " ac_refine underflow: correction_bit at EOB k=%d byte_off=%zu\n",
                    k, (size_t)bs->byte_off);
              return GIMG_ERR_CORRUPT;
            }
          }
          int16_t old_val = block[k];
          if (rbit & 1) {
            int16_t delta = (int16_t)(block[k] >= 0 ? (1 << bitpos) : -(1 << bitpos));
            block[k] += delta;
            if (trace_all) {
              (void)fprintf(stderr, "AC_REFINE_CORRECTION_BIT block=%d k=%d bit=%d delta=%d old=%d new=%d byte_off=%zu bit_off=%u (after EOB)\n",
                  trace_block_id, k, rbit & 1, (int)delta, (int)old_val, (int)block[k],
                  (size_t)bs->byte_off, (unsigned)bs->bit_off);
              (void)fflush(stderr);
            }
          } else if (trace_all) {
            (void)fprintf(stderr, "AC_REFINE_CORRECTION_BIT block=%d k=%d bit=%d (no change) byte_off=%zu bit_off=%u (after EOB)\n",
                trace_block_id, k, rbit & 1, (size_t)bs->byte_off, (unsigned)bs->bit_off);
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
          (void)fprintf(stderr, "TRACE_JPEG_AC_REFINE block=%d EOB\n", trace_block_id);
        else
          (void)fprintf(stderr, "TRACE_JPEG_AC_REFINE EOB\n");
        (void)fflush(stderr);
      }
      if (log_sanity && trace_block_id >= 0 && k == ss) {
        (void)fprintf(stderr, "SANITY_OUR block=%d first_sym EOB\n", trace_block_id);
        (void)fprintf(stderr, "SANITY_OUR block=%d after_first_sym byte_off=%zu bit_off=%u\n",
            trace_block_id, (size_t)bs->byte_off, (unsigned)bs->bit_off);
        (void)fflush(stderr);
      }
      break;
    }
    int run = sym >> 4;
    int size = sym & 15;
    if (trace_all) {
      (void)fprintf(stderr, "AC_REFINE_HUFF_EXIT block=%d sym=0x%02x run=%d size=%d byte_off=%zu bit_off=%u\n",
          trace_block_id, (unsigned)sym, run, size, (size_t)bs->byte_off, (unsigned)bs->bit_off);
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
        if (bs->recover_stuff_zero) {
          b = 0;
        } else {
          if (getenv("GIMG_JPEG_PROGRESSIVE_DEBUG") && getenv("GIMG_JPEG_PROGRESSIVE_DEBUG")[0] == '1')
            (void)fprintf(stderr, " ac_refine underflow: refinement_bit at k=%d byte_off=%zu\n",
                k, (size_t)bs->byte_off);
          return GIMG_ERR_CORRUPT;
        }
      }
      if (trace_all) {
        (void)fprintf(stderr, "AC_REFINE_REFINEMENT_BIT block=%d bit=%d byte_off=%zu bit_off=%u\n",
            trace_block_id, b & 1, (size_t)bs->byte_off, (unsigned)bs->bit_off);
        (void)fflush(stderr);
      }
      if (log_sanity && trace_block_id >= 0 && k == ss) {
        (void)fprintf(stderr, "SANITY_OUR block=%d after_first_sym byte_off=%zu bit_off=%u\n",
            trace_block_id, (size_t)bs->byte_off, (unsigned)bs->bit_off);
        (void)fflush(stderr);
      }
      // Advance from k: skip run zeros; for each already-nonzero read one correction bit.
      while (run >= 0 && k <= se) {
        if (trace_all) {
          (void)fprintf(stderr, "AC_REFINE_STEP block=%d k=%d block_k=%d %s run_left=%d\n",
              trace_block_id, k, (int)block[k], block[k] == 0 ? "zero" : "nz", run);
          (void)fflush(stderr);
        }
        if (block[k] == 0) {
          if (--run < 0) {
            break;
          }
        } else {
          int rbit = jpeg_bitstream_read_bit(bs);
          if (rbit < 0) {
            if (bs->recover_stuff_zero) {
              rbit = 0;
            } else {
              if (getenv("GIMG_JPEG_PROGRESSIVE_DEBUG") && getenv("GIMG_JPEG_PROGRESSIVE_DEBUG")[0] == '1')
                (void)fprintf(stderr, " ac_refine underflow: correction_bit at k=%d byte_off=%zu\n",
                    k, (size_t)bs->byte_off);
              return GIMG_ERR_CORRUPT;
            }
          }
          int16_t old_val = block[k];
          /* Correction bit 1 = increase magnitude by 2^Al (T.81 G.1.2.2); add
           * (1<<Al) or -(1<<Al) to match libjpeg. */
          if (rbit & 1) {
            int16_t delta = (int16_t)(block[k] >= 0 ? (1 << bitpos) : -(1 << bitpos));
            block[k] += delta;
            if (trace_all) {
              (void)fprintf(stderr, "AC_REFINE_CORRECTION_BIT block=%d k=%d bit=%d delta=%d old=%d new=%d byte_off=%zu bit_off=%u\n",
                  trace_block_id, k, rbit & 1, (int)delta, (int)old_val, (int)block[k],
                  (size_t)bs->byte_off, (unsigned)bs->bit_off);
              (void)fflush(stderr);
            }
          } else if (trace_all) {
            (void)fprintf(stderr, "AC_REFINE_CORRECTION_BIT block=%d k=%d bit=%d (no change) byte_off=%zu bit_off=%u\n",
                trace_block_id, k, rbit & 1, (size_t)bs->byte_off, (unsigned)bs->bit_off);
            (void)fflush(stderr);
          }
          if (trace_block_id == 0 && getenv("GIMG_JPEG_AC_REFINE_CORRECTION_K") != NULL && !trace_all) {
            (void)fprintf(stderr, "AC_REFINE_CORRECTION_K block=0 k=%d block[k]=%d\n",
                k, (int)block[k]);
            (void)fflush(stderr);
          }
          if (do_trace && !trace_all) {
            if (trace_block_id >= 0)
              (void)fprintf(stderr, "TRACE_JPEG_AC_REFINE block=%d k=%d (already nz) bit=%d\n",
                  trace_block_id, k, rbit & 1);
            else
              (void)fprintf(stderr, "TRACE_JPEG_AC_REFINE k=%d (already nz) bit=%d\n",
                  k, rbit & 1);
            (void)fflush(stderr);
          }
        }
        k++;
      }
      if (k > se) {
        if (bs->recover_stuff_zero) {
          break;  // Stop this block; match recovery behavior.
        }
        return GIMG_ERR_CORRUPT;
      }
      if (do_trace) {
        if (trace_block_id >= 0)
          (void)fprintf(stderr, "TRACE_JPEG_AC_REFINE block=%d k=%d bit=%d (bitpos=%d)\n",
              trace_block_id, k, b & 1, bitpos);
        else
          (void)fprintf(stderr, "TRACE_JPEG_AC_REFINE k=%d bit=%d (bitpos=%d)\n",
              k, b & 1, bitpos);
        (void)fflush(stderr);
      }
      // Newly nonzero: refinement bit is the sign (1 = +, 0 = −); magnitude at Al is 1.
      if (k > se || k < ss) {
        if (getenv("GIMG_JPEG_PROGRESSIVE_DEBUG") && getenv("GIMG_JPEG_PROGRESSIVE_DEBUG")[0] == '1')
          (void)fprintf(stderr, " ac_refine k=%d out of band [%d,%d]\n", k, ss, se);
        return GIMG_ERR_CORRUPT;
      }
      {
        int16_t val = (int16_t)((b & 1) ? (1 << bitpos) : -(1 << bitpos));
        block[k] = val;
        if (trace_all) {
          (void)fprintf(stderr, "AC_REFINE_NEW_NZ block=%d k=%d val=%d (bitpos=%d sign=%d) byte_off=%zu bit_off=%u\n",
              trace_block_id, k, (int)val, bitpos, (b & 1) ? 1 : 0, (size_t)bs->byte_off, (unsigned)bs->bit_off);
          (void)fflush(stderr);
        }
      }
      k++;
    } else {
      if (run != 15) {
        return GIMG_ERR_CORRUPT;
      }
      int left = 16;
      while (left > 0 && k <= se) {
        if (block[k] == 0) {
          left--;
        }
        k++;
      }
    }
    /* Break-the-circle: log position after first symbol of block 0 (before
     * next Huffman decode). If (0,5) then next bit is 6th; if (0,4) we're
     * one bit short in the first symbol. */
    if (trace_block_id == 0 && k_at_iter_start == ss) {
      (void)fprintf(stderr, "AC_REFINE_AFTER_FIRST_SYM block=0 byte_off=%zu bit_off=%u\n",
          (size_t)bs->byte_off, (unsigned)bs->bit_off);
      (void)fflush(stderr);
    }
  }
  return GIMG_OK;
}

/** Dezigzag: block has 64 entries in zigzag order; write to out in row-major.
 */
static void jpeg_dezigzag(const int16_t * block, int16_t * out) {
  for (int i = 0; i < 64; i++) {
    out[gimg_jpeg_zigzag[i]] = block[i];
  }
}

/** Dequantise: out[i] = block[i] * quant[zigzag_index(i)]. DQT is in zigzag
 * order. */
static void jpeg_dequantise(
    const int16_t * block, const uint16_t * quant, int16_t * out) {
  for (int i = 0; i < 64; i++) {
    out[i] = (int16_t)((int)block[i] * (int)quant[gimg_jpeg_inv_zigzag[i]]);
  }
}

/** Dequantise to int32_t for 12/16-bit precision (avoids overflow). */
static void jpeg_dequantise_32(
    const int16_t * block, const uint16_t * quant, int32_t * out) {
  for (int i = 0; i < 64; i++) {
    out[i] = (int32_t)block[i] * (int32_t)quant[gimg_jpeg_inv_zigzag[i]];
  }
}

// 1D IDCT for 8 points (row or column). Coefficients in, samples out.
// Round only at the end (sum/scale + 0.5) to match reference IDCT and avoid
// per-term rounding drift (e.g. Cb block differing by 1 from reference).
static void jpeg_idct_1d(const int16_t * in, int16_t * out) {
  static const int scale = 256;
  for (int x = 0; x < 8; x++) {
    double sum = 0.0;
    for (int u = 0; u < 8; u++) {
      int32_t c = (u == 0) ? 181 : 256; // 256/sqrt(2) ~ 181
      double angle = (2 * x + 1) * u * 3.14159265358979323846 / 16.0;
      sum += (double)in[u] * (double)c * cos(angle);
    }
    out[x] = (int16_t)(int)(sum / (double)scale + 0.5);
  }
}

/** 1D IDCT for 8 points, int32_t in/out; scale applied (256=8b, 4096=12b,
 * 65536=16b). Round at end to match 8-bit path. */
static void jpeg_idct_1d_32(const int32_t * in, int32_t * out, int scale) {
  for (int x = 0; x < 8; x++) {
    double sum = 0.0;
    for (int u = 0; u < 8; u++) {
      int32_t c = (u == 0) ? 181 : 256;
      double angle = (2 * x + 1) * u * 3.14159265358979323846 / 16.0;
      sum += (double)in[u] * (double)c * cos(angle);
    }
    out[x] = (int32_t)(int)(sum / (double)scale + 0.5);
  }
}

/** 2D 8x8 IDCT (row-major 64 entries). */
static void jpeg_idct_8x8(const int16_t * in, int16_t * out) {
  int16_t row[8];
  int16_t tmp[64];
  for (int y = 0; y < 8; y++) {
    jpeg_idct_1d(in + y * 8, row);
    for (int x = 0; x < 8; x++) {
      tmp[x * 8 + y] = row[x];
    }
  }
  for (int x = 0; x < 8; x++) {
    jpeg_idct_1d(tmp + x * 8, row);
    for (int y = 0; y < 8; y++) {
      out[y * 8 + x] = row[y];
    }
  }
}

/** 2D 8x8 IDCT int32_t path; scale = 256 (8b), 4096 (12b), or 65536 (16b) per
 * 1D. */
static void jpeg_idct_8x8_32(const int32_t * in, int32_t * out, int scale) {
  int32_t row[8];
  int32_t tmp[64];
  for (int y = 0; y < 8; y++) {
    jpeg_idct_1d_32(in + y * 8, row, scale);
    for (int x = 0; x < 8; x++) {
      tmp[x * 8 + y] = row[x];
    }
  }
  for (int x = 0; x < 8; x++) {
    jpeg_idct_1d_32(tmp + x * 8, row, scale);
    for (int y = 0; y < 8; y++) {
      out[y * 8 + x] = row[y];
    }
  }
}

//
// 8x8 inverse DCT: integer implementation per T.81 Annex A.3.3 (Loeffler–
// Ligtenberg–Moschytz). CONST_BITS=13, PASS1_BITS=2; input is row-major
// dequantized coefficients; output is row-major int16_t in range ~[-128,127]
// (caller adds 128 and clamps to 0..255).
//
#define IDCT_ISLOW_CONST_BITS 13
#define IDCT_ISLOW_PASS1_BITS 2
#define IDCT_ISLOW_ONE ((int32_t)1)
#define IDCT_ISLOW_LEFT_SHIFT(x, n) ((int32_t)(x) << (n))
#define IDCT_ISLOW_RIGHT_SHIFT(x, n) ((x) >> (n))
#define IDCT_ISLOW_DESCALE(x, n)                                               \
  IDCT_ISLOW_RIGHT_SHIFT((x) + (IDCT_ISLOW_ONE << ((n)-1)), n)
#define IDCT_ISLOW_MULTIPLY(var, c) ((int32_t)(var) * (int32_t)(c))

// FIX constants for CONST_BITS=13 (T.81 Annex A.3.3 integer IDCT).
static const int32_t idct_islow_fix_0_298631336 = 2446;
static const int32_t idct_islow_fix_0_390180644 = 3196;
static const int32_t idct_islow_fix_0_541196100 = 4433;
static const int32_t idct_islow_fix_0_765366865 = 6270;
static const int32_t idct_islow_fix_0_899976223 = 7373;
static const int32_t idct_islow_fix_1_175875602 = 9633;
static const int32_t idct_islow_fix_1_501321110 = 12299;
static const int32_t idct_islow_fix_1_847759065 = 15137;
static const int32_t idct_islow_fix_1_961570560 = 16069;
static const int32_t idct_islow_fix_2_053119869 = 16819;
static const int32_t idct_islow_fix_2_562915447 = 20995;
static const int32_t idct_islow_fix_3_072711026 = 25172;

static void jpeg_idct_8x8_islow(const int16_t * in, int16_t * out) {
  int32_t tmp0, tmp1, tmp2, tmp3;
  int32_t tmp10, tmp11, tmp12, tmp13;
  int32_t z1, z2, z3, z4, z5;
  int workspace[64];
  const int dct_bits = IDCT_ISLOW_CONST_BITS;
  const int pass1_bits = IDCT_ISLOW_PASS1_BITS;
  const int descale_pass1 = dct_bits - pass1_bits;     // 11
  const int descale_pass2 = dct_bits + pass1_bits + 3; // 18
  const int descale_dc_row = pass1_bits + 3;           // 5: for DC-only row

  // Pass 1: process columns from input, store into work array.
  for (int ctr = 0; ctr < 8; ctr++) {
    const int16_t * inptr = in + ctr;
    int * wsptr = workspace + ctr;

    if (inptr[8] == 0 && inptr[16] == 0 && inptr[24] == 0 && inptr[32] == 0 &&
        inptr[40] == 0 && inptr[48] == 0 && inptr[56] == 0) {
      int32_t dcval = IDCT_ISLOW_LEFT_SHIFT((int32_t)inptr[0], pass1_bits);
      for (int r = 0; r < 8; r++) {
        wsptr[r * 8] = (int)dcval;
      }
      continue;
    }

    z2 = (int32_t)inptr[16];
    z3 = (int32_t)inptr[48];
    z1 = IDCT_ISLOW_MULTIPLY(z2 + z3, idct_islow_fix_0_541196100);
    tmp2 = z1 + IDCT_ISLOW_MULTIPLY(z3, -idct_islow_fix_1_847759065);
    tmp3 = z1 + IDCT_ISLOW_MULTIPLY(z2, idct_islow_fix_0_765366865);

    z2 = (int32_t)inptr[0];
    z3 = (int32_t)inptr[32];
    tmp0 = IDCT_ISLOW_LEFT_SHIFT(z2 + z3, dct_bits);
    tmp1 = IDCT_ISLOW_LEFT_SHIFT(z2 - z3, dct_bits);

    tmp10 = tmp0 + tmp3;
    tmp13 = tmp0 - tmp3;
    tmp11 = tmp1 + tmp2;
    tmp12 = tmp1 - tmp2;

    tmp0 = (int32_t)inptr[56];
    tmp1 = (int32_t)inptr[40];
    tmp2 = (int32_t)inptr[24];
    tmp3 = (int32_t)inptr[8];

    z1 = tmp0 + tmp3;
    z2 = tmp1 + tmp2;
    z3 = tmp0 + tmp2;
    z4 = tmp1 + tmp3;
    z5 = IDCT_ISLOW_MULTIPLY(z3 + z4, idct_islow_fix_1_175875602);

    tmp0 = IDCT_ISLOW_MULTIPLY(tmp0, idct_islow_fix_0_298631336);
    tmp1 = IDCT_ISLOW_MULTIPLY(tmp1, idct_islow_fix_2_053119869);
    tmp2 = IDCT_ISLOW_MULTIPLY(tmp2, idct_islow_fix_3_072711026);
    tmp3 = IDCT_ISLOW_MULTIPLY(tmp3, idct_islow_fix_1_501321110);
    z1 = IDCT_ISLOW_MULTIPLY(z1, -idct_islow_fix_0_899976223);
    z2 = IDCT_ISLOW_MULTIPLY(z2, -idct_islow_fix_2_562915447);
    z3 = IDCT_ISLOW_MULTIPLY(z3, -idct_islow_fix_1_961570560);
    z4 = IDCT_ISLOW_MULTIPLY(z4, -idct_islow_fix_0_390180644);

    z3 += z5;
    z4 += z5;

    tmp0 += z1 + z3;
    tmp1 += z2 + z4;
    tmp2 += z2 + z3;
    tmp3 += z1 + z4;

    wsptr[0] = (int)IDCT_ISLOW_DESCALE(tmp10 + tmp3, descale_pass1);
    wsptr[56] = (int)IDCT_ISLOW_DESCALE(tmp10 - tmp3, descale_pass1);
    wsptr[8] = (int)IDCT_ISLOW_DESCALE(tmp11 + tmp2, descale_pass1);
    wsptr[48] = (int)IDCT_ISLOW_DESCALE(tmp11 - tmp2, descale_pass1);
    wsptr[16] = (int)IDCT_ISLOW_DESCALE(tmp12 + tmp1, descale_pass1);
    wsptr[40] = (int)IDCT_ISLOW_DESCALE(tmp12 - tmp1, descale_pass1);
    wsptr[24] = (int)IDCT_ISLOW_DESCALE(tmp13 + tmp0, descale_pass1);
    wsptr[32] = (int)IDCT_ISLOW_DESCALE(tmp13 - tmp0, descale_pass1);
  }

  // Pass 2: process rows from work array, store into output.
  for (int ctr = 0; ctr < 8; ctr++) {
    const int * wsptr = workspace + ctr * 8;
    int16_t * outptr = out + ctr * 8;

    if (wsptr[1] == 0 && wsptr[2] == 0 && wsptr[3] == 0 && wsptr[4] == 0 &&
        wsptr[5] == 0 && wsptr[6] == 0 && wsptr[7] == 0) {
      int32_t v = IDCT_ISLOW_DESCALE((int32_t)wsptr[0], descale_dc_row);
      if (v < -128)
        v = -128;
      if (v > 127)
        v = 127;
      int16_t dcval = (int16_t)v;
      for (int c = 0; c < 8; c++) {
        outptr[c] = dcval;
      }
      continue;
    }

    z2 = (int32_t)wsptr[2];
    z3 = (int32_t)wsptr[6];
    z1 = IDCT_ISLOW_MULTIPLY(z2 + z3, idct_islow_fix_0_541196100);
    tmp2 = z1 + IDCT_ISLOW_MULTIPLY(z3, -idct_islow_fix_1_847759065);
    tmp3 = z1 + IDCT_ISLOW_MULTIPLY(z2, idct_islow_fix_0_765366865);

    tmp0 =
        IDCT_ISLOW_LEFT_SHIFT((int32_t)wsptr[0] + (int32_t)wsptr[4], dct_bits);
    tmp1 =
        IDCT_ISLOW_LEFT_SHIFT((int32_t)wsptr[0] - (int32_t)wsptr[4], dct_bits);

    tmp10 = tmp0 + tmp3;
    tmp13 = tmp0 - tmp3;
    tmp11 = tmp1 + tmp2;
    tmp12 = tmp1 - tmp2;

    tmp0 = (int32_t)wsptr[7];
    tmp1 = (int32_t)wsptr[5];
    tmp2 = (int32_t)wsptr[3];
    tmp3 = (int32_t)wsptr[1];

    z1 = tmp0 + tmp3;
    z2 = tmp1 + tmp2;
    z3 = tmp0 + tmp2;
    z4 = tmp1 + tmp3;
    z5 = IDCT_ISLOW_MULTIPLY(z3 + z4, idct_islow_fix_1_175875602);

    tmp0 = IDCT_ISLOW_MULTIPLY(tmp0, idct_islow_fix_0_298631336);
    tmp1 = IDCT_ISLOW_MULTIPLY(tmp1, idct_islow_fix_2_053119869);
    tmp2 = IDCT_ISLOW_MULTIPLY(tmp2, idct_islow_fix_3_072711026);
    tmp3 = IDCT_ISLOW_MULTIPLY(tmp3, idct_islow_fix_1_501321110);
    z1 = IDCT_ISLOW_MULTIPLY(z1, -idct_islow_fix_0_899976223);
    z2 = IDCT_ISLOW_MULTIPLY(z2, -idct_islow_fix_2_562915447);
    z3 = IDCT_ISLOW_MULTIPLY(z3, -idct_islow_fix_1_961570560);
    z4 = IDCT_ISLOW_MULTIPLY(z4, -idct_islow_fix_0_390180644);

    z3 += z5;
    z4 += z5;

    tmp0 += z1 + z3;
    tmp1 += z2 + z4;
    tmp2 += z2 + z3;
    tmp3 += z1 + z4;

    outptr[0] = (int16_t)IDCT_ISLOW_DESCALE(tmp10 + tmp3, descale_pass2);
    outptr[7] = (int16_t)IDCT_ISLOW_DESCALE(tmp10 - tmp3, descale_pass2);
    outptr[1] = (int16_t)IDCT_ISLOW_DESCALE(tmp11 + tmp2, descale_pass2);
    outptr[6] = (int16_t)IDCT_ISLOW_DESCALE(tmp11 - tmp2, descale_pass2);
    outptr[2] = (int16_t)IDCT_ISLOW_DESCALE(tmp12 + tmp1, descale_pass2);
    outptr[5] = (int16_t)IDCT_ISLOW_DESCALE(tmp12 - tmp1, descale_pass2);
    outptr[3] = (int16_t)IDCT_ISLOW_DESCALE(tmp13 + tmp0, descale_pass2);
    outptr[4] = (int16_t)IDCT_ISLOW_DESCALE(tmp13 - tmp0, descale_pass2);
  }
}

#undef IDCT_ISLOW_CONST_BITS
#undef IDCT_ISLOW_PASS1_BITS
#undef IDCT_ISLOW_ONE
#undef IDCT_ISLOW_LEFT_SHIFT
#undef IDCT_ISLOW_RIGHT_SHIFT
#undef IDCT_ISLOW_DESCALE
#undef IDCT_ISLOW_MULTIPLY

/** Baseline decode for 12- or 16-bit precision (SOF1/SOF2). Grayscale and YCbCr
 * only; CMYK at 12/16-bit returns GIMG_ERR_UNSUPPORTED. */
static GIMG_Result jpeg_decode_baseline_extended(
    const gimg_jpeg_doc_state_t * state, const GIMG_Decode_Options * options,
    GIMG_Raster ** out_raster) {
  const gimg_jpeg_sof_t * sof = &state->sof;
  uint8_t precision = sof->precision;
  if (precision != 12 && precision != 16) {
    return GIMG_ERR_UNSUPPORTED;
  }
  uint16_t width = sof->width;
  uint16_t height = sof->height;
  uint8_t num_comp = sof->num_components;
  if (num_comp != 1 && num_comp != 3) {
    return GIMG_ERR_UNSUPPORTED; // 12/16-bit: grayscale or YCbCr only
  }

  size_t pixel_count = 0;
  if (gimg_safe_pixel_count((uint32_t)width, (uint32_t)height, &pixel_count) !=
      GIMG_OK) {
    return GIMG_ERR_LIMIT;
  }
  const GIMG_Limits * limits =
      options && options->limits ? options->limits : NULL;
  if (limits && limits->max_decoded_pixels != 0 &&
      pixel_count > limits->max_decoded_pixels) {
    return GIMG_ERR_LIMIT;
  }

  const gimg_jpeg_scan_t * scan0 = &state->scans[0];
  gimg_jpeg_huff_table_t dc_tables[4];
  gimg_jpeg_huff_table_t ac_tables[4];
  memset(dc_tables, 0, sizeof(dc_tables));
  memset(ac_tables, 0, sizeof(ac_tables));
  for (uint8_t c = 0; c < scan0->comp_count; c++) {
    uint8_t dc_id = scan0->dc_tbl[c];
    uint8_t ac_id = scan0->ac_tbl[c];
    if (dc_id >= 4 || !state->huff_dc[dc_id] ||
        jpeg_build_huff_table(state->huff_dc[dc_id], state->huff_dc_len[dc_id],
            &dc_tables[dc_id]) != 0) {
      return GIMG_ERR_CORRUPT;
    }
    if (ac_id >= 4 || !state->huff_ac[ac_id] ||
        jpeg_build_huff_table(state->huff_ac[ac_id], state->huff_ac_len[ac_id],
            &ac_tables[ac_id]) != 0) {
      return GIMG_ERR_CORRUPT;
    }
  }

  uint8_t h_max = 0;
  uint8_t v_max = 0;
  for (uint8_t i = 0; i < num_comp; i++) {
    if (sof->h_samp[i] > h_max)
      h_max = sof->h_samp[i];
    if (sof->v_samp[i] > v_max)
      v_max = sof->v_samp[i];
  }
  if (h_max == 0 || v_max == 0) {
    return GIMG_ERR_FORMAT;
  }
  uint32_t mcu_w = (uint32_t)(8 * h_max);
  uint32_t mcu_h = (uint32_t)(8 * v_max);
  uint32_t mcu_per_row = (width + mcu_w - 1) / mcu_w;
  uint32_t mcu_per_col = (height + mcu_h - 1) / mcu_h;

  const GIMG_Allocator * alloc = state->allocator;
  alloc = gimg_alloc_or_default(alloc);

  uint32_t denom_w = (uint32_t)(8 * h_max);
  uint32_t denom_h = (uint32_t)(8 * v_max);
  uint32_t comp_w[GIMG_JPEG_MAX_COMPONENTS];
  uint32_t comp_h[GIMG_JPEG_MAX_COMPONENTS];
  for (uint8_t i = 0; i < num_comp; i++) {
    uint32_t w = (uint32_t)width * (uint32_t)sof->h_samp[i];
    uint32_t h = (uint32_t)height * (uint32_t)sof->v_samp[i];
    comp_w[i] = 8 * ((w + denom_w - 1) / denom_w);
    comp_h[i] = 8 * ((h + denom_h - 1) / denom_h);
    if (comp_w[i] == 0)
      comp_w[i] = 8;
    if (comp_h[i] == 0)
      comp_h[i] = 8;
  }

  size_t comp_size[GIMG_JPEG_MAX_COMPONENTS];
  size_t comp_stride_el[GIMG_JPEG_MAX_COMPONENTS];
  uint16_t * comp_buf[GIMG_JPEG_MAX_COMPONENTS];
  for (uint8_t i = 0; i < num_comp; i++) {
    comp_stride_el[i] = (size_t)comp_w[i];
    if (!gimg_safe_mul_size(
            comp_stride_el[i], (size_t)comp_h[i], &comp_size[i]) ||
        !gimg_safe_mul_size(comp_size[i], sizeof(uint16_t), &comp_size[i])) {
      for (uint8_t j = 0; j < i; j++)
        gimg_free(alloc, comp_buf[j]);
      return GIMG_ERR_LIMIT;
    }
    comp_buf[i] = (uint16_t *)gimg_malloc(alloc, comp_size[i]);
    if (!comp_buf[i]) {
      for (uint8_t j = 0; j < i; j++)
        gimg_free(alloc, comp_buf[j]);
      return GIMG_ERR_OOM;
    }
    memset(comp_buf[i], 0, comp_size[i]);
  }

  int idct_scale = (precision == 12) ? 4096 : 65536;
  int level_shift = (precision == 12) ? 2048 : 32768;
  int max_val = (precision == 12) ? 4095 : 65535;

  gimg_jpeg_bitstream_t bs;
  jpeg_bitstream_init(&bs, scan0->data, scan0->data_size);
  int16_t dc_pred[GIMG_JPEG_MAX_COMPONENTS];
  memset(dc_pred, 0, sizeof(dc_pred));
  int16_t block_zig[64];
  int16_t block_rz[64];
  int32_t block_q[64];
  int32_t block_idct[64];

  uint16_t restart_interval = state->restart_interval;
  for (uint32_t mcu_y = 0; mcu_y < mcu_per_col; mcu_y++) {
    for (uint32_t mcu_x = 0; mcu_x < mcu_per_row; mcu_x++) {
      if (restart_interval > 0) {
        uint32_t mcu_index = mcu_y * mcu_per_row + mcu_x;
        if (mcu_index > 0 && (mcu_index % (uint32_t)restart_interval) == 0) {
          memset(dc_pred, 0, sizeof(dc_pred));
        }
      }
      for (uint8_t s = 0; s < scan0->comp_count; s++) {
        uint8_t comp_idx = 0;
        for (; comp_idx < num_comp; comp_idx++) {
          if (sof->comp_id[comp_idx] == scan0->comp_id[s])
            break;
        }
        if (comp_idx >= num_comp) {
          goto ext_fail;
        }
        uint8_t h_samp = sof->h_samp[comp_idx];
        uint8_t v_samp = sof->v_samp[comp_idx];
        uint8_t qid = sof->quant_tbl_id[comp_idx];
        if (qid >= GIMG_JPEG_MAX_QUANT_TABLES ||
            !state->quant_tbl_present[qid]) {
          goto ext_fail;
        }
        const uint16_t * quant = state->quant_tbl[qid];
        const gimg_jpeg_huff_table_t * dc_tbl = &dc_tables[scan0->dc_tbl[s]];
        const gimg_jpeg_huff_table_t * ac_tbl = &ac_tables[scan0->ac_tbl[s]];

        for (uint8_t by = 0; by < v_samp; by++) {
          for (uint8_t bx = 0; bx < h_samp; bx++) {
            GIMG_Result r = jpeg_decode_block(
                &bs, dc_tbl, ac_tbl, block_zig, &dc_pred[comp_idx]);
            if (r != GIMG_OK) {
              goto ext_fail;
            }
            jpeg_dezigzag(block_zig, block_rz);
            jpeg_dequantise_32(block_rz, quant, block_q);
            jpeg_idct_8x8_32(block_q, block_idct, idct_scale);

            uint32_t dst_x = mcu_x * (uint32_t)(8 * h_samp) + (uint32_t)bx * 8;
            uint32_t dst_y = mcu_y * (uint32_t)(8 * v_samp) + (uint32_t)by * 8;
            uint32_t cw = comp_w[comp_idx];
            uint32_t ch = comp_h[comp_idx];
            for (int dy = 0; dy < 8; dy++) {
              uint32_t y = dst_y + (uint32_t)dy;
              if (y >= ch)
                break;
              for (int dx = 0; dx < 8; dx++) {
                uint32_t x = dst_x + (uint32_t)dx;
                if (x >= cw)
                  break;
                int32_t v = block_idct[dy * 8 + dx] + level_shift;
                if (v < 0)
                  v = 0;
                if (v > max_val)
                  v = max_val;
                if (precision == 12) {
                  v = v << 4; // left-justify 12-bit in 16-bit
                }
                comp_buf[comp_idx][y * comp_stride_el[comp_idx] + x] =
                    (uint16_t)v;
              }
            }
          }
        }
      }
    }
  }

  GIMG_Result r;
  if (num_comp == 1) {
    r = gimg_raster_create_with_allocator(alloc, (uint32_t)width,
        (uint32_t)height, &GIMG_PIXEL_GRAY16, GIMG_RASTER_OWNED, NULL, 0,
        out_raster);
    if (r != GIMG_OK) {
      goto ext_fail;
    }
    uint16_t * pixels = (uint16_t *)gimg_raster_pixels(*out_raster);
    size_t stride_el = gimg_raster_stride_bytes(*out_raster) / 2;
    for (uint32_t y = 0; y < height; y++) {
      for (uint32_t x = 0; x < width; x++) {
        pixels[y * stride_el + x] = comp_buf[0][y * comp_stride_el[0] + x];
      }
    }
  }
  else {
    r = gimg_raster_create_with_allocator(alloc, (uint32_t)width,
        (uint32_t)height, &GIMG_PIXEL_RGBA16, GIMG_RASTER_OWNED, NULL, 0,
        out_raster);
    if (r != GIMG_OK) {
      goto ext_fail;
    }
    uint16_t * pixels = (uint16_t *)gimg_raster_pixels(*out_raster);
    size_t stride_el = gimg_raster_stride_bytes(*out_raster) / 8;
    uint32_t cw1 = comp_w[1];
    uint32_t ch1 = comp_h[1];
    uint32_t cw2 = comp_w[2];
    uint32_t ch2 = comp_h[2];
    // For 12-bit, comp_buf is left-justified (sample<<4); for 16-bit, raw.
    int mid = level_shift;
    for (uint32_t y = 0; y < height; y++) {
      uint32_t cy1 = (ch1 > 1 && height > 1) ? (y * ch1 / height) : 0;
      uint32_t cy2 = (ch2 > 1 && height > 1) ? (y * ch2 / height) : 0;
      for (uint32_t x = 0; x < width; x++) {
        uint32_t cx1 = (cw1 > 1 && width > 1) ? (x * cw1 / width) : 0;
        uint32_t cx2 = (cw2 > 1 && width > 1) ? (x * cw2 / width) : 0;
        int yy, cb, cr;
        if (precision == 12) {
          yy = (comp_buf[0][y * comp_stride_el[0] + x] >> 4) - mid;
          cb = (comp_buf[1][cy1 * comp_stride_el[1] + cx1] >> 4) - mid;
          cr = (comp_buf[2][cy2 * comp_stride_el[2] + cx2] >> 4) - mid;
        }
        else {
          yy = (int)comp_buf[0][y * comp_stride_el[0] + x] - mid;
          cb = (int)comp_buf[1][cy1 * comp_stride_el[1] + cx1] - mid;
          cr = (int)comp_buf[2][cy2 * comp_stride_el[2] + cx2] - mid;
        }
        int r_val = yy + (int)(1.40200 * cr + 0.5);
        int g_val = yy - (int)(0.34414 * cb + 0.71414 * cr + 0.5);
        int b_val = yy + (int)(1.77200 * cb + 0.5);
        // 12-bit: scale to 16-bit range (left-justified); 16-bit: full range.
        int out_max = (precision == 12) ? 65520 : 65535;
        if (precision == 12) {
          r_val <<= 4;
          g_val <<= 4;
          b_val <<= 4;
        }
        if (r_val < 0)
          r_val = 0;
        if (r_val > out_max)
          r_val = out_max;
        if (g_val < 0)
          g_val = 0;
        if (g_val > out_max)
          g_val = out_max;
        if (b_val < 0)
          b_val = 0;
        if (b_val > out_max)
          b_val = out_max;
        pixels[y * stride_el + x * 4 + 0] = (uint16_t)r_val;
        pixels[y * stride_el + x * 4 + 1] = (uint16_t)g_val;
        pixels[y * stride_el + x * 4 + 2] = (uint16_t)b_val;
        pixels[y * stride_el + x * 4 + 3] = (uint16_t)65535;
      }
    }
  }

  if (state->app2_icc && state->app2_icc_len > 0u) {
    GIMG_Color_Info color_info;
    gimg_color_info_default(&color_info);
    if (state->app2_icc_num_chunks > 0) {
      color_info.icc_bytes = state->app2_icc;
      color_info.icc_size = state->app2_icc_len;
    }
    else {
      color_info.icc_bytes = state->app2_icc + 14;
      color_info.icc_size = state->app2_icc_len - 14u;
    }
    (void)gimg_raster_set_color_info(*out_raster, &color_info);
  }
  for (uint8_t i = 0; i < num_comp; i++) {
    gimg_free(alloc, comp_buf[i]);
  }
  return GIMG_OK;

ext_fail:
  if (*out_raster) {
    gimg_raster_destroy(*out_raster);
    *out_raster = NULL;
  }
  for (uint8_t i = 0; i < num_comp; i++) {
    gimg_free(alloc, comp_buf[i]);
  }
  return GIMG_ERR_CORRUPT;
}

GIMG_Result gimg_jpeg_decode_baseline(const gimg_jpeg_doc_state_t * state,
    const GIMG_Decode_Options * options, GIMG_Raster ** out_raster) {
  GIMG_Result r = GIMG_ERR_CORRUPT;
  if (!state || !out_raster) {
    return GIMG_ERR_INTERNAL;
  }
  *out_raster = NULL;

  if (state->is_progressive) {
    return GIMG_ERR_UNSUPPORTED;
  }

  const gimg_jpeg_sof_t * sof = &state->sof;
  uint16_t width = sof->width;
  uint16_t height = sof->height;
  uint8_t num_comp = sof->num_components;

  size_t pixel_count = 0;
  if (gimg_safe_pixel_count((uint32_t)width, (uint32_t)height, &pixel_count) !=
      GIMG_OK) {
    return GIMG_ERR_LIMIT;
  }
  const GIMG_Limits * limits =
      options && options->limits ? options->limits : NULL;
  if (limits && limits->max_decoded_pixels != 0 &&
      pixel_count > limits->max_decoded_pixels) {
    return GIMG_ERR_LIMIT;
  }

  if (state->num_scans == 0) {
    return GIMG_ERR_CORRUPT;
  }
  const gimg_jpeg_scan_t * scan0 = &state->scans[0];
  if (!scan0->data || scan0->data_size == 0) {
    return GIMG_ERR_CORRUPT;
  }

  if (sof->precision != 8) {
    return jpeg_decode_baseline_extended(state, options, out_raster);
  }

  // Build Huffman tables for scan components.
  gimg_jpeg_huff_table_t dc_tables[4];
  gimg_jpeg_huff_table_t ac_tables[4];
  memset(dc_tables, 0, sizeof(dc_tables));
  memset(ac_tables, 0, sizeof(ac_tables));
  for (uint8_t c = 0; c < scan0->comp_count; c++) {
    uint8_t dc_id = scan0->dc_tbl[c];
    uint8_t ac_id = scan0->ac_tbl[c];
    if (dc_id >= 4 || !state->huff_dc[dc_id] ||
        jpeg_build_huff_table(state->huff_dc[dc_id], state->huff_dc_len[dc_id],
            &dc_tables[dc_id]) != 0) {
      return GIMG_ERR_CORRUPT;
    }
    if (ac_id >= 4 || !state->huff_ac[ac_id] ||
        jpeg_build_huff_table(state->huff_ac[ac_id], state->huff_ac_len[ac_id],
            &ac_tables[ac_id]) != 0) {
      return GIMG_ERR_CORRUPT;
    }
  }
  if (getenv("GIMG_JPEG_DEBUG_BASELINE_FAIL")) {
    /* Sanity: AC Th=1 first 3-bit symbol should be EOB (0x00) per T.81 Table K.6. */
    if (state->huff_ac[1] && state->huff_ac_len[1] >= 17 + 2) {
      const unsigned char * bits = state->huff_ac[1] + 1;
      if (bits[2] >= 1) {
        (void)fprintf(stderr,
            "BASELINE_DEBUG AC1 first 3-bit symbol=0x%02x (expect 0x00 EOB)\n",
            (unsigned)state->huff_ac[1][17]);
      }
    }
    (void)fprintf(stderr, "BASELINE_DEBUG scan_data_size=%zu first 16 bytes:",
        (size_t)scan0->data_size);
    for (size_t i = 0; i < 16 && i < scan0->data_size; i++) {
      (void)fprintf(stderr, " %02x", (unsigned)scan0->data[i]);
    }
    (void)fprintf(stderr, "\n");
    (void)fflush(stderr);
  }

  // MCU dimensions: Hmax, Vmax; blocks per MCU.
  uint8_t h_max = 0;
  uint8_t v_max = 0;
  for (uint8_t i = 0; i < num_comp; i++) {
    if (sof->h_samp[i] > h_max) {
      h_max = sof->h_samp[i];
    }
    if (sof->v_samp[i] > v_max) {
      v_max = sof->v_samp[i];
    }
  }
  if (h_max == 0 || v_max == 0) {
    return GIMG_ERR_FORMAT;
  }
  uint32_t mcu_w = (uint32_t)(8 * h_max);
  uint32_t mcu_h = (uint32_t)(8 * v_max);
  uint32_t mcu_per_row = (width + mcu_w - 1) / mcu_w;
  uint32_t mcu_per_col = (height + mcu_h - 1) / mcu_h;
  size_t mcu_total = 0;
  if (!gimg_safe_mul_size(
          (size_t)mcu_per_row, (size_t)mcu_per_col, &mcu_total)) {
    return GIMG_ERR_LIMIT;
  }

  // Blocks per MCU per component.
  size_t blocks_per_mcu = 0;
  for (uint8_t i = 0; i < scan0->comp_count; i++) {
    uint8_t comp_idx = 0;
    for (; comp_idx < num_comp; comp_idx++) {
      if (sof->comp_id[comp_idx] == scan0->comp_id[i]) {
        break;
      }
    }
    if (comp_idx >= num_comp) {
      return GIMG_ERR_FORMAT;
    }
    blocks_per_mcu +=
        (size_t)sof->h_samp[comp_idx] * (size_t)sof->v_samp[comp_idx];
  }

  const GIMG_Allocator * alloc = state->allocator;
  alloc = gimg_alloc_or_default(alloc);

  // Per-component dimensions: 8 * ceil((width*H_c)/(8*H_max)) (ITU-T T.81).
  uint32_t comp_w[GIMG_JPEG_MAX_COMPONENTS];
  uint32_t comp_h[GIMG_JPEG_MAX_COMPONENTS];
  uint32_t denom_w = (uint32_t)(8 * h_max);
  uint32_t denom_h = (uint32_t)(8 * v_max);
  for (uint8_t i = 0; i < num_comp; i++) {
    uint32_t w = (uint32_t)width * (uint32_t)sof->h_samp[i];
    uint32_t h = (uint32_t)height * (uint32_t)sof->v_samp[i];
    comp_w[i] = 8 * ((w + denom_w - 1) / denom_w);
    comp_h[i] = 8 * ((h + denom_h - 1) / denom_h);
    if (comp_w[i] == 0)
      comp_w[i] = 8;
    if (comp_h[i] == 0)
      comp_h[i] = 8;
  }

  size_t comp_size[GIMG_JPEG_MAX_COMPONENTS];
  size_t comp_stride[GIMG_JPEG_MAX_COMPONENTS];
  unsigned char * comp_buf[GIMG_JPEG_MAX_COMPONENTS];
  for (uint8_t i = 0; i < num_comp; i++) {
    comp_stride[i] = (size_t)comp_w[i];
    if (!gimg_safe_mul_size(comp_stride[i], (size_t)comp_h[i], &comp_size[i])) {
      for (uint8_t j = 0; j < i; j++) {
        gimg_free(alloc, comp_buf[j]);
      }
      return GIMG_ERR_LIMIT;
    }
    comp_buf[i] = (unsigned char *)gimg_malloc(alloc, comp_size[i]);
    if (!comp_buf[i]) {
      for (uint8_t j = 0; j < i; j++) {
        gimg_free(alloc, comp_buf[j]);
      }
      return GIMG_ERR_OOM;
    }
    memset(comp_buf[i], 0, comp_size[i]);
  }

  gimg_jpeg_bitstream_t bs;
  jpeg_bitstream_init(&bs, scan0->data, scan0->data_size);

  int16_t dc_pred[GIMG_JPEG_MAX_COMPONENTS];
  memset(dc_pred, 0, sizeof(dc_pred));

  int16_t block_zig[64];
  int16_t block_rz[64];
  int16_t block_q[64];
  memset(block_zig, 0, sizeof(block_zig));
  memset(block_rz, 0, sizeof(block_rz));
  memset(block_q, 0, sizeof(block_q));

  // Decode MCU by MCU. At each restart boundary (DRI), reset DC predictors.
  uint16_t restart_interval = state->restart_interval;
  for (uint32_t mcu_y = 0; mcu_y < mcu_per_col; mcu_y++) {
    for (uint32_t mcu_x = 0; mcu_x < mcu_per_row; mcu_x++) {
      if (restart_interval > 0) {
        uint32_t mcu_index = mcu_y * mcu_per_row + mcu_x;
        if (mcu_index > 0 && (mcu_index % (uint32_t)restart_interval) == 0) {
          memset(dc_pred, 0, sizeof(dc_pred));
        }
      }
      size_t block_idx = 0;
      for (uint8_t s = 0; s < scan0->comp_count; s++) {
        uint8_t comp_idx = 0;
        for (; comp_idx < num_comp; comp_idx++) {
          if (sof->comp_id[comp_idx] == scan0->comp_id[s]) {
            break;
          }
        }
        if (comp_idx >= num_comp) {
          goto fail_comp;
        }
        uint8_t h_samp = sof->h_samp[comp_idx];
        uint8_t v_samp = sof->v_samp[comp_idx];
        uint8_t qid = sof->quant_tbl_id[comp_idx];
        if (qid >= GIMG_JPEG_MAX_QUANT_TABLES ||
            !state->quant_tbl_present[qid]) {
          goto fail_comp;
        }
        const uint16_t * quant = state->quant_tbl[qid];
        const gimg_jpeg_huff_table_t * dc_tbl = &dc_tables[scan0->dc_tbl[s]];
        const gimg_jpeg_huff_table_t * ac_tbl = &ac_tables[scan0->ac_tbl[s]];

        for (uint8_t by = 0; by < v_samp; by++) {
          for (uint8_t bx = 0; bx < h_samp; bx++) {
            (void)block_idx;
            GIMG_Result r = jpeg_decode_block(
                &bs, dc_tbl, ac_tbl, block_zig, &dc_pred[comp_idx]);
            if (r != GIMG_OK) {
              if (getenv("GIMG_JPEG_DEBUG_BIT_POS") || getenv("GIMG_JPEG_DEBUG_BASELINE_FAIL")) {
                (void)fprintf(stderr,
                    "BASELINE_FAIL mcu=(%u,%u) comp=%u (comp_idx=%u) dc_tbl=%u ac_tbl=%u byte_off=%zu bit_off=%u\n",
                    (unsigned)mcu_x, (unsigned)mcu_y, (unsigned)s,
                    (unsigned)comp_idx, (unsigned)scan0->dc_tbl[s],
                    (unsigned)scan0->ac_tbl[s], (size_t)bs.byte_off, (unsigned)bs.bit_off);
                if (bs.byte_off < scan0->data_size) {
                  size_t n = (scan0->data_size - bs.byte_off) > 8u
                      ? 8u
                      : (scan0->data_size - bs.byte_off);
                  (void)fprintf(stderr, " next %zu bytes:", n);
                  for (size_t i = 0; i < n; i++)
                    (void)fprintf(stderr, " %02x",
                        (unsigned)scan0->data[bs.byte_off + i]);
                  (void)fprintf(stderr, "\n");
                }
                (void)fflush(stderr);
              }
              goto fail_decode;
            }
            if (getenv("GIMG_JPEG_DEBUG_BIT_POS") && mcu_x == 0 && mcu_y == 0 &&
                s == 0 && by == 0 && bx == 0) {
              (void)fprintf(stderr,
                  "DEC after block0: byte_off=%zu bit_off=%u (next byte(s):",
                  (size_t)bs.byte_off, (unsigned)bs.bit_off);
              for (size_t i = 0; i < 6u && bs.byte_off + i < scan0->data_size; i++)
                (void)fprintf(stderr, " %02x",
                    (unsigned)scan0->data[bs.byte_off + i]);
              (void)fprintf(stderr, ")\n");
              (void)fflush(stderr);
            }
            jpeg_dezigzag(block_zig, block_rz);
            jpeg_dequantise(block_rz, quant, block_q);
            jpeg_idct_8x8_islow(block_q, block_rz);

            if (mcu_x == 0 && mcu_y == 0) {
              const char * dump_dir = getenv("DUMP_JPEG_COMPONENTS");
              if (dump_dir && dump_dir[0] != '\0') {
                char path[1024];
                int n = -1;
                if (comp_idx == 0) {
                  unsigned block_no =
                      (unsigned)by * (unsigned)h_samp + (unsigned)bx;
                  n = snprintf(path, sizeof(path), "%s/Y_block%u.bin", dump_dir,
                      block_no);
                }
                else if (comp_idx == 1 && by == 0 && bx == 0) {
                  n = snprintf(
                      path, sizeof(path), "%s/Cb_block0.bin", dump_dir);
                }
                else if (comp_idx == 2 && by == 0 && bx == 0) {
                  n = snprintf(
                      path, sizeof(path), "%s/Cr_block0.bin", dump_dir);
                }
                if (n > 0 && (size_t)n < sizeof(path)) {
                  FILE * f = fopen(path, "wb");
                  if (f) {
                    fwrite(block_zig, 2, 64, f);
                    fwrite(quant, 2, 64, f);
                    for (int i = 0; i < 64; i++) {
                      int v = block_rz[i] + 128;
                      if (v < 0)
                        v = 0;
                      if (v > 255)
                        v = 255;
                      unsigned char b = (unsigned char)v;
                      fwrite(&b, 1, 1, f);
                    }
                    fclose(f);
                  }
                }
              }
            }

            uint32_t dst_x = mcu_x * (uint32_t)(8 * h_samp) + (uint32_t)bx * 8;
            uint32_t dst_y = mcu_y * (uint32_t)(8 * v_samp) + (uint32_t)by * 8;
            uint32_t comp_width = comp_w[comp_idx];
            uint32_t comp_height = comp_h[comp_idx];
            for (int dy = 0; dy < 8; dy++) {
              uint32_t y = dst_y + (uint32_t)dy;
              if (y >= comp_height) {
                break;
              }
              for (int dx = 0; dx < 8; dx++) {
                uint32_t x = dst_x + (uint32_t)dx;
                if (x >= comp_width) {
                  break;
                }
                int v = block_rz[dy * 8 + dx] + 128;
                if (v < 0) {
                  v = 0;
                }
                if (v > 255) {
                  v = 255;
                }
                comp_buf[comp_idx][y * comp_stride[comp_idx] + x] =
                    (unsigned char)v;
              }
            }
          }
        }
      }
    }
  }

  // Create output raster: grayscale or RGB.
  if (num_comp == 1) {
    r = gimg_raster_create_with_allocator(alloc, (uint32_t)width,
        (uint32_t)height, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED, NULL, 0,
        out_raster);
    if (r != GIMG_OK) {
      goto fail_decode;
    }
    unsigned char * pixels = (unsigned char *)gimg_raster_pixels(*out_raster);
    size_t stride = gimg_raster_stride_bytes(*out_raster);
    for (uint32_t y = 0; y < height; y++) {
      memcpy(
          pixels + y * stride, comp_buf[0] + y * comp_stride[0], (size_t)width);
    }
  }
  else if (num_comp == 3) {
    // Optional debug dump: Y, Cb, Cr component buffers (see
    // compare_ycbcr_components.py).
    {
      const char * dir = getenv("DUMP_JPEG_COMPONENTS");
      if (dir && dir[0] != '\0') {
        const char * names[] = {"Y.raw", "Cb.raw", "Cr.raw"};
        const uint32_t cw[] = {comp_w[0], comp_w[1], comp_w[2]};
        const uint32_t ch[] = {comp_h[0], comp_h[1], comp_h[2]};
        for (int i = 0; i < 3; i++) {
          char path[1024];
          int n = snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
          if (n > 0 && (size_t)n < sizeof(path)) {
            FILE * f = fopen(path, "wb");
            if (f) {
              uint32_t w = cw[i];
              uint32_t h = ch[i];
              fwrite(&w, 4, 1, f);
              fwrite(&h, 4, 1, f);
              for (uint32_t row = 0; row < h; row++) {
                fwrite(comp_buf[i] + row * comp_stride[i], 1, (size_t)w, f);
              }
              fclose(f);
            }
          }
        }
      }
    }
    // YCbCr -> RGB per Rec. ITU-R BT.601 (JFIF/JPEG).
    r = gimg_raster_create_with_allocator(alloc, (uint32_t)width,
        (uint32_t)height, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, NULL, 0,
        out_raster);
    if (r != GIMG_OK) {
      goto fail_decode;
    }
    unsigned char * pixels = (unsigned char *)gimg_raster_pixels(*out_raster);
    size_t stride = gimg_raster_stride_bytes(*out_raster);
    uint32_t cw1 = comp_w[1];
    uint32_t ch1 = comp_h[1];
    uint32_t cw2 = comp_w[2];
    uint32_t ch2 = comp_h[2];
    int use_fancy = (options && options->jpeg_chroma_upsampling ==
                            GIMG_JPEG_CHROMA_UPSAMPLE_FANCY) &&
        (width == cw1 * 2 && height == ch1 * 2 && width == cw2 * 2 &&
            height == ch2 * 2);
    for (uint32_t y = 0; y < height; y++) {
      uint32_t cy1 = (ch1 > 1 && height > 1) ? (y * ch1 / height) : 0;
      uint32_t cy2 = (ch2 > 1 && height > 1) ? (y * ch2 / height) : 0;
      for (uint32_t x = 0; x < width; x++) {
        uint32_t cx1 = (cw1 > 1 && width > 1) ? (x * cw1 / width) : 0;
        uint32_t cx2 = (cw2 > 1 && width > 1) ? (x * cw2 / width) : 0;
        int yy = comp_buf[0][y * comp_stride[0] + x];
        int cb, cr;
        if (use_fancy) {
          cb = jpeg_chroma_sample_fancy_2h2v(
              comp_buf[1], comp_stride[1], cw1, ch1, x, y);
          cr = jpeg_chroma_sample_fancy_2h2v(
              comp_buf[2], comp_stride[2], cw2, ch2, x, y);
        }
        else {
          cb = comp_buf[1][cy1 * comp_stride[1] + cx1];
          cr = comp_buf[2][cy2 * comp_stride[2] + cx2];
        }
        // YCbCr→RGB with scaled integer (SCALEBITS=16) per common practice.
        int cb_x = cb - 128;
        int cr_x = cr - 128;
        int r_val = yy + (int)((91881L * cr_x + 32768) >> 16);
        int g_val = yy +
            (int)(((int32_t)(-22554) * cb_x + (int32_t)(-46802) * cr_x +
                      32768) >>
                16);
        int b_val = yy + (int)((116130L * cb_x + 32768) >> 16);
        if (r_val < 0)
          r_val = 0;
        if (r_val > 255)
          r_val = 255;
        if (g_val < 0)
          g_val = 0;
        if (g_val > 255)
          g_val = 255;
        if (b_val < 0)
          b_val = 0;
        if (b_val > 255)
          b_val = 255;
        pixels[y * stride + x * 4 + 0] = (unsigned char)r_val;
        pixels[y * stride + x * 4 + 1] = (unsigned char)g_val;
        pixels[y * stride + x * 4 + 2] = (unsigned char)b_val;
        pixels[y * stride + x * 4 + 3] = 255;
      }
    }
  }
  else if (num_comp == 4) {
    uint32_t cw[4] = {comp_w[0], comp_w[1], comp_w[2], comp_w[3]};
    uint32_t ch[4] = {comp_h[0], comp_h[1], comp_h[2], comp_h[3]};
    // APP14 Adobe transform 2 = YCCK: Y,Cb,Cr,K -> convert to CMYK (raw CMYK
    // output format).
    if (state->adobe_transform == 2) {
      r = gimg_raster_create_with_allocator(alloc, (uint32_t)width,
          (uint32_t)height, &GIMG_PIXEL_CMYK8, GIMG_RASTER_OWNED, NULL, 0,
          out_raster);
      if (r != GIMG_OK) {
        goto fail_decode;
      }
      unsigned char * pixels = (unsigned char *)gimg_raster_pixels(*out_raster);
      size_t stride = gimg_raster_stride_bytes(*out_raster);
      for (uint32_t y = 0; y < height; y++) {
        uint32_t cy[4];
        for (int i = 0; i < 4; i++) {
          cy[i] =
              (ch[i] > 1 && height > 1) ? (y * (ch[i] - 1) / (height - 1)) : 0;
        }
        for (uint32_t x = 0; x < width; x++) {
          uint32_t cx[4];
          for (int i = 0; i < 4; i++) {
            cx[i] =
                (cw[i] > 1 && width > 1) ? (x * (cw[i] - 1) / (width - 1)) : 0;
          }
          int yy = comp_buf[0][cy[0] * comp_stride[0] + cx[0]];
          int cb_x = comp_buf[1][cy[1] * comp_stride[1] + cx[1]] - 128;
          int cr_x = comp_buf[2][cy[2] * comp_stride[2] + cx[2]] - 128;
          int k = comp_buf[3][cy[3] * comp_stride[3] + cx[3]];
          // YCCK -> CMYK: YCbCr -> RGB (BT.601 integer), then C=255-R, M=255-G,
          // Y=255-B, K=K.
          int r_val = yy + (int)((91881L * cr_x + 32768) >> 16);
          int g_val = yy +
              (int)(((int32_t)(-22554) * cb_x + (int32_t)(-46802) * cr_x +
                        32768) >>
                  16);
          int b_val = yy + (int)((116130L * cb_x + 32768) >> 16);
          if (r_val < 0)
            r_val = 0;
          if (r_val > 255)
            r_val = 255;
          if (g_val < 0)
            g_val = 0;
          if (g_val > 255)
            g_val = 255;
          if (b_val < 0)
            b_val = 0;
          if (b_val > 255)
            b_val = 255;
          pixels[y * stride + x * 4 + 0] = (unsigned char)(255 - r_val);
          pixels[y * stride + x * 4 + 1] = (unsigned char)(255 - g_val);
          pixels[y * stride + x * 4 + 2] = (unsigned char)(255 - b_val);
          pixels[y * stride + x * 4 + 3] = (unsigned char)k;
        }
      }
    }
    else {
      // Raw CMYK: output decompressed components unchanged to match libjpeg
      // (jdcolor.c null_convert for JCS_CMYK). Decode*PillowOracle uses
      // dump_jpeg_pixels_ref (libjpeg) as oracle; libjpeg does not invert.
      r = gimg_raster_create_with_allocator(alloc, (uint32_t)width,
          (uint32_t)height, &GIMG_PIXEL_CMYK8, GIMG_RASTER_OWNED, NULL, 0,
          out_raster);
      if (r != GIMG_OK) {
        goto fail_decode;
      }
      unsigned char * pixels = (unsigned char *)gimg_raster_pixels(*out_raster);
      size_t stride = gimg_raster_stride_bytes(*out_raster);
      for (uint32_t y = 0; y < height; y++) {
        uint32_t cy[4];
        for (int i = 0; i < 4; i++) {
          cy[i] =
              (ch[i] > 1 && height > 1) ? (y * (ch[i] - 1) / (height - 1)) : 0;
        }
        for (uint32_t x = 0; x < width; x++) {
          uint32_t cx[4];
          for (int i = 0; i < 4; i++) {
            cx[i] =
                (cw[i] > 1 && width > 1) ? (x * (cw[i] - 1) / (width - 1)) : 0;
          }
          pixels[y * stride + x * 4 + 0] =
              (unsigned char)comp_buf[0][cy[0] * comp_stride[0] + cx[0]];
          pixels[y * stride + x * 4 + 1] =
              (unsigned char)comp_buf[1][cy[1] * comp_stride[1] + cx[1]];
          pixels[y * stride + x * 4 + 2] =
              (unsigned char)comp_buf[2][cy[2] * comp_stride[2] + cx[2]];
          pixels[y * stride + x * 4 + 3] =
              (unsigned char)comp_buf[3][cy[3] * comp_stride[3] + cx[3]];
        }
      }
    }
  }
  else {
    r = GIMG_ERR_UNSUPPORTED;
    goto fail_decode;
  }

  // Attach ICC profile from APP2 to raster color info (single or
  // multi-segment).
  if (state->app2_icc && state->app2_icc_len > 0u) {
    GIMG_Color_Info color_info;
    gimg_color_info_default(&color_info);
    if (state->app2_icc_num_chunks > 0) {
      color_info.icc_bytes = state->app2_icc;
      color_info.icc_size = state->app2_icc_len;
    }
    else {
      color_info.icc_bytes = state->app2_icc + 14;
      color_info.icc_size = state->app2_icc_len - 14u;
    }
    (void)gimg_raster_set_color_info(*out_raster, &color_info);
  }

  for (uint8_t i = 0; i < num_comp; i++) {
    gimg_free(alloc, comp_buf[i]);
  }
  return GIMG_OK;

fail_decode:
  if (*out_raster) {
    gimg_raster_destroy(*out_raster);
    *out_raster = NULL;
  }
  for (uint8_t i = 0; i < num_comp; i++) {
    gimg_free(alloc, comp_buf[i]);
  }
  return r;
fail_comp:
  for (uint8_t i = 0; i < num_comp; i++) {
    gimg_free(alloc, comp_buf[i]);
  }
  return GIMG_ERR_FORMAT;
}

/** Progressive decode for 12- or 16-bit precision. Grayscale and YCbCr only. */
static GIMG_Result jpeg_decode_progressive_extended(
    const gimg_jpeg_doc_state_t * state, const GIMG_Decode_Options * options,
    GIMG_Raster ** out_raster) {
  const gimg_jpeg_sof_t * sof = &state->sof;
  uint8_t precision = sof->precision;
  if (precision != 12 && precision != 16) {
    return GIMG_ERR_UNSUPPORTED;
  }
  uint16_t width = sof->width;
  uint16_t height = sof->height;
  uint8_t num_comp = sof->num_components;
  if (num_comp != 1 && num_comp != 3) {
    return GIMG_ERR_UNSUPPORTED;
  }

  size_t pixel_count = 0;
  if (gimg_safe_pixel_count((uint32_t)width, (uint32_t)height, &pixel_count) !=
      GIMG_OK) {
    return GIMG_ERR_LIMIT;
  }
  const GIMG_Limits * limits =
      options && options->limits ? options->limits : NULL;
  if (limits && limits->max_decoded_pixels != 0 &&
      pixel_count > limits->max_decoded_pixels) {
    return GIMG_ERR_LIMIT;
  }

  uint8_t h_max = 0;
  uint8_t v_max = 0;
  for (uint8_t i = 0; i < num_comp; i++) {
    if (sof->h_samp[i] > h_max)
      h_max = sof->h_samp[i];
    if (sof->v_samp[i] > v_max)
      v_max = sof->v_samp[i];
  }
  if (h_max == 0 || v_max == 0) {
    return GIMG_ERR_FORMAT;
  }
  uint32_t mcu_w = (uint32_t)(8 * h_max);
  uint32_t mcu_h = (uint32_t)(8 * v_max);
  uint32_t mcu_per_row = (width + mcu_w - 1) / mcu_w;
  uint32_t mcu_per_col = (height + mcu_h - 1) / mcu_h;

  uint32_t denom_w = (uint32_t)(8 * h_max);
  uint32_t denom_h = (uint32_t)(8 * v_max);
  uint32_t comp_w[GIMG_JPEG_MAX_COMPONENTS];
  uint32_t comp_h[GIMG_JPEG_MAX_COMPONENTS];
  for (uint8_t i = 0; i < num_comp; i++) {
    uint32_t w = (uint32_t)width * (uint32_t)sof->h_samp[i];
    uint32_t h = (uint32_t)height * (uint32_t)sof->v_samp[i];
    comp_w[i] = 8 * ((w + denom_w - 1) / denom_w);
    comp_h[i] = 8 * ((h + denom_h - 1) / denom_h);
    if (comp_w[i] == 0)
      comp_w[i] = 8;
    if (comp_h[i] == 0)
      comp_h[i] = 8;
  }

  const GIMG_Allocator * alloc = state->allocator;
  alloc = gimg_alloc_or_default(alloc);

  size_t blocks_per_comp[GIMG_JPEG_MAX_COMPONENTS];
  int16_t * coef_blocks[GIMG_JPEG_MAX_COMPONENTS];
  memset(coef_blocks, 0, sizeof(coef_blocks));
  for (uint8_t i = 0; i < num_comp; i++) {
    size_t bw = (size_t)(comp_w[i] / 8);
    size_t bh = (size_t)(comp_h[i] / 8);
    if (!gimg_safe_mul_size(bw, bh, &blocks_per_comp[i])) {
      for (uint8_t j = 0; j < i; j++)
        gimg_free(alloc, coef_blocks[j]);
      return GIMG_ERR_LIMIT;
    }
    size_t coef_size = 0;
    if (!gimg_safe_mul_size(
            blocks_per_comp[i], 64 * sizeof(int16_t), &coef_size)) {
      for (uint8_t j = 0; j < i; j++)
        gimg_free(alloc, coef_blocks[j]);
      return GIMG_ERR_LIMIT;
    }
    coef_blocks[i] = (int16_t *)gimg_malloc(alloc, coef_size);
    if (!coef_blocks[i]) {
      for (uint8_t j = 0; j < i; j++)
        gimg_free(alloc, coef_blocks[j]);
      return GIMG_ERR_OOM;
    }
    memset(coef_blocks[i], 0, coef_size);
  }

  int16_t dc_pred[GIMG_JPEG_MAX_COMPONENTS];
  memset(dc_pred, 0, sizeof(dc_pred));

  for (unsigned scan_idx = 0; scan_idx < state->num_scans; scan_idx++) {
    const gimg_jpeg_scan_t * scan = &state->scans[scan_idx];
    if (!scan->data || scan->data_size == 0) {
      goto prog_ext_fail;
    }
    int is_dc = (scan->ss == 0 && scan->se == 0);
    gimg_jpeg_huff_table_t dc_tables[4];
    gimg_jpeg_huff_table_t ac_tables[4];
    gimg_jpeg_huff_table_t ac_refine_tables[4];
    memset(dc_tables, 0, sizeof(dc_tables));
    memset(ac_tables, 0, sizeof(ac_tables));
    memset(ac_refine_tables, 0, sizeof(ac_refine_tables));
    for (uint8_t c = 0; c < scan->comp_count; c++) {
      uint8_t dc_id = scan->dc_tbl[c];
      uint8_t ac_id = scan->ac_tbl[c];
      const unsigned char * dc_src =
          (scan->huff_dc[dc_id] && scan->huff_dc_len[dc_id] > 0)
          ? scan->huff_dc[dc_id]
          : state->huff_dc[dc_id];
      size_t dc_len = (scan->huff_dc[dc_id] && scan->huff_dc_len[dc_id] > 0)
          ? scan->huff_dc_len[dc_id]
          : state->huff_dc_len[dc_id];
      const unsigned char * ac_src;
      size_t ac_len;
      // AC table selection per T.81 B.2.4: table for a scan is the one most
      // recently defined before that scan's entropy-coded segment. Use default
      // (K.4) only when neither scan nor state has a table. For scan 1
      // AC-initial, use Pillow-compat 2-symbol (0,4)+EOB only when there is
      // no table at all; otherwise use scan or state table.
      if ((!scan->huff_ac[ac_id] || scan->huff_ac_len[ac_id] == 0) &&
          (int)scan->ah == 0 &&
          (!state->huff_ac[ac_id] || state->huff_ac_len[ac_id] == 0)) {
        ac_src = jpeg_default_ac_dht_payload(&ac_len);
      }
      // Scan 1 AC-initial: Pillow-compat 2-symbol only when no DHT exists;
      // otherwise use file's table (T.81 B.2.4).
      else if (scan_idx == 1 && (int)scan->ah == 0 &&
          (!scan->huff_ac[ac_id] || scan->huff_ac_len[ac_id] == 0) &&
          (!state->huff_ac[ac_id] || state->huff_ac_len[ac_id] == 0)) {
        jpeg_build_pillow_compat_ac_scan1_table(&ac_tables[ac_id]);
        ac_src = NULL;
        ac_len = 0;
      }
      else {
        ac_src =
            (scan->huff_ac[ac_id] && scan->huff_ac_len[ac_id] > 0)
            ? scan->huff_ac[ac_id]
            : state->huff_ac[ac_id];
        ac_len = (scan->huff_ac[ac_id] && scan->huff_ac_len[ac_id] > 0)
            ? scan->huff_ac_len[ac_id]
            : state->huff_ac_len[ac_id];
      }
      const unsigned char * ac_refine_src =
          (scan->huff_ac_refine[ac_id] && scan->huff_ac_refine_len[ac_id] > 0)
          ? scan->huff_ac_refine[ac_id]
          : state->huff_ac_refine[ac_id];
      size_t ac_refine_len =
          (scan->huff_ac_refine[ac_id] && scan->huff_ac_refine_len[ac_id] > 0)
          ? scan->huff_ac_refine_len[ac_id]
          : state->huff_ac_refine_len[ac_id];
      if (dc_id >= 4 || !dc_src || dc_len == 0 ||
          jpeg_build_huff_table(dc_src, dc_len, &dc_tables[dc_id]) != 0) {
        goto prog_ext_fail;
      }
      if (ac_id >= 4) {
        goto prog_ext_fail;
      }
      if (ac_src && ac_len > 0) {
        if (jpeg_build_huff_table(ac_src, ac_len, &ac_tables[ac_id]) != 0) {
          goto prog_ext_fail;
        }
      }
      else if (!ac_src && ac_len == 0 &&
          ac_tables[ac_id].num_values == 0) {
        goto prog_ext_fail;
      }
      if (ac_refine_src && ac_refine_len > 0) {
        (void)jpeg_build_huff_table(
            ac_refine_src, ac_refine_len, &ac_refine_tables[ac_id]);
      }
      else if ((int)scan->ah != 0) {
        (void)jpeg_build_huff_table(
            jpeg_default_ac_refine_dht, sizeof(jpeg_default_ac_refine_dht),
            &ac_refine_tables[ac_id]);
      }
    }
    /* 2-symbol AC refinement: decode from DHT only (T.81 Annex C); no value swap. */
    gimg_jpeg_bitstream_t bs;
    jpeg_bitstream_init(&bs, scan->data, scan->data_size);
    if (!is_dc && (int)scan->ah != 0) {
      bs.pad_at_eob = 1;
    }
    {
      const char * e = getenv("GIMG_JPEG_RECOVER_STUFF_ZERO");
      if (e && e[0] == '1') {
        bs.recover_stuff_zero = 1;
      }
    }
    uint16_t restart_interval = state->restart_interval;
    int ss = (int)scan->ss;
    int se = (int)scan->se;
    int ah = (int)scan->ah;
    int al = (int)scan->al;
    unsigned int eobrun = 0;

    for (uint32_t mcu_y = 0; mcu_y < mcu_per_col; mcu_y++) {
      for (uint32_t mcu_x = 0; mcu_x < mcu_per_row; mcu_x++) {
        if (restart_interval > 0) {
          uint32_t mcu_index = mcu_y * mcu_per_row + mcu_x;
          if (mcu_index > 0 && (mcu_index % (uint32_t)restart_interval) == 0) {
            memset(dc_pred, 0, sizeof(dc_pred));
            eobrun = 0;
          }
        }
        for (uint8_t s = 0; s < scan->comp_count; s++) {
          uint8_t comp_idx = 0;
          for (; comp_idx < num_comp; comp_idx++) {
            if (sof->comp_id[comp_idx] == scan->comp_id[s])
              break;
          }
          if (comp_idx >= num_comp) {
            goto prog_ext_fail;
          }
          uint8_t h_samp = sof->h_samp[comp_idx];
          uint8_t v_samp = sof->v_samp[comp_idx];
          size_t blocks_per_mcu_comp = (size_t)h_samp * (size_t)v_samp;
          size_t mcu_block_start =
              (size_t)(mcu_y * mcu_per_row + mcu_x) * blocks_per_mcu_comp;

          for (uint8_t by = 0; by < v_samp; by++) {
            for (uint8_t bx = 0; bx < h_samp; bx++) {
              size_t block_idx =
                  mcu_block_start + (size_t)by * (size_t)h_samp + (size_t)bx;
              int16_t * block = coef_blocks[comp_idx] + block_idx * 64;
              if (is_dc) {
                GIMG_Result r;
                if (ah == 0) {
                  r = jpeg_decode_block_progressive_dc(&bs,
                      &dc_tables[scan->dc_tbl[s]], block, &dc_pred[comp_idx],
                      al, NULL, NULL, 0);
                }
                else {
                  r = jpeg_decode_block_progressive_dc_refine(
                      &bs, block, &dc_pred[comp_idx], (unsigned int)al, NULL, 0);
                }
                if (r != GIMG_OK) {
                  goto prog_ext_fail;
                }
              }
              else {
                if (ah == 0) {
                  GIMG_Result r = jpeg_decode_block_progressive_ac_initial(
                      &bs, &ac_tables[scan->ac_tbl[s]], block, ss, se, al, 0, -1,
                      0u, &eobrun, 0);
                  if (r != GIMG_OK) {
                    goto prog_ext_fail;
                  }
                }
                else {
                  const gimg_jpeg_huff_table_t * ac_ref_tbl =
                      (ac_refine_tables[scan->ac_tbl[s]].num_values > 0)
                      ? &ac_refine_tables[scan->ac_tbl[s]]
                      : &ac_tables[scan->ac_tbl[s]];
                  GIMG_Result r = jpeg_decode_block_progressive_ac_refine(
                      &bs, ac_ref_tbl, block, ss, se, al, 0, -1, 0, 0);
                  if (r != GIMG_OK) {
                    goto prog_ext_fail;
                  }
                }
              }
            }
          }
        }
      }
    }
  }

  int idct_scale = (precision == 12) ? 4096 : 65536;
  int level_shift = (precision == 12) ? 2048 : 32768;
  int max_val = (precision == 12) ? 4095 : 65535;

  size_t comp_stride_el[GIMG_JPEG_MAX_COMPONENTS];
  size_t comp_size[GIMG_JPEG_MAX_COMPONENTS];
  uint16_t * comp_buf[GIMG_JPEG_MAX_COMPONENTS];
  for (uint8_t i = 0; i < num_comp; i++) {
    comp_stride_el[i] = (size_t)comp_w[i];
    if (!gimg_safe_mul_size(
            comp_stride_el[i], (size_t)comp_h[i], &comp_size[i]) ||
        !gimg_safe_mul_size(comp_size[i], sizeof(uint16_t), &comp_size[i])) {
      for (uint8_t j = 0; j < i; j++)
        gimg_free(alloc, comp_buf[j]);
      goto prog_ext_fail;
    }
    comp_buf[i] = (uint16_t *)gimg_malloc(alloc, comp_size[i]);
    if (!comp_buf[i]) {
      for (uint8_t j = 0; j < i; j++)
        gimg_free(alloc, comp_buf[j]);
      goto prog_ext_fail;
    }
    memset(comp_buf[i], 0, comp_size[i]);
  }

  int16_t block_rz[64];
  int32_t block_q[64];
  int32_t block_idct[64];
  for (uint8_t comp_idx = 0; comp_idx < num_comp; comp_idx++) {
    uint8_t qid = sof->quant_tbl_id[comp_idx];
    if (qid >= GIMG_JPEG_MAX_QUANT_TABLES || !state->quant_tbl_present[qid]) {
      goto prog_ext_fail_buf;
    }
    const uint16_t * quant = state->quant_tbl[qid];
    uint32_t blocks_w = comp_w[comp_idx] / 8;
    uint32_t blocks_h = comp_h[comp_idx] / 8;
    for (uint32_t by = 0; by < blocks_h; by++) {
      for (uint32_t bx = 0; bx < blocks_w; bx++) {
        size_t block_idx = (size_t)by * (size_t)blocks_w + (size_t)bx;
        const int16_t * block_zig = coef_blocks[comp_idx] + block_idx * 64;
        jpeg_dezigzag(block_zig, block_rz);
        jpeg_dequantise_32(block_rz, quant, block_q);
        jpeg_idct_8x8_32(block_q, block_idct, idct_scale);
        uint32_t dst_x = bx * 8;
        uint32_t dst_y = by * 8;
        for (int dy = 0; dy < 8; dy++) {
          uint32_t y = dst_y + (uint32_t)dy;
          if (y >= comp_h[comp_idx])
            break;
          for (int dx = 0; dx < 8; dx++) {
            uint32_t x = dst_x + (uint32_t)dx;
            if (x >= comp_w[comp_idx])
              break;
            int32_t v = block_idct[dy * 8 + dx] + level_shift;
            if (v < 0)
              v = 0;
            if (v > max_val)
              v = max_val;
            if (precision == 12) {
              v = v << 4;
            }
            comp_buf[comp_idx][y * comp_stride_el[comp_idx] + x] = (uint16_t)v;
          }
        }
      }
    }
  }

  for (uint8_t i = 0; i < num_comp; i++) {
    gimg_free(alloc, coef_blocks[i]);
  }

  GIMG_Result r;
  if (num_comp == 1) {
    r = gimg_raster_create_with_allocator(alloc, (uint32_t)width,
        (uint32_t)height, &GIMG_PIXEL_GRAY16, GIMG_RASTER_OWNED, NULL, 0,
        out_raster);
    if (r != GIMG_OK) {
      goto prog_ext_fail_buf;
    }
    uint16_t * pixels = (uint16_t *)gimg_raster_pixels(*out_raster);
    size_t stride_el = gimg_raster_stride_bytes(*out_raster) / 2;
    for (uint32_t y = 0; y < height; y++) {
      for (uint32_t x = 0; x < width; x++) {
        pixels[y * stride_el + x] = comp_buf[0][y * comp_stride_el[0] + x];
      }
    }
  }
  else {
    r = gimg_raster_create_with_allocator(alloc, (uint32_t)width,
        (uint32_t)height, &GIMG_PIXEL_RGBA16, GIMG_RASTER_OWNED, NULL, 0,
        out_raster);
    if (r != GIMG_OK) {
      goto prog_ext_fail_buf;
    }
    uint16_t * pixels = (uint16_t *)gimg_raster_pixels(*out_raster);
    size_t stride_el = gimg_raster_stride_bytes(*out_raster) / 8;
    uint32_t cw1 = comp_w[1];
    uint32_t ch1 = comp_h[1];
    uint32_t cw2 = comp_w[2];
    uint32_t ch2 = comp_h[2];
    int mid = level_shift;
    for (uint32_t y = 0; y < height; y++) {
      uint32_t cy1 = (ch1 > 1 && height > 1) ? (y * ch1 / height) : 0;
      uint32_t cy2 = (ch2 > 1 && height > 1) ? (y * ch2 / height) : 0;
      for (uint32_t x = 0; x < width; x++) {
        uint32_t cx1 = (cw1 > 1 && width > 1) ? (x * cw1 / width) : 0;
        uint32_t cx2 = (cw2 > 1 && width > 1) ? (x * cw2 / width) : 0;
        int yy, cb, cr;
        if (precision == 12) {
          yy = (comp_buf[0][y * comp_stride_el[0] + x] >> 4) - mid;
          cb = (comp_buf[1][cy1 * comp_stride_el[1] + cx1] >> 4) - mid;
          cr = (comp_buf[2][cy2 * comp_stride_el[2] + cx2] >> 4) - mid;
        }
        else {
          yy = (int)comp_buf[0][y * comp_stride_el[0] + x] - mid;
          cb = (int)comp_buf[1][cy1 * comp_stride_el[1] + cx1] - mid;
          cr = (int)comp_buf[2][cy2 * comp_stride_el[2] + cx2] - mid;
        }
        int r_val = yy + (int)(1.40200 * cr + 0.5);
        int g_val = yy - (int)(0.34414 * cb + 0.71414 * cr + 0.5);
        int b_val = yy + (int)(1.77200 * cb + 0.5);
        int out_max = (precision == 12) ? 65520 : 65535;
        if (precision == 12) {
          r_val <<= 4;
          g_val <<= 4;
          b_val <<= 4;
        }
        if (r_val < 0)
          r_val = 0;
        if (r_val > out_max)
          r_val = out_max;
        if (g_val < 0)
          g_val = 0;
        if (g_val > out_max)
          g_val = out_max;
        if (b_val < 0)
          b_val = 0;
        if (b_val > out_max)
          b_val = out_max;
        pixels[y * stride_el + x * 4 + 0] = (uint16_t)r_val;
        pixels[y * stride_el + x * 4 + 1] = (uint16_t)g_val;
        pixels[y * stride_el + x * 4 + 2] = (uint16_t)b_val;
        pixels[y * stride_el + x * 4 + 3] = (uint16_t)65535;
      }
    }
  }

  if (state->app2_icc && state->app2_icc_len > 0u) {
    GIMG_Color_Info color_info;
    gimg_color_info_default(&color_info);
    if (state->app2_icc_num_chunks > 0) {
      color_info.icc_bytes = state->app2_icc;
      color_info.icc_size = state->app2_icc_len;
    }
    else {
      color_info.icc_bytes = state->app2_icc + 14;
      color_info.icc_size = state->app2_icc_len - 14u;
    }
    (void)gimg_raster_set_color_info(*out_raster, &color_info);
  }
  for (uint8_t i = 0; i < num_comp; i++) {
    gimg_free(alloc, comp_buf[i]);
  }
  return GIMG_OK;

prog_ext_fail_buf:
  for (uint8_t i = 0; i < num_comp; i++) {
    gimg_free(alloc, comp_buf[i]);
  }
prog_ext_fail:
  for (uint8_t i = 0; i < num_comp; i++) {
    gimg_free(alloc, coef_blocks[i]);
  }
  return GIMG_ERR_CORRUPT;
}

GIMG_Result gimg_jpeg_decode_progressive(const gimg_jpeg_doc_state_t * state,
    const GIMG_Decode_Options * options, GIMG_Raster ** out_raster) {
  if (!state || !out_raster) {
    return GIMG_ERR_INTERNAL;
  }
  *out_raster = NULL;
  if (!state->is_progressive || state->num_scans == 0) {
    return GIMG_ERR_CORRUPT;
  }

  const gimg_jpeg_sof_t * sof = &state->sof;
  if (sof->precision != 8) {
    return jpeg_decode_progressive_extended(state, options, out_raster);
  }

  uint16_t width = sof->width;
  uint16_t height = sof->height;
  uint8_t num_comp = sof->num_components;

  size_t pixel_count = 0;
  if (gimg_safe_pixel_count((uint32_t)width, (uint32_t)height, &pixel_count) !=
      GIMG_OK) {
    return GIMG_ERR_LIMIT;
  }
  const GIMG_Limits * limits =
      options && options->limits ? options->limits : NULL;
  if (limits && limits->max_decoded_pixels != 0 &&
      pixel_count > limits->max_decoded_pixels) {
    return GIMG_ERR_LIMIT;
  }

  uint8_t h_max = 0;
  uint8_t v_max = 0;
  for (uint8_t i = 0; i < num_comp; i++) {
    if (sof->h_samp[i] > h_max) {
      h_max = sof->h_samp[i];
    }
    if (sof->v_samp[i] > v_max) {
      v_max = sof->v_samp[i];
    }
  }
  if (h_max == 0 || v_max == 0) {
    return GIMG_ERR_FORMAT;
  }
  uint32_t mcu_w = (uint32_t)(8 * h_max);
  uint32_t mcu_h = (uint32_t)(8 * v_max);
  uint32_t mcu_per_row = (width + mcu_w - 1) / mcu_w;
  uint32_t mcu_per_col = (height + mcu_h - 1) / mcu_h;

  uint32_t comp_w[GIMG_JPEG_MAX_COMPONENTS];
  uint32_t comp_h[GIMG_JPEG_MAX_COMPONENTS];
  uint32_t denom_w = (uint32_t)(8 * h_max);
  uint32_t denom_h = (uint32_t)(8 * v_max);
  for (uint8_t i = 0; i < num_comp; i++) {
    uint32_t w = (uint32_t)width * (uint32_t)sof->h_samp[i];
    uint32_t h = (uint32_t)height * (uint32_t)sof->v_samp[i];
    comp_w[i] = 8 * ((w + denom_w - 1) / denom_w);
    comp_h[i] = 8 * ((h + denom_h - 1) / denom_h);
    if (comp_w[i] == 0) {
      comp_w[i] = 8;
    }
    if (comp_h[i] == 0) {
      comp_h[i] = 8;
    }
  }

  const GIMG_Allocator * alloc = state->allocator;
  alloc = gimg_alloc_or_default(alloc);

  // Coefficient buffers: one 64-int16 per block per component (zigzag order).
  size_t blocks_per_comp[GIMG_JPEG_MAX_COMPONENTS];
  int16_t * coef_blocks[GIMG_JPEG_MAX_COMPONENTS];
  memset(coef_blocks, 0, sizeof(coef_blocks));
  for (uint8_t i = 0; i < num_comp; i++) {
    size_t bw = (size_t)(comp_w[i] / 8);
    size_t bh = (size_t)(comp_h[i] / 8);
    if (!gimg_safe_mul_size(bw, bh, &blocks_per_comp[i])) {
      for (uint8_t j = 0; j < i; j++) {
        gimg_free(alloc, coef_blocks[j]);
      }
      return GIMG_ERR_LIMIT;
    }
    size_t coef_size = 0;
    if (!gimg_safe_mul_size(
            blocks_per_comp[i], 64 * sizeof(int16_t), &coef_size)) {
      for (uint8_t j = 0; j < i; j++) {
        gimg_free(alloc, coef_blocks[j]);
      }
      return GIMG_ERR_LIMIT;
    }
    coef_blocks[i] = (int16_t *)gimg_malloc(alloc, coef_size);
    if (!coef_blocks[i]) {
      for (uint8_t j = 0; j < i; j++) {
        gimg_free(alloc, coef_blocks[j]);
      }
      return GIMG_ERR_OOM;
    }
    memset(coef_blocks[i], 0, coef_size);
  }

  int16_t dc_pred[GIMG_JPEG_MAX_COMPONENTS];
  memset(dc_pred, 0, sizeof(dc_pred));

  // Debug: failure location (scan index and where). Set
  // GIMG_JPEG_PROGRESSIVE_DEBUG=1 to log.
  static unsigned s_prog_fail_scan = 0;
  static const char * s_prog_fail_where = NULL;
  const char * prog_debug_env = getenv("GIMG_JPEG_PROGRESSIVE_DEBUG");
  const int prog_debug = (prog_debug_env && prog_debug_env[0] == '1');

  // Bisect/debug: limit how many scans to run (rest stay zero).
  unsigned max_scans = 0;
  const char * max_scans_env = getenv("GIMG_JPEG_PROGRESSIVE_MAX_SCANS");
  if (max_scans_env && max_scans_env[0] != '\0') {
    int val = atoi(max_scans_env);
    if (val > 0 && (unsigned)val <= state->num_scans) {
      max_scans = (unsigned)val;
    }
  }

  const char * dump_dc_env = getenv("DUMP_JPEG_DC");
  const int dump_dc = (dump_dc_env && dump_dc_env[0] == '1');
  const char * trace_ac_sym_env = getenv("TRACE_JPEG_AC_SYMBOLS");
  const int trace_ac_sym = (trace_ac_sym_env && trace_ac_sym_env[0] == '1');
  const char * trace_ac_sym_scan_env = getenv("TRACE_JPEG_AC_SYMBOLS_SCAN");
  /* Comma-separated list of scan indices to trace (e.g. "1,4"); empty = first AC-initial only. */
  #define GIMG_JPEG_MAX_TRACE_AC_SCANS 16
  static unsigned s_trace_ac_sym_scans[GIMG_JPEG_MAX_TRACE_AC_SCANS];
  static size_t s_num_trace_ac_sym_scans = 0;
  if (trace_ac_sym && trace_ac_sym_scan_env && trace_ac_sym_scan_env[0] != '\0') {
    s_num_trace_ac_sym_scans = 0;
    const char * p = trace_ac_sym_scan_env;
    while (s_num_trace_ac_sym_scans < GIMG_JPEG_MAX_TRACE_AC_SCANS) {
      unsigned v = 0;
      while (*p >= '0' && *p <= '9') {
        v = v * 10u + (unsigned)(*p - '0');
        p++;
      }
      s_trace_ac_sym_scans[s_num_trace_ac_sym_scans++] = v;
      if (*p != ',')
        break;
      p++;
    }
  } else if (trace_ac_sym) {
    s_num_trace_ac_sym_scans = 0;
  }
  const char * trace_dc_sym_env = getenv("TRACE_JPEG_DC_SYMBOLS");
  const int trace_dc_sym = (trace_dc_sym_env && trace_dc_sym_env[0] == '1');
  const char * trace_dc_sym_scan_env = getenv("TRACE_JPEG_DC_SYMBOLS_SCAN");
  unsigned trace_dc_sym_scan = (unsigned)-1;
  if (trace_dc_sym_scan_env && trace_dc_sym_scan_env[0] != '\0') {
    trace_dc_sym_scan = (unsigned)atoi(trace_dc_sym_scan_env);
  }
  const char * trace_dc_refine_env = getenv("TRACE_JPEG_DC_REFINE");
  const int trace_dc_refine =
      (trace_dc_refine_env && trace_dc_refine_env[0] == '1');
  const char * trace_ac_initial_bitstream_env =
      getenv("TRACE_JPEG_AC_INITIAL_BITSTREAM");
  const int trace_ac_initial_bitstream =
      (trace_ac_initial_bitstream_env && trace_ac_initial_bitstream_env[0] == '1');
  const char * trace_all_env = getenv("GIMG_JPEG_TRACE_ALL");
  const int trace_all = (trace_all_env && trace_all_env[0] == '1');
  static unsigned s_first_ac_initial_scan = (unsigned)-1;
  if (trace_ac_sym) {
    s_first_ac_initial_scan = (unsigned)-1;
  }

  // Process each scan.
  for (unsigned scan_idx = 0; scan_idx < state->num_scans; scan_idx++) {
    if (max_scans != 0 && scan_idx >= max_scans) {
      break;
    }
    s_prog_fail_scan = scan_idx;
    s_prog_fail_where = NULL;
    const gimg_jpeg_scan_t * scan = &state->scans[scan_idx];
    if (prog_debug) {
      size_t blocks_this_scan = 0;
      for (uint8_t s = 0; s < scan->comp_count; s++) {
        uint8_t comp_idx = 0;
        for (; comp_idx < num_comp; comp_idx++) {
          if (sof->comp_id[comp_idx] == scan->comp_id[s]) {
            break;
          }
        }
        if (comp_idx < num_comp) {
          blocks_this_scan +=
              (size_t)sof->h_samp[comp_idx] * (size_t)sof->v_samp[comp_idx];
        }
      }
      blocks_this_scan *= (size_t)mcu_per_row * (size_t)mcu_per_col;
      (void)fprintf(stderr,
          "progressive scan %u data_size=%zu Ss=%u Se=%u Ah=%u Al=%u is_dc=%d "
          "blocks=%zu mcu=%ux%u\n",
          (unsigned)scan_idx, (size_t)scan->data_size, (unsigned)scan->ss,
          (unsigned)scan->se, (unsigned)scan->ah, (unsigned)scan->al,
          (scan->ss == 0 && scan->se == 0) ? 1 : 0, blocks_this_scan,
          (unsigned)mcu_per_row, (unsigned)mcu_per_col);
    }
    if (!scan->data || scan->data_size == 0) {
      s_prog_fail_where = "no_scan_data";
      if (prog_debug) {
        (void)fprintf(stderr, "progressive decode fail: scan %u %s\n",
            (unsigned)scan_idx, s_prog_fail_where);
      }
      goto fail_prog;  // no scan data
    }

    // Debug: dump scan N bytes (loader output) to compare with script.
    {
      const char * dump_scan0 = getenv("DUMP_JPEG_SCAN0_BYTES");
      const char * dump_scan1 = getenv("DUMP_JPEG_SCAN1_BYTES");
      const char * dump_scan2 = getenv("DUMP_JPEG_SCAN2_BYTES");
      const char * dump_scan3 = getenv("DUMP_JPEG_SCAN3_BYTES");
      const char * dump_scan4 = getenv("DUMP_JPEG_SCAN4_BYTES");
      const char * dump_scan5 = getenv("DUMP_JPEG_SCAN5_BYTES");
      int dump_this = (scan_idx == 0 && dump_scan0 && dump_scan0[0] == '1') ||
          (scan_idx == 1 && dump_scan1 && dump_scan1[0] == '1') ||
          (scan_idx == 2 && dump_scan2 && dump_scan2[0] == '1') ||
          (scan_idx == 3 && dump_scan3 && dump_scan3[0] == '1') ||
          (scan_idx == 4 && dump_scan4 && dump_scan4[0] == '1') ||
          (scan_idx == 5 && dump_scan5 && dump_scan5[0] == '1');
      if (dump_this) {
        (void)fprintf(stderr, "SCAN%u_BYTES %zu ", (unsigned)scan_idx,
            (size_t)scan->data_size);
        size_t n = (size_t)scan->data_size;
        if (n > 512u) {
          n = 512u;
        }
        for (size_t i = 0; i < n; i++) {
          (void)fprintf(stderr, "%02x", (unsigned)scan->data[i]);
        }
        (void)fprintf(stderr, "\n");
        (void)fflush(stderr);
      }
    }
    // Debug: AC table size per scan when DUMP_JPEG_AC_TABLE=1 (which DHT used).
    if (getenv("DUMP_JPEG_AC_TABLE") && getenv("DUMP_JPEG_AC_TABLE")[0] == '1' &&
        (scan->ss != 0 || scan->se != 0)) {
      size_t alen = (scan->huff_ac[0] && scan->huff_ac_len[0] > 0)
          ? scan->huff_ac_len[0] : state->huff_ac_len[0];
      (void)fprintf(stderr, "DUMP_JPEG_AC_TABLE scan%u snapshot_len=%zu\n",
          (unsigned)scan_idx, alen);
      (void)fflush(stderr);
    }
    // Debug: first bytes of each scan (to verify loader vs file).
    if (getenv("DUMP_JPEG_SCAN_DATA_HEAD") && getenv("DUMP_JPEG_SCAN_DATA_HEAD")[0] == '1') {
      size_t n = (size_t)scan->data_size;
      if (n > 4u) {
        n = 4u;
      }
      (void)fprintf(stderr, "DUMP_JPEG_SCAN_DATA_HEAD scan%u size=%zu first:",
          (unsigned)scan_idx, (size_t)scan->data_size);
      for (size_t i = 0; i < n; i++) {
        (void)fprintf(stderr, " %02x", (unsigned)scan->data[i]);
      }
      (void)fprintf(stderr, "\n");
      (void)fflush(stderr);
    }

    int is_dc = (scan->ss == 0 && scan->se == 0);
    gimg_jpeg_huff_table_t dc_tables[4];
    gimg_jpeg_huff_table_t ac_tables[4];
    gimg_jpeg_huff_table_t ac_refine_tables[4];
    memset(dc_tables, 0, sizeof(dc_tables));
    memset(ac_tables, 0, sizeof(ac_tables));
    memset(ac_refine_tables, 0, sizeof(ac_refine_tables));
    for (uint8_t c = 0; c < scan->comp_count; c++) {
      uint8_t dc_id = scan->dc_tbl[c];
      uint8_t ac_id = scan->ac_tbl[c];
      const unsigned char * dc_src =
          (scan->huff_dc[dc_id] && scan->huff_dc_len[dc_id] > 0)
          ? scan->huff_dc[dc_id]
          : state->huff_dc[dc_id];
      size_t dc_len = (scan->huff_dc[dc_id] && scan->huff_dc_len[dc_id] > 0)
          ? scan->huff_dc_len[dc_id]
          : state->huff_dc_len[dc_id];
      const unsigned char * ac_src;
      size_t ac_len;
      // AC table selection per T.81 B.2.4: table for a scan is the one most
      // recently defined before that scan's entropy-coded segment. Use default
      // (K.4) only when neither scan nor state has a table. For scan 1
      // AC-initial, use Pillow-compat 2-symbol (0,4)+EOB only when there is
      // no table at all; otherwise use scan or state table.
      if ((!scan->huff_ac[ac_id] || scan->huff_ac_len[ac_id] == 0) &&
          (int)scan->ah == 0 &&
          (!state->huff_ac[ac_id] || state->huff_ac_len[ac_id] == 0)) {
        ac_src = jpeg_default_ac_dht_payload(&ac_len);
      }
      // Scan 1 AC-initial: Pillow-compat 2-symbol only when no DHT exists;
      // otherwise use file's table (T.81 B.2.4).
      else if (scan_idx == 1 && (int)scan->ah == 0 &&
          (!scan->huff_ac[ac_id] || scan->huff_ac_len[ac_id] == 0) &&
          (!state->huff_ac[ac_id] || state->huff_ac_len[ac_id] == 0)) {
        jpeg_build_pillow_compat_ac_scan1_table(&ac_tables[ac_id]);
        ac_src = NULL;
        ac_len = 0;
      }
      else {
        ac_src =
            (scan->huff_ac[ac_id] && scan->huff_ac_len[ac_id] > 0)
            ? scan->huff_ac[ac_id]
            : state->huff_ac[ac_id];
        ac_len = (scan->huff_ac[ac_id] && scan->huff_ac_len[ac_id] > 0)
            ? scan->huff_ac_len[ac_id]
            : state->huff_ac_len[ac_id];
      }
      if (getenv("GIMG_JPEG_DEBUG_AC_SRC") && getenv("GIMG_JPEG_DEBUG_AC_SRC")[0] == '1' &&
          !is_dc && (int)scan->ah == 0) {
        int from_scan = (scan->huff_ac[ac_id] && scan->huff_ac_len[ac_id] > 0) ? 1 : 0;
        (void)fprintf(stderr, "DEBUG_AC_SRC scan%u ac_id=%u from_scan=%d ac_len=%zu\n",
            (unsigned)scan_idx, (unsigned)ac_id, from_scan, (size_t)ac_len);
        (void)fflush(stderr);
      }
      const unsigned char * ac_refine_src =
          (scan->huff_ac_refine[ac_id] && scan->huff_ac_refine_len[ac_id] > 0)
          ? scan->huff_ac_refine[ac_id]
          : state->huff_ac_refine[ac_id];
      size_t ac_refine_len =
          (scan->huff_ac_refine[ac_id] && scan->huff_ac_refine_len[ac_id] > 0)
          ? scan->huff_ac_refine_len[ac_id]
          : state->huff_ac_refine_len[ac_id];
      if (dc_id >= 4 || !dc_src || dc_len == 0 ||
          jpeg_build_huff_table(dc_src, dc_len, &dc_tables[dc_id]) != 0) {
        s_prog_fail_where = "dc_table";
        if (prog_debug) {
          (void)fprintf(stderr,
              "progressive decode fail: scan %u %s (dc_id=%u)\n",
              (unsigned)scan_idx, s_prog_fail_where, (unsigned)dc_id);
        }
        goto fail_prog;  // missing or invalid DC table for scan
      }
      if (ac_id >= 4) {
        s_prog_fail_where = "ac_table";
        if (prog_debug) {
          (void)fprintf(stderr,
              "progressive decode fail: scan %u %s (ac_id=%u)\n",
              (unsigned)scan_idx, s_prog_fail_where, (unsigned)ac_id);
        }
        goto fail_prog;
      }
      if (ac_src && ac_len > 0) {
        if (jpeg_build_huff_table(ac_src, ac_len, &ac_tables[ac_id]) != 0) {
          s_prog_fail_where = "ac_table";
          if (prog_debug) {
            (void)fprintf(stderr,
                "progressive decode fail: scan %u %s (ac_id=%u)\n",
                (unsigned)scan_idx, s_prog_fail_where, (unsigned)ac_id);
          }
          goto fail_prog;
        }
      }
      else if (!ac_src && ac_len == 0 &&
          ac_tables[ac_id].num_values == 0 &&
          (int)scan->ah == 0) {
        // For AC refinement scans (Ah>0), it's valid to have only the 17-symbol
        // refinement table (Th) defined (T.81 Annex G.1.2.2, Table K.6 style),
        // with no separate "initial" AC table for this ac_id. In that case we
        // rely solely on ac_refine_tables[ac_id] and must not treat the missing
        // ac_tables[ac_id] as corruption.
        s_prog_fail_where = "ac_table";
        if (prog_debug) {
          (void)fprintf(stderr,
              "progressive decode fail: scan %u %s (ac_id=%u)\n",
              (unsigned)scan_idx, s_prog_fail_where, (unsigned)ac_id);
        }
        goto fail_prog;
      }
      if (ac_refine_src && ac_refine_len > 0) {
        (void)jpeg_build_huff_table(
            ac_refine_src, ac_refine_len, &ac_refine_tables[ac_id]);
      }
      else if ((int)scan->ah != 0) {
        /* AC refinement scan but no 17-symbol DHT in file: use T.81 Table K.6
         * default so we do not fall back to the regular AC table (wrong codes). */
        (void)jpeg_build_huff_table(
            jpeg_default_ac_refine_dht, sizeof(jpeg_default_ac_refine_dht),
            &ac_refine_tables[ac_id]);
      }
    }
    /* 2-symbol AC refinement: decode strictly from the DHT (T.81 Annex C).
     * No value swap or code-length override. First value gets first code at its
     * length, second value gets first code at its length, per canonical assignment. */
    if (getenv("GIMG_JPEG_DEBUG_AC_REFINE_TABLE") != NULL) {
      for (uint8_t c = 0; c < scan->comp_count; c++) {
        uint8_t ac_id = scan->ac_tbl[c];
        gimg_jpeg_huff_table_t * rt = &ac_refine_tables[ac_id];
        if (rt->num_values == 2) {
          uint8_t va = rt->values[rt->base_index[1]];
          uint8_t vb = (rt->base_index[1] != rt->base_index[2])
                           ? rt->values[rt->base_index[2]]
                           : rt->values[1];
          (void)fprintf(stderr, "AC_REFINE_TABLE scan%u ac_id=%u base_index[1]=%u [2]=%u va=0x%02x vb=0x%02x\n",
              (unsigned)scan_idx, (unsigned)ac_id,
              (unsigned)rt->base_index[1], (unsigned)rt->base_index[2],
              (unsigned)va, (unsigned)vb);
          (void)fflush(stderr);
        }
      }
    }
    if (!is_dc && getenv("DUMP_JPEG_AC_TABLE") && getenv("DUMP_JPEG_AC_TABLE")[0] == '1') {
      (void)fprintf(stderr, "DUMP_JPEG_AC_TABLE scan%u Ta=0 num_values=%d\n",
          (unsigned)scan_idx, ac_tables[0].num_values);
      (void)fflush(stderr);
    }
    /* When GIMG_JPEG_TRACE_ALL=1, dump AC refinement Huffman table for this scan
     * so trace can be compared to ref (LIBJPEG_DEBUG_AC_TABLE). */
    if (trace_all && !is_dc && (int)scan->ah != 0) {
      for (uint8_t c = 0; c < scan->comp_count; c++) {
        const gimg_jpeg_huff_table_t * at =
            (ac_refine_tables[scan->ac_tbl[c]].num_values > 0)
                ? &ac_refine_tables[scan->ac_tbl[c]]
                : &ac_tables[scan->ac_tbl[c]];
        (void)fprintf(stderr, "OUR_AC_REFINE_TABLE scan%u comp=%u Ta=%u num_values=%d\n",
            (unsigned)scan_idx, (unsigned)c, (unsigned)scan->ac_tbl[c], at->num_values);
        for (int len = 1; len <= 16; len++) {
          uint16_t mn = at->min_code[len];
          uint16_t mx = at->max_code[len];
          int count = (mx >= mn) ? (int)(mx - mn + 1) : 0;
          if (count > 0) {
            (void)fprintf(stderr, "  len%2d min=%u max=%u base=%u",
                len, (unsigned)mn, (unsigned)mx, (unsigned)at->base_index[len]);
            for (int i = 0; i < count && i < 20; i++) {
              (void)fprintf(stderr, " v[%d]=0x%02x",
                  i, (unsigned)at->values[at->base_index[len] + i]);
            }
            if (count > 20) {
              (void)fprintf(stderr, " ...");
            }
            (void)fprintf(stderr, "\n");
          }
        }
        /* DHT-style one-line dump for direct diff with REF_AC_REFINE_TABLE_DHT. */
        (void)fprintf(stderr, "OUR_AC_REFINE_TABLE_DHT scan=%u tbl=%u bits:",
            (unsigned)scan_idx, (unsigned)scan->ac_tbl[c]);
        for (int len = 1; len <= 16; len++) {
          uint16_t mn = at->min_code[len];
          uint16_t mx = at->max_code[len];
          int count = (mx >= mn) ? (int)(mx - mn + 1) : 0;
          (void)fprintf(stderr, " %d", count);
        }
        (void)fprintf(stderr, "\nOUR_AC_REFINE_TABLE_DHT scan=%u tbl=%u huffval:",
            (unsigned)scan_idx, (unsigned)scan->ac_tbl[c]);
        int idx = 0;
        for (int len = 1; len <= 16 && idx < at->num_values; len++) {
          uint16_t mn = at->min_code[len];
          uint16_t mx = at->max_code[len];
          int count = (mx >= mn) ? (int)(mx - mn + 1) : 0;
          for (int i = 0; i < count && idx < at->num_values && idx < 32; i++) {
            (void)fprintf(stderr, " 0x%02x",
                (unsigned)at->values[at->base_index[len] + i]);
            idx++;
          }
        }
        if (at->num_values > 32) {
          (void)fprintf(stderr, " ...");
        }
        (void)fprintf(stderr, "\n");
        (void)fflush(stderr);
      }
    }
    gimg_jpeg_bitstream_t bs;
    jpeg_bitstream_init(&bs, scan->data, scan->data_size);
    if (prog_debug && scan->data_size > 0) {
      (void)fprintf(stderr, " scan%u data_size=%zu first_bytes:",
          (unsigned)scan_idx, (size_t)scan->data_size);
      for (size_t i = 0; i < (size_t)scan->data_size && i < 8u; i++)
        (void)fprintf(stderr, " %02x", (unsigned)(unsigned char)scan->data[i]);
      (void)fprintf(stderr, "\n");
      (void)fflush(stderr);
    }
    if (!is_dc && (int)scan->ah != 0) {
      bs.pad_at_eob = 1;
    }
    {
      const char * e = getenv("GIMG_JPEG_RECOVER_STUFF_ZERO");
      if (e && e[0] == '1') {
        bs.recover_stuff_zero = 1;
      }
    }

    uint16_t restart_interval = state->restart_interval;
    int ss = (int)scan->ss;
    int se = (int)scan->se;
    int ah = (int)scan->ah;
    int al = (int)scan->al;
    unsigned int eobrun = 0;

    if (trace_all) {
      (void)fprintf(stderr,
          "SCAN_ENTER scan%u Ss=%u Se=%u Ah=%u Al=%u is_dc=%d data_size=%zu first_bytes:",
          (unsigned)scan_idx, (unsigned)scan->ss, (unsigned)scan->se,
          (unsigned)scan->ah, (unsigned)scan->al,
          (scan->ss == 0 && scan->se == 0) ? 1 : 0, (size_t)scan->data_size);
      for (size_t i = 0; i < (size_t)scan->data_size && i < 16u; i++) {
        (void)fprintf(stderr, " %02x", (unsigned)(unsigned char)scan->data[i]);
      }
      (void)fprintf(stderr, "\n");
      /* Help compare with ref: if ref consumes more bytes for first block, show
       * previous scan's last bytes (missing bytes might be at end of prev scan). */
      if (scan_idx > 0) {
        const gimg_jpeg_scan_t * prev = &state->scans[scan_idx - 1];
        if (prev->data && prev->data_size > 0) {
          size_t n = prev->data_size > 8u ? 8u : prev->data_size;
          (void)fprintf(stderr, "SCAN%u_PREV_LAST_BYTES scan%u_size=%zu",
              (unsigned)scan_idx, (unsigned)(scan_idx - 1), (size_t)prev->data_size);
          for (size_t i = prev->data_size - n; i < prev->data_size; i++) {
            (void)fprintf(stderr, " %02x", (unsigned)(unsigned char)prev->data[i]);
          }
          (void)fprintf(stderr, "\n");
        }
      }
      (void)fflush(stderr);
    }

    for (uint32_t mcu_y = 0; mcu_y < mcu_per_col; mcu_y++) {
      for (uint32_t mcu_x = 0; mcu_x < mcu_per_row; mcu_x++) {
        if (restart_interval > 0) {
          uint32_t mcu_index = mcu_y * mcu_per_row + mcu_x;
          if (mcu_index > 0 && (mcu_index % (uint32_t)restart_interval) == 0) {
            memset(dc_pred, 0, sizeof(dc_pred));
            /* T.81 Annex G: at restart, EOB run counter is reset to 0. */
            eobrun = 0;
          }
        }
        for (uint8_t s = 0; s < scan->comp_count; s++) {
          uint8_t comp_idx = 0;
          for (; comp_idx < num_comp; comp_idx++) {
            if (sof->comp_id[comp_idx] == scan->comp_id[s]) {
              break;
            }
          }
          if (comp_idx >= num_comp) {
            s_prog_fail_where = "comp_idx";
            goto fail_prog;
          }
          uint8_t h_samp = sof->h_samp[comp_idx];
          uint8_t v_samp = sof->v_samp[comp_idx];
          size_t blocks_per_mcu_comp = (size_t)h_samp * (size_t)v_samp;
          size_t mcu_block_start =
              (size_t)(mcu_y * mcu_per_row + mcu_x) * blocks_per_mcu_comp;

          for (uint8_t by = 0; by < v_samp; by++) {
            for (uint8_t bx = 0; bx < h_samp; bx++) {
              size_t block_idx =
                  mcu_block_start + (size_t)by * (size_t)h_samp + (size_t)bx;
              int16_t * block = coef_blocks[comp_idx] + block_idx * 64;
              size_t block_in_mcu_ta = (size_t)by * (size_t)h_samp + (size_t)bx;

              if (trace_all && mcu_x == 0 && mcu_y == 0) {
                jpeg_trace_all_dump_block("BLOCK_BEFORE", (unsigned)scan_idx,
                    mcu_x, mcu_y, (unsigned)comp_idx, block_in_mcu_ta,
                    bs.byte_off, bs.bit_off, block);
              }

              if (is_dc) {
                GIMG_Result r;
                if (ah == 0) {
                  int trace_sym = -1;
                  int trace_diff = 0;
                  int do_trace_dc = (trace_dc_sym &&
                      (trace_dc_sym_scan != (unsigned)-1
                          ? (scan_idx == trace_dc_sym_scan)
                          : (scan_idx == 0)));
                  int * psym = (do_trace_dc && mcu_x == 0 && mcu_y == 0 && s == 0
                      && by == 0 && bx == 0) ? &trace_sym : NULL;
                  int * pdiff = psym ? &trace_diff : NULL;
                  r = jpeg_decode_block_progressive_dc(&bs,
                      &dc_tables[scan->dc_tbl[s]], block, &dc_pred[comp_idx],
                      al, psym, pdiff,
                      (trace_all && mcu_x == 0 && mcu_y == 0 && s == 0 && by == 0 && bx == 0) ? 1 : 0);
                  if (r == GIMG_OK && trace_dc_sym && do_trace_dc && psym &&
                      mcu_x == 0 && mcu_y == 0 && s == 0 && by == 0 && bx == 0) {
                    const char * comp_name = (comp_idx == 0) ? "Y"
                        : (comp_idx == 1)                    ? "Cb"
                                                             : "Cr";
                    unsigned blk = (unsigned)(block_idx - mcu_block_start);
                    uint32_t mcu_id = mcu_y * mcu_per_row + mcu_x;
                    (void)fprintf(stderr,
                        "TRACE_JPEG_DC_SYMBOLS scan%u mcu=%u %s%u sym=%d diff=%d dc=%d\n",
                        (unsigned)scan_idx, (unsigned)mcu_id, comp_name, blk,
                        trace_sym, trace_diff, (int)block[0]);
                    (void)fflush(stderr);
                  }
                }
                else {
                  int trace_refine_bit = 0;
                  int * p_refine_bit =
                      (trace_dc_refine && is_dc && ah != 0 && mcu_x == 0 &&
                       mcu_y == 0)
                      ? &trace_refine_bit
                      : NULL;
                  r = jpeg_decode_block_progressive_dc_refine(
                      &bs, block, &dc_pred[comp_idx], (unsigned int)al,
                      p_refine_bit,
                      (trace_all && mcu_x == 0 && mcu_y == 0) ? 1 : 0);
                  if (r == GIMG_OK && trace_dc_refine && p_refine_bit) {
                    const char * comp_name = (comp_idx == 0) ? "Y"
                        : (comp_idx == 1)                    ? "Cb"
                                                             : "Cr";
                    unsigned blk = (unsigned)(block_idx - mcu_block_start);
                    (void)fprintf(stderr,
                        "TRACE_JPEG_DC_REFINE scan%u %s%u bit=%d\n",
                        (unsigned)scan_idx, comp_name, blk, trace_refine_bit);
                    (void)fflush(stderr);
                  }
                }
                if (r != GIMG_OK) {
                  s_prog_fail_where = "dc_decode";
                  goto fail_prog;
                }
              }
              else {
                if (ah == 0) {
                  if (trace_ac_sym && (int)scan->ah == 0 &&
                      s_first_ac_initial_scan == (unsigned)-1) {
                    s_first_ac_initial_scan = scan_idx;
                  }
                  /* Trace first 6 blocks of first MCU for selected AC-initial scan(s). */
                  unsigned blk_in_mcu = (unsigned)(by * (unsigned)h_samp + bx);
                  int scan_in_trace_list = 0;
                  if (s_num_trace_ac_sym_scans > 0) {
                    for (size_t ti = 0; ti < s_num_trace_ac_sym_scans; ti++) {
                      if (s_trace_ac_sym_scans[ti] == scan_idx) {
                        scan_in_trace_list = 1;
                        break;
                      }
                    }
                  } else {
                    scan_in_trace_list = (scan_idx == s_first_ac_initial_scan);
                  }
                  int do_trace =
                      (trace_ac_sym && scan_in_trace_list &&
                       mcu_x == 0 && mcu_y == 0 && blk_in_mcu < 6);
                  // Debug: dump AC table for first AC-initial block (compare to script).
                  if (do_trace && getenv("DUMP_JPEG_AC_TABLE") &&
                      getenv("DUMP_JPEG_AC_TABLE")[0] == '1') {
                    const gimg_jpeg_huff_table_t * at = &ac_tables[scan->ac_tbl[s]];
                    (void)fprintf(stderr, "DUMP_JPEG_AC_TABLE scan%u Ta=%u num_values=%d\n",
                        (unsigned)scan_idx, (unsigned)scan->ac_tbl[s], at->num_values);
                    for (int len = 1; len <= 16; len++) {
                      uint16_t mn = at->min_code[len];
                      uint16_t mx = at->max_code[len];
                      int count = (mx >= mn) ? (int)(mx - mn + 1) : 0;
                      (void)fprintf(stderr, "  len%2d min=%u max=%u base=%u",
                          len, (unsigned)mn, (unsigned)mx,
                          (unsigned)at->base_index[len]);
                      for (int i = 0; i < count && i < 8; i++) {
                        (void)fprintf(stderr, " v[%d]=%u",
                            i, (unsigned)at->values[at->base_index[len] + i]);
                      }
                      if (count > 8) {
                        (void)fprintf(stderr, " ...");
                      }
                      (void)fprintf(stderr, "\n");
                    }
                    (void)fflush(stderr);
                  }
                  GIMG_Result r = jpeg_decode_block_progressive_ac_initial(
                      &bs, &ac_tables[scan->ac_tbl[s]], block, ss, se, al, do_trace,
                      (do_trace || (trace_all && mcu_x == 0 && mcu_y == 0))
                          ? (int)blk_in_mcu
                          : -1,
                      (unsigned int)scan_idx, &eobrun,
                      (trace_all && mcu_x == 0 && mcu_y == 0) ? 1 : 0);
                  if (r != GIMG_OK) {
                    s_prog_fail_where = "ac_initial";
                    if (prog_debug) {
                      (void)fprintf(stderr,
                          " ac_initial fail block_idx=%zu byte_off=%zu "
                          "size=%zu\n",
                          block_idx, (size_t)bs.byte_off,
                          (size_t)scan->data_size);
                    }
                    goto fail_prog;
                  }
                  if (trace_ac_initial_bitstream && mcu_x == 0 && mcu_y == 0) {
                    const char * comp_name = (comp_idx == 0) ? "Y"
                        : (comp_idx == 1) ? "Cb" : "Cr";
                    unsigned blk = (unsigned)(block_idx - mcu_block_start);
                    (void)fprintf(stderr,
                        "TRACE_JPEG_AC_INITIAL_BITSTREAM scan%u %s%u "
                        "byte_off=%zu bit_off=%u\n",
                        (unsigned)scan_idx, comp_name, blk,
                        (size_t)bs.byte_off, (unsigned)bs.bit_off);
                    (void)fflush(stderr);
                  }
                }
                else {
                  // Ah!=0: use 17-symbol refinement table if defined (shorter
                  // codes, spec T.81 Table K.6); else same AC table.
                  const gimg_jpeg_huff_table_t * ac_ref_tbl =
                      (ac_refine_tables[scan->ac_tbl[s]].num_values > 0)
                      ? &ac_refine_tables[scan->ac_tbl[s]]
                      : &ac_tables[scan->ac_tbl[s]];
                  const char * trace_ac_ref_env = getenv("TRACE_JPEG_AC_REFINE");
                  unsigned blk_in_mcu_ref = (comp_idx == 0)
                      ? (unsigned)(by * (unsigned)h_samp + bx)
                      : (unsigned)(4 + comp_idx - 1);
                  int do_trace_ref = (trace_ac_ref_env && trace_ac_ref_env[0] == '1'
                      && mcu_x == 0 && mcu_y == 0 && blk_in_mcu_ref < 6);
                  // Debug: dump AC table used for this refinement scan (T.81 B.2.4: which DHT).
                  if (do_trace_ref && blk_in_mcu_ref == 0 && getenv("DUMP_JPEG_AC_TABLE") &&
                      getenv("DUMP_JPEG_AC_TABLE")[0] == '1') {
                    const gimg_jpeg_huff_table_t * at = ac_ref_tbl;
                    (void)fprintf(stderr, "DUMP_JPEG_AC_TABLE scan%u (refine) Ta=%u num_values=%d\n",
                        (unsigned)scan_idx, (unsigned)scan->ac_tbl[s], at->num_values);
                    for (int len = 1; len <= 16; len++) {
                      uint16_t mn = at->min_code[len];
                      uint16_t mx = at->max_code[len];
                      int count = (mx >= mn) ? (int)(mx - mn + 1) : 0;
                      if (count > 0) {
                        (void)fprintf(stderr, "  len%2d min=%u max=%u base=%u",
                            len, (unsigned)mn, (unsigned)mx,
                            (unsigned)at->base_index[len]);
                        for (int i = 0; i < count && i < 16; i++) {
                          (void)fprintf(stderr, " v[%d]=0x%02x",
                              i, (unsigned)at->values[at->base_index[len] + i]);
                        }
                        (void)fprintf(stderr, "\n");
                      }
                    }
                    (void)fflush(stderr);
                  }
                  if (do_trace_ref && blk_in_mcu_ref == 0 && getenv("DUMP_JPEG_AC_REFINE_BLOCK") &&
                      getenv("DUMP_JPEG_AC_REFINE_BLOCK")[0] == '1') {
                    (void)fprintf(stderr,
                        "DUMP_JPEG_AC_REFINE_BLOCK scan%u ss=%d se=%d ah=%d block[%d..%d]:",
                        (unsigned)scan_idx, ss, se, ah, ss, se);
                    for (int ki = ss; ki <= se; ki++) {
                      (void)fprintf(stderr, " %d", (int)block[ki]);
                    }
                    (void)fprintf(stderr, "\n");
                    (void)fflush(stderr);
                  }
                  if (do_trace_ref && getenv("TRACE_JPEG_AC_REFINE_POS") &&
                      getenv("TRACE_JPEG_AC_REFINE_POS")[0] == '1') {
                    (void)fprintf(stderr,
                        "TRACE_JPEG_AC_REFINE_POS block=%u byte_off=%zu bit_off=%u next2bits=",
                        (unsigned)blk_in_mcu_ref, (size_t)bs.byte_off, (unsigned)bs.bit_off);
                    size_t bo = bs.byte_off;
                    int bto = bs.bit_off;
                    for (int i = 0; i < 2 && bo < bs.size; i++) {
                      int bit = (bto < 8) ? ((int)(bs.data[bo] >> (7 - bto)) & 1) : -1;
                      (void)fprintf(stderr, "%d", bit >= 0 ? bit : -1);
                      bto++;
                      if (bto == 8) {
                        bto = 0;
                        bo++;
                      }
                    }
                    (void)fprintf(stderr, "\n");
                    (void)fflush(stderr);
                  }
                  int do_sanity = (getenv("GIMG_JPEG_AC_REFINE_SANITY") != NULL &&
                      getenv("GIMG_JPEG_AC_REFINE_SANITY")[0] == '1' &&
                      scan_idx == 5 && blk_in_mcu_ref < 6);
                  if (do_sanity) {
                    (void)fprintf(stderr, "SANITY_OUR block=%u byte_off=%zu bit_off=%u\n",
                        (unsigned)blk_in_mcu_ref, (size_t)bs.byte_off, (unsigned)bs.bit_off);
                    (void)fflush(stderr);
                  }
                  GIMG_Result r = jpeg_decode_block_progressive_ac_refine(
                      &bs, ac_ref_tbl, block, ss, se, al, do_trace_ref,
                      (do_trace_ref || do_sanity || (trace_all && mcu_x == 0 && mcu_y == 0))
                          ? (int)blk_in_mcu_ref
                          : -1,
                      do_sanity ? 1 : 0,
                      (trace_all && mcu_x == 0 && mcu_y == 0) ? 1 : 0);
                  if (r != GIMG_OK) {
                    s_prog_fail_where = "ac_refine";
                    if (prog_debug) {
                      (void)fprintf(stderr,
                          " ac_refine fail block_idx=%zu byte_off=%zu "
                          "size=%zu\n",
                          block_idx, (size_t)bs.byte_off,
                          (size_t)scan->data_size);
                    }
                    goto fail_prog;
                  }
                  if (prog_debug && !is_dc && (int)scan->ah != 0) {
                    (void)fprintf(stderr,
                        " ac_refine block=%zu done byte_off=%zu\n",
                        block_idx, (size_t)bs.byte_off);
                    (void)fflush(stderr);
                  }
                  if (do_trace_ref && !is_dc && (int)scan->ah != 0 &&
                      scan_idx == 5 && blk_in_mcu_ref < 2) {
                    (void)fprintf(stderr,
                        "TRACE_JPEG_AC_REFINE block=%u done byte_off=%zu bit_off=%u\n",
                        (unsigned)blk_in_mcu_ref, (size_t)bs.byte_off,
                        (unsigned)bs.bit_off);
                    (void)fflush(stderr);
                  }
                }
              }

              if (trace_all && mcu_x == 0 && mcu_y == 0) {
                jpeg_trace_all_dump_block("BLOCK_AFTER", (unsigned)scan_idx,
                    mcu_x, mcu_y, (unsigned)comp_idx, block_in_mcu_ta,
                    bs.byte_off, bs.bit_off, block);
              }
            }
          }
        }
        if (dump_dc && is_dc && mcu_x == 0 && mcu_y == 0) {
          // First MCU: dump DCs for Y0..Y3, Cb0, Cr0 (4:2:0).
          (void)fprintf(stderr,
              "DUMP_JPEG_DC scan%u: Y0=%d Y1=%d Y2=%d Y3=%d "
              "Cb0=%d Cr0=%d\n",
              (unsigned)scan_idx, (int)coef_blocks[0][0],
              (int)coef_blocks[0][1 * 64], (int)coef_blocks[0][2 * 64],
              (int)coef_blocks[0][3 * 64], (int)coef_blocks[1][0],
              (int)coef_blocks[2][0]);
          (void)fflush(stderr);
        }
      }
    }

    /* Per-scan coefficient hash (first MCU) for regression / reference compare.
     * Hash in natural order per block (iterate nat 0..63, feed blk[inv_zigzag[nat]])
     * to match libjpeg jdtrans.c dump_ref_coef_after_scan. */
    {
      const char * dump_after_env = getenv("DUMP_JPEG_COEF_AFTER_SCAN");
      if (dump_after_env && dump_after_env[0] == '1') {
        uint64_t fnv = 0xcbf29ce484222325ULL;
        const uint64_t fnv_prime = 0x100000001b3ULL;
        for (uint8_t c = 0; c < num_comp; c++) {
          size_t first_mcu_blocks =
              (size_t)sof->h_samp[c] * (size_t)sof->v_samp[c];
          const int16_t * blocks = coef_blocks[c];
          for (size_t b = 0; b < first_mcu_blocks; b++) {
            const int16_t * blk = blocks + b * 64;
            for (int nat = 0; nat < 64; nat++) {
              int16_t v = blk[gimg_jpeg_inv_zigzag[nat]];
              const unsigned char * p = (const unsigned char *)&v;
              fnv ^= (uint64_t)p[0];
              fnv *= fnv_prime;
              fnv ^= (uint64_t)p[1];
              fnv *= fnv_prime;
            }
          }
        }
        (void)fprintf(stderr, "DUMP_JPEG_COEF_AFTER_SCAN scan%u hash=0x%" PRIx64 "\n",
            (unsigned)scan_idx, fnv);
        if (width == 8 && height == 8 && num_comp == 1) {
          const int16_t * blk = coef_blocks[0];
          (void)fprintf(stderr, "DUMP_JPEG_COEF_AFTER_SCAN scan%u block:",
              (unsigned)scan_idx);
          for (int i = 0; i < 64; i++) {
            (void)fprintf(stderr, " %d", (int)blk[i]);
          }
          (void)fprintf(stderr, "\n");
        }
        /* Dump first MCU blocks in REF-comparable format (natural order) for
         * compare_progressive_trace.py and diff vs instrumented ref. */
        if (getenv("DUMP_JPEG_COEF_BLOCKS_AFTER_SCAN") &&
            getenv("DUMP_JPEG_COEF_BLOCKS_AFTER_SCAN")[0] == '1') {
          for (uint8_t c = 0; c < num_comp; c++) {
            size_t first_mcu_blocks =
                (size_t)sof->h_samp[c] * (size_t)sof->v_samp[c];
            const int16_t * blocks = coef_blocks[c];
            for (size_t b = 0; b < first_mcu_blocks; b++) {
              const int16_t * blk = blocks + b * 64;
              (void)fprintf(stderr, "OUR_SCAN%u_COMP%u_BLOCK%zu",
                  (unsigned)scan_idx, (unsigned)c, b);
              for (int nat = 0; nat < 64; nat++) {
                (void)fprintf(stderr, " %d",
                    (int)blk[gimg_jpeg_inv_zigzag[nat]]);
              }
              (void)fprintf(stderr, "\n");
            }
          }
          (void)fflush(stderr);
        }
        (void)fflush(stderr);
      }
    }
  }

  // Debug: dump first block coefficients for 8×8 single-component (e.g.
  // progressive_8x8_gray).
  if (width == 8 && height == 8 && num_comp == 1) {
    const char * dump_coef_env = getenv("DUMP_JPEG_COEF_BLOCK");
    if (dump_coef_env && dump_coef_env[0] == '1') {
      const int16_t * blk = coef_blocks[0];
      (void)fprintf(stderr, "DUMP_JPEG_COEF_BLOCK 8x8 gray (zigzag order):");
      for (int i = 0; i < 64; i++) {
        (void)fprintf(stderr, " %d", (int)blk[i]);
      }
      (void)fprintf(stderr, "\n");
      (void)fflush(stderr);
    }
  }

  // Debug: dump first MCU coefficients (natural order per block) for diff vs
  // dump_jpeg_coef_ref DUMP_FIRST_MCU=1 (progressive_sample.jpg etc.).
  {
    const char * dump_first_mcu = getenv("DUMP_JPEG_COEF_FIRST_MCU");
    if (dump_first_mcu && dump_first_mcu[0] == '1') {
      for (uint8_t c = 0; c < num_comp; c++) {
        size_t first_mcu_blocks =
            (size_t)sof->h_samp[c] * (size_t)sof->v_samp[c];
        const int16_t * blocks = coef_blocks[c];
        for (size_t b = 0; b < first_mcu_blocks; b++) {
          const int16_t * blk = blocks + b * 64;
          (void)fprintf(stderr, "OUR_COMP%u_BLOCK%zu", (unsigned)c, b);
          for (int nat = 0; nat < 64; nat++) {
            (void)fprintf(stderr, " %d", (int)blk[gimg_jpeg_inv_zigzag[nat]]);
          }
          (void)fprintf(stderr, "\n");
        }
      }
      (void)fflush(stderr);
    }
  }

  // Dequantise, IDCT, write to component buffers (same layout as baseline).
  size_t comp_stride[GIMG_JPEG_MAX_COMPONENTS];
  size_t comp_size[GIMG_JPEG_MAX_COMPONENTS];
  unsigned char * comp_buf[GIMG_JPEG_MAX_COMPONENTS];
  for (uint8_t i = 0; i < num_comp; i++) {
    comp_stride[i] = (size_t)comp_w[i];
    if (!gimg_safe_mul_size(comp_stride[i], (size_t)comp_h[i], &comp_size[i])) {
      s_prog_fail_where = "comp_size";
      goto fail_prog;
    }
    comp_buf[i] = (unsigned char *)gimg_malloc(alloc, comp_size[i]);
    if (!comp_buf[i]) {
      for (uint8_t j = 0; j < i; j++) {
        gimg_free(alloc, comp_buf[j]);
      }
      s_prog_fail_where = "comp_alloc";
      goto fail_prog;
    }
    memset(comp_buf[i], 0, comp_size[i]);
  }

  // Optional: log per-component quant table ID and first values (spec T.81 SOF Tqi).
  if (getenv("DUMP_JPEG_QUANT_IDS") && getenv("DUMP_JPEG_QUANT_IDS")[0] == '1') {
    for (uint8_t i = 0; i < num_comp; i++) {
      uint8_t qid = sof->quant_tbl_id[i];
      (void)fprintf(stderr, "DUMP_JPEG_QUANT_IDS comp%u quant_tbl_id=%u present=%d",
          (unsigned)i, (unsigned)qid,
          (qid < GIMG_JPEG_MAX_QUANT_TABLES && state->quant_tbl_present[qid]) ? 1 : 0);
      if (qid < GIMG_JPEG_MAX_QUANT_TABLES && state->quant_tbl_present[qid]) {
        const uint16_t * q = state->quant_tbl[qid];
        (void)fprintf(stderr, " q[0..3]=%u %u %u %u", (unsigned)q[0], (unsigned)q[1],
            (unsigned)q[2], (unsigned)q[3]);
      }
      (void)fprintf(stderr, "\n");
    }
    (void)fflush(stderr);
  }

  int16_t block_rz[64];
  int16_t block_q[64];
  for (uint8_t comp_idx = 0; comp_idx < num_comp; comp_idx++) {
    uint8_t qid = sof->quant_tbl_id[comp_idx];
    if (qid >= GIMG_JPEG_MAX_QUANT_TABLES || !state->quant_tbl_present[qid]) {
      s_prog_fail_where = "quant_tbl";
      goto fail_prog_buf;
    }
    const uint16_t * quant = state->quant_tbl[qid];
    uint32_t blocks_w = comp_w[comp_idx] / 8;
    uint32_t blocks_h = comp_h[comp_idx] / 8;

    for (uint32_t by = 0; by < blocks_h; by++) {
      for (uint32_t bx = 0; bx < blocks_w; bx++) {
        size_t block_idx = (size_t)by * (size_t)blocks_w + (size_t)bx;
        const int16_t * block_zig = coef_blocks[comp_idx] + block_idx * 64;
        jpeg_dezigzag(block_zig, block_rz);
        jpeg_dequantise(block_rz, quant, block_q);
        jpeg_idct_8x8_islow(block_q, block_rz);
        uint32_t dst_x = bx * 8;
        uint32_t dst_y = by * 8;
        for (int dy = 0; dy < 8; dy++) {
          uint32_t y = dst_y + (uint32_t)dy;
          if (y >= comp_h[comp_idx]) {
            break;
          }
          for (int dx = 0; dx < 8; dx++) {
            uint32_t x = dst_x + (uint32_t)dx;
            if (x >= comp_w[comp_idx]) {
              break;
            }
            int v = block_rz[dy * 8 + dx] + 128;
            if (v < 0) {
              v = 0;
            }
            if (v > 255) {
              v = 255;
            }
            comp_buf[comp_idx][y * comp_stride[comp_idx] + x] =
                (unsigned char)v;
          }
        }
      }
    }
  }

  // Optional debug dump: Y, Cb, Cr (see compare_ycbcr_components.py).
  if (num_comp == 3) {
    const char * dir = getenv("DUMP_JPEG_COMPONENTS");
    if (dir && dir[0] != '\0') {
      const char * names[] = {"Y.raw", "Cb.raw", "Cr.raw"};
      const uint32_t cw[] = {comp_w[0], comp_w[1], comp_w[2]};
      const uint32_t ch[] = {comp_h[0], comp_h[1], comp_h[2]};
      for (int i = 0; i < 3; i++) {
        char path[1024];
        int n = snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        if (n > 0 && (size_t)n < sizeof(path)) {
          FILE * f = fopen(path, "wb");
          if (f) {
            uint32_t w = cw[i];
            uint32_t h = ch[i];
            fwrite(&w, 4, 1, f);
            fwrite(&h, 4, 1, f);
            for (uint32_t row = 0; row < h; row++) {
              fwrite(comp_buf[i] + row * comp_stride[i], 1, (size_t)w, f);
            }
            fclose(f);
          }
        }
      }
    }
  }

  // Create output raster (same as baseline).
  GIMG_Result r;
  if (num_comp == 1) {
    r = gimg_raster_create_with_allocator(alloc, (uint32_t)width,
        (uint32_t)height, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED, NULL, 0,
        out_raster);
    if (r != GIMG_OK) {
      s_prog_fail_where = "raster_gray";
      goto fail_prog_buf;
    }
    unsigned char * pixels = (unsigned char *)gimg_raster_pixels(*out_raster);
    size_t stride = gimg_raster_stride_bytes(*out_raster);
    for (uint32_t y = 0; y < height; y++) {
      memcpy(
          pixels + y * stride, comp_buf[0] + y * comp_stride[0], (size_t)width);
    }
  }
  else if (num_comp == 3) {
    r = gimg_raster_create_with_allocator(alloc, (uint32_t)width,
        (uint32_t)height, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, NULL, 0,
        out_raster);
    if (r != GIMG_OK) {
      s_prog_fail_where = "raster_rgba";
      goto fail_prog_buf;
    }
    unsigned char * pixels = (unsigned char *)gimg_raster_pixels(*out_raster);
    size_t stride = gimg_raster_stride_bytes(*out_raster);
    uint32_t cw1 = comp_w[1];
    uint32_t ch1 = comp_h[1];
    uint32_t cw2 = comp_w[2];
    uint32_t ch2 = comp_h[2];
    int use_fancy = (options && options->jpeg_chroma_upsampling ==
                            GIMG_JPEG_CHROMA_UPSAMPLE_FANCY) &&
        (width == cw1 * 2 && height == ch1 * 2 && width == cw2 * 2 &&
            height == ch2 * 2);
    for (uint32_t y = 0; y < height; y++) {
      uint32_t cy1 = (ch1 > 1 && height > 1) ? (y * ch1 / height) : 0;
      uint32_t cy2 = (ch2 > 1 && height > 1) ? (y * ch2 / height) : 0;
      for (uint32_t x = 0; x < width; x++) {
        uint32_t cx1 = (cw1 > 1 && width > 1) ? (x * cw1 / width) : 0;
        uint32_t cx2 = (cw2 > 1 && width > 1) ? (x * cw2 / width) : 0;
        int yy = comp_buf[0][y * comp_stride[0] + x];
        int cb, cr;
        if (use_fancy) {
          cb = jpeg_chroma_sample_fancy_2h2v(
              comp_buf[1], comp_stride[1], cw1, ch1, x, y);
          cr = jpeg_chroma_sample_fancy_2h2v(
              comp_buf[2], comp_stride[2], cw2, ch2, x, y);
        }
        else {
          cb = comp_buf[1][cy1 * comp_stride[1] + cx1];
          cr = comp_buf[2][cy2 * comp_stride[2] + cx2];
        }
        // Same integer YCbCr→RGB as baseline (SCALEBITS=16) for bit-exact
        // match.
        int cb_x = cb - 128;
        int cr_x = cr - 128;
        int r_val = yy + (int)((91881L * cr_x + 32768) >> 16);
        int g_val = yy +
            (int)(((int32_t)(-22554) * cb_x + (int32_t)(-46802) * cr_x +
                      32768) >>
                16);
        int b_val = yy + (int)((116130L * cb_x + 32768) >> 16);
        if (r_val < 0)
          r_val = 0;
        if (r_val > 255)
          r_val = 255;
        if (g_val < 0)
          g_val = 0;
        if (g_val > 255)
          g_val = 255;
        if (b_val < 0)
          b_val = 0;
        if (b_val > 255)
          b_val = 255;
        pixels[y * stride + x * 4 + 0] = (unsigned char)r_val;
        pixels[y * stride + x * 4 + 1] = (unsigned char)g_val;
        pixels[y * stride + x * 4 + 2] = (unsigned char)b_val;
        pixels[y * stride + x * 4 + 3] = 255;
      }
    }
  }
  else if (num_comp == 4) {
    uint32_t cw[4] = {comp_w[0], comp_w[1], comp_w[2], comp_w[3]};
    uint32_t ch[4] = {comp_h[0], comp_h[1], comp_h[2], comp_h[3]};
    if (state->adobe_transform == 2) {
      r = gimg_raster_create_with_allocator(alloc, (uint32_t)width,
          (uint32_t)height, &GIMG_PIXEL_CMYK8, GIMG_RASTER_OWNED, NULL, 0,
          out_raster);
      if (r != GIMG_OK) {
        s_prog_fail_where = "raster_cmyk";
        goto fail_prog_buf;
      }
      unsigned char * pixels = (unsigned char *)gimg_raster_pixels(*out_raster);
      size_t stride = gimg_raster_stride_bytes(*out_raster);
      for (uint32_t y = 0; y < height; y++) {
        uint32_t cy[4];
        for (int i = 0; i < 4; i++) {
          cy[i] =
              (ch[i] > 1 && height > 1) ? (y * (ch[i] - 1) / (height - 1)) : 0;
        }
        for (uint32_t x = 0; x < width; x++) {
          uint32_t cx[4];
          for (int i = 0; i < 4; i++) {
            cx[i] =
                (cw[i] > 1 && width > 1) ? (x * (cw[i] - 1) / (width - 1)) : 0;
          }
          int yy = comp_buf[0][cy[0] * comp_stride[0] + cx[0]];
          int cb_x = comp_buf[1][cy[1] * comp_stride[1] + cx[1]] - 128;
          int cr_x = comp_buf[2][cy[2] * comp_stride[2] + cx[2]] - 128;
          int k = comp_buf[3][cy[3] * comp_stride[3] + cx[3]];
          int r_val = yy + (int)((91881L * cr_x + 32768) >> 16);
          int g_val = yy +
              (int)(((int32_t)(-22554) * cb_x + (int32_t)(-46802) * cr_x +
                        32768) >>
                  16);
          int b_val = yy + (int)((116130L * cb_x + 32768) >> 16);
          if (r_val < 0)
            r_val = 0;
          if (r_val > 255)
            r_val = 255;
          if (g_val < 0)
            g_val = 0;
          if (g_val > 255)
            g_val = 255;
          if (b_val < 0)
            b_val = 0;
          if (b_val > 255)
            b_val = 255;
          pixels[y * stride + x * 4 + 0] = (unsigned char)(255 - r_val);
          pixels[y * stride + x * 4 + 1] = (unsigned char)(255 - g_val);
          pixels[y * stride + x * 4 + 2] = (unsigned char)(255 - b_val);
          pixels[y * stride + x * 4 + 3] = (unsigned char)k;
        }
      }
    }
    else {
      // Raw CMYK: output decompressed components unchanged (match libjpeg
      // null_convert for JCS_CMYK; see baseline path).
      r = gimg_raster_create_with_allocator(alloc, (uint32_t)width,
          (uint32_t)height, &GIMG_PIXEL_CMYK8, GIMG_RASTER_OWNED, NULL, 0,
          out_raster);
      if (r != GIMG_OK) {
        s_prog_fail_where = "raster_cmyk_raw";
        goto fail_prog_buf;
      }
      unsigned char * pixels = (unsigned char *)gimg_raster_pixels(*out_raster);
      size_t stride = gimg_raster_stride_bytes(*out_raster);
      for (uint32_t y = 0; y < height; y++) {
        uint32_t cy[4];
        for (int i = 0; i < 4; i++) {
          cy[i] =
              (ch[i] > 1 && height > 1) ? (y * (ch[i] - 1) / (height - 1)) : 0;
        }
        for (uint32_t x = 0; x < width; x++) {
          uint32_t cx[4];
          for (int i = 0; i < 4; i++) {
            cx[i] =
                (cw[i] > 1 && width > 1) ? (x * (cw[i] - 1) / (width - 1)) : 0;
          }
          pixels[y * stride + x * 4 + 0] =
              (unsigned char)comp_buf[0][cy[0] * comp_stride[0] + cx[0]];
          pixels[y * stride + x * 4 + 1] =
              (unsigned char)comp_buf[1][cy[1] * comp_stride[1] + cx[1]];
          pixels[y * stride + x * 4 + 2] =
              (unsigned char)comp_buf[2][cy[2] * comp_stride[2] + cx[2]];
          pixels[y * stride + x * 4 + 3] =
              (unsigned char)comp_buf[3][cy[3] * comp_stride[3] + cx[3]];
        }
      }
    }
  }
  else {
    r = GIMG_ERR_UNSUPPORTED;
    s_prog_fail_where = "unsupported";
    goto fail_prog_buf;
  }

  if (state->app2_icc && state->app2_icc_len > 0u) {
    GIMG_Color_Info color_info;
    gimg_color_info_default(&color_info);
    if (state->app2_icc_num_chunks > 0) {
      color_info.icc_bytes = state->app2_icc;
      color_info.icc_size = state->app2_icc_len;
    }
    else {
      color_info.icc_bytes = state->app2_icc + 14;
      color_info.icc_size = state->app2_icc_len - 14u;
    }
    (void)gimg_raster_set_color_info(*out_raster, &color_info);
  }

  for (uint8_t i = 0; i < num_comp; i++) {
    gimg_free(alloc, coef_blocks[i]);
    gimg_free(alloc, comp_buf[i]);
  }
  return GIMG_OK;

fail_prog_buf:
  for (uint8_t i = 0; i < num_comp; i++) {
    gimg_free(alloc, comp_buf[i]);
  }
fail_prog:
  if (prog_debug) {
    (void)fprintf(stderr, "progressive decode fail: scan %u %s\n",
        (unsigned)s_prog_fail_scan,
        s_prog_fail_where ? s_prog_fail_where : "?");
  }
  for (uint8_t i = 0; i < num_comp; i++) {
    gimg_free(alloc, coef_blocks[i]);
  }
  return GIMG_ERR_CORRUPT;
}

//
// Encode: default quant tables, forward DCT, bitstream write, Huffman encode.
//

/** Default luminance quantisation table (ITU-T T.81 Annex K.1), row-major. */
static const uint8_t gimg_jpeg_default_quant_luma[64] = {
    16,
    11,
    10,
    16,
    24,
    40,
    51,
    61,
    12,
    12,
    14,
    19,
    26,
    58,
    60,
    55,
    14,
    13,
    16,
    24,
    40,
    57,
    69,
    56,
    14,
    17,
    22,
    29,
    51,
    87,
    80,
    62,
    18,
    22,
    37,
    56,
    68,
    109,
    103,
    77,
    24,
    35,
    55,
    64,
    81,
    104,
    113,
    92,
    49,
    64,
    78,
    87,
    103,
    121,
    120,
    101,
    72,
    92,
    95,
    98,
    112,
    100,
    103,
    99,
};

/** Default chrominance quantisation table (ITU-T T.81 Annex K.2), row-major. */
static const uint8_t gimg_jpeg_default_quant_chroma[64] = {
    17,
    18,
    24,
    47,
    99,
    99,
    99,
    99,
    18,
    21,
    26,
    66,
    99,
    99,
    99,
    99,
    24,
    26,
    56,
    99,
    99,
    99,
    99,
    99,
    47,
    66,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
};

/** Scale default quant by quality (1..100; 100 = finest). Clamp to 1..255. */
static void jpeg_scale_quant(
    unsigned quality, const uint8_t * in, uint16_t * out) {
  if (quality < 1) {
    quality = 1;
  }
  if (quality > 100) {
    quality = 100;
  }
  // scale 1 at quality 100, ~50 at quality 50, 100 at quality 1
  unsigned scale = 101 - quality;
  for (int i = 0; i < 64; i++) {
    unsigned v = (in[i] * scale + 50) / 100;
    out[i] = (uint16_t)(v < 1 ? 1 : (v > 255 ? 255 : v));
  }
}

/** Scale default quant to 16-bit range for 12/16-bit DQT. Quality 1..100;
 * output 1..65535. Same relative scaling as 8-bit; values * 256 for DQT Pq=1.
 */
static void jpeg_scale_quant_16bit(
    unsigned quality, const uint8_t * in, uint16_t * out) {
  if (quality < 1) {
    quality = 1;
  }
  if (quality > 100) {
    quality = 100;
  }
  unsigned scale = 101 - quality;
  for (int i = 0; i < 64; i++) {
    unsigned v = (in[i] * scale + 50) / 100;
    if (v < 1) {
      v = 1;
    }
    // 16-bit DQT: scale so dequant range matches; use 256 * quant for same
    // effective step as 8-bit. Clamp to 1..65535.
    uint32_t v16 = (uint32_t)v * 256u;
    out[i] = (uint16_t)(v16 > 65535u ? 65535u : v16);
  }
}

/** Forward 1D DCT for 8 points (samples in, coefficients out). */
static void jpeg_fdct_1d(const int16_t * in, int16_t * out) {
  for (int u = 0; u < 8; u++) {
    int32_t sum = 0;
    int32_t c = (u == 0) ? 181 : 256; // 256/sqrt(2) for u=0
    for (int x = 0; x < 8; x++) {
      double angle = (2 * x + 1) * u * 3.14159265358979323846 / 16.0;
      sum += (int32_t)((double)in[x] * (double)c * cos(angle) + 0.5);
    }
    out[u] = (int16_t)(sum / 256);
  }
}

/** Forward 2D 8x8 DCT (level-shifted samples -128..127 in, coeffs out). */
static void jpeg_fdct_8x8(int16_t * block) {
  int16_t row[8];
  int16_t tmp[64];
  for (int y = 0; y < 8; y++) {
    jpeg_fdct_1d(block + y * 8, row);
    for (int x = 0; x < 8; x++) {
      tmp[x * 8 + y] = row[x];
    }
  }
  for (int x = 0; x < 8; x++) {
    jpeg_fdct_1d(tmp + x * 8, row);
    for (int y = 0; y < 8; y++) {
      block[y * 8 + x] = row[y];
    }
  }
}

/** Quantise block in place: block[i] = round(block[i] / quant[i]). */
static void jpeg_quantise(int16_t * block, const uint16_t * quant) {
  for (int i = 0; i < 64; i++) {
    int32_t v = (int32_t)block[i] * 256 / (int32_t)quant[i];
    block[i] = (int16_t)(v < -32768 ? -32768 : (v > 32767 ? 32767 : v));
  }
}

/** Reorder block from row-major to zigzag stream order. */
static void jpeg_zigzag_encode(const int16_t * block_rm, int16_t * block_zz) {
  for (int i = 0; i < 64; i++) {
    block_zz[i] = block_rm[gimg_jpeg_zigzag[i]];
  }
}

/** Bitstream writer: append bits (MSB first), flush with 0xFF stuffing.
 * If data_base is non-null, it points to a reserved prefix (before data) where
 * write_bits stores the current code so it cannot be overwritten by data[]. */
typedef struct {
  unsigned char * data;
  unsigned char * data_base; ///< if non-null, prefix; write_bits stores code at [0..3]
  size_t alloc_size;
  size_t size;
  int bit_off;   ///< 0..7; next bit goes at data[size] >> (7 - bit_off)
  uint8_t cache; ///< current byte being filled
} gimg_jpeg_bitstream_write_t;

static void jpeg_bitstream_write_init(
    gimg_jpeg_bitstream_write_t * w, const GIMG_Allocator * alloc) {
  w->alloc_size = 4096;
  w->data_base = NULL;
  w->data =
      (unsigned char *)gimg_malloc(gimg_alloc_or_default(alloc), w->alloc_size);
  w->size = 0;
  w->bit_off = 0;
  w->cache = 0;
}

static void jpeg_bitstream_write_byte(
    gimg_jpeg_bitstream_write_t * w, uint8_t b, const GIMG_Allocator * alloc) {
  if (w->size >= w->alloc_size) {
    size_t new_size = w->alloc_size * 2;
    unsigned char * p = (unsigned char *)gimg_realloc(
        gimg_alloc_or_default(alloc), w->data, new_size);
    if (!p) {
      return;
    }
    w->data = p;
    w->alloc_size = new_size;
  }
  if (getenv("GIMG_JPEG_TRACE_ENC") && getenv("GIMG_JPEG_TRACE_ENC")[0] == '1'
      && w->size == 6u) {
    (void)fprintf(stderr, "TRACE_ENC byte[6]=0x%02x (7th byte)\n", (unsigned)b);
    (void)fflush(stderr);
  }
  w->data[w->size++] = b;
  /* Do not add 0x00 stuffing here: jpeg_write_scan_data_with_stuffing (save
   * path) adds stuffing when writing to the stream. If we stuffed here we would
   * double-stuff (save path adds again after each 0xFF). */
  if (getenv("GIMG_JPEG_DEBUG_SCAN_BYTES") && getenv("GIMG_JPEG_DEBUG_SCAN_BYTES")[0] == '1'
      && w->size <= 5u) {
    (void)fprintf(stderr, "BASELINE_ENCODE write_byte b=0x%02x w->size=%zu\n",
        (unsigned)b, (size_t)w->size);
    (void)fflush(stderr);
  }
}

/* When w->data_base is set, store code there so write_byte (w->data[]) never
 * overwrites it (allocator can return a block that overlaps stack or .bss). */
static void jpeg_bitstream_write_bits(gimg_jpeg_bitstream_write_t * w,
    unsigned code, int num_bits, const GIMG_Allocator * alloc) {
  uint32_t code_val;
  if (w->data_base) {
    *(uint32_t *)w->data_base = (uint32_t)code;
    code_val = *(uint32_t *)w->data_base;
  } else {
    code_val = (uint32_t)code;
  }
  if (getenv("GIMG_JPEG_DEBUG_BASELINE_ENCODE") && num_bits == 4) {
    (void)fprintf(stderr, "BASELINE_ENCODE write_bits code=%u num_bits=%d w->bit_off=%d w->cache=0x%02x w->size=%zu\n",
        (unsigned)code_val, num_bits, w->bit_off, (unsigned)w->cache, (size_t)w->size);
    (void)fflush(stderr);
  }
  const int trace_enc = (getenv("GIMG_JPEG_TRACE_ENC") && getenv("GIMG_JPEG_TRACE_ENC")[0] == '1');
  while (num_bits > 0) {
    int shift = num_bits - 1;
    int bit = (int)((code_val >> shift) & 1u);
    uint8_t cache_before = w->cache;
    w->cache = (uint8_t)((w->cache << 1) | (unsigned)bit);
    w->bit_off++;
    if (trace_enc && num_bits <= 4 && w->size >= 3u && w->size <= 7u) {
      (void)fprintf(stderr,
          "TRACE_ENC write_bits code=%u remaining_bits=%d bit=%d cache_before=0x%02x bit_off_before=%d -> cache=0x%02x bit_off=%d size=%zu\n",
          (unsigned)code_val, num_bits, bit, (unsigned)cache_before,
          w->bit_off - 1, (unsigned)w->cache, w->bit_off, (size_t)w->size);
      (void)fflush(stderr);
    }
    if (w->bit_off == 8) {
      if (trace_enc && w->size >= 4u && w->size <= 6u) {
        (void)fprintf(stderr, "TRACE_ENC flush to byte[%zu]=0x%02x\n",
            (size_t)w->size, (unsigned)w->cache);
        (void)fflush(stderr);
      }
      jpeg_bitstream_write_byte(w, w->cache, alloc);
      w->bit_off = 0;
      w->cache = 0;
      if (w->data_base) {
        code_val = *(uint32_t *)w->data_base;
      }
    }
    num_bits--;
  }
}

static void jpeg_bitstream_write_flush(
    gimg_jpeg_bitstream_write_t * w, const GIMG_Allocator * alloc) {
  if (w->bit_off > 0) {
    while (w->bit_off < 8) {
      w->cache = (uint8_t)(w->cache << 1);
      w->bit_off++;
    }
    /* Write final byte without byte stuffing (T.81 B.2.2: 0x00 after 0xFF
     * is only for stuffing when more data follows; at end of segment do not
     * emit the stuffing byte so the next marker is immediately 0xFF). */
    if (w->size >= w->alloc_size) {
      size_t new_size = w->alloc_size * 2;
      unsigned char * p = (unsigned char *)gimg_realloc(
          gimg_alloc_or_default(alloc), w->data, new_size);
      if (p) {
        w->data = p;
        w->alloc_size = new_size;
      }
    }
    if (w->size < w->alloc_size) {
      w->data[w->size++] = (unsigned char)w->cache;
    }
  }
  /* If buffer ends with 0xFF 0x00 (stuffing after last 0xFF), remove the
   * stuffing byte so the next marker can follow immediately (libjpeg and
   * T.81 B.2.2: no stuffing byte at end of entropy-coded segment). */
  if (w->size >= 2 && w->data[w->size - 2] == 0xFF && w->data[w->size - 1] == 0x00) {
    w->size--;
  }
  /* If buffer ends with 0xFF 0x00 0x00 (stuffing then padding byte), remove
   * both trailing 0x00 so the segment ends with 0xFF. */
  if (w->size >= 3 && w->data[w->size - 3] == 0xFF && w->data[w->size - 2] == 0x00 &&
      w->data[w->size - 1] == 0x00) {
    w->size -= 2;
  }
}

/** Emit RSTm marker (0xFF 0xD0..0xD7) at byte boundary. Call after flush. No
 * byte stuffing: the second byte is the marker. */
static void jpeg_bitstream_write_rst(gimg_jpeg_bitstream_write_t * w,
    uint32_t mcu_index, const GIMG_Allocator * alloc) {
  jpeg_bitstream_write_flush(w, alloc);
  if (w->size + 2 > w->alloc_size) {
    size_t new_size = w->alloc_size * 2;
    if (new_size < w->size + 2) {
      new_size = w->size + 2;
    }
    unsigned char * p = (unsigned char *)gimg_realloc(
        gimg_alloc_or_default(alloc), w->data, new_size);
    if (!p) {
      return;
    }
    w->data = p;
    w->alloc_size = new_size;
  }
  w->data[w->size++] = 0xFF;
  w->data[w->size++] = (unsigned char)(0xD0 + (mcu_index % 8));
}

/** Encode table: symbol -> (code, length). */
typedef struct {
  uint16_t code[256];
  uint8_t len[256];
} gimg_jpeg_huff_enc_t;

/** Build encode table from DHT-style bits (16 counts) and values. */
static void jpeg_build_huff_enc(const unsigned char * bits,
    const unsigned char * values, int num_values, gimg_jpeg_huff_enc_t * enc) {
  uint16_t code = 0;
  int idx = 0;
  for (int len = 1; len <= 16; len++) {
    uint8_t count = bits[len - 1];
    for (int k = 0; k < count && idx < num_values; k++) {
      uint8_t sym = values[idx++];
      enc->code[sym] = (uint16_t)code;
      enc->len[sym] = (uint8_t)len;
      code++;
    }
    code = (uint16_t)(code << 1);
  }
}

/** Number of bits for signed value (Table K.2 extend). */
static int jpeg_nbits(int val) {
  if (val < 0) {
    val = -val;
  }
  int n = 0;
  while (val > 0) {
    n++;
    val >>= 1;
  }
  return n;
}

/** Encode one 8x8 block (DC + AC). DC predictor updated. max_dc_cat and
 * max_ac_size clamp category/size for 8-bit (11/10); use 16/15 for 12/16-bit.
 */
static GIMG_Result jpeg_encode_block_ex(gimg_jpeg_bitstream_write_t * w,
    const int16_t * block_zz, const gimg_jpeg_huff_enc_t * dc_enc,
    const gimg_jpeg_huff_enc_t * ac_enc, int32_t * dc_pred, int max_dc_cat,
    int max_ac_size, const GIMG_Allocator * alloc) {
  int32_t dc_val = block_zz[0];
  int32_t diff = dc_val - *dc_pred;
  *dc_pred = dc_val;
  int cat = jpeg_nbits(diff);
  if (cat > max_dc_cat) {
    cat = max_dc_cat;
  }
  jpeg_bitstream_write_bits(w, dc_enc->code[cat], dc_enc->len[cat], alloc);
  if (cat > 0) {
    unsigned extra =
        (diff < 0) ? (unsigned)(diff + (int)(1u << cat) - 1) : (unsigned)diff;
    jpeg_bitstream_write_bits(w, extra, cat, alloc);
  }
  int k = 1;
  while (k < 64) {
    int run = 0;
    while (k < 64 && block_zz[k] == 0) {
      run++;
      k++;
    }
    if (k >= 64) {
      break;
    }
    int ac = block_zz[k];
    int size = jpeg_nbits(ac);
    if (size > max_ac_size) {
      size = max_ac_size;
    }
    uint8_t sym = (uint8_t)((run << 4) | size);
    if (ac_enc->len[sym] > 0) {
      jpeg_bitstream_write_bits(w, ac_enc->code[sym], ac_enc->len[sym], alloc);
      if (size > 0) {
        unsigned extra =
            (ac < 0) ? (unsigned)(ac + (int)(1u << size) - 1) : (unsigned)ac;
        jpeg_bitstream_write_bits(w, extra, size, alloc);
      }
    }
    k++;
  }
  {
    /* EOB: luminance 4-bit 1010 (10), chrominance 4-bit 0100 (4) per T.81 K.4/K.6. */
    uint8_t eob_len = ac_enc->len[0];
    uint16_t eob_code = (uint16_t)ac_enc->code[0];
    if (getenv("GIMG_JPEG_DEBUG_BASELINE_ENCODE")) {
      (void)fprintf(stderr, "BASELINE_ENCODE EOB in block code=0x%x len=%u ac_enc=%p\n",
          (unsigned)eob_code, (unsigned)eob_len, (void *)ac_enc);
      (void)fflush(stderr);
    }
    if (getenv("GIMG_JPEG_DEBUG_SCAN_BYTES") && getenv("GIMG_JPEG_DEBUG_SCAN_BYTES")[0] == '1') {
      (void)fprintf(stderr, "BASELINE_ENCODE EOB write code=0x%x len=%u (binary: ", (unsigned)eob_code, (unsigned)eob_len);
      for (int i = eob_len - 1; i >= 0; i--) {
        (void)fprintf(stderr, "%u", (unsigned)((eob_code >> i) & 1));
      }
      (void)fprintf(stderr, ") ac_enc=%p\n", (void *)ac_enc);
      (void)fflush(stderr);
    }
    jpeg_bitstream_write_bits(w, eob_code, (int)eob_len, alloc);
  }
  return GIMG_OK;
}

static GIMG_Result jpeg_encode_block(gimg_jpeg_bitstream_write_t * w,
    const int16_t * block_zz, const gimg_jpeg_huff_enc_t * dc_enc,
    const gimg_jpeg_huff_enc_t * ac_enc, int32_t * dc_pred,
    const GIMG_Allocator * alloc) {
  return jpeg_encode_block_ex(
      w, block_zz, dc_enc, ac_enc, dc_pred, 11, 10, alloc);
}

/** Encode DC only (progressive first scan). max_dc_cat: 11 for 8-bit, 16 for
 * extended. */
static void jpeg_encode_block_dc_only_ex(gimg_jpeg_bitstream_write_t * w,
    const int16_t * block_zz, const gimg_jpeg_huff_enc_t * dc_enc,
    int32_t * dc_pred, int max_dc_cat, const GIMG_Allocator * alloc) {
  int32_t dc_val = block_zz[0];
  int32_t diff = dc_val - *dc_pred;
  *dc_pred = dc_val;
  int cat = jpeg_nbits(diff);
  if (cat > max_dc_cat) {
    cat = max_dc_cat;
  }
  jpeg_bitstream_write_bits(w, dc_enc->code[cat], dc_enc->len[cat], alloc);
  if (cat > 0) {
    unsigned extra =
        (diff < 0) ? (unsigned)(diff + (int)(1u << cat) - 1) : (unsigned)diff;
    jpeg_bitstream_write_bits(w, extra, cat, alloc);
  }
}

static void jpeg_encode_block_dc_only(gimg_jpeg_bitstream_write_t * w,
    const int16_t * block_zz, const gimg_jpeg_huff_enc_t * dc_enc,
    int32_t * dc_pred, const GIMG_Allocator * alloc) {
  jpeg_encode_block_dc_only_ex(w, block_zz, dc_enc, dc_pred, 11, alloc);
}

/** Encode DC refinement (Ss=0, Se=0, Ah>0): one bit per block. Decoder sets
 *  bit at position Al per T.81 Annex G.1.1.2.1; encoder sends (v>>Al)&1 for
 *  round-trip (Ah-1 = Al for the first refinement scan). */
static void jpeg_encode_block_dc_refine(gimg_jpeg_bitstream_write_t * w,
    int16_t dc_val, uint8_t Ah, const GIMG_Allocator * alloc) {
  int32_t v = (int32_t)dc_val;
  if (v < 0) {
    v = -v;
  }
  // T.81 G.1.1.2.1: refinement scan sends bit at Al; for this scan Al = Ah-1.
  unsigned bitpos = (Ah >= 1u && Ah <= 16u) ? (Ah - 1u) : 0u;
  unsigned bit = (unsigned)((v >> bitpos) & 1u);
  jpeg_bitstream_write_bits(w, bit, 1, alloc);
}

/** Encode AC band only. max_ac_size: 10 for 8-bit, 15 for extended.
 * If symbol (run,size) is not in the table (e.g. chroma extended has 242 of
 * 256), emit fallback (15,15) + 15 bits so the decoder stays in sync. */
static void jpeg_encode_block_ac_band_ex(gimg_jpeg_bitstream_write_t * w,
    const int16_t * block_zz, int ss, int se,
    const gimg_jpeg_huff_enc_t * ac_enc, int max_ac_size,
    const GIMG_Allocator * alloc) {
  static const uint8_t kFallbackSym = 0xFF; // (15,15) in extended table
  int k = ss;
  while (k <= se) {
    int run = 0;
    while (k <= se && block_zz[k] == 0) {
      run++;
      k++;
    }
    if (k > se) {
      break;
    }
    int ac = block_zz[k];
    int size = jpeg_nbits(ac);
    if (size > max_ac_size) {
      size = max_ac_size;
    }
    uint8_t sym = (uint8_t)((run << 4) | size);
    if (ac_enc->len[sym] == 0) {
      sym = kFallbackSym;
      size = 15;
      if (ac_enc->len[sym] == 0) {
        k++;
        continue;
      }
    }
    jpeg_bitstream_write_bits(w, ac_enc->code[sym], ac_enc->len[sym], alloc);
    if (size > 0) {
      int nbits = size;
      int val = ac;
      if (sym == kFallbackSym) {
        nbits = 15;
        if (val < -(1 << 14))
          val = -(1 << 14);
        else if (val >= (1 << 14))
          val = (1 << 14) - 1;
      }
      unsigned extra =
          (val < 0) ? (unsigned)(val + (int)(1u << nbits) - 1) : (unsigned)val;
      jpeg_bitstream_write_bits(w, extra, nbits, alloc);
    }
    k++;
  }
  jpeg_bitstream_write_bits(w, ac_enc->code[0], ac_enc->len[0], alloc);
}

static void jpeg_encode_block_ac_band(gimg_jpeg_bitstream_write_t * w,
    const int16_t * block_zz, int ss, int se,
    const gimg_jpeg_huff_enc_t * ac_enc, const GIMG_Allocator * alloc) {
  jpeg_encode_block_ac_band_ex(w, block_zz, ss, se, ac_enc, 10, alloc);
}

/** Encode AC refinement (Ah>0) per T.81 Annex G.1.2.2.
 *
 * Bitstream order for (run, ssss=1): [refinement bit for new coefficient]
 * then [one correction bit per already-nonzero coefficient while advancing
 * over `run` zeros]. Decoder reads refinement bit first, then advances k
 * (decrementing run for zeros, reading correction bit for already-nonzero).
 *
 * - "Newly nonzero" = magnitude exactly 1<<(Ah-1) (only refinement bit set).
 * - "Already nonzero" = magnitude > 1<<(Ah-1); we emit one correction bit
 *   (1 = set bit at Al, 0 = leave unchanged) when the decoder passes it.
 */
static void jpeg_encode_block_ac_band_refine(gimg_jpeg_bitstream_write_t * w,
    const int16_t * block_zz, int ss, int se, uint8_t Ah,
    const gimg_jpeg_huff_enc_t * ac_refine_enc, const GIMG_Allocator * alloc) {
  unsigned bitpos = (Ah >= 1u && Ah <= 16u) ? (Ah - 1u) : 0u;
  int k = ss;
  int run = 0;
  while (k <= se) {
    if (block_zz[k] == 0) {
      run++;
      k++;
      continue;
    }
    int32_t v = (int32_t)block_zz[k];
    if (v < 0) {
      v = -v;
    }
    if ((unsigned)v == (1u << bitpos)) {
      /* Newly nonzero: emit (run, 1) then refinement bit (sign: 1 = +, 0 = −). */
      while (run > 15) {
        uint8_t sym = 0xf0;
        jpeg_bitstream_write_bits(
            w, ac_refine_enc->code[sym], ac_refine_enc->len[sym], alloc);
        run -= 15;
      }
      uint8_t sym = (uint8_t)((run << 4) | 0x01u);
      if (ac_refine_enc->len[sym] > 0) {
        jpeg_bitstream_write_bits(
            w, ac_refine_enc->code[sym], ac_refine_enc->len[sym], alloc);
      }
      unsigned bit = (unsigned)((block_zz[k] > 0) ? 1u : 0u);
      jpeg_bitstream_write_bits(w, bit, 1, alloc);
      run = 0;
      k++;
    } else {
      /* Already nonzero: emit one correction bit (set bit at Al or not). */
      unsigned bit = (unsigned)((v >> bitpos) & 1u);
      jpeg_bitstream_write_bits(w, bit, 1, alloc);
      k++;
    }
  }
  jpeg_bitstream_write_bits(
      w, ac_refine_enc->code[0], ac_refine_enc->len[0], alloc);
}

/* Standard Huffman: DC luminance (ITU-T T.81 / ISO/IEC 10918-1 Table K.3), 12 symbols.
 * Code lengths: 1×2-bit (cat 0), 5×3-bit (cat 1–5), 1×4-bit, 1×5-bit, …
 * bits[i] = count of codes of length (i+1). */
static const unsigned char jpeg_std_bits_dc_lum[16] = {
    0,
    1,  /* one 2-bit code (category 0 → 00), T.81 Table K.3 */
    5,  /* five 3-bit codes (categories 1–5) */
    1,
    1,
    1,
    1,
    1,
    1,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
};
static const unsigned char jpeg_std_vals_dc_lum[12] = {
    0,
    1,
    2,
    3,
    4,
    5,
    6,
    7,
    8,
    9,
    10,
    11,
};

/* AC luminance (T.81 Table K.4): 162 symbols; we use first 37 in DHT (sum of bits). */
static const unsigned char jpeg_std_bits_ac_lum[16] = {
    0,
    0,
    2,
    1,
    3,
    3,
    2,
    4,
    3,
    5,
    5,
    4,
    4,
    0,
    0,
    1,
};
/* Order matches T.81 Table K.4: by length, then canonical order. Length 3: (0,1),(0,2);
 * length 4: EOB (0,0) = 4-bit code 1010; length 5: (0,3),(0,4)... So EOB at index 2
 * (the single 4-bit symbol) so jpeg_build_huff_enc assigns code[sym=0x00]=1010, len=4. */
static const unsigned char jpeg_std_vals_ac_lum[162] = {
    0x01,
    0x02,
    0x00, /* EOB: first 4-bit code 1010 (T.81 Table K.4) */
    0x03,
    0x04,
    0x11,
    0x05,
    0x12,
    0x21,
    0x31,
    0x41,
    0x06,
    0x13,
    0x51,
    0x61,
    0x07,
    0x22,
    0x71,
    0x14,
    0x32,
    0x81,
    0x91,
    0xa1,
    0x08,
    0x23,
    0x42,
    0xb1,
    0xc1,
    0x15,
    0x52,
    0xd1,
    0xf0,
    0x24,
    0x33,
    0x62,
    0x72,
    0x82,
    0x09,
    0x0a,
    0x16,
    0x17,
    0x18,
    0x19,
    0x1a,
    0x25,
    0x26,
    0x27,
    0x28,
    0x29,
    0x2a,
    0x34,
    0x35,
    0x36,
    0x37,
    0x38,
    0x39,
    0x3a,
    0x43,
    0x44,
    0x45,
    0x46,
    0x47,
    0x48,
    0x49,
    0x4a,
    0x53,
    0x54,
    0x55,
    0x56,
    0x57,
    0x58,
    0x59,
    0x5a,
    0x63,
    0x64,
    0x65,
    0x66,
    0x67,
    0x68,
    0x69,
    0x6a,
    0x73,
    0x74,
    0x75,
    0x76,
    0x77,
    0x78,
    0x79,
    0x7a,
    0x83,
    0x84,
    0x85,
    0x86,
    0x87,
    0x88,
    0x89,
    0x8a,
    0x92,
    0x93,
    0x94,
    0x95,
    0x96,
    0x97,
    0x98,
    0x99,
    0x9a,
    0xa2,
    0xa3,
    0xa4,
    0xa5,
    0xa6,
    0xa7,
    0xa8,
    0xa9,
    0xaa,
    0xb2,
    0xb3,
    0xb4,
    0xb5,
    0xb6,
    0xb7,
    0xb8,
    0xb9,
    0xba,
    0xc2,
    0xc3,
    0xc4,
    0xc5,
    0xc6,
    0xc7,
    0xc8,
    0xc9,
    0xca,
    0xd2,
    0xd3,
    0xd4,
    0xd5,
    0xd6,
    0xd7,
    0xd8,
    0xd9,
    0xda,
    0xe1,
    0xe2,
    0xe3,
    0xe4,
    0xe5,
    0xe6,
    0xe7,
    0xe8,
    0xe9,
    0xea,
    0xf1,
    0xf2,
    0xf3,
    0xf4,
    0xf5,
    0xf6,
    0xf7,
    0xf8,
    0xf9,
    0xfa,
};

/** Default AC table (T.81 Table K.4) as DHT payload. Length = 1 + 16 + 37 = 54
 * (T.81 Annex C: Lh = 2 + (1 + 16 + sum(BITS)); sum(bits_ac_lum)=37).
 * Used when no AC DHT appeared before the first SOS. */
static const unsigned char * jpeg_default_ac_dht_payload(size_t * out_len) {
  static unsigned char dht_buf[1 + 16 + 37];
  static int initialized = 0;
  if (!initialized) {
    dht_buf[0] = 0x10;  // Tc=1 (AC), Th=0
    memcpy(dht_buf + 1, jpeg_std_bits_ac_lum, 16);
    memcpy(dht_buf + 17, jpeg_std_vals_ac_lum, 37);
    initialized = 1;
  }
  *out_len = sizeof(dht_buf);
  return dht_buf;
}

/** Pillow/libjpeg compat: non-canonical 2-symbol AC table. Encoder uses
 * 00=(0,4), 10=EOB (code value 2). Canonical would assign 00 and 01. */
static void jpeg_build_pillow_compat_ac_scan1_table(gimg_jpeg_huff_table_t * tbl) {
  memset(tbl, 0, sizeof(*tbl));
  for (int len = 1; len <= 16; len++) {
    tbl->min_code[len] = 1;
    tbl->max_code[len] = 0;
  }
  tbl->min_code[2] = 0;
  tbl->max_code[2] = 2;
  tbl->base_index[2] = 0;
  tbl->values[0] = 0x04;  // (0,4) for code 00
  tbl->values[1] = 0x00;  // unused (code 01 not used by Pillow)
  tbl->values[2] = 0x00;  // EOB for code 10
  tbl->num_values = 3;
}

/* DC chrominance (T.81 Table K.5): 12 symbols; 3×3-bit, 1×4-bit, … */
static const unsigned char jpeg_std_bits_dc_chrom[16] = {
    0,
    0,
    3,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    0,
    0,
    0,
    0,
};
static const unsigned char jpeg_std_vals_dc_chrom[12] = {
    0,
    1,
    2,
    3,
    4,
    5,
    6,
    7,
    8,
    9,
    10,
    11,
};

/* AC chrominance (T.81 Table K.6): bits[] has 2 at len 3, 1 at len 4. Canonical
 * assigns 000/001 to first two symbols, 100 to the first 4-bit symbol. Decoder
 * reads EOB as bits 4,5,6 of byte after DC (100). So EOB must be at index 2. */
static const unsigned char jpeg_std_bits_ac_chrom[16] = {
    0,
    0,
    2,
    1,
    2,
    4,
    4,
    3,
    4,
    7,
    5,
    4,
    4,
    0,
    1,
    2,
};
static const unsigned char jpeg_std_vals_ac_chrom[162] = {
    0x01,
    0x02,
    0x00, /* EOB: first 4-bit code 100 so encoder emits 100, decoder in sync */
    0x03,
    0x11,
    0x04,
    0x05,
    0x21,
    0x31,
    0x06,
    0x12,
    0x41,
    0x51,
    0x07,
    0x61,
    0x71,
    0x13,
    0x22,
    0x32,
    0x81,
    0x08,
    0x14,
    0x42,
    0x91,
    0xa1,
    0xb1,
    0xc1,
    0x09,
    0x23,
    0x33,
    0x52,
    0xf0,
    0x15,
    0x62,
    0x72,
    0xd1,
    0x0a,
    0x16,
    0x24,
    0x34,
    0xe1,
    0x25,
    0xf1,
    0x17,
    0x18,
    0x19,
    0x1a,
    0x26,
    0x27,
    0x28,
    0x29,
    0x2a,
    0x35,
    0x36,
    0x37,
    0x38,
    0x39,
    0x3a,
    0x43,
    0x44,
    0x45,
    0x46,
    0x47,
    0x48,
    0x49,
    0x4a,
    0x53,
    0x54,
    0x55,
    0x56,
    0x57,
    0x58,
    0x59,
    0x5a,
    0x63,
    0x64,
    0x65,
    0x66,
    0x67,
    0x68,
    0x69,
    0x6a,
    0x73,
    0x74,
    0x75,
    0x76,
    0x77,
    0x78,
    0x79,
    0x7a,
    0x82,
    0x83,
    0x84,
    0x85,
    0x86,
    0x87,
    0x88,
    0x89,
    0x8a,
    0x92,
    0x93,
    0x94,
    0x95,
    0x96,
    0x97,
    0x98,
    0x99,
    0x9a,
    0xa2,
    0xa3,
    0xa4,
    0xa5,
    0xa6,
    0xa7,
    0xa8,
    0xa9,
    0xaa,
    0xb2,
    0xb3,
    0xb4,
    0xb5,
    0xb6,
    0xb7,
    0xb8,
    0xb9,
    0xba,
    0xc2,
    0xc3,
    0xc4,
    0xc5,
    0xc6,
    0xc7,
    0xc8,
    0xc9,
    0xca,
    0xd2,
    0xd3,
    0xd4,
    0xd5,
    0xd6,
    0xd7,
    0xd8,
    0xd9,
    0xda,
    0xe2,
    0xe3,
    0xe4,
    0xe5,
    0xe6,
    0xe7,
    0xe8,
    0xe9,
    0xea,
    0xf2,
    0xf3,
    0xf4,
    0xf5,
    0xf6,
    0xf7,
    0xf8,
    0xf9,
    0xfa,
};

/** AC refinement (successive approximation): 17 symbols — EOB (0x00), run 0
 * (0x01), and (run, 0) for run 1..15. Valid prefix: 1 at len 2, 2 at len 4,
 * 14 at len 5. */
static const unsigned char jpeg_std_bits_ac_refine[16] = {
    0,
    0,
    1,
    0,
    2,
    14,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
};
static const unsigned char jpeg_std_vals_ac_refine[17] = {
    0x00,
    0x01,
    0x10,
    0x20,
    0x30,
    0x40,
    0x50,
    0x60,
    0x70,
    0x80,
    0x90,
    0xa0,
    0xb0,
    0xc0,
    0xd0,
    0xe0,
    0xf0,
};

/** Extended precision (12/16-bit): DC 0..16 (5 extra symbols at length 12). */
static const unsigned char jpeg_ext_bits_dc_lum[16] = {
    0,
    0,
    1,
    5,
    1,
    1,
    1,
    1,
    1,
    1,
    0,
    5,
    0,
    0,
    0,
    0,
};
static const unsigned char jpeg_ext_vals_dc_lum[17] = {
    0,
    1,
    2,
    3,
    4,
    5,
    6,
    7,
    8,
    9,
    10,
    11,
    12,
    13,
    14,
    15,
    16,
};
static const unsigned char jpeg_ext_bits_dc_chrom[16] = {
    0,
    0,
    3,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    5,
    0,
    0,
    0,
    0,
};
static const unsigned char jpeg_ext_vals_dc_chrom[17] = {
    0,
    1,
    2,
    3,
    4,
    5,
    6,
    7,
    8,
    9,
    10,
    11,
    12,
    13,
    14,
    15,
    16,
};
/** Extended AC: 242 symbols (162 standard + 80 for size 11..15). Length-16
 * count set so sum(bits)=242 so DHT payload is 1+16+242=259 bytes. */
static const unsigned char jpeg_ext_bits_ac_lum[16] = {
    0,
    0,
    2,
    1,
    3,
    3,
    2,
    4,
    3,
    5,
    5,
    4,
    4,
    0,
    0,
    206,
};
static const unsigned char jpeg_ext_bits_ac_chrom[16] = {
    0,
    0,
    2,
    1,
    2,
    4,
    4,
    3,
    4,
    7,
    5,
    4,
    4,
    0,
    1,
    201,
};
// Extended AC values: 162 standard then (run<<4|size) for run 0..15,
// size 11..15.
static unsigned char jpeg_ext_vals_ac_lum[242];
static unsigned char jpeg_ext_vals_ac_chrom[242];
static int jpeg_ext_ac_vals_initialized;

static void jpeg_init_ext_ac_vals(void) {
  if (jpeg_ext_ac_vals_initialized) {
    return;
  }
  memcpy(jpeg_ext_vals_ac_lum, jpeg_std_vals_ac_lum, 162);
  memcpy(jpeg_ext_vals_ac_chrom, jpeg_std_vals_ac_chrom, 162);
  for (int run = 0; run < 16; run++) {
    for (int size = 11; size <= 15; size++) {
      jpeg_ext_vals_ac_lum[162 + run * 5 + (size - 11)] =
          (unsigned char)((run << 4) | size);
      jpeg_ext_vals_ac_chrom[162 + run * 5 + (size - 11)] =
          (unsigned char)((run << 4) | size);
    }
  }
  jpeg_ext_ac_vals_initialized = 1;
}

GIMG_Result gimg_jpeg_encode_baseline_scan(uint32_t width, uint32_t height,
    int num_components, const unsigned char * comp0,
    const unsigned char * comp1, const unsigned char * comp2, size_t stride0,
    size_t stride1, size_t stride2, const uint8_t * h_samp,
    const uint8_t * v_samp, const uint16_t * quant_luma,
    const uint16_t * quant_chroma, const GIMG_Allocator * alloc,
    uint16_t restart_interval, unsigned char ** out_scan_data,
    size_t * out_scan_size) {
  if (!out_scan_data || !out_scan_size || width == 0 || height == 0) {
    return GIMG_ERR_INTERNAL;
  }
  if (num_components != 1 && num_components != 3) {
    return GIMG_ERR_UNSUPPORTED;
  }
  const GIMG_Allocator * a = gimg_alloc_or_default(alloc);

  uint8_t hs[3] = {1, 1, 1};
  uint8_t vs[3] = {1, 1, 1};
  if (h_samp) {
    for (int i = 0; i < num_components; i++) {
      hs[i] = h_samp[i] ? h_samp[i] : 1;
    }
  }
  if (v_samp) {
    for (int i = 0; i < num_components; i++) {
      vs[i] = v_samp[i] ? v_samp[i] : 1;
    }
  }
  uint8_t h_max = hs[0];
  uint8_t v_max = vs[0];
  if (num_components >= 3) {
    if (hs[1] > h_max) {
      h_max = hs[1];
    }
    if (hs[2] > h_max) {
      h_max = hs[2];
    }
    if (vs[1] > v_max) {
      v_max = vs[1];
    }
    if (vs[2] > v_max) {
      v_max = vs[2];
    }
  }

  gimg_jpeg_huff_enc_t dc_lum, dc_chrom, ac_lum, ac_chrom;
  memset(&dc_lum, 0, sizeof(dc_lum));
  memset(&dc_chrom, 0, sizeof(dc_chrom));
  memset(&ac_lum, 0, sizeof(ac_lum));
  memset(&ac_chrom, 0, sizeof(ac_chrom));
  jpeg_build_huff_enc(jpeg_std_bits_dc_lum, jpeg_std_vals_dc_lum, 12, &dc_lum);
  jpeg_build_huff_enc(jpeg_std_bits_ac_lum, jpeg_std_vals_ac_lum, 162, &ac_lum);
  jpeg_build_huff_enc(
      jpeg_std_bits_dc_chrom, jpeg_std_vals_dc_chrom, 12, &dc_chrom);
  jpeg_build_huff_enc(
      jpeg_std_bits_ac_chrom, jpeg_std_vals_ac_chrom, 162, &ac_chrom);

  gimg_jpeg_bitstream_write_t w;
  jpeg_bitstream_write_init(&w, a);
  if (!w.data) {
    return GIMG_ERR_OOM;
  }

  uint32_t mcu_w = (uint32_t)(8 * h_max);
  uint32_t mcu_h = (uint32_t)(8 * v_max);
  uint32_t mcu_per_row = (width + mcu_w - 1) / mcu_w;
  uint32_t mcu_per_col = (height + mcu_h - 1) / mcu_h;
  int32_t dc_pred[3] = {0, 0, 0};
  int16_t block_rm[64];
  int16_t block_zz[64];
  const unsigned char * comps[3] = {comp0, comp1, comp2};
  size_t strides[3] = {stride0, stride1, stride2};
  const uint16_t * quants[3] = {quant_luma, quant_chroma, quant_chroma};
  const gimg_jpeg_huff_enc_t * dc_tbls[3] = {&dc_lum, &dc_chrom, &dc_chrom};
  const gimg_jpeg_huff_enc_t * ac_tbls[3] = {&ac_lum, &ac_chrom, &ac_chrom};

  for (uint32_t mcu_y = 0; mcu_y < mcu_per_col; mcu_y++) {
    for (uint32_t mcu_x = 0; mcu_x < mcu_per_row; mcu_x++) {
      uint32_t mcu_index = mcu_y * mcu_per_row + mcu_x;
      if (restart_interval > 0 && mcu_index > 0 &&
          (mcu_index % (uint32_t)restart_interval) == 0) {
        jpeg_bitstream_write_rst(&w, mcu_index, a);
        memset(dc_pred, 0, sizeof(dc_pred));
      }
      for (int c = 0; c < num_components; c++) {
        uint8_t hc = hs[c];
        uint8_t vc = vs[c];
        uint32_t comp_width = 8 * ((width * (uint32_t)hc + mcu_w - 1) / mcu_w);
        if (comp_width == 0) {
          comp_width = 8;
        }
        uint32_t comp_height =
            8 * ((height * (uint32_t)vc + mcu_h - 1) / mcu_h);
        if (comp_height == 0) {
          comp_height = 8;
        }
        for (uint8_t by = 0; by < vc; by++) {
          for (uint8_t bx = 0; bx < hc; bx++) {
            uint32_t blk_x = mcu_x * 8 * (uint32_t)hc + (uint32_t)bx * 8;
            uint32_t blk_y = mcu_y * 8 * (uint32_t)vc + (uint32_t)by * 8;
            const unsigned char * row_ptr =
                comps[c] + (size_t)blk_y * strides[c];
            const uint16_t * q = quants[c];
            for (int dy = 0; dy < 8; dy++) {
              uint32_t y = blk_y + (uint32_t)dy;
              if (y >= comp_height) {
                for (int dx = 0; dx < 8; dx++) {
                  block_rm[dy * 8 + dx] = 0;
                }
                continue;
              }
              const unsigned char * p =
                  row_ptr + (size_t)dy * strides[c] + (size_t)blk_x;
              for (int dx = 0; dx < 8; dx++) {
                uint32_t x = blk_x + (uint32_t)dx;
                int16_t s;
                if (x < comp_width && y < comp_height &&
                    (blk_y + (uint32_t)dy) < height &&
                    (size_t)(blk_x + (uint32_t)dx) < strides[c]) {
                  s = (int16_t)((int)p[dx] - 128);
                }
                else {
                  s = 0;
                }
                block_rm[dy * 8 + dx] = s;
              }
            }
            jpeg_fdct_8x8(block_rm);
            jpeg_quantise(block_rm, q);
            jpeg_zigzag_encode(block_rm, block_zz);
            jpeg_encode_block(
                &w, block_zz, dc_tbls[c], ac_tbls[c], &dc_pred[c], a);
          }
        }
      }
    }
  }

  jpeg_bitstream_write_flush(&w, a);
  *out_scan_data = w.data;
  *out_scan_size = w.size;
  return GIMG_OK;
}

/** Baseline encode for 12/16-bit: uint16_t components, level shift, extended
 * DHT. precision 12: level shift 2048; 16: level shift 32768. */
GIMG_Result gimg_jpeg_encode_baseline_scan_16bit(uint32_t width,
    uint32_t height, int num_components, const uint16_t * comp0,
    const uint16_t * comp1, const uint16_t * comp2, size_t stride0,
    size_t stride1, size_t stride2, const uint8_t * h_samp,
    const uint8_t * v_samp, const uint16_t * quant_luma,
    const uint16_t * quant_chroma, int precision, const GIMG_Allocator * alloc,
    uint16_t restart_interval, unsigned char ** out_scan_data,
    size_t * out_scan_size) {
  if (!out_scan_data || !out_scan_size || width == 0 || height == 0) {
    return GIMG_ERR_INTERNAL;
  }
  if (num_components != 1 && num_components != 3) {
    return GIMG_ERR_UNSUPPORTED;
  }
  if (precision != 12 && precision != 16) {
    return GIMG_ERR_UNSUPPORTED;
  }
  int level_shift = (precision == 12) ? 2048 : 32768;
  const GIMG_Allocator * a = gimg_alloc_or_default(alloc);
  jpeg_init_ext_ac_vals();

  uint8_t hs[3] = {1, 1, 1};
  uint8_t vs[3] = {1, 1, 1};
  if (h_samp) {
    for (int i = 0; i < num_components; i++) {
      hs[i] = h_samp[i] ? h_samp[i] : 1;
    }
  }
  if (v_samp) {
    for (int i = 0; i < num_components; i++) {
      vs[i] = v_samp[i] ? v_samp[i] : 1;
    }
  }
  uint8_t h_max = hs[0];
  uint8_t v_max = vs[0];
  if (num_components >= 3) {
    if (hs[1] > h_max) {
      h_max = hs[1];
    }
    if (hs[2] > h_max) {
      h_max = hs[2];
    }
    if (vs[1] > v_max) {
      v_max = vs[1];
    }
    if (vs[2] > v_max) {
      v_max = vs[2];
    }
  }

  gimg_jpeg_huff_enc_t dc_lum, dc_chrom, ac_lum, ac_chrom;
  memset(&dc_lum, 0, sizeof(dc_lum));
  memset(&dc_chrom, 0, sizeof(dc_chrom));
  memset(&ac_lum, 0, sizeof(ac_lum));
  memset(&ac_chrom, 0, sizeof(ac_chrom));
  jpeg_build_huff_enc(jpeg_ext_bits_dc_lum, jpeg_ext_vals_dc_lum, 17, &dc_lum);
  jpeg_build_huff_enc(jpeg_ext_bits_ac_lum, jpeg_ext_vals_ac_lum, 242, &ac_lum);
  jpeg_build_huff_enc(
      jpeg_ext_bits_dc_chrom, jpeg_ext_vals_dc_chrom, 17, &dc_chrom);
  // Use same 242 symbol set as lum so chroma never emits a symbol not in table.
  jpeg_build_huff_enc(
      jpeg_ext_bits_ac_chrom, jpeg_ext_vals_ac_lum, 242, &ac_chrom);

  gimg_jpeg_bitstream_write_t w;
  jpeg_bitstream_write_init(&w, a);
  if (!w.data) {
    return GIMG_ERR_OOM;
  }

  uint32_t mcu_w = (uint32_t)(8 * h_max);
  uint32_t mcu_h = (uint32_t)(8 * v_max);
  uint32_t mcu_per_row = (width + mcu_w - 1) / mcu_w;
  uint32_t mcu_per_col = (height + mcu_h - 1) / mcu_h;
  int32_t dc_pred[3] = {0, 0, 0};
  int16_t block_rm[64];
  int16_t block_zz[64];
  const uint16_t * comps[3] = {comp0, comp1, comp2};
  size_t strides[3] = {stride0, stride1, stride2};
  const uint16_t * quants[3] = {quant_luma, quant_chroma, quant_chroma};
  const gimg_jpeg_huff_enc_t * dc_tbls[3] = {&dc_lum, &dc_chrom, &dc_chrom};
  const gimg_jpeg_huff_enc_t * ac_tbls[3] = {&ac_lum, &ac_chrom, &ac_chrom};

  for (uint32_t mcu_y = 0; mcu_y < mcu_per_col; mcu_y++) {
    for (uint32_t mcu_x = 0; mcu_x < mcu_per_row; mcu_x++) {
      uint32_t mcu_index = mcu_y * mcu_per_row + mcu_x;
      if (restart_interval > 0 && mcu_index > 0 &&
          (mcu_index % (uint32_t)restart_interval) == 0) {
        jpeg_bitstream_write_rst(&w, mcu_index, a);
        memset(dc_pred, 0, sizeof(dc_pred));
      }
      for (int c = 0; c < num_components; c++) {
        uint8_t hc = hs[c];
        uint8_t vc = vs[c];
        uint32_t comp_width = 8 * ((width * (uint32_t)hc + mcu_w - 1) / mcu_w);
        if (comp_width == 0) {
          comp_width = 8;
        }
        uint32_t comp_height =
            8 * ((height * (uint32_t)vc + mcu_h - 1) / mcu_h);
        if (comp_height == 0) {
          comp_height = 8;
        }
        for (uint8_t by = 0; by < vc; by++) {
          for (uint8_t bx = 0; bx < hc; bx++) {
            uint32_t blk_x = mcu_x * 8 * (uint32_t)hc + (uint32_t)bx * 8;
            uint32_t blk_y = mcu_y * 8 * (uint32_t)vc + (uint32_t)by * 8;
            const uint16_t * row_ptr = comps[c] + (size_t)blk_y * strides[c];
            const uint16_t * q = quants[c];
            for (int dy = 0; dy < 8; dy++) {
              uint32_t y = blk_y + (uint32_t)dy;
              if (y >= comp_height) {
                for (int dx = 0; dx < 8; dx++) {
                  block_rm[dy * 8 + dx] = 0;
                }
                continue;
              }
              const uint16_t * p =
                  row_ptr + (size_t)dy * strides[c] + (size_t)blk_x;
              for (int dx = 0; dx < 8; dx++) {
                uint32_t x = blk_x + (uint32_t)dx;
                int16_t s;
                if (x < comp_width && y < comp_height &&
                    (blk_y + (uint32_t)dy) < height &&
                    (size_t)(blk_x + (uint32_t)dx) < strides[c]) {
                  s = (int16_t)((int)p[dx] - level_shift);
                }
                else {
                  s = 0;
                }
                block_rm[dy * 8 + dx] = s;
              }
            }
            jpeg_fdct_8x8(block_rm);
            jpeg_quantise(block_rm, q);
            jpeg_zigzag_encode(block_rm, block_zz);
            jpeg_encode_block_ex(
                &w, block_zz, dc_tbls[c], ac_tbls[c], &dc_pred[c], 16, 15, a);
          }
        }
      }
    }
  }

  jpeg_bitstream_write_flush(&w, a);
  *out_scan_data = w.data;
  *out_scan_size = w.size;
  return GIMG_OK;
}

/** Fill coefficient buffer for progressive encode: DCT, quantise, zigzag; store
 * blocks in MCU order (same as baseline). Caller allocates coef_buffer for
 * total_blocks * 64 int16_t; total_blocks is returned in *out_total_blocks. */
GIMG_Result gimg_jpeg_progressive_fill_coef_buffer(uint32_t width,
    uint32_t height, int num_components, const unsigned char * comp0,
    const unsigned char * comp1, const unsigned char * comp2, size_t stride0,
    size_t stride1, size_t stride2, const uint8_t * h_samp,
    const uint8_t * v_samp, const uint16_t * quant_luma,
    const uint16_t * quant_chroma, int16_t * coef_buffer,
    size_t * out_total_blocks) {
  if (!coef_buffer || !out_total_blocks || width == 0 || height == 0) {
    return GIMG_ERR_INTERNAL;
  }
  if (num_components != 1 && num_components != 3) {
    return GIMG_ERR_UNSUPPORTED;
  }
  uint8_t hs[3] = {1, 1, 1};
  uint8_t vs[3] = {1, 1, 1};
  if (h_samp) {
    for (int i = 0; i < num_components; i++) {
      hs[i] = h_samp[i] ? h_samp[i] : 1;
    }
  }
  if (v_samp) {
    for (int i = 0; i < num_components; i++) {
      vs[i] = v_samp[i] ? v_samp[i] : 1;
    }
  }
  uint8_t h_max = hs[0];
  uint8_t v_max = vs[0];
  if (num_components >= 3) {
    if (hs[1] > h_max) {
      h_max = hs[1];
    }
    if (hs[2] > h_max) {
      h_max = hs[2];
    }
    if (vs[1] > v_max) {
      v_max = vs[1];
    }
    if (vs[2] > v_max) {
      v_max = vs[2];
    }
  }
  uint32_t mcu_w = (uint32_t)(8 * h_max);
  uint32_t mcu_h = (uint32_t)(8 * v_max);
  uint32_t mcu_per_row = (width + mcu_w - 1) / mcu_w;
  uint32_t mcu_per_col = (height + mcu_h - 1) / mcu_h;
  size_t blocks_per_mcu = 0;
  for (int c = 0; c < num_components; c++) {
    blocks_per_mcu += (size_t)hs[c] * (size_t)vs[c];
  }
  size_t total_blocks = 0;
  if (!gimg_safe_mul_size(
          (size_t)mcu_per_row, (size_t)mcu_per_col, &total_blocks) ||
      !gimg_safe_mul_size(total_blocks, blocks_per_mcu, &total_blocks)) {
    return GIMG_ERR_LIMIT;
  }
  *out_total_blocks = total_blocks;

  int16_t block_rm[64];
  int16_t block_zz[64];
  const unsigned char * comps[3] = {comp0, comp1, comp2};
  size_t strides[3] = {stride0, stride1, stride2};
  const uint16_t * quants[3] = {quant_luma, quant_chroma, quant_chroma};
  size_t block_offset = 0;

  for (uint32_t mcu_y = 0; mcu_y < mcu_per_col; mcu_y++) {
    for (uint32_t mcu_x = 0; mcu_x < mcu_per_row; mcu_x++) {
      for (int c = 0; c < num_components; c++) {
        uint8_t hc = hs[c];
        uint8_t vc = vs[c];
        uint32_t comp_width = 8 * ((width * (uint32_t)hc + mcu_w - 1) / mcu_w);
        if (comp_width == 0) {
          comp_width = 8;
        }
        uint32_t comp_height =
            8 * ((height * (uint32_t)vc + mcu_h - 1) / mcu_h);
        if (comp_height == 0) {
          comp_height = 8;
        }
        /* T.81 Annex A: X_c = ceil(X * H_c / H_max), Y_c = ceil(Y * V_c / V_max).
         * Replicate edge pixel for positions past component bounds so we do not
         * read past buffer and 1x1 yields a constant block (DC-only). */
        uint32_t comp_width_px =
            (width * (uint32_t)hc + h_max - 1) / (uint32_t)h_max;
        uint32_t comp_height_px =
            (height * (uint32_t)vc + v_max - 1) / (uint32_t)v_max;
        if (comp_width_px == 0) {
          comp_width_px = 1;
        }
        if (comp_height_px == 0) {
          comp_height_px = 1;
        }
        for (uint8_t by = 0; by < vc; by++) {
          for (uint8_t bx = 0; bx < hc; bx++) {
            uint32_t blk_x = mcu_x * 8 * (uint32_t)hc + (uint32_t)bx * 8;
            uint32_t blk_y = mcu_y * 8 * (uint32_t)vc + (uint32_t)by * 8;
            const uint16_t * q = quants[c];
            for (int dy = 0; dy < 8; dy++) {
              uint32_t y = blk_y + (uint32_t)dy;
              if (y >= comp_height) {
                for (int dx = 0; dx < 8; dx++) {
                  block_rm[dy * 8 + dx] = 0;
                }
                continue;
              }
              for (int dx = 0; dx < 8; dx++) {
                uint32_t x = blk_x + (uint32_t)dx;
                uint32_t sy = y < comp_height_px ? y : comp_height_px - 1u;
                uint32_t sx = x < comp_width_px ? x : comp_width_px - 1u;
                int16_t s =
                    (int16_t)((int)comps[c][(size_t)sy * strides[c] + (size_t)sx] -
                        128);
                block_rm[dy * 8 + dx] = s;
              }
            }
            jpeg_fdct_8x8(block_rm);
            jpeg_quantise(block_rm, q);
            jpeg_zigzag_encode(block_rm, block_zz);
            memcpy(coef_buffer + block_offset * 64, block_zz,
                64 * sizeof(int16_t));
            block_offset++;
          }
        }
      }
    }
  }
  return GIMG_OK;
}

/** Fill coefficient buffer for 12/16-bit progressive: uint16_t components,
 * level shift (2048 or 32768), 16-bit quant. */
GIMG_Result gimg_jpeg_progressive_fill_coef_buffer_16bit(uint32_t width,
    uint32_t height, int num_components, const uint16_t * comp0,
    const uint16_t * comp1, const uint16_t * comp2, size_t stride0,
    size_t stride1, size_t stride2, const uint8_t * h_samp,
    const uint8_t * v_samp, const uint16_t * quant_luma,
    const uint16_t * quant_chroma, int precision, int16_t * coef_buffer,
    size_t * out_total_blocks) {
  if (!coef_buffer || !out_total_blocks || width == 0 || height == 0) {
    return GIMG_ERR_INTERNAL;
  }
  if (num_components != 1 && num_components != 3) {
    return GIMG_ERR_UNSUPPORTED;
  }
  if (precision != 12 && precision != 16) {
    return GIMG_ERR_UNSUPPORTED;
  }
  int level_shift = (precision == 12) ? 2048 : 32768;
  uint8_t hs[3] = {1, 1, 1};
  uint8_t vs[3] = {1, 1, 1};
  if (h_samp) {
    for (int i = 0; i < num_components; i++) {
      hs[i] = h_samp[i] ? h_samp[i] : 1;
    }
  }
  if (v_samp) {
    for (int i = 0; i < num_components; i++) {
      vs[i] = v_samp[i] ? v_samp[i] : 1;
    }
  }
  uint8_t h_max = hs[0];
  uint8_t v_max = vs[0];
  if (num_components >= 3) {
    if (hs[1] > h_max) {
      h_max = hs[1];
    }
    if (hs[2] > h_max) {
      h_max = hs[2];
    }
    if (vs[1] > v_max) {
      v_max = vs[1];
    }
    if (vs[2] > v_max) {
      v_max = vs[2];
    }
  }
  uint32_t mcu_w = (uint32_t)(8 * h_max);
  uint32_t mcu_h = (uint32_t)(8 * v_max);
  uint32_t mcu_per_row = (width + mcu_w - 1) / mcu_w;
  uint32_t mcu_per_col = (height + mcu_h - 1) / mcu_h;
  size_t blocks_per_mcu = 0;
  for (int c = 0; c < num_components; c++) {
    blocks_per_mcu += (size_t)hs[c] * (size_t)vs[c];
  }
  size_t total_blocks = 0;
  if (!gimg_safe_mul_size(
          (size_t)mcu_per_row, (size_t)mcu_per_col, &total_blocks) ||
      !gimg_safe_mul_size(total_blocks, blocks_per_mcu, &total_blocks)) {
    return GIMG_ERR_LIMIT;
  }
  *out_total_blocks = total_blocks;

  int16_t block_rm[64];
  int16_t block_zz[64];
  const uint16_t * comps[3] = {comp0, comp1, comp2};
  size_t strides[3] = {stride0, stride1, stride2};
  const uint16_t * quants[3] = {quant_luma, quant_chroma, quant_chroma};
  size_t block_offset = 0;

  for (uint32_t mcu_y = 0; mcu_y < mcu_per_col; mcu_y++) {
    for (uint32_t mcu_x = 0; mcu_x < mcu_per_row; mcu_x++) {
      for (int c = 0; c < num_components; c++) {
        uint8_t hc = hs[c];
        uint8_t vc = vs[c];
        uint32_t comp_width = 8 * ((width * (uint32_t)hc + mcu_w - 1) / mcu_w);
        if (comp_width == 0) {
          comp_width = 8;
        }
        uint32_t comp_height =
            8 * ((height * (uint32_t)vc + mcu_h - 1) / mcu_h);
        if (comp_height == 0) {
          comp_height = 8;
        }
        for (uint8_t by = 0; by < vc; by++) {
          for (uint8_t bx = 0; bx < hc; bx++) {
            uint32_t blk_x = mcu_x * 8 * (uint32_t)hc + (uint32_t)bx * 8;
            uint32_t blk_y = mcu_y * 8 * (uint32_t)vc + (uint32_t)by * 8;
            const uint16_t * row_ptr = comps[c] + (size_t)blk_y * strides[c];
            const uint16_t * q = quants[c];
            for (int dy = 0; dy < 8; dy++) {
              uint32_t y = blk_y + (uint32_t)dy;
              if (y >= comp_height) {
                for (int dx = 0; dx < 8; dx++) {
                  block_rm[dy * 8 + dx] = 0;
                }
                continue;
              }
              const uint16_t * p =
                  row_ptr + (size_t)dy * strides[c] + (size_t)blk_x;
              for (int dx = 0; dx < 8; dx++) {
                uint32_t x = blk_x + (uint32_t)dx;
                int16_t s;
                if (x < comp_width && y < comp_height &&
                    (blk_y + (uint32_t)dy) < height &&
                    (size_t)(blk_x + (uint32_t)dx) < strides[c]) {
                  s = (int16_t)((int)p[dx] - level_shift);
                }
                else {
                  s = 0;
                }
                block_rm[dy * 8 + dx] = s;
              }
            }
            jpeg_fdct_8x8(block_rm);
            jpeg_quantise(block_rm, q);
            jpeg_zigzag_encode(block_rm, block_zz);
            memcpy(coef_buffer + block_offset * 64, block_zz,
                64 * sizeof(int16_t));
            block_offset++;
          }
        }
      }
    }
  }
  return GIMG_OK;
}

/** Encode one progressive scan from coefficient buffer. Block order must match
 * gimg_jpeg_progressive_fill_coef_buffer. Supports Ah>0 (refinement) for DC
 * and AC. */
GIMG_Result gimg_jpeg_encode_progressive_scan(uint32_t width, uint32_t height,
    int num_components, const int16_t * coef_buffer, size_t total_blocks,
    const uint8_t * h_samp, const uint8_t * v_samp, uint8_t Ss, uint8_t Se,
    uint8_t Ah, uint8_t Al, const GIMG_Allocator * alloc,
    uint16_t restart_interval, unsigned char ** out_scan_data,
    size_t * out_scan_size) {
  if (!out_scan_data || !out_scan_size || !coef_buffer) {
    return GIMG_ERR_INTERNAL;
  }
  (void)width;
  (void)height;
  (void)Al;
  uint8_t hs[3] = {1, 1, 1};
  uint8_t vs[3] = {1, 1, 1};
  if (h_samp) {
    for (int i = 0; i < num_components; i++) {
      hs[i] = h_samp[i] ? h_samp[i] : 1;
    }
  }
  if (v_samp) {
    for (int i = 0; i < num_components; i++) {
      vs[i] = v_samp[i] ? v_samp[i] : 1;
    }
  }
  uint8_t h_max = hs[0];
  uint8_t v_max = vs[0];
  if (num_components >= 3) {
    if (hs[1] > h_max) {
      h_max = hs[1];
    }
    if (hs[2] > h_max) {
      h_max = hs[2];
    }
    if (vs[1] > v_max) {
      v_max = vs[1];
    }
    if (vs[2] > v_max) {
      v_max = vs[2];
    }
  }
  size_t blocks_per_mcu = (num_components == 1)
      ? 1
      : (size_t)hs[0] * (size_t)vs[0] + (size_t)hs[1] * (size_t)vs[1] +
          (size_t)hs[2] * (size_t)vs[2];
  const GIMG_Allocator * a = gimg_alloc_or_default(alloc);
  gimg_jpeg_huff_enc_t dc_lum, dc_chrom, ac_lum, ac_chrom, ac_refine;
  memset(&dc_lum, 0, sizeof(dc_lum));
  memset(&dc_chrom, 0, sizeof(dc_chrom));
  memset(&ac_lum, 0, sizeof(ac_lum));
  memset(&ac_chrom, 0, sizeof(ac_chrom));
  memset(&ac_refine, 0, sizeof(ac_refine));
  jpeg_build_huff_enc(jpeg_std_bits_dc_lum, jpeg_std_vals_dc_lum, 12, &dc_lum);
  jpeg_build_huff_enc(jpeg_std_bits_ac_lum, jpeg_std_vals_ac_lum, 162, &ac_lum);
  jpeg_build_huff_enc(
      jpeg_std_bits_dc_chrom, jpeg_std_vals_dc_chrom, 12, &dc_chrom);
  jpeg_build_huff_enc(
      jpeg_std_bits_ac_chrom, jpeg_std_vals_ac_chrom, 162, &ac_chrom);
  jpeg_build_huff_enc(
      jpeg_std_bits_ac_refine, jpeg_std_vals_ac_refine, 17, &ac_refine);

  gimg_jpeg_bitstream_write_t w;
  jpeg_bitstream_write_init(&w, a);
  if (!w.data) {
    return GIMG_ERR_OOM;
  }

  int32_t dc_pred[3] = {0, 0, 0};
  const gimg_jpeg_huff_enc_t * dc_tbls[3] = {&dc_lum, &dc_chrom, &dc_chrom};
  const gimg_jpeg_huff_enc_t * ac_tbls[3] = {&ac_lum, &ac_chrom, &ac_chrom};
  int is_dc = (Ss == 0 && Se == 0);
  int ss = (int)Ss;
  int se = (int)Se;
  int is_refine = (Ah != 0);

  for (size_t block_idx = 0; block_idx < total_blocks; block_idx++) {
    if (restart_interval > 0 && block_idx > 0 &&
        (block_idx % blocks_per_mcu) == 0) {
      uint32_t mcu_index = (uint32_t)(block_idx / blocks_per_mcu);
      if (mcu_index > 0 && (mcu_index % (uint32_t)restart_interval) == 0) {
        jpeg_bitstream_write_rst(&w, mcu_index, a);
        memset(dc_pred, 0, sizeof(dc_pred));
      }
    }
    const int16_t * block_zz = coef_buffer + block_idx * 64;
    // Block order matches fill: MCU order, comp 0 then 1 then 2.
    int c = 0;
    if (num_components == 3) {
      size_t in_mcu = block_idx % blocks_per_mcu;
      size_t acc = 0;
      for (c = 0; c < 3; c++) {
        size_t n = (size_t)hs[c] * (size_t)vs[c];
        if (in_mcu < acc + n) {
          break;
        }
        acc += n;
      }
    }
    if (is_dc) {
      if (is_refine) {
        jpeg_encode_block_dc_refine(&w, block_zz[0], Ah, a);
      }
      else {
        jpeg_encode_block_dc_only(&w, block_zz, dc_tbls[c], &dc_pred[c], a);
      }
    }
    else {
      if (is_refine) {
        jpeg_encode_block_ac_band_refine(
            &w, block_zz, ss, se, Ah, &ac_refine, a);
      }
      else {
        jpeg_encode_block_ac_band(&w, block_zz, ss, se, ac_tbls[c], a);
      }
    }
  }

  jpeg_bitstream_write_flush(&w, a);
  *out_scan_data = w.data;
  *out_scan_size = w.size;
  return GIMG_OK;
}

/** Encode a single baseline (sequential) scan from a coefficient buffer.
 * Block order and Huffman encoding match gimg_jpeg_encode_baseline_scan so
 * that baseline and progressive decoders produce identical pixels when the
 * coefficients come from the same source
 * (gimg_jpeg_progressive_fill_coef_buffer).
 */
GIMG_Result gimg_jpeg_encode_baseline_scan_from_coef_buffer(
    uint32_t GIMG_MAYBE_UNUSED(width), uint32_t GIMG_MAYBE_UNUSED(height),
    int num_components, const int16_t * coef_buffer, size_t total_blocks,
    const uint8_t * h_samp, const uint8_t * v_samp,
    const GIMG_Allocator * alloc, uint16_t restart_interval,
    unsigned char ** out_scan_data, size_t * out_scan_size) {
  if (!out_scan_data || !out_scan_size || !coef_buffer) {
    return GIMG_ERR_INTERNAL;
  }
  if (num_components != 1 && num_components != 3) {
    return GIMG_ERR_UNSUPPORTED;
  }
  uint8_t hs[3] = {1, 1, 1};
  uint8_t vs[3] = {1, 1, 1};
  if (h_samp) {
    for (int i = 0; i < num_components; i++) {
      hs[i] = h_samp[i] ? h_samp[i] : 1;
    }
  }
  if (v_samp) {
    for (int i = 0; i < num_components; i++) {
      vs[i] = v_samp[i] ? v_samp[i] : 1;
    }
  }
  size_t blocks_per_mcu = (num_components == 1)
      ? 1
      : (size_t)hs[0] * (size_t)vs[0] + (size_t)hs[1] * (size_t)vs[1] +
          (size_t)hs[2] * (size_t)vs[2];
  const GIMG_Allocator * a = gimg_alloc_or_default(alloc);
  /* Reserve 64 bytes before the stream so write_bits can store the current
   * code at data_base[0..3]; then write_byte never overwrites it. */
  static const size_t k_code_prefix = 64u;
  static const size_t k_baseline_scan_buf_size = 4096u;
  unsigned char * scan_buf =
      (unsigned char *)gimg_malloc(a, k_code_prefix + k_baseline_scan_buf_size);
  if (!scan_buf) {
    return GIMG_ERR_OOM;
  }
  gimg_jpeg_bitstream_write_t w;
  w.data_base = scan_buf;
  w.data = scan_buf + k_code_prefix;
  w.alloc_size = k_baseline_scan_buf_size;
  w.size = 0;
  w.bit_off = 0;
  w.cache = 0;

  static gimg_jpeg_huff_enc_t dc_lum, dc_chrom, ac_lum, ac_chrom;
  memset(&dc_lum, 0, sizeof(dc_lum));
  memset(&dc_chrom, 0, sizeof(dc_chrom));
  memset(&ac_lum, 0, sizeof(ac_lum));
  memset(&ac_chrom, 0, sizeof(ac_chrom));
  jpeg_build_huff_enc(jpeg_std_bits_dc_lum, jpeg_std_vals_dc_lum, 12, &dc_lum);
  jpeg_build_huff_enc(jpeg_std_bits_ac_lum, jpeg_std_vals_ac_lum, 162, &ac_lum);
  jpeg_build_huff_enc(
      jpeg_std_bits_dc_chrom, jpeg_std_vals_dc_chrom, 12, &dc_chrom);
  jpeg_build_huff_enc(
      jpeg_std_bits_ac_chrom, jpeg_std_vals_ac_chrom, 162, &ac_chrom);

  if (getenv("GIMG_JPEG_DEBUG_BASELINE_ENCODE")) {
    (void)fprintf(stderr,
        "BASELINE_ENCODE ac_lum.code[0]=%u &ac_lum=%p ac_chrom.code[0]=%u &ac_chrom=%p w.data=%p\n",
        (unsigned)ac_lum.code[0], (void *)&ac_lum,
        (unsigned)ac_chrom.code[0], (void *)&ac_chrom, (void *)w.data);
    (void)fflush(stderr);
  }

  int32_t dc_pred[3] = {0, 0, 0};
  const gimg_jpeg_huff_enc_t * dc_tbls[3] = {&dc_lum, &dc_chrom, &dc_chrom};
  const gimg_jpeg_huff_enc_t * ac_tbls[3] = {&ac_lum, &ac_chrom, &ac_chrom};

  if (getenv("GIMG_JPEG_DEBUG_BASELINE_ENCODE")) {
    (void)fprintf(stderr,
        "BASELINE_ENCODE num_components=%d total_blocks=%zu blocks_per_mcu=%zu "
        "hs=%u,%u,%u vs=%u,%u,%u\n",
        num_components, total_blocks, blocks_per_mcu, (unsigned)hs[0],
        (unsigned)hs[1], (unsigned)hs[2], (unsigned)vs[0], (unsigned)vs[1],
        (unsigned)vs[2]);
    (void)fflush(stderr);
  }

  for (size_t block_idx = 0; block_idx < total_blocks; block_idx++) {
    if (restart_interval > 0 && block_idx > 0 &&
        (block_idx % blocks_per_mcu) == 0) {
      uint32_t mcu_index = (uint32_t)(block_idx / blocks_per_mcu);
      if (mcu_index > 0 && (mcu_index % (uint32_t)restart_interval) == 0) {
        jpeg_bitstream_write_rst(&w, mcu_index, a);
        memset(dc_pred, 0, sizeof(dc_pred));
      }
    }
    const int16_t * block_zz = coef_buffer + block_idx * 64;
    int c = 0;
    if (num_components == 3) {
      size_t in_mcu = block_idx % blocks_per_mcu;
      size_t acc = 0;
      for (c = 0; c < 3; c++) {
        size_t n = (size_t)hs[c] * (size_t)vs[c];
        if (in_mcu < acc + n) {
          break;
        }
        acc += n;
      }
    }
    if (getenv("GIMG_JPEG_DEBUG_BASELINE_ENCODE") &&
        (block_idx < 3u || block_idx == 4u || block_idx == 5u)) {
      size_t in_mcu = (num_components == 3)
          ? (block_idx % blocks_per_mcu)
          : 0;
      int32_t dc_val = (int32_t)block_zz[0];
      int32_t diff = dc_val - dc_pred[c];
      int cat = jpeg_nbits(diff);
      if (cat > 11) {
        cat = 11;
      }
      (void)fprintf(stderr,
          "BASELINE_ENCODE block_idx=%zu in_mcu=%zu c=%d dc_cat=%d dc_code=0x%x "
          "dc_len=%u ac_eob_code=0x%x ac_eob_len=%u ac_tbl=%p\n",
          block_idx, in_mcu, c, cat, (unsigned)dc_tbls[c]->code[cat],
          (unsigned)dc_tbls[c]->len[cat], (unsigned)ac_tbls[c]->code[0],
          (unsigned)ac_tbls[c]->len[0], (void *)ac_tbls[c]);
      (void)fflush(stderr);
    }
    GIMG_Result r =
        jpeg_encode_block(&w, block_zz, dc_tbls[c], ac_tbls[c], &dc_pred[c], a);
    if (r != GIMG_OK) {
      gimg_free(a, scan_buf);
      return r;
    }
    if (getenv("GIMG_JPEG_DEBUG_BASELINE_ENCODE") && block_idx < 5u) {
      (void)fprintf(stderr, "BASELINE_ENCODE after block %zu w.size=%zu w.bit_off=%d\n",
          block_idx, (size_t)w.size, w.bit_off);
    }
  }

  jpeg_bitstream_write_flush(&w, a);
  if (getenv("GIMG_JPEG_DEBUG_BASELINE_ENCODE")) {
    (void)fprintf(stderr, "BASELINE_ENCODE after flush scan_size=%zu (total_blocks=%zu)\n",
        (size_t)w.size, total_blocks);
    if (getenv("GIMG_JPEG_DEBUG_SCAN_BYTES") && getenv("GIMG_JPEG_DEBUG_SCAN_BYTES")[0] == '1'
        && w.data && w.size > 0) {
      (void)fprintf(stderr, "BASELINE_ENCODE encoder_buffer (before save path stuffing)");
      for (size_t i = 0; i < w.size && i < 24u; i++) {
        (void)fprintf(stderr, " %zu:0x%02x", i, (unsigned)w.data[i]);
      }
      if (w.size > 24u) {
        (void)fprintf(stderr, " ...");
      }
      (void)fprintf(stderr, "\n");
    }
    (void)fflush(stderr);
  }
  unsigned char * copy_buf =
      (unsigned char *)gimg_malloc(gimg_alloc_or_default(alloc), w.size);
  if (!copy_buf) {
    gimg_free(a, scan_buf);
    return GIMG_ERR_OOM;
  }
  memcpy(copy_buf, w.data, w.size);
  gimg_free(a, scan_buf);
  *out_scan_data = copy_buf;
  *out_scan_size = w.size;
  return GIMG_OK;
}

/** Progressive scan encode with extended tables (12/16-bit coefficient range).
 */
GIMG_Result gimg_jpeg_encode_progressive_scan_16bit(uint32_t width,
    uint32_t height, int num_components, const int16_t * coef_buffer,
    size_t total_blocks, const uint8_t * h_samp, const uint8_t * v_samp,
    uint8_t Ss, uint8_t Se, uint8_t Ah, uint8_t Al,
    const GIMG_Allocator * alloc, uint16_t restart_interval,
    unsigned char ** out_scan_data, size_t * out_scan_size) {
  if (!out_scan_data || !out_scan_size || !coef_buffer) {
    return GIMG_ERR_INTERNAL;
  }
  (void)width;
  (void)height;
  (void)Al;
  uint8_t hs[3] = {1, 1, 1};
  uint8_t vs[3] = {1, 1, 1};
  if (h_samp) {
    for (int i = 0; i < num_components; i++) {
      hs[i] = h_samp[i] ? h_samp[i] : 1;
    }
  }
  if (v_samp) {
    for (int i = 0; i < num_components; i++) {
      vs[i] = v_samp[i] ? v_samp[i] : 1;
    }
  }
  uint8_t h_max = hs[0];
  uint8_t v_max = vs[0];
  if (num_components >= 3) {
    if (hs[1] > h_max) {
      h_max = hs[1];
    }
    if (hs[2] > h_max) {
      h_max = hs[2];
    }
    if (vs[1] > v_max) {
      v_max = vs[1];
    }
    if (vs[2] > v_max) {
      v_max = vs[2];
    }
  }
  size_t blocks_per_mcu = (num_components == 1)
      ? 1
      : (size_t)hs[0] * (size_t)vs[0] + (size_t)hs[1] * (size_t)vs[1] +
          (size_t)hs[2] * (size_t)vs[2];
  const GIMG_Allocator * a = gimg_alloc_or_default(alloc);
  jpeg_init_ext_ac_vals();
  gimg_jpeg_huff_enc_t dc_lum, dc_chrom, ac_lum, ac_chrom, ac_refine;
  memset(&dc_lum, 0, sizeof(dc_lum));
  memset(&dc_chrom, 0, sizeof(dc_chrom));
  memset(&ac_lum, 0, sizeof(ac_lum));
  memset(&ac_chrom, 0, sizeof(ac_chrom));
  memset(&ac_refine, 0, sizeof(ac_refine));
  jpeg_build_huff_enc(jpeg_ext_bits_dc_lum, jpeg_ext_vals_dc_lum, 17, &dc_lum);
  jpeg_build_huff_enc(jpeg_ext_bits_ac_lum, jpeg_ext_vals_ac_lum, 242, &ac_lum);
  jpeg_build_huff_enc(
      jpeg_ext_bits_dc_chrom, jpeg_ext_vals_dc_chrom, 17, &dc_chrom);
  // Use same 242 symbol set as lum so chroma never emits a symbol not in table.
  jpeg_build_huff_enc(
      jpeg_ext_bits_ac_chrom, jpeg_ext_vals_ac_lum, 242, &ac_chrom);
  jpeg_build_huff_enc(
      jpeg_std_bits_ac_refine, jpeg_std_vals_ac_refine, 17, &ac_refine);

  gimg_jpeg_bitstream_write_t w;
  jpeg_bitstream_write_init(&w, a);
  if (!w.data) {
    return GIMG_ERR_OOM;
  }

  int32_t dc_pred[3] = {0, 0, 0};
  const gimg_jpeg_huff_enc_t * dc_tbls[3] = {&dc_lum, &dc_chrom, &dc_chrom};
  const gimg_jpeg_huff_enc_t * ac_tbls[3] = {&ac_lum, &ac_chrom, &ac_chrom};
  int is_dc = (Ss == 0 && Se == 0);
  int ss = (int)Ss;
  int se = (int)Se;
  int is_refine = (Ah != 0);

  for (size_t block_idx = 0; block_idx < total_blocks; block_idx++) {
    if (restart_interval > 0 && block_idx > 0 &&
        (block_idx % blocks_per_mcu) == 0) {
      uint32_t mcu_index = (uint32_t)(block_idx / blocks_per_mcu);
      if (mcu_index > 0 && (mcu_index % (uint32_t)restart_interval) == 0) {
        jpeg_bitstream_write_rst(&w, mcu_index, a);
        memset(dc_pred, 0, sizeof(dc_pred));
      }
    }
    const int16_t * block_zz = coef_buffer + block_idx * 64;
    int c = 0;
    if (num_components == 3) {
      size_t in_mcu = block_idx % blocks_per_mcu;
      size_t acc = 0;
      for (c = 0; c < 3; c++) {
        size_t n = (size_t)hs[c] * (size_t)vs[c];
        if (in_mcu < acc + n) {
          break;
        }
        acc += n;
      }
    }
    if (is_dc) {
      if (is_refine) {
        jpeg_encode_block_dc_refine(&w, block_zz[0], Ah, a);
      }
      else {
        jpeg_encode_block_dc_only_ex(
            &w, block_zz, dc_tbls[c], &dc_pred[c], 16, a);
      }
    }
    else {
      if (is_refine) {
        jpeg_encode_block_ac_band_refine(
            &w, block_zz, ss, se, Ah, &ac_refine, a);
      }
      else {
        jpeg_encode_block_ac_band_ex(&w, block_zz, ss, se, ac_tbls[c], 15, a);
      }
    }
  }

  jpeg_bitstream_write_flush(&w, a);
  *out_scan_data = w.data;
  *out_scan_size = w.size;
  return GIMG_OK;
}

void gimg_jpeg_default_quant_scaled(
    unsigned quality, uint16_t * quant_luma, uint16_t * quant_chroma) {
  jpeg_scale_quant(quality, gimg_jpeg_default_quant_luma, quant_luma);
  jpeg_scale_quant(quality, gimg_jpeg_default_quant_chroma, quant_chroma);
}

/** 16-bit DQT: scale default quant to 1..65535 for 12/16-bit encode. */
void gimg_jpeg_default_quant_scaled_16bit(
    unsigned quality, uint16_t * quant_luma, uint16_t * quant_chroma) {
  jpeg_scale_quant_16bit(quality, gimg_jpeg_default_quant_luma, quant_luma);
  jpeg_scale_quant_16bit(quality, gimg_jpeg_default_quant_chroma, quant_chroma);
}

/** Write the four standard DHT segments (DC0, AC0, DC1, AC1) to stream. */
GIMG_Result gimg_jpeg_write_standard_dht(
    GIMG_Stream * stream, size_t * out_bytes_written) {
  size_t total = 0;
  size_t n = 0;
  GIMG_Result r;
  unsigned char seg[2];
  // DHT DC0: payload = TcTh(1) + 16 bits + 12 vals = 29; Lh = 31
  unsigned char dht_dc0[29];
  dht_dc0[0] = 0x00; // Tc=0, Th=0
  memcpy(dht_dc0 + 1, jpeg_std_bits_dc_lum, 16);
  memcpy(dht_dc0 + 17, jpeg_std_vals_dc_lum, 12);
  uint16_t len_dc0 = 31; // 2 + 29
  r = gimg_stream_write(stream, (const unsigned char *)"\xFF\xC4", 2, &n);
  if (r != GIMG_OK) {
    return r;
  }
  total += n;
  seg[0] = (unsigned char)(len_dc0 >> 8);
  seg[1] = (unsigned char)(len_dc0 & 0xFF);
  r = gimg_stream_write(stream, seg, 2, &n);
  if (r != GIMG_OK) {
    return r;
  }
  total += n;
  r = gimg_stream_write(stream, dht_dc0, sizeof(dht_dc0), &n);
  if (r != GIMG_OK) {
    return r;
  }
  total += n;
  /* DHT AC0 (T.81 Annex C): Tc=1 Th=0, 16 bytes BITS[1..16], then sum(BITS) values. */
  static const size_t k_ac_lum_num_vals = 37;
  unsigned char dht_ac0[1 + 16 + k_ac_lum_num_vals];
  dht_ac0[0] = 0x10; // Tc=1, Th=0
  memcpy(dht_ac0 + 1, jpeg_std_bits_ac_lum, 16);
  memcpy(dht_ac0 + 17, jpeg_std_vals_ac_lum, k_ac_lum_num_vals);
  uint16_t len_ac0 = (uint16_t)(2 + sizeof(dht_ac0));
  r = gimg_stream_write(stream, (const unsigned char *)"\xFF\xC4", 2, &n);
  if (r != GIMG_OK) {
    return r;
  }
  total += n;
  seg[0] = (unsigned char)(len_ac0 >> 8);
  seg[1] = (unsigned char)(len_ac0 & 0xFF);
  r = gimg_stream_write(stream, seg, 2, &n);
  if (r != GIMG_OK) {
    return r;
  }
  total += n;
  r = gimg_stream_write(stream, dht_ac0, sizeof(dht_ac0), &n);
  if (r != GIMG_OK) {
    return r;
  }
  total += n;
  // DHT DC1 chrominance
  unsigned char dht_dc1[29];
  dht_dc1[0] = 0x01; // Tc=0, Th=1
  memcpy(dht_dc1 + 1, jpeg_std_bits_dc_chrom, 16);
  memcpy(dht_dc1 + 17, jpeg_std_vals_dc_chrom, 12);
  uint16_t len_dc1 = 31;
  r = gimg_stream_write(stream, (const unsigned char *)"\xFF\xC4", 2, &n);
  if (r != GIMG_OK) {
    return r;
  }
  total += n;
  seg[0] = (unsigned char)(len_dc1 >> 8);
  seg[1] = (unsigned char)(len_dc1 & 0xFF);
  r = gimg_stream_write(stream, seg, 2, &n);
  if (r != GIMG_OK) {
    return r;
  }
  total += n;
  r = gimg_stream_write(stream, dht_dc1, sizeof(dht_dc1), &n);
  if (r != GIMG_OK) {
    return r;
  }
  total += n;
  /* DHT AC1 chrominance (T.81 Table K.6): payload 1+16+sum(bits); sum = 43. */
  static const size_t k_ac_chrom_num_vals = 43;
  unsigned char dht_ac1[1 + 16 + k_ac_chrom_num_vals];
  dht_ac1[0] = 0x11; // Tc=1, Th=1
  memcpy(dht_ac1 + 1, jpeg_std_bits_ac_chrom, 16);
  memcpy(dht_ac1 + 17, jpeg_std_vals_ac_chrom, k_ac_chrom_num_vals);
  uint16_t len_ac1 = (uint16_t)(2 + sizeof(dht_ac1));
  r = gimg_stream_write(stream, (const unsigned char *)"\xFF\xC4", 2, &n);
  if (r != GIMG_OK) {
    return r;
  }
  total += n;
  seg[0] = (unsigned char)(len_ac1 >> 8);
  seg[1] = (unsigned char)(len_ac1 & 0xFF);
  r = gimg_stream_write(stream, seg, 2, &n);
  if (r != GIMG_OK) {
    return r;
  }
  total += n;
  r = gimg_stream_write(stream, dht_ac1, sizeof(dht_ac1), &n);
  if (r != GIMG_OK) {
    return r;
  }
  total += n;
  if (out_bytes_written) {
    *out_bytes_written = total;
  }
  return GIMG_OK;
}

/** Write extended DHT (12/16-bit): DC 0..16, AC 242 symbols. */
GIMG_Result gimg_jpeg_write_standard_dht_extended(
    GIMG_Stream * stream, size_t * out_bytes_written) {
  jpeg_init_ext_ac_vals();
  size_t total = 0;
  size_t n = 0;
  GIMG_Result r;
  unsigned char seg[2];
  // DHT DC0 extended: 1 + 16 + 17 = 34; Lh = 36
  unsigned char dht_dc0[34];
  dht_dc0[0] = 0x00;
  memcpy(dht_dc0 + 1, jpeg_ext_bits_dc_lum, 16);
  memcpy(dht_dc0 + 17, jpeg_ext_vals_dc_lum, 17);
  uint16_t len_dc0 = 36;
  r = gimg_stream_write(stream, (const unsigned char *)"\xFF\xC4", 2, &n);
  if (r != GIMG_OK) {
    return r;
  }
  total += n;
  seg[0] = (unsigned char)(len_dc0 >> 8);
  seg[1] = (unsigned char)(len_dc0 & 0xFF);
  r = gimg_stream_write(stream, seg, 2, &n);
  if (r != GIMG_OK) {
    return r;
  }
  total += n;
  r = gimg_stream_write(stream, dht_dc0, sizeof(dht_dc0), &n);
  if (r != GIMG_OK) {
    return r;
  }
  total += n;
  // DHT AC0 extended: 1 + 16 + 242 = 259; Lh = 261
  unsigned char dht_ac0[259];
  dht_ac0[0] = 0x10;
  memcpy(dht_ac0 + 1, jpeg_ext_bits_ac_lum, 16);
  memcpy(dht_ac0 + 17, jpeg_ext_vals_ac_lum, 242);
  uint16_t len_ac0 = 261;
  r = gimg_stream_write(stream, (const unsigned char *)"\xFF\xC4", 2, &n);
  if (r != GIMG_OK) {
    return r;
  }
  total += n;
  seg[0] = (unsigned char)(len_ac0 >> 8);
  seg[1] = (unsigned char)(len_ac0 & 0xFF);
  r = gimg_stream_write(stream, seg, 2, &n);
  if (r != GIMG_OK) {
    return r;
  }
  total += n;
  r = gimg_stream_write(stream, dht_ac0, sizeof(dht_ac0), &n);
  if (r != GIMG_OK) {
    return r;
  }
  total += n;
  unsigned char dht_dc1[34];
  dht_dc1[0] = 0x01;
  memcpy(dht_dc1 + 1, jpeg_ext_bits_dc_chrom, 16);
  memcpy(dht_dc1 + 17, jpeg_ext_vals_dc_chrom, 17);
  r = gimg_stream_write(stream, (const unsigned char *)"\xFF\xC4", 2, &n);
  if (r != GIMG_OK) {
    return r;
  }
  total += n;
  seg[0] = (unsigned char)(len_dc0 >> 8);
  seg[1] = (unsigned char)(len_dc0 & 0xFF);
  r = gimg_stream_write(stream, seg, 2, &n);
  if (r != GIMG_OK) {
    return r;
  }
  total += n;
  r = gimg_stream_write(stream, dht_dc1, sizeof(dht_dc1), &n);
  if (r != GIMG_OK) {
    return r;
  }
  total += n;
  unsigned char dht_ac1[259];
  dht_ac1[0] = 0x11;
  memcpy(dht_ac1 + 1, jpeg_ext_bits_ac_chrom, 16);
  // Same 242 symbol set as AC0 so decoder table has every (run,size) we emit.
  memcpy(dht_ac1 + 17, jpeg_ext_vals_ac_lum, 242);
  r = gimg_stream_write(stream, (const unsigned char *)"\xFF\xC4", 2, &n);
  if (r != GIMG_OK) {
    return r;
  }
  total += n;
  seg[0] = (unsigned char)(len_ac0 >> 8);
  seg[1] = (unsigned char)(len_ac0 & 0xFF);
  r = gimg_stream_write(stream, seg, 2, &n);
  if (r != GIMG_OK) {
    return r;
  }
  total += n;
  r = gimg_stream_write(stream, dht_ac1, sizeof(dht_ac1), &n);
  if (r != GIMG_OK) {
    return r;
  }
  total += n;
  if (out_bytes_written) {
    *out_bytes_written = total;
  }
  return GIMG_OK;
}

/** Write one DHT segment for AC refinement (Table K.6 style, Th=2). Used when
 * progressive scan script includes AC refinement (Ah>0, Ss..Se not DC). */
GIMG_Result gimg_jpeg_write_ac_refine_dht(
    GIMG_Stream * stream, size_t * out_bytes_written) {
  size_t total = 0;
  size_t n = 0;
  GIMG_Result r;
  unsigned char seg[2];
  unsigned char dht[1 + 16 + 17];
  size_t dht_len = sizeof(dht);
  uint16_t len_u16 = (uint16_t)(2 + dht_len);
  dht[0] = 0x12; // Tc=1 (AC), Th=2
  memcpy(dht + 1, jpeg_std_bits_ac_refine, 16);
  memcpy(dht + 17, jpeg_std_vals_ac_refine, 17);
  r = gimg_stream_write(stream, (const unsigned char *)"\xFF\xC4", 2, &n);
  if (r != GIMG_OK) {
    return r;
  }
  total += n;
  seg[0] = (unsigned char)(len_u16 >> 8);
  seg[1] = (unsigned char)(len_u16 & 0xFF);
  r = gimg_stream_write(stream, seg, 2, &n);
  if (r != GIMG_OK) {
    return r;
  }
  total += n;
  r = gimg_stream_write(stream, dht, dht_len, &n);
  if (r != GIMG_OK) {
    return r;
  }
  total += n;
  if (out_bytes_written) {
    *out_bytes_written = total;
  }
  return GIMG_OK;
}

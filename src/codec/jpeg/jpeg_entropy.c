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
#include "jpeg_debug_internal.h"
#include "jpeg_huffman_tables_internal.h"
#include "jpeg_internal.h"

/** When GIMG_JPEG_TRACE_ALL=1, dump block[0..63] (zigzag order) to stderr as
 * 64 space-separated values in natural order. Prefix with label (e.g.
 * "BLOCK_BEFORE"). */
static void jpeg_trace_all_dump_block(const char * label, unsigned int scan_idx,
    uint32_t mcu_x, uint32_t mcu_y, unsigned int comp_idx, size_t block_in_mcu,
    size_t byte_off, unsigned int bit_off, const int16_t * block) {
  (void)fprintf(stderr,
      "%s scan%u mcu=(%u,%u) comp=%u block=%zu byte_off=%zu bit_off=%u coeffs:",
      label, scan_idx, (unsigned)mcu_x, (unsigned)mcu_y, comp_idx, block_in_mcu,
      (size_t)byte_off, (unsigned)bit_off);
  for (int nat = 0; nat < 64; nat++) {
    (void)fprintf(stderr, " %d", (int)block[gimg_jpeg_inv_zigzag[nat]]);
  }
  (void)fprintf(stderr, "\n");
  (void)fflush(stderr);
}

/** When GIMG_JPEG_DEBUG_PROG_SYNC=1, log every encoder/decoder step with
 * PROG_SYNC_ENC / PROG_SYNC_DEC scan=N block=B byte=X bit=Y op=... so you can
 * diff enc vs dec and find first divergence. */
#define PROG_SYNC_DEBUG() (GIMG_JPEG_DEBUG_PROG_SYNC)

/** Fancy chroma upsampling (triangle filter, 2h2v). ISO/IEC 10918-1 does not
 * mandate a specific upsampling method—multiple approaches are valid and
 * produce different pixel values. This implements the common triangle/smooth
 * filter. See also:
 * https://nigeltao.github.io/blog/2024/jpeg-chroma-upsampling.html
 * Output grid: x=0 and x=2*cw-1 use vertical-only (3*cur+other)/4 with bias
 * 8/7; odd x use 2x2 center (9,3,3,1)/16 (bias 7); even x in between use center
 * (bias 8). Call only when width==cw*2 and height==ch*2. */
static int jpeg_chroma_sample_fancy_2h2v(const unsigned char * buf,
    size_t stride, uint32_t cw, uint32_t ch, uint32_t x, uint32_t y) {
  uint32_t ri = y >> 1;
  uint32_t ri_other =
      (y & 1u) ? (ri + 1 < ch ? ri + 1 : ri) : (ri ? ri - 1 : 0);
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
  /** Legacy: when set, read_bit returns 0 at end of buffer. T.81 B.2.2 does not
   * specify padding bit value; we prefer last-block underflow handling (caller
   * treats -1 as EOB/0 when decoding the final block) for spec compliance and
   * third-party file support. Unused when last-block logic is used. */
  int pad_at_eob;
  /** Opt-in recovery only (GIMG_JPEG_RECOVER_STUFF_ZERO=1): at end of segment
   * treat missing bits as 0 and continue. Not from T.81; matches libjpeg
   * JWRN_HIT_MARKER-style recovery for truncated or non-byte-aligned streams.
   * Normal decode path does not assume any padding value. */
  int recover_stuff_zero;
  /** One-time warning for recover_stuff_zero. */
  int stuffed_any;
  /** RST handling (T.81 3.1.110): when 1, next 0xFF 0xD0..0xD7 is skipped as
   * restart marker; when 0, 0xFF 0xDx is not skipped (treated as entropy).
   * Decoder sets to 1 at start of each restart interval (mcu_index % Ri == 0).
   */
  int expect_rst;
  /** Set to 1 when we skipped an RST (T.81 3.1.110); caller must reset DC
   * predictors and clear this before decoding the next MCU. */
  int rst_just_skipped;
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
  bs->expect_rst = 0;
  bs->rst_just_skipped = 0;
}

/** Return 1 if marker m has no length/payload (SOI, EOI, RST0..RST7). */
static int jpeg_marker_no_length(unsigned char m) {
  if (m == GIMG_JPEG_MARKER_SOI || m == GIMG_JPEG_MARKER_EOI) {
    return 1;
  }
  if (m >= 0xD0 && m <= 0xD7) {
    return 1; // RST0..RST7
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
  if (m >= 0xD0 && m <= 0xD7) {
    /* RST (T.81 3.1.110): only skip when decoder expects RST at this position
     * (start of restart interval), matching libjpeg read_restart_marker. */
    if (!bs->expect_rst) {
      return 0; /* treat 0xFF 0xDx as entropy data (invalid stream if RST
                   misaligned) */
    }
    bs->expect_rst = 0;
#if GIMG_JPEG_DEBUG_RST_DEC
    (void)fprintf(stderr,
        "RST_DEC skip_marker_at_ff at byte_off=%zu marker=0x%02x\n",
        bs->byte_off, (unsigned)m);
    (void)fflush(stderr);
#endif
    bs->byte_off += 2; // skip 0xFF and marker byte (RST is on byte boundary)
    bs->bit_off = 0;
    bs->rst_just_skipped = 1;
    return 1;
  }
  bs->byte_off += 2; // skip 0xFF and marker byte
  if (jpeg_marker_no_length(m)) {
    return 1;
  }
  // Marker with 2-byte length (DHT, DQT, SOS, etc.). Skip only if the full
  // segment is in the buffer; otherwise leave position unchanged and do not
  // skip (caller will treat 0xFF as data — buffer should not be truncated).
  if (bs->byte_off + 2 > bs->size) {
    bs->byte_off -= 2; // undo skip
    return 0;
  }
  uint16_t seg_len =
      (uint16_t)((bs->data[bs->byte_off] << 8) | bs->data[bs->byte_off + 1]);
  if (seg_len < 2 || bs->byte_off + (size_t)seg_len > bs->size) {
    bs->byte_off -= 2;
    return 0;
  }
  bs->byte_off += (size_t)seg_len; // skip length bytes and payload
  return 1;
}

/** Skip marker segments and stuffing after 0xFF (T.81 B.2.2). Call only when
 * byte_off was just advanced past a 0xFF byte (data[byte_off - 1] == 0xFF).
 * Skip 0x00 (stuffing) or RST/marker bytes; do not skip entropy data. We do
 * not consume bytes beyond the current block/phase — each scan has its own
 * data buffer; within a block we only advance by bits we read. */
static void jpeg_bitstream_skip_after_ff(gimg_jpeg_bitstream_t * bs) {
  while (bs->byte_off < bs->size) {
    unsigned char m = bs->data[bs->byte_off];
    if (m == 0x00) {
      // Byte stuffing: 0xFF 0x00 is data 0xFF; skip the 0x00.
#if GIMG_JPEG_DEBUG_SKIP_FF
      (void)fprintf(stderr,
          "SKIP_FF skipping stuffing 0x00 at byte_off=%zu -> %zu\n",
          (size_t)bs->byte_off, (size_t)(bs->byte_off + 1));
      (void)fflush(stderr);
#endif
      bs->byte_off++;
      // Only a single 0x00 after 0xFF is stuffing. If another 0x00 follows,
      // it is real entropy data and must not be skipped again (T.81 B.2.2).
      break;
    }
    if (m == 0xFF) {
      /* Could be start of RST (0xFF 0xD0..0xD7); skip both bytes when
       * expect_rst. Otherwise treat as entropy data. */
      if (bs->byte_off + 1 < bs->size && bs->data[bs->byte_off + 1] >= 0xD0 &&
          bs->data[bs->byte_off + 1] <= 0xD7 && bs->expect_rst) {
        bs->expect_rst = 0;
#if GIMG_JPEG_DEBUG_RST_DEC
        (void)fprintf(stderr,
            "RST_DEC skip_after_ff (0xFF 0xDx) at byte_off=%zu\n",
            bs->byte_off - 1);
        (void)fflush(stderr);
#endif
        bs->byte_off += 2;
        bs->bit_off = 0;
        bs->rst_just_skipped = 1;
      }
      break;
    }
    if (m >= 0xD0 && m <= 0xD7) {
      /* We have already consumed the 0xFF as 8 bits of data (we advanced past
       * it). Do not skip the 0xDx here: that would leave us 8 bits ahead (we
       * fed RST as data). RST is only skipped in align_skip_rst at the start of
       * a block, before any bits of that block are read. Treat 0xDx as entropy
       * data here. */
      break;
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
    int bit = (int)bs->pushback_buf[--bs->pushback_n];
    /* Advance logical position so byte_off/bit_off match encoder (bits
     * consumed). */
    bs->bit_off++;
    if (bs->bit_off == 8) {
      bs->bit_off = 0;
      bs->byte_off++;
    }
    return bit;
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
    /* T.81 B.2.2: padding bit value in final incomplete byte is unspecified.
     * Normal path: return -1; caller uses last-block underflow handling (treat
     * as EOB or 0) so we do not assume 0 or 1. */
    if (bs->recover_stuff_zero) {
      if (!bs->stuffed_any) {
        (void)fprintf(stderr,
            "Corrupt JPEG data: premature end of data segment (stuffing zero "
            "bits)\n");
        (void)fflush(stderr);
        bs->stuffed_any = 1;
      }
      return 0; /* Opt-in recovery: assume 0 for missing bits (not from spec).
                 */
    }
    if (bs->pad_at_eob) {
      return 0; /* Legacy: assume 0 when encoder omitted byte stuffing. */
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
 * symbols). Contract: dht != NULL, dht_len >= 17 and dht_len >= 17 + sum(bits[1..16])
 * (T.81 B.2.4); tbl != NULL. Returns 0 on success, -1 if payload is invalid. */
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
  /* T.81 B.2.4: DHT value bytes must equal sum of the 16 bit counts. */
  if (dht_len < 17 + num_syms) {
    return -1;
  }
  const unsigned char * vals = dht + 17;
  tbl->num_values = (int)num_syms;

  /* Canonical code assignment per T.81 Annex C. */
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
#if GIMG_JPEG_DEBUG_DHT_DC
  if (dht[0] == 0x00) {
    (void)fprintf(stderr,
        "DHT_DC build: len3 min=0x%x max=0x%x base_idx=%u vals[1..5]=",
        (unsigned)tbl->min_code[3], (unsigned)tbl->max_code[3],
        (unsigned)tbl->base_index[3]);
    for (int i = 1; i <= 5 && i < 256; i++) {
      (void)fprintf(stderr, " %u", (unsigned)tbl->values[i]);
    }
    (void)fprintf(stderr, " (code4->sym=%u)\n",
        (unsigned)tbl->values[tbl->base_index[3] + (4 - tbl->min_code[3])]);
    (void)fflush(stderr);
  }
#endif
  return 0;
}

/** Default AC refinement DHT payload (T.81 Table K.6): Tc=1 Th=0 + 16 bits + 17
 * vals. Used when scan is AC refinement (Ah!=0) but no 17-symbol DHT is in the
 * file. */
/* (0,0)=EOB, (0,1)..(15,1)=run then newly nz, (15,0)=ZRL (T.81 G.1.2.2). */
static const unsigned char jpeg_default_ac_refine_dht[1 + 16 + 18] = {
    0x10,
    0,
    0,
    1,
    0,
    2,
    15,
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
    0x00,
    0x01,
    0x11,
    0x21,
    0x31,
    0x41,
    0x51,
    0x61,
    0x71,
    0x81,
    0x91,
    0xa1,
    0xb1,
    0xc1,
    0xd1,
    0xe1,
    0xf1,
    0xf0,
};

/** Return default AC table (T.81 Table K.4, 162 symbols) for first AC-initial
 * scan when no DHT before first SOS. */
static const unsigned char * jpeg_default_ac_dht_payload(size_t * out_len) {
  *out_len = sizeof(gimg_jpeg_default_ac_lum_dht_payload);
  return gimg_jpeg_default_ac_lum_dht_payload;
}

/** Pillow/libjpeg compat: build AC table for first AC-initial scan when DHT
 * was between scan 0 and 1. Encoder uses 00=(0,4), 10=EOB (non-canonical).
 * Fills tbl so decode accepts code 0 and 2 at length 2: 3 slots, values 4,0,0.
 */
static void jpeg_build_pillow_compat_ac_scan1_table(
    gimg_jpeg_huff_table_t * tbl) {
  memset(tbl, 0, sizeof(*tbl));
  tbl->num_values = 3;
  for (int len = 1; len <= 16; len++) {
    tbl->min_code[len] = 1;
    tbl->max_code[len] = 0;
    tbl->base_index[len] = 0;
  }
  tbl->min_code[2] = 0;
  tbl->max_code[2] = 2;
  tbl->base_index[2] = 0;
  tbl->values[0] = 4; /* code 00 -> (0,4) */
  tbl->values[1] = 0; /* code 01 -> EOB (unused in stream) */
  tbl->values[2] = 0; /* code 10 -> EOB */
}

/** Decode one symbol using the given Huffman table. Return symbol or -1.
 *
 * DC vs AC behaviour (T.81 Annex F):
 * - DC (is_ac=0): Decode exactly one Huffman symbol (the size category), then
 *   the caller reads that many extra bits. T.81 does not define or require
 *   longest-match for DC; peeking and pushing back bits would desync the
 *   stream because the following "extra bits" would consume bits that belong
 *   to the next symbol. So we never use longest-match for DC.
 * - AC (is_ac=1): For progressive scans, (0,0) EOB and (r,0) EOBRUN can share
 *   the same bit prefix; longest-match (peek up to 16 bits, take longest valid
 *   code) avoids mis-decoding. Optional; set GIMG_JPEG_AC_LONGEST_MATCH=0 to
 *   disable.
 *
 * When ac_prefer_eob is 1 and we match (0,1) or (0,2) at 3 bits (code 010/011),
 * we read one more bit: if 0 we have 0100 and return EOB (0); if 1 we push back
 * the bit and return the 3-bit symbol. For baseline (Annex F), using this can
 * consume a bit that belongs to the next symbol when the stream has (0,1)/(0,2)
 * followed by a codeword starting with 0; Annex F requires decoding the
 * codeword that appears in the bitstream (first matching length).
 *
 * is_ac: 1 for AC table (longest-match allowed when ac_prefer_eob=0);
 *        0 for DC table (never longest-match, per T.81 Annex F).
 * first_match_only: 1 for baseline AC (T.81 Annex F Figure F.16): return on
 *         first matching codeword; baseline does not use longest-match. */
static int jpeg_huff_decode(gimg_jpeg_bitstream_t * bs,
    const gimg_jpeg_huff_table_t * tbl, int ac_prefer_eob, int is_ac,
    int first_match_only) {
  uint16_t code = 0;
  const size_t start_byte_off = bs->byte_off;
  const int start_bit_off = bs->bit_off;
  const int trace_bits =
      (GIMG_JPEG_TRACE_HUFF_BITS) || (GIMG_JPEG_TRACE_ALL);
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
    if (GIMG_JPEG_TRACE_AC_FAIL) {
      (void)fprintf(stderr,
          "AC_FAIL_BIT len=%d bit=%d code=0x%04x (binary: ", len, b,
          (unsigned)code);
      for (int i = len - 1; i >= 0; i--) {
        (void)fprintf(stderr, "%d", (code >> i) & 1);
      }
      (void)fprintf(stderr, ") byte_off=%zu bit_off=%u\n", (size_t)bs->byte_off,
          (unsigned)bs->bit_off);
      (void)fflush(stderr);
    }
    if (trace_bits) {
      (void)fprintf(stderr,
          "HUFF_BIT len=%d bit=%d code=0x%x byte_off=%zu bit_off=%u\n", len, b,
          (unsigned)code, (size_t)bs->byte_off, (unsigned)bs->bit_off);
      (void)fflush(stderr);
    }
    if (GIMG_JPEG_TRACE_DC && !ac_prefer_eob) {
      (void)fprintf(stderr,
          "TRACE_DC len=%d bit=%d code=0x%x pre=(byte=%zu,bit=%d) "
          "post=(byte=%zu,bit=%d)\n",
          len, b, (unsigned)code, (size_t)pre_byte, pre_bit,
          (size_t)bs->byte_off, bs->bit_off);
      (void)fflush(stderr);
    }
    /* Length 16 can overflow uint16_t: e.g. 205 symbols give range
     * [65410,65614]; 65614 wraps to 78. Valid 16-bit codes are then
     * [65410,65535] and [0,78]. T.81 allows up to 16-bit codes. */
    int match = 0;
    uint16_t idx_off = 0;
    if (code >= tbl->min_code[len] && code <= tbl->max_code[len]) {
      match = 1;
      idx_off = code - tbl->min_code[len];
    }
    else if (len == 16 && tbl->max_code[16] < tbl->min_code[16] &&
        (code >= tbl->min_code[16] || code <= tbl->max_code[16])) {
      match = 1;
      idx_off = (code >= tbl->min_code[16])
          ? (uint16_t)(code - tbl->min_code[16])
          : (uint16_t)(code + 65536u - tbl->min_code[16]);
    }
    if (match) {
      uint16_t idx = tbl->base_index[len] + idx_off;
      if (idx >= (uint16_t)tbl->num_values) {
        /* T.81 assumes well-formed DHT (Annex K) and bitstream. Invalid index
         * indicates corrupt DHT or wrong table; treat as no match, continue
         * (caller will get underflow or no-match, return corrupt). */
        match = 0;
      }
      else {
      int sym = (int)tbl->values[idx];
      if (is_ac && GIMG_JPEG_AC_INITIAL_SYM_LOG) {
        (void)fprintf(stderr,
            "AC_INITIAL_HUFF_MATCH len=%d code=%u sym=0x%02x (run=%d size=%d) "
            "%s\n",
            len, (unsigned)code, (unsigned)sym, sym >> 4, sym & 15,
            sym == 0 ? "(EOB)" : "");
        (void)fflush(stderr);
      }
      if (!is_ac && GIMG_JPEG_TRACE_DC_MATCH) {
        size_t pos_after = (size_t)bs->byte_off * 8u + (unsigned)bs->bit_off;
        if (pos_after >= 966u && pos_after <= 976u) {
          (void)fprintf(stderr,
              "DC_HUFF_MATCH pos_after_code=%zu len=%d code=0x%x sym=%d "
              "idx=%u\n",
              pos_after, len, (unsigned)code, sym, (unsigned)idx);
          (void)fflush(stderr);
        }
      }
      if (trace_bits) {
        (void)fprintf(stderr,
            "HUFF_MATCH len=%d code=0x%x sym=0x%02x byte_off=%zu bit_off=%u\n",
            len, (unsigned)code, (unsigned)sym, (size_t)bs->byte_off,
            (unsigned)bs->bit_off);
        (void)fflush(stderr);
      }
      if (first_match_only) {
        return sym;
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
      /* Longest match for AC only. For baseline, use it only when the first
       * match is at 2 bits so we decode EOB/longer codes correctly when
       * (0,1)/(0,2) is a prefix; avoid using it for longer first matches to
       * prevent over-consumption. */
      {
        const int ac_longest = GIMG_JPEG_AC_LONGEST_MATCH;
        int do_longest = is_ac && !first_match_only && ac_longest &&
            !ac_prefer_eob &&
            len == 2; /* baseline: only extend 2-bit prefix to avoid desync */
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
            /* Stream position was advanced by n_read bits; we only consumed
             * 'used' of them (rest in pushback). Rewind so byte_off/bit_off
             * reflect logical position (match sync with encoder and traces). */
            if (n_read > used) {
              int rewind_bits = n_read - used;
              size_t pos_bits =
                  (size_t)bs->byte_off * 8u + (unsigned)bs->bit_off;
              if (pos_bits >= (size_t)rewind_bits) {
                pos_bits -= (size_t)rewind_bits;
                bs->byte_off = pos_bits / 8u;
                bs->bit_off = (int)(pos_bits % 8u);
              }
            }
          }
          return best_sym;
        }
      }
      return sym;
      }
    }
  }
  if (bs->pad_at_eob) {
    while (bs->byte_off < bs->size) {
      (void)jpeg_bitstream_read_bit(bs);
    }
  }
  /* Opt-in recovery only (GIMG_JPEG_RECOVER_STUFF_ZERO): treat no-match as EOB.
   * Not from T.81; for truncated or misaligned third-party streams. */
  if (bs->recover_stuff_zero) {
    return 0;
  }
  if (GIMG_JPEG_TRACE_AC_FAIL) {
    (void)fprintf(stderr,
        "AC_HUFF_FAIL no match after 16 bits: code=0x%04u (%u)\n",
        (unsigned)code, (unsigned)code);
    (void)fprintf(stderr,
        "  stream start: byte_off=%zu bit_off=%d (pos_bits=%zu)\n",
        (size_t)start_byte_off, start_bit_off,
        (size_t)start_byte_off * 8u + (size_t)start_bit_off);
    (void)fprintf(stderr, "  stream end:   byte_off=%zu bit_off=%u\n",
        (size_t)bs->byte_off, (unsigned)bs->bit_off);
    /* Sanity: does table have EOB (sym 0) at 4 bits? */
    (void)fprintf(stderr, "  table len=4: min=0x%x max=0x%x\n",
        (unsigned)tbl->min_code[4], (unsigned)tbl->max_code[4]);
    if (tbl->max_code[4] != 0 && 4u >= (unsigned)tbl->min_code[4] &&
        4u <= (unsigned)tbl->max_code[4]) {
      uint16_t idx4 = tbl->base_index[4] + (uint16_t)(4 - tbl->min_code[4]);
      (void)fprintf(stderr,
          "  table sym at code 4: 0x%02x (expect 0 for EOB)\n",
          (unsigned)tbl->values[idx4]);
    }
    (void)fprintf(stderr, "  code binary (MSB first): ");
    for (int i = 15; i >= 0; i--) {
      (void)fprintf(stderr, "%d", (int)((code >> i) & 1));
    }
    (void)fprintf(stderr, "\n  table bounds:\n");
    for (int len = 1; len <= 16; len++) {
      int in_range = (code >= tbl->min_code[len] && code <= tbl->max_code[len]);
      (void)fprintf(stderr, "    len=%2d min=0x%04x max=0x%04x %s\n", len,
          (unsigned)tbl->min_code[len], (unsigned)tbl->max_code[len],
          in_range ? " <-- WOULD MATCH" : "");
    }
    if (bs->byte_off <= bs->size && bs->size >= 2) {
      size_t lo = (start_byte_off >= 2) ? start_byte_off - 2 : 0;
      size_t hi = bs->byte_off + 2;
      if (hi > bs->size) {
        hi = bs->size;
      }
      (void)fprintf(
          stderr, "  raw bytes [%zu..%zu]:", (size_t)lo, (size_t)(hi - 1));
      for (size_t i = lo; i < hi; i++) {
        (void)fprintf(stderr, " %02x", (unsigned)bs->data[i]);
      }
      (void)fprintf(stderr, "\n");
    }
    (void)fflush(stderr);
  }
  return -1;
}

/** Extend sign for n-bit value (DC and AC). Per T.81 Annex F Figure F.12,
 * Table K.2: if val < 2^(n-1) then negative (val - (2^n - 1)); else unchanged.
 * Precondition: n in [0, 16] (DC/AC size category per T.81; avoids UB on shift).
 */
static int16_t jpeg_extend(int val, int n) {
  if (n <= 0) {
    return 0;
  }
  if (n > 16) {
    return (int16_t)val; /* Clamp: spec allows only 0..16; guard against misuse.
                          */
  }
  int half = 1 << (n - 1);
  if (val < half) {
    return (int16_t)(val - ((1 << n) - 1));
  }
  return (int16_t)val;
}


/** If expect_rst, skip the next 0xFF 0xD0..0xD7 (RST). T.81 B.2: RST between
 * entropy-coded segments; 3.1.110 defines RSTm. We may only skip when the
 * decoder has reached the RST (consumed data up to the 0xFF). Call at the
 * start of each block decode. Current byte 0xFF => skip; mid-byte and next
 * two bytes 0xFF 0xDx => RST at next byte (rest is padding); at byte boundary
 * with next byte not consumed => do not skip (may be entropy data). */
static void jpeg_bitstream_align_skip_rst(gimg_jpeg_bitstream_t * bs) {
  if (!bs->expect_rst || bs->byte_off >= bs->size) {
    return;
  }
  if (bs->byte_off + 1 >= bs->size) {
    return;
  }
  size_t rst_start;
  if (bs->data[bs->byte_off] == 0xFF && bs->data[bs->byte_off + 1] >= 0xD0 &&
      bs->data[bs->byte_off + 1] <= 0xD7) {
    /* RST at current byte (at byte boundary or we've read some bits of 0xFF).
     */
    rst_start = bs->byte_off;
  }
  else if (bs->bit_off > 0) {
    size_t next_byte = bs->byte_off + 1;
    if (next_byte + 1 >= bs->size) {
      return;
    }
    if (bs->data[next_byte] != 0xFF || bs->data[next_byte + 1] < 0xD0 ||
        bs->data[next_byte + 1] > 0xD7) {
      return;
    }
    /* RST starts at next byte (rest of current byte is encoder padding). */
    rst_start = next_byte;
  }
  else {
    /* T.81 B.2: skip RST only when we have reached the 0xFF. At byte boundary
     * the next byte is not yet consumed; it may be entropy data. Do not skip.
     */
    return;
  }
  bs->expect_rst = 0;
  bs->byte_off = rst_start + 2; /* skip 0xFF and 0xD0..0xD7 */
  bs->bit_off = 0;
  bs->rst_just_skipped = 1;
#if GIMG_JPEG_DEBUG_RST_DEC
  (void)fprintf(
      stderr, "RST_DEC align_skip_rst at byte_off=%zu\n", rst_start);
  (void)fflush(stderr);
#endif
}

/** Decode one 8x8 block (DC + AC). Block is 64 int16_t in zigzag order.
 * T.81 Annex F: DC differential then AC (run, size) or EOB (0,0); rest zero.
 * Contract: bs, dc_tbl, ac_tbl, block, dc_predictor all non-NULL; dc_tbl and
 * ac_tbl must be built from valid DHT (e.g. via jpeg_build_huff_table).
 * is_last_block: when 1, AC underflow is treated as EOB (T.81 B.2.2 does not
 * specify padding bit value; supports 0-bit padding / segment end before byte).
 */
static GIMG_Result jpeg_decode_block(gimg_jpeg_bitstream_t * bs,
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
  /* RST is skipped inside the DC read; reset predictor for this component. */
  if (bs->rst_just_skipped) {
    *dc_predictor = 0;
    bs->rst_just_skipped =
        0; /* clear so next block in same MCU / next MCU keeps predictor */
  }
  int nbits = sym;
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
    /* T.81 Annex F Figure F.16: baseline AC uses first-match only (no
     * longest-match). */
    sym = jpeg_huff_decode(bs, ac_tbl, 0, 1, 1);
    if (sym < 0) {
      if (is_last_block) {
        /* Segment ended before byte boundary (e.g. 0-bit padding); treat as
         * EOB. */
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
      /* T.81 Annex F: EOB (0,0); remaining coefficients in zigzag order are
       * zero. */
      for (; k < 64; k++) {
        block[k] = 0;
      }
      break;
    }
    int run = sym >> 4;
    int size = sym & 0x0F;
    k += run;
    /* T.81 Annex F: (run, size) with k+run >= 64 is invalid. */
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
          /* T.81 B.2.2: decoders must not interpret padding as data; treat
           * underflow as EOB. */
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
static GIMG_Result jpeg_decode_block_progressive_dc(gimg_jpeg_bitstream_t * bs,
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
      sym = 0; /* Segment ended before byte boundary; treat as DC size 0 (no
                  extra bits). */
    }
    else {
      return GIMG_ERR_CORRUPT;
    }
  }
  int nbits = sym;
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
        diff = 0; /* Padding / end of segment; treat as 0. */
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
  *dc_predictor += (diff << al);
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
static GIMG_Result jpeg_decode_block_progressive_dc_refine(
    gimg_jpeg_bitstream_t * bs, int16_t * block, int16_t * dc_predictor,
    unsigned int al, int * out_bit, int trace_all, int is_last_block) {
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
      b = 0; /* Segment ended; treat as 0 (do not assume padding value). */
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
 *  T.81 Annex G: (0,0)=EOB; (15,0)=ZRL (16 zero coeffs); (r,0) r=1..14 = EOB
 * with run length EOBRUN = 2^r + next r bits (then skip EOBRUN-1 following
 * blocks). If out_eobrun is non-NULL we implement EOBRUN; else (r,0) r!=15 is
 * error. If do_trace, emit TRACE_JPEG_AC_SYMBOLS [block=N] run=X size=Y val=Z
 * or EOB. trace_block_id: when >= 0 and do_trace, prefix lines with "block=N ".
 *  trace_scan_idx: when do_trace and TRACE_AC_COMPARE=1, emit canonical
 *  "AC_INITIAL scan=N block=B ..." for compare_progressive_trace.py.
 *  When trace_all, log every step: AC_INITIAL_HUFF_ENTER/EXIT,
 * EOB/ZRL/EOBRUN/COEFF. is_last_block: when 1, underflow treated as EOB (T.81
 * B.2.2 padding unspecified). */
static GIMG_Result jpeg_decode_block_progressive_ac_initial(
    gimg_jpeg_bitstream_t * bs, const gimg_jpeg_huff_table_t * ac_tbl,
    int16_t * block, int ss, int se, int al, int do_trace, int trace_block_id,
    unsigned int trace_scan_idx, unsigned int * out_eobrun, int trace_all,
    int is_last_block) {
  /* Full step dump for harmonization with ref
   * (GIMG_JPEG_DUMP_AC_INITIAL_FULL=1). */
#if GIMG_JPEG_DUMP_AC_INITIAL_FULL
  const int dump_ac_initial_env = 1;
#else
  const int dump_ac_initial_env = 0;
#endif
  const int dump_full = (trace_block_id >= 0 && dump_ac_initial_env);
  int k = ss;
  /* T.81 Annex G: at start of block, if EOBRUN > 0 this block is all-zero in
   * band. */
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
    /* Use first_match_only=0 (longest-match) for progressive AC initial: the
     * standard table can have EOB (e.g. 4-bit 1010) as a prefix of a longer
     * codeword (e.g. (10,1) at 9 bits). First-match would return EOB and drop
     * the coefficient; longest-match reads the full codeword (T.81 Annex G). */
    int sym = jpeg_huff_decode(bs, ac_tbl, 0, 1, 0);
    if (sym < 0) {
      if (is_last_block) {
        sym = 0; /* Segment ended; treat as EOB (T.81 B.2.2 padding
                    unspecified). */
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
    /* T.81 Annex G: (0,0)=EOB; (15,0)=ZRL; (r,0) r=1..14 = EOB with run 2^r + r
     * bits. */
    if (size == 0) {
      if (run != 15) {
        if (out_eobrun && run >= 1 && run <= 14) {
          /* EOB run: EOBRUN = 2^r + next r bits (T.81 Annex G). */
          unsigned int eobrun = 1u << (unsigned)run;
          if (run > 0) {
            int rbits = jpeg_bitstream_read_bits(bs, run);
            if (rbits < 0) {
              if (is_last_block) {
                rbits = 0; /* T.81 B.2.2: segment end; treat as 0. */
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
          *out_eobrun = eobrun;
          for (; k <= se; k++) {
            block[k] = 0;
          }
          break;
        }
        return GIMG_ERR_CORRUPT; /* (r,0) r!=15 without EOBRUN support. */
      }
      /* ZRL: 16 zero coefficients (T.81 Annex G). */
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
          ac = 0; /* T.81 B.2.2: segment end; treat as 0. */
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
    block[k] = (int16_t)(ac << al);
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
      if (GIMG_JPEG_TRACE_AC_COMPARE &&
          trace_block_id >= 0) {
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
 */
static GIMG_Result jpeg_decode_block_progressive_ac_refine(
    gimg_jpeg_bitstream_t * bs, const gimg_jpeg_huff_table_t * ac_tbl,
    int16_t * block, int ss, int se, int al, int do_trace, int trace_block_id,
    int log_sanity, int trace_all, int trace_scan_idx, int is_last_block) {
  /* Full dump for compare with ref: every byte/bit/block
   * (GIMG_JPEG_DUMP_AC_REFINE_FULL=1). */
  const int dump_full = GIMG_JPEG_DUMP_AC_REFINE_FULL;
  /* T.81: AC band is [Ss, Se] with 1 <= Ss <= Se <= 63. Clamp to prevent
   * overrun. */
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
    /* T.81 Annex F / Table K.6: AC refinement uses 17-symbol table; decode
     * first matching codeword only (no longest-match, no EOB peeking). */
    int sym = jpeg_huff_decode(bs, ac_tbl, 1, 1, 1);
    if (sym < 0) {
      if (is_last_block) {
        break; /* Segment ended; treat as EOB (T.81 B.2.2 padding unspecified).
                */
      }
      if (bs->recover_stuff_zero) {
        break; /* Opt-in recovery only (e.g. truncated stream). */
      }
      /* T.81 B.2.4: entropy-coded segment ends at the next marker. */
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
      /* T.81 Annex G.1.2.2 / libjpeg jdphuff: after EOB we set EOBRUN and break
       * from the symbol loop; then we must read one correction bit per
       * already-nonzero coefficient in the band [k,Se] (libjpeg does this in
       * the "if (EOBRUN > 0)" loop). So we read correction bits here. */
      for (; k <= se; k++) {
        if (block[k] != 0) {
          int rbit = jpeg_bitstream_read_bit(bs);
          if (rbit < 0) {
            if (is_last_block || bs->recover_stuff_zero) {
              rbit = 0; /* T.81 B.2.2 / recovery: treat as 0. */
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
            /* T.81 Annex G.1.2.2: correction bit 1 = Al-th bit of magnitude is
             * 1; only add when that bit is not already set (successive
             * approximation). */
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
          b = 0; /* T.81 B.2.2 / recovery: treat as 0. */
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
              rbit = 0; /* T.81 B.2.2 / recovery: treat as 0. */
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
          /* T.81 Annex G.1.2.2: correction bit 1 = Al-th bit of magnitude is 1;
           * only add when that bit is not already set (successive
           * approximation). */
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
          if (trace_block_id == 0 &&
              GIMG_JPEG_AC_REFINE_CORRECTION_K &&
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
        if (bs->recover_stuff_zero) {
          break; // Stop this block; match recovery behavior.
        }
        return GIMG_ERR_CORRUPT;
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
      /* size==0: T.81 Annex G.1.2.2 — (r,0) is either EOB run (r < 15) or ZRL
       * (r == 15). Not a workaround: the spec explicitly defines (r,0) for
       * r < 15 as EOB run with run length 2^r + (r appended bits). */
      if (run != 15) {
        /* EOB run: consume r appended bits, then correction bits for nonzero in
         * band. */
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
            (void)b; /* EOBRUN value not needed for single-block decode */
          }
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
                rbit = 0; /* T.81 B.2.2 / recovery: treat as 0. */
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
        break; /* EOB: done with this block */
      }
      /* run == 15: ZRL — skip 16 zero coefficients */
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
      (void)fprintf(stderr,
          "AC_REFINE_AFTER_FIRST_SYM block=0 byte_off=%zu bit_off=%u\n",
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
  size_t mcu_total = 0;
  if (!gimg_safe_mul_size(
          (size_t)mcu_per_row, (size_t)mcu_per_col, &mcu_total)) {
    return GIMG_ERR_LIMIT;
  }
  size_t blocks_per_mcu_ext = 0;
  for (uint8_t s = 0; s < scan0->comp_count; s++) {
    uint8_t comp_idx = 0;
    for (; comp_idx < num_comp; comp_idx++) {
      if (sof->comp_id[comp_idx] == scan0->comp_id[s])
        break;
    }
    if (comp_idx < num_comp)
      blocks_per_mcu_ext +=
          (size_t)sof->h_samp[comp_idx] * (size_t)sof->v_samp[comp_idx];
  }
  size_t total_blocks_ext = 0;
  if (!gimg_safe_mul_size(mcu_total, blocks_per_mcu_ext, &total_blocks_ext)) {
    return GIMG_ERR_LIMIT;
  }

  const GIMG_Allocator * alloc = state->allocator;
  alloc = gimg_alloc_or_default(alloc);

  /* Per-component dimensions in samples (T.81): X_i = ceil(X*H_i/H_max). */
  uint32_t comp_w[GIMG_JPEG_MAX_COMPONENTS];
  uint32_t comp_h[GIMG_JPEG_MAX_COMPONENTS];
  for (uint8_t i = 0; i < num_comp; i++) {
    comp_w[i] = (width * (uint32_t)sof->h_samp[i] + (uint32_t)h_max - 1) /
        (uint32_t)h_max;
    comp_h[i] = (height * (uint32_t)sof->v_samp[i] + (uint32_t)v_max - 1) /
        (uint32_t)v_max;
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
  size_t block_counter = 0;
  const int trace_baseline_sync =
      (GIMG_JPEG_TRACE_BASELINE_SYNC)
      ? 1
      : 0;
  for (uint32_t mcu_y = 0; mcu_y < mcu_per_col; mcu_y++) {
    for (uint32_t mcu_x = 0; mcu_x < mcu_per_row; mcu_x++) {
      uint32_t mcu_index = mcu_y * mcu_per_row + mcu_x;
      if (restart_interval > 0) {
        if (bs.rst_just_skipped) {
          memset(dc_pred, 0, sizeof(dc_pred));
          bs.rst_just_skipped = 0;
        }
        /* Encoder byte-aligns before RST (bit_writer_flush), so we only see
         * 0xFF 0xDx at the start of the first block after each RST. Set
         * expect_rst before MCU ri, 2*ri, ... so align_skip_rst skips there. */
        if (mcu_index > 0 && mcu_index % (uint32_t)restart_interval == 0) {
          bs.expect_rst = 1; /* T.81 3.1.110: next 0xFF 0xD0..0xD7 is RST */
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
            int is_last =
                (total_blocks_ext > 0 && block_counter == total_blocks_ext - 1)
                ? 1
                : 0;
            GIMG_Result r = jpeg_decode_block(
                &bs, dc_tbl, ac_tbl, block_zig, &dc_pred[comp_idx], is_last);
            if (r != GIMG_OK) {
              goto ext_fail;
            }
            if (trace_baseline_sync) {
              (void)fprintf(stderr,
                  "DEC_BLOCK_AFTER block=%zu byte=%zu bit=%u\n", block_counter,
                  (size_t)bs.byte_off, (unsigned)bs.bit_off);
              (void)fflush(stderr);
            }
            block_counter++;
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
    if (restart_interval > 0 && bs.rst_just_skipped) {
      bs.rst_just_skipped = 0; /* clear after all components of this MCU */
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
    if (GIMG_JPEG_DEBUG_DHT_AC && state->huff_ac[ac_id] &&
        state->huff_ac_len[ac_id] >= 18) {
      const unsigned char * bits = state->huff_ac[ac_id] + 1;
      size_t n_sym = 0;
      for (int i = 0; i < 16; i++) {
        n_sym += bits[i];
      }
      (void)fprintf(stderr,
          "BASELINE_DHT_AC ac_id=%u len=%zu num_syms=%zu bits[15]=%u "
          "min_code[16]=0x%04x\n",
          (unsigned)ac_id, (size_t)state->huff_ac_len[ac_id], (size_t)n_sym,
          (unsigned)bits[15], (unsigned)ac_tables[ac_id].min_code[16]);
      (void)fflush(stderr);
    }
  }
  if (GIMG_JPEG_DEBUG_BASELINE_FAIL) {
    /* Sanity: AC Th=1 first 3-bit symbol should be EOB (0x00) per T.81 Table
     * K.6. */
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
  size_t total_blocks = 0;
  if (!gimg_safe_mul_size(mcu_total, blocks_per_mcu, &total_blocks)) {
    return GIMG_ERR_LIMIT;
  }

  const GIMG_Allocator * alloc = state->allocator;
  alloc = gimg_alloc_or_default(alloc);

  /* Per-component dimensions in samples (T.81): X_i = ceil(X*H_i/H_max). */
  uint32_t comp_w[GIMG_JPEG_MAX_COMPONENTS];
  uint32_t comp_h[GIMG_JPEG_MAX_COMPONENTS];
  for (uint8_t i = 0; i < num_comp; i++) {
    comp_w[i] = (width * (uint32_t)sof->h_samp[i] + (uint32_t)h_max - 1) /
        (uint32_t)h_max;
    comp_h[i] = (height * (uint32_t)sof->v_samp[i] + (uint32_t)v_max - 1) /
        (uint32_t)v_max;
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

  /* Step 2: log first 80 bytes of scan data as seen by decoder (compare to
   * encoder buffer). */
  {
#if GIMG_JPEG_DEBUG_SCAN_LOADED
    const char * dbg_scan = "1";
#else
    const char * dbg_scan = NULL;
#endif
    if (dbg_scan && dbg_scan[0] == '1' && scan0->data && scan0->data_size > 0) {
      (void)fprintf(stderr, "DECODER scan_data (first 80 bytes) size=%zu:\n",
          (size_t)scan0->data_size);
      for (size_t i = 0; i < 80u && i < scan0->data_size; i++) {
        (void)fprintf(stderr, " %zu:0x%02x", i, (unsigned)scan0->data[i]);
        if ((i + 1) % 16 == 0) {
          (void)fprintf(stderr, "\n");
        }
      }
      if (scan0->data_size < 80u) {
        (void)fprintf(stderr, "\n");
      }
      (void)fflush(stderr);
    }
  }

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
  size_t block_counter_8 = 0;
  const int trace_baseline_sync_8 =
      (GIMG_JPEG_TRACE_BASELINE_SYNC)
      ? 1
      : 0;
  for (uint32_t mcu_y = 0; mcu_y < mcu_per_col; mcu_y++) {
    for (uint32_t mcu_x = 0; mcu_x < mcu_per_row; mcu_x++) {
      uint32_t mcu_index = mcu_y * mcu_per_row + mcu_x;
      if (restart_interval > 0) {
        /* dc_pred is reset in decode_block when rst_just_skipped; do not clear
         * here or the next MCU would use predictor 0 and corrupt. */
        if (mcu_index > 0 && mcu_index % (uint32_t)restart_interval == 0) {
          bs.expect_rst = 1;
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
            if (GIMG_JPEG_TRACE_DEC_BLOCK_POS) {
              size_t linear_block =
                  (size_t)mcu_y * (size_t)mcu_per_row + (size_t)mcu_x;
              if (linear_block <= 40u ||
                  (linear_block >= 1195u && linear_block <= 1225u)) {
                size_t pos = (size_t)bs.byte_off * 8u + (unsigned)bs.bit_off;
                (void)fprintf(stderr,
                    "DEC_BLOCK_POS block=%zu pos_bits=%zu byte_off=%zu "
                    "bit_off=%u\n",
                    linear_block, pos, (size_t)bs.byte_off,
                    (unsigned)bs.bit_off);
                (void)fflush(stderr);
              }
            }
            int is_last_8 =
                (total_blocks > 0 && block_counter_8 == total_blocks - 1) ? 1
                                                                          : 0;
            GIMG_Result r = jpeg_decode_block(
                &bs, dc_tbl, ac_tbl, block_zig, &dc_pred[comp_idx], is_last_8);
            if (r != GIMG_OK) {
              if (GIMG_JPEG_DEBUG_BIT_POS || GIMG_JPEG_DEBUG_BASELINE_FAIL) {
                (void)fprintf(stderr,
                    "BASELINE_FAIL mcu=(%u,%u) comp=%u (comp_idx=%u) dc_tbl=%u "
                    "ac_tbl=%u byte_off=%zu bit_off=%u\n",
                    (unsigned)mcu_x, (unsigned)mcu_y, (unsigned)s,
                    (unsigned)comp_idx, (unsigned)scan0->dc_tbl[s],
                    (unsigned)scan0->ac_tbl[s], (size_t)bs.byte_off,
                    (unsigned)bs.bit_off);
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
            if (trace_baseline_sync_8) {
              (void)fprintf(stderr,
                  "DEC_BLOCK_AFTER block=%zu byte=%zu bit=%u\n",
                  block_counter_8, (size_t)bs.byte_off, (unsigned)bs.bit_off);
              (void)fflush(stderr);
            }
            if (block_counter_8 == 0 && GIMG_JPEG_TRACE_FIRST_BLOCK) {
              (void)fprintf(stderr, "DEC_FIRST_BLOCK comp=%u DC=%d",
                  (unsigned)comp_idx, (int)block_zig[0]);
              for (int ki = 1; ki < 64; ki++) {
                if (block_zig[ki] != 0) {
                  (void)fprintf(stderr, " k=%d val=%d", ki, (int)block_zig[ki]);
                }
              }
              (void)fprintf(stderr, "\n");
              (void)fflush(stderr);
            }
            block_counter_8++;
            if (GIMG_JPEG_DEBUG_BIT_POS && mcu_x == 0 && mcu_y == 0 &&
                s == 0 && by == 0 && bx == 0) {
              (void)fprintf(stderr,
                  "DEC after block0: byte_off=%zu bit_off=%u (next byte(s):",
                  (size_t)bs.byte_off, (unsigned)bs.bit_off);
              for (size_t i = 0; i < 6u && bs.byte_off + i < scan0->data_size;
                   i++)
                (void)fprintf(
                    stderr, " %02x", (unsigned)scan0->data[bs.byte_off + i]);
              (void)fprintf(stderr, ")\n");
              (void)fflush(stderr);
            }
            jpeg_dezigzag(block_zig, block_rz);
            jpeg_dequantise(block_rz, quant, block_q);
            jpeg_idct_8x8_islow(block_q, block_rz);

            if (comp_idx == 1 && by == 0 && bx == 0 &&
                GIMG_JPEG_TRACE_FIRST_CB) {
              (void)fprintf(stderr,
                  "BASELINE first Cb: quant[0]=%u dequant[0]=%d idct[0]=%d "
                  "stored=%d\n",
                  (unsigned)quant[0], (int)block_q[0], (int)block_rz[0],
                  (int)block_rz[0] + 128);
              (void)fflush(stderr);
            }

            /* Bit-by-bit debug: first MCU coefficients and pipeline (compare to
             * encoder dump). */
            if (mcu_x == 0 && mcu_y == 0 &&
                GIMG_JPEG_TRACE_BASELINE_FIRST_MCU) {
              unsigned linear = (unsigned)block_counter_8 -
                  1u; /* 0-based to match encoder block_0.bin */
              (void)fprintf(stderr,
                  "BLOCK_DEC linear=%u comp=%u by=%u bx=%u ZIG:", linear,
                  (unsigned)comp_idx, (unsigned)by, (unsigned)bx);
              for (int i = 0; i < 64; i++)
                (void)fprintf(stderr, " %d", (int)block_zig[i]);
              (void)fprintf(stderr, "\n");
              (void)fprintf(stderr, "BLOCK_DEC linear=%u DEQUANT:", linear);
              for (int i = 0; i < 64; i++)
                (void)fprintf(stderr, " %d", (int)block_q[i]);
              (void)fprintf(stderr, "\n");
              (void)fprintf(stderr, "BLOCK_DEC linear=%u IDCT:", linear);
              for (int i = 0; i < 64; i++)
                (void)fprintf(stderr, " %d", (int)block_rz[i]);
              (void)fprintf(stderr, "\n");
              /* For chroma (Cr block linear==5), compare integer IDCT to float
               * IDCT on same DEQUANT. */
              if (linear == 5) {
                int16_t idct_float[64];
                jpeg_idct_8x8(block_q, idct_float);
                (void)fprintf(stderr, "BLOCK_DEC linear=5 IDCT_float:");
                for (int i = 0; i < 64; i++)
                  (void)fprintf(stderr, " %d", (int)idct_float[i]);
                (void)fprintf(stderr, "\n");
                (void)fprintf(
                    stderr, "BLOCK_DEC linear=5 IDCT_diff_islow_minus_float:");
                for (int i = 0; i < 64; i++)
                  (void)fprintf(
                      stderr, " %d", (int)block_rz[i] - (int)idct_float[i]);
                (void)fprintf(stderr, "\n");
              }
              (void)fprintf(stderr, "BLOCK_DEC linear=%u STORED:", linear);
              for (int i = 0; i < 64; i++) {
                int v = block_rz[i] + 128;
                if (v < 0)
                  v = 0;
                if (v > 255)
                  v = 255;
                (void)fprintf(stderr, " %d", v);
              }
              (void)fprintf(stderr, "\n");
              (void)fflush(stderr);
            }

            if (mcu_x == 0 && mcu_y == 0) {
#if GIMG_JPEG_DUMP_JPEG_COMPONENTS
              const char * dump_dir =
                  getenv("GIMG_JPEG_DUMP_JPEG_COMPONENTS");
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
#endif
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
#if GIMG_JPEG_DUMP_JPEG_COMPONENTS
    {
      const char * dir = getenv("GIMG_JPEG_DUMP_JPEG_COMPONENTS");
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
#endif
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
    int use_fancy = ((!options ||
                         options->jpeg_chroma_upsampling ==
                             GIMG_JPEG_CHROMA_UPSAMPLE_FANCY) &&
        (width == cw1 * 2 && height == ch1 * 2 && width == cw2 * 2 &&
            height == ch2 * 2));
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
  // multi-segment). For CMYK, always set color info so cmyk_polarity is set.
  if (num_comp == 4u ||
      (state->app2_icc && state->app2_icc_len > 0u)) {
    GIMG_Color_Info color_info;
    gimg_color_info_default(&color_info);
    if (num_comp == 4u) {
      color_info.cmyk_polarity = GIMG_CMYK_POLARITY_INK;
    }
    if (state->app2_icc && state->app2_icc_len > 0u) {
      if (state->app2_icc_num_chunks > 0) {
        color_info.icc_bytes = state->app2_icc;
        color_info.icc_size = state->app2_icc_len;
      }
      else {
        color_info.icc_bytes = state->app2_icc + 14;
        color_info.icc_size = state->app2_icc_len - 14u;
      }
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

/** Progressive decode for 8-, 12-, or 16-bit precision. Grayscale and YCbCr
 * only. */
static GIMG_Result jpeg_decode_progressive_extended(
    const gimg_jpeg_doc_state_t * state, const GIMG_Decode_Options * options,
    GIMG_Raster ** out_raster) {
  const gimg_jpeg_sof_t * sof = &state->sof;
  uint8_t precision = sof->precision;
  if (precision != 8 && precision != 12 && precision != 16) {
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

  /* Per-component dimensions in samples (T.81): X_i = ceil(X*H_i/H_max). */
  uint32_t comp_w[GIMG_JPEG_MAX_COMPONENTS];
  uint32_t comp_h[GIMG_JPEG_MAX_COMPONENTS];
  for (uint8_t i = 0; i < num_comp; i++) {
    comp_w[i] = (width * (uint32_t)sof->h_samp[i] + (uint32_t)h_max - 1) /
        (uint32_t)h_max;
    comp_h[i] = (height * (uint32_t)sof->v_samp[i] + (uint32_t)v_max - 1) /
        (uint32_t)v_max;
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
    size_t bw = (size_t)(comp_w[i] + 7) / 8;
    size_t bh = (size_t)(comp_h[i] + 7) / 8;
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
        ac_src = (scan->huff_ac[ac_id] && scan->huff_ac_len[ac_id] > 0)
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
      else if (!ac_src && ac_len == 0 && ac_tables[ac_id].num_values == 0) {
        /* Refinement scans (Ah!=0) use only the AC refinement table (Ta=2);
         * no initial AC table is required (T.81 G.1.1.2.2). */
        if ((int)scan->ah == 0) {
          goto prog_ext_fail;
        }
      }
      /* T.81 B.2.4: DHT defines tables for following scans. Refinement scan
       * (Ah!=0) requires a valid AC refinement table (G.1.1.2.2); reject if
       * build fails. */
      if (ac_refine_src && ac_refine_len > 0) {
        if (jpeg_build_huff_table(
                ac_refine_src, ac_refine_len, &ac_refine_tables[ac_id]) != 0) {
          goto prog_ext_fail;
        }
      }
      else if ((int)scan->ah != 0) {
        /* T.81 Table K.6: default AC refinement table when no DHT in file. */
        if (jpeg_build_huff_table(jpeg_default_ac_refine_dht,
                sizeof(jpeg_default_ac_refine_dht),
                &ac_refine_tables[ac_id]) != 0) {
          goto prog_ext_fail;
        }
      }
    }
    /* T.81 B.2.2: segment ends at next marker; padding bit value unspecified.
     * We track expected block count and treat underflow in the last block as
     * EOB/0 so we do not assume 0 or 1 for padding (spec compliance,
     * third-party files). Do not set pad_at_eob — use last-block underflow
     * handling instead. */
    size_t blocks_per_mcu_prog = 0;
    for (uint8_t s = 0; s < scan->comp_count; s++) {
      uint8_t comp_idx = 0;
      for (; comp_idx < num_comp; comp_idx++) {
        if (sof->comp_id[comp_idx] == scan->comp_id[s])
          break;
      }
      if (comp_idx < num_comp)
        blocks_per_mcu_prog +=
            (size_t)sof->h_samp[comp_idx] * (size_t)sof->v_samp[comp_idx];
    }
    size_t mcu_total_prog = 0;
    if (!gimg_safe_mul_size(
            (size_t)mcu_per_row, (size_t)mcu_per_col, &mcu_total_prog)) {
      goto prog_ext_fail;
    }
    size_t total_blocks_prog = 0;
    if (!gimg_safe_mul_size(
            mcu_total_prog, blocks_per_mcu_prog, &total_blocks_prog)) {
      goto prog_ext_fail;
    }

    gimg_jpeg_bitstream_t bs;
    jpeg_bitstream_init(&bs, scan->data, scan->data_size);
    {
      const char * e = getenv("GIMG_JPEG_RECOVER_STUFF_ZERO");
      if (e && e[0] == '1') {
        bs.recover_stuff_zero = 1; /* Opt-in recovery only; not from T.81. */
      }
    }
    uint16_t restart_interval = state->restart_interval;
    int ss = (int)scan->ss;
    int se = (int)scan->se;
    int ah = (int)scan->ah;
    int al = (int)scan->al;
    unsigned int eobrun = 0;
    size_t block_counter_prog = 0;

    for (uint32_t mcu_y = 0; mcu_y < mcu_per_col; mcu_y++) {
      for (uint32_t mcu_x = 0; mcu_x < mcu_per_row; mcu_x++) {
        uint32_t mcu_index = mcu_y * mcu_per_row + mcu_x;
        if (restart_interval > 0) {
          if (bs.rst_just_skipped) {
            memset(dc_pred, 0, sizeof(dc_pred));
            eobrun = 0;
            bs.rst_just_skipped = 0;
          }
          if (mcu_index > 0 && mcu_index % (uint32_t)restart_interval == 0) {
            eobrun = 0;
            bs.expect_rst = 1;
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
              int is_last_prog =
                  (total_blocks_prog > 0 &&
                      block_counter_prog == total_blocks_prog - 1)
                  ? 1
                  : 0;
              size_t block_idx =
                  mcu_block_start + (size_t)by * (size_t)h_samp + (size_t)bx;
              int16_t * block = coef_blocks[comp_idx] + block_idx * 64;
              if (is_dc) {
                GIMG_Result r;
                if (ah == 0) {
                  r = jpeg_decode_block_progressive_dc(&bs,
                      &dc_tables[scan->dc_tbl[s]], block, &dc_pred[comp_idx],
                      al, NULL, NULL, 0, is_last_prog);
                }
                else {
                  r = jpeg_decode_block_progressive_dc_refine(&bs, block,
                      &dc_pred[comp_idx], (unsigned int)al, NULL, 0,
                      is_last_prog);
                }
                if (r != GIMG_OK) {
                  goto prog_ext_fail;
                }
                if (block_counter_prog < 6 &&
                    GIMG_JPEG_TRACE_PROG_FIRST_DC) {
                  (void)fprintf(stderr,
                      "PROG_DEC_DC block=%zu comp=%u block[0]=%d\n",
                      block_counter_prog, (unsigned)comp_idx, (int)block[0]);
                  (void)fflush(stderr);
                }
              }
              else {
                if (ah == 0) {
                  int trace_blk = GIMG_JPEG_TRACE_PROG_FIRST_AC
                      ? (int)block_counter_prog
                      : -1;
                  GIMG_Result r = jpeg_decode_block_progressive_ac_initial(&bs,
                      &ac_tables[scan->ac_tbl[s]], block, ss, se, al, 0,
                      trace_blk, 0u, &eobrun, 0, is_last_prog);
                  if (r != GIMG_OK) {
                    goto prog_ext_fail;
                  }
                }
                else {
                  /* T.81 G.1.1.2.2: AC refinement uses only the refinement
                   * table (Ta selects it). Do not fall back to initial AC
                   * table (may be unbuilt for Ta=2). Require valid table. */
                  const gimg_jpeg_huff_table_t * ac_ref_tbl =
                      &ac_refine_tables[scan->ac_tbl[s]];
                  if (ac_ref_tbl->num_values == 0) {
                    goto prog_ext_fail;
                  }
                  GIMG_Result r =
                      jpeg_decode_block_progressive_ac_refine(&bs, ac_ref_tbl,
                          block, ss, se, al, 0, -1, 0, 0, -1, is_last_prog);
                  if (r != GIMG_OK) {
                    goto prog_ext_fail;
                  }
                }
              }
              block_counter_prog++;
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
  int16_t block_q_16[64]; /* 8-bit path: dequant output for islow IDCT */
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
        uint32_t dst_x = bx * 8;
        uint32_t dst_y = by * 8;
        if (precision == 8) {
          /* Same pipeline as baseline 8-bit: dequant (int16_t) + islow IDCT.
           * Chroma may underflow/overflow (TBD: match baseline or use 32-bit).
           */
          jpeg_dequantise(block_rz, quant, block_q_16);
          jpeg_idct_8x8_islow(block_q_16, block_rz);
          if (comp_idx == 1 && by == 0 && bx == 0 &&
              GIMG_JPEG_TRACE_FIRST_CB) {
            (void)fprintf(stderr,
                "PROGRESSIVE first Cb: quant[0]=%u dequant[0]=%d idct[0]=%d "
                "stored=%d\n",
                (unsigned)quant[0], (int)block_q_16[0], (int)block_rz[0],
                (int)block_rz[0] + 128);
            (void)fflush(stderr);
          }
          for (int dy = 0; dy < 8; dy++) {
            uint32_t y = dst_y + (uint32_t)dy;
            if (y >= comp_h[comp_idx])
              break;
            for (int dx = 0; dx < 8; dx++) {
              uint32_t x = dst_x + (uint32_t)dx;
              if (x >= comp_w[comp_idx])
                break;
              int v = (int)block_rz[dy * 8 + dx] + 128;
              if (v < 0)
                v = 0;
              if (v > 255)
                v = 255;
              comp_buf[comp_idx][y * comp_stride_el[comp_idx] + x] =
                  (uint16_t)(unsigned)v;
            }
          }
        }
        else {
          jpeg_dequantise_32(block_rz, quant, block_q);
          jpeg_idct_8x8_32(block_q, block_idct, idct_scale);
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
              comp_buf[comp_idx][y * comp_stride_el[comp_idx] + x] =
                  (uint16_t)v;
            }
          }
        }
      }
    }
  }

  for (uint8_t i = 0; i < num_comp; i++) {
    gimg_free(alloc, coef_blocks[i]);
  }

  /* For 8-bit precision we used islow IDCT and stored 0..255 in comp_buf;
   * output 8-bit raster (GRAY8/RGBA8) to match baseline decode. */
  const int prog_8bit = (precision == 8);

  GIMG_Result r;
  if (prog_8bit && num_comp == 1) {
    r = gimg_raster_create_with_allocator(alloc, (uint32_t)width,
        (uint32_t)height, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED, NULL, 0,
        out_raster);
    if (r != GIMG_OK) {
      goto prog_ext_fail_buf;
    }
    unsigned char * pixels = (unsigned char *)gimg_raster_pixels(*out_raster);
    size_t stride = gimg_raster_stride_bytes(*out_raster);
    for (uint32_t y = 0; y < height; y++) {
      for (uint32_t x = 0; x < width; x++) {
        uint16_t v = comp_buf[0][y * comp_stride_el[0] + x];
        pixels[y * stride + x] = (unsigned char)(v > 255u ? 255u : v);
      }
    }
  }
  else if (prog_8bit && num_comp == 3) {
    /* 8-bit colour: comp_buf holds 0..255; use same chroma and RGB as baseline.
     */
    unsigned char * comp_buf_8[GIMG_JPEG_MAX_COMPONENTS];
    size_t comp_size_8[GIMG_JPEG_MAX_COMPONENTS];
    for (uint8_t i = 0; i < num_comp; i++) {
      comp_size_8[i] = comp_stride_el[i] * (size_t)comp_h[i];
      comp_buf_8[i] = (unsigned char *)gimg_malloc(alloc, comp_size_8[i]);
      if (!comp_buf_8[i]) {
        for (uint8_t j = 0; j < i; j++)
          gimg_free(alloc, comp_buf_8[j]);
        goto prog_ext_fail_buf;
      }
      for (size_t k = 0; k < comp_size_8[i]; k++) {
        uint16_t v = comp_buf[i][k];
        comp_buf_8[i][k] = (unsigned char)(v > 255u ? 255u : v);
      }
    }
    int use_fancy = ((!options ||
                         options->jpeg_chroma_upsampling ==
                             GIMG_JPEG_CHROMA_UPSAMPLE_FANCY) &&
        (width == comp_w[1] * 2 && height == comp_h[1] * 2 &&
            width == comp_w[2] * 2 && height == comp_h[2] * 2));
    uint32_t cw1 = comp_w[1];
    uint32_t ch1 = comp_h[1];
    uint32_t cw2 = comp_w[2];
    uint32_t ch2 = comp_h[2];
    r = gimg_raster_create_with_allocator(alloc, (uint32_t)width,
        (uint32_t)height, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, NULL, 0,
        out_raster);
    if (r != GIMG_OK) {
      for (uint8_t i = 0; i < num_comp; i++)
        gimg_free(alloc, comp_buf_8[i]);
      goto prog_ext_fail_buf;
    }
    unsigned char * pixels = (unsigned char *)gimg_raster_pixels(*out_raster);
    size_t stride = gimg_raster_stride_bytes(*out_raster);
    for (uint32_t y = 0; y < height; y++) {
      uint32_t cy1 = (ch1 > 1 && height > 1) ? (y * ch1 / height) : 0;
      uint32_t cy2 = (ch2 > 1 && height > 1) ? (y * ch2 / height) : 0;
      for (uint32_t x = 0; x < width; x++) {
        uint32_t cx1 = (cw1 > 1 && width > 1) ? (x * cw1 / width) : 0;
        uint32_t cx2 = (cw2 > 1 && width > 1) ? (x * cw2 / width) : 0;
        int yy = comp_buf_8[0][y * comp_stride_el[0] + x];
        int cb, cr;
        if (use_fancy) {
          cb = jpeg_chroma_sample_fancy_2h2v(
              comp_buf_8[1], comp_stride_el[1], cw1, ch1, x, y);
          cr = jpeg_chroma_sample_fancy_2h2v(
              comp_buf_8[2], comp_stride_el[2], cw2, ch2, x, y);
        }
        else {
          cb = comp_buf_8[1][cy1 * comp_stride_el[1] + cx1];
          cr = comp_buf_8[2][cy2 * comp_stride_el[2] + cx2];
        }
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
    for (uint8_t i = 0; i < num_comp; i++) {
      gimg_free(alloc, comp_buf_8[i]);
    }
  }
  else if (num_comp == 1) {
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
  return jpeg_decode_progressive_extended(state, options, out_raster);
}
/**
 * @file
 *
 * Bitstream reader over JPEG scan data (MSB first; 0xFF 0x00 is data), RST
 * skip, Huffman table build from DHT payload, and decode next symbol. Used by
 * jpeg_block.c and jpeg_entropy.c. No IDCT or raster.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/codec.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "jpeg_debug_internal.h"
#include "jpeg_huffman_tables_internal.h"
#include "jpeg_internal.h"

void jpeg_bitstream_init(
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
    // RST (T.81 3.1.110): only skip when decoder expects RST at this position
    // (start of restart interval), matching libjpeg read_restart_marker.
    if (!bs->expect_rst) {
      return 0; // treat 0xFF 0xDx as entropy data (invalid stream if RST misaligned)
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
 * Skip 0x00 (stuffing) or RST/marker bytes; do not skip entropy data. */
static void jpeg_bitstream_skip_after_ff(gimg_jpeg_bitstream_t * bs) {
  while (bs->byte_off < bs->size) {
    unsigned char m = bs->data[bs->byte_off];
    if (m == 0x00) {
#if GIMG_JPEG_DEBUG_SKIP_FF
      (void)fprintf(stderr,
          "SKIP_FF skipping stuffing 0x00 at byte_off=%zu -> %zu\n",
          (size_t)bs->byte_off, (size_t)(bs->byte_off + 1));
      (void)fflush(stderr);
#endif
      bs->byte_off++;
      break;
    }
    if (m == 0xFF) {
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
      break;
    }
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

int jpeg_bitstream_read_bit(gimg_jpeg_bitstream_t * bs) {
  if (bs->pushback_n > 0) {
    int bit = (int)bs->pushback_buf[--bs->pushback_n];
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
            "Corrupt JPEG data: premature end of data segment (stuffing zero "
            "bits)\n");
        (void)fflush(stderr);
        bs->stuffed_any = 1;
      }
      return 0;
    }
    if (bs->pad_at_eob) {
      return 0;
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

int jpeg_bitstream_read_bits(gimg_jpeg_bitstream_t * bs, int n) {
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

int jpeg_build_huff_table(
    const unsigned char * dht, size_t dht_len, gimg_jpeg_huff_table_t * tbl) {
  if (dht_len < GIMG_JPEG_DHT_HEADER_LEN) {
    return -1;
  }
  const unsigned char * bits = dht + 1;
  size_t num_syms = 0;
  for (unsigned int i = 0; i < GIMG_JPEG_DHT_BIT_COUNTS; i++) {
    num_syms += bits[i];
  }
  if (dht_len < GIMG_JPEG_DHT_HEADER_LEN + num_syms) {
    return -1;
  }
  const unsigned char * vals = dht + GIMG_JPEG_DHT_HEADER_LEN;
  tbl->num_values = (int)num_syms;

  uint32_t code = 0;
  uint16_t base = 0;
  for (int len = 1; len <= (int)GIMG_JPEG_DHT_BIT_COUNTS; len++) {
    uint8_t count = bits[len - 1];
    tbl->min_code[len] = (uint16_t)code;
    if (count > 0) {
      tbl->max_code[len] = (uint16_t)(code + count - 1);
      tbl->base_index[len] = base;
      for (int k = 0; k < count; k++) {
        if ((size_t)base + (size_t)k >= num_syms) {
          return -1;
        }
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

const unsigned char * jpeg_default_ac_dht_payload(size_t * out_len) {
  *out_len = sizeof(gimg_jpeg_default_ac_lum_dht_payload);
  return gimg_jpeg_default_ac_lum_dht_payload;
}

void jpeg_build_pillow_compat_ac_scan1_table(
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
  tbl->values[0] = 4;
  tbl->values[1] = 0;
  tbl->values[2] = 0;
}

int jpeg_huff_decode(gimg_jpeg_bitstream_t * bs,
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
      {
        const int ac_longest = GIMG_JPEG_AC_LONGEST_MATCH;
        int do_longest = is_ac && !first_match_only && ac_longest &&
            !ac_prefer_eob &&
            len == 2;
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
          {
            int used = best_len - len;
            for (int i = n_read - 1; i >= used && bs->pushback_n < 16; i--) {
              bs->pushback_buf[bs->pushback_n++] = (unsigned char)peek_bits[i];
            }
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

int16_t jpeg_extend(int val, int n) {
  if (n <= 0) {
    return 0;
  }
  if (n > 16) {
    return (int16_t)val;
  }
  int half = 1 << (n - 1);
  if (val < half) {
    return (int16_t)(val - ((1 << n) - 1));
  }
  return (int16_t)val;
}

void jpeg_bitstream_align_skip_rst(gimg_jpeg_bitstream_t * bs) {
  if (!bs->expect_rst) {
    return;
  }
  // T.81 B.2.1: the encoder pads to a byte boundary before a restart marker, so
  // the decoder discards whatever is left of the byte it is part-way through
  // and the marker follows.  Finding the marker is not a matter of guessing an
  // offset: it is wherever byte alignment lands.
  size_t pos = bs->byte_off;
  if (bs->bit_off > 0) {
    // The partially-read byte is spent.  If it was 0xFF then B.1.1.5 stuffed a
    // 0x00 after it, and that stuffing belongs to the byte being discarded.
    // Missing this was the bug: the marker then began two bytes on rather than
    // one, the search gave up, and the scan was declared corrupt - which is why
    // restart intervals worked or failed depending on whether a 0xFF happened
    // to fall last before the marker.
    if (pos + 1 < bs->size && bs->data[pos] == 0xFF &&
        bs->data[pos + 1] == 0x00) {
      pos += 2;
    }
    else {
      pos += 1;
    }
  }
  // B.1.1.2: any number of 0xFF fill bytes may precede a marker.
  while (pos + 1 < bs->size && bs->data[pos] == 0xFF &&
      bs->data[pos + 1] == 0xFF) {
    pos++;
  }
  if (pos + 1 >= bs->size) {
    return;
  }
  if (bs->data[pos] != 0xFF || bs->data[pos + 1] < 0xD0 ||
      bs->data[pos + 1] > 0xD7) {
    return;
  }
  bs->expect_rst = 0;
  bs->byte_off = pos + 2;
  bs->bit_off = 0;
  bs->rst_just_skipped = 1;
#if GIMG_JPEG_DEBUG_RST_DEC
  (void)fprintf(stderr, "RST_DEC align_skip_rst at byte_off=%zu\n", pos);
  (void)fflush(stderr);
#endif
}

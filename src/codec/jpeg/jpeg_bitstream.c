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
 * Bitstream reader over JPEG scan data (MSB first; 0xFF 0x00 is data), RST
 * skip, Huffman table build from DHT payload, and decode next symbol. Used by
 * jpeg_block.c and jpeg_entropy.c. No IDCT or raster.
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
  // The width comes from a Huffman symbol, and a crafted DHT can carry any byte
  // value there, so it has to be bounded here rather than trusted.  T.81 F.1.2
  // caps a magnitude category at 15 even at 12-bit precision, so anything wider
  // than a machine word is certainly invalid; fuzzing reached a 255-bit read,
  // which shifted an int by more than its width.  Accumulate in unsigned so the
  // shift is defined for every width this accepts.
  if (n < 0 || n > 16) {
    return -1;
  }
  uint32_t v = 0;
  for (int i = 0; i < n; i++) {
    int b = jpeg_bitstream_read_bit(bs);
    if (b < 0) {
      return -1;
    }
    v = (v << 1) | (uint32_t)(b & 1);
  }
  return (int)v;
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
  // T.81 B.2.4.2: a Huffman table has at most 256 symbols, and Figure C.1
  // (Generate_size) assigns codes in order of increasing length, so the code
  // values must remain representable in their own length.
  if (num_syms == 0 || num_syms > 256) {
    return -1;
  }
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
    code += count;
    // T.81 Figure C.2 (Generate_code) and C.3: after the codes of length `len`
    // are assigned, the next code must still fit in `len` bits.  If it does
    // not, the bit counts describe more codes than that length can hold - an
    // over-subscribed table, which has no canonical assignment.  libjpeg
    // rejects the same condition as "Bogus Huffman table definition".  Without
    // this, min_code/max_code silently wrap and the decoder matches codewords
    // that the table never defined.
    if (code > (uint32_t)(1u << len)) {
      return -1;
    }
    code <<= 1;
  }
  // C.2: the all-ones codeword of the longest length is reserved, so a table
  // that consumes the entire code space at 16 bits is still valid only if it
  // leaves that one free.
  if (code > (uint32_t)(1u << 17)) {
    return -1;
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

/**
 * Decode the next Huffman symbol. @return the symbol, or -1 on error.
 *
 * A prefix code is decoded by reading bits until one of them completes a
 * codeword, and the first codeword that matches is the answer - there is no
 * choice to make. This used to make two, both left over from an era when the
 * progressive decoder was being fitted to Pillow's output rather than to
 * T.81, and both removed here because neither could ever change the answer:
 *
 *   - *prefer EOB*: on a three-bit match of symbol 1 or 2 it peeked a fourth
 *     bit and, if the four-bit code named EOB, returned EOB instead. In a
 *     canonical table `min_code[4] = (max_code[3] + 1) * 2`, so a code that
 *     matched at three bits shifted left by one is strictly below
 *     `min_code[4]`: the four-bit lookup could not match. Never taken, over
 *     every fixture in the suite.
 *   - *longest match*: on a two-bit match it read up to fourteen more bits
 *     looking for a longer codeword with the same prefix, then rewound. A
 *     prefix code has no such codeword, by the same argument at every length.
 *     Also never taken - and not free. It had already cost one bug: bits read
 *     past a codeword were pushed back for the next call, and a restart marker
 *     is a hard resynchronization point (T.81 B.2.1), so the first symbol of
 *     every interval after the first was decoded from bits that preceded the
 *     marker. That was fixed by dropping the buffered bits at each marker,
 *     which is a function this deletion also removes. The other half was never
 *     fixed: the rewind restored `byte_off` and `bit_off` arithmetically,
 *     which does not undo a stuffed `0xFF 0x00` or a marker segment that
 *     `jpeg_bitstream_read_bit` skipped on the way out - so a lookahead that
 *     crossed one left the stream pointing somewhere else.
 *
 * With them went the `pushback` scalar, which nothing ever set to a bit, and
 * the sixteen-bit pushback buffer the two of them wrote into.
 */
int jpeg_huff_decode(
    gimg_jpeg_bitstream_t * bs, const gimg_jpeg_huff_table_t * tbl) {
  uint16_t code = 0;
  for (int len = 1; len <= 16; len++) {
    int b = jpeg_bitstream_read_bit(bs);
    if (b < 0) {
      return -1;
    }
    code = (code << 1) | (uint16_t)b;
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
      if (idx < (uint16_t)tbl->num_values) {
        return (int)tbl->values[idx];
      }
    }
  }
  if (bs->recover_stuff_zero) {
    return 0;
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

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

/**
 * Consume a restart marker at the current position, whose byte must be 0xFF.
 *
 * **What can be in this buffer is decided elsewhere, and it is not much.** A
 * scan's entropy data is gathered by jpeg_load.c, which reads the file a byte
 * at a time and puts into the buffer only: entropy bytes, the `0xFF 0x00` of
 * B.2.2 byte stuffing, the `0xFF 0xDn` of a restart marker, and - where the
 * file ends mid-marker - a trailing lone `0xFF`.  Every other marker is
 * recognised there and either applied or used to end the scan.  Six call
 * sites build a bitstream and all six pass such a buffer.
 *
 * So this used to carry a marker-segment parser - read the two length bytes,
 * skip the payload, continue - that no input could reach.  It was also the
 * wrong thing to do if one ever had: B.2.4.1 ends the entropy-coded segment
 * at the next marker, so leaping over a marker segment and carrying on
 * decodes bytes that belong to something else.  What is left refuses to skip
 * anything it does not recognise, which leaves the 0xFF to be read as data
 * and the scan to end where it runs out - the same thing that already
 * happened to a restart marker arriving where none was expected.
 *
 * @return 1 if a restart marker was consumed, 0 otherwise.
 */
static int jpeg_bitstream_skip_marker_at_ff(gimg_jpeg_bitstream_t * bs) {
  // The only caller is the loop at the top of jpeg_bitstream_read_bit(), whose
  // own condition is `byte_off < size && data[byte_off] == 0xFF`.  A test for
  // those two stood here and could not fire.  What is still needed is the
  // byte after the 0xFF, which the caller has not looked at: at the end of the
  // buffer there is none, and then this is not a marker.
  //
  // No file reaches this either, and the reason is upstream: the scan
  // extractor ends the entropy-coded segment at the next marker, so a
  // trailing 0xFF is either part of that marker or a B.1.1.2 fill byte, and
  // is not passed on.  Measured over a scan of {FF}, {00 FF} and {00 00 FF},
  // with and without an EOI: the segment handed here never ends on a 0xFF,
  // and the versions with no following marker are refused before a bitstream
  // is built at all.  Kept because this function's contract is about the
  // buffer it is given, not about who fills it.
  if (bs->byte_off + 1 >= bs->size) {
    return 0;
  }
  const unsigned char m = bs->data[bs->byte_off + 1];
  // T.81 3.1.110: a restart is consumed only where the decoder expects one,
  // at the start of a restart interval, matching libjpeg's
  // read_restart_marker.  Anywhere else the two bytes are entropy data as far
  // as this reader is concerned, and the stream is invalid.
  if (m < 0xD0 || m > 0xD7 || !bs->expect_rst) {
    return 0;
  }
  bs->expect_rst = 0;
#if GIMG_JPEG_DEBUG_RST_DEC
  (void)fprintf(stderr,
      "RST_DEC skip_marker_at_ff at byte_off=%zu marker=0x%02x\n",
      bs->byte_off, (unsigned)m);
  (void)fflush(stderr);
#endif
  bs->byte_off += 2; // RST is on a byte boundary
  bs->bit_off = 0;
  bs->rst_just_skipped = 1;
  return 1;
}

/**
 * Step over what follows a 0xFF the reader has just consumed the bits of.
 *
 * Call only when byte_off was advanced past a 0xFF byte.  By the invariant
 * described above, what follows is the 0x00 of byte stuffing that B.1.1.5
 * requires after every 0xFF the entropy coder emits, or something this
 * reader may not move over.
 */
static void jpeg_bitstream_skip_after_ff(gimg_jpeg_bitstream_t * bs) {
  // Unreachable for the same upstream reason as the end-of-buffer test in
  // jpeg_bitstream_skip_marker_at_ff(): getting here needs the last byte of
  // the segment to have been a 0xFF, and the scan extractor does not hand one
  // over.
  if (bs->byte_off >= bs->size) {
    return;
  }
  const unsigned char m = bs->data[bs->byte_off];
  if (m == 0x00) {
#if GIMG_JPEG_DEBUG_SKIP_FF
    (void)fprintf(stderr,
        "SKIP_FF skipping stuffing 0x00 at byte_off=%zu -> %zu\n",
        (size_t)bs->byte_off, (size_t)(bs->byte_off + 1));
    (void)fflush(stderr);
#endif
    bs->byte_off++; // T.81 B.2.2
    return;
  }
  // A restart marker used to be consumed here as well, for the shape
  // 0xFF 0xFF 0xDn - a data 0xFF followed by a fill byte and a marker.  It
  // never ran: B.1.1.5 stuffs a 0x00 after every 0xFF the entropy coder
  // emits, so the byte after a data 0xFF is 0x00 in any conformant stream,
  // and a non-conformant one has no claim on being decoded.  Deleting it
  // leaves every test passing, fill-byte streams included - those are
  // handled by jpeg_bitstream_align_skip_rst(), which is where the decoder
  // actually looks for a restart marker.
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
  //
  // No caller can reach this today, and that is a property of the callers
  // rather than of the input: every one of them bounds the width to 15 first -
  // the two DC paths refuse a category above 15, the AC paths take `sym & 0x0F`
  // and the EOB run is 1..14, and the lossless decoder refuses `s > 16` and
  // handles 16 without reading.  It is kept as the second line of defence it
  // has already had to be once, and ADcCategoryAboveFifteenIsRefused documents
  // which of the two is load-bearing for which decoder.
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
      // base + k cannot reach num_syms: base is the number of codes shorter
      // than `len`, k < count, and num_syms is the sum of every count, so the
      // index is always below it.  A bound was tested here and could not fire;
      // the guard that makes it safe is the `num_syms > 256` refusal above,
      // which is what keeps the write inside tbl->values[256].
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
    code += count;
    // T.81 Figure C.2 (Generate_code) and C.3: after the codes of length `len`
    // are assigned, the next code must still fit in `len` bits, and the codes
    // already assigned must not have used the all-ones codeword of that
    // length, which C.2 reserves.  Both are the same test with `>=`: at
    // equality the last code handed out was all ones, and above it the bit
    // counts describe more codes than the length can hold - an
    // over-subscribed table, which has no canonical assignment.
    //
    // This was `>`, which caught only over-subscription.  That accepted two
    // shapes libjpeg rejects as "Bogus Huffman table definition": a table
    // complete at some length, and one whose last 16-bit codeword is 0xFFFF.
    // The asymmetry was one-sided - jpeg_gen_huff_table() already gives that
    // codeword up when writing - so the reader accepted tables this library
    // will not produce and libjpeg will not read.
    //
    // Without the check at all, min_code/max_code silently wrap and the
    // decoder matches codewords the table never defined.
    if (code >= (uint32_t)(1u << len)) {
      return -1;
    }
    code <<= 1;
  }
  // No check follows the loop.  One did - `code > (1u << 17)`, described as
  // enforcing the reserved codeword - and it could not fire: the in-loop test
  // holds code below 1<<16 at len 16, so the final shift leaves it below
  // 1<<17.  Searching the bit-count vectors that reach the end of the loop put
  // the maximum at exactly 1<<17, the threshold and never past it.  The rule
  // it named is enforced above, at the length where the codeword is assigned.
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
  // Neither arm is reachable from the four call sites in jpeg_block.c: each
  // sits inside an `if (n > 0)` and each width is already bounded to 15 - a DC
  // category the decoder refused above 15, or an AC size taken as `sym & 0x0F`.
  // They are the function's own contract rather than a check on the input, and
  // are kept so that it is total for the type it accepts.
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
  // B.1.1.2 permits any number of 0xFF fill bytes before a marker, and a loop
  // skipping them used to stand here.  It was measured, with a counter inside
  // it, to run zero times: a stream padded with one to four fill bytes before
  // every restart marker still decodes to identical pixels without it,
  // because the fill is consumed by the bit reader as data bits before
  // alignment runs, so `pos` already points at the marker when it gets here.
  // Restored only against an input that reaches it - see
  // FillBytesBeforeARestartMarkerAreSteppedOver, which covers the behaviour
  // and does not reach this.
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

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
 * CCITT Group 3 and Group 4, as TIFF stores them (ITU-T T.4 and T.6).
 *
 * Three compressions share this file because they share almost everything:
 *
 *   2  "Modified Huffman", one-dimensional, every row starting on a byte
 *      boundary and no end-of-line codes at all.
 *   3  Group 3, one-dimensional by default and two-dimensional when
 *      T4Options bit 0 says so, with end-of-line codes between rows and a
 *      tag bit after each saying which coding the next row uses.
 *   4  Group 4, purely two-dimensional, no end-of-line codes, every row
 *      coded against the one above it.
 *
 * The run-length codes are in src/codec/shared/ccitt.c, shared with the BMP
 * codec's OS/2 Huffman 1D - the same 208 codes with different framing.
 *
 * **Two-dimensional coding is about the row above.** A row is described as a
 * sequence of edits to the *changing elements* of the previous row: a
 * vertical mode says "this edge is within three pixels of the one above",
 * horizontal says "here are two explicit runs", pass says "the edge above
 * ends before this one starts". That is why the decoder carries two arrays
 * of transition positions rather than two rows of pixels - the positions are
 * what the coding refers to, and reconstructing them from pixels every row
 * would be the same work done twice.
 *
 * The output is packed one-bit rows, most significant bit first, each row
 * padded to a byte - which is exactly what an uncompressed one-bit TIFF
 * holds, so everything downstream treats a fax as what it is once this has
 * run. A set bit is black, which is what the shared tables' "black run"
 * means and what PhotometricInterpretation 0 then turns into ink.
 */

#include <ghoti.io/image/macros.h>

#include <ghoti.io/cutil/safemath.h>
#include <string.h>

#include "../../core/alloc_internal.h"
#include "../shared/ccitt.h"
#include "tiff_internal.h"

/** How many changing elements one row may have: one per pixel, plus the two
 * sentinels every reference line ends with. */
#define GIMG_TIFF_FAX_SENTINELS 2u

/** One line's changing elements: the positions where its colour changes.
 *
 * Position `changes[i]` is where the colour changes for the i'th time. A
 * line begins white, so an even index changes to black and an odd one back
 * to white - which is how the decoder knows a candidate's colour without
 * storing it. */
typedef struct {
  uint32_t * changes;
  size_t count;
  size_t capacity;
} gimg_tiff_fax_line_t;

static bool fax_push(gimg_tiff_fax_line_t * line, uint32_t at) {
  if (line->count >= line->capacity) {
    return false;
  }
  line->changes[line->count++] = at;
  return true;
}

/**
 * b1: the first changing element on the reference line strictly right of
 * @p a0 whose colour is opposite to @p white.
 *
 * "Opposite to the current colour" is a parity test rather than a lookup,
 * because the reference line begins white: an even index is a change *to*
 * black, so when the current run is white the element wanted is at an even
 * index.
 *
 * @return the position, or @p width when there is none - which is the
 *   sentinel every reference line is given so that this never runs off.
 */
static size_t fax_b1(const gimg_tiff_fax_line_t * ref, int64_t a0,
    bool white) {
  const size_t want_parity = white ? 0u : 1u;
  for (size_t i = 0; i < ref->count; i++) {
    if ((int64_t)ref->changes[i] > a0 && (i & 1u) == want_parity) {
      return i;
    }
  }
  return ref->count;
}

/** The position an index stands for, the end of the line standing in for one
 * that ran off the array. Returning the index and reading it through this is
 * what lets b2 be *the element after b1* rather than the element after the
 * first one that happens to share b1's value - a distinction that only shows
 * up on a line with two changes at the same position, which is exactly what
 * the sentinels are. */
static uint32_t fax_at(
    const gimg_tiff_fax_line_t * ref, size_t index, uint32_t width) {
  return index < ref->count ? ref->changes[index] : width;
}

/** Paint @p from to @p to black; white needs nothing, the row starting zero. */
static void fax_paint(
    unsigned char * row, uint32_t from, uint32_t to, bool white) {
  if (white) {
    return;
  }
  for (uint32_t x = from; x < to; x++) {
    row[x >> 3] |= (unsigned char)(0x80u >> (x & 7u));
  }
}

/** Step over an end-of-line code and any fill bits before it.
 *
 * T.4 writes eleven or more zeros then a one. A shorter run of zeros is not
 * an end-of-line, so the reader is put back and the bits are decoded as a
 * code - the same judgment the BMP decoder makes about the same pattern.
 *
 * @return true when one was consumed. */
static bool fax_skip_eol(gimg_ccitt_bits_t * b) {
  const size_t start = b->bit;
  unsigned int zeros = 0;
  for (;;) {
    unsigned int bit = 0;
    if (!gimg_ccitt_read_bit(b, &bit)) {
      b->bit = start;
      return false;
    }
    if (bit) {
      break;
    }
    if (++zeros > 64u) {
      b->bit = start;
      return false;
    }
  }
  if (zeros < 11u) {
    b->bit = start;
    return false;
  }
  return true;
}

/** Decode one one-dimensional row (T.4 section 4.1). */
static bool fax_row_1d(gimg_ccitt_bits_t * bits, unsigned char * row,
    uint32_t width, gimg_tiff_fax_line_t * cur) {
  uint32_t x = 0;
  bool white = true; // Every line begins with a white run, possibly empty.
  cur->count = 0;
  while (x < width) {
    uint32_t run = 0;
    if (!gimg_ccitt_read_run(bits, white, &run)) {
      return false;
    }
    if (run > width - x) {
      // A run that overhangs the line is clipped rather than refused, the
      // same judgment every other run-length decoder here makes.
      run = width - x;
    }
    fax_paint(row, x, x + run, white);
    x += run;
    if (!fax_push(cur, x)) {
      return false;
    }
    white = !white;
  }
  return true;
}

/** Decode one two-dimensional row (T.4 section 4.2, T.6 section 2). */
static bool fax_row_2d(gimg_ccitt_bits_t * bits, unsigned char * row,
    uint32_t width, const gimg_tiff_fax_line_t * ref,
    gimg_tiff_fax_line_t * cur) {
  int64_t a0 = -1;
  bool white = true;
  cur->count = 0;

  while (a0 < (int64_t)width) {
    // The mode codes, read a bit at a time. They are a prefix code in which
    // the number of zeros before the first one names the mode, so that is
    // what is counted:
    //
    //   0  1        V(0)
    //   1  011 010  V(1) right and left
    //   2  001      horizontal
    //   3  0001     pass
    //   4  00001x   V(2) right and left
    //   5  000001x  V(3) right and left
    //   6  0000001  extension
    //
    // Reading it as "zeros then a one" rather than as seven separate codes
    // is what keeps V(2) and V(3) from being off by one, which is what they
    // were until the sweep over libtiff's own Group 4 output said so.
    unsigned int bit = 0;
    unsigned int zeros = 0;
    for (;;) {
      if (!gimg_ccitt_read_bit(bits, &bit)) {
        return false;
      }
      if (bit) {
        break;
      }
      if (++zeros > 11u) {
        return false; // An end-of-line or nothing this decoder knows.
      }
    }

    const size_t b1_index = fax_b1(ref, a0, white);
    const uint32_t b1 = fax_at(ref, b1_index, width);
    int64_t a1 = 0;
    bool vertical = false;
    int delta = 0;

    switch (zeros) {
    case 0u: // 1: V(0)
      vertical = true;
      delta = 0;
      break;
    case 1u: { // 01x: VR(1) when x is 1, VL(1) when 0
      unsigned int side = 0;
      if (!gimg_ccitt_read_bit(bits, &side)) {
        return false;
      }
      vertical = true;
      delta = side ? 1 : -1;
      break;
    }
    case 2u: { // 001: horizontal - two explicit runs
      const int64_t start = a0 < 0 ? 0 : a0;
      uint32_t run1 = 0, run2 = 0;
      if (!gimg_ccitt_read_run(bits, white, &run1) ||
          !gimg_ccitt_read_run(bits, !white, &run2)) {
        return false;
      }
      int64_t mid = start + run1;
      if (mid > (int64_t)width) {
        mid = width;
      }
      int64_t end = mid + run2;
      if (end > (int64_t)width) {
        end = width;
      }
      fax_paint(row, (uint32_t)start, (uint32_t)mid, white);
      fax_paint(row, (uint32_t)mid, (uint32_t)end, !white);
      if (!fax_push(cur, (uint32_t)mid) || !fax_push(cur, (uint32_t)end)) {
        return false;
      }
      a0 = end;
      continue; // The colour is unchanged: two runs put it back.
    }
    case 3u: { // 0001: pass - the edge above ends before this run does
      const uint32_t b2 = fax_at(ref, b1_index + 1u, width);
      const int64_t start = a0 < 0 ? 0 : a0;
      fax_paint(row, (uint32_t)start, b2, white);
      a0 = b2;
      continue; // No changing element: the run simply continues.
    }
    case 4u: { // 00001x: VR(2) or VL(2)
      unsigned int side = 0;
      if (!gimg_ccitt_read_bit(bits, &side)) {
        return false;
      }
      vertical = true;
      delta = side ? 2 : -2;
      break;
    }
    case 5u: { // 000001x: VR(3) or VL(3)
      unsigned int side = 0;
      if (!gimg_ccitt_read_bit(bits, &side)) {
        return false;
      }
      vertical = true;
      delta = side ? 3 : -3;
      break;
    }
    default:
      // Six zeros before the one is 0000001..., the extension code that
      // introduces uncompressed mode; more than that is an end-of-line or
      // nothing this decoder knows. Both end the row.
      return false;
    }

    if (!vertical) {
      return false;
    }
    a1 = (int64_t)b1 + delta;
    if (a1 < 0) {
      a1 = 0;
    }
    if (a1 > (int64_t)width) {
      a1 = width;
    }
    const int64_t start = a0 < 0 ? 0 : a0;
    fax_paint(row, (uint32_t)start, (uint32_t)a1, white);
    if (!fax_push(cur, (uint32_t)a1)) {
      return false;
    }
    a0 = a1;
    white = !white;
  }
  return true;
}

GIMG_Result gimg_tiff_fax_decode(const GIMG_Allocator * allocator,
    const gimg_tiff_ifd_t * ifd, const unsigned char * src, size_t src_size,
    unsigned char * out, size_t out_size) {
  const uint32_t width = ifd->tiled ? ifd->tile_width : ifd->width;
  if (width == 0u) {
    return GIMG_ERR_FORMAT;
  }
  const size_t row_bytes = ((size_t)width + 7u) / 8u;
  const size_t rows = out_size / row_bytes;
  if (rows == 0u) {
    return GIMG_ERR_FORMAT;
  }
  memset(out, 0, out_size);

  // FillOrder 2 says the bits of each byte run the other way. It is rare
  // outside fax equipment and this is the only place in the codec that cares,
  // because every other compression this codec handles is defined on bytes.
  gimg_ccitt_bits_t bits = {src, src_size, 0u, ifd->fill_order == 2u};

  // One changing element per pixel is the worst case - an alternating row -
  // plus the sentinels a reference line is padded with.
  size_t capacity = 0;
  if (!gcu_safe_add_size(width, GIMG_TIFF_FAX_SENTINELS, &capacity)) {
    return GIMG_ERR_LIMIT;
  }
  uint32_t * store =
      (uint32_t *)gimg_calloc(allocator, capacity * 2u, sizeof(uint32_t));
  if (!store) {
    return GIMG_ERR_OOM;
  }
  gimg_tiff_fax_line_t ref = {store, 0u, capacity};
  gimg_tiff_fax_line_t cur = {store + capacity, 0u, capacity};

  // An imaginary all-white line above the first row, which is what both T.4
  // and T.6 code the first two-dimensional row against.
  ref.changes[0] = width;
  ref.changes[1] = width;
  ref.count = 2u;

  const bool group4 = ifd->compression == GIMG_TIFF_COMPRESSION_CCITT_T6;
  const bool group3 = ifd->compression == GIMG_TIFF_COMPRESSION_CCITT_T4;
  const bool mixed = group3 && (ifd->t4_options & 1u) != 0u;

  size_t done = 0;
  for (; done < rows; done++) {
    unsigned char * row = out + (done * row_bytes);
    bool two_d = group4;

    if (group3) {
      // End-of-line codes are optional in a TIFF (T4Options says nothing
      // about them), so one is stepped over when present rather than
      // required. In mixed coding the bit after it chooses the row's
      // dimension; with no end-of-line there is no tag bit either, and the
      // file must be one-dimensional throughout.
      if (fax_skip_eol(&bits) && mixed) {
        unsigned int tag = 0;
        if (!gimg_ccitt_read_bit(&bits, &tag)) {
          break;
        }
        two_d = tag == 0u;
      }
    }
    else if (ifd->compression == GIMG_TIFF_COMPRESSION_CCITT_RLE) {
      // Compression 2 is the one that pads: every row starts on a byte
      // boundary, which is the whole difference between it and a Group 3
      // file with no end-of-line codes.
      bits.bit = (bits.bit + 7u) & ~(size_t)7u;
    }

    const bool ok = two_d ? fax_row_2d(&bits, row, width, &ref, &cur)
                          : fax_row_1d(&bits, row, width, &cur);
    if (!ok) {
      break;
    }

    // This row becomes the next row's reference, with the two sentinels that
    // let b1 and b2 always find something.
    if (cur.count + GIMG_TIFF_FAX_SENTINELS <= cur.capacity) {
      cur.changes[cur.count] = width;
      cur.changes[cur.count + 1u] = width;
      cur.count += GIMG_TIFF_FAX_SENTINELS;
    }
    const gimg_tiff_fax_line_t swap = ref;
    ref = cur;
    cur = swap;
  }

  gimg_free(allocator, store);
  // A fax that ends early keeps the rows that arrived, which is how every
  // other short block in this codec is treated; one that produced nothing at
  // all is data this decoder could not read.
  return done == 0u ? GIMG_ERR_CORRUPT : GIMG_OK;
}

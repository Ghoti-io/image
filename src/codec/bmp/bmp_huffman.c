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
 * CCITT Group 3 one-dimensional decoding, for OS/2 BMPs whose ulCompression
 * is 3.  Windows spells 3 BI_BITFIELDS; an OS/2 BITMAPCOREHEADER2 means this,
 * which is why the two vocabularies are normalized before anything reads the
 * number.
 *
 * The code tables are ITU-T T.4 Table 1 and Table 2, plus the extended makeup
 * codes both colours share.  A run is a makeup code, which is always a
 * multiple of 64, followed by a terminating code of 0 to 63; a run under 64
 * needs no makeup.  That is why no entry carries a "terminating" flag here:
 * a run below 64 is one by definition.
 *
 * The code tables themselves are not here: they are in
 * src/codec/shared/ccitt.c, because TIFF's CCITT Group 3 and Group 4 use the
 * same 208 codes with different framing, and a second copy is a copy that can
 * be corrected once.
 *
 * What no specification states is which palette index T.4's "white" and
 * "black" mean.  There is no BMP document that says, and the format's own
 * author calls the documentation close to non-existent.  It is settled here by
 * measurement rather than by convention: bmpsuite stores one picture twice,
 * as q/pal1huffmsb.bmp and as g/pal1.bmp, and only black = index 1 makes the
 * two decode alike.  The test asserts that, so a future change of mind has to
 * disagree with the picture rather than with a comment.
 */

#include <ghoti.io/image/macros.h>
#include "../shared/ccitt.h"
#include "bmp_internal.h"

#include <string.h>








/**
 * Step over an end-of-line marker, and any fill bits before it.
 *
 * T.4 allows a line to be preceded by eleven or more zero bits followed by a
 * one.  A run of zeros that turns out to be shorter than that is not an EOL,
 * so the reader is put back where it started and the bits are decoded as a
 * code.
 */
static void g31d_skip_eol(gimg_ccitt_bits_t * b) {
  size_t start = b->bit;
  unsigned int zeros = 0;
  for (;;) {
    unsigned int bit;
    if (!gimg_ccitt_read_bit(b, &bit)) {
      b->bit = start;
      return;
    }
    if (bit) {
      break;
    }
    if (++zeros > 64u) {  // Not an EOL, and not worth scanning further.
      b->bit = start;
      return;
    }
  }
  if (zeros < 11u) {
    b->bit = start;
  }
}

GIMG_Result gimg_bmp_huffman_expand(const unsigned char * data, size_t size,
    uint32_t width, uint32_t height, size_t stride, unsigned char * out) {
  if (!data || !out || !width || !height) {
    return GIMG_ERR_INTERNAL;
  }
  gimg_ccitt_bits_t bits = {data, size, 0, false};

  // The encoded lines are in the file's own row order, so they are written
  // straight into the packed rows an uncompressed 1-bit image would have had.
  // Everything downstream - the palette lookup, the bottom-up flip, the limits
  // - then treats this exactly like one, because after this it is one.
  memset(out, 0, stride * (size_t)height);

  for (uint32_t y = 0; y < height; y++) {
    unsigned char * row = out + ((size_t)y * stride);
    uint32_t x = 0;
    bool white = true;  // T.4 lines begin with a white run, possibly empty.
    while (x < width) {
      g31d_skip_eol(&bits);
      uint32_t run = 0;
      if (!gimg_ccitt_read_run(&bits, white, &run)) {
        return GIMG_ERR_CORRUPT;
      }
      if (run > width - x) {
        // A run that overhangs the line is clipped rather than refused, the
        // same judgment the RLE decoder makes: the pixels it would have
        // written past the end have nowhere to go, and the rest of the image
        // is still readable.
        run = width - x;
      }
      if (!white) {
        for (uint32_t i = 0; i < run; i++) {
          uint32_t at = x + i;
          row[at >> 3] |= (unsigned char)(0x80u >> (at & 7u));
        }
      }
      x += run;
      white = !white;
    }
  }
  return GIMG_OK;
}

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

/** MSB-first bit writer, counting past the end rather than writing past it so
 * that a caller can size a buffer by encoding into none. */
typedef struct {
  unsigned char * data; ///< NULL to measure without writing.
  size_t capacity;
  size_t bit;
  bool overflow;
} gimg_bmp_g31d_out_t;

static void g31d_put_bits(
    gimg_bmp_g31d_out_t * o, uint32_t code, unsigned int bits) {
  for (unsigned int i = 0; i < bits; i++) {
    unsigned int bit = (code >> (bits - 1u - i)) & 1u;
    size_t byte = o->bit >> 3;
    if (o->data) {
      if (byte >= o->capacity) {
        o->overflow = true;
        return;
      }
      if ((o->bit & 7u) == 0u) {
        o->data[byte] = 0;
      }
      o->data[byte] |= (unsigned char)(bit << (7u - (o->bit & 7u)));
    }
    o->bit++;
  }
}

/** Find a code for an exact run length in one colour's table. */

/**
 * Write one run as a makeup code, or several, followed by a terminating one.
 *
 * T.4 states runs of 64 and over as a makeup code carrying a multiple of 64
 * and then a terminating code of 0 to 63.  The largest makeup either colour
 * has is 2560, so a longer run takes more than one - and a terminating code is
 * always written, including for a remainder of zero, because that is what ends
 * the run.
 */
static bool g31d_put_run(
    gimg_bmp_g31d_out_t * o, bool white, uint32_t run) {
  while (run >= GIMG_CCITT_TERMINATING) {
    uint32_t makeup = run - (run % GIMG_CCITT_TERMINATING);
    if (makeup > 2560u) {
      makeup = 2560u;
    }
    const gimg_ccitt_code_t * code = gimg_ccitt_code_for(white, makeup);
    if (!code) {
      return false;
    }
    g31d_put_bits(o, code->code, code->bits);
    run -= makeup;
  }
  const gimg_ccitt_code_t * code = gimg_ccitt_code_for(white, run);
  if (!code) {
    return false;
  }
  g31d_put_bits(o, code->code, code->bits);
  return true;
}

GIMG_Result gimg_bmp_huffman_encode(const unsigned char * rows, uint32_t width,
    uint32_t height, size_t stride, unsigned char * out, size_t capacity,
    size_t * out_size) {
  if (!rows || !width || !height || !out_size) {
    return GIMG_ERR_INTERNAL;
  }
  gimg_bmp_g31d_out_t o = {out, capacity, 0, false};

  for (uint32_t y = 0; y < height; y++) {
    const unsigned char * row = rows + ((size_t)y * stride);
    // Every T.4 line begins with a white run, which is empty when the line
    // starts on black.  Writing that zero-length code is not optional: a
    // decoder counts colours by alternation, so leaving it out inverts the
    // line.
    bool white = true;
    uint32_t x = 0;
    while (x < width) {
      uint32_t run = 0;
      while (x + run < width) {
        bool bit = ((row[(x + run) >> 3] >> (7u - ((x + run) & 7u))) & 1u) != 0;
        if (bit == white) {  // A set bit is black, which is not white.
          break;
        }
        run++;
      }
      if (!g31d_put_run(&o, white, run)) {
        return GIMG_ERR_INTERNAL;
      }
      x += run;
      white = !white;
    }
    // An end-of-line after every line, which is what T.4 says and what makes
    // the result readable by decoders stricter than this one's.
    g31d_put_bits(&o, 1u, 12u);
  }

  if (o.overflow) {
    return GIMG_ERR_LIMIT;
  }
  *out_size = (o.bit + 7u) / 8u;
  return GIMG_OK;
}

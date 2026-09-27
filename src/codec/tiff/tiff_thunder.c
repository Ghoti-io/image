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
 * ThunderScan, compression 32809: four-bit greyscale, coded as deltas.
 *
 * A scanner format from the late eighties, registered in the TIFF tag list
 * and implemented by almost nothing except libtiff. It stores four-bit
 * pixels, and every byte is one of four things, chosen by its top two bits:
 *
 *   00nnnnnn  repeat the previous pixel n times
 *   01aabbcc  three pixels, each a two-bit delta from the one before
 *   10aaabbb  two pixels, each a three-bit delta
 *   11xxxxxx  one pixel, stored outright in the low four bits
 *
 * A delta of 2 in the two-bit form and 4 in the three-bit form is not a
 * delta at all: it is a filler that emits nothing, which is how an encoder
 * pads a byte it has only one or two pixels left to fill.
 *
 * Rows are independent. Each begins with the previous pixel taken as zero
 * and begins on a byte boundary, so a row that ends mid-byte wastes the
 * rest of it.
 *
 * The output is packed four-bit rows, which is what an uncompressed four-bit
 * TIFF holds, so the row converter downstream needs to know nothing about
 * this.
 */

#include <ghoti.io/image/macros.h>

#include <string.h>

#include "tiff_internal.h"

/** The two-bit deltas, with the filler at index 2 (see the file comment). */
static const int thunder_delta2[4] = {0, 1, 0, -1};
/** The three-bit deltas, with the filler at index 4. */
static const int thunder_delta3[8] = {0, 1, 2, 3, 0, -3, -2, -1};
#define THUNDER_DELTA2_FILLER 2
#define THUNDER_DELTA3_FILLER 4

/** One row's worth of output, written a nibble at a time. */
typedef struct {
  unsigned char * row;
  size_t width;
  size_t at;
  unsigned last;
} thunder_row_t;

static void thunder_emit(thunder_row_t * r, int value) {
  const unsigned v = (unsigned)(value & 0xF);
  r->last = v;
  if (r->at >= r->width) {
    // A row that codes more pixels than the image is wide. The excess is
    // dropped rather than refused: it costs nothing to keep decoding, and
    // the next row starts at a known place whatever this one did.
    return;
  }
  if ((r->at & 1u) == 0u) {
    r->row[r->at >> 1] = (unsigned char)(v << 4);
  }
  else {
    r->row[r->at >> 1] |= (unsigned char)v;
  }
  r->at++;
}

GIMG_Result gimg_tiff_thunder_decode(const gimg_tiff_ifd_t * ifd,
    const unsigned char * src, size_t src_size, unsigned char * out,
    size_t out_size) {
  const uint32_t width = ifd->tiled ? ifd->tile_width : ifd->width;
  if (width == 0u) {
    return GIMG_ERR_FORMAT;
  }
  const size_t row_bytes = ((size_t)width + 1u) / 2u;
  const size_t rows = out_size / row_bytes;
  if (rows == 0u) {
    return GIMG_ERR_FORMAT;
  }
  memset(out, 0, out_size);

  size_t at = 0;
  size_t done = 0;
  for (; done < rows; done++) {
    thunder_row_t r = {out + (done * row_bytes), width, 0u, 0u};
    while (r.at < width) {
      if (at >= src_size) {
        break;
      }
      const unsigned n = src[at++];
      switch (n & 0xC0u) {
      case 0x00u: { // Repeat.
        unsigned count = n & 0x3Fu;
        while (count-- > 0u && r.at < width) {
          thunder_emit(&r, (int)r.last);
        }
        break;
      }
      case 0x40u: // Three two-bit deltas, most significant first.
        for (int shift = 4; shift >= 0; shift -= 2) {
          const unsigned d = (n >> shift) & 3u;
          if ((int)d != THUNDER_DELTA2_FILLER) {
            thunder_emit(&r, (int)r.last + thunder_delta2[d]);
          }
        }
        break;
      case 0x80u: // Two three-bit deltas.
        for (int shift = 3; shift >= 0; shift -= 3) {
          const unsigned d = (n >> shift) & 7u;
          if ((int)d != THUNDER_DELTA3_FILLER) {
            thunder_emit(&r, (int)r.last + thunder_delta3[d]);
          }
        }
        break;
      default: // Raw.
        thunder_emit(&r, (int)(n & 0xFu));
        break;
      }
    }
    if (r.at == 0u && at >= src_size) {
      break; // Out of data with nothing written: the block ends here.
    }
  }
  // A block that ran out early keeps the rows that arrived, as every other
  // short block in this codec does; one that produced nothing is data this
  // decoder could not read.
  return done == 0u ? GIMG_ERR_CORRUPT : GIMG_OK;
}

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
 * Undo a strip's or tile's compression, and its predictor.
 *
 * Every method here comes from Ghoti.io Compress rather than from a copy
 * written into this codec. That is the point of a suite: TIFF's LZW is the
 * same LZW the compress library already implements with `lzw.format` set to
 * "tiff" - most significant bit first, which is the half GIF spells the other
 * way round - and PackBits is that library's default RLE profile, named after
 * the TIFF section that defines it. Deflate is Deflate.
 *
 * What is left here is the part that is TIFF's own: which numbers name which
 * method, how large a block should come out, and the predictor.
 */

#include <ghoti.io/image/macros.h>

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/cutil/safemath.h>
#include <stdbool.h>

#include "../../core/alloc_internal.h"
#include "tiff_internal.h"

bool gimg_tiff_compression_known(uint16_t compression) {
  switch (compression) {
  case GIMG_TIFF_COMPRESSION_NONE:
  case GIMG_TIFF_COMPRESSION_LZW:
  case GIMG_TIFF_COMPRESSION_DEFLATE:
  case GIMG_TIFF_COMPRESSION_DEFLATE_OLD:
  case GIMG_TIFF_COMPRESSION_PACKBITS:
    return true;
  default:
    return false;
  }
}

/** The compress library's name for a TIFF compression number. */
static const char * tiff_method_name(uint16_t compression) {
  switch (compression) {
  case GIMG_TIFF_COMPRESSION_LZW:
    return "lzw";
  case GIMG_TIFF_COMPRESSION_DEFLATE:
  case GIMG_TIFF_COMPRESSION_DEFLATE_OLD:
    // Both numbers are a zlib stream. 8 is Adobe's registration and 32946
    // predates it; libtiff writes 8 and reads either, and so does this.
    return "zlib";
  case GIMG_TIFF_COMPRESSION_PACKBITS:
    return "rle";
  default:
    return NULL;
  }
}

/**
 * Options for one method, or NULL when it needs none.
 *
 * LZW is the only one that does, and the option is the whole difference
 * between TIFF's LZW and GIF's: the code words are packed most significant
 * bit first. A decoder handed the wrong end produces bytes rather than an
 * error, which is why this is set explicitly and not left to a default that
 * happens to be "gif".
 */
/**
 * Whether a strip holds the old bit-reversed LZW.
 *
 * TIFF's first LZW encoders, before the 1993 correction, packed their code
 * words least significant bit first - the way GIF does - and files written by
 * them are still in circulation. libtiff reads them, choosing by the same
 * two-byte test used here: a stream that begins with a zero byte whose
 * successor has its low bit set cannot be a correctly packed one, because a
 * correct stream opens with the Clear code 256 in nine bits, which is 0x80
 * followed by a byte with its top bit clear.
 *
 * That the reversed spelling *is* Ghoti.io Compress's "gif" profile is
 * measured rather than assumed: quad-lzw.tif's first strip decodes to
 * exactly its 7,680 bytes under "gif" and to nothing at all under "tiff".
 */
static bool tiff_lzw_is_reversed(const unsigned char * stored, size_t size) {
  return size >= 2u && stored[0] == 0u && (stored[1] & 1u) != 0u;
}

static GIMG_Result tiff_method_options(uint16_t compression,
    const unsigned char * stored, size_t stored_size,
    gcomp_options_t ** out_opts) {
  *out_opts = NULL;
  if (compression != GIMG_TIFF_COMPRESSION_LZW) {
    return GIMG_OK;
  }
  gcomp_options_t * opts = NULL;
  if (gcomp_options_create(&opts) != GCOMP_OK) {
    return GIMG_ERR_OOM;
  }
  const char * format =
      tiff_lzw_is_reversed(stored, stored_size) ? "gif" : "tiff";
  if (gcomp_options_set_string(opts, "lzw.format", format) != GCOMP_OK ||
      gcomp_options_set_uint64(opts, "lzw.lit_width", 8u) != GCOMP_OK) {
    gcomp_options_destroy(opts);
    return GIMG_ERR_INTERNAL;
  }
  *out_opts = opts;
  return GIMG_OK;
}

/**
 * Undo horizontal differencing (TIFF Technical Note 2, and section 14).
 *
 * Each sample after the first pixel of a row was stored as the difference
 * from the sample one pixel to its left in the *same channel*, so the sum
 * runs along the row per channel and wraps at the sample width. With
 * PlanarConfiguration 2 a row carries one channel, which is why the stride
 * between a sample and its predecessor is a parameter rather than
 * SamplesPerPixel.
 *
 * Only 8 and 16 bits are defined for it; anything else is refused at load
 * rather than silently left undifferenced, which would decode as a gradient
 * of noise that looks like a corrupt file.
 */
static void tiff_undo_predictor(const gimg_tiff_ifd_t * ifd,
    unsigned char * data, size_t size, size_t row_bytes, size_t channels) {
  if (ifd->predictor != 2u || row_bytes == 0u) {
    return;
  }
  const size_t rows = size / row_bytes;
  for (size_t y = 0; y < rows; y++) {
    unsigned char * row = data + (y * row_bytes);
    if (ifd->bits_per_sample == 8u) {
      const size_t samples = row_bytes;
      for (size_t i = channels; i < samples; i++) {
        row[i] = (unsigned char)(row[i] + row[i - channels]);
      }
    }
    else if (ifd->bits_per_sample == 16u) {
      const size_t samples = row_bytes / 2u;
      for (size_t i = channels; i < samples; i++) {
        // The sum is taken in the file's byte order, because that is the
        // order the differences were written in: swapping first and adding
        // afterwards gives a different number.
        const size_t a = i * 2u, b = (i - channels) * 2u;
        uint16_t prev = ifd->file_big_endian
            ? (uint16_t)((row[b] << 8) | row[b + 1u])
            : (uint16_t)((row[b + 1u] << 8) | row[b]);
        uint16_t here = ifd->file_big_endian
            ? (uint16_t)((row[a] << 8) | row[a + 1u])
            : (uint16_t)((row[a + 1u] << 8) | row[a]);
        const uint16_t sum = (uint16_t)(here + prev);
        if (ifd->file_big_endian) {
          row[a] = (unsigned char)(sum >> 8);
          row[a + 1u] = (unsigned char)sum;
        }
        else {
          row[a] = (unsigned char)sum;
          row[a + 1u] = (unsigned char)(sum >> 8);
        }
      }
    }
  }
}

GIMG_Result gimg_tiff_block_bytes(const gimg_tiff_doc_state_t * st,
    const gimg_tiff_ifd_t * ifd, size_t block, size_t want,
    const unsigned char ** out_bytes, size_t * out_size, bool * out_owned) {
  *out_bytes = NULL;
  *out_size = 0;
  *out_owned = false;
  if (block >= ifd->block_count) {
    return GIMG_ERR_INTERNAL;
  }
  const unsigned char * stored = st->file + ifd->block_offsets[block];
  const size_t stored_size = (size_t)ifd->block_byte_counts[block];

  if (ifd->compression == GIMG_TIFF_COMPRESSION_NONE) {
    *out_bytes = stored;
    *out_size = stored_size;
    return GIMG_OK;
  }
  const char * method = tiff_method_name(ifd->compression);
  if (!method || want == 0u) {
    return GIMG_ERR_UNSUPPORTED;
  }

  unsigned char * room = (unsigned char *)gimg_malloc(st->allocator, want);
  if (!room) {
    return GIMG_ERR_OOM;
  }
  gcomp_options_t * opts = NULL;
  GIMG_Result r =
      tiff_method_options(ifd->compression, stored, stored_size, &opts);
  if (r != GIMG_OK) {
    gimg_free(st->allocator, room);
    return r;
  }
  size_t written = 0;
  const gcomp_status_t gs = gcomp_decode_buffer(gcomp_registry_default(),
      method, opts, stored, stored_size, room, want, &written);
  if (opts) {
    gcomp_options_destroy(opts);
  }
  // A block that expands to less than its geometry says is truncated data
  // rather than a reason to refuse the picture - the rows that arrived are
  // kept, exactly as for a stored block that is short. A method that failed
  // outright before writing anything is a different matter and is refused.
  if (gs != GCOMP_OK && written == 0u) {
    gimg_free(st->allocator, room);
    return GIMG_ERR_CORRUPT;
  }
  *out_bytes = room;
  *out_size = written;
  *out_owned = true;
  return GIMG_OK;
}

void gimg_tiff_undo_block_predictor(const gimg_tiff_ifd_t * ifd,
    unsigned char * data, size_t size, size_t row_bytes, size_t channels) {
  tiff_undo_predictor(ifd, data, size, row_bytes, channels);
}

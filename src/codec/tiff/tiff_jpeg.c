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
 * JPEG-in-TIFF: compression 7, as TIFF Technical Note 2 redefined it.
 *
 * A strip or tile of such a file is a JPEG datastream with its tables taken
 * out - SOI, a frame header, a scan, EOI - and the tables live once in the
 * JPEGTables tag (347), which is itself a table-specification stream: SOI,
 * DQT and DHT, EOI. That pair is not a TIFF invention. It is T.81 B.4's two
 * abbreviated formats, which exist so a set of images can share one copy of
 * its tables, and this library's JPEG codec already reads both halves:
 * gimg_jpeg_tables_load() for the tag, GIMG_Load_Options.jpeg_tables for the
 * strip.
 *
 * So this file contains no JPEG. It hands one strip and one table set to the
 * decoder that already exists and copies the result into the right rectangle
 * of the TIFF's raster - which is the whole reason a suite of codecs in one
 * library is worth having.
 *
 * **The colour conversion is the JPEG's, not the TIFF's.** A compression-7
 * file almost always declares PhotometricInterpretation 6 and leaves
 * YCbCrSubSampling at its default, and the subsampling that matters is the
 * one in the frame header; the strip's own SOF says 2x2 or whatever it is,
 * and the JPEG decoder upsamples and converts by it. Applying the TIFF tags
 * on top would be converting twice. libtiff's RGBA reader does the same,
 * for the same reason.
 *
 * Compression 6, the 1992 spelling, comes through here too, but everything
 * particular to it is in tiff_ojpeg.c: that file writes the JPEG datastream
 * the TIFF failed to write, and this one then reads it exactly as it reads a
 * compression-7 strip.
 */

#include <ghoti.io/image/macros.h>

#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <string.h>

#include "../../core/alloc_internal.h"
#include "tiff_internal.h"

GIMG_Result gimg_tiff_jpeg_block(const gimg_tiff_doc_state_t * st,
    const gimg_tiff_ifd_t * ifd, size_t block, const GIMG_Limits * limits,
    GIMG_Raster ** out_raster) {
  *out_raster = NULL;
  if (block >= ifd->block_count) {
    return GIMG_ERR_INTERNAL;
  }

  unsigned char * assembled = NULL;
  size_t assembled_size = 0;
  const unsigned char * bytes = st->file + ifd->block_offsets[block];
  size_t byte_count = (size_t)ifd->block_byte_counts[block];

  if (ifd->compression == GIMG_TIFF_COMPRESSION_JPEG_OLD) {
    if (ifd->jpeg_interchange_size >= 4u &&
        ifd->jpeg_interchange_offset <= st->file_size &&
        ifd->jpeg_interchange_size <=
            st->file_size - ifd->jpeg_interchange_offset) {
      // The one shape of compression 6 that carries a real JPEG: tags 513
      // and 514 point at a complete datastream for the whole image, so the
      // strips are ignored and this runs once. It still needs its scan
      // header corrected, which is why it is copied rather than read where
      // it lies.
      const GIMG_Result fr = gimg_tiff_ojpeg_fix_interchange(st,
          st->file + ifd->jpeg_interchange_offset,
          (size_t)ifd->jpeg_interchange_size, &assembled, &assembled_size);
      if (fr != GIMG_OK) {
        return fr;
      }
      bytes = assembled;
      byte_count = assembled_size;
    }
    else {
      // How many image rows *this* block covers, which is what its frame
      // header has to declare.
      //
      // A tile always covers its full height - tiles are padded, which is
      // the whole point of them - but the last strip of a page covers
      // whatever is left, and declaring a strip taller than it is would ask
      // the JPEG decoder for rows the entropy data does not hold. Neither
      // file in the libtiff sample set has more than one block, so this is
      // reasoning about the format rather than about a file: the two shapes
      // that exist are one strip and one tile, and both come out the same
      // either way.
      uint32_t rows = ifd->tiled ? ifd->tile_height : ifd->rows_per_strip;
      if (rows == 0u || rows > ifd->height) {
        rows = ifd->height;
      }
      if (!ifd->tiled) {
        const uint64_t top = (uint64_t)block * rows;
        if (top >= ifd->height) {
          rows = 0u;
        }
        else if ((uint64_t)rows > ifd->height - top) {
          rows = (uint32_t)(ifd->height - top);
        }
      }
      if (rows == 0u) {
        return GIMG_ERR_CORRUPT; // A strip that begins past the last row.
      }
      const GIMG_Result ar = gimg_tiff_ojpeg_assemble(
          st, ifd, block, rows, &assembled, &assembled_size);
      if (ar != GIMG_OK) {
        return ar;
      }
      bytes = assembled;
      byte_count = assembled_size;
    }
  }

  GIMG_JPEG_Tables * tables = NULL;
  if (ifd->jpeg_tables && ifd->jpeg_tables_size &&
      ifd->compression == GIMG_TIFF_COMPRESSION_JPEG) {
    GIMG_Stream * ts = NULL;
    if (gimg_stream_create_memory(
            ifd->jpeg_tables, ifd->jpeg_tables_size, &ts) != GIMG_OK) {
      return GIMG_ERR_OOM;
    }
    const GIMG_Result tr = gimg_jpeg_tables_load(ts, &tables);
    gimg_stream_destroy(ts);
    if (tr != GIMG_OK) {
      // A JPEGTables that is not a table-specification stream is a file
      // describing tables it does not have. Refused rather than decoded
      // without them: a scan read with the wrong tables is not a warning,
      // it is a different picture.
      gimg_free(st->allocator, assembled);
      return tr;
    }
  }

  GIMG_Stream * stream = NULL;
  GIMG_Result r = gimg_stream_create_memory(bytes, byte_count, &stream);
  if (r != GIMG_OK) {
    gimg_jpeg_tables_destroy(tables);
    gimg_free(st->allocator, assembled);
    return r;
  }

  GIMG_Load_Options opts;
  memset(&opts, 0, sizeof(opts));
  opts.jpeg_tables = tables;
  opts.limits = limits;

  GIMG_Doc * doc = NULL;
  r = gimg_doc_load(stream, &opts, NULL, &doc);
  if (r == GIMG_OK && doc) {
    GIMG_Decode_Options dopts;
    memset(&dopts, 0, sizeof(dopts));
    dopts.limits = limits;
    r = gimg_item_decode(gimg_doc_item(doc, 0), &dopts, out_raster);
  }
  else if (r == GIMG_OK) {
    r = GIMG_ERR_CORRUPT;
  }
  if (doc) {
    gimg_doc_destroy(doc);
  }
  gimg_stream_destroy(stream);
  gimg_jpeg_tables_destroy(tables);
  gimg_free(st->allocator, assembled);
  return r;
}

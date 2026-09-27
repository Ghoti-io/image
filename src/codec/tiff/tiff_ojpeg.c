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
 * Old-style JPEG-in-TIFF: compression 6, the 1992 spelling.
 *
 * TIFF 6.0 section 22 described a way of putting JPEG in a TIFF that nobody
 * could implement interoperably, and Technical Note 2 replaced it with
 * compression 7 and said not to write 6 any more. Files written before that
 * still exist, so this reads them.
 *
 * **What makes it hard is that the file contains no JPEG datastream.** The
 * quantization and Huffman tables are stored as bare arrays at file offsets
 * named by tags 519, 520 and 521 - no marker, no length, no identifier - and
 * every strip or tile holds entropy-coded data with no frame header in front
 * of it. So a decoder cannot hand the bytes to a JPEG library; it has to
 * write the JPEG the file failed to write, out of the TIFF tags: the frame
 * header from ImageWidth, the block geometry and YCbCrSubSampling, the table
 * segments from those three arrays, the scan header from the component
 * count.
 *
 * One shape of the format needs none of that. When JPEGInterchangeFormat
 * (513) and its length (514) are present they point at a complete JPEG
 * datastream for the whole image, tables and frame header included, and the
 * right answer is to read it as it stands. `smallliz.tif` in the libtiff
 * sample set is that shape and `zackthecat.tif` is the other.
 *
 * Everything assembled here is *by construction* a baseline JPEG that this
 * library's own decoder then reads, so there is no second entropy decoder and
 * no second IDCT. The only thing this file knows about JPEG is the shape of
 * its headers.
 */

#include <ghoti.io/image/macros.h>

#include <ghoti.io/cutil/safemath.h>
#include <string.h>

#include "../../core/alloc_internal.h"
#include "tiff_internal.h"

/** A growable byte buffer, because a JPEG header's length is known only
 * after the thing it measures has been written. */
typedef struct {
  unsigned char * bytes;
  size_t size;
  size_t capacity;
  const GIMG_Allocator * allocator;
  bool failed;
} ojpeg_out_t;

static void ojpeg_put(ojpeg_out_t * o, const void * data, size_t n) {
  if (o->failed) {
    return;
  }
  if (o->size + n > o->capacity) {
    size_t want = o->capacity ? o->capacity * 2u : 1024u;
    while (want < o->size + n) {
      if (want > (size_t)-1 / 2u) {
        o->failed = true;
        return;
      }
      want *= 2u;
    }
    unsigned char * grown =
        (unsigned char *)gimg_realloc(o->allocator, o->bytes, want);
    if (!grown) {
      o->failed = true;
      return;
    }
    o->bytes = grown;
    o->capacity = want;
  }
  memcpy(o->bytes + o->size, data, n);
  o->size += n;
}

static void ojpeg_u8(ojpeg_out_t * o, unsigned v) {
  const unsigned char b = (unsigned char)v;
  ojpeg_put(o, &b, 1u);
}

static void ojpeg_u16(ojpeg_out_t * o, unsigned v) {
  const unsigned char b[2] = {(unsigned char)(v >> 8), (unsigned char)v};
  ojpeg_put(o, b, 2u);
}

/** Whether @p offset + @p size lies inside the file. */
static bool ojpeg_in_file(
    const gimg_tiff_doc_state_t * st, uint64_t offset, uint64_t size) {
  return offset <= st->file_size && size <= st->file_size - offset;
}

/**
 * One quantization table: 64 bytes at the offset tag 519 names.
 *
 * The 1992 tags store the table alone, so the precision nibble and the
 * identifier that a DQT segment carries are supplied here. Eight-bit
 * precision is the only thing compression 6 ever used, and JPEGProc 1 -
 * baseline - is the only process this reads at all.
 */
static bool ojpeg_write_dqt(ojpeg_out_t * o, const gimg_tiff_doc_state_t * st,
    uint64_t offset, unsigned slot) {
  if (!ojpeg_in_file(st, offset, 64u)) {
    return false;
  }
  ojpeg_u16(o, 0xFFDBu);
  ojpeg_u16(o, 2u + 1u + 64u);
  ojpeg_u8(o, slot); // Pq = 0 in the high nibble, Tq in the low.
  ojpeg_put(o, st->file + offset, 64u);
  return true;
}

/**
 * One Huffman table: sixteen counts then that many values.
 *
 * The length is not stored anywhere, which is the point at which this format
 * stops being describable without reading T.81: BITS sums to the number of
 * HUFFVAL bytes that follow, so the segment's length has to be computed from
 * the data before the data can be trusted to be there.
 */
static bool ojpeg_write_dht(ojpeg_out_t * o, const gimg_tiff_doc_state_t * st,
    uint64_t offset, unsigned class_, unsigned slot) {
  if (!ojpeg_in_file(st, offset, 16u)) {
    return false;
  }
  const unsigned char * bits = st->file + offset;
  unsigned total = 0;
  for (unsigned i = 0; i < 16u; i++) {
    total += bits[i];
  }
  if (total > 256u || !ojpeg_in_file(st, offset, 16u + total)) {
    return false;
  }
  ojpeg_u16(o, 0xFFC4u);
  ojpeg_u16(o, 2u + 1u + 16u + total);
  ojpeg_u8(o, (class_ << 4) | slot);
  ojpeg_put(o, bits, 16u + total);
  return true;
}

GIMG_Result gimg_tiff_ojpeg_assemble(const gimg_tiff_doc_state_t * st,
    const gimg_tiff_ifd_t * ifd, size_t block, uint32_t rows,
    unsigned char ** out_stream, size_t * out_size) {
  *out_stream = NULL;
  *out_size = 0;
  if (block >= ifd->block_count) {
    return GIMG_ERR_INTERNAL;
  }
  const unsigned components = ifd->samples_per_pixel;
  if (components == 0u || components > GIMG_TIFF_JPEG_MAX_TABLES) {
    return GIMG_ERR_UNSUPPORTED;
  }
  // A table per component of each class. Fewer is legal in the tags - two
  // components may share a table, and the sample set has a file where they
  // do - so a short array repeats its last entry rather than being refused,
  // which is what libtiff's reader does with the same files.
  if (ifd->jpeg_q_count == 0u || ifd->jpeg_dc_count == 0u ||
      ifd->jpeg_ac_count == 0u) {
    return GIMG_ERR_UNSUPPORTED;
  }

  const uint32_t width = ifd->tiled ? ifd->tile_width : ifd->width;
  if (width == 0u || rows == 0u) {
    return GIMG_ERR_FORMAT;
  }

  ojpeg_out_t o;
  memset(&o, 0, sizeof(o));
  o.allocator = st->allocator;

  ojpeg_u16(&o, 0xFFD8u); // SOI

  bool ok = true;
  for (unsigned c = 0; c < components && ok; c++) {
    const unsigned qi = c < ifd->jpeg_q_count ? c : ifd->jpeg_q_count - 1u;
    ok = ojpeg_write_dqt(&o, st, ifd->jpeg_q_tables[qi], c);
  }
  for (unsigned c = 0; c < components && ok; c++) {
    const unsigned di = c < ifd->jpeg_dc_count ? c : ifd->jpeg_dc_count - 1u;
    ok = ojpeg_write_dht(&o, st, ifd->jpeg_dc_tables[di], 0u, c);
  }
  for (unsigned c = 0; c < components && ok; c++) {
    const unsigned ai = c < ifd->jpeg_ac_count ? c : ifd->jpeg_ac_count - 1u;
    ok = ojpeg_write_dht(&o, st, ifd->jpeg_ac_tables[ai], 1u, c);
  }
  if (!ok) {
    gimg_free(st->allocator, o.bytes);
    return GIMG_ERR_CORRUPT;
  }

  if (ifd->jpeg_restart_interval) {
    ojpeg_u16(&o, 0xFFDDu);
    ojpeg_u16(&o, 4u);
    ojpeg_u16(&o, ifd->jpeg_restart_interval);
  }

  // SOF0. The subsampling is the TIFF's, because the frame header that would
  // have carried it does not exist: YCbCrSubSampling applies to the first
  // component and the rest are 1x1, which is what a three-component TIFF
  // means by it.
  ojpeg_u16(&o, 0xFFC0u);
  ojpeg_u16(&o, 8u + (3u * components));
  ojpeg_u8(&o, 8u); // Sample precision; compression 6 is eight-bit.
  ojpeg_u16(&o, rows);
  ojpeg_u16(&o, width);
  ojpeg_u8(&o, components);
  const bool ycbcr = ifd->photometric == GIMG_TIFF_PHOTOMETRIC_YCBCR;
  for (unsigned c = 0; c < components; c++) {
    ojpeg_u8(&o, c + 1u); // Component identifier.
    const unsigned h = (ycbcr && c == 0u) ? ifd->ycbcr_h : 1u;
    const unsigned v = (ycbcr && c == 0u) ? ifd->ycbcr_v : 1u;
    ojpeg_u8(&o, (h << 4) | v);
    ojpeg_u8(&o, c); // Tq: the table written for this component above.
  }

  // SOS. One interleaved scan over every component, which is the only thing
  // baseline compression 6 stores.
  ojpeg_u16(&o, 0xFFDAu);
  ojpeg_u16(&o, 6u + (2u * components));
  ojpeg_u8(&o, components);
  for (unsigned c = 0; c < components; c++) {
    ojpeg_u8(&o, c + 1u);
    ojpeg_u8(&o, (c << 4) | c); // Td and Ta, matching the tables above.
  }
  ojpeg_u8(&o, 0u);  // Ss
  ojpeg_u8(&o, 63u); // Se
  ojpeg_u8(&o, 0u);  // Ah and Al

  const uint64_t at = ifd->block_offsets[block];
  const uint64_t n = ifd->block_byte_counts[block];
  if (!ojpeg_in_file(st, at, n)) {
    gimg_free(st->allocator, o.bytes);
    return GIMG_ERR_CORRUPT;
  }
  ojpeg_put(&o, st->file + at, (size_t)n);
  // An EOI, which the stored entropy data may or may not already end with; a
  // second one after the first is not read, the decoder having stopped.
  ojpeg_u16(&o, 0xFFD9u);

  if (o.failed) {
    gimg_free(st->allocator, o.bytes);
    return GIMG_ERR_OOM;
  }
  *out_stream = o.bytes;
  *out_size = o.size;
  return GIMG_OK;
}

GIMG_Result gimg_tiff_ojpeg_fix_interchange(const gimg_tiff_doc_state_t * st,
    const unsigned char * bytes, size_t size, unsigned char ** out_stream,
    size_t * out_size) {
  *out_stream = NULL;
  *out_size = 0;
  if (size < 4u || bytes[0] != 0xFFu || bytes[1] != 0xD8u) {
    return GIMG_ERR_FORMAT;
  }
  unsigned char * copy = (unsigned char *)gimg_malloc(st->allocator, size);
  if (!copy) {
    return GIMG_ERR_OOM;
  }
  memcpy(copy, bytes, size);

  // Walk to the scan header, remembering which frame header was passed.
  //
  // What this is here to fix: TIFF 6.0 section 22 gave the scan header's
  // spectral selection and successive approximation fields no meaning, and
  // 1992-era encoders wrote zeros into all four. `smallliz.tif` in the
  // libtiff sample set says Ss = 0, Se = 0, which under T.81 B.2.3 names a
  // progressive DC scan - in a baseline frame, where it is nonsense. A
  // decoder that reads the standard rather than the era refuses it, so the
  // era is corrected here rather than the standard relaxed there: the fix
  // belongs to the one compression that needs it and not to the JPEG codec,
  // which every other format in this library shares.
  unsigned frame_marker = 0;
  size_t i = 2;
  while (i + 3u < size) {
    if (copy[i] != 0xFFu) {
      break;
    }
    const unsigned m = copy[i + 1u];
    if (m == 0xD8u || (m >= 0xD0u && m <= 0xD7u) || m == 0x01u) {
      i += 2u;
      continue;
    }
    const size_t len = ((size_t)copy[i + 2u] << 8) | copy[i + 3u];
    if (len < 2u || i + 2u + len > size) {
      break;
    }
    if (m >= 0xC0u && m <= 0xCFu && m != 0xC4u && m != 0xC8u && m != 0xCCu) {
      frame_marker = m;
    }
    if (m == 0xDAu) {
      // Ss, Se, Ah|Al are the last three bytes of the segment.
      const bool sequential = frame_marker == 0xC0u || frame_marker == 0xC1u;
      unsigned char * p = copy + i + 2u + len - 3u;
      if (sequential && p[0] == 0u && p[1] == 0u && p[2] == 0u) {
        p[1] = 63u;
      }
      break;
    }
    i += 2u + len;
  }
  *out_stream = copy;
  *out_size = size;
  return GIMG_OK;
}

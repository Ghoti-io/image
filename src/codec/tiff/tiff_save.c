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
 * Write a TIFF (TIFF 6.0 sections 2 and 8).
 *
 * **Every item becomes a page.** TIFF is the only format this library writes
 * that can hold several pictures as pictures rather than as frames of an
 * animation or as a thumbnail, so a document of several items goes out whole
 * and none of the "item 0 and a documented silence" that BMP and JPEG need.
 * A one-item document is an ordinary single-page TIFF.
 *
 * The file is laid out in one pass over a plan rather than written and
 * patched. Everything in a TIFF is found by absolute offset, so a writer
 * either computes its offsets before it writes a byte or seeks back to fix
 * them up - and seeking back is not available here, because an output stream
 * is append-only and the failing sink the write-failure sweep uses would have
 * no way to model it. So: measure every strip, lay the file out on paper,
 * then write it front to back.
 */

#include <ghoti.io/image/macros.h>

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/cutil/safemath.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/color.h>
#include <ghoti.io/image/ops.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <string.h>

#include "../../container/doc_internal.h"
#include "../../core/alloc_internal.h"
#include "../../core/safe_math_internal.h"
#include "../codec_internal.h"
#include "tiff_internal.h"

/** Roughly how many bytes one strip should hold, uncompressed.
 *
 * libtiff's own default, and the reason for it is the same here: a strip is
 * the unit a reader has to hold in memory, and 8 KB is small enough that a
 * very tall image does not force a huge allocation and large enough that the
 * per-strip overhead - two offsets and two byte counts - stays negligible. */
#define GIMG_TIFF_STRIP_TARGET 8192u

/** One directory entry, before it is laid out. */
typedef struct {
  uint16_t tag;
  uint16_t type;
  uint32_t count;
  /** Values, already packed in the file's byte order. Points into the page's
   * value pool when longer than four bytes. */
  unsigned char inline_bytes[4];
  const unsigned char * values;
  size_t value_size;
} tiff_entry_out_t;

/** Everything one page needs, measured before anything is written. */
typedef struct {
  GIMG_Raster * raster;    ///< Owned when `raster_owned`.
  bool raster_owned;
  uint32_t width, height;
  uint16_t samples;        ///< SamplesPerPixel.
  uint16_t bits;           ///< BitsPerSample, the same for every sample.
  uint16_t photometric;
  bool has_alpha;
  uint32_t rows_per_strip;
  size_t strip_count;
  unsigned char ** strips; ///< Each strip's bytes as they go into the file.
  size_t * strip_sizes;
  uint32_t * strip_offsets;
  uint32_t x_dpi, y_dpi;
  bool has_dpi;
  const unsigned char * icc; ///< Borrowed from the raster's colour info.
  size_t icc_size;
  const char * description;  ///< Borrowed from the document's metadata.
  const unsigned char * xmp; ///< Borrowed from the document's raw metadata.
  size_t xmp_size;
  unsigned char * xmp_copy;  ///< Owned; gimg_meta_raw_get copies into it.
  tiff_entry_out_t entries[16];
  size_t entry_count;
  unsigned char * pool;    ///< Values too long for an entry.
  size_t pool_size;
  uint32_t pool_at;        ///< Where the pool lands in the file.
  unsigned char * offsets_in_pool; ///< Where StripOffsets landed, or NULL
                                   ///< when one strip put it inline.
  size_t offsets_entry;            ///< Its index in `entries`.
} tiff_page_t;

// ---------------------------------------------------------------------------
// Byte-order-aware writers
// ---------------------------------------------------------------------------

static void tiff_put_u16(unsigned char * p, uint16_t v, bool be) {
  if (be) {
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)v;
  }
  else {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
  }
}

static void tiff_put_u32(unsigned char * p, uint32_t v, bool be) {
  if (be) {
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
  }
  else {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
  }
}

/** Write @p size bytes, refusing a short write rather than reporting one. */
static GIMG_Result tiff_write(
    GIMG_Stream * stream, const void * data, size_t size, size_t * total) {
  size_t wrote = 0;
  const GIMG_Result r = gimg_stream_write(stream, data, size, &wrote);
  if (r != GIMG_OK) {
    return r;
  }
  if (wrote != size) {
    return GIMG_ERR_IO;
  }
  *total += wrote;
  return GIMG_OK;
}

// ---------------------------------------------------------------------------
// Measuring one page
// ---------------------------------------------------------------------------

/** What this writer makes of a raster's pixel format, or false to refuse. */
static bool tiff_describe_raster(const GIMG_Pixel_Format * fmt,
    uint16_t * out_samples, uint16_t * out_bits, uint16_t * out_photometric,
    bool * out_alpha) {
  if (!fmt) {
    return false;
  }
  const unsigned bits = gimg_pixel_format_channel_bits(fmt, 0);
  if (bits != 8u && bits != 16u) {
    return false;
  }
  for (unsigned i = 1; i < fmt->channel_count; i++) {
    if (gimg_pixel_format_channel_bits(fmt, i) != bits) {
      return false; // TIFF allows it; this writer has no reason to produce it.
    }
  }
  *out_bits = (uint16_t)bits;
  *out_alpha = false;
  switch (fmt->channel_model) {
  case GIMG_CHANNEL_GRAY:
    *out_samples = 1u;
    *out_photometric = GIMG_TIFF_PHOTOMETRIC_BLACK_IS_ZERO;
    return fmt->channel_count == 1u;
  case GIMG_CHANNEL_RGBA:
    *out_samples = 4u;
    *out_photometric = GIMG_TIFF_PHOTOMETRIC_RGB;
    *out_alpha = true;
    return fmt->channel_count == 4u;
  case GIMG_CHANNEL_RGB:
    *out_samples = 3u;
    *out_photometric = GIMG_TIFF_PHOTOMETRIC_RGB;
    return fmt->channel_count == 3u;
  case GIMG_CHANNEL_CMYK:
    *out_samples = 4u;
    *out_photometric = GIMG_TIFF_PHOTOMETRIC_CMYK;
    return fmt->channel_count == 4u;
  default:
    return false;
  }
}

/** The compress library's name for a compression option, or NULL for none. */
static const char * tiff_save_method(uint8_t compression) {
  switch (compression) {
  case GIMG_TIFF_COMPRESS_PACKBITS:
    return "rle";
  case GIMG_TIFF_COMPRESS_LZW:
    return "lzw";
  case GIMG_TIFF_COMPRESS_DEFLATE:
    return "zlib";
  default:
    return NULL;
  }
}

static uint16_t tiff_save_compression_tag(uint8_t compression) {
  switch (compression) {
  case GIMG_TIFF_COMPRESS_PACKBITS:
    return GIMG_TIFF_COMPRESSION_PACKBITS;
  case GIMG_TIFF_COMPRESS_LZW:
    return GIMG_TIFF_COMPRESSION_LZW;
  case GIMG_TIFF_COMPRESS_DEFLATE:
    return GIMG_TIFF_COMPRESSION_DEFLATE;
  default:
    return GIMG_TIFF_COMPRESSION_NONE;
  }
}

/** Horizontal differencing, the inverse of what the decoder undoes. */
static void tiff_apply_predictor(unsigned char * data, size_t size,
    size_t row_bytes, size_t channels, unsigned bits, bool be) {
  if (row_bytes == 0u) {
    return;
  }
  const size_t rows = size / row_bytes;
  for (size_t y = 0; y < rows; y++) {
    unsigned char * row = data + (y * row_bytes);
    if (bits == 8u) {
      for (size_t i = row_bytes; i-- > channels;) {
        row[i] = (unsigned char)(row[i] - row[i - channels]);
      }
    }
    else {
      const size_t samples = row_bytes / 2u;
      for (size_t i = samples; i-- > channels;) {
        const size_t a = i * 2u, b = (i - channels) * 2u;
        const uint16_t prev = be ? (uint16_t)((row[b] << 8) | row[b + 1u])
                                 : (uint16_t)((row[b + 1u] << 8) | row[b]);
        const uint16_t here = be ? (uint16_t)((row[a] << 8) | row[a + 1u])
                                 : (uint16_t)((row[a + 1u] << 8) | row[a]);
        const uint16_t diff = (uint16_t)(here - prev);
        tiff_put_u16(row + a, diff, be);
      }
    }
  }
}

/**
 * Pack one strip's rows out of the raster, in the file's byte order.
 *
 * A 16-bit sample is written in the order the header declares, which is the
 * whole reason this takes @p be: the raster holds host-order values and the
 * file holds whichever order the caller asked for.
 */
static void tiff_pack_strip(const tiff_page_t * page, const uint8_t * pixels,
    size_t stride, uint32_t first_row, uint32_t rows, unsigned char * out,
    bool be) {
  const size_t row_bytes =
      (size_t)page->width * page->samples * (page->bits / 8u);
  for (uint32_t y = 0; y < rows; y++) {
    const uint8_t * src = pixels + ((size_t)(first_row + y) * stride);
    unsigned char * dst = out + ((size_t)y * row_bytes);
    if (page->bits == 8u) {
      memcpy(dst, src, row_bytes);
      continue;
    }
    const uint16_t * wide = (const uint16_t *)(const void *)src;
    const size_t samples = (size_t)page->width * page->samples;
    for (size_t i = 0; i < samples; i++) {
      tiff_put_u16(dst + (i * 2u), wide[i], be);
    }
  }
}

/**
 * Add one entry to a page's directory, and put its values where they belong.
 *
 * **This is the only place that decides inline against pool**, and it both
 * decides and copies. It used to decide here and reserve the space in the
 * caller, and the two disagreed the moment a value happened to be four bytes
 * or fewer: a one-sample image's BitsPerSample went inline while the caller
 * had still reserved two bytes for it, which slid every later value two bytes
 * and made every strip offset point into the value before it. The file was
 * the right length and its directory read perfectly; only the pixels were
 * somewhere else.
 *
 * @return Where the values landed in the pool, so a caller that must fill
 *   them in later - StripOffsets, whose values are not known until the file
 *   has a shape - can find them. NULL when they went inline.
 */
static unsigned char * tiff_add_entry(tiff_page_t * page, uint16_t tag,
    uint16_t type, uint32_t count, const unsigned char * values,
    size_t value_size) {
  if (page->entry_count >= sizeof(page->entries) / sizeof(page->entries[0])) {
    return NULL;
  }
  tiff_entry_out_t * e = &page->entries[page->entry_count++];
  e->tag = tag;
  e->type = type;
  e->count = count;
  e->values = NULL;
  e->value_size = value_size;
  memset(e->inline_bytes, 0, sizeof(e->inline_bytes));
  if (value_size <= 4u) {
    // Four bytes or fewer live in the entry itself, left-justified and in the
    // file's byte order (section 2, "IFD Entry").
    memcpy(e->inline_bytes, values, value_size);
    return NULL;
  }
  unsigned char * where = page->pool + page->pool_size;
  memcpy(where, values, value_size);
  page->pool_size += value_size;
  e->values = where;
  return where;
}

static void tiff_free_page(const GIMG_Allocator * alloc, tiff_page_t * page) {
  if (page->strips) {
    for (size_t i = 0; i < page->strip_count; i++) {
      gimg_free(alloc, page->strips[i]);
    }
  }
  gimg_free(alloc, page->strips);
  gimg_free(alloc, page->strip_sizes);
  gimg_free(alloc, page->strip_offsets);
  gimg_free(alloc, page->pool);
  gimg_free(alloc, page->xmp_copy);
  if (page->raster_owned && page->raster) {
    gimg_raster_destroy(page->raster);
  }
  memset(page, 0, sizeof(*page));
}

/**
 * Compress every strip of a page, or copy it when nothing is asked for.
 *
 * The strips are kept rather than streamed because a TIFF's StripByteCounts
 * has to be written before the strips are, and a compressed strip's length
 * is not known until it exists.
 */
static GIMG_Result tiff_build_strips(const GIMG_Allocator * alloc,
    tiff_page_t * page, const GIMG_Save_Options * options, bool be) {
  const uint8_t compression = options ? options->tiff_compression : 0u;
  const char * method = tiff_save_method(compression);
  const uint8_t predictor = options ? options->tiff_predictor : 0u;
  const size_t row_bytes =
      (size_t)page->width * page->samples * (page->bits / 8u);

  uint32_t rows = options ? options->tiff_rows_per_strip : 0u;
  if (rows == 0u) {
    rows = (uint32_t)(GIMG_TIFF_STRIP_TARGET / (row_bytes ? row_bytes : 1u));
    if (rows == 0u) {
      rows = 1u;
    }
  }
  if (rows > page->height) {
    rows = page->height;
  }
  page->rows_per_strip = rows;
  page->strip_count = ((size_t)page->height + rows - 1u) / rows;

  page->strips = (unsigned char **)gimg_calloc(
      alloc, page->strip_count, sizeof(unsigned char *));
  page->strip_sizes =
      (size_t *)gimg_calloc(alloc, page->strip_count, sizeof(size_t));
  page->strip_offsets =
      (uint32_t *)gimg_calloc(alloc, page->strip_count, sizeof(uint32_t));
  if (!page->strips || !page->strip_sizes || !page->strip_offsets) {
    return GIMG_ERR_OOM;
  }

  const uint8_t * pixels =
      (const uint8_t *)gimg_raster_pixels_const(page->raster);
  const size_t stride = gimg_raster_stride_bytes(page->raster);

  for (size_t s = 0; s < page->strip_count; s++) {
    const uint32_t first = (uint32_t)(s * rows);
    const uint32_t count =
        (page->height - first < rows) ? (page->height - first) : rows;
    size_t raw_size = 0;
    if (!gcu_safe_mul_size(count, row_bytes, &raw_size)) {
      return GIMG_ERR_LIMIT;
    }
    unsigned char * raw = (unsigned char *)gimg_malloc(alloc, raw_size);
    if (!raw) {
      return GIMG_ERR_OOM;
    }
    tiff_pack_strip(page, pixels, stride, first, count, raw, be);
    if (predictor == 2u) {
      tiff_apply_predictor(
          raw, raw_size, row_bytes, page->samples, page->bits, be);
    }
    if (!method) {
      page->strips[s] = raw;
      page->strip_sizes[s] = raw_size;
      continue;
    }
    // Room for a strip that does not compress, and the bound is twice the
    // input rather than the textbook one.
    //
    // PackBits *ought* to expand by at most one byte per 128 (section 9), and
    // a first draft allowed exactly that. Measured on a photographic strip of
    // 7,680 bytes, Ghoti.io Compress's RLE encoder produces 8,679 - about one
    // byte in eight rather than one in 128 - because it spells a two-byte run
    // as a repeat instead of folding it into the literal around it. That is
    // legal PackBits and a legal file, and it is not this codec's to fix;
    // what it changes here is the bound, because a writer that sizes its
    // buffer by what an encoder ought to do fails on the data where it does
    // not. Twice the input is the true worst case for any legal PackBits
    // output: a one-byte literal run costs two bytes, and nothing costs more.
    size_t room = 0;
    if (!gcu_safe_mul_size(raw_size, 2u, &room) ||
        !gcu_safe_add_size(room, 64u, &room)) {
      gimg_free(alloc, raw);
      return GIMG_ERR_LIMIT;
    }
    unsigned char * packed = (unsigned char *)gimg_malloc(alloc, room);
    if (!packed) {
      gimg_free(alloc, raw);
      return GIMG_ERR_OOM;
    }
    gcomp_options_t * opts = NULL;
    if (compression == GIMG_TIFF_COMPRESS_LZW) {
      if (gcomp_options_create(&opts) != GCOMP_OK ||
          gcomp_options_set_string(opts, "lzw.format", "tiff") != GCOMP_OK ||
          gcomp_options_set_uint64(opts, "lzw.lit_width", 8u) != GCOMP_OK) {
        if (opts) {
          gcomp_options_destroy(opts);
        }
        gimg_free(alloc, raw);
        gimg_free(alloc, packed);
        return GIMG_ERR_INTERNAL;
      }
    }
    size_t written = 0;
    const gcomp_status_t gs = gcomp_encode_buffer(gcomp_registry_default(),
        method, opts, raw, raw_size, packed, room, &written);
    if (opts) {
      gcomp_options_destroy(opts);
    }
    gimg_free(alloc, raw);
    if (gs != GCOMP_OK) {
      gimg_free(alloc, packed);
      return gs == GCOMP_ERR_MEMORY ? GIMG_ERR_OOM : GIMG_ERR_INTERNAL;
    }
    page->strips[s] = packed;
    page->strip_sizes[s] = written;
  }
  return GIMG_OK;
}

/** Build one page's directory entries into its value pool. */
static GIMG_Result tiff_build_entries(const GIMG_Allocator * alloc,
    tiff_page_t * page, const GIMG_Save_Options * options, bool be) {
  // Room for every value that could need the pool. tiff_add_entry decides
  // which actually do, and this is only an upper bound on what it may use.
  size_t need = 0;
  need += (size_t)page->samples * 2u;  // BitsPerSample
  need += page->strip_count * 4u * 2u; // StripOffsets, StripByteCounts
  need += 8u * 2u;                     // Two RATIONAL resolutions
  need += page->icc_size;              // The profile, if there is one
  need += page->xmp_size;              // The XMP packet, if there is one
  need += page->description ? strlen(page->description) + 1u : 0u;
  need += 16u;
  page->pool = (unsigned char *)gimg_calloc(alloc, need, 1u);
  if (!page->pool) {
    return GIMG_ERR_OOM;
  }
  page->pool_size = 0;

  unsigned char scratch[8];
  tiff_put_u32(scratch, page->width, be);
  tiff_add_entry(page, GIMG_TIFF_TAG_IMAGE_WIDTH, GIMG_TIFF_TYPE_LONG, 1u,
      scratch, 4u);
  tiff_put_u32(scratch, page->height, be);
  tiff_add_entry(page, GIMG_TIFF_TAG_IMAGE_LENGTH, GIMG_TIFF_TYPE_LONG, 1u,
      scratch, 4u);

  // BitsPerSample is one value per sample, so it is two bytes for grayscale
  // and goes inline, and six or eight for colour and goes in the pool. That
  // difference is exactly what the note on tiff_add_entry is about.
  unsigned char bps[8];
  for (uint16_t i = 0; i < page->samples && i < 4u; i++) {
    tiff_put_u16(bps + (i * 2u), page->bits, be);
  }
  tiff_add_entry(page, GIMG_TIFF_TAG_BITS_PER_SAMPLE, GIMG_TIFF_TYPE_SHORT,
      page->samples, bps, (size_t)page->samples * 2u);

  tiff_put_u16(scratch,
      tiff_save_compression_tag(options ? options->tiff_compression : 0u), be);
  tiff_add_entry(page, GIMG_TIFF_TAG_COMPRESSION, GIMG_TIFF_TYPE_SHORT, 1u,
      scratch, 2u);
  tiff_put_u16(scratch, page->photometric, be);
  tiff_add_entry(page, GIMG_TIFF_TAG_PHOTOMETRIC, GIMG_TIFF_TYPE_SHORT, 1u,
      scratch, 2u);

  // Zeroes for now: the offsets are not known until the file has a shape, and
  // the entry has to exist before the shape can be computed. Whichever of the
  // two places they end up - inline for a single strip, the pool for more -
  // is remembered so they can be filled in.
  size_t offsets_bytes = page->strip_count * 4u;
  unsigned char * offsets_scratch = (unsigned char *)gimg_calloc(
      alloc, offsets_bytes ? offsets_bytes : 1u, 1u);
  if (!offsets_scratch) {
    return GIMG_ERR_OOM;
  }
  page->offsets_in_pool = tiff_add_entry(page, GIMG_TIFF_TAG_STRIP_OFFSETS,
      GIMG_TIFF_TYPE_LONG, (uint32_t)page->strip_count, offsets_scratch,
      offsets_bytes);
  page->offsets_entry = page->entry_count - 1u;
  gimg_free(alloc, offsets_scratch);

  tiff_put_u16(scratch, page->samples, be);
  tiff_add_entry(page, GIMG_TIFF_TAG_SAMPLES_PER_PIXEL, GIMG_TIFF_TYPE_SHORT,
      1u, scratch, 2u);
  tiff_put_u32(scratch, page->rows_per_strip, be);
  tiff_add_entry(page, GIMG_TIFF_TAG_ROWS_PER_STRIP, GIMG_TIFF_TYPE_LONG, 1u,
      scratch, 4u);

  unsigned char * counts = (unsigned char *)gimg_calloc(
      alloc, offsets_bytes ? offsets_bytes : 1u, 1u);
  if (!counts) {
    return GIMG_ERR_OOM;
  }
  for (size_t i = 0; i < page->strip_count; i++) {
    tiff_put_u32(counts + (i * 4u), (uint32_t)page->strip_sizes[i], be);
  }
  tiff_add_entry(page, GIMG_TIFF_TAG_STRIP_BYTE_COUNTS, GIMG_TIFF_TYPE_LONG,
      (uint32_t)page->strip_count, counts, offsets_bytes);
  gimg_free(alloc, counts);

  if (page->has_dpi) {
    unsigned char res[8];
    tiff_put_u32(res, page->x_dpi, be);
    tiff_put_u32(res + 4u, 1u, be);
    tiff_add_entry(page, GIMG_TIFF_TAG_X_RESOLUTION,
        GIMG_TIFF_TYPE_RATIONAL, 1u, res, 8u);
    tiff_put_u32(res, page->y_dpi, be);
    tiff_put_u32(res + 4u, 1u, be);
    tiff_add_entry(page, GIMG_TIFF_TAG_Y_RESOLUTION,
        GIMG_TIFF_TYPE_RATIONAL, 1u, res, 8u);
  }

  tiff_put_u16(scratch, 1u, be); // PlanarConfiguration: interleaved.
  tiff_add_entry(page, GIMG_TIFF_TAG_PLANAR_CONFIG, GIMG_TIFF_TYPE_SHORT, 1u,
      scratch, 2u);

  if (page->has_dpi) {
    tiff_put_u16(scratch, 2u, be); // Inches.
    tiff_add_entry(page, GIMG_TIFF_TAG_RESOLUTION_UNIT, GIMG_TIFF_TYPE_SHORT,
        1u, scratch, 2u);
  }
  if (options && options->tiff_predictor == 2u) {
    tiff_put_u16(scratch, 2u, be);
    tiff_add_entry(page, GIMG_TIFF_TAG_PREDICTOR, GIMG_TIFF_TYPE_SHORT, 1u,
        scratch, 2u);
  }
  // Tags above 700 in number order, which is where the metadata lands.
  if (page->description) {
    const size_t n = strlen(page->description) + 1u;
    tiff_add_entry(page, GIMG_TIFF_TAG_IMAGE_DESCRIPTION,
        GIMG_TIFF_TYPE_ASCII, (uint32_t)n,
        (const unsigned char *)page->description, n);
  }
  // **No Orientation tag, deliberately.**
  //
  // gimg_item_decode applies the orientation for every codec in this
  // library - a raster that reaches a writer is the display image, already
  // rotated - so writing the source's tag beside those pixels would have a
  // reader rotate them a second time. The first draft did exactly that, and
  // the round trip came back rotated twice.
  //
  // Absent means 1, which is what these pixels are, so nothing is lost: the
  // orientation was not dropped, it was applied.
  if (page->has_alpha) {
    // Unassociated, which is what this library's RGBA means: the colour is
    // not premultiplied. Saying nothing would leave a reader to guess, and
    // libtiff's guess is that a fourth sample it was not told about is
    // unspecified rather than alpha.
    tiff_put_u16(scratch, GIMG_TIFF_EXTRA_UNASSOCIATED_ALPHA, be);
    tiff_add_entry(page, GIMG_TIFF_TAG_EXTRA_SAMPLES, GIMG_TIFF_TYPE_SHORT,
        1u, scratch, 2u);
  }
  if (page->xmp && page->xmp_size > 0u) {
    tiff_add_entry(page, GIMG_TIFF_TAG_XMP, GIMG_TIFF_TYPE_BYTE,
        (uint32_t)page->xmp_size, page->xmp, page->xmp_size);
  }
  if (page->icc && page->icc_size > 0u) {
    // A TIFF is what professional colour work is stored in, so a profile
    // that arrived has to leave again: dropping it makes the file's colours
    // mean something else without saying so.
    tiff_add_entry(page, GIMG_TIFF_TAG_ICC_PROFILE, GIMG_TIFF_TYPE_UNDEFINED,
        (uint32_t)page->icc_size, page->icc, page->icc_size);
  }
  return GIMG_OK;
}

GIMG_Result gimg_tiff_save(GIMG_Codec * codec, const GIMG_Doc * doc,
    GIMG_Stream * stream, const char * format_name,
    const GIMG_Save_Options * options, GIMG_Save_Report * report) {
  (void)format_name;
  if (!codec || !doc || !stream || !report) {
    return GIMG_ERR_INTERNAL;
  }
  report->bytes_written = 0;
  const GIMG_Allocator * alloc = gimg_alloc_or_default(codec->allocator);
  const bool be = options && options->tiff_big_endian;

  const size_t pages = gimg_doc_item_count(doc);
  if (pages == 0u) {
    return GIMG_ERR_FORMAT;
  }
  const uint8_t compression = options ? options->tiff_compression : 0u;
  if (compression > GIMG_TIFF_COMPRESS_DEFLATE) {
    return GIMG_ERR_UNSUPPORTED;
  }
  const uint8_t predictor = options ? options->tiff_predictor : 0u;
  if (predictor > 2u) {
    return GIMG_ERR_UNSUPPORTED;
  }
  if (predictor == 2u && compression == 0u) {
    // Horizontal differencing without a compressor behind it makes a file
    // larger and harder to read for nothing at all. Refusing is better than
    // honouring a request that can only have been a mistake.
    return GIMG_ERR_UNSUPPORTED;
  }

  tiff_page_t * plan =
      (tiff_page_t *)gimg_calloc(alloc, pages, sizeof(tiff_page_t));
  if (!plan) {
    return GIMG_ERR_OOM;
  }
  GIMG_Result r = GIMG_OK;

  // ---- Measure every page --------------------------------------------
  for (size_t i = 0; i < pages && r == GIMG_OK; i++) {
    tiff_page_t * page = &plan[i];
    GIMG_Item * item = gimg_doc_item((GIMG_Doc *)doc, i);
    if (!item) {
      r = GIMG_ERR_INTERNAL;
      break;
    }
    page->raster = gimg_item_raster(item);
    if (!page->raster) {
      GIMG_Result dr = gimg_item_decode(item, NULL, &page->raster);
      if (dr != GIMG_OK || !page->raster) {
        r = (dr == GIMG_OK) ? GIMG_ERR_FORMAT : dr;
        break;
      }
      page->raster_owned = true;
    }
    if (!tiff_describe_raster(gimg_raster_format(page->raster),
            &page->samples, &page->bits, &page->photometric,
            &page->has_alpha)) {
      r = GIMG_ERR_UNSUPPORTED;
      break;
    }
    if (predictor == 2u && page->bits != 8u && page->bits != 16u) {
      r = GIMG_ERR_UNSUPPORTED;
      break;
    }
    page->width = gimg_raster_width(page->raster);
    page->height = gimg_raster_height(page->raster);
    if (page->width == 0u || page->height == 0u) {
      r = GIMG_ERR_FORMAT;
      break;
    }
    const GIMG_Color_Info * info =
        gimg_raster_color_info_const(page->raster);
    if (info && info->icc_bytes && info->icc_size > 0u) {
      page->icc = (const unsigned char *)info->icc_bytes;
      page->icc_size = info->icc_size;
    }
    GIMG_Meta_Common * common = gimg_doc_meta_common(doc);
    if (common) {
      uint32_t x = 0, y = 0;
      gimg_meta_common_dpi(common, &x, &y);
      if (x > 0u && y > 0u) {
        page->x_dpi = x;
        page->y_dpi = y;
        page->has_dpi = true;
      }
      page->description = gimg_meta_common_description(common);
      if (page->description && page->description[0] == '\0') {
        page->description = NULL;
      }
    }
    GIMG_Meta_Raw * raw = gimg_doc_meta_raw(doc);
    if (raw) {
      size_t xmp_size = 0;
      if (gimg_meta_raw_get(raw, "tiff", GIMG_TIFF_TAG_XMP, NULL,
              &xmp_size) == GIMG_OK &&
          xmp_size > 0u) {
        page->xmp_copy = (unsigned char *)gimg_malloc(alloc, xmp_size);
        if (page->xmp_copy &&
            gimg_meta_raw_get(raw, "tiff", GIMG_TIFF_TAG_XMP, page->xmp_copy,
                &xmp_size) == GIMG_OK) {
          page->xmp = page->xmp_copy;
          page->xmp_size = xmp_size;
        }
      }
    }
    r = tiff_build_strips(alloc, page, options, be);
    if (r == GIMG_OK) {
      r = tiff_build_entries(alloc, page, options, be);
    }
  }

  // ---- Lay the file out ------------------------------------------------
  //
  // Header, then every page's strips, then every page's directory followed
  // by its own value pool. Nothing is patched afterwards, so every offset
  // below has to be right the first time.
  size_t cursor = 8u;
  for (size_t i = 0; i < pages && r == GIMG_OK; i++) {
    tiff_page_t * page = &plan[i];
    for (size_t s = 0; s < page->strip_count; s++) {
      if (cursor > 0xFFFFFFFFu) {
        r = GIMG_ERR_LIMIT; // Past what a classic TIFF offset can say.
        break;
      }
      page->strip_offsets[s] = (uint32_t)cursor;
      cursor += page->strip_sizes[s];
      cursor += cursor & 1u; // Keep every offset even, as the format asks.
    }
  }
  size_t ifd_at[64];
  const bool many = pages > (sizeof(ifd_at) / sizeof(ifd_at[0]));
  size_t * directory_at = many
      ? (size_t *)gimg_calloc(alloc, pages, sizeof(size_t))
      : ifd_at;
  if (!directory_at) {
    r = GIMG_ERR_OOM;
  }
  for (size_t i = 0; i < pages && r == GIMG_OK; i++) {
    directory_at[i] = cursor;
    cursor += 2u + (plan[i].entry_count * 12u) + 4u;
    plan[i].pool_at = (uint32_t)cursor;
    cursor += plan[i].pool_size;
    if (cursor > 0xFFFFFFFFu) {
      r = GIMG_ERR_LIMIT;
    }
  }
  // The strip offsets are known now, so the entry that names them can be
  // filled in - it was reserved with the right length and nothing else.
  for (size_t i = 0; i < pages && r == GIMG_OK; i++) {
    tiff_page_t * page = &plan[i];
    if (page->offsets_in_pool) {
      for (size_t s = 0; s < page->strip_count; s++) {
        tiff_put_u32(page->offsets_in_pool + (s * 4u), page->strip_offsets[s],
            be);
      }
    }
    else if (page->strip_count > 0u) {
      // One strip, so the offset lives in the entry itself.
      tiff_put_u32(page->entries[page->offsets_entry].inline_bytes,
          page->strip_offsets[0], be);
    }
  }

  // ---- Write it front to back ------------------------------------------
  size_t total = 0;
  if (r == GIMG_OK) {
    unsigned char header[8];
    header[0] = be ? 'M' : 'I';
    header[1] = header[0];
    tiff_put_u16(header + 2, 42u, be);
    tiff_put_u32(header + 4, (uint32_t)directory_at[0], be);
    r = tiff_write(stream, header, sizeof(header), &total);
  }
  const unsigned char pad = 0u;
  for (size_t i = 0; i < pages && r == GIMG_OK; i++) {
    tiff_page_t * page = &plan[i];
    for (size_t s = 0; s < page->strip_count && r == GIMG_OK; s++) {
      r = tiff_write(stream, page->strips[s], page->strip_sizes[s], &total);
      if (r == GIMG_OK && (page->strip_sizes[s] & 1u)) {
        r = tiff_write(stream, &pad, 1u, &total);
      }
    }
  }
  for (size_t i = 0; i < pages && r == GIMG_OK; i++) {
    tiff_page_t * page = &plan[i];
    unsigned char count[2];
    tiff_put_u16(count, (uint16_t)page->entry_count, be);
    r = tiff_write(stream, count, sizeof(count), &total);
    // Entries are written in tag order, which TIFF 6.0 section 2 requires
    // and which tiff_build_entries already produces by construction.
    uint32_t pool_cursor = page->pool_at;
    for (size_t e = 0; e < page->entry_count && r == GIMG_OK; e++) {
      unsigned char raw[12];
      tiff_put_u16(raw, page->entries[e].tag, be);
      tiff_put_u16(raw + 2, page->entries[e].type, be);
      tiff_put_u32(raw + 4, page->entries[e].count, be);
      if (page->entries[e].value_size <= 4u) {
        memcpy(raw + 8, page->entries[e].inline_bytes, 4u);
      }
      else {
        tiff_put_u32(raw + 8, pool_cursor, be);
        pool_cursor += (uint32_t)page->entries[e].value_size;
      }
      r = tiff_write(stream, raw, sizeof(raw), &total);
    }
    if (r == GIMG_OK) {
      unsigned char next[4];
      tiff_put_u32(next,
          (i + 1u < pages) ? (uint32_t)directory_at[i + 1u] : 0u, be);
      r = tiff_write(stream, next, sizeof(next), &total);
    }
    if (r == GIMG_OK && page->pool_size > 0u) {
      r = tiff_write(stream, page->pool, page->pool_size, &total);
    }
  }

  for (size_t i = 0; i < pages; i++) {
    tiff_free_page(alloc, &plan[i]);
  }
  gimg_free(alloc, plan);
  if (many && directory_at) {
    gimg_free(alloc, directory_at);
  }
  report->bytes_written = total;
  return r;
}

/**
 * @file
 *
 * BMP save: raster -> BITMAPINFOHEADER bitmap, indexed or true color.
 *
 * Copyright 2026 by Corey Pennycuff
 *
 * --- Internal algorithms and design ---
 *
 * Form selection, in order.  A raster whose alpha is not uniformly opaque is
 * written as 32-bit BI_BITFIELDS with an explicit alpha mask, since 32-bit
 * BI_RGB leaves the fourth byte undefined and readers disagree about it - see
 * GIMG_Load_Options.bmp_rgb32_alpha for the other side of that.  An opaque
 * raster of no more than 256 distinct colors is written through a palette
 * when that is the smaller file, at the smallest depth that holds the indices
 * - 1, 4 or 8.  Everything else is 24-bit BI_RGB, which is the most widely
 * readable BMP there is.
 *
 * The palette is lossless and not color quantization: with 256 colors or
 * fewer there is exactly one palette that reproduces the image, so nothing is
 * being decided about the picture, only about how it is stored.  Which form
 * is smaller is arithmetic here rather than a measurement, because a BMP's
 * size follows from its stride and height alone; the PNG writer has to encode
 * both forms to find out, since DEFLATE makes it unpredictable.
 *
 * 2 bits per pixel is read but never written.  It is a Windows CE addition
 * that the desktop API does not accept, and an image that fits in 4 colors
 * fits in 1 or 4 bits as well, so writing it would cost portability for at
 * most a byte a row.
 *
 * Counting colors uses an open-addressed table keyed on the packed RGB, so a
 * pixel costs one probe rather than a walk of everything seen so far.  It
 * gives up at 257 distinct colors: the answer past that point is only ever
 * "too many".
 *
 * Row order: bottom-up with a positive height by default, the layout every
 * BMP reader handles.  GIMG_Save_Options.bmp_top_down writes them the other
 * way, which is legal from BITMAPINFOHEADER onwards and is refused together
 * with RLE, whose end-of-line walks one way only.
 *
 * Source formats: the raster is read through gimg_raster_* accessors for RGBA8
 * and GRAY8, the two 8-bit formats the library decodes to.  Anything else is
 * reported as unsupported rather than reinterpreted.
 *
 * Resolution: biXPelsPerMeter and biYPelsPerMeter are written from the
 * document's common metadata when it states a dpi, and left at zero - "not
 * stated", which is what most writers emit - when it does not.  This file
 * used to write a fixed 2835 (72 dpi) into every image, which is a claim
 * about the picture that nothing in it supported.
 */

#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/ops.h>
#include <ghoti.io/image/raster.h>
#include <string.h>

#include "../../container/doc_internal.h"
#include "../../core/alloc_internal.h"
#include "../../core/resolution_internal.h"
#include "../../core/safe_math_internal.h"
#include "../codec_internal.h"
#include "bmp_internal.h"

/** Append a little-endian 16-bit value to a byte cursor. */
static void bmp_write_u16(unsigned char * p, uint16_t value) {
  p[0] = (unsigned char)(value & 0xFFu);
  p[1] = (unsigned char)((value >> 8) & 0xFFu);
}


/** Write a whole buffer, treating a short write as an I/O error. */
static GIMG_Result bmp_write_all(
    GIMG_Stream * stream, const void * buffer, size_t size) {
  size_t written = 0;
  GIMG_Result r = gimg_stream_write(stream, buffer, size, &written);
  if (r != GIMG_OK) {
    return r;
  }
  return written == size ? GIMG_OK : GIMG_ERR_IO;
}

/** True when the format is one this encoder can read samples from. */
static bool bmp_format_supported(const GIMG_Pixel_Format * f) {
  if (!f || f->layout != GIMG_LAYOUT_INTERLEAVED ||
      f->channel_type != GIMG_CHANNEL_UNORM) {
    return false;
  }
  if (f->channel_model == GIMG_CHANNEL_RGBA && f->channel_count == 4 &&
      f->bits_per_channel[0] == 8) {
    return true;
  }
  if (f->channel_model == GIMG_CHANNEL_GRAY && f->channel_count == 1 &&
      f->bits_per_channel[0] == 8) {
    return true;
  }
  return false;
}

/**
 * Whether a raster this writer cannot store could be narrowed into one.
 *
 * A BMP sample is a byte at most - the format has no deeper form - so a 12-
 * or 16-bit raster is written by restating it at 8 bits, which is what every
 * other writer of this format does.  Before this, saving a 16-bit PNG as a
 * BMP returned GIMG_ERR_UNSUPPORTED, so the two formats could not be
 * converted between at all in that direction.
 *
 * CMYK is deliberately not here: narrowing it to 8 bits would still leave
 * four ink amounts, and turning those into RGB is a color conversion this
 * library does not do - see the format page.
 */
static bool bmp_format_narrowable(const GIMG_Pixel_Format * f) {
  if (!f || f->layout != GIMG_LAYOUT_INTERLEAVED ||
      f->channel_type != GIMG_CHANNEL_UNORM) {
    return false;
  }
  if (f->bits_per_channel[0] != 12 && f->bits_per_channel[0] != 16) {
    return false;
  }
  return (f->channel_model == GIMG_CHANNEL_RGBA && f->channel_count == 4) ||
      (f->channel_model == GIMG_CHANNEL_GRAY && f->channel_count == 1);
}

/** Read one pixel as RGBA8 from a supported raster format. */
static void bmp_sample(const GIMG_Pixel_Format * f, const uint8_t * row,
    uint32_t x, uint8_t out[4]) {
  if (f->channel_model == GIMG_CHANNEL_GRAY) {
    uint8_t v = row[x];
    out[0] = v;
    out[1] = v;
    out[2] = v;
    out[3] = 255u;
    return;
  }
  const uint8_t * px = row + ((size_t)x * 4u);
  out[0] = px[0];
  out[1] = px[1];
  out[2] = px[2];
  out[3] = px[3];
}

/** True when every pixel's alpha is 255. */
static bool bmp_raster_is_opaque(const GIMG_Raster * raster) {
  const GIMG_Pixel_Format * f = gimg_raster_format(raster);
  if (f->channel_model != GIMG_CHANNEL_RGBA) {
    return true;
  }
  uint32_t width = gimg_raster_width(raster);
  uint32_t height = gimg_raster_height(raster);
  size_t stride = gimg_raster_stride_bytes(raster);
  const uint8_t * pixels = (const uint8_t *)gimg_raster_pixels_const(raster);

  for (uint32_t y = 0; y < height; y++) {
    const uint8_t * row = pixels + ((size_t)y * stride);
    for (uint32_t x = 0; x < width; x++) {
      if (row[((size_t)x * 4u) + 3u] != 255u) {
        return false;
      }
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------

/** Entries the table can address at each depth this writer will emit. */
#define BMP_PALETTE_MAX 256u

/**
 * Open-addressed map from a packed 0x00RRGGBB to a palette index.
 *
 * Sized well above the 256 entries it can hold so the load factor stays low
 * and a probe almost always lands first try.  A power of two, so the wrap is
 * a mask.
 */
#define BMP_COLOR_SLOTS 1024u

/** @brief The table described above. */
typedef struct {
  uint32_t key[BMP_COLOR_SLOTS];    ///< Packed color, bit 24 set when in use.
  uint16_t index[BMP_COLOR_SLOTS];  ///< Palette index for the key beside it.
  uint32_t colors[BMP_PALETTE_MAX]; ///< Packed colors, in first-seen order.
  uint32_t count;                   ///< Distinct colors seen so far.
} bmp_color_table_t;

/** Bit above the 24 color bits, marking a slot as occupied. */
#define BMP_SLOT_USED UINT32_C(0x01000000)

static uint32_t bmp_color_hash(uint32_t packed) {
  // Knuth's multiplicative hash; the top bits are the well-mixed ones.
  return (packed * UINT32_C(2654435761)) >> 22;
}

/**
 * Note one color, assigning it the next index if it is new.
 *
 * @return false once the image has more distinct colors than a palette can
 *   hold, at which point the caller stops asking.
 */
static bool bmp_color_table_add(bmp_color_table_t * t, uint32_t packed) {
  uint32_t slot = bmp_color_hash(packed) & (BMP_COLOR_SLOTS - 1u);
  for (;;) {
    uint32_t key = t->key[slot];
    if (!(key & BMP_SLOT_USED)) {
      if (t->count >= BMP_PALETTE_MAX) {
        return false;
      }
      t->key[slot] = packed | BMP_SLOT_USED;
      t->index[slot] = (uint16_t)t->count;
      t->colors[t->count++] = packed;
      return true;
    }
    if ((key & 0x00FFFFFFu) == packed) {
      return true;
    }
    slot = (slot + 1u) & (BMP_COLOR_SLOTS - 1u);
  }
}

/** The index a color was given; only ever called for a color already added. */
static unsigned int bmp_color_table_lookup(
    const bmp_color_table_t * t, uint32_t packed) {
  uint32_t slot = bmp_color_hash(packed) & (BMP_COLOR_SLOTS - 1u);
  for (;;) {
    uint32_t key = t->key[slot];
    if (!(key & BMP_SLOT_USED)) {
      return 0; // Unreachable: every color present was added first.
    }
    if ((key & 0x00FFFFFFu) == packed) {
      return t->index[slot];
    }
    slot = (slot + 1u) & (BMP_COLOR_SLOTS - 1u);
  }
}

/**
 * Fill `table` with the image's distinct colors.
 *
 * @return false when there are more than a palette can hold, which is the
 *   only thing worth knowing past that point.
 */
static bool bmp_collect_colors(
    const GIMG_Raster * raster, bmp_color_table_t * table) {
  const GIMG_Pixel_Format * f = gimg_raster_format(raster);
  uint32_t width = gimg_raster_width(raster);
  uint32_t height = gimg_raster_height(raster);
  size_t stride = gimg_raster_stride_bytes(raster);
  const uint8_t * pixels = (const uint8_t *)gimg_raster_pixels_const(raster);

  memset(table, 0, sizeof(*table));
  for (uint32_t y = 0; y < height; y++) {
    const uint8_t * row = pixels + ((size_t)y * stride);
    for (uint32_t x = 0; x < width; x++) {
      uint8_t rgba[4];
      bmp_sample(f, row, x, rgba);
      uint32_t packed = ((uint32_t)rgba[0] << 16) | ((uint32_t)rgba[1] << 8) |
          (uint32_t)rgba[2];
      if (!bmp_color_table_add(table, packed)) {
        return false;
      }
    }
  }
  return true;
}

/** The smallest depth this writer will emit that addresses `count` entries. */
static uint16_t bmp_depth_for(uint32_t count, bool allow_2bit) {
  if (count <= 2u) {
    return 1u;
  }
  // 2 bits per pixel is a Windows CE addition the desktop API does not accept,
  // so it is never chosen unless the caller said they can read it back.
  if (allow_2bit && count <= 4u) {
    return 2u;
  }
  if (count <= 16u) {
    return 4u;
  }
  return 8u;
}

// ---------------------------------------------------------------------------
// Row packing
// ---------------------------------------------------------------------------

/**
 * Write one image row into `out`, which the caller has zeroed to `stride`.
 *
 * Indices are packed most significant bits first, which is what clause 2 of
 * the DIB layout says and what the decoder here reads back.
 */
static void bmp_pack_row(const GIMG_Pixel_Format * format, const uint8_t * src,
    uint32_t width, uint16_t bit_count, const bmp_color_table_t * table,
    unsigned char * out) {
  for (uint32_t x = 0; x < width; x++) {
    uint8_t rgba[4];
    bmp_sample(format, src, x, rgba);

    if (bit_count >= 24u) {
      unsigned char * px = out + ((size_t)x * (size_t)(bit_count / 8u));
      px[0] = rgba[2]; // Blue.
      px[1] = rgba[1]; // Green.
      px[2] = rgba[0]; // Red.
      if (bit_count == 32u) {
        px[3] = rgba[3];
      }
      continue;
    }

    uint32_t packed = ((uint32_t)rgba[0] << 16) | ((uint32_t)rgba[1] << 8) |
        (uint32_t)rgba[2];
    unsigned int index = bmp_color_table_lookup(table, packed);
    size_t bit = (size_t)x * bit_count;
    unsigned int shift = (unsigned int)(8u - bit_count - (bit % 8u));
    out[bit / 8u] |= (unsigned char)((index & ((1u << bit_count) - 1u))
        << shift);
  }
}

// ---------------------------------------------------------------------------
// RLE8
// ---------------------------------------------------------------------------

/**
 * Run-length encode one row of 8-bit indices, appending to `out`.
 *
 * A run of three or more identical indices is worth an encoded pair; anything
 * shorter goes into an absolute run, which costs two bytes of header and so
 * only pays from three literals up.  Absolute runs are padded to a 16-bit
 * boundary, and a run of fewer than three literals is emitted as encoded
 * pairs instead, since counts below three are escapes.
 *
 * @return the number of bytes appended, or SIZE_MAX if `capacity` was short.
 */
static size_t bmp_rle8_row(const unsigned char * indices, uint32_t width,
    unsigned char * out, size_t capacity) {
  size_t used = 0;
  uint32_t x = 0;

#define BMP_RLE_PUT(byte)                                                      \
  do {                                                                         \
    if (used >= capacity) {                                                    \
      return SIZE_MAX;                                                         \
    }                                                                          \
    out[used++] = (unsigned char)(byte);                                       \
  } while (0)

  while (x < width) {
    // How far the run of equal indices starting here reaches, capped at the
    // 255 a count byte can state.
    uint32_t run = 1;
    while (x + run < width && run < 255u && indices[x + run] == indices[x]) {
      run++;
    }
    if (run >= 3u) {
      BMP_RLE_PUT(run);
      BMP_RLE_PUT(indices[x]);
      x += run;
      continue;
    }

    // A stretch with no run worth encoding: gather literals until one starts.
    uint32_t start = x;
    uint32_t literals = 0;
    while (x < width && literals < 255u) {
      uint32_t ahead = 1;
      while (x + ahead < width && ahead < 3u && indices[x + ahead] ==
          indices[x]) {
        ahead++;
      }
      if (ahead >= 3u) {
        break;
      }
      x++;
      literals++;
    }
    if (literals < 3u) {
      // Too few to be an absolute run - 0, 1 and 2 are escapes - so they go
      // out as encoded pairs of one.
      for (uint32_t i = 0; i < literals; i++) {
        BMP_RLE_PUT(1);
        BMP_RLE_PUT(indices[start + i]);
      }
      continue;
    }
    BMP_RLE_PUT(0);
    BMP_RLE_PUT(literals);
    for (uint32_t i = 0; i < literals; i++) {
      BMP_RLE_PUT(indices[start + i]);
    }
    if (literals & 1u) {
      BMP_RLE_PUT(0); // Pad to a 16-bit boundary.
    }
  }

  BMP_RLE_PUT(0); // End of line.
  BMP_RLE_PUT(0);
#undef BMP_RLE_PUT
  return used;
}

/**
 * Run-length encode one row of 4-bit indices, appending to `out`.
 *
 * The shape is RLE8's, with one difference that is the whole of RLE4: an
 * encoded pair's value byte holds *two* nibbles and the run alternates between
 * them, so a run of one repeated index stores that index in both halves, and a
 * run of two alternating indices is expressible as a single pair.  An absolute
 * run packs two indices per byte and pads to a 16-bit boundary.
 *
 * @return the number of bytes appended, or SIZE_MAX if `capacity` was short.
 */
static size_t bmp_rle4_row(const unsigned char * indices, uint32_t width,
    unsigned char * out, size_t capacity) {
  size_t used = 0;
  uint32_t x = 0;

#define BMP_RLE4_PUT(byte)                                                     \
  do {                                                                         \
    if (used >= capacity) {                                                    \
      return SIZE_MAX;                                                         \
    }                                                                          \
    out[used++] = (unsigned char)(byte);                                       \
  } while (0)

  while (x < width) {
    // An encoded run alternates two nibbles, so what repeats is the *pair*:
    // a, b, a, b, ...  A run of one repeated value is the case a == b.
    unsigned char a = indices[x];
    unsigned char b = (x + 1u < width) ? indices[x + 1u] : a;
    uint32_t run = 1;
    while (x + run < width && run < 255u &&
        indices[x + run] == ((run & 1u) ? b : a)) {
      run++;
    }
    if (run >= 4u || (run >= 3u && a == b)) {
      BMP_RLE4_PUT(run);
      BMP_RLE4_PUT((unsigned)(a << 4) | (b & 0x0Fu));
      x += run;
      continue;
    }

    // Gather literals until an encodable run starts.
    uint32_t start = x;
    uint32_t literals = 0;
    while (x < width && literals < 255u) {
      unsigned char ra = indices[x];
      unsigned char rb = (x + 1u < width) ? indices[x + 1u] : ra;
      uint32_t ahead = 1;
      while (x + ahead < width && ahead < 4u &&
          indices[x + ahead] == ((ahead & 1u) ? rb : ra)) {
        ahead++;
      }
      if (ahead >= 4u || (ahead >= 3u && ra == rb)) {
        break;
      }
      x++;
      literals++;
    }
    if (literals < 3u) {
      // Counts below three are escapes, so short stretches go out as encoded
      // pairs of one - which in RLE4 means both nibbles set to the index.
      for (uint32_t i = 0; i < literals; i++) {
        unsigned char v = indices[start + i];
        BMP_RLE4_PUT(1);
        BMP_RLE4_PUT((unsigned)(v << 4) | (v & 0x0Fu));
      }
      continue;
    }
    BMP_RLE4_PUT(0);
    BMP_RLE4_PUT(literals);
    // Two indices to the byte, high nibble first.
    size_t bytes = ((size_t)literals + 1u) / 2u;
    for (size_t i = 0; i < bytes; i++) {
      unsigned char hi = indices[start + (i * 2u)];
      unsigned char lo = ((i * 2u) + 1u < literals)
          ? indices[start + (i * 2u) + 1u]
          : 0u;
      BMP_RLE4_PUT((unsigned)(hi << 4) | (lo & 0x0Fu));
    }
    if (bytes & 1u) {
      BMP_RLE4_PUT(0); // Pad to a 16-bit boundary.
    }
  }

  BMP_RLE4_PUT(0); // End of line.
  BMP_RLE4_PUT(0);
#undef BMP_RLE4_PUT
  return used;
}

/**
 * Run-length encode one row of BGR triples, appending to `out`.
 *
 * OS/2's RLE24 has RLE8's escape structure with a three-byte value: an encoded
 * run is a count and one BGR triple, and an absolute run is a count followed
 * by that many triples, padded to a 16-bit boundary.
 *
 * @return the number of bytes appended, or SIZE_MAX if `capacity` was short.
 */
static size_t bmp_rle24_row(const unsigned char * bgr, uint32_t width,
    unsigned char * out, size_t capacity) {
  size_t used = 0;
  uint32_t x = 0;

#define BMP_RLE24_PUT(byte)                                                    \
  do {                                                                         \
    if (used >= capacity) {                                                    \
      return SIZE_MAX;                                                         \
    }                                                                          \
    out[used++] = (unsigned char)(byte);                                       \
  } while (0)
#define BMP_RLE24_SAME(i, j)                                                   \
  (bgr[(i) * 3u] == bgr[(j) * 3u] &&                                           \
      bgr[((i) * 3u) + 1u] == bgr[((j) * 3u) + 1u] &&                          \
      bgr[((i) * 3u) + 2u] == bgr[((j) * 3u) + 2u])

  while (x < width) {
    uint32_t run = 1;
    while (x + run < width && run < 255u && BMP_RLE24_SAME(x + run, x)) {
      run++;
    }
    // An encoded run costs four bytes and an absolute triple costs three, so a
    // run pays from two upwards.
    if (run >= 2u) {
      BMP_RLE24_PUT(run);
      BMP_RLE24_PUT(bgr[x * 3u]);
      BMP_RLE24_PUT(bgr[(x * 3u) + 1u]);
      BMP_RLE24_PUT(bgr[(x * 3u) + 2u]);
      x += run;
      continue;
    }

    uint32_t start = x;
    uint32_t literals = 0;
    while (x < width && literals < 255u) {
      if (x + 1u < width && BMP_RLE24_SAME(x + 1u, x)) {
        break;
      }
      x++;
      literals++;
    }
    if (literals < 3u) {
      for (uint32_t i = 0; i < literals; i++) {
        BMP_RLE24_PUT(1);
        BMP_RLE24_PUT(bgr[(start + i) * 3u]);
        BMP_RLE24_PUT(bgr[((start + i) * 3u) + 1u]);
        BMP_RLE24_PUT(bgr[((start + i) * 3u) + 2u]);
      }
      continue;
    }
    BMP_RLE24_PUT(0);
    BMP_RLE24_PUT(literals);
    for (uint32_t i = 0; i < literals; i++) {
      BMP_RLE24_PUT(bgr[(start + i) * 3u]);
      BMP_RLE24_PUT(bgr[((start + i) * 3u) + 1u]);
      BMP_RLE24_PUT(bgr[((start + i) * 3u) + 2u]);
    }
    // The run is `literals * 3` bytes; pad when that is odd, which is when
    // the count is.
    if (literals & 1u) {
      BMP_RLE24_PUT(0);
    }
  }

  BMP_RLE24_PUT(0); // End of line.
  BMP_RLE24_PUT(0);
#undef BMP_RLE24_SAME
#undef BMP_RLE24_PUT
  return used;
}

/**
 * Write a BI_JPEG or BI_PNG wrapper: a BMP header whose pixel data is a whole
 * image in another format.
 *
 * The payload is produced by this library's own codec for that format, by
 * saving a copy of the raster.  A copy, because a document takes ownership of
 * the raster it is given and this one belongs to the caller's document.
 *
 * biBitCount is written as zero, which is what a conformant BI_PNG file does:
 * the embedded stream carries its own depth, and stating a second one here
 * would be a claim this writer cannot keep.
 */
static GIMG_Result bmp_save_wrapper(GIMG_Stream * stream,
    const GIMG_Raster * raster, uint8_t wrapper, const GIMG_Allocator * alloc,
    GIMG_Save_Report * report) {
  const char * format_name =
      wrapper == GIMG_BMP_WRAPPER_JPEG ? "jpeg" : "png";

  GIMG_Raster * copy = NULL;
  GIMG_Result r = gimg_raster_copy_with_allocator(alloc, raster, &copy);
  if (r != GIMG_OK) {
    return r;
  }
  GIMG_Doc * inner = NULL;
  r = gimg_doc_create_with_allocator(alloc, &inner);
  if (r == GIMG_OK) {
    r = gimg_doc_set_item_count(inner, 1);
  }
  if (r != GIMG_OK) {
    gimg_raster_destroy(copy);
    if (inner) {
      gimg_doc_destroy(inner);
    }
    return r;
  }
  gimg_item_set_raster(gimg_doc_item(inner, 0), copy); // Takes ownership.

  GIMG_Stream * payload = NULL;
  r = gimg_stream_create_memory_output(&payload);
  if (r != GIMG_OK) {
    gimg_doc_destroy(inner);
    return r;
  }
  GIMG_Save_Report inner_report;
  memset(&inner_report, 0, sizeof(inner_report));
  r = gimg_doc_save(inner, payload, format_name, NULL, &inner_report);
  if (r != GIMG_OK) {
    gimg_stream_destroy(payload);
    gimg_doc_destroy(inner);
    return r;
  }

  const void * bytes = NULL;
  size_t size = 0;
  gimg_stream_output_buffer(payload, &bytes, &size);

  uint32_t width = gimg_raster_width(raster);
  uint32_t height = gimg_raster_height(raster);
  size_t data_offset =
      (size_t)GIMG_BMP_FILE_HEADER_SIZE + (size_t)GIMG_BMP_INFOHEADER_SIZE;
  size_t file_size;
  if (!size || !gcu_safe_add_size(data_offset, size, &file_size) ||
      file_size > UINT32_MAX) {
    gimg_stream_destroy(payload);
    gimg_doc_destroy(inner);
    // bfSize cannot describe it, so the file cannot say how long it is.
    return size ? GIMG_ERR_LIMIT : GIMG_ERR_INTERNAL;
  }

  unsigned char header[GIMG_BMP_FILE_HEADER_SIZE + GIMG_BMP_INFOHEADER_SIZE];
  memset(header, 0, sizeof(header));
  header[0] = 'B';
  header[1] = 'M';
  gimg_bmp_write_u32(header + 2, (uint32_t)file_size);
  gimg_bmp_write_u32(header + 10, (uint32_t)data_offset);
  unsigned char * dib = header + GIMG_BMP_FILE_HEADER_SIZE;
  gimg_bmp_write_u32(dib + 0, GIMG_BMP_INFOHEADER_SIZE);
  gimg_bmp_write_u32(dib + 4, width);
  gimg_bmp_write_u32(dib + 8, height);
  bmp_write_u16(dib + 12, 1u); // Planes.
  bmp_write_u16(dib + 14, 0u); // biBitCount: the payload states its own.
  gimg_bmp_write_u32(dib + 16,
      wrapper == GIMG_BMP_WRAPPER_JPEG ? GIMG_BMP_BI_JPEG : GIMG_BMP_BI_PNG);
  gimg_bmp_write_u32(dib + 20, (uint32_t)size); // biSizeImage.

  r = bmp_write_all(stream, header, sizeof(header));
  if (r == GIMG_OK) {
    r = bmp_write_all(stream, bytes, size);
  }
  if (r == GIMG_OK) {
    report->bytes_written = file_size;
  }
  gimg_stream_destroy(payload);
  gimg_doc_destroy(inner);
  return r;
}

// ---------------------------------------------------------------------------
// Save
// ---------------------------------------------------------------------------

/** Everything the header writer needs, decided before a byte goes out. */
typedef struct {
  uint16_t bit_count;       ///< Bits per pixel to write.
  uint32_t compression;     ///< biCompression, in the Windows vocabulary.
  uint32_t dib_size;        ///< DIB header length: 40, or 56 with masks.
  /** True when dib_size names an OS/2 BITMAPCOREHEADER2 rather than a Windows
   * header.  The two share their first 40 bytes, so only the length and the
   * vocabulary of `compression` differ - and they differ in a way that matters:
   * 4 is RLE24 in one and BI_JPEG in the other. */
  bool os2;
  uint32_t palette_entries; ///< Palette entries to write; 0 for true color.
  bool top_down;            ///< True to write rows top to bottom.
  size_t stride;            ///< Bytes per row of uncompressed pixel data.
  size_t pixel_bytes;       ///< Bytes of pixel data, encoded or not.
  /** Bytes @ref GIMG_BMP_V4_TAIL_AT onwards of a V4 or V5 header; used only
   * when dib_size says one of those is being written. */
  unsigned char color_tail[GIMG_BMP_V5HEADER_SIZE - GIMG_BMP_V4_TAIL_AT];
  const void * profile;     ///< ICC profile to embed, or NULL.
  size_t profile_bytes;     ///< Its length; 0 when there is none.
} bmp_plan_t;

GIMG_Result gimg_bmp_save(GIMG_Codec * codec, const GIMG_Doc * doc,
    GIMG_Stream * stream, const char * format_name,
    const GIMG_Save_Options * options, GIMG_Save_Report * report) {
  (void)format_name;
  if (!codec || !doc || !stream || !report) {
    return GIMG_ERR_INTERNAL;
  }
  report->bytes_written = 0;

  const uint8_t want_palette =
      options ? options->bmp_palette : (uint8_t)GIMG_BMP_PALETTE_AUTO;
  const uint8_t want_rle =
      options ? options->bmp_rle : (uint8_t)GIMG_BMP_RLE_NEVER;
  const bool want_top_down = options && options->bmp_top_down;
  const bool allow_2bit = options && options->bmp_allow_2bit;
  const bool allow_rle24 = options && options->bmp_allow_rle24;
  const bool allow_huffman = options && options->bmp_allow_huffman;

  if (want_rle == GIMG_BMP_RLE_AUTO && want_top_down) {
    // The format does not allow the pair: an RLE stream's end-of-line walks
    // one way only, so a top-down RLE bitmap does not say which way it walks.
    // The loader here refuses one, and so do GdkPixbuf and netpbm.
    return GIMG_ERR_UNSUPPORTED;
  }

  if (gimg_doc_item_count(doc) == 0) {
    return GIMG_ERR_FORMAT;
  }
  GIMG_Item * item = gimg_doc_item((GIMG_Doc *)doc, 0);
  if (!item) {
    return GIMG_ERR_INTERNAL;
  }

  // Prefer a raster already attached to the item; otherwise decode one and
  // take ownership of it for the duration of the save.
  GIMG_Raster * raster = gimg_item_raster(item);
  bool raster_owned = false;
  if (!raster) {
    GIMG_Result dr = gimg_item_decode(item, NULL, &raster);
    if (dr != GIMG_OK || !raster) {
      return dr == GIMG_ERR_UNSUPPORTED || dr == GIMG_OK ? GIMG_ERR_FORMAT : dr;
    }
    raster_owned = true;
  }

  // A deeper raster than this format can hold is restated at 8 bits rather
  // than refused; the conversion carries the color info across with it.
  if (bmp_format_narrowable(gimg_raster_format(raster))) {
    GIMG_Raster * narrowed = NULL;
    GIMG_Result cr = gimg_ops_convert_bit_depth(raster, 8, &narrowed);
    if (cr != GIMG_OK || !narrowed) {
      if (raster_owned) {
        gimg_raster_destroy(raster);
      }
      return cr != GIMG_OK ? cr : GIMG_ERR_UNSUPPORTED;
    }
    if (raster_owned) {
      gimg_raster_destroy(raster);
    }
    raster = narrowed;
    raster_owned = true;
  }

  GIMG_Result result = GIMG_OK;
  const GIMG_Allocator * alloc = gimg_alloc_or_default(codec->allocator);

  // A wrapper's pixel data is a whole other image, so none of the palette,
  // depth or compression work below applies to it.
  if (options && options->bmp_wrapper != GIMG_BMP_WRAPPER_NONE) {
    result = bmp_save_wrapper(
        stream, raster, options->bmp_wrapper, alloc, report);
    if (raster_owned) {
      gimg_raster_destroy(raster);
    }
    return result;
  }

  unsigned char * row_buffer = NULL;
  unsigned char * encoded = NULL;
  bmp_color_table_t * table = NULL;

  const GIMG_Pixel_Format * format = gimg_raster_format(raster);
  if (!bmp_format_supported(format)) {
    result = GIMG_ERR_UNSUPPORTED;
    goto done;
  }

  {
    uint32_t width = gimg_raster_width(raster);
    uint32_t height = gimg_raster_height(raster);
    if (!width || !height) {
      result = GIMG_ERR_FORMAT;
      goto done;
    }

    bmp_plan_t plan;
    memset(&plan, 0, sizeof(plan));
    plan.top_down = want_top_down;

    bool opaque = bmp_raster_is_opaque(raster);
    bool indexed = false;

    if (!opaque) {
      // An alpha channel needs explicit masks; 32-bit BI_RGB does not define
      // the fourth byte and readers disagree on whether to honor it.  A BMP
      // palette has no alpha at all, so this rules the palette out too.
      plan.bit_count = 32u;
      plan.compression = GIMG_BMP_BI_BITFIELDS;
      plan.dib_size = GIMG_BMP_V3HEADER_SIZE;
    }
    else {
      plan.bit_count = 24u;
      plan.compression = GIMG_BMP_BI_RGB;
      plan.dib_size = GIMG_BMP_INFOHEADER_SIZE;

      if (want_palette == GIMG_BMP_PALETTE_AUTO) {
        table =
            (bmp_color_table_t *)gimg_calloc(alloc, 1, sizeof(*table));
        if (!table) {
          result = GIMG_ERR_OOM;
          goto done;
        }
        if (bmp_collect_colors(raster, table) && table->count > 0) {
          uint16_t depth = bmp_depth_for(table->count, allow_2bit);
          size_t plain_stride = 0, indexed_stride = 0;
          if (gimg_bmp_row_stride(width, 24u, &plain_stride) == GIMG_OK &&
              gimg_bmp_row_stride(width, depth, &indexed_stride) == GIMG_OK) {
            // Both forms' sizes follow from the stride, so which is smaller is
            // arithmetic and not a measurement.  The palette costs four bytes
            // an entry and is part of the comparison.
            size_t plain = plain_stride * (size_t)height;
            size_t with_palette = (indexed_stride * (size_t)height) +
                ((size_t)table->count * 4u);
            if (with_palette < plain) {
              indexed = true;
              plan.bit_count = depth;
              plan.palette_entries = table->count;
            }
          }
        }
        if (!indexed) {
          gimg_free(alloc, table);
          table = NULL;
        }
      }
    }

    // What the raster says about its color, and which header version carries
    // it.  A V4 or V5 header is longer than the one the pixels alone would
    // need, so this has to be settled before any offset is computed.  The
    // two policies that drop the resolution below drop this too.
    GIMG_Meta_Policy policy =
        options ? options->metadata_policy : GIMG_META_PRESERVE_ALL;
    if (policy != GIMG_META_DROP_ALL && policy != GIMG_META_KEEP_RAW_ONLY) {
      uint32_t color_dib = gimg_bmp_color_to_header(
          gimg_raster_color_info_const(raster), plan.color_tail);
      if (color_dib > plan.dib_size) {
        plan.dib_size = color_dib;
        // Whether a profile is to be embedded is what the header itself now
        // says, not what the raster carries: the two differ when the profile
        // is past what this codec will embed, and reading the decision back
        // from the tail keeps one answer rather than two that could drift.
        unsigned char * cs = plan.color_tail + GIMG_BMP_V4_CS_TYPE_AT;
        uint32_t cs_type = (uint32_t)cs[0] | ((uint32_t)cs[1] << 8) |
            ((uint32_t)cs[2] << 16) | ((uint32_t)cs[3] << 24);
        if (cs_type == GIMG_BMP_PROFILE_EMBEDDED) {
          const GIMG_Color_Info * ci = gimg_raster_color_info_const(raster);
          plan.profile = ci->icc_bytes;
          plan.profile_bytes = ci->icc_size;
        }
      }
    }

    result = gimg_bmp_row_stride(width, plan.bit_count, &plan.stride);
    if (result != GIMG_OK) {
      goto done;
    }
    if (!gcu_safe_mul_size(plan.stride, (size_t)height, &plan.pixel_bytes)) {
      result = GIMG_ERR_LIMIT;
      goto done;
    }

    // RLE8, when it was asked for, applies and comes out smaller.  It is
    // encoded up front rather than streamed, because biSizeImage and bfSize
    // both have to state its length before any of it is written.
    size_t encoded_size = 0;
    bool use_rle = false;
    const bool rle_indexed = want_rle == GIMG_BMP_RLE_AUTO && indexed &&
        (plan.bit_count == 8u || plan.bit_count == 4u);
    // RLE24 needs a true-color image and an OS/2 header to put it in, so it is
    // only reached when the indexed forms were not taken.
    const bool rle_true_color = want_rle == GIMG_BMP_RLE_AUTO && allow_rle24 &&
        !indexed && plan.bit_count == 24u;
    const bool want_huffman = want_rle == GIMG_BMP_RLE_AUTO && allow_huffman &&
        indexed && plan.bit_count == 1u;
    if (want_huffman) {
      // Huffman 1D, like RLE24, exists only in OS/2's vocabulary.  The rows
      // are packed exactly as they would have been written plainly and then
      // encoded, so the encoder is fed the same bytes the decoder produces.
      unsigned char * packed =
          (unsigned char *)gimg_calloc(alloc, plan.stride, (size_t)height);
      if (!packed) {
        result = GIMG_ERR_OOM;
        goto done;
      }
      size_t src_stride = gimg_raster_stride_bytes(raster);
      const uint8_t * pixels =
          (const uint8_t *)gimg_raster_pixels_const(raster);
      for (uint32_t y = 0; y < height; y++) {
        uint32_t source_row = height - 1u - y;
        bmp_pack_row(format, pixels + ((size_t)source_row * src_stride), width,
            plan.bit_count, table, packed + ((size_t)y * plan.stride));
      }

      // Measure first: the encoder counts bits without writing when given no
      // buffer, so the allocation is exact rather than a worst case.
      size_t needed = 0;
      result = gimg_bmp_huffman_encode(
          packed, width, height, plan.stride, NULL, 0, &needed);
      if (result == GIMG_OK && needed && needed < plan.pixel_bytes) {
        encoded = (unsigned char *)gimg_malloc(alloc, needed);
        if (!encoded) {
          gimg_free(alloc, packed);
          result = GIMG_ERR_OOM;
          goto done;
        }
        result = gimg_bmp_huffman_encode(
            packed, width, height, plan.stride, encoded, needed, &encoded_size);
        if (result == GIMG_OK) {
          use_rle = true;
          plan.os2 = true;
          plan.dib_size = GIMG_BMP_OS2V2_MAX_SIZE;
          plan.compression = GIMG_BMP_OS2_HUFFMAN1D;
          plan.pixel_bytes = encoded_size;
        }
        else {
          gimg_free(alloc, encoded);
          encoded = NULL;
        }
      }
      gimg_free(alloc, packed);
      if (result != GIMG_OK) {
        goto done;
      }
    }
    else if (rle_true_color) {
      // RLE24 exists only in OS/2's vocabulary, so writing it means writing an
      // OS/2 header.  The two headers share their first 40 bytes, so this is
      // a length and a compression number rather than a different writer.
      size_t per_row;
      size_t capacity;
      if (!gcu_safe_mul_size((size_t)width, 4u, &per_row) ||
          !gcu_safe_add_size(per_row, 2u, &per_row) ||
          !gcu_safe_mul_size(per_row, (size_t)height, &capacity) ||
          !gcu_safe_add_size(capacity, 2u, &capacity)) {
        result = GIMG_ERR_LIMIT;
        goto done;
      }
      encoded = (unsigned char *)gimg_malloc(alloc, capacity);
      unsigned char * bgr = (unsigned char *)gimg_malloc(alloc,
          (size_t)width * 3u);
      if (!encoded || !bgr) {
        gimg_free(alloc, bgr);
        result = GIMG_ERR_OOM;
        goto done;
      }

      size_t src_stride = gimg_raster_stride_bytes(raster);
      const uint8_t * pixels =
          (const uint8_t *)gimg_raster_pixels_const(raster);
      encoded_size = 0;
      for (uint32_t y = 0; y < height && encoded_size != SIZE_MAX; y++) {
        uint32_t source_row = height - 1u - y;
        const uint8_t * src = pixels + ((size_t)source_row * src_stride);
        for (uint32_t x = 0; x < width; x++) {
          uint8_t rgba[4];
          bmp_sample(format, src, x, rgba);
          bgr[(x * 3u) + 0u] = rgba[2];
          bgr[(x * 3u) + 1u] = rgba[1];
          bgr[(x * 3u) + 2u] = rgba[0];
        }
        size_t n = bmp_rle24_row(bgr, width, encoded + encoded_size,
            capacity - encoded_size);
        if (n == SIZE_MAX) {
          encoded_size = SIZE_MAX;
          break;
        }
        encoded_size += n;
      }
      gimg_free(alloc, bgr);

      if (encoded_size != SIZE_MAX && encoded_size + 2u <= capacity) {
        encoded[encoded_size++] = 0; // End of bitmap.
        encoded[encoded_size++] = 1;
        if (encoded_size < plan.pixel_bytes) {
          use_rle = true;
          plan.os2 = true;
          plan.dib_size = GIMG_BMP_OS2V2_MAX_SIZE;
          plan.compression = GIMG_BMP_OS2_RLE24;
          plan.pixel_bytes = encoded_size;
        }
      }
      if (!use_rle) {
        gimg_free(alloc, encoded);
        encoded = NULL;
      }
    }
    else if (rle_indexed) {
      // Worst case per row: every pixel its own encoded pair, plus the
      // end-of-line, plus the end-of-bitmap at the end of the image.
      size_t per_row;
      size_t capacity;
      if (!gcu_safe_mul_size((size_t)width, 2u, &per_row) ||
          !gcu_safe_add_size(per_row, 2u, &per_row) ||
          !gcu_safe_mul_size(per_row, (size_t)height, &capacity) ||
          !gcu_safe_add_size(capacity, 2u, &capacity)) {
        result = GIMG_ERR_LIMIT;
        goto done;
      }
      encoded = (unsigned char *)gimg_malloc(alloc, capacity);
      if (!encoded) {
        result = GIMG_ERR_OOM;
        goto done;
      }
      unsigned char * indices =
          (unsigned char *)gimg_malloc(alloc, (size_t)width);
      if (!indices) {
        result = GIMG_ERR_OOM;
        goto done;
      }

      size_t src_stride = gimg_raster_stride_bytes(raster);
      const uint8_t * pixels =
          (const uint8_t *)gimg_raster_pixels_const(raster);
      encoded_size = 0;
      for (uint32_t y = 0; y < height && encoded_size != SIZE_MAX; y++) {
        // Rows go out in the order the file stores them, which is bottom-up.
        uint32_t source_row = height - 1u - y;
        const uint8_t * src = pixels + ((size_t)source_row * src_stride);
        for (uint32_t x = 0; x < width; x++) {
          uint8_t rgba[4];
          bmp_sample(format, src, x, rgba);
          uint32_t packed = ((uint32_t)rgba[0] << 16) |
              ((uint32_t)rgba[1] << 8) | (uint32_t)rgba[2];
          indices[x] = (unsigned char)bmp_color_table_lookup(table, packed);
        }
        size_t n = plan.bit_count == 8u
            ? bmp_rle8_row(indices, width, encoded + encoded_size,
                  capacity - encoded_size)
            : bmp_rle4_row(indices, width, encoded + encoded_size,
                  capacity - encoded_size);
        if (n == SIZE_MAX) {
          encoded_size = SIZE_MAX;
          break;
        }
        encoded_size += n;
      }
      gimg_free(alloc, indices);

      if (encoded_size != SIZE_MAX && encoded_size + 2u <= capacity) {
        encoded[encoded_size++] = 0; // End of bitmap.
        encoded[encoded_size++] = 1;
        if (encoded_size < plan.pixel_bytes) {
          use_rle = true;
          plan.compression = plan.bit_count == 8u ? GIMG_BMP_BI_RLE8
                                                  : GIMG_BMP_BI_RLE4;
          plan.pixel_bytes = encoded_size;
        }
      }
      if (!use_rle) {
        gimg_free(alloc, encoded);
        encoded = NULL;
      }
    }

    size_t data_offset = (size_t)GIMG_BMP_FILE_HEADER_SIZE +
        (size_t)plan.dib_size + ((size_t)plan.palette_entries * 4u);
    size_t file_size;
    if (!gcu_safe_add_size(data_offset, plan.pixel_bytes, &file_size) ||
        !gcu_safe_add_size(file_size, plan.profile_bytes, &file_size) ||
        file_size > UINT32_MAX) {
      result = GIMG_ERR_LIMIT;
      goto done;
    }
    if (plan.profile_bytes) {
      // bV5ProfileData is measured from the start of the DIB header, and the
      // profile goes after the pixels: putting it before them would make
      // bfOffBits depend on it, and every reader that ignores the profile
      // still has to find the pixels.
      gimg_bmp_write_u32(
          plan.color_tail + GIMG_BMP_V5_PROFILE_AT,
          (uint32_t)(file_size - plan.profile_bytes - GIMG_BMP_FILE_HEADER_SIZE));
      gimg_bmp_write_u32(plan.color_tail + GIMG_BMP_V5_PROFILE_AT + 4,
          (uint32_t)plan.profile_bytes);
    }

    // File header.
    unsigned char file_header[GIMG_BMP_FILE_HEADER_SIZE];
    memset(file_header, 0, sizeof(file_header));
    file_header[0] = gimg_bmp_signature[0];
    file_header[1] = gimg_bmp_signature[1];
    gimg_bmp_write_u32(file_header + 2, (uint32_t)file_size);
    gimg_bmp_write_u32(file_header + 10, (uint32_t)data_offset);
    result = bmp_write_all(stream, file_header, sizeof(file_header));
    if (result != GIMG_OK) {
      goto done;
    }

    // DIB header.
    unsigned char dib[GIMG_BMP_V5HEADER_SIZE];
    memset(dib, 0, sizeof(dib));
    gimg_bmp_write_u32(dib + 0, plan.dib_size);
    gimg_bmp_write_u32(dib + 4, width);
    // A negative height means the rows run top to bottom.
    gimg_bmp_write_u32(dib + 8,
        plan.top_down ? (uint32_t)(-(int64_t)height) : height);
    bmp_write_u16(dib + 12, 1u); // Planes.
    bmp_write_u16(dib + 14, plan.bit_count);
    gimg_bmp_write_u32(dib + 16, plan.compression);
    gimg_bmp_write_u32(dib + 20, (uint32_t)plan.pixel_bytes);

    // biXPelsPerMeter / biYPelsPerMeter.  Zero means the file does not state
    // a resolution, which is both legal and what most writers emit; inventing
    // one would be a claim about the picture that nothing in it supports.
    uint32_t x_ppm = 0, y_ppm = 0;
    if (policy != GIMG_META_DROP_ALL && policy != GIMG_META_KEEP_RAW_ONLY) {
      const GIMG_Meta_Common * meta_common = gimg_doc_meta_common(doc);
      if (meta_common) {
        uint32_t x_dpi = 0, y_dpi = 0;
        gimg_meta_common_dpi(meta_common, &x_dpi, &y_dpi);
        if (x_dpi > 0 && y_dpi > 0) {
          x_ppm = gimg_dpi_to_pixels_per_meter(x_dpi);
          y_ppm = gimg_dpi_to_pixels_per_meter(y_dpi);
        }
      }
    }
    gimg_bmp_write_u32(dib + 24, x_ppm);
    gimg_bmp_write_u32(dib + 28, y_ppm);
    gimg_bmp_write_u32(dib + 32, plan.palette_entries);
    gimg_bmp_write_u32(dib + 36, plan.palette_entries);

    if (plan.bit_count == 32u) {
      gimg_bmp_write_u32(dib + 40, 0x00FF0000u); // Red.
      gimg_bmp_write_u32(dib + 44, 0x0000FF00u); // Green.
      gimg_bmp_write_u32(dib + 48, 0x000000FFu); // Blue.
      gimg_bmp_write_u32(dib + 52, 0xFF000000u); // Alpha.
    }
    if (plan.dib_size >= GIMG_BMP_V4HEADER_SIZE) {
      memcpy(dib + GIMG_BMP_V4_TAIL_AT, plan.color_tail,
          plan.dib_size - GIMG_BMP_V4_TAIL_AT);
    }
    result = bmp_write_all(stream, dib, plan.dib_size);
    if (result != GIMG_OK) {
      goto done;
    }

    // Palette, blue-green-red-reserved per entry.
    if (plan.palette_entries) {
      for (uint32_t i = 0; i < plan.palette_entries; i++) {
        uint32_t packed = table->colors[i];
        unsigned char entry[4];
        entry[0] = (unsigned char)(packed & 0xFFu);         // Blue.
        entry[1] = (unsigned char)((packed >> 8) & 0xFFu);  // Green.
        entry[2] = (unsigned char)((packed >> 16) & 0xFFu); // Red.
        entry[3] = 0;
        result = bmp_write_all(stream, entry, sizeof(entry));
        if (result != GIMG_OK) {
          goto done;
        }
      }
    }

    if (use_rle) {
      result = bmp_write_all(stream, encoded, plan.pixel_bytes);
      if (result != GIMG_OK) {
        goto done;
      }
    }
    else {
      row_buffer = (unsigned char *)gimg_calloc(alloc, 1, plan.stride);
      if (!row_buffer) {
        result = GIMG_ERR_OOM;
        goto done;
      }

      size_t src_stride = gimg_raster_stride_bytes(raster);
      const uint8_t * pixels =
          (const uint8_t *)gimg_raster_pixels_const(raster);

      for (uint32_t y = 0; y < height; y++) {
        uint32_t source_row = plan.top_down ? y : (height - 1u - y);
        const uint8_t * src = pixels + ((size_t)source_row * src_stride);
        memset(row_buffer, 0, plan.stride);
        bmp_pack_row(format, src, width, plan.bit_count, table, row_buffer);
        result = bmp_write_all(stream, row_buffer, plan.stride);
        if (result != GIMG_OK) {
          goto done;
        }
      }
    }

    if (plan.profile_bytes) {
      result =
          bmp_write_all(stream, plan.profile, plan.profile_bytes);
      if (result != GIMG_OK) {
        goto done;
      }
    }

    report->bytes_written = file_size;
  }

done:
  gimg_free(alloc, row_buffer);
  gimg_free(alloc, encoded);
  gimg_free(alloc, table);
  if (raster_owned) {
    gimg_raster_destroy(raster);
  }
  return result;
}

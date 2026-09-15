/**
 * @file
 *
 * BMP document load: file header, DIB header (core/info/V2..V5), palette, and
 * the raw pixel bytes.
 *
 * Copyright 2026 by Corey Pennycuff
 *
 * --- Internal algorithms and design ---
 *
 * Header versions: all DIB variants share the BITMAPINFOHEADER prefix except
 * BITMAPCOREHEADER, whose width and height are 16-bit.  We read whichever
 * header the size field announces, normalize it into gimg_bmp_header_t, and
 * skip any trailing bytes of the versions we do not interpret (colorspace
 * endpoints and gamma in V4/V5).  Unknown sizes are rejected rather than
 * guessed at, because misreading the header shifts everything after it.
 *
 * Channel masks: BI_BITFIELDS supplies explicit red/green/blue masks, and
 * BI_ALPHABITFIELDS (or a V3+ header) adds an alpha mask.  For BI_RGB we
 * install the implied masks instead, which is where a common BMP bug lives:
 * 16-bit BI_RGB is 5-5-5, *not* 5-6-5.  Writers that want 5-6-5 must declare
 * BI_BITFIELDS.  Masks are required to be non-empty and contiguous.
 *
 * Bounds: every palette index is validated against the number of entries the
 * file actually carries, including on the RLE paths.  The pixel buffer is
 * sized from the stride and height computed here rather than from
 * biSizeImage, which is attacker-controlled and often zero.
 *
 * Limits: GIMG_Load_Options.limits caps the pixel count (max_decoded_pixels)
 * and the pixel data allocation (max_memory) before either is committed.
 */

#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/stream.h>
#include <string.h>

#include "../../container/doc_internal.h"
#include "../../core/alloc_internal.h"
#include "../../core/safe_math_internal.h"
#include "../codec_internal.h"
#include "bmp_internal.h"

/** Append a load diagnostic tagged with the codec name and stream offset. */
static void bmp_load_diag(GIMG_Diagnostics * d, size_t offset,
    const char * action) {
  if (!d) {
    return;
  }
  (void)gimg_diagnostics_append(
      d, "bmp", offset, 0u, GIMG_DIAG_ERROR, action);
}

// ---------------------------------------------------------------------------
// Little-endian field readers
// ---------------------------------------------------------------------------

static uint16_t bmp_read_u16(const unsigned char * p) {
  return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

static uint32_t bmp_read_u32(const unsigned char * p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
      ((uint32_t)p[3] << 24);
}

static int32_t bmp_read_i32(const unsigned char * p) {
  uint32_t raw = bmp_read_u32(p);
  // Convert without relying on implementation-defined conversion of an
  // out-of-range unsigned value.
  return raw <= 0x7FFFFFFFu ? (int32_t)raw
                            : -(int32_t)(0xFFFFFFFFu - raw) - 1;
}

// ---------------------------------------------------------------------------
// Channel masks
// ---------------------------------------------------------------------------

/**
 * Decompose a channel mask into a shift and a maximum value.
 *
 * A mask must be a single contiguous run of set bits; anything else is a file
 * we cannot interpret unambiguously.  An all-zero mask is accepted only for
 * alpha, where it means "no alpha channel", and the caller checks that.
 */
static bool bmp_mask_decompose(
    uint32_t mask, gimg_bmp_channel_mask_t * out) {
  out->mask = mask;
  out->shift = 0;
  out->max = 0;
  if (!mask) {
    return true;
  }

  unsigned int shift = 0;
  while (((mask >> shift) & 1u) == 0u) {
    shift++;
  }
  uint32_t shifted = mask >> shift;
  // Contiguity: shifted must be 2^n - 1.
  if ((shifted & (shifted + 1u)) != 0u) {
    return false;
  }
  out->shift = shift;
  out->max = (unsigned int)shifted;
  return true;
}

/** Install the masks implied by an uncompressed BI_RGB image. */
static bool bmp_default_masks(gimg_bmp_header_t * h) {
  switch (h->bit_count) {
    case 16:
      // BI_RGB at 16bpp is 5-5-5 with the top bit unused.  5-6-5 files must
      // declare BI_BITFIELDS; treating every 16bpp file as 5-6-5 shifts the
      // green and red channels and is a frequent source of wrong colors.
      return bmp_mask_decompose(0x00007C00u, &h->red) &&
          bmp_mask_decompose(0x000003E0u, &h->green) &&
          bmp_mask_decompose(0x0000001Fu, &h->blue) &&
          bmp_mask_decompose(0u, &h->alpha);
    case 24:
      return bmp_mask_decompose(0x00FF0000u, &h->red) &&
          bmp_mask_decompose(0x0000FF00u, &h->green) &&
          bmp_mask_decompose(0x000000FFu, &h->blue) &&
          bmp_mask_decompose(0u, &h->alpha);
    case 32:
      // The high byte is officially unused in BI_RGB.  It is recorded as a
      // candidate alpha mask; decode decides whether to honor it based on
      // whether any pixel actually sets it.
      return bmp_mask_decompose(0x00FF0000u, &h->red) &&
          bmp_mask_decompose(0x0000FF00u, &h->green) &&
          bmp_mask_decompose(0x000000FFu, &h->blue) &&
          bmp_mask_decompose(0xFF000000u, &h->alpha);
    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
// Row stride
// ---------------------------------------------------------------------------

GIMG_Result gimg_bmp_row_stride(
    uint32_t width, uint16_t bit_count, size_t * out_stride) {
  if (!out_stride) {
    return GIMG_ERR_INTERNAL;
  }
  // (width * bpp + 31) / 32 * 4, in size_t so wide images cannot wrap.
  size_t bits;
  if (!gcu_safe_mul_size((size_t)width, (size_t)bit_count, &bits)) {
    return GIMG_ERR_LIMIT;
  }
  size_t padded;
  if (!gcu_safe_add_size(bits, 31u, &padded)) {
    return GIMG_ERR_LIMIT;
  }
  size_t stride;
  if (!gcu_safe_mul_size(padded / 32u, 4u, &stride)) {
    return GIMG_ERR_LIMIT;
  }
  *out_stride = stride;
  return GIMG_OK;
}

// ---------------------------------------------------------------------------
// DIB header
// ---------------------------------------------------------------------------

/** True for the DIB header sizes this codec knows how to interpret. */
static bool bmp_header_size_known(uint32_t size) {
  return size == GIMG_BMP_COREHEADER_SIZE || size == GIMG_BMP_INFOHEADER_SIZE ||
      size == GIMG_BMP_V2HEADER_SIZE || size == GIMG_BMP_V3HEADER_SIZE ||
      size == GIMG_BMP_V4HEADER_SIZE || size == GIMG_BMP_V5HEADER_SIZE;
}

/** True for bit depths that have a defined pixel layout. */
static bool bmp_bit_count_known(uint16_t bit_count) {
  return bit_count == 1 || bit_count == 2 || bit_count == 4 ||
      bit_count == 8 || bit_count == 16 || bit_count == 24 || bit_count == 32;
}

/** Number of palette entries implied by a bit depth when biClrUsed is 0. */
static uint32_t bmp_default_palette_count(uint16_t bit_count) {
  return bit_count <= 8 ? (1u << bit_count) : 0u;
}

/**
 * Read the DIB header and normalize it.
 *
 * On return the stream sits immediately after the DIB header, i.e. at the
 * start of the palette when there is one.
 */
static GIMG_Result bmp_read_dib_header(GIMG_Stream * stream,
    GIMG_Diagnostics * diagnostics, gimg_bmp_header_t * out) {
  unsigned char size_bytes[4];
  GIMG_Result r = gimg_stream_read_exact(stream, size_bytes, 4);
  if (r != GIMG_OK) {
    bmp_load_diag(diagnostics, GIMG_BMP_FILE_HEADER_SIZE,
        "truncated before DIB header size");
    return r;
  }

  uint32_t header_size = bmp_read_u32(size_bytes);
  if (!bmp_header_size_known(header_size)) {
    bmp_load_diag(diagnostics, GIMG_BMP_FILE_HEADER_SIZE,
        "unrecognized DIB header size");
    return GIMG_ERR_UNSUPPORTED;
  }

  memset(out, 0, sizeof(*out));
  out->header_size = header_size;

  // Everything after the 4-byte size field.
  unsigned char body[GIMG_BMP_V5HEADER_SIZE];
  size_t body_size = (size_t)header_size - 4u;
  r = gimg_stream_read_exact(stream, body, body_size);
  if (r != GIMG_OK) {
    bmp_load_diag(
        diagnostics, GIMG_BMP_FILE_HEADER_SIZE, "truncated DIB header");
    return r;
  }

  uint32_t clr_used = 0;
  int64_t signed_height = 0;

  if (header_size == GIMG_BMP_COREHEADER_SIZE) {
    // BITMAPCOREHEADER: 16-bit dimensions, always bottom-up, never compressed.
    out->width = bmp_read_u16(body + 0);
    signed_height = (int64_t)bmp_read_u16(body + 2);
    out->bit_count = bmp_read_u16(body + 6);
    out->compression = GIMG_BMP_BI_RGB;
  }
  else {
    out->width = bmp_read_u32(body + 0);
    signed_height = (int64_t)bmp_read_i32(body + 4);
    out->bit_count = bmp_read_u16(body + 10);
    out->compression = bmp_read_u32(body + 12);
    clr_used = bmp_read_u32(body + 28);
  }

  if (signed_height < 0) {
    out->top_down = true;
    signed_height = -signed_height;
  }
  if (signed_height > (int64_t)UINT32_MAX) {
    bmp_load_diag(diagnostics, GIMG_BMP_FILE_HEADER_SIZE, "height out of range");
    return GIMG_ERR_CORRUPT;
  }
  out->height = (uint32_t)signed_height;

  if (out->width == 0 || out->height == 0) {
    bmp_load_diag(
        diagnostics, GIMG_BMP_FILE_HEADER_SIZE, "zero width or height");
    return GIMG_ERR_CORRUPT;
  }
  // A width above INT32_MAX came from a negative biWidth, which is not legal.
  if (out->width > (uint32_t)INT32_MAX) {
    bmp_load_diag(diagnostics, GIMG_BMP_FILE_HEADER_SIZE, "negative width");
    return GIMG_ERR_CORRUPT;
  }
  if (!bmp_bit_count_known(out->bit_count)) {
    bmp_load_diag(
        diagnostics, GIMG_BMP_FILE_HEADER_SIZE, "unsupported bit depth");
    return GIMG_ERR_UNSUPPORTED;
  }

  // Compression must agree with the bit depth.
  switch (out->compression) {
    case GIMG_BMP_BI_RGB:
      break;
    case GIMG_BMP_BI_RLE8:
      if (out->bit_count != 8) {
        bmp_load_diag(diagnostics, GIMG_BMP_FILE_HEADER_SIZE,
            "RLE8 requires 8 bits per pixel");
        return GIMG_ERR_CORRUPT;
      }
      break;
    case GIMG_BMP_BI_RLE4:
      if (out->bit_count != 4) {
        bmp_load_diag(diagnostics, GIMG_BMP_FILE_HEADER_SIZE,
            "RLE4 requires 4 bits per pixel");
        return GIMG_ERR_CORRUPT;
      }
      break;
    case GIMG_BMP_BI_BITFIELDS:
    case GIMG_BMP_BI_ALPHABITFIELDS:
      if (out->bit_count != 16 && out->bit_count != 32) {
        bmp_load_diag(diagnostics, GIMG_BMP_FILE_HEADER_SIZE,
            "bitfields require 16 or 32 bits per pixel");
        return GIMG_ERR_CORRUPT;
      }
      break;
    default:
      bmp_load_diag(diagnostics, GIMG_BMP_FILE_HEADER_SIZE,
          "unsupported compression method");
      return GIMG_ERR_UNSUPPORTED;
  }

  // Channel masks.  V2 and later carry them inline; a plain BITMAPINFOHEADER
  // with BI_BITFIELDS stores them in the three 32-bit words that follow the
  // header, which is where the palette would otherwise begin.
  bool masks_from_header = header_size >= GIMG_BMP_V2HEADER_SIZE;
  bool wants_masks = out->compression == GIMG_BMP_BI_BITFIELDS ||
      out->compression == GIMG_BMP_BI_ALPHABITFIELDS;

  if (wants_masks) {
    uint32_t r_mask, g_mask, b_mask, a_mask = 0;
    if (masks_from_header) {
      r_mask = bmp_read_u32(body + 36);
      g_mask = bmp_read_u32(body + 40);
      b_mask = bmp_read_u32(body + 44);
      if (header_size >= GIMG_BMP_V3HEADER_SIZE) {
        a_mask = bmp_read_u32(body + 48);
      }
    }
    else {
      unsigned char mask_bytes[16];
      size_t mask_size =
          out->compression == GIMG_BMP_BI_ALPHABITFIELDS ? 16u : 12u;
      r = gimg_stream_read_exact(stream, mask_bytes, mask_size);
      if (r != GIMG_OK) {
        bmp_load_diag(diagnostics, (size_t)header_size,
            "truncated before bitfield masks");
        return r;
      }
      r_mask = bmp_read_u32(mask_bytes + 0);
      g_mask = bmp_read_u32(mask_bytes + 4);
      b_mask = bmp_read_u32(mask_bytes + 8);
      if (mask_size == 16u) {
        a_mask = bmp_read_u32(mask_bytes + 12);
      }
    }

    if (!r_mask || !g_mask || !b_mask) {
      bmp_load_diag(
          diagnostics, (size_t)header_size, "empty color channel mask");
      return GIMG_ERR_CORRUPT;
    }
    if (!bmp_mask_decompose(r_mask, &out->red) ||
        !bmp_mask_decompose(g_mask, &out->green) ||
        !bmp_mask_decompose(b_mask, &out->blue) ||
        !bmp_mask_decompose(a_mask, &out->alpha)) {
      bmp_load_diag(
          diagnostics, (size_t)header_size, "non-contiguous channel mask");
      return GIMG_ERR_CORRUPT;
    }
    out->has_masks = true;
  }
  else if (out->bit_count >= 16) {
    if (!bmp_default_masks(out)) {
      return GIMG_ERR_INTERNAL;
    }
    out->has_masks = true;
  }

  // Palette entry count.  biClrUsed of 0 means "the maximum for this depth".
  if (out->bit_count <= 8) {
    uint32_t maximum = bmp_default_palette_count(out->bit_count);
    out->palette_count = clr_used ? clr_used : maximum;
    if (out->palette_count > maximum) {
      // More entries than the indices can address.  Clamp rather than reject:
      // such files are common and the extra entries are simply unreachable.
      out->palette_count = maximum;
    }
  }

  return GIMG_OK;
}

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------

/**
 * Read the palette that follows the DIB header.
 *
 * Entries are 4 bytes in BITMAPINFOHEADER and later, 3 bytes in
 * BITMAPCOREHEADER, and are stored blue-green-red in both.
 */
static GIMG_Result bmp_read_palette(GIMG_Stream * stream,
    GIMG_Diagnostics * diagnostics, const gimg_bmp_header_t * header,
    const GIMG_Allocator * alloc, gimg_bmp_palette_entry_t ** out_palette) {
  *out_palette = NULL;
  if (!header->palette_count) {
    return GIMG_OK;
  }

  size_t entry_size =
      header->header_size == GIMG_BMP_COREHEADER_SIZE ? 3u : 4u;

  gimg_bmp_palette_entry_t * palette =
      (gimg_bmp_palette_entry_t *)gimg_calloc(alloc, header->palette_count,
          sizeof(gimg_bmp_palette_entry_t));
  if (!palette) {
    return GIMG_ERR_OOM;
  }

  for (uint32_t i = 0; i < header->palette_count; i++) {
    unsigned char entry[4];
    GIMG_Result r = gimg_stream_read_exact(stream, entry, entry_size);
    if (r != GIMG_OK) {
      gimg_free(alloc, palette);
      bmp_load_diag(diagnostics, gimg_stream_tell(stream), "truncated palette");
      return r;
    }
    palette[i].b = entry[0];
    palette[i].g = entry[1];
    palette[i].r = entry[2];
  }

  *out_palette = palette;
  return GIMG_OK;
}

// ---------------------------------------------------------------------------
// Pixel data
// ---------------------------------------------------------------------------

/**
 * Read the pixel data starting at `data_offset`.
 *
 * For uncompressed images the exact size is known from the stride and height,
 * and a short file is an error.  For the RLE formats the encoded length is not
 * predictable, so everything from the offset to the end of the stream is
 * taken and the decoder stops at the end-of-bitmap marker.
 */
static GIMG_Result bmp_read_pixels(GIMG_Stream * stream,
    GIMG_Diagnostics * diagnostics, const gimg_bmp_header_t * header,
    uint32_t data_offset, const GIMG_Limits * limits,
    const GIMG_Allocator * alloc, unsigned char ** out_pixels,
    size_t * out_size) {
  *out_pixels = NULL;
  *out_size = 0;

  size_t stream_size = gimg_stream_size(stream);
  if (data_offset < GIMG_BMP_FILE_HEADER_SIZE ||
      (stream_size && (size_t)data_offset > stream_size)) {
    bmp_load_diag(diagnostics, (size_t)data_offset,
        "pixel data offset outside the file");
    return GIMG_ERR_CORRUPT;
  }

  bool is_rle = header->compression == GIMG_BMP_BI_RLE8 ||
      header->compression == GIMG_BMP_BI_RLE4;

  size_t needed;
  if (is_rle) {
    if (!stream_size) {
      // A non-seekable or unsized stream gives us nothing to bound the RLE
      // data with.
      bmp_load_diag(diagnostics, (size_t)data_offset,
          "RLE data requires a sized stream");
      return GIMG_ERR_UNSUPPORTED;
    }
    needed = stream_size - (size_t)data_offset;
  }
  else {
    size_t stride;
    GIMG_Result r =
        gimg_bmp_row_stride(header->width, header->bit_count, &stride);
    if (r != GIMG_OK) {
      return r;
    }
    if (!gcu_safe_mul_size(stride, (size_t)header->height, &needed)) {
      bmp_load_diag(
          diagnostics, (size_t)data_offset, "pixel data size overflows");
      return GIMG_ERR_LIMIT;
    }
  }

  if (limits && limits->max_memory && needed > limits->max_memory) {
    bmp_load_diag(diagnostics, (size_t)data_offset, "increase max_memory");
    return GIMG_ERR_LIMIT;
  }
  if (!needed) {
    bmp_load_diag(diagnostics, (size_t)data_offset, "no pixel data");
    return GIMG_ERR_CORRUPT;
  }

  GIMG_Result r = gimg_stream_seek(stream, (size_t)data_offset);
  if (r != GIMG_OK) {
    bmp_load_diag(
        diagnostics, (size_t)data_offset, "cannot seek to pixel data");
    return r;
  }

  unsigned char * pixels = (unsigned char *)gimg_malloc(alloc, needed);
  if (!pixels) {
    return GIMG_ERR_OOM;
  }

  r = gimg_stream_read_exact(stream, pixels, needed);
  if (r != GIMG_OK) {
    gimg_free(alloc, pixels);
    bmp_load_diag(diagnostics, (size_t)data_offset, "truncated pixel data");
    return r;
  }

  *out_pixels = pixels;
  *out_size = needed;
  return GIMG_OK;
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

void gimg_bmp_free_doc_state(GIMG_Codec * codec, void * codec_private) {
  (void)codec;
  gimg_bmp_doc_state_t * state = (gimg_bmp_doc_state_t *)codec_private;
  if (!state) {
    return;
  }
  const GIMG_Allocator * alloc = gimg_alloc_or_default(state->allocator);
  gimg_free(alloc, state->palette);
  gimg_free(alloc, state->pixels);
  gimg_free(alloc, state);
}

GIMG_Result gimg_bmp_load(GIMG_Codec * codec, GIMG_Stream * stream,
    const GIMG_Load_Options * options, GIMG_Diagnostics * diagnostics,
    GIMG_Doc ** out_doc) {
  if (!codec || !stream || !out_doc) {
    return GIMG_ERR_INTERNAL;
  }
  *out_doc = NULL;

  GIMG_Result r = gimg_bmp_verify_signature(stream);
  if (r != GIMG_OK) {
    return r;
  }

  const GIMG_Allocator * alloc = gimg_alloc_or_default(codec->allocator);
  const GIMG_Limits * limits = options ? options->limits : NULL;

  // Remainder of the file header: size, two reserved words, data offset.
  unsigned char file_rest[GIMG_BMP_FILE_HEADER_SIZE - GIMG_BMP_SIGNATURE_LEN];
  r = gimg_stream_read_exact(stream, file_rest, sizeof(file_rest));
  if (r != GIMG_OK) {
    bmp_load_diag(diagnostics, GIMG_BMP_SIGNATURE_LEN, "truncated file header");
    return r;
  }
  uint32_t data_offset = bmp_read_u32(file_rest + 8);

  gimg_bmp_header_t header;
  r = bmp_read_dib_header(stream, diagnostics, &header);
  if (r != GIMG_OK) {
    return r;
  }

  // Reject images whose pixel count exceeds the caller's limit before any
  // large allocation happens.
  size_t pixel_count;
  if (!gcu_safe_mul_size(
          (size_t)header.width, (size_t)header.height, &pixel_count)) {
    bmp_load_diag(
        diagnostics, GIMG_BMP_FILE_HEADER_SIZE, "pixel count overflows");
    return GIMG_ERR_LIMIT;
  }
  if (limits && limits->max_decoded_pixels &&
      pixel_count > limits->max_decoded_pixels) {
    bmp_load_diag(diagnostics, GIMG_BMP_FILE_HEADER_SIZE,
        "increase max_decoded_pixels");
    return GIMG_ERR_LIMIT;
  }

  gimg_bmp_palette_entry_t * palette = NULL;
  r = bmp_read_palette(stream, diagnostics, &header, alloc, &palette);
  if (r != GIMG_OK) {
    return r;
  }

  unsigned char * pixels = NULL;
  size_t pixels_size = 0;
  r = bmp_read_pixels(stream, diagnostics, &header, data_offset, limits, alloc,
      &pixels, &pixels_size);
  if (r != GIMG_OK) {
    gimg_free(alloc, palette);
    return r;
  }

  gimg_bmp_doc_state_t * state = (gimg_bmp_doc_state_t *)gimg_calloc(
      alloc, 1, sizeof(gimg_bmp_doc_state_t));
  if (!state) {
    gimg_free(alloc, palette);
    gimg_free(alloc, pixels);
    return GIMG_ERR_OOM;
  }
  state->allocator = alloc;
  state->header = header;
  state->palette = palette;
  state->palette_count = header.palette_count;
  state->pixels = pixels;
  state->pixels_size = pixels_size;

  GIMG_Doc * doc = NULL;
  r = gimg_doc_create_with_allocator(alloc, &doc);
  if (r != GIMG_OK) {
    gimg_bmp_free_doc_state(codec, state);
    return r;
  }
  r = gimg_doc_set_item_count(doc, 1);
  if (r != GIMG_OK) {
    gimg_doc_destroy(doc);
    gimg_bmp_free_doc_state(codec, state);
    return r;
  }

  doc->loaded_by_codec = codec;
  doc->codec_private = state;

  *out_doc = doc;
  return GIMG_OK;
}

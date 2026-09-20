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

#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/color.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/stream.h>
#include <string.h>

#include "../../container/doc_internal.h"
#include "../../core/alloc_internal.h"
#include "../../core/resolution_internal.h"
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

/** True for a DIB header size only Windows uses. */
static bool bmp_header_size_windows(uint32_t size) {
  return size == GIMG_BMP_COREHEADER_SIZE || size == GIMG_BMP_INFOHEADER_SIZE ||
      size == GIMG_BMP_V2HEADER_SIZE || size == GIMG_BMP_V3HEADER_SIZE ||
      size == GIMG_BMP_V4HEADER_SIZE || size == GIMG_BMP_V5HEADER_SIZE;
}

/**
 * True for a header size that can only be a BITMAPCOREHEADER2 (OS/2 2.x).
 *
 * OS/2 2.x allows the header to stop at any multiple of 4 from 16 to 64, with
 * every field it stops short of reading as zero.  Three of those sizes are
 * also Windows headers - 40 is BITMAPINFOHEADER, 52 and 56 are the V2 and V3
 * forms - and nothing in the file says which was meant.  They are read as
 * Windows, because that is what wrote them: bmpsuite's q/rgb32h52.bmp and
 * q/rgba32h56.bmp both carry RGB masks in the bytes where OS/2 would put
 * usRecording, and the masks are what makes them decode.  The rest of the
 * range is unambiguous.
 */
static bool bmp_header_size_os2_v2(uint32_t size) {
  return size >= GIMG_BMP_OS2V2_MIN_SIZE && size <= GIMG_BMP_OS2V2_MAX_SIZE &&
      (size % 4u) == 0u && !bmp_header_size_windows(size);
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
  bool os2_v2 = bmp_header_size_os2_v2(header_size);
  if (!os2_v2 && !bmp_header_size_windows(header_size)) {
    bmp_load_diag(diagnostics, GIMG_BMP_FILE_HEADER_SIZE,
        "unrecognized DIB header size");
    return GIMG_ERR_UNSUPPORTED;
  }

  memset(out, 0, sizeof(*out));
  out->header_size = header_size;
  out->os2_v2 = os2_v2;

  // Everything after the 4-byte size field.
  unsigned char body[GIMG_BMP_V5HEADER_SIZE];
  memset(body, 0, sizeof(body));
  size_t body_size = (size_t)header_size - 4u;
  r = gimg_stream_read_exact(stream, body, body_size);
  if (r != GIMG_OK) {
    bmp_load_diag(
        diagnostics, GIMG_BMP_FILE_HEADER_SIZE, "truncated DIB header");
    return r;
  }

  uint32_t clr_used = 0;
  uint32_t raw_compression = GIMG_BMP_BI_RGB;
  int64_t signed_height = 0;

  if (header_size == GIMG_BMP_COREHEADER_SIZE) {
    // BITMAPCOREHEADER: 16-bit dimensions, always bottom-up, never compressed.
    out->width = bmp_read_u16(body + 0);
    signed_height = (int64_t)bmp_read_u16(body + 2);
    out->bit_count = bmp_read_u16(body + 6);
  }
  else {
    // A BITMAPCOREHEADER2 and a BITMAPINFOHEADER agree byte for byte over
    // their first 40, so the same reads serve both.  What an OS/2 header
    // stops short of is zero, which body was cleared to - and zero is the
    // right answer for every one of these fields: no compression, no stated
    // resolution, and a palette of the depth's full size.
    out->width = bmp_read_u32(body + 0);
    signed_height = (int64_t)bmp_read_i32(body + 4);
    out->bit_count = bmp_read_u16(body + 10);
    raw_compression = bmp_read_u32(body + 12);
    out->size_image = bmp_read_u32(body + 16);
    // biXPelsPerMeter and biYPelsPerMeter.  Stored signed, but a negative
    // resolution is meaningless, so anything with the top bit set is read as
    // "not stated" rather than as an enormous density.
    int32_t x_ppm = bmp_read_i32(body + 20);
    int32_t y_ppm = bmp_read_i32(body + 24);
    out->x_ppm = x_ppm > 0 ? (uint32_t)x_ppm : 0u;
    out->y_ppm = y_ppm > 0 ? (uint32_t)y_ppm : 0u;
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
  // Resolve the compression number against the vocabulary of the header that
  // carried it, then check it against the bit depth.  0, 1 and 2 mean the
  // same thing to both; 3 and 4 do not.
  bool alpha_bitfields = false;
  if (os2_v2) {
    switch (raw_compression) {
      case GIMG_BMP_BI_RGB:
        out->compression = GIMG_BMP_COMP_RGB;
        break;
      case GIMG_BMP_BI_RLE8:
        out->compression = GIMG_BMP_COMP_RLE8;
        break;
      case GIMG_BMP_BI_RLE4:
        out->compression = GIMG_BMP_COMP_RLE4;
        break;
      case GIMG_BMP_OS2_RLE24:
        out->compression = GIMG_BMP_COMP_RLE24;
        break;
      case GIMG_BMP_OS2_HUFFMAN1D:
        bmp_load_diag(diagnostics, GIMG_BMP_FILE_HEADER_SIZE,
            "OS/2 Huffman 1D compression is not implemented");
        return GIMG_ERR_UNSUPPORTED;
      default:
        bmp_load_diag(diagnostics, GIMG_BMP_FILE_HEADER_SIZE,
            "unsupported compression method");
        return GIMG_ERR_UNSUPPORTED;
    }
  }
  else {
    switch (raw_compression) {
      case GIMG_BMP_BI_RGB:
        out->compression = GIMG_BMP_COMP_RGB;
        break;
      case GIMG_BMP_BI_RLE8:
        out->compression = GIMG_BMP_COMP_RLE8;
        break;
      case GIMG_BMP_BI_RLE4:
        out->compression = GIMG_BMP_COMP_RLE4;
        break;
      case GIMG_BMP_BI_BITFIELDS:
        out->compression = GIMG_BMP_COMP_BITFIELDS;
        break;
      case GIMG_BMP_BI_ALPHABITFIELDS:
        out->compression = GIMG_BMP_COMP_BITFIELDS;
        alpha_bitfields = true;
        break;
      case GIMG_BMP_BI_JPEG:
        out->compression = GIMG_BMP_COMP_JPEG;
        break;
      case GIMG_BMP_BI_PNG:
        out->compression = GIMG_BMP_COMP_PNG;
        break;
      default:
        bmp_load_diag(diagnostics, GIMG_BMP_FILE_HEADER_SIZE,
            "unsupported compression method");
        return GIMG_ERR_UNSUPPORTED;
    }
  }

  switch (out->compression) {
    case GIMG_BMP_COMP_RGB:
      break;
    case GIMG_BMP_COMP_RLE8:
      if (out->bit_count != 8) {
        bmp_load_diag(diagnostics, GIMG_BMP_FILE_HEADER_SIZE,
            "RLE8 requires 8 bits per pixel");
        return GIMG_ERR_CORRUPT;
      }
      break;
    case GIMG_BMP_COMP_RLE4:
      if (out->bit_count != 4) {
        bmp_load_diag(diagnostics, GIMG_BMP_FILE_HEADER_SIZE,
            "RLE4 requires 4 bits per pixel");
        return GIMG_ERR_CORRUPT;
      }
      break;
    case GIMG_BMP_COMP_RLE24:
      if (out->bit_count != 24) {
        bmp_load_diag(diagnostics, GIMG_BMP_FILE_HEADER_SIZE,
            "RLE24 requires 24 bits per pixel");
        return GIMG_ERR_CORRUPT;
      }
      break;
    case GIMG_BMP_COMP_BITFIELDS:
      if (out->bit_count != 16 && out->bit_count != 32) {
        bmp_load_diag(diagnostics, GIMG_BMP_FILE_HEADER_SIZE,
            "bitfields require 16 or 32 bits per pixel");
        return GIMG_ERR_CORRUPT;
      }
      break;
    case GIMG_BMP_COMP_JPEG:
    case GIMG_BMP_COMP_PNG:
      // The embedded stream carries its own depth and its own everything
      // else.  biBitCount describes the image the BMP wrapper stands in for,
      // and is not a constraint on what is inside - a conformant BI_PNG file
      // may leave it at zero - so it is not checked for these.
      break;
  }

  if (!gimg_bmp_is_embedded(out->compression) &&
      !bmp_bit_count_known(out->bit_count)) {
    bmp_load_diag(
        diagnostics, GIMG_BMP_FILE_HEADER_SIZE, "unsupported bit depth");
    return GIMG_ERR_UNSUPPORTED;
  }

  // A negative biHeight means top-down rows, and the format does not allow it
  // together with either RLE encoding: an RLE stream is a sequence of row
  // instructions whose "end of line" walks in one direction only, so a
  // top-down RLE bitmap does not say which way it walks.  Windows refuses
  // such a file and so do GdkPixbuf and netpbm.  Decoding one bottom-up, as
  // this codec used to, produced a silently upside-down image.
  if (out->top_down && gimg_bmp_is_rle(out->compression)) {
    bmp_load_diag(diagnostics, GIMG_BMP_FILE_HEADER_SIZE,
        "RLE cannot be combined with top-down rows");
    return GIMG_ERR_CORRUPT;
  }

  // Channel masks.  V2 and later carry them inline; a plain BITMAPINFOHEADER
  // with BI_BITFIELDS stores them in the three 32-bit words that follow the
  // header, which is where the palette would otherwise begin.
  bool masks_from_header = header_size >= GIMG_BMP_V2HEADER_SIZE;
  bool wants_masks = out->compression == GIMG_BMP_COMP_BITFIELDS;

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
      size_t mask_size = alpha_bitfields ? 16u : 12u;
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

  // BITMAPV4HEADER and BITMAPV5HEADER color fields.  They are interpreted in
  // bmp_color.c; here they are only read out of the bytes.  An OS/2 header of
  // the same length holds something else entirely at these offsets, so it is
  // excluded.
  if (!os2_v2 && header_size >= GIMG_BMP_V4HEADER_SIZE) {
    out->cs_type = bmp_read_u32(body + 52);
    for (unsigned int i = 0; i < 9; i++) {
      out->endpoints[i] = bmp_read_i32(body + 56 + (i * 4u));
    }
    out->gamma[0] = bmp_read_u32(body + 92);
    out->gamma[1] = bmp_read_u32(body + 96);
    out->gamma[2] = bmp_read_u32(body + 100);
  }
  if (!os2_v2 && header_size >= GIMG_BMP_V5HEADER_SIZE) {
    out->intent = bmp_read_u32(body + 104);
    out->profile_offset = bmp_read_u32(body + 108);
    out->profile_size = bmp_read_u32(body + 112);
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
 * Load the JPEG or PNG that is the "pixel data" of a BI_JPEG or BI_PNG file.
 *
 * The codec is chosen by name rather than by probing the bytes.  Probing would
 * let a BI_PNG wrapper hold another BMP, which could hold another, and the
 * nesting has no bound this side of the stack: the format says the payload is
 * a JPEG or a PNG, so that is what it is asked to be.  Neither of those can
 * hold a BMP in turn, so the depth is one.
 *
 * The caller's load options go down unchanged, so `GIMG_Limits` applies to
 * what is inside exactly as it would to a file that arrived on its own.
 */
static GIMG_Result bmp_load_embedded(GIMG_Diagnostics * diagnostics,
    const gimg_bmp_header_t * header, const unsigned char * bytes,
    size_t size, const GIMG_Load_Options * options, GIMG_Doc ** out_doc) {
  *out_doc = NULL;
  const char * name =
      header->compression == GIMG_BMP_COMP_JPEG ? "jpeg" : "png";

  GIMG_Codec * inner = gimg_codec_by_name(name);
  if (!inner || !inner->load_cb) {
    bmp_load_diag(diagnostics, 0u, "no codec for the embedded stream");
    return GIMG_ERR_UNSUPPORTED;
  }

  GIMG_Stream * stream = NULL;
  GIMG_Result r = gimg_stream_create_memory(bytes, size, &stream);
  if (r != GIMG_OK) {
    return r;
  }

  GIMG_Doc * doc = NULL;
  r = inner->load_cb(inner, stream, options, diagnostics, &doc);
  gimg_stream_destroy(stream);
  if (r != GIMG_OK) {
    if (doc) {
      gimg_doc_destroy(doc);
    }
    bmp_load_diag(diagnostics, 0u, "the embedded stream did not load");
    return r;
  }
  if (!doc || gimg_doc_item_count(doc) == 0) {
    if (doc) {
      gimg_doc_destroy(doc);
    }
    bmp_load_diag(diagnostics, 0u, "the embedded stream held no image");
    return GIMG_ERR_CORRUPT;
  }

  *out_doc = doc;
  return GIMG_OK;
}

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

  bool variable_length =
      gimg_bmp_is_rle(header->compression) ||
      gimg_bmp_is_embedded(header->compression);

  size_t needed;
  if (variable_length) {
    if (!stream_size) {
      // Neither an RLE stream nor an embedded JPEG or PNG has a length that
      // can be predicted from the header, so both are taken from bfOffBits to
      // the end of the file - which needs a stream that knows where that is.
      bmp_load_diag(diagnostics, (size_t)data_offset,
          "this compression requires a sized stream");
      return GIMG_ERR_UNSUPPORTED;
    }
    needed = stream_size - (size_t)data_offset;
    // biSizeImage states the length when it is believable.  It is
    // attacker-controlled and often zero, so it may only ever shorten what is
    // taken, never extend it past the end of the file.
    if (gimg_bmp_is_embedded(header->compression) && header->size_image &&
        (size_t)header->size_image < needed) {
      needed = (size_t)header->size_image;
    }
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
  if (state->embedded) {
    gimg_doc_destroy(state->embedded);
  }
  gimg_free(alloc, state->icc);
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
  state->rgb32_alpha = options ? options->bmp_rgb32_alpha
                               : (uint8_t)GIMG_BMP_RGB32_ALPHA_IGNORE;

  gimg_bmp_color_from_header(&header, &state->color);
  r = gimg_bmp_read_profile(
      stream, &header, limits, alloc, &state->icc, &state->icc_size);
  if (r != GIMG_OK) {
    gimg_bmp_free_doc_state(codec, state);
    return r;
  }
  if (state->icc) {
    state->color.icc_bytes = state->icc;
    state->color.icc_size = state->icc_size;
  }

  if (gimg_bmp_is_embedded(header.compression)) {
    r = bmp_load_embedded(diagnostics, &header, state->pixels,
        state->pixels_size, options, &state->embedded);
    if (r != GIMG_OK) {
      gimg_bmp_free_doc_state(codec, state);
      return r;
    }
  }

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

  // A BMP states its physical resolution in the header rather than in an
  // optional chunk, so there is nothing to preserve verbatim: the only way to
  // carry it is through the document's common metadata, which is where the
  // PNG and JPEG codecs put theirs.  Without this a resolution was lost the
  // moment an image became a BMP, in both directions.
  //
  // Both axes must be stated.  One alone describes a pixel's shape rather
  // than its size, which is not what dpi means.
  if (header.x_ppm && header.y_ppm) {
    GIMG_Meta_Common * meta_common = NULL;
    if (gimg_doc_ensure_meta_common(doc, &meta_common) == GIMG_OK) {
      gimg_meta_common_set_dpi(meta_common,
          gimg_pixels_per_meter_to_dpi(header.x_ppm),
          gimg_pixels_per_meter_to_dpi(header.y_ppm));
    }
  }

  *out_doc = doc;
  return GIMG_OK;
}

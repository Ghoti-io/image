/**
 * @file
 *
 * PNG save: encode raster to PNG (signature, IHDR, ancillary, IDAT, IEND).
 * Uses DEFLATE via compress library; zlib-wraps for IDAT (2-byte header +
 * raw DEFLATE + 4-byte Adler-32 per RFC 1950).
 *
 * Copyright 2026 by Corey Pennycuff
 *
 * --- Internal algorithms and design ---
 *
 * Chunk order: We emit chunks in PNG spec order (see formats/png.md):
 * signature, IHDR, ancillary (per GIMG_Meta_Policy), PLTE/tRNS if palette,
 * IDAT (one or multiple), IEND. Ancillary is written in the order stored
 * during load (read order) when policy is
 * PRESERVE_ALL/STRIP_GPS/NORMALIZE_EXIF.
 *
 * Filter: All rows use filter type 0 (None). A heuristic (e.g. Sub/Up/Average/
 * Paeth) could be added later to improve compression; raw bytes are passed to
 * DEFLATE as-is.
 *
 * Compression: We use the compress library's "zlib" method (RFC 1950) with
 * strategy "lazy" passed through to the DEFLATE underneath. The raw image
 * buffer (filter byte + row data per row, or Adam7 pass order when
 * interlaced) is compressed in one shot, straight into the container: two
 * header bytes, the DEFLATE data, and a big-endian Adler-32 of the
 * uncompressed bytes.
 *
 * Those six bytes used to be assembled here by hand -- a literal 0x78 0x9C, a
 * local Adler-32, and a second allocation to concatenate them around the
 * DEFLATE output -- because the compress library had raw deflate and gzip and
 * nothing in between. It has the container now, so this no longer keeps a
 * private copy of a format somebody else already implements.
 *
 * IDAT splitting: The zlib payload is written as one or more IDAT chunks with
 * a maximum of 32 KiB per chunk. Some decoders expect smaller IDATs; splitting
 * avoids compatibility issues while keeping chunk count low.
 *
 * Palette round-trip: When the doc was loaded from a palette PNG (state has
 * PLTE/tRNS), we encode as palette if the raster is RGBA8 and every pixel
 * matches a PLTE entry (and tRNS alpha when present). Matching is exact; no
 * quantization. If any pixel has no match we fall back to unsupported (caller
 * would need to requantize or use RGB).
 *
 * Interlace (Adam7): When options->interlaced is set, we fill raw rows in
 * Adam7 pass order (seven passes per W3C §2.6), each row prefixed with filter
 * byte 0, then DEFLATE the entire interlaced buffer.
 */

#include <ctype.h>
#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/color.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/options.h>

#include "../../container/doc_internal.h"
#include "../../core/alloc_internal.h"
#include "../../core/safe_math_internal.h"
#include "../../meta/exif_internal.h"
#include "../../raster/raster_internal.h"
#include "../codec_internal.h"
#include "../../core/resolution_internal.h"
#include "png_internal.h"

/**
 * Map raster format to PNG color_type and bit_depth.
 * Returns 1 on success, 0 on unsupported. Does not handle palette (caller uses
 * doc state for that). When @a state is non-NULL and state->ihdr.color_type is
 * 2 (RGB) or 4 (grayscale+alpha), and the raster is RGBA with matching bit
 * depth, that color_type is used so round-trip preserves format.
 */
/**
 * Would every sample of this grayscale raster survive being written at
 * @a bit_depth and read back?
 *
 * PNG 13.12 rescales a sample of depth d to 8 bits as round(s * 255 / (2^d-1)),
 * and only the values that rescaling can produce come back unchanged when the
 * writer reverses it. A raster whose samples all came from that depth passes;
 * one carrying any other value does not, and is written at 8 bits instead so
 * nothing is quietly rounded away.
 */
static bool gimg_png_gray_fits_depth(
    const GIMG_Raster * raster, uint8_t bit_depth) {
  uint32_t w = gimg_raster_width(raster);
  uint32_t h = gimg_raster_height(raster);
  size_t stride = gimg_raster_stride_bytes(raster);
  const unsigned char * pixels =
      (const unsigned char *)gimg_raster_pixels_const(raster);
  unsigned int max_val = (1u << bit_depth) - 1u;
  for (uint32_t y = 0; y < h; y++) {
    const unsigned char * row = pixels + (size_t)y * stride;
    for (uint32_t x = 0; x < w; x++) {
      unsigned int v = row[x];
      unsigned int packed = (v * max_val + 127u) / 255u;
      if ((packed * 255u + max_val / 2u) / max_val != v) {
        return false;
      }
    }
  }
  return true;
}

/**
 * Can this raster's transparency be expressed by a tRNS chunk beside a
 * color type 2 image?
 *
 * PNG 11.3.2.1: for color type 2, tRNS holds one RGB triple, and pixels of
 * exactly that color are fully transparent while all others are fully opaque.
 * So the alpha channel qualifies only when every pixel is either fully opaque
 * or fully transparent, every fully transparent pixel shares one color, and
 * no opaque pixel wears that same color - otherwise writing the chunk would
 * make opaque pixels vanish.
 *
 * On success @a out_trns receives the six-byte payload (three 16-bit samples,
 * big-endian, as the chunk stores them at either bit depth).
 */
static bool gimg_png_alpha_fits_trns(const GIMG_Raster * raster,
    uint8_t color_type, uint8_t bit_depth, unsigned char * out_trns,
    size_t * out_trns_size) {
  uint32_t w = gimg_raster_width(raster);
  uint32_t h = gimg_raster_height(raster);
  size_t stride = gimg_raster_stride_bytes(raster);
  const unsigned char * pixels =
      (const unsigned char *)gimg_raster_pixels_const(raster);
  unsigned int opaque = (bit_depth == 16) ? 65535u : 255u;
  int have_key = 0;
  uint16_t key[3] = {0, 0, 0};

  // First pass: every pixel must be wholly opaque or wholly transparent, and
  // the transparent ones must agree on a color.
  for (uint32_t y = 0; y < h; y++) {
    const unsigned char * row = pixels + (size_t)y * stride;
    for (uint32_t x = 0; x < w; x++) {
      uint16_t c[4];
      if (bit_depth == 16) {
        const uint16_t * p = (const uint16_t *)(const void *)row + (size_t)x * 4u;
        c[0] = p[0];
        c[1] = p[1];
        c[2] = p[2];
        c[3] = p[3];
      }
      else {
        const unsigned char * p = row + (size_t)x * 4u;
        c[0] = p[0];
        c[1] = p[1];
        c[2] = p[2];
        c[3] = p[3];
      }
      // Color type 0 stores one sample, so the three color channels must
      // agree for every pixel, transparent or not.
      if (color_type == 0 && (c[0] != c[1] || c[1] != c[2])) {
        return false;
      }
      if (c[3] == opaque) {
        continue;
      }
      if (c[3] != 0) {
        return false;  // partial transparency needs a real alpha channel
      }
      if (!have_key) {
        key[0] = c[0];
        key[1] = c[1];
        key[2] = c[2];
        have_key = 1;
      }
      else if (c[0] != key[0] || c[1] != key[1] || c[2] != key[2]) {
        return false;  // more than one transparent color
      }
    }
  }
  if (!have_key) {
    *out_trns_size = 0;  // nothing transparent: color type 2 needs no tRNS
    return true;
  }
  // Second pass: no opaque pixel may share the key color.
  for (uint32_t y = 0; y < h; y++) {
    const unsigned char * row = pixels + (size_t)y * stride;
    for (uint32_t x = 0; x < w; x++) {
      uint16_t c[4];
      if (bit_depth == 16) {
        const uint16_t * p = (const uint16_t *)(const void *)row + (size_t)x * 4u;
        c[0] = p[0];
        c[1] = p[1];
        c[2] = p[2];
        c[3] = p[3];
      }
      else {
        const unsigned char * p = row + (size_t)x * 4u;
        c[0] = p[0];
        c[1] = p[1];
        c[2] = p[2];
        c[3] = p[3];
      }
      if (c[3] == opaque && c[0] == key[0] && c[1] == key[1] &&
          c[2] == key[2]) {
        return false;
      }
    }
  }
  // PNG 11.3.2.1 stores each sample as two bytes whatever the bit depth: one
  // sample for color type 0, three for color type 2.
  int samples = (color_type == 0) ? 1 : 3;
  for (int i = 0; i < samples; i++) {
    out_trns[i * 2] = (unsigned char)(key[i] >> 8);
    out_trns[i * 2 + 1] = (unsigned char)(key[i] & 0xFFu);
  }
  *out_trns_size = (size_t)samples * 2u;
  return true;
}

static bool gimg_png_raster_to_ihdr(const GIMG_Raster * raster,
    const gimg_png_doc_state_t * state, uint8_t * color_type,
    uint8_t * bit_depth, unsigned char * out_trns, size_t * out_trns_size) {
  *out_trns_size = 0;
  const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
  if (!fmt || fmt->layout != GIMG_LAYOUT_INTERLEAVED) {
    return false;
  }
  if (fmt->channel_model == GIMG_CHANNEL_GRAY && fmt->channel_count >= 1) {
    if (fmt->bits_per_channel[0] == 8) {
      // A frame that arrived at 1, 2 or 4 bits goes back out at that depth
      // when every sample still survives the trip (PNG 13.12 rescales on the
      // way in, and the way back is only exact for values that came from that
      // depth). Otherwise 8 bits, which always holds what the raster holds.
      if (state && state->ihdr.color_type == 0 &&
          (state->ihdr.bit_depth == 1 || state->ihdr.bit_depth == 2 ||
              state->ihdr.bit_depth == 4) &&
          gimg_png_gray_fits_depth(raster, state->ihdr.bit_depth)) {
        *color_type = 0;
        *bit_depth = state->ihdr.bit_depth;
        return true;
      }
      *color_type = 0;
      *bit_depth = 8;
      return true;
    }
    if (fmt->bits_per_channel[0] == 16) {
      *color_type = 0;
      *bit_depth = 16;
      return true;
    }
    return false;
  }
  if (fmt->channel_model == GIMG_CHANNEL_RGBA && fmt->channel_count == 4) {
    uint8_t bd = 0;
    if (fmt->bits_per_channel[0] == 8) {
      bd = 8;
    }
    else if (fmt->bits_per_channel[0] == 16) {
      bd = 16;
    }
    else {
      return false;
    }
    // A grayscale frame decodes to RGBA once tRNS gives it an alpha channel.
    // It can go back the way it came when the color channels still agree and
    // the transparency is still one key value (PNG 11.3.2.1).
    if (state && state->ihdr.color_type == 0 && state->ihdr.bit_depth == bd) {
      if (gimg_png_alpha_fits_trns(raster, 0, bd, out_trns, out_trns_size)) {
        *color_type = 0;
        *bit_depth = bd;
        return true;
      }
    }
    // Color type 4 carries alpha of its own, so keeping it loses nothing.
    if (state && state->ihdr.color_type == 4 && state->ihdr.bit_depth == bd) {
      *color_type = 4;
      *bit_depth = bd;
      return true;
    }
    // Color type 2 has no alpha channel. PNG 11.3.2.1 lets a tRNS chunk name
    // one fully transparent color beside it, and nothing more: every other
    // pixel is opaque. Keeping color type 2 for a raster whose alpha does not
    // fit that shape would drop transparency silently, so the alpha decides.
    if (state && state->ihdr.color_type == 2 && state->ihdr.bit_depth == bd) {
      if (gimg_png_alpha_fits_trns(raster, 2, bd, out_trns, out_trns_size)) {
        *color_type = 2;
        *bit_depth = bd;
        return true;
      }
    }
    *color_type = 6;
    *bit_depth = bd;
    return true;
  }
  return false;
}

/**
 * Build a pHYs payload (9 bytes): pixels per meter on each axis, then the unit
 * specifier (11.3.4.3). Always unit 1, because a resolution this library has
 * is a physical one - unit 0 states an aspect ratio and no size at all.
 */
static void gimg_png_build_phys(
    unsigned char * out, uint32_t x_ppm, uint32_t y_ppm) {
  out[0] = (unsigned char)(x_ppm >> 24);
  out[1] = (unsigned char)(x_ppm >> 16);
  out[2] = (unsigned char)(x_ppm >> 8);
  out[3] = (unsigned char)(x_ppm & 0xFFu);
  out[4] = (unsigned char)(y_ppm >> 24);
  out[5] = (unsigned char)(y_ppm >> 16);
  out[6] = (unsigned char)(y_ppm >> 8);
  out[7] = (unsigned char)(y_ppm & 0xFFu);
  out[8] = (unsigned char)GIMG_PNG_PHYS_UNIT_METER;
}

/** Build IHDR payload (13 bytes). @a interlace_method 0 or 1 (Adam7). */
static void gimg_png_build_ihdr(unsigned char * out, uint32_t width,
    uint32_t height, uint8_t bit_depth, uint8_t color_type,
    uint8_t interlace_method) {
  out[0] = (unsigned char)(width >> 24);
  out[1] = (unsigned char)(width >> 16);
  out[2] = (unsigned char)(width >> 8);
  out[3] = (unsigned char)(width & 0xFF);
  out[4] = (unsigned char)(height >> 24);
  out[5] = (unsigned char)(height >> 16);
  out[6] = (unsigned char)(height >> 8);
  out[7] = (unsigned char)(height & 0xFF);
  out[8] = bit_depth;
  out[9] = color_type;
  out[10] = 0; // compression_method
  out[11] = 0; // filter_method
  out[12] = interlace_method;
}

/** Build acTL payload (8 bytes): num_frames, num_plays (big-endian). */
static void gimg_png_build_actl(
    unsigned char * out, uint32_t num_frames, uint32_t num_plays) {
  out[0] = (unsigned char)(num_frames >> 24);
  out[1] = (unsigned char)(num_frames >> 16);
  out[2] = (unsigned char)(num_frames >> 8);
  out[3] = (unsigned char)(num_frames & 0xFFu);
  out[4] = (unsigned char)(num_plays >> 24);
  out[5] = (unsigned char)(num_plays >> 16);
  out[6] = (unsigned char)(num_plays >> 8);
  out[7] = (unsigned char)(num_plays & 0xFFu);
}

/** Build fcTL payload (26 bytes) from frame dimensions and item timing. */
static void gimg_png_build_fctl(unsigned char * out, uint32_t sequence_number,
    uint32_t width, uint32_t height, uint32_t x_offset, uint32_t y_offset,
    uint16_t delay_num, uint16_t delay_den, uint8_t dispose_op,
    uint8_t blend_op) {
  out[0] = (unsigned char)(sequence_number >> 24);
  out[1] = (unsigned char)(sequence_number >> 16);
  out[2] = (unsigned char)(sequence_number >> 8);
  out[3] = (unsigned char)(sequence_number & 0xFFu);
  out[4] = (unsigned char)(width >> 24);
  out[5] = (unsigned char)(width >> 16);
  out[6] = (unsigned char)(width >> 8);
  out[7] = (unsigned char)(width & 0xFFu);
  out[8] = (unsigned char)(height >> 24);
  out[9] = (unsigned char)(height >> 16);
  out[10] = (unsigned char)(height >> 8);
  out[11] = (unsigned char)(height & 0xFFu);
  out[12] = (unsigned char)(x_offset >> 24);
  out[13] = (unsigned char)(x_offset >> 16);
  out[14] = (unsigned char)(x_offset >> 8);
  out[15] = (unsigned char)(x_offset & 0xFFu);
  out[16] = (unsigned char)(y_offset >> 24);
  out[17] = (unsigned char)(y_offset >> 16);
  out[18] = (unsigned char)(y_offset >> 8);
  out[19] = (unsigned char)(y_offset & 0xFFu);
  out[20] = (unsigned char)(delay_num >> 8);
  out[21] = (unsigned char)(delay_num & 0xFFu);
  out[22] = (unsigned char)(delay_den >> 8);
  out[23] = (unsigned char)(delay_den & 0xFFu);
  out[24] = dispose_op;
  out[25] = blend_op;
}

/** Return true if chunk type is known semantic metadata (color, Exif, text). */
//
// Ancillary chunks whose shape depends on the color type
// =======================================================
//
// bKGD, sBIT and hIST are not self-describing: their length and meaning are a
// function of the color type in the IHDR beside them (11.3.4.1, 11.3.2.4,
// 11.3.4.2). The writer does not always emit the color type a frame arrived
// as - a grayscale image whose tRNS cannot be expressed against an alpha
// channel is promoted to truecolor with alpha, for one - and copying these
// three across unchanged then produces a chunk whose length contradicts the
// header in the same file.
//
// That is not a theoretical complaint. Saving the conformance suite's
// tbbn0g04.png - 4-bit grayscale, tRNS, a 2-byte bKGD - correctly wrote
// color type 6 and kept the 2-byte bKGD, which needs 6 bytes there, and
// libpng said so: "libpng warning: bKGD: invalid".
//
// So each is either translated, kept, or dropped. Translation is only done
// where it is exact; where it would be a guess the chunk is dropped, because
// an absent advisory chunk is a smaller lie than a wrong one.
//

/** Payload length bKGD must have for a color type (11.3.4.1). */
static size_t gimg_png_bkgd_len(uint8_t color_type) {
  if (color_type == 3) {
    return 1u; // a palette index
  }
  if (color_type == 0 || color_type == 4) {
    return 2u; // one gray level
  }
  return 6u; // three 16-bit samples, color types 2 and 6
}

/** Payload length sBIT must have for a color type (11.3.2.4). */
static size_t gimg_png_sbit_len(uint8_t color_type) {
  switch (color_type) {
  case 0:
    return 1u; // gray
  case 2:
  case 3:
    return 3u; // R, G, B - for color type 3 these describe the palette
  case 4:
    return 2u; // gray, alpha
  default:
    return 4u; // R, G, B, alpha
  }
}

/**
 * The depth of the samples a chunk's values are expressed in. For color type
 * 3 that is the palette's 8 bits, whatever the bit depth of the indices
 * (11.2.2: PLTE entries are always three 8-bit samples).
 */
static uint8_t gimg_png_sample_depth(uint8_t color_type, uint8_t bit_depth) {
  return (color_type == 3) ? 8u : bit_depth;
}

/**
 * Rescale one sample between depths, by the rule decoding uses in the other
 * direction (PNG 13.12): round(v * (2^to - 1) / (2^from - 1)).
 */
static uint16_t gimg_png_rescale_sample(
    uint32_t v, uint8_t from_depth, uint8_t to_depth) {
  if (from_depth == to_depth) {
    return (uint16_t)v;
  }
  uint32_t from_max = (1u << from_depth) - 1u;
  uint32_t to_max = (1u << to_depth) - 1u;
  if (from_max == 0u) {
    return 0u;
  }
  return (uint16_t)(
      ((uint64_t)v * (uint64_t)to_max + (uint64_t)from_max / 2u) / from_max);
}

/** Read a big-endian 16-bit value. */
static uint32_t gimg_png_be16(const unsigned char * p) {
  return ((uint32_t)p[0] << 8) | (uint32_t)p[1];
}

/** Write a big-endian 16-bit value. */
static void gimg_png_put_be16(unsigned char * p, uint16_t v) {
  p[0] = (unsigned char)(v >> 8);
  p[1] = (unsigned char)(v & 0xFFu);
}

/**
 * bKGD (11.3.4.1) from the color type the frame arrived as to the one being
 * written.
 *
 * The background is a color, so it translates whenever the destination can
 * hold it: gray becomes R=G=B, a palette index becomes the color it names,
 * and a color becomes gray only when its three samples already agree. A
 * change of bit depth rescales by 13.12, the same rule the pixels took.
 */
static gimg_png_retarget_t gimg_png_retarget_bkgd(const unsigned char * payload,
    size_t payload_size, const gimg_png_doc_state_t * state,
    uint8_t out_color_type, uint8_t out_bit_depth, unsigned char * out_buf,
    size_t * out_size) {
  uint8_t src_ct = state->ihdr.color_type;
  uint8_t src_bd = state->ihdr.bit_depth;

  // A chunk that was already the wrong length for the file it came from is not
  // something to carry forward into a new one.
  if (payload_size != gimg_png_bkgd_len(src_ct)) {
    return GIMG_PNG_RETARGET_DROP;
  }
  if (src_ct == out_color_type && src_bd == out_bit_depth) {
    return GIMG_PNG_RETARGET_KEEP;
  }

  // Resolve to R, G, B at whatever depth the source samples were in.
  uint32_t r = 0, g = 0, b = 0;
  uint8_t src_depth = gimg_png_sample_depth(src_ct, src_bd);
  if (src_ct == 0 || src_ct == 4) {
    r = g = b = gimg_png_be16(payload);
  }
  else if (src_ct == 2 || src_ct == 6) {
    r = gimg_png_be16(payload);
    g = gimg_png_be16(payload + 2);
    b = gimg_png_be16(payload + 4);
  }
  else { // color type 3: the byte is an index into PLTE
    if (!state->plte || state->plte_size < 3u) {
      return GIMG_PNG_RETARGET_DROP;
    }
    size_t entries = state->plte_size / 3u;
    if ((size_t)payload[0] >= entries) {
      return GIMG_PNG_RETARGET_DROP;
    }
    const unsigned char * e = state->plte + (size_t)payload[0] * 3u;
    r = e[0];
    g = e[1];
    b = e[2];
  }

  if (out_color_type == 3) {
    // Writing a palette means the frame arrived as one, so the index is still
    // an index into the same palette; any other source has no index to give.
    return (src_ct == 3) ? GIMG_PNG_RETARGET_KEEP : GIMG_PNG_RETARGET_DROP;
  }
  if (out_color_type == 0 || out_color_type == 4) {
    if (r != g || g != b) {
      return GIMG_PNG_RETARGET_DROP; // no gray level says this color
    }
    gimg_png_put_be16(
        out_buf, gimg_png_rescale_sample(r, src_depth, out_bit_depth));
    *out_size = 2u;
    return GIMG_PNG_RETARGET_REPLACE;
  }
  gimg_png_put_be16(
      out_buf, gimg_png_rescale_sample(r, src_depth, out_bit_depth));
  gimg_png_put_be16(
      out_buf + 2, gimg_png_rescale_sample(g, src_depth, out_bit_depth));
  gimg_png_put_be16(
      out_buf + 4, gimg_png_rescale_sample(b, src_depth, out_bit_depth));
  *out_size = 6u;
  return GIMG_PNG_RETARGET_REPLACE;
}

/**
 * sBIT (11.3.2.4) from the color type the frame arrived as to the one being
 * written.
 *
 * sBIT counts how many of the bits in each stored sample carry the original
 * data, so unlike bKGD it does not survive a change of depth: rescaling by
 * 13.12 spreads the original value across the whole of the new sample, and a
 * count taken before that would tell a decoder to shift data that has already
 * been scaled. Those are dropped. A change of channel count at the same depth
 * is exact - a gray level repeated into R, G and B is significant in each to
 * exactly the same degree - and an alpha channel this writer synthesized is
 * significant in all of its bits.
 */
static gimg_png_retarget_t gimg_png_retarget_sbit(const unsigned char * payload,
    size_t payload_size, const gimg_png_doc_state_t * state,
    uint8_t out_color_type, uint8_t out_bit_depth, unsigned char * out_buf,
    size_t * out_size) {
  uint8_t src_ct = state->ihdr.color_type;
  uint8_t src_bd = state->ihdr.bit_depth;

  if (payload_size != gimg_png_sbit_len(src_ct)) {
    return GIMG_PNG_RETARGET_DROP;
  }
  if (src_ct == out_color_type && src_bd == out_bit_depth) {
    return GIMG_PNG_RETARGET_KEEP;
  }
  if (out_color_type == 3) {
    return (src_ct == 3) ? GIMG_PNG_RETARGET_KEEP : GIMG_PNG_RETARGET_DROP;
  }

  uint8_t src_depth = gimg_png_sample_depth(src_ct, src_bd);
  uint8_t out_depth = gimg_png_sample_depth(out_color_type, out_bit_depth);
  if (src_depth != out_depth) {
    return GIMG_PNG_RETARGET_DROP;
  }

  // Unpack to R, G, B, A significant-bit counts. Where the source had no alpha
  // the writer is adding one, and what it adds is fully significant.
  unsigned int sr, sg, sb, sa = out_depth;
  switch (src_ct) {
  case 0:
    sr = sg = sb = payload[0];
    break;
  case 4:
    sr = sg = sb = payload[0];
    sa = payload[1];
    break;
  case 2:
  case 3:
    sr = payload[0];
    sg = payload[1];
    sb = payload[2];
    break;
  default: // 6
    sr = payload[0];
    sg = payload[1];
    sb = payload[2];
    sa = payload[3];
    break;
  }

  // 11.3.2.4: each value is at least 1 and no more than the sample depth.
  // A source that already broke that is not carried forward.
  if (sr == 0u || sg == 0u || sb == 0u || sa == 0u || sr > out_depth ||
      sg > out_depth || sb > out_depth || sa > out_depth) {
    return GIMG_PNG_RETARGET_DROP;
  }

  if (out_color_type == 0 || out_color_type == 4) {
    if (sr != sg || sg != sb) {
      return GIMG_PNG_RETARGET_DROP; // no single gray count says this
    }
    out_buf[0] = (unsigned char)sr;
    *out_size = 1u;
    if (out_color_type == 4) {
      out_buf[1] = (unsigned char)sa;
      *out_size = 2u;
    }
    return GIMG_PNG_RETARGET_REPLACE;
  }
  out_buf[0] = (unsigned char)sr;
  out_buf[1] = (unsigned char)sg;
  out_buf[2] = (unsigned char)sb;
  *out_size = 3u;
  if (out_color_type == 6) {
    out_buf[3] = (unsigned char)sa;
    *out_size = 4u;
  }
  return GIMG_PNG_RETARGET_REPLACE;
}

/**
 * hIST (11.3.4.2) carries one 16-bit frequency per palette entry and, by the
 * same clause, "shall not appear unless a PLTE chunk appears". There is
 * nothing to translate it into: a truecolor image has no palette for its
 * entries to be about.
 */
static gimg_png_retarget_t gimg_png_retarget_hist(size_t payload_size,
    const gimg_png_doc_state_t * state, uint8_t out_color_type) {
  if (out_color_type != 3 || !state->plte || state->plte_size < 3u) {
    return GIMG_PNG_RETARGET_DROP;
  }
  if (payload_size != (state->plte_size / 3u) * 2u) {
    return GIMG_PNG_RETARGET_DROP; // one entry per palette entry, or nothing
  }
  return GIMG_PNG_RETARGET_KEEP;
}

/**
 * Decide what to do with one preserved ancillary chunk, given the color type
 * and depth the image is actually being written as.
 *
 * @param out_buf Receives a rewritten payload when REPLACE is returned. Must
 *                have room for at least 6 bytes, the longest any of these
 *                produces.
 * @return KEEP for every chunk whose meaning does not depend on the color
 *         type, which is most of them.
 */
gimg_png_retarget_t gimg_png_retarget_ancillary(
    gimg_png_chunk_type_t type, const unsigned char * payload,
    size_t payload_size, const gimg_png_doc_state_t * state,
    uint8_t out_color_type, uint8_t out_bit_depth, unsigned char * out_buf,
    size_t * out_size) {
  if (!state || !payload || payload_size == 0) {
    return GIMG_PNG_RETARGET_KEEP;
  }
  if (type == GIMG_PNG_bKGD) {
    return gimg_png_retarget_bkgd(payload, payload_size, state, out_color_type,
        out_bit_depth, out_buf, out_size);
  }
  if (type == GIMG_PNG_sBIT) {
    return gimg_png_retarget_sbit(payload, payload_size, state, out_color_type,
        out_bit_depth, out_buf, out_size);
  }
  if (type == GIMG_PNG_hIST) {
    return gimg_png_retarget_hist(payload_size, state, out_color_type);
  }
  return GIMG_PNG_RETARGET_KEEP;
}

//
// Building a palette (PNG 11.2.2, color type 3)
// ==============================================
//
// A palette is not written for an arbitrary raster, because choosing which
// colors to keep is quantization - an image-processing decision, and not a
// codec's. But when an image already has 256 colors or fewer there is nothing
// to choose: exactly one palette reproduces it, up to the order of its
// entries. That is not quantization, it is a way of storing what is already
// there, and it is the same kind of decision as picking a row filter.
//
// So a palette is built only when it is lossless, and used only when it is
// smaller. Screenshots, diagrams, icons and line art land here; photographs
// exceed 256 colors in their first few hundred pixels and never do.
//

/** Pack an RGBA8 pixel into one comparable value. */
static uint32_t gimg_png_rgba_key(const unsigned char * p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
      ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/** Slots in the color lookup: twice the largest palette, so it stays sparse. */
#define GIMG_PNG_LUT_SLOTS 512u

/**
 * Open-addressed map from an RGBA8 color to a palette index.
 *
 * The writer needs a pixel's index for every pixel, and a palette holds up to
 * 256 entries, so the obvious linear scan is 256 comparisons per pixel - a
 * megapixel image spends a quarter of a billion comparisons deciding what it
 * already knows. A `used` array rather than a sentinel key, because fully
 * transparent black is a real color and would make a poor "empty".
 */
typedef struct {
  uint32_t key[GIMG_PNG_LUT_SLOTS];
  uint8_t value[GIMG_PNG_LUT_SLOTS];
  uint8_t used[GIMG_PNG_LUT_SLOTS];
  size_t count;
} gimg_png_color_lut_t;

static void gimg_png_lut_init(gimg_png_color_lut_t * lut) {
  memset(lut->used, 0, sizeof(lut->used));
  lut->count = 0;
}

/** Fibonacci hashing: one multiply, then take the high bits. */
static size_t gimg_png_lut_slot(uint32_t key) {
  return (size_t)((key * UINT32_C(2654435761)) >> 23) & (GIMG_PNG_LUT_SLOTS - 1u);
}

static bool gimg_png_lut_get(
    const gimg_png_color_lut_t * lut, uint32_t key, uint8_t * out_value) {
  size_t i = gimg_png_lut_slot(key);
  for (size_t probe = 0; probe < GIMG_PNG_LUT_SLOTS; probe++) {
    if (!lut->used[i]) {
      return false;
    }
    if (lut->key[i] == key) {
      *out_value = lut->value[i];
      return true;
    }
    i = (i + 1u) & (GIMG_PNG_LUT_SLOTS - 1u);
  }
  return false;
}

/** Insert, or leave an existing entry alone. False when the table is full. */
static bool gimg_png_lut_put(
    gimg_png_color_lut_t * lut, uint32_t key, uint8_t value) {
  size_t i = gimg_png_lut_slot(key);
  for (size_t probe = 0; probe < GIMG_PNG_LUT_SLOTS; probe++) {
    if (!lut->used[i]) {
      if (lut->count >= GIMG_PNG_PLTE_MAX_ENTRIES) {
        return false;
      }
      lut->used[i] = 1;
      lut->key[i] = key;
      lut->value[i] = value;
      lut->count++;
      return true;
    }
    if (lut->key[i] == key) {
      return true;
    }
    i = (i + 1u) & (GIMG_PNG_LUT_SLOTS - 1u);
  }
  return false;
}

/**
 * Collect the distinct colors of an 8-bit RGBA raster into a palette, giving
 * up as soon as a 257th appears.
 *
 * Entries with alpha below 255 are placed first. PNG 11.3.2.1 lets tRNS be
 * shorter than the palette, every entry past its end being opaque, so putting
 * the transparent ones first is what makes that saving available - and an
 * image with no transparency then needs no tRNS at all.
 *
 * @param out_plte      Receives up to 256 RGB triples.
 * @param out_trns      Receives the alpha of the leading non-opaque entries.
 * @return false when the image has more than 256 colors, or is not RGBA8.
 */
static bool gimg_png_build_palette(const GIMG_Raster * raster,
    unsigned char * out_plte, size_t * out_plte_size, unsigned char * out_trns,
    size_t * out_trns_size) {
  const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
  if (!fmt || fmt->layout != GIMG_LAYOUT_INTERLEAVED ||
      fmt->channel_model != GIMG_CHANNEL_RGBA || fmt->channel_count != 4 ||
      fmt->bits_per_channel[0] != 8) {
    return false;
  }
  uint32_t w = gimg_raster_width(raster);
  uint32_t h = gimg_raster_height(raster);
  if (w == 0 || h == 0) {
    return false;
  }
  size_t stride = gimg_raster_stride_bytes(raster);
  const unsigned char * pixels =
      (const unsigned char *)gimg_raster_pixels_const(raster);

  // First pass: the distinct colors, in the order they first appear.
  gimg_png_color_lut_t seen;
  gimg_png_lut_init(&seen);
  uint32_t colors[GIMG_PNG_PLTE_MAX_ENTRIES];
  size_t n = 0;
  for (uint32_t y = 0; y < h; y++) {
    const unsigned char * row = pixels + (size_t)y * stride;
    for (uint32_t x = 0; x < w; x++) {
      uint32_t key = gimg_png_rgba_key(row + (size_t)x * 4u);
      uint8_t ignored = 0;
      if (gimg_png_lut_get(&seen, key, &ignored)) {
        continue;
      }
      if (n >= GIMG_PNG_PLTE_MAX_ENTRIES ||
          !gimg_png_lut_put(&seen, key, (uint8_t)n)) {
        return false; // a 257th color: not a palette image
      }
      colors[n++] = key;
    }
  }

  // Second pass: the non-opaque entries first, each group keeping the order it
  // was found in, so the result is deterministic.
  size_t out = 0;
  size_t non_opaque = 0;
  for (int opaque = 0; opaque <= 1; opaque++) {
    for (size_t i = 0; i < n; i++) {
      unsigned char a = (unsigned char)(colors[i] & 0xFFu);
      if ((a == 255u) != (opaque != 0)) {
        continue;
      }
      out_plte[out * 3u] = (unsigned char)(colors[i] >> 24);
      out_plte[out * 3u + 1u] = (unsigned char)((colors[i] >> 16) & 0xFFu);
      out_plte[out * 3u + 2u] = (unsigned char)((colors[i] >> 8) & 0xFFu);
      if (opaque == 0) {
        out_trns[out] = a;
        non_opaque = out + 1u;
      }
      out++;
    }
  }
  *out_plte_size = out * 3u;
  *out_trns_size = non_opaque;
  return true;
}

/** Smallest bit depth color type 3 allows for @a entries indices (11.2.2). */
static uint8_t gimg_png_palette_bit_depth(size_t entries) {
  if (entries <= 2u) {
    return 1u;
  }
  if (entries <= 4u) {
    return 2u;
  }
  if (entries <= 16u) {
    return 4u;
  }
  return 8u;
}

/**
 * True for the chunks that say what the samples mean.
 *
 * A file that brought any of these has already said something about its color,
 * and what it said wins over what the raster's GIMG_Color_Info was reduced to
 * on the way in - the chunk is what was actually there.  cICP is included even
 * though this writer never generates one, because a file that carried one has
 * said the most specific thing of all.
 */
static bool gimg_png_chunk_is_color(gimg_png_chunk_type_t t) {
  return t == GIMG_PNG_iCCP || t == GIMG_PNG_sRGB || t == GIMG_PNG_gAMA ||
      t == GIMG_PNG_cHRM || t == GIMG_PNG_cICP;
}

static bool gimg_png_chunk_is_known_semantic(gimg_png_chunk_type_t t) {
  return t == GIMG_PNG_iCCP || t == GIMG_PNG_sRGB || t == GIMG_PNG_gAMA ||
      t == GIMG_PNG_cHRM || t == GIMG_PNG_eXIf || t == GIMG_PNG_tEXt ||
      t == GIMG_PNG_zTXt || t == GIMG_PNG_iTXt;
}

/**
 * Return true if the text chunk payload has keyword "Description" or "Comment"
 * (case-sensitive; keyword is the first null-terminated string).
 */
static bool gimg_png_text_keyword_is_description_or_comment(
    const unsigned char * payload, size_t payload_size) {
  if (!payload || payload_size == 0) {
    return false;
  }
  size_t kw_len = 0;
  while (kw_len < payload_size && payload[kw_len] != 0) {
    kw_len++;
  }
  if (kw_len >= payload_size) {
    return false;
  }
  if (kw_len == 11 && memcmp(payload, "Description", 11) == 0) {
    return true;
  }
  if (kw_len == 7 && memcmp(payload, "Comment", 7) == 0) {
    return true;
  }
  return false;
}

/**
 * Return true if the text chunk payload has a GPS-related keyword (tEXt/zTXt/iTXt:
 * keyword is the first null-terminated string). STRIP_GPS skips such chunks.
 */
static bool gimg_png_text_keyword_is_gps(
    const unsigned char * payload, size_t payload_size) {
  if (!payload || payload_size == 0) {
    return false;
  }
  size_t kw_len = 0;
  while (kw_len < payload_size && payload[kw_len] != 0) {
    kw_len++;
  }
  if (kw_len == 0) {
    return false;
  }
  // Case-insensitive: "GPS", "GPS ", "EXIF:GPS", "exif:gps", etc.
  if (kw_len >= 3) {
    unsigned char a = (unsigned char)tolower((unsigned char)payload[0]);
    unsigned char b = (unsigned char)tolower((unsigned char)payload[1]);
    unsigned char c = (unsigned char)tolower((unsigned char)payload[2]);
    if (a == 'g' && b == 'p' && c == 's') {
      if (kw_len == 3 || payload[3] == ' ' || payload[3] == 0) {
        return true;
      }
    }
  }
  if (kw_len >= 8) {
    const char * exif_gps = "exif:gps";
    bool match = true;
    for (size_t i = 0; i < 8 && i < kw_len; i++) {
      if (tolower((unsigned char)payload[i]) != (unsigned char)exif_gps[i]) {
        match = false;
        break;
      }
    }
    if (match) {
      return true;
    }
  }
  return false;
}

/** Write a 16-bit sample (host order) to buffer in PNG big-endian order. */
static void gimg_png_write_be16(unsigned char * out, uint16_t value) {
  out[0] = (unsigned char)(value >> 8);
  out[1] = (unsigned char)(value & 0xFFu);
}

/** Fill raw image rows (filter byte + row data) from raster. Caller allocates
 * raw_size = height * (1 + row_bytes). For palette (color_type 3), @a state
 * must be non-NULL with plte/trns; raster must be RGBA8. */
//
// Row filtering (PNG 9: filter method 0, types 0-4).
//
// Filtering works on bytes, not pixels (PNG 9.2), with bpp the number of bytes
// in a complete pixel rounded up to one, so at depths below 8 the "pixel to
// the left" is the byte to the left. Each filter subtracts a prediction from
// the raw byte, modulo 256; the decoder adds it back.
//
// Rows are filtered in place from the last to the first, and within a row from
// the last byte to the first. Every predictor reads only the bytes above
// (a previous row, not yet filtered because we are moving upwards) and to the
// left (a lower index, not yet filtered because we are moving leftwards), so
// no copy of the unfiltered data is needed.
//

/** Paeth predictor (PNG 9.4). Same function the decoder reconstructs with. */
static unsigned char gimg_png_paeth_predictor(int a, int b, int c) {
  int p = a + b - c;
  int pa = p > a ? p - a : a - p;
  int pb = p > b ? p - b : b - p;
  int pc = p > c ? p - c : c - p;
  if (pa <= pb && pa <= pc) {
    return (unsigned char)a;
  }
  if (pb <= pc) {
    return (unsigned char)b;
  }
  return (unsigned char)c;
}

/** The value filter @a type subtracts from raw byte @a i. PNG 9.3, Table 9.1. */
static unsigned int gimg_png_filter_prediction(unsigned int type,
    const unsigned char * raw, const unsigned char * prior, size_t i,
    unsigned int bpp) {
  unsigned int left = (i >= (size_t)bpp) ? raw[i - bpp] : 0u;
  unsigned int up = prior ? prior[i] : 0u;
  switch (type) {
  case 0: // None
    return 0u;
  case 1: // Sub
    return left;
  case 2: // Up
    return up;
  case 3: // Average
    return (left + up) / 2u;
  case 4: // Paeth
  {
    unsigned int up_left = (prior && i >= (size_t)bpp) ? prior[i - bpp] : 0u;
    return gimg_png_paeth_predictor((int)left, (int)up, (int)up_left);
  }
  default:
    return 0u;
  }
}

/**
 * Cost of filtering a row with @a type, as PNG 12.8 defines it: the sum of the
 * absolute values of the filtered bytes read as signed. The smallest sum tends
 * to leave the most for DEFLATE to work with, which is the whole point of
 * choosing per row.
 */
static unsigned long gimg_png_filter_cost(unsigned int type,
    const unsigned char * raw, const unsigned char * prior, size_t row_bytes,
    unsigned int bpp) {
  unsigned long sum = 0;
  for (size_t i = 0; i < row_bytes; i++) {
    unsigned int pred = gimg_png_filter_prediction(type, raw, prior, i, bpp);
    unsigned char out = (unsigned char)((unsigned int)raw[i] - pred);
    sum += (out < 128u) ? out : (unsigned long)(256u - out);
  }
  return sum;
}

/** Apply filter @a type to a row in place, right to left. PNG 9.3. */
static void gimg_png_apply_filter(unsigned int type, unsigned char * raw,
    const unsigned char * prior, size_t row_bytes, unsigned int bpp) {
  if (type == 0) {
    return;
  }
  for (size_t i = row_bytes; i-- > 0;) {
    unsigned int pred = gimg_png_filter_prediction(type, raw, prior, i, bpp);
    raw[i] = (unsigned char)((unsigned int)raw[i] - pred);
  }
}

/**
 * Filter @a row_count consecutive rows laid out as a filter byte followed by
 * @a row_bytes of data, which is how both a whole non-interlaced image and one
 * Adam7 pass are stored. @a filter_choice is a GIMG_PNG_FILTER_* value.
 */
static void gimg_png_filter_rows(unsigned char * rows, size_t row_count,
    size_t row_bytes, unsigned int bpp, unsigned int filter_choice) {
  if (row_count == 0 || row_bytes == 0) {
    return;
  }
  size_t row_stride = 1u + row_bytes;
  for (size_t y = row_count; y-- > 0;) {
    unsigned char * row = rows + y * row_stride;
    // PNG 9.2: the row above the first one in a pass is treated as all zeroes.
    const unsigned char * prior =
        (y == 0) ? NULL : (rows + (y - 1u) * row_stride + 1u);
    unsigned int chosen = 0;
    if (filter_choice == GIMG_PNG_FILTER_ADAPTIVE) {
      unsigned long best = ULONG_MAX;
      for (unsigned int type = 0; type <= 4; type++) {
        unsigned long cost =
            gimg_png_filter_cost(type, row + 1, prior, row_bytes, bpp);
        if (cost < best) {
          best = cost;
          chosen = type;
        }
      }
    }
    else {
      chosen = filter_choice - 1u; // NONE == 1 maps to filter type 0.
    }
    gimg_png_apply_filter(chosen, row + 1, prior, row_bytes, bpp);
    row[0] = (unsigned char)chosen;
  }
}

/** Bytes per complete pixel, rounded up to one (PNG 9.2). */
static unsigned int gimg_png_save_bpp(uint8_t color_type, uint8_t bit_depth) {
  unsigned int channels;
  switch (color_type) {
  case 0:
  case 3:
    channels = 1u;
    break;
  case 2:
    channels = 3u;
    break;
  case 4:
    channels = 2u;
    break;
  case 6:
    channels = 4u;
    break;
  default:
    return 1u;
  }
  unsigned int bits = channels * (unsigned int)bit_depth;
  return (bits + 7u) / 8u;
}

/** Palette index for a raster pixel; false when no entry matches. PNG 11.2.2. */
static bool gimg_png_palette_index_at(const GIMG_Raster * raster,
    const gimg_png_doc_state_t * state, uint32_t x, uint32_t y,
    uint8_t * out_index);

/** Color-to-index map for a palette, built once per image. */
static void gimg_png_palette_lut_build(
    const gimg_png_doc_state_t * state, gimg_png_color_lut_t * lut);
static bool gimg_png_palette_index_lut(const GIMG_Raster * raster,
    const gimg_png_color_lut_t * lut, uint32_t x, uint32_t y,
    uint8_t * out_index);

/** One sample at a bit depth below 8 (palette index or grayscale level). */
static bool gimg_png_sub_byte_sample_at(const GIMG_Raster * raster,
    uint8_t color_type, uint8_t bit_depth, const gimg_png_doc_state_t * state,
    uint32_t x, uint32_t y, uint8_t * out_sample);

static GIMG_Result gimg_png_raster_to_raw_rows(const GIMG_Raster * raster,
    uint8_t color_type, uint8_t bit_depth, const gimg_png_doc_state_t * state,
    unsigned char * raw, size_t raw_size, unsigned int filter_choice) {
  uint32_t w = gimg_raster_width(raster);
  uint32_t h = gimg_raster_height(raster);
  size_t row_bytes = gimg_png_row_bytes(color_type, bit_depth, w);
  if (row_bytes == 0) {
    return GIMG_ERR_FORMAT;
  }
  if (raw_size < (size_t)h * (1u + row_bytes)) {
    return GIMG_ERR_INTERNAL;
  }
  size_t stride = gimg_raster_stride_bytes(raster);
  const unsigned char * pixels =
      (const unsigned char *)gimg_raster_pixels_const(raster);
  const GIMG_Pixel_Format * src_fmt = gimg_raster_format(raster);
  bool gray_source =
      src_fmt && src_fmt->channel_model == GIMG_CHANNEL_GRAY;

  if (color_type == 3) {
    if (!state || !state->plte || state->plte_size == 0) {
      return GIMG_ERR_FORMAT;
    }
    // Built once for the whole image rather than once per pixel: this is the
    // inner loop of writing a palette image.
    gimg_png_color_lut_t lut;
    gimg_png_palette_lut_build(state, &lut);
    for (uint32_t y = 0; y < h; y++) {
      unsigned char * row = raw + (size_t)y * (1u + row_bytes);
      row[0] = 0;
      // PNG 7.2: at depth 1, 2 or 4 several indices share a byte, so the row
      // is row_bytes long and not w. Clear it first because packing writes
      // single samples into shared bytes and leaves the row's padding bits.
      memset(row + 1, 0, row_bytes);
      for (uint32_t x = 0; x < w; x++) {
        uint8_t index = 0;
        if (!gimg_png_palette_index_lut(raster, &lut, x, y, &index)) {
          return GIMG_ERR_UNSUPPORTED;
        }
        if (bit_depth < 8) {
          gimg_png_set_sample_bits(row + 1, x, bit_depth, index);
        }
        else {
          row[1u + (size_t)x] = (unsigned char)index;
        }
      }
    }
    gimg_png_filter_rows(raw, (size_t)h, row_bytes,
        gimg_png_save_bpp(color_type, bit_depth), filter_choice);
    return GIMG_OK;
  }

  for (uint32_t y = 0; y < h; y++) {
    unsigned char * row = raw + (size_t)y * (1u + row_bytes);
    row[0] = 0;
    const unsigned char * src = pixels + (size_t)y * stride;
    if (color_type == 0) {
      // The source is a GRAY raster, or an RGBA one whose color channels
      // agree - which is how a grayscale frame comes back when tRNS gave it an
      // alpha channel on the way in (PNG 11.3.2.1). Either way only the first
      // channel is written.
      if (bit_depth < 8) {
        // PNG 7.2: several samples to a byte, so the row is shorter than w and
        // the samples are placed by bit. Cleared first because packing writes
        // into shared bytes and leaves the row's padding bits.
        memset(row + 1, 0, row_bytes);
        for (uint32_t x = 0; x < w; x++) {
          uint8_t sample = 0;
          if (!gimg_png_sub_byte_sample_at(
                  raster, color_type, bit_depth, state, x, y, &sample)) {
            return GIMG_ERR_UNSUPPORTED;
          }
          gimg_png_set_sample_bits(row + 1, x, bit_depth, sample);
        }
        continue;
      }
      size_t src_pixel_bytes = gray_source ? (bit_depth == 8 ? 1u : 2u)
                                           : (bit_depth == 8 ? 4u : 8u);
      if (bit_depth == 8) {
        if (gray_source) {
          memcpy(row + 1, src, row_bytes);
        }
        else {
          for (uint32_t x = 0; x < w; x++) {
            row[1u + (size_t)x] = src[(size_t)x * src_pixel_bytes];
          }
        }
      }
      else {
        for (uint32_t x = 0; x < w; x++) {
          const unsigned char * p = src + (size_t)x * src_pixel_bytes;
          gimg_png_write_be16(
              row + 1 + (size_t)x * 2u, (uint16_t)(p[0] | (p[1] << 8)));
        }
      }
    }
    else if (color_type == 2) {
      if (bit_depth == 8) {
        for (uint32_t x = 0; x < w; x++) {
          row[1u + (size_t)x * 3u + 0u] = src[0];
          row[1u + (size_t)x * 3u + 1u] = src[1];
          row[1u + (size_t)x * 3u + 2u] = src[2];
          src += 4;
        }
      }
      else {
        for (uint32_t x = 0; x < w; x++) {
          gimg_png_write_be16(row + 1 + 6u * (size_t)x + 0u,
              (uint16_t)(src[0] | (src[1] << 8)));
          gimg_png_write_be16(row + 1 + 6u * (size_t)x + 2u,
              (uint16_t)(src[2] | (src[3] << 8)));
          gimg_png_write_be16(row + 1 + 6u * (size_t)x + 4u,
              (uint16_t)(src[4] | (src[5] << 8)));
          src += 8;
        }
      }
    }
    else if (color_type == 4) {
      if (bit_depth == 8) {
        for (uint32_t x = 0; x < w; x++) {
          row[1u + (size_t)x * 2u + 0u] = src[0];
          row[1u + (size_t)x * 2u + 1u] = src[3];
          src += 4;
        }
      }
      else {
        for (uint32_t x = 0; x < w; x++) {
          gimg_png_write_be16(row + 1 + 4u * (size_t)x + 0u,
              (uint16_t)(src[0] | (src[1] << 8)));
          gimg_png_write_be16(row + 1 + 4u * (size_t)x + 2u,
              (uint16_t)(src[6] | (src[7] << 8)));
          src += 8;
        }
      }
    }
    else if (color_type == 6) {
      if (bit_depth == 8) {
        memcpy(row + 1, src, row_bytes);
      }
      else {
        for (uint32_t x = 0; x < w; x++) {
          gimg_png_write_be16(row + 1 + 8u * (size_t)x + 0u,
              (uint16_t)(src[0] | (src[1] << 8)));
          gimg_png_write_be16(row + 1 + 8u * (size_t)x + 2u,
              (uint16_t)(src[2] | (src[3] << 8)));
          gimg_png_write_be16(row + 1 + 8u * (size_t)x + 4u,
              (uint16_t)(src[4] | (src[5] << 8)));
          gimg_png_write_be16(row + 1 + 8u * (size_t)x + 6u,
              (uint16_t)(src[6] | (src[7] << 8)));
          src += 8;
        }
      }
    }
  }
  gimg_png_filter_rows(raw, (size_t)h, row_bytes,
      gimg_png_save_bpp(color_type, bit_depth), filter_choice);
  return GIMG_OK;
}

/**
 * Write one pixel from raster at (x,y) to dest in PNG sample order (BE for
 * 16-bit). Returns number of bytes written (1/2 for gray, 1 for palette, 4/8
 * for RGBA).
 */
/**
 * Build the color-to-index map for a palette.
 *
 * PNG 11.3.2.1: tRNS gives the alpha of the leading entries and every entry
 * past its end is opaque, so an entry's color is its RGB together with that
 * alpha - two entries with the same RGB and different alpha are different
 * colors, and a pixel matches only one of them.
 *
 * A palette may legitimately hold the same color twice. The first index wins,
 * which is what a linear scan did as well.
 */
static void gimg_png_palette_lut_build(
    const gimg_png_doc_state_t * state, gimg_png_color_lut_t * lut) {
  gimg_png_lut_init(lut);
  if (!state || !state->plte || state->plte_size == 0) {
    return;
  }
  size_t entries = state->plte_size / 3u;
  if (entries > GIMG_PNG_PLTE_MAX_ENTRIES) {
    entries = GIMG_PNG_PLTE_MAX_ENTRIES;
  }
  size_t trns_count = state->trns ? state->trns_size : 0;
  for (size_t i = 0; i < entries; i++) {
    unsigned char rgba[4] = {state->plte[i * 3u], state->plte[i * 3u + 1u],
        state->plte[i * 3u + 2u],
        (i < trns_count) ? state->trns[i] : (unsigned char)255};
    (void)gimg_png_lut_put(lut, gimg_png_rgba_key(rgba), (uint8_t)i);
  }
}

/** One pixel's palette index, through a map built by the caller. */
static bool gimg_png_palette_index_lut(const GIMG_Raster * raster,
    const gimg_png_color_lut_t * lut, uint32_t x, uint32_t y,
    uint8_t * out_index) {
  size_t stride = gimg_raster_stride_bytes(raster);
  const unsigned char * pixels =
      (const unsigned char *)gimg_raster_pixels_const(raster);
  return gimg_png_lut_get(
      lut, gimg_png_rgba_key(pixels + (size_t)y * stride + (size_t)x * 4u),
      out_index);
}

static bool gimg_png_palette_index_at(const GIMG_Raster * raster,
    const gimg_png_doc_state_t * state, uint32_t x, uint32_t y,
    uint8_t * out_index) {
  if (!state || !state->plte || state->plte_size == 0) {
    return false;
  }
  gimg_png_color_lut_t lut;
  gimg_png_palette_lut_build(state, &lut);
  return gimg_png_palette_index_lut(raster, &lut, x, y, out_index);
}

/**
 * Sample value for one pixel at a bit depth below 8: a palette index for
 * color type 3, or a grayscale level for color type 0, rescaled from the
 * raster's 8 bits by the inverse of the rescaling decode applies (PNG 13.12).
 * Returns false when no palette entry matches the pixel.
 */
static bool gimg_png_sub_byte_sample_at(const GIMG_Raster * raster,
    uint8_t color_type, uint8_t bit_depth, const gimg_png_doc_state_t * state,
    uint32_t x, uint32_t y, uint8_t * out_sample) {
  if (color_type == 3) {
    return gimg_png_palette_index_at(raster, state, x, y, out_sample);
  }
  if (color_type != 0) {
    return false;
  }
  size_t stride = gimg_raster_stride_bytes(raster);
  const unsigned char * pixels =
      (const unsigned char *)gimg_raster_pixels_const(raster);
  unsigned int gray = pixels[(size_t)y * stride + (size_t)x];
  unsigned int max_val = (1u << bit_depth) - 1u;
  *out_sample = (uint8_t)((gray * max_val + 127u) / 255u);
  return true;
}

static size_t gimg_png_write_pixel_at(const GIMG_Raster * raster,
    uint8_t color_type, uint8_t bit_depth, const gimg_png_doc_state_t * state,
    uint32_t x, uint32_t y, unsigned char * dest) {
  size_t stride = gimg_raster_stride_bytes(raster);
  const unsigned char * pixels =
      (const unsigned char *)gimg_raster_pixels_const(raster);
  const unsigned char * src = pixels + (size_t)y * stride;

  if (color_type == 3) {
    uint8_t index = 0;
    if (!gimg_png_palette_index_at(raster, state, x, y, &index)) {
      return 0;  // no palette match
    }
    dest[0] = (unsigned char)index;
    return 1;
  }

  if (color_type == 0) {
    // As in the non-interlaced filler: the source may be GRAY, or RGBA whose
    // color channels agree because tRNS gave the frame an alpha channel.
    const GIMG_Pixel_Format * src_fmt = gimg_raster_format(raster);
    bool gray_source = src_fmt && src_fmt->channel_model == GIMG_CHANNEL_GRAY;
    size_t src_pixel_bytes = gray_source ? (bit_depth == 8 ? 1u : 2u)
                                         : (bit_depth == 8 ? 4u : 8u);
    const unsigned char * p = src + (size_t)x * src_pixel_bytes;
    if (bit_depth == 8) {
      dest[0] = p[0];
      return 1;
    }
    gimg_png_write_be16(dest, (uint16_t)(p[0] | (p[1] << 8)));
    return 2;
  }
  if (color_type == 2) {
    const unsigned char * p = src + (size_t)x * (bit_depth == 8 ? 4u : 8u);
    if (bit_depth == 8) {
      dest[0] = p[0];
      dest[1] = p[1];
      dest[2] = p[2];
      return 3;
    }
    gimg_png_write_be16(dest + 0, (uint16_t)(p[0] | (p[1] << 8)));
    gimg_png_write_be16(dest + 2, (uint16_t)(p[2] | (p[3] << 8)));
    gimg_png_write_be16(dest + 4, (uint16_t)(p[4] | (p[5] << 8)));
    return 6;
  }
  if (color_type == 4) {
    const unsigned char * p = src + (size_t)x * (bit_depth == 8 ? 4u : 8u);
    if (bit_depth == 8) {
      dest[0] = p[0];
      dest[1] = p[3];
      return 2;
    }
    gimg_png_write_be16(dest + 0, (uint16_t)(p[0] | (p[1] << 8)));
    gimg_png_write_be16(dest + 2, (uint16_t)(p[6] | (p[7] << 8)));
    return 4;
  }
  if (color_type == 6) {
    if (bit_depth == 8) {
      memcpy(dest, src + (size_t)x * 4u, 4);
      return 4;
    }
    const unsigned char * p = src + (size_t)x * 8u;
    gimg_png_write_be16(dest + 0, (uint16_t)(p[0] | (p[1] << 8)));
    gimg_png_write_be16(dest + 2, (uint16_t)(p[2] | (p[3] << 8)));
    gimg_png_write_be16(dest + 4, (uint16_t)(p[4] | (p[5] << 8)));
    gimg_png_write_be16(dest + 6, (uint16_t)(p[6] | (p[7] << 8)));
    return 8;
  }
  return 0;
}

/** Fill raw buffer with Adam7 pass-ordered rows (filter byte + row per pass
 * row). */
static GIMG_Result gimg_png_raster_to_raw_rows_adam7(const GIMG_Raster * raster,
    uint8_t color_type, uint8_t bit_depth, const gimg_png_doc_state_t * state,
    unsigned char * raw, size_t raw_size, unsigned int filter_choice) {
  uint32_t w = gimg_raster_width(raster);
  uint32_t h = gimg_raster_height(raster);
  size_t expected = 0;
  if (!gimg_png_adam7_raw_size(w, h, color_type, bit_depth, &expected)) {
    return GIMG_ERR_LIMIT;
  }
  if (raw_size < expected) {
    return GIMG_ERR_INTERNAL;
  }
  size_t raw_off = 0;
  for (int pass = 0; pass < 7; pass++) {
    uint32_t pw = 0;
    uint32_t ph = 0;
    gimg_png_adam7_pass_dims(w, h, (unsigned int)pass, &pw, &ph);
    if (pw == 0 || ph == 0) {
      continue;
    }
    const gimg_png_adam7_pass_t * ap = &gimg_png_adam7_passes[pass];

    // Each pass is its own image as far as PNG 7.2 is concerned: pw pixels a
    // row, padded to a whole byte independently of the other passes.
    size_t pass_row_bytes = gimg_png_row_bytes(color_type, bit_depth, pw);
    if (pass_row_bytes == 0) {
      return GIMG_ERR_FORMAT;
    }
    size_t pass_start = raw_off;
    for (uint32_t j = 0; j < ph; j++) {
      unsigned char * row = raw + raw_off;
      row[0] = 0;  // filter byte
      memset(row + 1, 0, pass_row_bytes);
      uint32_t y = ap->y_offset + j * ap->y_step;
      size_t off = 0;
      for (uint32_t i = 0; i < pw; i++) {
        uint32_t x = ap->x_offset + i * ap->x_step;
        if (bit_depth < 8) {
          uint8_t sample = 0;
          if (!gimg_png_sub_byte_sample_at(
                  raster, color_type, bit_depth, state, x, y, &sample)) {
            return GIMG_ERR_UNSUPPORTED;
          }
          // The sample's place in the pass row is i, not x: the pass is
          // packed densely and only scattered to x on decode.
          gimg_png_set_sample_bits(row + 1, i, bit_depth, sample);
        }
        else {
          size_t n = gimg_png_write_pixel_at(
              raster, color_type, bit_depth, state, x, y, row + 1 + off);
          if (n == 0) {
            return GIMG_ERR_UNSUPPORTED;
          }
          off += n;
        }
      }
      raw_off += 1u + pass_row_bytes;
    }
    // PNG 9.2: each pass is filtered as its own image, so the row above the
    // first row of a pass is all zeroes rather than the last row of the
    // previous pass.
    gimg_png_filter_rows(raw + pass_start, (size_t)ph, pass_row_bytes,
        gimg_png_save_bpp(color_type, bit_depth), filter_choice);
  }
  return GIMG_OK;
}


/**
 * Write the color chunk that the raster's GIMG_Color_Info calls for.
 *
 * At most one is written: PNG 11.3.3.3 does not want sRGB and iCCP in the same
 * file, and gAMA is redundant beside either.  sRGB comes first because it is
 * the smaller and more widely acted-upon statement; an embedded profile is
 * written only when there is nothing more specific to say than the profile
 * itself.  A document carrying both therefore keeps the sRGB flag.
 *
 * This exists because it is wanted twice: once for GIMG_META_KEEP_COMMON_ONLY,
 * which writes nothing else, and once as a fallback for the ordinary policies,
 * where a document that arrived as something other than a PNG has no ancillary
 * chunks to preserve and would otherwise lose its color entirely.
 *
 * @param stream Destination.
 * @param info The raster's color info.
 * @param alloc Allocator for the iCCP compression buffer.
 * @param report Byte count is added to.
 * @return GIMG_OK, or a write error.  A profile that cannot be compressed is
 *   skipped rather than fatal: the picture is not wrong because its color
 *   annotation could not be written.
 */
static GIMG_Result gimg_png_write_color_from_info(GIMG_Stream * stream,
    const GIMG_Color_Info * info, const GIMG_Allocator * alloc,
    GIMG_Save_Report * report) {
  GIMG_Result r = GIMG_OK;
  // The sRGB chunk asserts the whole of sRGB, its transfer curve included, so
  // it takes the transfer actually saying so.  Matching on the primaries alone
  // was too loose: a BMP with a calibrated V4 header naming sRGB's primaries
  // and a gamma of 2.2 is not an sRGB image, and writing sRGB for it threw the
  // gamma away and claimed a curve the file never stated.
  if (info->transfer == GIMG_TRANSFER_SRGB) {
    unsigned char srgb_byte =
        (unsigned char)(info->intent & 3u);
    r = gimg_png_write_chunk(stream, GIMG_PNG_sRGB, &srgb_byte, 1);
    if (r != GIMG_OK) {
      return r;
    }
    report->bytes_written += 8 + 1 + 4;
    return r;
  }

  // cHRM states the gamut and nothing else, so it goes beside gAMA rather
  // than instead of it (PNG 11.3.2.1; the two are a pair).  It is written
  // only for a gamut a reader would not otherwise assume: sRGB's primaries
  // are what a PNG with no such chunk means, so stating them costs 44 bytes
  // and says nothing new, while leaving Adobe RGB unstated loses it - which
  // it did, so a BMP with a calibrated V4 header naming Adobe RGB came out
  // of a save as PNG carrying its gamma and not its gamut.
  //
  // Not written beside iCCP: the profile is the more specific statement and
  // supersedes it, and 11.3.3.3 does not want the two disagreeing.
  if (info->primaries == GIMG_PRIMARIES_ADOBE_RGB &&
      !(info->icc_bytes && info->icc_size > 0)) {
    // White point D65, then red, green and blue, each x and y times 100000.
    static const uint32_t adobe_rgb_chrm[8] = {
        31270u, 32900u, 64000u, 33000u, 21000u, 71000u, 15000u, 6000u};
    unsigned char chrm[32];
    for (unsigned int i = 0; i < 8; i++) {
      chrm[i * 4] = (unsigned char)(adobe_rgb_chrm[i] >> 24);
      chrm[(i * 4) + 1] = (unsigned char)(adobe_rgb_chrm[i] >> 16);
      chrm[(i * 4) + 2] = (unsigned char)(adobe_rgb_chrm[i] >> 8);
      chrm[(i * 4) + 3] = (unsigned char)(adobe_rgb_chrm[i] & 0xFFu);
    }
    r = gimg_png_write_chunk(stream, GIMG_PNG_cHRM, chrm, sizeof(chrm));
    if (r != GIMG_OK) {
      return r;
    }
    report->bytes_written += 8 + sizeof(chrm) + 4;
  }

  if ((info->transfer == GIMG_TRANSFER_GAMMA &&
          info->gamma_value > 0.0) ||
      info->transfer == GIMG_TRANSFER_LINEAR) {
    // gAMA states the transfer and nothing about the primaries, which for an
    // image whose primaries are sRGB's costs nothing: those are what a PNG
    // reader assumes when no chunk says otherwise.  Linear is a gamma of 1.
    double gamma =
        info->transfer == GIMG_TRANSFER_LINEAR ? 1.0 : info->gamma_value;
    // gAMA holds gamma x 100000 in four bytes, so it cannot state a gamma
    // above about 42949.  A BMP's V4 gamma is 16.16 fixed point and reaches
    // 65535, and converting one of those to uint32_t is undefined behaviour
    // rather than a large number - UBSan caught exactly that here, on a value
    // of 4.98588e+09.  A gamma the chunk cannot hold goes unsaid, which is
    // what this writer does with every other thing it cannot state.
    double scaled = (gamma * 100000.0) + 0.5;
    if (scaled >= 1.0 && scaled <= 4294967295.0) {
      uint32_t gama_val = (uint32_t)scaled;
      unsigned char gama[4];
      gama[0] = (unsigned char)(gama_val >> 24);
      gama[1] = (unsigned char)(gama_val >> 16);
      gama[2] = (unsigned char)(gama_val >> 8);
      gama[3] = (unsigned char)(gama_val & 0xFFu);
      r = gimg_png_write_chunk(stream, GIMG_PNG_gAMA, gama, 4);
      if (r != GIMG_OK) {
        return r;
      }
      report->bytes_written += 8 + 4 + 4;
    }
  }
  else if (info->icc_bytes &&
      info->icc_size > 0) {
    // iCCP is "ICC Profile\0", a compression-method byte, then the profile
    // as a zlib stream (PNG 11.3.3.3).  Encoded straight into place after
    // that 13-byte prefix, so the container never has to be assembled by
    // hand.
    size_t icc_prefix = 13;  // "ICC Profile" + NUL + compression method
    size_t icc_cap = info->icc_size +
        (info->icc_size / 2) + 64 + GIMG_PNG_ZLIB_MIN_BYTES;
    unsigned char * iccp_buf = (unsigned char *)gimg_malloc(
        alloc, icc_prefix + icc_cap);
    if (iccp_buf) {
      memcpy(iccp_buf, "ICC Profile", 11);
      iccp_buf[11] = 0;
      iccp_buf[12] = 0;  // compression method: zlib, the only one PNG has
      gcomp_options_t * gopts_icc = NULL;
      gcomp_status_t gs = gcomp_options_create(&gopts_icc);
      if (gs == GCOMP_OK && gopts_icc) {
        size_t icc_len = 0;
        gs = gcomp_encode_buffer(gcomp_registry_default(), "zlib", gopts_icc,
            (const unsigned char *)info->icc_bytes,
            info->icc_size, iccp_buf + icc_prefix, icc_cap,
            &icc_len);
        gcomp_options_destroy(gopts_icc);
        if (gs == GCOMP_OK && icc_len > 0) {
          r = gimg_png_write_chunk(
              stream, GIMG_PNG_iCCP, iccp_buf, icc_prefix + icc_len);
          if (r == GIMG_OK) {
            report->bytes_written += 8 + (icc_prefix + icc_len) + 4;
          }
        }
      }
      gimg_free(alloc, iccp_buf);
    }
    if (r != GIMG_OK) {
      return r;
    }
  }
  return r;
}

/**
 * Encode one raster to zlib-wrapped DEFLATE (same format as IDAT/fdAT).
 * On success, *out_zlib is allocated and must be freed by caller.
 */
static GIMG_Result gimg_png_raster_to_zlib(const GIMG_Raster * raster,
    uint8_t color_type, uint8_t bit_depth, const gimg_png_doc_state_t * state,
    int do_interlaced, unsigned int filter_choice,
    const GIMG_Allocator * allocator,
    unsigned char ** out_zlib, size_t * out_zlib_len) {
  uint32_t width = gimg_raster_width(raster);
  uint32_t height = gimg_raster_height(raster);
  size_t row_bytes = gimg_png_row_bytes(color_type, bit_depth, width);
  if (row_bytes == 0) {
    return GIMG_ERR_FORMAT;
  }
  size_t raw_size = 0;
  if (do_interlaced) {
    if (!gimg_png_adam7_raw_size(
            width, height, color_type, bit_depth, &raw_size)) {
      return GIMG_ERR_LIMIT;
    }
  }
  else {
    size_t row_stride = 1u + row_bytes;
    if (!gcu_safe_mul_size((size_t)height, row_stride, &raw_size)) {
      return GIMG_ERR_LIMIT;
    }
  }
  unsigned char * raw =
      (unsigned char *)gimg_malloc(gimg_alloc_or_default(allocator), raw_size);
  if (!raw) {
    return GIMG_ERR_OOM;
  }
  GIMG_Result r;
  if (do_interlaced) {
    r = gimg_png_raster_to_raw_rows_adam7(
        raster, color_type, bit_depth, state, raw, raw_size, filter_choice);
  }
  else {
    r = gimg_png_raster_to_raw_rows(
        raster, color_type, bit_depth, state, raw, raw_size, filter_choice);
  }
  if (r != GIMG_OK) {
    gimg_free(gimg_alloc_or_default(allocator), raw);
    return r;
  }
  // Room for the compressed rows plus the six bytes RFC 1950 wraps them in.
  // The zlib method writes the whole container, so this is the only buffer:
  // the header bytes and the Adler-32 used to be assembled here by hand, into
  // a second allocation, after a separate deflate pass.
  size_t zlib_cap = raw_size + (raw_size / 2) + 64 + GIMG_PNG_ZLIB_MIN_BYTES;
  if (zlib_cap < raw_size) {
    gimg_free(gimg_alloc_or_default(allocator), raw);
    return GIMG_ERR_OOM;
  }
  unsigned char * zlib_buf = (unsigned char *)gimg_malloc(
      gimg_alloc_or_default(allocator), zlib_cap);
  if (!zlib_buf) {
    gimg_free(gimg_alloc_or_default(allocator), raw);
    return GIMG_ERR_OOM;
  }
  gcomp_options_t * gopts = NULL;
  gcomp_status_t gs = gcomp_options_create(&gopts);
  if (gs != GCOMP_OK || !gopts) {
    gimg_free(gimg_alloc_or_default(allocator), zlib_buf);
    gimg_free(gimg_alloc_or_default(allocator), raw);
    return GIMG_ERR_OOM;
  }
  // Ask for deferred matching by name.
  //
  // At the compression level used here this is identical to "default" - the
  // compress library defers matches from level 4 up, and this strategy
  // differs from the default only at levels 1 to 3, where it defers and the
  // default does not.  It is set anyway, as a statement of what this codec
  // wants rather than a reliance on where that library happens to draw the
  // line, and so that dropping to a fast level for speed would keep the
  // deferral rather than silently lose it.
  //
  // The strategy used to be spelled "filtered" and used to be measurably
  // different here - over six images and the published conformance suite it
  // was 9.0% smaller on a gradient, 1.25% smaller across PngSuite, and 9.3%
  // larger on a photograph, at about 3.5x the time.  None of that holds any
  // more.  Two things changed in the compress library: the strategy stopped
  // searching four times as deep as the default, which was measured to be
  // worth 0.1 points against deferral's 1.7; and the default started
  // deferring from level 4.  What is left is a name that says what it does.
  gs = gcomp_options_set_string(gopts, "deflate.strategy", "lazy");
  if (gs != GCOMP_OK) {
    gcomp_options_destroy(gopts);
    gimg_free(gimg_alloc_or_default(allocator), zlib_buf);
    gimg_free(gimg_alloc_or_default(allocator), raw);
    return GIMG_ERR_INTERNAL;
  }
  size_t zlib_len = 0;
  gs = gcomp_encode_buffer(gcomp_registry_default(), "zlib", gopts, raw,
      raw_size, zlib_buf, zlib_cap, &zlib_len);
  gcomp_options_destroy(gopts);
  gimg_free(gimg_alloc_or_default(allocator), raw);
  if (gs != GCOMP_OK) {
    gimg_free(gimg_alloc_or_default(allocator), zlib_buf);
    if (gs == GCOMP_ERR_MEMORY) {
      return GIMG_ERR_OOM;
    }
    if (gs == GCOMP_ERR_LIMIT) {
      return GIMG_ERR_LIMIT;
    }
    return GIMG_ERR_FORMAT;
  }
  *out_zlib = zlib_buf;
  *out_zlib_len = zlib_len;
  return GIMG_OK;
}

static GIMG_Result png_save_body(GIMG_Codec * codec, const GIMG_Doc * doc,
    GIMG_Stream * stream, const char * format_name,
    const GIMG_Save_Options * options, GIMG_Save_Report * report,
    void ** out_icc_copy) {
  (void)format_name;
  if (!codec || !doc || !stream || !report) {
    return GIMG_ERR_INTERNAL;
  }
  report->bytes_written = 0;
  // PNG 12.8 recommends choosing a filter per row; that is the default. A
  // caller may pin one instead, which is how the tests reach each of the five.
  // Checked here, before anything is allocated, so a refusal frees nothing.
  unsigned int filter_choice =
      options ? (unsigned int)options->png_filter : GIMG_PNG_FILTER_ADAPTIVE;
  if (filter_choice > GIMG_PNG_FILTER_PAETH) {
    return GIMG_ERR_UNSUPPORTED;
  }
  if (gimg_doc_item_count(doc) == 0) {
    return GIMG_ERR_FORMAT;
  }
  GIMG_Item * item = gimg_doc_item((GIMG_Doc *)doc, 0);
  if (!item) {
    return GIMG_ERR_INTERNAL;
  }
  // Prefer attached raster (e.g. load -> modify -> set_raster -> save); else decode
  // or raster from synthetic doc. When attached, doc owns it; when from decode we own.
  GIMG_Raster * raster = NULL;
  int raster_owned = 0;
  GIMG_Result r = GIMG_OK;
  raster = gimg_item_raster(item);
  if (raster) {
    raster_owned = 0;
  }
  else {
    r = gimg_item_decode(item, NULL, &raster);
    if (r == GIMG_OK && raster) {
      raster_owned = 1;
    }
    else if (r == GIMG_ERR_UNSUPPORTED) {
      return GIMG_ERR_FORMAT;
    }
    else {
      return r != GIMG_OK ? r : GIMG_ERR_FORMAT;
    }
  }
  // codec_private belongs to whichever codec loaded this document, and the type
  // is that codec's.  Saving a document that some other codec loaded - a JPEG
  // re-saved as PNG, say - used to cast that codec's state to this one's and
  // walk its ancillary-chunk list, reading unrelated fields as a pointer.
  // Fuzzing reached it in the seed corpus: the crash was a read through
  // 0x3000300030003.  The document records which codec owns the state, so
  // consult it.
  gimg_png_doc_state_t * state =
      (doc->loaded_by_codec == (struct GIMG_Codec *)codec)
      ? (gimg_png_doc_state_t *)doc->codec_private
      : NULL;
  // The colour is captured here because the raster does not live long enough
  // to be asked later: it is destroyed as soon as the image data is deflated,
  // and every chunk - the signature included - is written after that.  The
  // struct is values apart from the profile, which points into the raster, so
  // that one field is copied and owned by gimg_png_save, which frees it
  // however this returns.  Without the copy the iCCP branch read freed memory
  // and wrote whatever was in it into the file; the JPEG writer had the same
  // defect, and ASan found that one.
  GIMG_Color_Info color_info_for_save;
  const GIMG_Color_Info * rci = gimg_raster_color_info_const(raster);
  if (rci) {
    color_info_for_save = *rci;
    if (rci->icc_bytes && rci->icc_size > 0) {
      const GIMG_Allocator * icc_alloc = gimg_alloc_or_default(codec->allocator);
      void * copy = gimg_malloc(icc_alloc, rci->icc_size);
      if (!copy) {
        if (raster_owned) {
          gimg_raster_destroy(raster);
        }
        return GIMG_ERR_OOM;
      }
      memcpy(copy, rci->icc_bytes, rci->icc_size);
      *out_icc_copy = copy;
      color_info_for_save.icc_bytes = copy;
    }
  }
  else {
    gimg_color_info_default(&color_info_for_save);
  }
  uint8_t color_type = 0;
  uint8_t bit_depth = 0;
  bool use_palette = false;
  if (state && state->plte && state->plte_size > 0 &&
      state->ihdr.color_type == 3) {
    const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
    if (fmt && fmt->channel_model == GIMG_CHANNEL_RGBA &&
        fmt->channel_count == 4 && fmt->bits_per_channel[0] == 8) {
      use_palette = true;
      color_type = 3;
      bit_depth = state->ihdr.bit_depth;
      if (bit_depth != 1 && bit_depth != 2 && bit_depth != 4 &&
          bit_depth != 8) {
        bit_depth = 8;
      }
    }
  }
  // A tRNS the writer derives from the raster's alpha, when color type 2 can
  // carry it (PNG 11.3.2.1). Empty when the image needs no transparency, or
  // needs more than one transparent color and so gets an alpha channel.
  unsigned char derived_trns[6];
  size_t derived_trns_size = 0;
  if (!use_palette &&
      !gimg_png_raster_to_ihdr(raster, state, &color_type, &bit_depth,
          derived_trns, &derived_trns_size)) {
    if (raster_owned) {
      gimg_raster_destroy(raster);
    }
    return GIMG_ERR_UNSUPPORTED;
  }
  uint32_t width = gimg_raster_width(raster);
  uint32_t height = gimg_raster_height(raster);
  int do_interlaced = options && options->interlaced;
  size_t num_items = gimg_doc_item_count(doc);
  int is_apng = (num_items > 1);

  // The palette to write, which is the frame's own when it arrived with one.
  const gimg_png_doc_state_t * palette_state = use_palette ? state : NULL;
  gimg_png_doc_state_t built_state;
  unsigned char built_plte[GIMG_PNG_PLTE_MAX_ENTRIES * 3u];
  unsigned char built_trns[GIMG_PNG_PLTE_MAX_ENTRIES];

  // Encode frame 0 raster to zlib (used for IDAT or first APNG frame).
  unsigned char * zlib_buf = NULL;
  size_t zlib_len = 0;

  //
  // A palette for a raster that did not arrive with one (11.2.2).
  //
  // Only when it is lossless - 256 colors or fewer, so there is nothing to
  // choose - and only when it is the smaller file, which is measured rather
  // than guessed: both forms are encoded and the loser is discarded. DEFLATE
  // makes the arithmetic hard to predict, and a small image can spend more on
  // PLTE than it saves on pixels.
  //
  // Not attempted for an animation: every APNG frame must share one color
  // type, and a palette that suits frame 0 need not suit the rest.
  //
  bool try_palette = !use_palette && !is_apng &&
      !(options && options->png_palette == GIMG_PNG_PALETTE_NEVER);
  if (try_palette) {
    size_t built_plte_size = 0;
    size_t built_trns_size = 0;
    if (gimg_png_build_palette(
            raster, built_plte, &built_plte_size, built_trns,
            &built_trns_size) &&
        built_plte_size > 0) {
      if (state) {
        built_state = *state;
      }
      else {
        memset(&built_state, 0, sizeof(built_state));
      }
      built_state.plte = built_plte;
      built_state.plte_size = built_plte_size;
      built_state.trns = built_trns_size > 0 ? built_trns : NULL;
      built_state.trns_size = built_trns_size;

      uint8_t pal_depth =
          gimg_png_palette_bit_depth(built_plte_size / 3u);
      unsigned char * pal_buf = NULL;
      size_t pal_len = 0;
      GIMG_Result pr = gimg_png_raster_to_zlib(raster, 3, pal_depth,
          &built_state, do_interlaced, filter_choice, codec->allocator,
          &pal_buf, &pal_len);
      if (pr == GIMG_OK) {
        unsigned char * dir_buf = NULL;
        size_t dir_len = 0;
        GIMG_Result dr = gimg_png_raster_to_zlib(raster, color_type, bit_depth,
            NULL, do_interlaced, filter_choice, codec->allocator, &dir_buf,
            &dir_len);
        // Chunk overhead is 12 bytes each: length, type and CRC (5.3).
        size_t pal_total = pal_len + 12u + built_plte_size +
            (built_trns_size > 0 ? 12u + built_trns_size : 0u);
        size_t dir_total =
            dir_len + (derived_trns_size > 0 ? 12u + derived_trns_size : 0u);
        if (dr != GIMG_OK || pal_total < dir_total) {
          gimg_free(gimg_alloc_or_default(codec->allocator), dir_buf);
          color_type = 3;
          bit_depth = pal_depth;
          use_palette = true;
          palette_state = &built_state;
          derived_trns_size = 0;
          zlib_buf = pal_buf;
          zlib_len = pal_len;
        }
        else {
          gimg_free(gimg_alloc_or_default(codec->allocator), pal_buf);
          zlib_buf = dir_buf;
          zlib_len = dir_len;
        }
      }
    }
  }

  if (!zlib_buf) {
    r = gimg_png_raster_to_zlib(raster, color_type, bit_depth, palette_state,
        do_interlaced, filter_choice, codec->allocator, &zlib_buf, &zlib_len);
  }
  if (raster_owned) {
    gimg_raster_destroy(raster);
  }
  raster = NULL;
  if (r != GIMG_OK) {
    return r;
  }

  // Write signature.
  size_t n = 0;
  r = gimg_stream_write(stream, gimg_png_signature, GIMG_PNG_SIGNATURE_LEN, &n);
  if (r != GIMG_OK || n != GIMG_PNG_SIGNATURE_LEN) {
    gimg_free(gimg_alloc_or_default(codec->allocator), zlib_buf);
    return r != GIMG_OK ? r : GIMG_ERR_IO;
  }
  report->bytes_written += GIMG_PNG_SIGNATURE_LEN;

  // IHDR
  unsigned char ihdr[GIMG_PNG_IHDR_LEN];
  gimg_png_build_ihdr(ihdr, width, height, bit_depth, color_type,
      (uint8_t)(do_interlaced ? 1 : 0));
  r = gimg_png_write_chunk(stream, GIMG_PNG_IHDR, ihdr, sizeof(ihdr));
  if (r != GIMG_OK) {
    gimg_free(gimg_alloc_or_default(codec->allocator), zlib_buf);
    return r;
  }
  report->bytes_written += 8 + GIMG_PNG_IHDR_LEN + 4;

  // Ancillary before IDAT (per metadata policy).
  //
  // PNG 5.6, Table 5.3: bKGD and hIST come *after* PLTE and before IDAT.
  //
  // They sit in the ancillary list in the order the file had them, which for a
  // palette image is already after its PLTE - but this writer emits PLTE
  // itself, after the whole list, so writing them where they are found puts
  // them in front of it. libpng then says "bKGD: out of place" and, for the
  // histogram, "hIST: invalid", because it cannot check the entry count
  // against a palette it has not seen yet - so the chunk is lost.
  //
  // Every other ancillary type this writer emits is allowed before PLTE:
  // cHRM, gAMA, iCCP, sBIT, sRGB, cICP, mDCv and cLLi must be, pHYs, sPLT and
  // eXIf only have to precede IDAT, and the text and time chunks may appear
  // anywhere.
  //
  // So these two are held back and written after the palette. Eight is more
  // than a conforming file can have - one of each - and the cap only matters
  // for a file that already broke that rule.
  //
  typedef struct {
    gimg_png_chunk_type_t type;
    const unsigned char * payload;
    size_t size;
    unsigned char inline_buf[6]; ///< Holds a rewritten bKGD, which is short.
  } gimg_png_deferred_t;
  gimg_png_deferred_t deferred[8];
  size_t deferred_count = 0;

  GIMG_Meta_Policy policy =
      options ? options->metadata_policy : GIMG_META_PRESERVE_ALL;

  if (policy == GIMG_META_KEEP_COMMON_ONLY) {
    // A resolution is common metadata, so this policy keeps it (11.3.4.3).
    GIMG_Meta_Common * common_meta = gimg_doc_meta_common(doc);
    if (common_meta) {
      uint32_t x_dpi = 0, y_dpi = 0;
      gimg_meta_common_dpi(common_meta, &x_dpi, &y_dpi);
      if (x_dpi > 0 && y_dpi > 0) {
        unsigned char phys[9];
        gimg_png_build_phys(phys, gimg_dpi_to_pixels_per_meter(x_dpi),
            gimg_dpi_to_pixels_per_meter(y_dpi));
        r = gimg_png_write_chunk(stream, GIMG_PNG_pHYs, phys, sizeof(phys));
        if (r != GIMG_OK) {
          gimg_free(gimg_alloc_or_default(codec->allocator), zlib_buf);
          return r;
        }
        report->bytes_written += 8 + sizeof(phys) + 4;
      }
    }
    // Emit only color-related metadata from raster's color info.
    r = gimg_png_write_color_from_info(stream, &color_info_for_save,
        gimg_alloc_or_default(codec->allocator), report);
    if (r != GIMG_OK) {
      gimg_free(gimg_alloc_or_default(codec->allocator), zlib_buf);
      return r;
    }
  }
  else if (policy != GIMG_META_DROP_ALL && policy != GIMG_META_KEEP_RAW_ONLY) {
    const GIMG_Allocator * alloc = gimg_alloc_or_default(codec->allocator);
    GIMG_Meta_Common * meta_common = gimg_doc_meta_common(doc);
    bool have_description_or_comment_from_ancillary = false;
    bool have_phys_from_ancillary = false;
    bool have_color_from_ancillary = false;
    // PRESERVE_ALL, STRIP_GPS, or NORMALIZE_EXIF: emit ancillary from state.
    if (state && state->ancillary) {
      for (size_t i = 0; i < state->ancillary_count; i++) {
        gimg_png_chunk_type_t t = state->ancillary[i].type;
        if (t == GIMG_PNG_PLTE || t == GIMG_PNG_tRNS) {
          continue;
        }
        if (gimg_png_chunk_is_color(t)) {
          have_color_from_ancillary = true;
        }
        if (t == GIMG_PNG_IHDR || t == GIMG_PNG_IDAT || t == GIMG_PNG_IEND) {
          continue;
        }
        if (policy == GIMG_META_STRIP_GPS &&
            (t == GIMG_PNG_tEXt || t == GIMG_PNG_zTXt || t == GIMG_PNG_iTXt) &&
            state->ancillary[i].payload &&
            state->ancillary[i].payload_size > 0) {
          if (gimg_png_text_keyword_is_gps(state->ancillary[i].payload,
                  state->ancillary[i].payload_size)) {
            continue;
          }
        }
        if ((t == GIMG_PNG_tEXt || t == GIMG_PNG_zTXt || t == GIMG_PNG_iTXt) &&
            state->ancillary[i].payload && state->ancillary[i].payload_size > 0 &&
            gimg_png_text_keyword_is_description_or_comment(
                state->ancillary[i].payload, state->ancillary[i].payload_size)) {
          have_description_or_comment_from_ancillary = true;
        }
        if (t == GIMG_PNG_pHYs) {
          have_phys_from_ancillary = true;
        }
        const void * chunk_payload = state->ancillary[i].payload;
        size_t chunk_size = state->ancillary[i].payload_size;
        // bKGD, sBIT and hIST are laid out according to the color type, and
        // the one being written is not always the one the frame arrived as.
        unsigned char retargeted[6];
        size_t retargeted_size = 0;
        gimg_png_retarget_t what = gimg_png_retarget_ancillary(t,
            state->ancillary[i].payload, state->ancillary[i].payload_size,
            state, color_type, bit_depth, retargeted, &retargeted_size);
        if (what == GIMG_PNG_RETARGET_DROP) {
          continue;
        }
        if (what == GIMG_PNG_RETARGET_REPLACE) {
          chunk_payload = retargeted;
          chunk_size = retargeted_size;
        }
        void * modified = NULL;
        size_t modified_size = 0;
        if (t == GIMG_PNG_eXIf && chunk_payload && chunk_size > 0) {
          if (policy == GIMG_META_STRIP_GPS) {
            if (gimg_exif_strip_gps(codec->allocator, chunk_payload,
                    chunk_size, &modified, &modified_size) == GIMG_OK) {
              chunk_payload = modified;
              chunk_size = modified_size;
            }
          }
          else if (policy == GIMG_META_NORMALIZE_EXIF) {
            if (gimg_exif_normalize(codec->allocator, chunk_payload,
                    chunk_size, &modified, &modified_size) == GIMG_OK) {
              chunk_payload = modified;
              chunk_size = modified_size;
            }
          }
        }
        if ((t == GIMG_PNG_bKGD || t == GIMG_PNG_hIST) &&
            deferred_count < sizeof(deferred) / sizeof(deferred[0])) {
          gimg_png_deferred_t * d = &deferred[deferred_count++];
          d->type = t;
          d->size = chunk_size;
          if (chunk_payload == retargeted && chunk_size <= sizeof(d->inline_buf)) {
            // The rewritten payload lives in a buffer that goes out of scope
            // with this iteration, so it is copied rather than pointed at.
            memcpy(d->inline_buf, chunk_payload, chunk_size);
            d->payload = d->inline_buf;
          }
          else {
            d->payload = (const unsigned char *)chunk_payload;
          }
          if (modified) {
            gimg_free(alloc, modified);
          }
          continue;
        }
        r = gimg_png_write_chunk(stream, t, chunk_payload, chunk_size);
        if (modified) {
          gimg_free(alloc, modified);
        }
        if (r != GIMG_OK) {
          gimg_free(alloc, zlib_buf);
          return r;
        }
        report->bytes_written += 8 + chunk_size + 4;
      }
    }
    // A color the raster carries but the file did not.  The chunk the file
    // came with wins, the same way the resolution below does; this is only a
    // way of not losing a color space that arrived from somewhere else.
    //
    // Without it a document that did not arrive as a PNG had no ancillary
    // chunks to preserve and so lost its color entirely: a BMP with a V5
    // embedded ICC profile, saved as a PNG, came out untagged, and the profile
    // was read only to be dropped.  The machinery to write one was already
    // here and reachable from GIMG_META_KEEP_COMMON_ONLY alone.
    if (!have_color_from_ancillary) {
      r = gimg_png_write_color_from_info(
          stream, &color_info_for_save, alloc, report);
      if (r != GIMG_OK) {
        gimg_free(alloc, zlib_buf);
        return r;
      }
    }

    // A resolution the document carries but the file did not: pHYs
    // (11.3.4.3). The chunk the file came with wins, the same way the
    // description does - it is what was actually there, and this is only a way
    // of not losing a resolution that arrived from somewhere else, such as a
    // JPEG's JFIF density.
    if (!have_phys_from_ancillary && meta_common) {
      uint32_t x_dpi = 0, y_dpi = 0;
      gimg_meta_common_dpi(meta_common, &x_dpi, &y_dpi);
      if (x_dpi > 0 && y_dpi > 0) {
        unsigned char phys[9];
        gimg_png_build_phys(phys, gimg_dpi_to_pixels_per_meter(x_dpi),
            gimg_dpi_to_pixels_per_meter(y_dpi));
        r = gimg_png_write_chunk(stream, GIMG_PNG_pHYs, phys, sizeof(phys));
        if (r != GIMG_OK) {
          gimg_free(alloc, zlib_buf);
          return r;
        }
        report->bytes_written += 8 + sizeof(phys) + 4;
      }
    }
    // If meta_common has description and we did not write one from ancillary,
    // emit one tEXt "Description\0" + description.
    if (!have_description_or_comment_from_ancillary && meta_common) {
      const char * desc = gimg_meta_common_description(meta_common);
      if (desc) {
        size_t dlen = strlen(desc);
        size_t kw_len = 11;  // "Description"
        if (dlen <= 0x7FFFFFFFu - kw_len - 1u) {
          size_t total = kw_len + 1u + dlen;
          unsigned char * tEXt_payload =
              (unsigned char *)gimg_malloc(alloc, total);
          if (tEXt_payload) {
            memcpy(tEXt_payload, "Description", 11);
            tEXt_payload[11] = 0;
            memcpy(tEXt_payload + 12, desc, dlen);
            r = gimg_png_write_chunk(
                stream, GIMG_PNG_tEXt, tEXt_payload, total);
            gimg_free(alloc, tEXt_payload);
            if (r == GIMG_OK) {
              report->bytes_written += 8 + total + 4;
            }
            else {
              gimg_free(alloc, zlib_buf);
              return r;
            }
          }
        }
      }
    }
    // eXIf from doc meta_raw when doc was not loaded from PNG (no state).
    if (!state) {
      GIMG_Meta_Raw * meta_raw = gimg_doc_meta_raw(doc);
      if (meta_raw) {
        size_t exif_size = 0;
        r = gimg_meta_raw_get(
            meta_raw, "png", (uint32_t)GIMG_PNG_eXIf, NULL, &exif_size);
        if (r == GIMG_OK && exif_size > 0) {
          unsigned char * exif_buf =
              (unsigned char *)gimg_malloc(alloc, exif_size);
          if (exif_buf) {
            r = gimg_meta_raw_get(
                meta_raw, "png", (uint32_t)GIMG_PNG_eXIf, exif_buf, &exif_size);
            if (r == GIMG_OK) {
              void * to_write = exif_buf;
              size_t to_write_size = exif_size;
              void * modified = NULL;
              size_t modified_size = 0;
              if (policy == GIMG_META_STRIP_GPS) {
                if (gimg_exif_strip_gps(codec->allocator, exif_buf, exif_size,
                        &modified, &modified_size) == GIMG_OK) {
                  to_write = modified;
                  to_write_size = modified_size;
                }
              }
              else if (policy == GIMG_META_NORMALIZE_EXIF) {
                if (gimg_exif_normalize(codec->allocator, exif_buf, exif_size,
                        &modified, &modified_size) == GIMG_OK) {
                  to_write = modified;
                  to_write_size = modified_size;
                }
              }
              r = gimg_png_write_chunk(
                  stream, GIMG_PNG_eXIf, to_write, to_write_size);
              report->bytes_written += 8 + to_write_size + 4;
              if (modified) {
                gimg_free(alloc, modified);
              }
            }
            gimg_free(alloc, exif_buf);
          }
          if (r != GIMG_OK) {
            gimg_free(alloc, zlib_buf);
            return r;
          }
        }
      }
    }
  }
  else if (policy == GIMG_META_KEEP_RAW_ONLY && state && state->ancillary) {
    // Emit only ancillary chunks that are not known semantic (raw/unknown
    // only).
    for (size_t i = 0; i < state->ancillary_count; i++) {
      gimg_png_chunk_type_t t = state->ancillary[i].type;
      if (t == GIMG_PNG_PLTE || t == GIMG_PNG_tRNS) {
        continue;
      }
      if (t == GIMG_PNG_IHDR || t == GIMG_PNG_IDAT || t == GIMG_PNG_IEND) {
        continue;
      }
      if (gimg_png_chunk_is_known_semantic(t)) {
        continue;
      }
      // Same color-type dependence as above: these three reach this policy
      // too, because none of them is semantic metadata in the sense
      // gimg_png_chunk_is_known_semantic() means.
      const void * raw_payload = state->ancillary[i].payload;
      size_t raw_size = state->ancillary[i].payload_size;
      unsigned char retargeted[6];
      size_t retargeted_size = 0;
      gimg_png_retarget_t what = gimg_png_retarget_ancillary(t,
          state->ancillary[i].payload, state->ancillary[i].payload_size, state,
          color_type, bit_depth, retargeted, &retargeted_size);
      if (what == GIMG_PNG_RETARGET_DROP) {
        continue;
      }
      if (what == GIMG_PNG_RETARGET_REPLACE) {
        raw_payload = retargeted;
        raw_size = retargeted_size;
      }
      if ((t == GIMG_PNG_bKGD || t == GIMG_PNG_hIST) &&
          deferred_count < sizeof(deferred) / sizeof(deferred[0])) {
        gimg_png_deferred_t * d = &deferred[deferred_count++];
        d->type = t;
        d->size = raw_size;
        if (raw_payload == retargeted && raw_size <= sizeof(d->inline_buf)) {
          memcpy(d->inline_buf, raw_payload, raw_size);
          d->payload = d->inline_buf;
        }
        else {
          d->payload = (const unsigned char *)raw_payload;
        }
        continue;
      }
      r = gimg_png_write_chunk(
          stream, state->ancillary[i].type, raw_payload, raw_size);
      if (r != GIMG_OK) {
        gimg_free(gimg_alloc_or_default(codec->allocator), zlib_buf);
        return r;
      }
      report->bytes_written += 8 + raw_size + 4;
    }
  }
  // DROP_ALL: no ancillary (already skipped above).

  // PNG 11.2.2 also lets a truecolor frame carry PLTE as a suggested palette.
  // It plays no part in decoding, but it is content the file came with, so the
  // policies that keep what was there keep it too.
  if (color_type != 3 && state && state->plte && state->plte_size > 0 &&
      state->plte_is_suggested && policy != GIMG_META_DROP_ALL &&
      policy != GIMG_META_KEEP_COMMON_ONLY) {
    r = gimg_png_write_chunk(
        stream, GIMG_PNG_PLTE, state->plte, state->plte_size);
    if (r != GIMG_OK) {
      gimg_free(gimg_alloc_or_default(codec->allocator), zlib_buf);
      return r;
    }
    report->bytes_written += 8 + state->plte_size + 4;
  }

  // A truecolor image whose alpha is one fully transparent color keeps it in
  // tRNS rather than growing an alpha channel (PNG 11.3.2.1). Written whatever
  // the metadata policy: it is part of the image, not metadata about it.
  if ((color_type == 2 || color_type == 0) && derived_trns_size > 0) {
    r = gimg_png_write_chunk(
        stream, GIMG_PNG_tRNS, derived_trns, derived_trns_size);
    if (r != GIMG_OK) {
      gimg_free(gimg_alloc_or_default(codec->allocator), zlib_buf);
      return r;
    }
    report->bytes_written += 8 + derived_trns_size + 4;
  }

  // Palette: PLTE and tRNS before IDAT per PNG spec. This is the palette that
  // was actually used - the frame's own, or one built for it here.
  if (color_type == 3 && palette_state && palette_state->plte &&
      palette_state->plte_size > 0) {
    r = gimg_png_write_chunk(
        stream, GIMG_PNG_PLTE, palette_state->plte, palette_state->plte_size);
    if (r != GIMG_OK) {
      gimg_free(gimg_alloc_or_default(codec->allocator), zlib_buf);
      return r;
    }
    report->bytes_written += 8 + palette_state->plte_size + 4;
    if (palette_state->trns && palette_state->trns_size > 0) {
      r = gimg_png_write_chunk(
          stream, GIMG_PNG_tRNS, palette_state->trns, palette_state->trns_size);
      if (r != GIMG_OK) {
        gimg_free(gimg_alloc_or_default(codec->allocator), zlib_buf);
        return r;
      }
      report->bytes_written += 8 + palette_state->trns_size + 4;
    }
  }

  // The chunks held back above, now that PLTE is behind them (5.6, Table 5.3).
  for (size_t i = 0; i < deferred_count; i++) {
    r = gimg_png_write_chunk(
        stream, deferred[i].type, deferred[i].payload, deferred[i].size);
    if (r != GIMG_OK) {
      gimg_free(gimg_alloc_or_default(codec->allocator), zlib_buf);
      return r;
    }
    report->bytes_written += 8 + deferred[i].size + 4;
  }

  // APNG: acTL (num_frames, num_plays) before first fcTL per spec.
  if (is_apng) {
    uint32_t num_plays = (state && state->is_apng) ? state->num_plays : 0u;
    unsigned char actl[GIMG_PNG_acTL_LEN];
    gimg_png_build_actl(actl, (uint32_t)num_items, num_plays);
    r = gimg_png_write_chunk(stream, GIMG_PNG_acTL, actl, sizeof(actl));
    if (r != GIMG_OK) {
      gimg_free(gimg_alloc_or_default(codec->allocator), zlib_buf);
      return r;
    }
    report->bytes_written += 8 + GIMG_PNG_acTL_LEN + 4;
  }

  // fcTL for frame 0 (APNG only).
  if (is_apng) {
    GIMG_Item * frame_item = gimg_doc_item((GIMG_Doc *)doc, 0);
    uint16_t delay_num = 0, delay_den = 0;
    gimg_item_frame_delay(frame_item, &delay_num, &delay_den);
    if (delay_den == 0) {
      delay_den = 100;
    }
    uint8_t dispose_op = (uint8_t)gimg_item_dispose_op(frame_item);
    uint8_t blend_op = (uint8_t)gimg_item_blend_op(frame_item);
    unsigned char fctl[GIMG_PNG_fcTL_LEN];
    gimg_png_build_fctl(fctl, 0u, width, height, 0u, 0u, delay_num, delay_den,
        dispose_op, blend_op);
    r = gimg_png_write_chunk(stream, GIMG_PNG_fcTL, fctl, sizeof(fctl));
    if (r != GIMG_OK) {
      gimg_free(gimg_alloc_or_default(codec->allocator), zlib_buf);
      return r;
    }
    report->bytes_written += 8 + GIMG_PNG_fcTL_LEN + 4;
  }

  // IDAT (frame 0 image data; single or multiple chunks when > 32 KiB).
  {
    size_t idat_chunk_max = GIMG_PNG_IDAT_CHUNK_MAX;
    size_t idat_offset = 0;
    while (idat_offset < zlib_len) {
      size_t chunk_len = zlib_len - idat_offset;
      if (chunk_len > idat_chunk_max) {
        chunk_len = idat_chunk_max;
      }
      r = gimg_png_write_chunk(
          stream, GIMG_PNG_IDAT, zlib_buf + idat_offset, chunk_len);
      if (r != GIMG_OK) {
        gimg_free(gimg_alloc_or_default(codec->allocator), zlib_buf);
        return r;
      }
      report->bytes_written += 8 + chunk_len + 4;
      idat_offset += chunk_len;
    }
  }
  gimg_free(gimg_alloc_or_default(codec->allocator), zlib_buf);
  zlib_buf = NULL;

  // APNG: fcTL + fdAT for frames 1 .. N-1. Sequence numbers are a single
  // increasing run: fcTL 0, fcTL 1, fdAT 2, fdAT 3, ... (see APNG spec).
  for (size_t frame_index = 1; frame_index < num_items && is_apng;
       frame_index++) {
    GIMG_Item * frame_item = gimg_doc_item((GIMG_Doc *)doc, frame_index);
    if (!frame_item) {
      return GIMG_ERR_INTERNAL;
    }
    GIMG_Raster * frame_raster = NULL;
    int frame_raster_owned = 0;
    frame_raster = gimg_item_raster(frame_item);
    if (frame_raster) {
      frame_raster_owned = 0;
    }
    else {
      r = gimg_item_decode(frame_item, NULL, &frame_raster);
      if (r == GIMG_OK && frame_raster) {
        frame_raster_owned = 1;
      }
      else if (r == GIMG_ERR_UNSUPPORTED) {
        return GIMG_ERR_FORMAT;
      }
      else {
        return r != GIMG_OK ? r : GIMG_ERR_FORMAT;
      }
    }
    if (gimg_raster_width(frame_raster) != width ||
        gimg_raster_height(frame_raster) != height) {
      if (frame_raster_owned) {
        gimg_raster_destroy(frame_raster);
      }
      return GIMG_ERR_FORMAT;
    }
    if (!use_palette) {
      uint8_t ct = 0;
      uint8_t bd = 0;
      unsigned char frame_trns[6];
      size_t frame_trns_size = 0;
      if (!gimg_png_raster_to_ihdr(
              frame_raster, state, &ct, &bd, frame_trns, &frame_trns_size) ||
          ct != color_type || bd != bit_depth) {
        if (frame_raster_owned) {
          gimg_raster_destroy(frame_raster);
        }
        return GIMG_ERR_FORMAT;
      }
    }
    uint16_t delay_num = 0, delay_den = 0;
    gimg_item_frame_delay(frame_item, &delay_num, &delay_den);
    if (delay_den == 0) {
      delay_den = 100;
    }
    uint8_t dispose_op = (uint8_t)gimg_item_dispose_op(frame_item);
    uint8_t blend_op = (uint8_t)gimg_item_blend_op(frame_item);
    // APNG: one global sequence (no duplicates). fcTL(0), fcTL(1), fdAT(2),
    // fcTL(3), fdAT(4), ... so fcTL for frame_index has seq 2*frame_index-1.
    uint32_t fctl_sequence = (uint32_t)(2u * frame_index - 1u);
    unsigned char fctl[GIMG_PNG_fcTL_LEN];
    gimg_png_build_fctl(fctl, fctl_sequence, width, height, 0u, 0u,
        delay_num, delay_den, dispose_op, blend_op);
    r = gimg_png_write_chunk(stream, GIMG_PNG_fcTL, fctl, sizeof(fctl));
    if (r != GIMG_OK) {
      if (frame_raster_owned) {
        gimg_raster_destroy(frame_raster);
      }
      return r;
    }
    report->bytes_written += 8 + GIMG_PNG_fcTL_LEN + 4;

    unsigned char * frame_zlib = NULL;
    size_t frame_zlib_len = 0;
    r = gimg_png_raster_to_zlib(frame_raster, color_type, bit_depth,
        use_palette ? state : NULL, do_interlaced, filter_choice,
        codec->allocator, &frame_zlib, &frame_zlib_len);
    if (frame_raster_owned) {
      gimg_raster_destroy(frame_raster);
    }
    if (r != GIMG_OK) {
      return r;
    }
    // fdAT: first chunk has sequence 2*frame_index, then increment (APNG).
    uint32_t fdat_sequence = (uint32_t)(2u * frame_index);
    size_t fdat_chunk_max = GIMG_PNG_IDAT_CHUNK_MAX;
    size_t fdat_offset = 0;
    while (fdat_offset < frame_zlib_len) {
      size_t frag_len = frame_zlib_len - fdat_offset;
      if (frag_len > fdat_chunk_max) {
        frag_len = fdat_chunk_max;
      }
      size_t payload_len = 4u + frag_len;
      unsigned char * fdat_payload = (unsigned char *)gimg_malloc(
          gimg_alloc_or_default(codec->allocator), payload_len);
      if (!fdat_payload) {
        gimg_free(gimg_alloc_or_default(codec->allocator), frame_zlib);
        return GIMG_ERR_OOM;
      }
      fdat_payload[0] = (unsigned char)(fdat_sequence >> 24);
      fdat_payload[1] = (unsigned char)(fdat_sequence >> 16);
      fdat_payload[2] = (unsigned char)(fdat_sequence >> 8);
      fdat_payload[3] = (unsigned char)(fdat_sequence & 0xFFu);
      memcpy(fdat_payload + 4, frame_zlib + fdat_offset, frag_len);
      r = gimg_png_write_chunk(
          stream, GIMG_PNG_fdAT, fdat_payload, payload_len);
      gimg_free(gimg_alloc_or_default(codec->allocator), fdat_payload);
      if (r != GIMG_OK) {
        gimg_free(gimg_alloc_or_default(codec->allocator), frame_zlib);
        return r;
      }
      report->bytes_written += 8 + payload_len + 4;
      fdat_offset += frag_len;
      fdat_sequence++;
    }
    gimg_free(gimg_alloc_or_default(codec->allocator), frame_zlib);
  }

  // IEND
  r = gimg_png_write_chunk(stream, GIMG_PNG_IEND, NULL, 0);
  if (r != GIMG_OK) {
    return r;
  }
  report->bytes_written += 12;
  return GIMG_OK;
}

/**
 * Save a document as a PNG.
 *
 * A thin owner around png_save_body: the colour the chunk writer states has
 * to outlive the raster it came from, because the raster is destroyed as soon
 * as the image data is deflated and every chunk is written after that.  The
 * body hands back the one allocation behind the captured profile, which is
 * freed here however the body returned.
 */
GIMG_Result gimg_png_save(GIMG_Codec * codec, const GIMG_Doc * doc,
    GIMG_Stream * stream, const char * format_name,
    const GIMG_Save_Options * options, GIMG_Save_Report * report) {
  void * icc_copy = NULL;
  GIMG_Result r =
      png_save_body(codec, doc, stream, format_name, options, report,
          &icc_copy);
  gimg_free(codec ? gimg_alloc_or_default(codec->allocator) : NULL, icc_copy);
  return r;
}

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
 * BMP color management: the BITMAPV4HEADER and BITMAPV5HEADER color fields.
 *
 * --- Internal algorithms and design ---
 *
 * A V4 header adds a color space type, three CIE endpoints and a gamma per
 * channel; a V5 header adds a rendering intent and, for PROFILE_EMBEDDED, an
 * ICC profile stored elsewhere in the file.  All of it used to be skipped, so
 * a V5 file with a real profile decoded as untagged and lost it.
 *
 * What is translated is only what GIMG_Color_Info can hold, which is the same
 * rule the PNG codec applies to cICP: a color space this model cannot state
 * is left unknown rather than rounded to the nearest thing it can say, since
 * that would be a claim about the pixels the file did not make.
 *
 * LCS_CALIBRATED_RGB means the endpoints and gammas describe the space.  The
 * endpoints are declared as CIEXYZ but every writer in reach puts xyY
 * chromaticities there instead, normalized so the triple sums to one -
 * bmpsuite's g/pal8v4.bmp carries exactly the sRGB primaries that way.  They
 * are matched against the two gamuts this model names and left unknown
 * otherwise.
 *
 * PROFILE_LINKED is deliberately not followed.  The "profile" is a file path,
 * and opening a path an image file names is acting on data: it is the shape
 * of a directory traversal, and on some platforms of a network fetch.  The
 * color is left unknown and a diagnostic says why.
 */

#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/color.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/stream.h>
#include <string.h>

#include "../../core/alloc_internal.h"
#include "bmp_internal.h"

/** @name bV5Intent values (wingdi.h LCS_GM_*).
 * @{ */
#define GIMG_BMP_LCS_GM_BUSINESS 1u         ///< Saturation.
#define GIMG_BMP_LCS_GM_GRAPHICS 2u         ///< Relative colorimetric.
#define GIMG_BMP_LCS_GM_IMAGES 4u           ///< Perceptual.
#define GIMG_BMP_LCS_GM_ABS_COLORIMETRIC 8u ///< Absolute colorimetric.
/** @} */

/**
 * @name Chromaticity comparison
 *
 * A chromaticity is an FXPT2DOT30 - a signed fixed-point value with 30
 * fractional bits - compared at a thousandth, which is finer than any of
 * these tables is quoted to and coarser than the rounding of writing one out.
 * @{
 */
#define GIMG_BMP_FXPT2DOT30_ONE (INT32_C(1) << 30) ///< 1.0 in FXPT2DOT30.
#define GIMG_BMP_CHROMA_TOLERANCE (GIMG_BMP_FXPT2DOT30_ONE / 1000) ///< 0.001.
/** @} */

/** A gamut this model can name, as the nine endpoint values would spell it. */
typedef struct {
  GIMG_Primaries primaries; ///< What GIMG_Color_Info calls this gamut.
  double xy[6]; ///< Red x, red y, green x, green y, blue x, blue y.
} bmp_gamut_t;

static const bmp_gamut_t bmp_known_gamuts[] = {
    // ITU-R BT.709, which sRGB shares.
    {GIMG_PRIMARIES_SRGB, {0.6400, 0.3300, 0.3000, 0.6000, 0.1500, 0.0600}},
    // Adobe RGB (1998): the same red and blue, a wider green.
    {GIMG_PRIMARIES_ADOBE_RGB,
        {0.6400, 0.3300, 0.2100, 0.7100, 0.1500, 0.0600}},
};

static bool bmp_chroma_matches(int32_t value, double expected) {
  double scaled = expected * (double)GIMG_BMP_FXPT2DOT30_ONE;
  double delta = (double)value - scaled;
  if (delta < 0) {
    delta = -delta;
  }
  return delta <= (double)GIMG_BMP_CHROMA_TOLERANCE;
}

/**
 * Name the gamut the nine endpoint values describe.
 *
 * The fields are declared CIEXYZ and written as xyY chromaticities, so only
 * the first two of each triple say anything; the third is what is left after
 * the other two, and is checked only to the extent that a triple summing to
 * something far from one is not chromaticities at all and is refused.
 */
static GIMG_Primaries bmp_gamut_from_endpoints(const int32_t endpoints[9]) {
  for (unsigned int channel = 0; channel < 3; channel++) {
    int64_t sum = (int64_t)endpoints[channel * 3] +
        (int64_t)endpoints[(channel * 3) + 1] +
        (int64_t)endpoints[(channel * 3) + 2];
    int64_t off = sum - GIMG_BMP_FXPT2DOT30_ONE;
    if (off < 0) {
      off = -off;
    }
    if (off > (int64_t)GIMG_BMP_CHROMA_TOLERANCE * 10) {
      return GIMG_PRIMARIES_UNKNOWN;
    }
  }
  for (size_t i = 0; i < GIMG_ARRAY_SIZE(bmp_known_gamuts); i++) {
    const bmp_gamut_t * gamut = &bmp_known_gamuts[i];
    if (bmp_chroma_matches(endpoints[0], gamut->xy[0]) &&
        bmp_chroma_matches(endpoints[1], gamut->xy[1]) &&
        bmp_chroma_matches(endpoints[3], gamut->xy[2]) &&
        bmp_chroma_matches(endpoints[4], gamut->xy[3]) &&
        bmp_chroma_matches(endpoints[6], gamut->xy[4]) &&
        bmp_chroma_matches(endpoints[7], gamut->xy[5])) {
      return gamut->primaries;
    }
  }
  return GIMG_PRIMARIES_UNKNOWN;
}

/** Map a rendering intent back onto bV5Intent. */
static uint32_t bmp_intent_to_v5(GIMG_Rendering_Intent intent) {
  switch (intent) {
    case GIMG_INTENT_SATURATION:
      return GIMG_BMP_LCS_GM_BUSINESS;
    case GIMG_INTENT_RELATIVE_COLORIMETRIC:
      return GIMG_BMP_LCS_GM_GRAPHICS;
    case GIMG_INTENT_ABSOLUTE_COLORIMETRIC:
      return GIMG_BMP_LCS_GM_ABS_COLORIMETRIC;
    case GIMG_INTENT_PERCEPTUAL:
    default:
      return GIMG_BMP_LCS_GM_IMAGES;
  }
}

/** Map bV5Intent onto the rendering intents this model names. */
static GIMG_Rendering_Intent bmp_intent_from_v5(uint32_t intent) {
  switch (intent) {
    case GIMG_BMP_LCS_GM_BUSINESS:
      return GIMG_INTENT_SATURATION;
    case GIMG_BMP_LCS_GM_GRAPHICS:
      return GIMG_INTENT_RELATIVE_COLORIMETRIC;
    case GIMG_BMP_LCS_GM_ABS_COLORIMETRIC:
      return GIMG_INTENT_ABSOLUTE_COLORIMETRIC;
    case GIMG_BMP_LCS_GM_IMAGES:
    default:
      // Perceptual is the enum's zero, so an intent the header did not state
      // and one it stated as LCS_GM_IMAGES are the same value here.  ICC
      // treats perceptual as the default too, so nothing is lost by it, but
      // the two cannot be told apart through this struct.
      return GIMG_INTENT_PERCEPTUAL;
  }
}

void gimg_bmp_color_from_header(
    const gimg_bmp_header_t * header, GIMG_Color_Info * out_info) {
  gimg_color_info_default(out_info);
  if (header->header_size < GIMG_BMP_V4HEADER_SIZE || header->os2_v2) {
    // Before V4 there is nothing in the header about color at all.  An
    // untagged BMP is overwhelmingly an sRGB one, but the file does not say
    // so and neither will this.
    return;
  }

  switch (header->cs_type) {
    case GIMG_BMP_LCS_sRGB:
    case GIMG_BMP_LCS_WINDOWS_COLOR_SPACE:
      out_info->primaries = GIMG_PRIMARIES_SRGB;
      out_info->white_point = GIMG_PRIMARIES_SRGB;
      out_info->transfer = GIMG_TRANSFER_SRGB;
      break;

    case GIMG_BMP_LCS_CALIBRATED_RGB: {
      GIMG_Primaries gamut = bmp_gamut_from_endpoints(header->endpoints);
      if (gamut != GIMG_PRIMARIES_UNKNOWN) {
        out_info->primaries = gamut;
        out_info->white_point = gamut;
      }
      // One transfer function, so three disagreeing gammas describe a space
      // this cannot hold and are left unsaid rather than averaged.
      if (header->gamma[0] && header->gamma[0] == header->gamma[1] &&
          header->gamma[1] == header->gamma[2]) {
        double gamma = (double)header->gamma[0] / 65536.0;
        if (gamma > 0.999 && gamma < 1.001) {
          out_info->transfer = GIMG_TRANSFER_LINEAR;
        }
        else {
          out_info->transfer = GIMG_TRANSFER_GAMMA;
          out_info->gamma_value = gamma;
        }
      }
      break;
    }

    case GIMG_BMP_PROFILE_EMBEDDED:
      // The profile itself is attached by the loader, which is where the
      // stream is; the intent below still applies.
      break;

    case GIMG_BMP_PROFILE_LINKED:
    default:
      // PROFILE_LINKED names a file, which is not followed - see the file
      // comment.  An unknown type is left alone for the same reason a cICP
      // this model cannot state is: saying nothing beats saying the wrong
      // thing.
      return;
  }

  if (header->header_size >= GIMG_BMP_V5HEADER_SIZE) {
    out_info->intent = bmp_intent_from_v5(header->intent);
  }
}

GIMG_Result gimg_bmp_read_profile(GIMG_Stream * stream,
    const gimg_bmp_header_t * header, const GIMG_Limits * limits,
    const GIMG_Allocator * alloc, void ** out_profile, size_t * out_size) {
  *out_profile = NULL;
  *out_size = 0;

  if (header->header_size < GIMG_BMP_V5HEADER_SIZE ||
      header->cs_type != GIMG_BMP_PROFILE_EMBEDDED || !header->profile_size) {
    return GIMG_OK;
  }

  // bV5ProfileData is measured from the start of the DIB header, which is the
  // file header's length into the file.
  size_t at = (size_t)GIMG_BMP_FILE_HEADER_SIZE + (size_t)header->profile_offset;
  size_t size = (size_t)header->profile_size;

  size_t stream_size = gimg_stream_size(stream);
  if (stream_size != GIMG_STREAM_SIZE_UNKNOWN &&
      (at > stream_size || size > stream_size - at)) {
    // A profile that runs off the end of the file is not one.  This is not
    // fatal to the image, which decodes perfectly well untagged.
    return GIMG_OK;
  }
  if (size > GIMG_BMP_ICC_MAX_SIZE) {
    // Past what any real profile is, so the file is describing something
    // other than its own color.  Untagged, for the same reason a profile
    // running off the end of the file is: the picture is not wrong.
    return GIMG_OK;
  }
  if (limits && limits->max_memory && size > limits->max_memory) {
    // A limit the caller set is different: they asked to be told.
    return GIMG_ERR_LIMIT;
  }

  size_t resume = gimg_stream_tell(stream);
  GIMG_Result r = gimg_stream_seek(stream, at);
  if (r != GIMG_OK) {
    return GIMG_OK; // Unreadable, so untagged; not a reason to refuse a file.
  }

  void * profile = gimg_malloc(alloc, size);
  if (!profile) {
    (void)gimg_stream_seek(stream, resume);
    return GIMG_ERR_OOM;
  }
  r = gimg_stream_read_exact(stream, profile, size);
  (void)gimg_stream_seek(stream, resume);
  if (r != GIMG_OK) {
    gimg_free(alloc, profile);
    return GIMG_OK;
  }

  *out_profile = profile;
  *out_size = size;
  return GIMG_OK;
}

/**
 * Write the nine endpoint values a gamut spells, as the header stores them.
 *
 * The fields are declared CIEXYZ and written by every writer in reach as xyY
 * chromaticities normalized so each triple sums to one, which is what the
 * reader above expects, so that is what goes out: x, y, and one minus the two
 * of them.
 */
static void bmp_endpoints_from_gamut(
    const bmp_gamut_t * gamut, unsigned char * out) {
  for (unsigned int channel = 0; channel < 3; channel++) {
    double x = gamut->xy[channel * 2];
    double y = gamut->xy[(channel * 2) + 1];
    double triple[3] = {x, y, 1.0 - x - y};
    for (unsigned int i = 0; i < 3; i++) {
      int32_t fixed =
          (int32_t)((triple[i] * (double)GIMG_BMP_FXPT2DOT30_ONE) + 0.5);
      gimg_bmp_write_u32(
          out + (((channel * 3) + i) * 4), (uint32_t)fixed);
    }
  }
}

uint32_t gimg_bmp_color_to_header(
    const GIMG_Color_Info * info, unsigned char * tail) {
  memset(tail, 0, GIMG_BMP_V5HEADER_SIZE - GIMG_BMP_V4_TAIL_AT);

  if (!info) {
    return 0;
  }

  // An embedded profile is the most specific thing a BMP can say, and only a
  // V5 header has the two fields that locate one.  The caller fills them in,
  // because where the profile lands depends on how much pixel data precedes
  // it.
  if (info->icc_bytes && info->icc_size > 0 &&
      info->icc_size <= GIMG_BMP_ICC_MAX_SIZE) {
    gimg_bmp_write_u32(tail + GIMG_BMP_V4_CS_TYPE_AT, GIMG_BMP_PROFILE_EMBEDDED);
    gimg_bmp_write_u32(
        tail + GIMG_BMP_V5_INTENT_AT, bmp_intent_to_v5(info->intent));
    return GIMG_BMP_V5HEADER_SIZE;
  }

  bool said_something = false;
  if (info->transfer == GIMG_TRANSFER_SRGB) {
    // LCS_sRGB asserts the whole of sRGB, its transfer curve included, so it
    // takes the transfer actually saying so - the same rule the PNG writer
    // applies to its sRGB chunk, and for the same reason: a file naming
    // sRGB's primaries with some other curve is not an sRGB image.
    gimg_bmp_write_u32(tail + GIMG_BMP_V4_CS_TYPE_AT, GIMG_BMP_LCS_sRGB);
    said_something = true;
  }
  else {
    // LCS_CALIBRATED_RGB, which means the endpoints and gammas below describe
    // the space.  Either half may be left at zero: a triple of zeros does not
    // sum to one and so reads back as an unnamed gamut, and a gamma of zero
    // reads back as no transfer stated.  Saying only the half that is known
    // beats inventing the other.
    for (size_t i = 0; i < GIMG_ARRAY_SIZE(bmp_known_gamuts); i++) {
      if (bmp_known_gamuts[i].primaries == info->primaries) {
        bmp_endpoints_from_gamut(
            &bmp_known_gamuts[i], tail + GIMG_BMP_V4_ENDPOINTS_AT);
        said_something = true;
        break;
      }
    }
    double gamma = 0.0;
    if (info->transfer == GIMG_TRANSFER_LINEAR) {
      gamma = 1.0;
    }
    else if (info->transfer == GIMG_TRANSFER_GAMMA && info->gamma_value > 0.0) {
      gamma = info->gamma_value;
    }
    // The field is 16.16 fixed point, so it holds a gamma below 65536 and
    // nothing above; one it cannot hold goes unsaid, as gAMA's does in the
    // PNG writer.
    double scaled = (gamma * 65536.0) + 0.5;
    if (scaled >= 1.0 && scaled <= 4294967295.0) {
      uint32_t fixed = (uint32_t)scaled;
      gimg_bmp_write_u32(tail + GIMG_BMP_V4_GAMMA_AT, fixed);
      gimg_bmp_write_u32(tail + GIMG_BMP_V4_GAMMA_AT + 4, fixed);
      gimg_bmp_write_u32(tail + GIMG_BMP_V4_GAMMA_AT + 8, fixed);
      said_something = true;
    }
  }

  if (!said_something) {
    return 0;
  }
  if (info->intent != GIMG_INTENT_PERCEPTUAL) {
    // Only a V5 header has bV5Intent, so an intent other than the one both
    // this model and ICC treat as the default is what makes the difference
    // between the two header versions here.
    gimg_bmp_write_u32(
        tail + GIMG_BMP_V5_INTENT_AT, bmp_intent_to_v5(info->intent));
    return GIMG_BMP_V5HEADER_SIZE;
  }
  return GIMG_BMP_V4HEADER_SIZE;
}

GIMG_Result gimg_bmp_read_linked_path(GIMG_Stream * stream,
    const gimg_bmp_header_t * header, const GIMG_Allocator * alloc,
    char ** out_path) {
  *out_path = NULL;

  if (header->header_size < GIMG_BMP_V5HEADER_SIZE ||
      header->cs_type != GIMG_BMP_PROFILE_LINKED || !header->profile_size) {
    return GIMG_OK;
  }

  // bV5ProfileData is measured from the start of the DIB header, the same as
  // an embedded profile's.
  size_t at = (size_t)GIMG_BMP_FILE_HEADER_SIZE + (size_t)header->profile_offset;
  size_t size = (size_t)header->profile_size;

  // A path is a path.  Anything longer than this is not one, and is not worth
  // allocating on the say-so of a header field.
  if (size > GIMG_BMP_LINKED_PATH_MAX) {
    return GIMG_OK;
  }
  size_t stream_size = gimg_stream_size(stream);
  if (stream_size != GIMG_STREAM_SIZE_UNKNOWN &&
      (at > stream_size || size > stream_size - at)) {
    return GIMG_OK;
  }

  size_t resume = gimg_stream_tell(stream);
  if (gimg_stream_seek(stream, at) != GIMG_OK) {
    return GIMG_OK;
  }
  char * path = (char *)gimg_malloc(alloc, size + 1u);
  if (!path) {
    (void)gimg_stream_seek(stream, resume);
    return GIMG_ERR_OOM;
  }
  GIMG_Result r = gimg_stream_read_exact(stream, path, size);
  (void)gimg_stream_seek(stream, resume);
  if (r != GIMG_OK) {
    gimg_free(alloc, path);
    return GIMG_OK;
  }
  // The stored bytes may or may not include their own terminator; terminating
  // here means the caller is handed a C string either way, and a path holding
  // an interior NUL ends where that NUL says rather than running on.
  path[size] = '\0';
  if (path[0] == '\0') {
    gimg_free(alloc, path);
    return GIMG_OK;
  }
  *out_path = path;
  return GIMG_OK;
}

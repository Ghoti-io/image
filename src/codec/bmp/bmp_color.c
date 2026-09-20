/**
 * @file
 *
 * BMP color management: the BITMAPV4HEADER and BITMAPV5HEADER color fields.
 *
 * Copyright 2026 by Corey Pennycuff
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

/** bV4CSType / bV5CSType values, as four-character codes where they are. */
#define GIMG_BMP_LCS_CALIBRATED_RGB UINT32_C(0x00000000)
#define GIMG_BMP_LCS_sRGB UINT32_C(0x73524742)           /* 'sRGB' */
#define GIMG_BMP_LCS_WINDOWS_COLOR_SPACE UINT32_C(0x57696E20) /* 'Win ' */
#define GIMG_BMP_PROFILE_LINKED UINT32_C(0x4C494E4B)     /* 'LINK' */
#define GIMG_BMP_PROFILE_EMBEDDED UINT32_C(0x4D424544)   /* 'MBED' */

/** bV5Intent values (wingdi.h LCS_GM_*). */
#define GIMG_BMP_LCS_GM_BUSINESS 1u          /* Saturation. */
#define GIMG_BMP_LCS_GM_GRAPHICS 2u          /* Relative colorimetric. */
#define GIMG_BMP_LCS_GM_IMAGES 4u            /* Perceptual. */
#define GIMG_BMP_LCS_GM_ABS_COLORIMETRIC 8u  /* Absolute colorimetric. */

/**
 * One chromaticity, as an FXPT2DOT30 - a signed fixed-point value with 30
 * fractional bits - compared at a thousandth, which is finer than any of
 * these tables is quoted to and coarser than the rounding of writing one out.
 */
#define GIMG_BMP_FXPT2DOT30_ONE (INT32_C(1) << 30)
#define GIMG_BMP_CHROMA_TOLERANCE (GIMG_BMP_FXPT2DOT30_ONE / 1000)

/** A gamut this model can name, as the nine endpoint values would spell it. */
typedef struct {
  GIMG_Primaries primaries;
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
  if (stream_size && (at > stream_size || size > stream_size - at)) {
    // A profile that runs off the end of the file is not one.  This is not
    // fatal to the image, which decodes perfectly well untagged.
    return GIMG_OK;
  }
  if (limits && limits->max_memory && size > limits->max_memory) {
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

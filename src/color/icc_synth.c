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
 * Synthesize a minimal ICC profile from a stated color model.
 *
 * --- Internal algorithms and design ---
 *
 * Most of this library repeats color statements rather than making them: a
 * profile read from one file is written to the next one byte for byte, and a
 * gamut named by a BMP's endpoints comes back out as a PNG's cHRM.  A JPEG
 * breaks that pattern, because APP2 is the only place a JPEG can name a color
 * space at all.  There is no gAMA, no cHRM, no cICP.  A raster that arrived
 * from a calibrated BMP knowing its primaries and its gamma, but carrying no
 * profile, had nowhere to put either, and a save as JPEG silently dropped
 * both.  This file is the remedy, and it is deliberately the only place in
 * the library that manufactures a color statement.
 *
 * What it builds is the smallest thing that is still a conforming profile: an
 * ICC v2.1 RGB display profile of the matrix/TRC kind, carrying the nine tags
 * ICC.1:2001-04 Table 16 requires of one - desc, cprt, wtpt, the three
 * colorants and the three tone curves.  Nothing else.  A matrix/TRC profile
 * is exactly as expressive as GIMG_Color_Info is: three primaries, a white
 * point and a curve, which is what the model states and no more.
 *
 * Version 2.1 rather than 4: a v2 profile is read by everything, and the one
 * thing v4 would buy here is parametricCurveType, which states the sRGB curve
 * in closed form rather than as samples.  That is what makes littleCMS's own
 * sRGB profile 588 bytes against the 976 of the one built here; going after
 * those 388 bytes would mean emitting v4, which also wants a chromatic
 * adaptation tag, for an annotation most readers would rather have in the
 * older form.  A profile stating a single gamma needs no samples at all and
 * comes to 492 bytes either way.
 *
 * The colorants are the published D50-adapted values for each gamut, not
 * something computed here from xy chromaticities.  An ICC matrix profile
 * states its colorants already adapted to the PCS illuminant, so computing
 * them would mean implementing a Bradford adaptation to reproduce numbers
 * that are fixed and tabulated.  Reproducing them exactly matters: these are
 * the values in the profiles everyone else ships, so a consumer comparing our
 * profile to theirs finds the same primaries rather than a rounding of them.
 * The test for them reads the colorants back out of a profile this file
 * wrote and checks that they sum to the media white point, which is the
 * property that makes a set of matrix colorants well formed.  Checking the
 * digits against a copy of themselves would assert nothing.
 *
 * GIMG_Color_Info.white_point is not read.  An ICC matrix profile states its
 * colorants already adapted to the PCS illuminant, so the white point it
 * carries is D50 whatever the gamut's native white was - here D65 for both,
 * which is what the tabulated colorants are adapted from.  A gamut with some
 * other native white would need its own adapted colorants in the table above,
 * not a different value in the wtpt tag.
 *
 * Both halves of the model must be known.  Primaries without a transfer
 * function, or the reverse, cannot become a matrix/TRC profile without
 * inventing the missing half, and inventing it is precisely what this file is
 * not for.  A caller that states neither gets no profile, which is what it
 * got before this file existed.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <ghoti.io/image/color.h>
#include <ghoti.io/image/macros.h>

#include "../core/alloc_internal.h"
#include "color_internal.h"

/** @{ @name ICC profile layout (ICC.1:2001-04 section 6). */
#define GIMG_ICC_HEADER_SIZE 128u   ///< Fixed by the spec.
#define GIMG_ICC_TAG_ENTRY_SIZE 12u ///< Signature, offset, size.
#define GIMG_ICC_TAG_COUNT 9u       ///< Table 16's required set, and no more.
/** Sample points in a tabulated tone curve.  Chosen by measurement: linear
 * interpolation between 256 samples of the sRGB curve is within 0.78 of 65535
 * of the true curve, which is the floor set by rounding the samples
 * themselves.  512 reaches 0.53 and 1024 reaches 0.51, so nothing past 256
 * buys accuracy a sixteen-bit sample could hold, and each doubling costs
 * another half kilobyte in every file. */
#define GIMG_ICC_TRC_POINTS 256u
/** @} */

/** @{ @name The PCS illuminant, D50, as the spec's own encoding (section 6.1.6).
 * Fixed constants rather than a rounding of 0.9642/1.0/0.8249, so that the
 * bytes match every other profile's header. */
#define GIMG_ICC_D50_X 0x0000F6D6L
#define GIMG_ICC_D50_Y 0x00010000L
#define GIMG_ICC_D50_Z 0x0000D32DL
/** @} */

/** A gamut this model can name, as an ICC profile would spell it: colorants
 * already adapted to D50, in the order red, green, blue, each X, Y, Z. */
typedef struct {
  GIMG_Primaries primaries;
  const char * name;
  double xyz[9];
} gimg_icc_gamut_t;

static const gimg_icc_gamut_t gimg_icc_known_gamuts[] = {
    // IEC 61966-2.1 (sRGB): BT.709 primaries, D65 white, adapted to D50.
    {GIMG_PRIMARIES_SRGB, "sRGB",
        {0.43607, 0.22249, 0.01392, 0.38515, 0.71687, 0.09708, 0.14307,
            0.06061, 0.71410}},
    // Adobe RGB (1998): the same red and blue as sRGB, a wider green.
    {GIMG_PRIMARIES_ADOBE_RGB, "Adobe RGB (1998)",
        {0.60974, 0.31111, 0.01947, 0.20528, 0.62567, 0.06087, 0.14919,
            0.06322, 0.74457}},
};

/** The gamut a model names, or NULL when this file has no colorants for it. */
static const gimg_icc_gamut_t * gimg_icc_gamut_for(GIMG_Primaries primaries) {
  for (size_t i = 0;
      i < sizeof(gimg_icc_known_gamuts) / sizeof(gimg_icc_known_gamuts[0]);
      i++) {
    if (gimg_icc_known_gamuts[i].primaries == primaries) {
      return &gimg_icc_known_gamuts[i];
    }
  }
  return NULL;
}

/** @{ @name Big-endian writers.  ICC is big-endian throughout (section 5). */
static void gimg_icc_put_u16(unsigned char * p, unsigned int v) {
  p[0] = (unsigned char)((v >> 8) & 0xFFu);
  p[1] = (unsigned char)(v & 0xFFu);
}

static void gimg_icc_put_u32(unsigned char * p, uint32_t v) {
  p[0] = (unsigned char)((v >> 24) & 0xFFu);
  p[1] = (unsigned char)((v >> 16) & 0xFFu);
  p[2] = (unsigned char)((v >> 8) & 0xFFu);
  p[3] = (unsigned char)(v & 0xFFu);
}

/** s15Fixed16Number: the value times 65536, rounded (section 5.3.8). */
static void gimg_icc_put_s15f16(unsigned char * p, double v) {
  double scaled = v * 65536.0;
  long rounded = (long)(scaled < 0 ? scaled - 0.5 : scaled + 0.5);
  gimg_icc_put_u32(p, (uint32_t)rounded);
}

static void gimg_icc_put_sig(unsigned char * p, const char * sig) {
  memcpy(p, sig, 4);
}
/** @} */

/** Round up to the four-byte boundary every tag's data starts on. */
static size_t gimg_icc_pad4(size_t n) {
  return (n + 3u) & ~(size_t)3u;
}

/** Bytes a textDescriptionType holds for an n-byte string (with its NUL):
 * the ASCII form, then the empty Unicode and ScriptCode forms the type
 * carries whether or not anything fills them (ICC.1:2001-04 section 6.5.17). */
static size_t gimg_icc_desc_size(size_t ascii_len) {
  return 12u + ascii_len + 4u + 4u + 2u + 1u + 67u;
}

/** Curve size for a tone curve of @p points samples; 0 points means the
 * identity, 1 means a single gamma value (section 6.5.5). */
static size_t gimg_icc_curv_size(unsigned int points) {
  return 12u + ((size_t)points * 2u);
}

/**
 * Format a gamma value the way "%.4g" writes it in the C locale, whatever
 * LC_NUMERIC the calling application happens to have set.
 *
 * printf takes its decimal separator from LC_NUMERIC, and this string becomes
 * the profile's desc tag - which the writers embed as a PNG iCCP chunk and a
 * JPEG APP2 segment. Left alone, a host that has called setlocale(LC_ALL, "")
 * on a German system writes "gamma 2,2" into the file, and the bytes of a
 * saved document stop being a function of the document and the options. That
 * is the same rule the JPEG thumbnail bug broke, with an environment variable
 * as the caller state instead of a decode order.
 *
 * The separator is identified by elimination rather than by asking
 * localeconv(): everything %.4g can emit for a finite value is a digit, a
 * sign or an exponent marker, so whatever else appears is the separator. That
 * avoids reading the locale object at all, which matters because a caller is
 * free to call setlocale() on another thread. A multi-byte separator collapses
 * to the single '.' the C locale would have written.
 *
 * A non-finite value is left as printf wrote it: "inf" and "nan" carry no
 * separator, and their letters would otherwise be mistaken for one.
 */
static void gimg_icc_format_gamma(char * buf, size_t size, double gamma) {
  (void)snprintf(buf, size, "%.4g", gamma);
  if (!isfinite(gamma)) {
    return;
  }
  char * w = buf;
  const char * r = buf;
  while (*r) {
    const int numeric = (*r >= '0' && *r <= '9') || *r == '-' || *r == '+' ||
        *r == 'e' || *r == 'E';
    if (numeric) {
      *w++ = *r++;
      continue;
    }
    *w++ = '.';
    while (*r && !((*r >= '0' && *r <= '9') || *r == '-' || *r == '+' ||
        *r == 'e' || *r == 'E')) {
      r++;
    }
  }
  *w = '\0';
}

/** Name the space in the way a human reading the profile would want it. */
static void gimg_icc_describe(const gimg_icc_gamut_t * gamut,
    const GIMG_Color_Info * info, char * out, size_t out_size) {
  if (info->transfer == GIMG_TRANSFER_SRGB &&
      gamut->primaries == GIMG_PRIMARIES_SRGB) {
    (void)snprintf(out, out_size, "sRGB");
    return;
  }
  if (info->transfer == GIMG_TRANSFER_GAMMA) {
    char gamma_text[32];
    gimg_icc_format_gamma(gamma_text, sizeof(gamma_text), info->gamma_value);
    (void)snprintf(out, out_size, "%s, gamma %s", gamut->name, gamma_text);
    return;
  }
  if (info->transfer == GIMG_TRANSFER_LINEAR) {
    (void)snprintf(out, out_size, "%s, linear", gamut->name);
    return;
  }
  (void)snprintf(out, out_size, "%s", gamut->name);
}

/** Write the tone curve the model asks for, and say how many bytes it took. */
static size_t gimg_icc_write_trc(
    unsigned char * p, const GIMG_Color_Info * info) {
  gimg_icc_put_sig(p, "curv");
  gimg_icc_put_u32(p + 4, 0);
  if (info->transfer == GIMG_TRANSFER_LINEAR) {
    // Zero sample points is the identity, which is what linear means.
    gimg_icc_put_u32(p + 8, 0);
    return gimg_icc_curv_size(0);
  }
  if (info->transfer == GIMG_TRANSFER_GAMMA) {
    // One sample point is a u8Fixed8Number gamma exponent.
    double g = info->gamma_value * 256.0;
    long q = (long)(g + 0.5);
    if (q < 0) {
      q = 0;
    }
    if (q > 0xFFFF) {
      q = 0xFFFF;
    }
    gimg_icc_put_u32(p + 8, 1);
    gimg_icc_put_u16(p + 12, (unsigned int)q);
    return gimg_icc_curv_size(1);
  }
  // sRGB: tabulate the piecewise curve of IEC 61966-2.1, which no v2 curve
  // type can state in closed form.
  gimg_icc_put_u32(p + 8, GIMG_ICC_TRC_POINTS);
  for (unsigned int i = 0; i < GIMG_ICC_TRC_POINTS; i++) {
    double x = (double)i / (double)(GIMG_ICC_TRC_POINTS - 1u);
    double linear = x <= 0.04045 ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4);
    double scaled = (linear * 65535.0) + 0.5;
    if (scaled < 0.0) {
      scaled = 0.0;
    }
    if (scaled > 65535.0) {
      scaled = 65535.0;
    }
    gimg_icc_put_u16(p + 12 + ((size_t)i * 2u), (unsigned int)scaled);
  }
  return gimg_icc_curv_size(GIMG_ICC_TRC_POINTS);
}

GIMG_Result gimg_icc_synthesize(const GIMG_Allocator * alloc,
    const GIMG_Color_Info * info, void ** out_bytes, size_t * out_size) {
  if (!out_bytes || !out_size) {
    return GIMG_ERR_INTERNAL;
  }
  *out_bytes = NULL;
  *out_size = 0;
  if (!info) {
    return GIMG_OK;
  }
  const gimg_icc_gamut_t * gamut = gimg_icc_gamut_for(info->primaries);
  if (!gamut || info->transfer == GIMG_TRANSFER_UNKNOWN) {
    // Half a color model cannot be written without inventing the other half.
    return GIMG_OK;
  }
  if (info->transfer == GIMG_TRANSFER_GAMMA &&
      !(info->gamma_value > 0.0 && info->gamma_value < 256.0)) {
    // A gamma the curve type cannot hold says nothing worth writing.
    return GIMG_OK;
  }

  char desc_text[96];
  gimg_icc_describe(gamut, info, desc_text, sizeof(desc_text));
  static const char copyright[] = "No copyright, use freely.";

  size_t desc_ascii = strlen(desc_text) + 1u;
  size_t desc_size = gimg_icc_desc_size(desc_ascii);
  size_t cprt_size = 8u + sizeof(copyright);
  unsigned int trc_points = info->transfer == GIMG_TRANSFER_LINEAR ? 0u
      : info->transfer == GIMG_TRANSFER_GAMMA                      ? 1u
                                                                   : GIMG_ICC_TRC_POINTS;
  size_t trc_size = gimg_icc_curv_size(trc_points);

  size_t off_desc = GIMG_ICC_HEADER_SIZE + 4u +
      (GIMG_ICC_TAG_COUNT * GIMG_ICC_TAG_ENTRY_SIZE);
  size_t off_cprt = off_desc + gimg_icc_pad4(desc_size);
  size_t off_wtpt = off_cprt + gimg_icc_pad4(cprt_size);
  size_t off_r = off_wtpt + 20u;
  size_t off_g = off_r + 20u;
  size_t off_b = off_g + 20u;
  size_t off_trc = off_b + 20u;
  size_t total = off_trc + gimg_icc_pad4(trc_size);

  unsigned char * buf = (unsigned char *)gimg_calloc(alloc, 1, total);
  if (!buf) {
    return GIMG_ERR_OOM;
  }

  // Header (section 6.1).  Everything not set here is zero, which is what the
  // spec asks for in the fields a profile like this does not use.
  gimg_icc_put_u32(buf, (uint32_t)total);
  gimg_icc_put_u32(buf + 8, 0x02100000u); // Version 2.1.
  gimg_icc_put_sig(buf + 12, "mntr");     // Display device.
  gimg_icc_put_sig(buf + 16, "RGB ");
  gimg_icc_put_sig(buf + 20, "XYZ "); // The PCS a matrix/TRC profile targets.
  gimg_icc_put_sig(buf + 36, "acsp");
  // ICC names four intents and nothing else.  This does not trust the field
  // to hold one of them: what goes into a file is the writer's responsibility,
  // and a number no reader can interpret is worse than the default.  The cast
  // makes a negative value fail the same bound as a too-large one.  Perceptual
  // is the enum's zero and the sensible reading of "unstated".
  uint32_t intent = (uint32_t)info->intent;
  if (intent >= (uint32_t)GIMG_INTENT_COUNT) {
    intent = (uint32_t)GIMG_INTENT_PERCEPTUAL;
  }
  gimg_icc_put_u32(buf + 64, intent);
  gimg_icc_put_u32(buf + 68, (uint32_t)GIMG_ICC_D50_X);
  gimg_icc_put_u32(buf + 72, (uint32_t)GIMG_ICC_D50_Y);
  gimg_icc_put_u32(buf + 76, (uint32_t)GIMG_ICC_D50_Z);

  // Tag table (section 6.3).  The three tone curves share one block of data,
  // which the spec allows and which keeps a sampled sRGB curve from being
  // stored three times.
  static const char * const tag_sig[GIMG_ICC_TAG_COUNT] = {
      "desc", "cprt", "wtpt", "rXYZ", "gXYZ", "bXYZ", "rTRC", "gTRC", "bTRC"};
  const size_t tag_off[GIMG_ICC_TAG_COUNT] = {off_desc, off_cprt, off_wtpt,
      off_r, off_g, off_b, off_trc, off_trc, off_trc};
  const size_t tag_len[GIMG_ICC_TAG_COUNT] = {
      desc_size, cprt_size, 20u, 20u, 20u, 20u, trc_size, trc_size, trc_size};
  gimg_icc_put_u32(buf + GIMG_ICC_HEADER_SIZE, GIMG_ICC_TAG_COUNT);
  for (unsigned int i = 0; i < GIMG_ICC_TAG_COUNT; i++) {
    unsigned char * e = buf + GIMG_ICC_HEADER_SIZE + 4u +
        ((size_t)i * GIMG_ICC_TAG_ENTRY_SIZE);
    gimg_icc_put_sig(e, tag_sig[i]);
    gimg_icc_put_u32(e + 4, (uint32_t)tag_off[i]);
    gimg_icc_put_u32(e + 8, (uint32_t)tag_len[i]);
  }

  // desc: the ASCII form filled in, the Unicode and Macintosh forms present
  // but empty, which is how the type is built whether or not they are used.
  gimg_icc_put_sig(buf + off_desc, "desc");
  gimg_icc_put_u32(buf + off_desc + 8, (uint32_t)desc_ascii);
  memcpy(buf + off_desc + 12, desc_text, desc_ascii);

  gimg_icc_put_sig(buf + off_cprt, "text");
  memcpy(buf + off_cprt + 8, copyright, sizeof(copyright));

  gimg_icc_put_sig(buf + off_wtpt, "XYZ ");
  gimg_icc_put_u32(buf + off_wtpt + 8, (uint32_t)GIMG_ICC_D50_X);
  gimg_icc_put_u32(buf + off_wtpt + 12, (uint32_t)GIMG_ICC_D50_Y);
  gimg_icc_put_u32(buf + off_wtpt + 16, (uint32_t)GIMG_ICC_D50_Z);

  const size_t colorant_off[3] = {off_r, off_g, off_b};
  for (unsigned int c = 0; c < 3; c++) {
    unsigned char * p = buf + colorant_off[c];
    gimg_icc_put_sig(p, "XYZ ");
    for (unsigned int axis = 0; axis < 3; axis++) {
      gimg_icc_put_s15f16(p + 8 + ((size_t)axis * 4u),
          gamut->xyz[((size_t)c * 3u) + axis]);
    }
  }

  (void)gimg_icc_write_trc(buf + off_trc, info);

  *out_bytes = buf;
  *out_size = total;
  return GIMG_OK;
}

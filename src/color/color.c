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
 * The colour model: the one table of named gamuts, and the default.
 *
 * --- Internal algorithms and design ---
 *
 * GIMG_Color_Info stores a gamut as coordinates and never as a name, so that
 * a name and coordinates cannot disagree.  gimg_gamut_identify() is how a
 * caller asks for the name, and it is the only place that decides what
 * "matches" means.
 *
 * This replaces two private tables that answered the same question in
 * different spellings: the PNG codec's, as cHRM writes it (white and three
 * primaries, x then y), and the BMP codec's, as the V4 endpoints write it
 * (three primaries, no white, in FXPT2DOT30).  Each could answer only "sRGB",
 * "Adobe RGB" or "unknown", each carried its own tolerance, and adding a
 * space meant editing both.  A third copy sat in png_save.c as eight
 * hard-coded integers, because Adobe RGB was the only non-sRGB gamut the
 * model could hold.  All three are gone; converting to each format's spelling
 * is that codec's job.
 *
 * icc_synth.c keeps a table of its own and is *not* a fourth copy of this
 * one.  It holds published D50-adapted XYZ colorants, which cannot be derived
 * from these chromaticities without implementing a Bradford adaptation, and
 * which have to match the numbers everyone else ships.  It names fewer spaces
 * than this table does, so a gamut named here may still get no synthesized
 * profile - which is the right answer, a matrix profile with invented
 * colorants being worse than none.
 */

#include <ghoti.io/image/color.h>
#include <ghoti.io/image/macros.h>
#include <string.h>

/** A named gamut and the coordinates that identify it. */
typedef struct {
  GIMG_Primaries named;
  GIMG_Gamut gamut;
} gimg_named_gamut_t;

/**
 * The spaces this library can put a name to.
 *
 * White points are the standard illuminants: D65 is (0.3127, 0.3290) and D50
 * is (0.3457, 0.3585), both as CIE 1931 2-degree observer chromaticities.
 *
 * No two entries share their three primaries, which is what lets
 * gimg_gamut_identify() name a gamut that carries no white point.  Adding a
 * space that breaks that - theatrical DCI-P3, whose primaries are Display
 * P3's and whose white is not D65 - means teaching that function to refuse
 * rather than guess, and the assertion in the unit tests is there to make the
 * addition fail loudly rather than silently pick one.
 */
static const gimg_named_gamut_t gimg_named_gamuts[] = {
    // IEC 61966-2.1 (sRGB), sharing ITU-R BT.709's primaries.
    {GIMG_PRIMARIES_SRGB,
        {{0.3127, 0.3290}, {0.6400, 0.3300}, {0.3000, 0.6000},
            {0.1500, 0.0600}}},
    // Adobe RGB (1998): BT.709's red and blue, a wider green.
    {GIMG_PRIMARIES_ADOBE_RGB,
        {{0.3127, 0.3290}, {0.6400, 0.3300}, {0.2100, 0.7100},
            {0.1500, 0.0600}}},
    // Display P3: SMPTE RP 431-2 primaries on a D65 white.
    {GIMG_PRIMARIES_DISPLAY_P3,
        {{0.3127, 0.3290}, {0.6800, 0.3200}, {0.2650, 0.6900},
            {0.1500, 0.0600}}},
    // ITU-R BT.2020.
    {GIMG_PRIMARIES_BT2020,
        {{0.3127, 0.3290}, {0.7080, 0.2920}, {0.1700, 0.7970},
            {0.1310, 0.0460}}},
    // ROMM RGB (ProPhoto), the one here on a D50 white.
    {GIMG_PRIMARIES_PROPHOTO,
        {{0.3457, 0.3585}, {0.7347, 0.2653}, {0.1596, 0.8404},
            {0.0366, 0.0001}}},
};

/** True when two chromaticities agree to @p tolerance on both axes. */
static bool gimg_chroma_close(const GIMG_Chromaticity * a,
    const GIMG_Chromaticity * b, double tolerance) {
  double dx = a->x - b->x;
  double dy = a->y - b->y;
  if (dx < 0) {
    dx = -dx;
  }
  if (dy < 0) {
    dy = -dy;
  }
  // Written so that a NaN fails: GIMG_Color_Info carries doubles that nothing
  // polices, and a NaN compares false against every bound rather than true.
  return dx <= tolerance && dy <= tolerance;
}

/** True when @p c is the unset {0, 0}. */
static bool gimg_chroma_unset(const GIMG_Chromaticity * c) {
  return c->x == 0.0 && c->y == 0.0;
}

GIMG_API GIMG_Primaries gimg_gamut_identify(
    const GIMG_Gamut * gamut, double tolerance) {
  if (!gamut) {
    return GIMG_PRIMARIES_UNKNOWN;
  }
  if (!(tolerance > 0.0)) {
    // Covers a NaN as well as zero and negatives.
    tolerance = GIMG_GAMUT_TOLERANCE_DEFAULT;
  }
  const bool have_white = !gimg_chroma_unset(&gamut->white);
  for (size_t i = 0; i < GIMG_ARRAY_SIZE(gimg_named_gamuts); i++) {
    const GIMG_Gamut * known = &gimg_named_gamuts[i].gamut;
    if (!gimg_chroma_close(&gamut->red, &known->red, tolerance) ||
        !gimg_chroma_close(&gamut->green, &known->green, tolerance) ||
        !gimg_chroma_close(&gamut->blue, &known->blue, tolerance)) {
      continue;
    }
    // A file that states no white point - a BMP V4 header has nowhere to put
    // one - is matched on its primaries, which is unambiguous only because no
    // two entries above share theirs.
    if (have_white &&
        !gimg_chroma_close(&gamut->white, &known->white, tolerance)) {
      continue;
    }
    return gimg_named_gamuts[i].named;
  }
  return GIMG_PRIMARIES_UNKNOWN;
}

GIMG_API bool gimg_gamut_named(GIMG_Primaries named, GIMG_Gamut * out_gamut) {
  if (!out_gamut) {
    return false;
  }
  for (size_t i = 0; i < GIMG_ARRAY_SIZE(gimg_named_gamuts); i++) {
    if (gimg_named_gamuts[i].named == named) {
      *out_gamut = gimg_named_gamuts[i].gamut;
      return true;
    }
  }
  return false;
}

GIMG_API bool gimg_color_info_set_gamut(
    GIMG_Color_Info * info, GIMG_Primaries named) {
  if (!info) {
    return false;
  }
  if (!gimg_gamut_named(named, &info->gamut)) {
    memset(&info->gamut, 0, sizeof(info->gamut));
    info->gamut_stated = false;
    return false;
  }
  info->gamut_stated = true;
  return true;
}

GIMG_API void gimg_color_info_default(GIMG_Color_Info * info) {
  if (!info) {
    return;
  }
  // memset rather than a designated initializer: every field added later is
  // then zeroed by construction instead of by remembering to name it here.
  memset(info, 0, sizeof(*info));
  info->transfer = GIMG_TRANSFER_UNKNOWN;
  info->reference = GIMG_REFERENCE_UNKNOWN;
  info->intent = GIMG_INTENT_PERCEPTUAL;
  info->cmyk_polarity = GIMG_CMYK_POLARITY_UNKNOWN;
}

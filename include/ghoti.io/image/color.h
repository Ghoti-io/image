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
 * Color model: primaries, transfer, ICC stub (spec §4.2).
 */

#ifndef GHOTI_IO_GIMG_COLOR_H
#define GHOTI_IO_GIMG_COLOR_H

#include <ghoti.io/image/core.h>
#include <ghoti.io/image/macros.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A CIE 1931 chromaticity.
 *
 * The (x, y) of a colour, which is what every format in reach states when it
 * states a gamut: PNG's cHRM, BMP's V4 endpoints, TIFF's WhitePoint and
 * PrimaryChromaticities.  Storing it is what lets a file whose space this
 * library has no name for still arrive intact.
 */
typedef struct {
  double x;
  double y;
} GIMG_Chromaticity;

/**
 * @brief A gamut stated exactly: a white point and three primaries.
 *
 * All-zero means "not stated"; see GIMG_Color_Info.gamut_stated, which is the
 * field to test rather than comparing against zero here.
 *
 * @a white may be {0, 0} while the primaries are set, because a BMP V4 header
 * carries three endpoints and no white point at all.  A reader that needs one
 * must say where it got it; this struct will not invent D65.
 */
typedef struct {
  GIMG_Chromaticity white; ///< White point, or {0, 0} when the file omits it.
  GIMG_Chromaticity red;
  GIMG_Chromaticity green;
  GIMG_Chromaticity blue;
} GIMG_Gamut;

/**
 * @brief Names for gamuts this library can recognise.
 *
 * This is a *result*, not a storage format.  GIMG_Color_Info carries a
 * GIMG_Gamut; gimg_gamut_identify() turns one into a name when it matches a
 * known set of coordinates, and gimg_gamut_named() goes the other way.  A
 * space with no name here is not a space this library cannot carry - it is
 * one it cannot label, which is a much smaller thing.
 */
typedef enum {
  GIMG_PRIMARIES_UNKNOWN = 0,
  GIMG_PRIMARIES_SRGB,       ///< ITU-R BT.709 primaries, D65 white.
  GIMG_PRIMARIES_ADOBE_RGB,  ///< Adobe RGB (1998), D65 white.
  GIMG_PRIMARIES_DISPLAY_P3, ///< SMPTE RP 431-2 primaries, D65 white.
  GIMG_PRIMARIES_BT2020,     ///< ITU-R BT.2020 primaries, D65 white.
  GIMG_PRIMARIES_PROPHOTO,   ///< ROMM RGB, D50 white.
  GIMG_PRIMARIES_COUNT
} GIMG_Primaries;

/**
 * @brief Transfer function.
 *
 * Two families that are not the same kind of thing share this enum.  Most are
 * curves that can be written as parameters (GIMG_TRANSFER_PARAMETRIC holds
 * the general form, and SRGB, LINEAR and GAMMA are special cases of it kept
 * separate because they are what the formats actually name).  PQ and HLG are
 * not curves of that form at all; they are defined by their own standards and
 * can only be named.
 */
typedef enum {
  GIMG_TRANSFER_UNKNOWN = 0,
  GIMG_TRANSFER_LINEAR,
  GIMG_TRANSFER_SRGB,  ///< IEC 61966-2.1; a parametric type 3 curve.
  GIMG_TRANSFER_GAMMA, ///< Plain exponent; uses gamma_value.
  /**
   * ICC parametricCurveType, held in GIMG_Color_Info.transfer_params as the
   * general (type 4) form: Y = (aX + b)^g + e for X >= d, and cX + f below
   * it, with the terms in the order {g, a, b, c, d, e, f}.  Types 0 to 3 are
   * special cases stored the same way with the unused terms zeroed, so that
   * one evaluator serves all five.
   */
  GIMG_TRANSFER_PARAMETRIC,
  GIMG_TRANSFER_BT1886, ///< ITU-R BT.1886, the reference CRT curve.
  GIMG_TRANSFER_PQ,     ///< SMPTE ST 2084 (perceptual quantizer).
  GIMG_TRANSFER_HLG,    ///< ITU-R BT.2100 hybrid log-gamma.
  GIMG_TRANSFER_COUNT
} GIMG_Transfer;

/**
 * @brief What a sample value is measured against.
 *
 * GIMG_TRANSFER_LINEAR alone is ambiguous, and the ambiguity is not academic:
 * a linear sRGB raster and a LogLuv raster are both "linear" and mean
 * completely different things.  A display-referred sample is a fraction of a
 * diffuse white that the file does not quantify; a scene-referred sample is a
 * measurement of light, and GIMG_Color_Info.white_luminance says in what.
 *
 * Nothing in this library produces GIMG_REFERENCE_SCENE yet - it exists
 * because the formats that need it (LogLuv, PQ, floating-point TIFF) would
 * otherwise have to be given somewhere to say this after rasters already
 * existed that could not.
 */
typedef enum {
  GIMG_REFERENCE_UNKNOWN = 0,
  GIMG_REFERENCE_DISPLAY, ///< Relative to an unstated diffuse white.
  GIMG_REFERENCE_SCENE,   ///< Absolute light; see white_luminance.
  GIMG_REFERENCE_COUNT
} GIMG_Reference;

/**
 * @brief Rendering intent (when ICC present).
 */
typedef enum {
  GIMG_INTENT_PERCEPTUAL = 0,
  GIMG_INTENT_RELATIVE_COLORIMETRIC,
  GIMG_INTENT_SATURATION,
  GIMG_INTENT_ABSOLUTE_COLORIMETRIC,
  GIMG_INTENT_COUNT
} GIMG_Rendering_Intent;

/**
 * @brief CMYK channel interpretation (only relevant when raster format is
 * CMYK). Raster pixels are stored as raw values; this describes how to
 * interpret them.
 */
typedef enum {
  GIMG_CMYK_POLARITY_UNKNOWN = 0,
  /** 0 = full ink, 255 = no ink (Adobe / JPEG file convention). */
  GIMG_CMYK_POLARITY_INK,
  /** 0 = no ink, 255 = full ink (reflection; e.g. many design-tool APIs). */
  GIMG_CMYK_POLARITY_REFLECTION,
  GIMG_CMYK_POLARITY_COUNT
} GIMG_CMYK_Polarity;

/** @brief Number of terms in GIMG_Color_Info.transfer_params. */
#define GIMG_TRANSFER_PARAM_COUNT 7

/**
 * @brief Color info attached to raster (spec §4.2).
 *
 * This structure *describes* colour and never converts it.  A field left at
 * its default means the file said nothing, which is different from the file
 * saying something this library cannot act on - in that case the field is
 * filled in and it is the caller's business what to do with it.
 */
typedef struct {
  /**
   * The gamut, stated exactly.  Valid only when @a gamut_stated is true.
   *
   * Use gimg_gamut_identify() to ask which named space this is, rather than
   * storing a name beside it: a name and coordinates that disagree is a state
   * this struct deliberately cannot reach.
   */
  GIMG_Gamut gamut;
  bool gamut_stated; ///< True when @a gamut holds something the file said.
  GIMG_Transfer transfer;
  /** parametricCurveType terms; see GIMG_TRANSFER_PARAMETRIC. */
  double transfer_params[GIMG_TRANSFER_PARAM_COUNT];
  double gamma_value; ///< Used when transfer == GIMG_TRANSFER_GAMMA.
  GIMG_Reference reference;
  /**
   * Luminance in cd/m^2 of a full-scale sample, or 0 when unstated.
   *
   * Meaningful for GIMG_REFERENCE_SCENE, where it is the scale that turns a
   * stored value into a measurement.  Zero is unambiguous as "unstated": a
   * file does not describe a display of no luminance.
   */
  double white_luminance;
  GIMG_Rendering_Intent intent;
  const void * icc_bytes; ///< Opaque; library does not take ownership.
  size_t icc_size;        ///< ICC profile size in bytes.
  /** Path of a profile the file named rather than carried, or NULL.
   *
   * BMP's PROFILE_LINKED states a file path instead of a profile.  The path is
   * reported and never opened: following a path an image file names is acting
   * on data, and is the shape of a directory traversal.  A caller who wants
   * the profile supplies GIMG_Load_Options.icc_resolver, which is the only
   * place that knows where the image came from and what it is willing to read.
   *
   * These are bytes as the file stored them, NUL-terminated, in whatever
   * encoding the writer used - bmpsuite's own case is a Windows path holding a
   * byte that is not ASCII and names no stated codepage - so treat this as a
   * path to show a user or hand to a resolver, not as UTF-8.
   *
   * Set only when the profile was not resolved; when a resolver supplied one,
   * icc_bytes carries it and this stays NULL. */
  const char * icc_linked_path;
  GIMG_CMYK_Polarity cmyk_polarity; ///< Interpretation of CMYK channels; use
                                    ///< when raster format is GIMG_PIXEL_CMYK8.
  uint8_t _reserved[16];
} GIMG_Color_Info;

/**
 * @brief The tolerance gimg_gamut_identify() uses when none is given.
 *
 * One thousandth, which is finer than any of the published tables is quoted
 * to and coarser than the rounding of writing one into a file.  It is the
 * figure the PNG, BMP and ICC paths each chose separately before they shared
 * a table.
 */
#define GIMG_GAMUT_TOLERANCE_DEFAULT 0.001

/**
 * @brief Name the gamut @p gamut describes.
 *
 * Compares the three primaries, and the white point too when @p gamut states
 * one.  A gamut whose white point is {0, 0} - a BMP V4 header, which has
 * nowhere to put one - is matched on its primaries alone.
 *
 * That is unambiguous for the spaces named here, because no two of them share
 * primaries.  It would stop being unambiguous the moment a space is added
 * that differs from another only in its white point: theatrical DCI-P3 and
 * Display P3 are exactly that pair, so adding the first means this function
 * must start refusing to name a white-point-less P3 rather than guessing.
 *
 * @param gamut The coordinates to identify.  NULL returns UNKNOWN.
 * @param tolerance Absolute tolerance per coordinate; pass
 *   GIMG_GAMUT_TOLERANCE_DEFAULT unless there is a reason not to.  A
 *   non-positive value is treated as the default.
 * @return The name, or GIMG_PRIMARIES_UNKNOWN when nothing matches.
 */
GIMG_API GIMG_Primaries gimg_gamut_identify(
    const GIMG_Gamut * gamut, double tolerance);

/**
 * @brief The coordinates of a named gamut.
 *
 * @param named The space to look up.
 * @param out_gamut Receives the coordinates; untouched when false is returned.
 * @return true when @p named is a space with coordinates here, false for
 *   GIMG_PRIMARIES_UNKNOWN, GIMG_PRIMARIES_COUNT and anything out of range.
 */
GIMG_API bool gimg_gamut_named(GIMG_Primaries named, GIMG_Gamut * out_gamut);

/**
 * @brief Set @p info's gamut from a named space.
 *
 * The convenience that keeps a codec from having to hold a coordinate table
 * of its own just to say "this file said sRGB".  Clears @a gamut_stated when
 * @p named has no coordinates here.
 *
 * @param info The colour info to modify.  NULL is ignored.
 * @param named The space to store.
 * @return true when the gamut was set.
 */
GIMG_API bool gimg_color_info_set_gamut(
    GIMG_Color_Info * info, GIMG_Primaries named);

/**
 * @brief Initialize color info to unknown/default.
 */
GIMG_API void gimg_color_info_default(GIMG_Color_Info * info);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GIMG_COLOR_H

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
 * Either half may be absent, so read GIMG_Color_Info.primaries_stated and
 * .white_stated rather than comparing against zero.  A reader that needs a
 * point this struct does not hold must say where it got it; nothing here
 * invents D65.
 */
typedef struct {
  GIMG_Chromaticity white; ///< White point; see GIMG_Color_Info.white_stated.
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
 * @brief Which light a sample describes.
 *
 * Display-referred samples describe light a display emits, already rendered
 * for it.  Scene-referred samples describe light as it was in front of the
 * camera, with no rendering applied.
 *
 * This is **not** the same question as whether the sample is an absolute
 * measurement - see GIMG_Sample_Scale.  The two are independent and the four
 * combinations all occur: sRGB is display-referred and relative, PQ is
 * display-referred and absolute, HLG is scene-referred and relative, and
 * LogLuv is scene-referred and absolute.  They were one enum when this was
 * first written, which could not have described PQ.
 */
typedef enum {
  GIMG_REFERENCE_UNKNOWN = 0,
  GIMG_REFERENCE_DISPLAY, ///< Light a display emits.
  GIMG_REFERENCE_SCENE,   ///< Light as it was in the scene.
  GIMG_REFERENCE_COUNT
} GIMG_Reference;

/**
 * @brief Whether a sample is a measurement or a fraction.
 *
 * A relative sample is a fraction of a diffuse white the file does not
 * quantify; doubling every sample means "brighter" and nothing more.  An
 * absolute sample is a measurement, and GIMG_Color_Info.white_luminance says
 * of what.
 *
 * GIMG_TRANSFER_LINEAR alone cannot tell these apart, and the difference is
 * not academic: a linear sRGB raster and a LogLuv raster are both "linear"
 * and mean entirely different things.
 */
typedef enum {
  GIMG_SAMPLE_SCALE_UNKNOWN = 0,
  GIMG_SAMPLE_SCALE_RELATIVE, ///< A fraction of an unstated white.
  GIMG_SAMPLE_SCALE_ABSOLUTE, ///< A measurement; see white_luminance.
  GIMG_SAMPLE_SCALE_COUNT
} GIMG_Sample_Scale;

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
   * The gamut, stated exactly.  Which halves are valid is said by the two
   * flags below, because the formats state them separately and one flag for
   * both produced a real bug: a TIFF carrying WhitePoint (318) with no
   * PrimaryChromaticities (319) was written out as a PNG whose cHRM named
   * three primaries at (0, 0), which is not a gamut in any sense.
   *
   * Use gimg_gamut_identify() to ask which named space this is, rather than
   * storing a name beside it: a name and coordinates that disagree is a state
   * this struct deliberately cannot reach.
   */
  GIMG_Gamut gamut;
  /** True when @a gamut.red, .green and .blue are what the file said.
   *
   * This is the flag meaning "there is a gamut here", because three
   * primaries are what make one.  A writer that needs a gamut tests this. */
  bool primaries_stated;
  /** True when @a gamut.white is what the file said.
   *
   * Independent of @a primaries_stated in both directions: a BMP V4 header
   * carries three endpoints and nowhere to put a white point, and a TIFF may
   * carry WhitePoint without PrimaryChromaticities. */
  bool white_stated;
  GIMG_Transfer transfer;
  /** parametricCurveType terms; see GIMG_TRANSFER_PARAMETRIC. */
  double transfer_params[GIMG_TRANSFER_PARAM_COUNT];
  double gamma_value; ///< Used when transfer == GIMG_TRANSFER_GAMMA.
  GIMG_Reference reference;
  GIMG_Sample_Scale sample_scale;
  /**
   * Luminance in cd/m^2 of a full-scale sample, or 0 when unstated.
   *
   * Meaningful when @a sample_scale is GIMG_SAMPLE_SCALE_ABSOLUTE, where it
   * is what turns a stored value into a measurement.  Zero is unambiguous as
   * "unstated": a file does not describe a display of no luminance.
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
  uint8_t _reserved[12];
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
 * nowhere to put one - is matched on its primaries alone.  A gamut with no
 * primaries matches nothing, so a lone white point never names a space.
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
 * of its own just to say "this file said sRGB".  Sets both flags, a named
 * space stating all four points; clears both when @p named has no
 * coordinates here.
 *
 * @param info The colour info to modify.  NULL is ignored.
 * @param named The space to store.
 * @return true when the gamut was set.
 */
GIMG_API bool gimg_color_info_set_gamut(
    GIMG_Color_Info * info, GIMG_Primaries named);

/**
 * @brief The reference, scale and peak luminance a named transfer implies.
 *
 * PQ is defined against absolute luminance and HLG against scene light, so
 * for those the transfer function settles all three; sRGB and its kin are
 * display-referred and relative by convention.  Keeping that in one place
 * stops each codec deciding it again, which is how the gamut tables drifted.
 *
 * LINEAR, GAMMA and PARAMETRIC are **not** answered here: a linear raster can
 * be scene- or display-referred and the curve does not say which, so a codec
 * that knows must set the fields itself.
 *
 * @param transfer The transfer function.
 * @param out_reference Receives the reference, or UNKNOWN. May be NULL.
 * @param out_scale Receives the scale, or UNKNOWN. May be NULL.
 * @param out_white_luminance Receives cd/m^2 at full scale, or 0 when the
 *   transfer does not fix one. May be NULL.
 * @return true when @p transfer settles the question, false otherwise (in
 *   which case the outputs are set to unknown and 0).
 */
GIMG_API bool gimg_transfer_conventions(GIMG_Transfer transfer,
    GIMG_Reference * out_reference, GIMG_Sample_Scale * out_scale,
    double * out_white_luminance);

/**
 * @brief Initialize color info to unknown/default.
 */
GIMG_API void gimg_color_info_default(GIMG_Color_Info * info);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GIMG_COLOR_H

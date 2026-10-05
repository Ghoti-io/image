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
 * Basic operations: orientation, pixel format conversion, alpha (spec §8).
 */

#ifndef GHOTI_IO_GIMG_OPS_H
#define GHOTI_IO_GIMG_OPS_H

#include <ghoti.io/image/core.h>
#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/raster.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Apply orientation to raster (from metadata or explicit value).
 * @param raster Raster to transform (in-place).
 * @param orientation Source orientation (e.g. from GIMG_META_COMMON).
 * @return GIMG_OK or GIMG_ERR_UNSUPPORTED for unimplemented orientation.
 */
GIMG_API GIMG_Result gimg_ops_apply_orientation(
    GIMG_Raster * raster, GIMG_Orientation orientation);

/** @name Mirrors and quarter turns
 *
 * These are the six transforms of CIPA DC-008 Table 6 that actually move
 * pixels, under the names a caller reaches for. Each forwards to
 * gimg_ops_apply_orientation(); there is one implementation, not two, because
 * two copies of one index remap drift apart.
 *
 * All six work in place on any format whose pixel is a whole number of bytes.
 * The four that exchange the axes rebuild the buffer, so the raster's
 * dimensions and stride change while the pointer stays valid.
 * @{
 */

/** @brief Mirror left to right. */
GIMG_API GIMG_Result gimg_ops_flip_horizontal(GIMG_Raster * raster);
/** @brief Mirror top to bottom. */
GIMG_API GIMG_Result gimg_ops_flip_vertical(GIMG_Raster * raster);
/** @brief Turn a quarter clockwise; width and height exchange. */
GIMG_API GIMG_Result gimg_ops_rotate_90_cw(GIMG_Raster * raster);
/** @brief Turn a quarter anticlockwise; width and height exchange. */
GIMG_API GIMG_Result gimg_ops_rotate_90_ccw(GIMG_Raster * raster);
/** @brief Turn a half. */
GIMG_API GIMG_Result gimg_ops_rotate_180(GIMG_Raster * raster);
/** @} */

/** @brief How a source raster is combined with what is already there. */
typedef enum {
  /** Replace. The destination under the source is discarded. */
  GIMG_COMPOSITE_SOURCE = 0,
  /**
   * Porter-Duff "over": the source covers the destination in proportion to
   * its own alpha. Requires an RGBA format at 8 or 16 bits - a format with no
   * alpha has not said what covers what, and nothing would distinguish the
   * result from a plain copy.
   */
  GIMG_COMPOSITE_OVER
} GIMG_Composite_Op;

/**
 * @brief Draw @p src onto @p dst at an offset, in place.
 *
 * The offsets are **signed**, and the source is clipped to the destination, so
 * a source may hang off any edge. One lying entirely outside is not an error:
 * it draws nothing and returns GIMG_OK, because a frame scrolled off the
 * canvas is an ordinary thing for an animation to do.
 *
 * The two rasters must have **the same pixel format**. Converting on the
 * caller's behalf would change an image's colour without being asked, which
 * nothing else here does either; convert first with
 * gimg_ops_convert_pixel_format() or gimg_ops_convert_bit_depth().
 *
 * GIMG_COMPOSITE_SOURCE works for any format whose pixel is a whole number of
 * bytes. GIMG_COMPOSITE_OVER needs RGBA at 8 or 16 bits.
 *
 * Alpha is straight, not premultiplied, on the way in and on the way out.
 * Where the result is wholly transparent the colour channels are set to zero
 * rather than left holding the destination's, so that compositing the same
 * source over two different destinations gives the same bytes wherever
 * nothing is visible.
 *
 * @param dst Destination raster, modified in place.
 * @param src Source raster.
 * @param x Horizontal offset of the source's left edge within @p dst.
 * @param y Vertical offset of the source's top edge within @p dst.
 * @param op Which combination to perform.
 * @return GIMG_OK (including when the source lies entirely outside);
 *   GIMG_ERR_INTERNAL for a null argument; GIMG_ERR_UNSUPPORTED for
 *   mismatched formats, an unknown operator, or OVER without alpha.
 */
GIMG_API GIMG_Result gimg_ops_composite(GIMG_Raster * dst,
    const GIMG_Raster * src, int32_t x, int32_t y, GIMG_Composite_Op op);

/**
 * @brief How the samples between two pixels are combined when resizing.
 *
 * These are kernels in one separable resampler rather than five different
 * algorithms, and the choice between them is **semantic, not aesthetic**.
 * GIMG_FILTER_NEAREST returns a sample that was in the source; every other
 * entry returns a weighted average, which is a value that was not. For a mask,
 * an index map, or anything whose samples are labels, the average of two of
 * them is not a label, and only NEAREST is correct.
 *
 * When an axis is reduced, every filter but NEAREST is stretched by the
 * reduction ratio so that it averages over the whole source region feeding one
 * destination pixel. Without that a downscale is a decimation.
 */
typedef enum {
  /**
   * The library's general-purpose choice: currently GIMG_FILTER_CATMULL_ROM.
   *
   * It is the zero value, so `GIMG_Resize_Options opts = {0};` means "pick
   * well for me" rather than selecting a filter by accident.
   *
   * **This is a pinned alias, not a judgement free to drift.** An AUTO that
   * changed kernel between releases would change the bytes a caller gets for
   * unchanged input, where only a comparison against an old output would find
   * it. Changing what it maps to is a behaviour change and is treated as one.
   *
   * **AUTO never selects NEAREST.** Nothing about a raster distinguishes
   * samples that are colours from samples that are labels, and deciding it
   * from the picture's contents is the judgement that keeps
   * gimg_ops_quantize() outside the codecs. A caller whose samples are labels
   * names the filter.
   */
  GIMG_FILTER_AUTO = 0,
  /** Take the nearest source sample. Never averages, never invents a value. */
  GIMG_FILTER_NEAREST,
  /**
   * Unweighted mean of the source region. At an integer reduction this is the
   * exact area average, which is what a thumbnail usually wants. Note that at
   * ratios near 1:1 its support covers a single sample, so it behaves as
   * NEAREST does - it is a filter for reducing by a real factor, not for
   * trimming a few pixels.
   */
  GIMG_FILTER_BOX,
  /** Linear interpolation; a tent kernel of radius 1. */
  GIMG_FILTER_TRIANGLE,
  /** The a = -0.5 cubic: sharper than TRIANGLE, and interpolating. */
  GIMG_FILTER_CATMULL_ROM,
  /** Windowed sinc of radius 3. The sharpest here, and the one that rings. */
  GIMG_FILTER_LANCZOS3,
  GIMG_FILTER_COUNT
} GIMG_Resample_Filter;

/**
 * @brief Which values the resampler averages.
 */
typedef enum {
  /**
   * Average the stored values as they are. This is what Pillow, ImageMagick's
   * default, GdkPixbuf and browsers do, and it is the only setting under
   * which this library's output can be compared with theirs.
   */
  GIMG_RESAMPLE_SPACE_ENCODED = 0,
  /**
   * Linearize, average, re-encode. More nearly correct - averaging non-linear
   * values darkens - and **opt-in**. The curve comes from the raster's
   * GCOL_Color_Info via libs/color: sRGB, BT.1886, PQ, HLG, a plain gamma,
   * or a complete parametric. An unstated transfer is still the caller's
   * assertion of sRGB; GCOL_Color_Info is not consulted to *infer* one, for
   * the same reason the CMYK conversion refuses to guess a polarity.
   *
   * A stated transfer that color cannot evaluate - GAMMA without a positive
   * gamma_value, PARAMETRIC without a positive g term - is refused with
   * GIMG_ERR_UNSUPPORTED rather than treated as identity. So is a channel
   * model that is not light (CMYK, UNKNOWN): the curve says something about
   * display-referred colour and those samples are not that.
   *
   * The primaries are not consulted at all: linearisation is per-channel and
   * depends on the transfer curve alone, so a Display P3 raster - whose
   * transfer *is* sRGB's - is resampled here correctly and is not refused.
   */
  GIMG_RESAMPLE_SPACE_LINEAR
} GIMG_Resample_Space;

/** @brief Options for gimg_ops_resize(). */
typedef struct {
  GIMG_Resample_Filter filter; ///< Default GIMG_FILTER_AUTO.
  GIMG_Resample_Space space;   ///< Default GIMG_RESAMPLE_SPACE_ENCODED.
  uint8_t _reserved[8]; ///< Zero; room to grow.
} GIMG_Resize_Options;

/**
 * @brief Fill @p options with the defaults.
 * @param options Struct to initialise; zeroed first, so a field added later
 *   is defaulted rather than left holding the caller's stack.
 */
GIMG_API void gimg_resize_options_default(GIMG_Resize_Options * options);

/**
 * @brief Resample a raster to a new size.
 *
 * Supported: GIMG_CHANNEL_GRAY, GIMG_CHANNEL_RGBA, GIMG_CHANNEL_CMYK and
 * GIMG_CHANNEL_UNKNOWN, interleaved, at 8, 12 or 16 bits per channel with
 * every channel the same width - the same matrix gimg_ops_convert_bit_depth()
 * declares. Anything else returns GIMG_ERR_UNSUPPORTED, GIMG_CHANNEL_INDEXED
 * included: the average of two palette indices is not a palette index, so a
 * caller resizes the colour form and calls gimg_ops_quantize() afterwards.
 *
 * **RGBA is filtered premultiplied.** Averaging straight alpha lets a
 * transparent pixel contribute its colour to opaque neighbours, so edges
 * against transparency pick up a halo of whatever was hiding underneath.
 * The source is not modified; the premultiplication happens on the way into
 * the filter and is undone on the way out.
 *
 * The result carries the source's GCOL_Color_Info, embedded profile included.
 *
 * @param src Source raster.
 * @param dst_width Target width; must be above zero.
 * @param dst_height Target height; must be above zero.
 * @param options Filter and colour space; NULL means the defaults.
 * @param out_raster On success, a new raster; the caller owns it. Set to NULL
 *   on every failure.
 * @return GIMG_OK; GIMG_ERR_INTERNAL for a null argument or a zero
 *   dimension; GIMG_ERR_UNSUPPORTED for a format or option outside the above;
 *   GIMG_ERR_LIMIT if the coefficient table overflows; GIMG_ERR_OOM.
 */
GIMG_API GIMG_Result gimg_ops_resize(const GIMG_Raster * src,
    uint32_t dst_width, uint32_t dst_height,
    const GIMG_Resize_Options * options, GIMG_Raster ** out_raster);

/**
 * @brief Cut a rectangle out of a raster.
 *
 * The rectangle is given in pixels from the top-left corner and must lie
 * wholly inside the source; a rectangle that leaves it, or one of zero width
 * or height, returns GIMG_ERR_INTERNAL rather than being clamped to fit.
 * Silently returning a smaller image than was asked for is the kind of
 * accommodation that hides an off-by-one in the caller's arithmetic for as
 * long as nobody checks the dimensions.
 *
 * No sample is interpreted, only moved, so this works for **any** format
 * whose pixel occupies a whole number of bytes - every channel model, every
 * depth, planar or interleaved. That is wider than gimg_ops_resize() reaches,
 * which has to know what a sample means in order to average two of them.
 *
 * The result carries the source's GCOL_Color_Info, embedded profile included:
 * showing less of a picture does not change what its samples mean.
 *
 * To mirror or rotate instead, use gimg_ops_apply_orientation(), which
 * implements all eight of the CIPA DC-008 Table 6 transforms.
 *
 * @param src Source raster.
 * @param x Left edge of the rectangle, in pixels.
 * @param y Top edge of the rectangle, in pixels.
 * @param width Width of the rectangle; must be above zero.
 * @param height Height of the rectangle; must be above zero.
 * @param out_raster On success, a new raster holding the rectangle; the
 *   caller owns it. Set to NULL on every failure.
 * @return GIMG_OK; GIMG_ERR_INTERNAL for a null argument or a rectangle that
 *   is empty or does not lie inside the source; GIMG_ERR_UNSUPPORTED for a
 *   format with no whole-byte pixel size; GIMG_ERR_LIMIT if the row length
 *   overflows; GIMG_ERR_OOM.
 */
GIMG_API GIMG_Result gimg_ops_crop(const GIMG_Raster * src, uint32_t x,
    uint32_t y, uint32_t width, uint32_t height, GIMG_Raster ** out_raster);

/**
 * @brief Convert pixel format: a same-format copy, or CMYK to RGBA.
 *
 * A **same-format** conversion copies the samples, and the result carries the
 * source's GCOL_Color_Info, profile included: copying samples does not change
 * what they mean.
 *
 * **CMYK to RGBA** at the same sample width (CMYK8 to RGBA8, CMYK12 to
 * RGBA12, CMYK16 to RGBA16) performs the naive conversion: each ink is taken
 * as an independent multiplicative filter over white, so a channel is the
 * product of its own colourant and the black, rounded to nearest. This exists
 * because neither PNG nor BMP has CMYK, so without it a four-component JPEG
 * could not be converted into anything at all.
 *
 * It is **not colorimetric**. A colorimetric conversion runs the samples
 * through the source profile and a destination via ::gimg_ops_transform_color.
 * This path stays the naive one so it continues to agree with Pillow and
 * libjpeg-based tools byte for byte on every CMYK and YCCK fixture in
 * tests/data/jpeg.
 *
 * The source's `cmyk_polarity` must say which way round the samples are:
 * GCOL_CMYK_POLARITY_UNKNOWN returns GIMG_ERR_UNSUPPORTED rather than a
 * guess, because the two readings are negatives of each other and the wrong
 * one gives a plausible but inverted picture. The JPEG decoder always states
 * it.
 *
 * The result is **opaque** and carries **no** GCOL_Color_Info: what the source
 * said described four ink amounts, and none of it - an embedded profile least
 * of all - is true of the three-channel result.
 *
 * No writer performs this conversion on your behalf. Saving a CMYK raster as
 * a PNG or a BMP still returns GIMG_ERR_UNSUPPORTED, so the library never
 * changes an image's colour without being asked.
 *
 * @param src Source raster.
 * @param dst_format Target format descriptor.
 * @param out_raster On success, new raster in target format.
 * @return GIMG_OK, or GIMG_ERR_UNSUPPORTED for any other pair of formats, for
 *   a CMYK source with no stated polarity, or for a width change (that is
 *   gimg_ops_convert_bit_depth's job).
 */
GIMG_API GIMG_Result gimg_ops_convert_pixel_format(const GIMG_Raster * src,
    const GIMG_Pixel_Format * dst_format, GIMG_Raster ** out_raster);

/**
 * @brief Convert raster bit depth (8, 12, or 16 bits per channel). Same
 * channel model and count; uses library bit-depth conversion (bitshift/clamp).
 *
 * Supported: GRAY, RGBA and CMYK at 8/12/16, and a raster of unnamed channels
 * (GIMG_CHANNEL_UNKNOWN) at any count. Other channel models, a source whose
 * channels are not all the same width, and any depth but 8, 12 or 16 return
 * GIMG_ERR_UNSUPPORTED.
 *
 * The result carries the source's GCOL_Color_Info, profile included: a sample
 * restated at a different precision still means what it meant.
 *
 * @param src Source raster (8-, 12-, or 16-bit per channel).
 * @param dst_bits Target bits per channel (8, 12, or 16).
 * @param out_raster On success, new raster in target bit depth; caller owns it.
 */
GIMG_API GIMG_Result gimg_ops_convert_bit_depth(const GIMG_Raster * src,
    uint8_t dst_bits, GIMG_Raster ** out_raster);

/**
 * @brief Options for ::gimg_ops_transform_color.
 *
 * @a dest must state a usable destination: an ICC profile (`icc_bytes`), or
 * primaries, white point and transfer. Intent defaults to relative
 * colorimetric. @a flags are passed to libs/color (::GCOL_TRANSFORM_CLIP,
 * ::GCOL_TRANSFORM_BPC, ::GCOL_TRANSFORM_TONE_MAP).
 */
typedef struct {
  GCOL_Color_Info dest;           ///< Destination colour description.
  GCOL_Rendering_Intent intent;   ///< Rendering intent; defaults to relative colorimetric.
  uint32_t flags;                 ///< GCOL_TRANSFORM_* flags passed to libs/color.
  uint8_t _reserved[8];           ///< Zero; room to grow.
} GIMG_Color_Transform_Options;

/**
 * @brief Zero @p options, default dest, relative colorimetric, flags 0.
 *
 * Dest starts UNKNOWN — call ::gimg_color_transform_options_srgb or fill
 * @a dest before transforming.
 */
GIMG_API void gimg_color_transform_options_default(
    GIMG_Color_Transform_Options * options);

/**
 * @brief Defaults plus dest = IEC 61966-2.1 sRGB (display-referred RGB).
 *
 * The common "make this displayable" request. Relative colorimetric, flags 0.
 */
GIMG_API void gimg_color_transform_options_srgb(
    GIMG_Color_Transform_Options * options);

/**
 * @brief Convert samples from the raster's colour space into @p options->dest.
 *
 * Load and save still only carry colour. This is the explicit CMM step,
 * matching Pillow ImageCms / WIC / ImageMagick `-profile`: nothing moves
 * until the caller asks.
 *
 * Source is taken from the raster's ::GCOL_Color_Info. Embedded (or
 * resolver-supplied) `icc_bytes` are parsed and used; otherwise stated
 * primaries, white and transfer feed the matrix path. Untagged CMYK, and
 * RGB with neither a profile nor a complete description, are refused rather
 * than guessed.
 *
 * v1 buffers: interleaved RGBA8 and CMYK8 only. Other depths return
 * ::GIMG_ERR_UNSUPPORTED (no silent narrow). CMYK sources need a stated
 * ::GCOL_CMYK_Polarity. The result is RGBA8 for an RGB destination (alpha
 * preserved from an RGBA source, opaque otherwise) or CMYK8 for a CMYK
 * destination profile, and carries @a options->dest.
 *
 * @param src Source raster.
 * @param options Destination and intent; NULL is refused (no implied sRGB).
 * @param out_raster On success, a new raster; set to NULL on every failure.
 */
GIMG_API GIMG_Result gimg_ops_transform_color(const GIMG_Raster * src,
    const GIMG_Color_Transform_Options * options, GIMG_Raster ** out_raster);

/**
 * @brief Premultiply alpha (straight -> premultiplied) (spec §4.4).
 * @param raster RGBA raster (in-place).
 * @return GIMG_OK or GIMG_ERR_UNSUPPORTED if format not supported.
 */
GIMG_API GIMG_Result gimg_alpha_premultiply(GIMG_Raster * raster);

/**
 * @brief Unpremultiply alpha (premultiplied -> straight).
 * @param raster RGBA raster (in-place).
 * @return GIMG_OK or GIMG_ERR_UNSUPPORTED if format not supported.
 */
GIMG_API GIMG_Result gimg_alpha_unpremultiply(GIMG_Raster * raster);

/** @name Palettes and colour reduction
 *
 * Every format in this library that stores an image through a colour table -
 * GIF, palette PNG (PNG 11.2.2 colour type 3), palette BMP - caps that table
 * at 256 entries, and every one of their writers refuses an image with more
 * colours than that rather than deciding which to throw away.  That decision
 * lives here.
 *
 * **The reduced image is a raster, not a set of indices.**
 * gimg_ops_quantize() hands back a raster in the source's own pixel format
 * whose pixels happen to take at most @a max_colors distinct values.  Nothing
 * downstream has to learn a new type: a writer that already refuses a
 * 300,000-colour image and accepts a 256-colour one accepts this one, through
 * the same code path, having been told nothing.  Which is also what keeps the
 * codecs honest - they still never quantize, they are handed an image that is
 * already within budget, and a caller can see in their own code that colours
 * were discarded and by what.
 *
 * **An image already within budget comes back with its exact colours.**  That
 * is checked before anything is reduced, so quantizing is safe to call
 * unconditionally: below the limit it is a copy, and the palette it reports is
 * the one the writer would have built for itself.
 *
 * Building a palette and applying one are separate calls, because an animation
 * needs one table for every frame - quantizing frames independently gives each
 * its own colours and makes the animation shimmer.  gimg_ops_palette_build()
 * takes as many rasters as the caller has; gimg_ops_palette_apply() maps each
 * one onto the result.
 *
 * @{
 */

/** The largest colour table any format in this library can store. */
#define GIMG_PALETTE_MAX_ENTRIES 256u

/**
 * @brief A colour table: what a palette format stores, in one struct.
 *
 * Entries are RGBA8 whatever the source raster was, because that is what the
 * formats hold: a PNG `PLTE` entry is three 8-bit samples with its alpha in
 * `tRNS` (PNG 11.2.2, 11.3.2.1), a GIF Global or Local Color Table is three
 * 8-bit samples with one index designated transparent (89a 18, 23), and a BMP
 * palette entry is four bytes of which one is unused.
 *
 * A plain value with no allocation: it is small enough to pass by pointer and
 * copy by assignment, and a caller holding one does not have to remember to
 * free it.
 */
typedef struct {
  uint8_t entries[GIMG_PALETTE_MAX_ENTRIES][4]; ///< R, G, B, A per entry.
  uint16_t count;                               ///< Entries in use, 1 to 256.
  uint8_t _reserved[6];                         ///< Zero; room to grow.
} GIMG_Palette;

/**
 * @brief How the colours to keep are chosen.
 */
typedef enum {
  /**
   * Median cut (Heckbert, SIGGRAPH 1982).  The colours present are put in one
   * box; the box worth splitting most is cut at the median of its longest
   * axis; repeat until there are as many boxes as entries wanted.  Each box
   * contributes the average of the colours in it, weighted by how many pixels
   * carry them.
   */
  GIMG_QUANTIZE_MEDIAN_CUT = 0,
  GIMG_QUANTIZE_METHOD_COUNT
} GIMG_Quantize_Method;

/**
 * @brief What to do with the difference between a pixel and the entry chosen
 * for it.
 */
typedef enum {
  /** Round each pixel to its nearest entry and discard the difference.
   * Flat regions stay flat; a gradient gets visible bands. */
  GIMG_DITHER_NONE = 0,
  /**
   * Floyd-Steinberg error diffusion: the difference is carried into the
   * neighbours not yet visited, 7/16 right, 3/16 down-left, 5/16 down, 1/16
   * down-right, scanning alternate rows in alternate directions so the
   * pattern does not drift.  Bands become texture.  It costs a nearest-entry
   * search per pixel rather than per distinct colour, which is the slower of
   * the two by roughly the ratio between them.
   */
  GIMG_DITHER_FLOYD_STEINBERG,
  GIMG_DITHER_COUNT
} GIMG_Dither;

/**
 * @brief What a colour reduction should do.
 *
 * Zero-initializing this struct asks for the default of every field: 256
 * colours, median cut, no dithering.  Which is also the reduction that changes
 * an image already within budget not at all.
 */
typedef struct {
  /** Entries the palette may use, 1 to 256.  0 means 256. */
  uint16_t max_colors;
  GIMG_Quantize_Method method; ///< How to choose them.
  GIMG_Dither dither;          ///< What to do with the rounding error.
  uint8_t _reserved[8];        ///< Zero; room to grow.
} GIMG_Quantize_Options;

/**
 * @brief How many distinct colours an image has, giving up at @a limit.
 *
 * The question a caller asks before deciding whether to reduce at all, and the
 * question every palette writer in this library already asks itself.  Counting
 * stops as soon as the answer is known to exceed @a limit, because past that
 * point the exact number costs more to find than it is worth: a photograph has
 * hundreds of thousands and the caller only wanted to know it was too many.
 *
 * Two pixels are the same colour when all four of their RGBA samples match,
 * except that **every fully transparent pixel counts as one colour** whatever
 * lies under it.  That is the rule the GIF, PNG and BMP writers use, and the
 * rule gimg_ops_quantize() applies: an invisible colour is not a colour.
 *
 * @param src RGBA8 or GRAY8 raster.
 * @param limit Stop counting here.  0 asks for the full count.
 * @param out_count Colours found; equal to @a limit when it gave up.
 * @param out_exact Optional: false when it gave up, true when @a out_count is
 *   the whole answer.
 * @return GIMG_OK, GIMG_ERR_UNSUPPORTED for another format, or GIMG_ERR_OOM.
 */
GIMG_API GIMG_Result gimg_ops_count_colors(const GIMG_Raster * src,
    size_t limit, size_t * out_count, bool * out_exact);

/**
 * @brief The exact colour table of an image that already has few enough
 * colours.
 *
 * Nothing is chosen or discarded: when an image has @a max_colors colours or
 * fewer there is exactly one palette that reproduces it, up to the order of
 * its entries, and this is that palette in the order the colours first appear.
 * An image with more returns GIMG_ERR_UNSUPPORTED, which is the same answer
 * the palette writers give and the signal to call gimg_ops_quantize().
 *
 * @param src RGBA8 or GRAY8 raster.
 * @param max_colors 1 to 256; 0 means 256.
 * @param out_palette Filled on success.
 * @return GIMG_OK, GIMG_ERR_UNSUPPORTED (too many colours, or another pixel
 *   format), or GIMG_ERR_OOM.
 */
GIMG_API GIMG_Result gimg_ops_palette_from_raster(
    const GIMG_Raster * src, uint16_t max_colors, GIMG_Palette * out_palette);

/**
 * @brief Choose a colour table for one or more images.
 *
 * Several rasters rather than one because the frames of an animation have to
 * share a table: quantized separately, a region that does not change between
 * two frames still gets slightly different colours in each, and the animation
 * shimmers. Pass every frame here, then gimg_ops_palette_apply() to each.
 *
 * The rasters need not agree in size, but they must agree in pixel format.
 * The colours of all of them are held at once while the palette is chosen,
 * which for photographic frames is where the memory goes; GIMG_ERR_OOM is the
 * answer when they will not fit, rather than a quietly coarser result.
 *
 * When the images between them have @a max_colors colours or fewer, the
 * palette is those colours exactly and nothing is averaged.
 *
 * @param src Array of @a count raster pointers, none NULL.
 * @param count Rasters in @a src, at least 1.
 * @param options May be NULL for the defaults.
 * @param out_palette Filled on success.
 * @return GIMG_OK, GIMG_ERR_UNSUPPORTED for an unsupported pixel format or
 *   rasters that disagree about it, GIMG_ERR_OOM, or GIMG_ERR_INTERNAL.
 */
GIMG_API GIMG_Result gimg_ops_palette_build(const GIMG_Raster * const * src,
    size_t count, const GIMG_Quantize_Options * options,
    GIMG_Palette * out_palette);

/**
 * @brief Map an image onto a palette, returning a raster in the same format.
 *
 * Every pixel becomes the entry nearest it, so the result holds no colour that
 * is not in @a palette.  Nearest is by squared distance over R, G, B and A
 * together, with alpha counted like a colour channel: a pixel is as wrong for
 * being opaque when it should be clear as for being red when it should be
 * green.
 *
 * Use it with gimg_ops_palette_build() to give several images one table, or on
 * its own to impose a table the caller already has - a brand palette, a
 * previous frame's table, the palette a file arrived with.
 *
 * @param src RGBA8 or GRAY8 raster.
 * @param palette Table to map onto; `count` must be at least 1.
 * @param dither What to do with the rounding error.
 * @param out_raster On success, a new raster in @a src's format and size;
 *   caller owns it.  It carries @a src's GCOL_Color_Info: rounding a sample to
 *   a nearby one does not change what the samples mean.
 * @return GIMG_OK, GIMG_ERR_UNSUPPORTED, GIMG_ERR_INTERNAL, or GIMG_ERR_OOM.
 */
GIMG_API GIMG_Result gimg_ops_palette_apply(const GIMG_Raster * src,
    const GIMG_Palette * palette, GIMG_Dither dither,
    GIMG_Raster ** out_raster);

/**
 * @brief Reduce an image to at most @a max_colors colours.
 *
 * gimg_ops_palette_build() followed by gimg_ops_palette_apply(), for the
 * common case of one image.  The result is a raster in @a src's own pixel
 * format, which is what makes this useful: hand it to any writer in this
 * library and the writer builds the palette it would have built anyway.
 *
 * @code
 * // Save a photograph as a GIF.
 * GIMG_Quantize_Options q = {0};   // 256 colours, median cut, no dither
 * q.dither = GIMG_DITHER_FLOYD_STEINBERG;
 * GIMG_Raster * reduced = NULL;
 * if (gimg_ops_quantize(photo, &q, &reduced, NULL) == GIMG_OK) {
 *   // ... put `reduced` in a document and save it as "gif" ...
 *   gimg_raster_destroy(reduced);
 * }
 * @endcode
 *
 * An image that already has @a max_colors colours or fewer is copied
 * unchanged, so calling this without first asking gimg_ops_count_colors()
 * costs a comparison per pixel and loses nothing.
 *
 * @param src RGBA8 or GRAY8 raster.
 * @param options May be NULL for the defaults.
 * @param out_raster On success, a new raster; caller owns it.
 * @param out_palette Optional: the table that was chosen.
 * @return GIMG_OK, GIMG_ERR_UNSUPPORTED, or GIMG_ERR_OOM.
 */
GIMG_API GIMG_Result gimg_ops_quantize(const GIMG_Raster * src,
    const GIMG_Quantize_Options * options, GIMG_Raster ** out_raster,
    GIMG_Palette * out_palette);

/** @} */

/**
 * @brief Compare two rasters: dimensions, format, and pixel data must match.
 * Strides may differ; comparison is row-by-row over pixel bytes. Color info is
 * not compared.
 * @return true if equal, false if either is NULL or dimensions/format/pixels
 * differ.
 */
GIMG_API bool gimg_ops_raster_equal(
    const GIMG_Raster * a, const GIMG_Raster * b);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GIMG_OPS_H

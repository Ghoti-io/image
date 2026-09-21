/**
 * @file
 *
 * Basic operations: orientation, pixel format conversion, alpha (spec §8).
 *
 * Copyright 2026 by Corey Pennycuff
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

/**
 * @brief Convert pixel format: a same-format copy, or CMYK to RGBA.
 *
 * A **same-format** conversion copies the samples, and the result carries the
 * source's GIMG_Color_Info, profile included: copying samples does not change
 * what they mean.
 *
 * **CMYK to RGBA** at the same sample width (CMYK8 to RGBA8, CMYK12 to
 * RGBA12, CMYK16 to RGBA16) performs the naive conversion: each ink is taken
 * as an independent multiplicative filter over white, so a channel is the
 * product of its own colourant and the black, rounded to nearest. This exists
 * because neither PNG nor BMP has CMYK, so without it a four-component JPEG
 * could not be converted into anything at all.
 *
 * It is **not colorimetric**. A real conversion would run the samples through
 * the source profile and a destination profile, and this library has no colour
 * engine; the choice it offers is between the naive conversion and none. It is
 * what libjpeg-based tools do, and it agrees with Pillow exactly on every
 * pixel of every CMYK and YCCK fixture in tests/data/jpeg.
 *
 * The source's `cmyk_polarity` must say which way round the samples are:
 * GIMG_CMYK_POLARITY_UNKNOWN returns GIMG_ERR_UNSUPPORTED rather than a
 * guess, because the two readings are negatives of each other and the wrong
 * one gives a plausible but inverted picture. The JPEG decoder always states
 * it.
 *
 * The result is **opaque** and carries **no** GIMG_Color_Info: what the source
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
 * The result carries the source's GIMG_Color_Info, profile included: a sample
 * restated at a different precision still means what it meant.
 *
 * @param src Source raster (8-, 12-, or 16-bit per channel).
 * @param dst_bits Target bits per channel (8, 12, or 16).
 * @param out_raster On success, new raster in target bit depth; caller owns it.
 */
GIMG_API GIMG_Result gimg_ops_convert_bit_depth(const GIMG_Raster * src,
    uint8_t dst_bits, GIMG_Raster ** out_raster);

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
 *   caller owns it.  It carries @a src's GIMG_Color_Info: rounding a sample to
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

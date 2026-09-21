/**
 * @file
 *
 * Internal GIF codec structures: the logical screen, the per-frame graphic
 * control and image descriptors, and the document state carried from load to
 * decode.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GIMG_SRC_CODEC_GIF_GIF_INTERNAL_H
#define GHOTI_IO_GIMG_SRC_CODEC_GIF_GIF_INTERNAL_H

#include <ghoti.io/image/macros.h>

#include <ghoti.io/image/allocator.h>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/color.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Length of the Header block (87a 17): "GIF" followed by a three-byte version.
 * The probe matches only the first three, because the version is not a magic -
 * a file may say "89a" and use nothing 89a added.
 */
#define GIMG_GIF_SIGNATURE_LEN 3

/** Header block length: the signature and the version that follows it. */
#define GIMG_GIF_HEADER_LEN 6

/** GIF signature bytes ('G', 'I', 'F'). */
extern const unsigned char gimg_gif_signature[GIMG_GIF_SIGNATURE_LEN];

/** Logical Screen Descriptor length (87a 18). */
#define GIMG_GIF_LSD_LEN 7

/** Image Descriptor length after the 0x2C separator (87a 20). */
#define GIMG_GIF_IMAGE_DESCRIPTOR_LEN 9

/** @name Block introducers (89a 15, 16, 23, 24, 25, 26, 27).
 * @{ */
#define GIMG_GIF_BLOCK_EXTENSION 0x21u  ///< Extension Introducer.
#define GIMG_GIF_BLOCK_IMAGE 0x2Cu      ///< Image Separator.
#define GIMG_GIF_BLOCK_TRAILER 0x3Bu    ///< Trailer.
/** @} */

/** @name Extension labels (89a 23, 24, 25, 26).
 * @{ */
#define GIMG_GIF_EXT_PLAIN_TEXT 0x01u   ///< Plain Text Extension.
#define GIMG_GIF_EXT_GRAPHIC_CONTROL 0xF9u ///< Graphic Control Extension.
#define GIMG_GIF_EXT_COMMENT 0xFEu      ///< Comment Extension.
#define GIMG_GIF_EXT_APPLICATION 0xFFu  ///< Application Extension.
/** @} */

/**
 * The largest colour table any GIF can carry: the size field is three bits, so
 * 3 << (7 + 1) bytes, which is 256 entries of red, green and blue (89a 18).
 */
#define GIMG_GIF_MAX_PALETTE 256u

/**
 * The largest LZW code size a GIF may declare (89a 22).  The code stream grows
 * from this width to twelve bits; a larger initial width has nowhere to grow
 * and no decoder accepts one.
 */
#define GIMG_GIF_MAX_LZW_MIN_CODE_SIZE 11u

/** @name Disposal methods, the three bits of the Graphic Control Extension's
 * packed field (89a 23).
 * @{ */
#define GIMG_GIF_DISPOSAL_UNSPECIFIED 0u ///< No disposal specified.
#define GIMG_GIF_DISPOSAL_NONE 1u        ///< Leave the frame in place.
#define GIMG_GIF_DISPOSAL_BACKGROUND 2u  ///< Restore to background colour.
#define GIMG_GIF_DISPOSAL_PREVIOUS 3u    ///< Restore to what was there before.
/** @} */

/** A colour table entry: three bytes, red first (89a 18). */
typedef struct {
  uint8_t r; ///< Red.
  uint8_t g; ///< Green.
  uint8_t b; ///< Blue.
} gimg_gif_rgb_t;

/**
 * One image in the stream, together with the Graphic Control Extension that
 * preceded it if there was one.
 *
 * The specification treats the two as separate blocks, and a GIF may hold an
 * image with no control block at all; carrying them in one structure is this
 * codec's arrangement, not the format's, and the `has_control` flag is what
 * keeps the distinction where it matters.
 */
typedef struct {
  uint16_t left;   ///< Column of the left edge on the logical screen.
  uint16_t top;    ///< Row of the top edge on the logical screen.
  uint16_t width;  ///< Image width in pixels.
  uint16_t height; ///< Image height in pixels.
  bool interlaced; ///< Rows are stored in the four-pass order (89a 20).

  /** The colour table this image is read through: its own when it carried a
   * Local Color Table, otherwise a copy of the Global one.  Resolved at load
   * so that decode never has to reach back to the screen descriptor. */
  gimg_gif_rgb_t palette[GIMG_GIF_MAX_PALETTE];
  uint16_t palette_count;   ///< Entries in `palette`; 0 when none was found.
  bool has_local_palette;   ///< The table above came from this image.

  bool has_control;         ///< A Graphic Control Extension preceded it.
  uint8_t disposal;         ///< GIMG_GIF_DISPOSAL_*; meaningless without it.
  bool has_transparency;    ///< The control block set the transparency flag.
  uint8_t transparent_index; ///< Index to render as transparent, if it did.
  uint16_t delay_cs;        ///< Delay in hundredths of a second.

  uint8_t lzw_min_code_size; ///< Initial LZW code width (89a 22).
  unsigned char * lzw;       ///< Sub-blocks concatenated into one code stream.
  size_t lzw_size;           ///< Length of `lzw`.
} gimg_gif_frame_t;

/**
 * Everything load learned, kept for decode.
 *
 * The frames hold their own colour tables, so the global one here is only
 * what the background index is resolved against.
 */
typedef struct {
  const GIMG_Allocator * allocator; ///< Allocator every field here came from.

  uint16_t canvas_width;  ///< Logical Screen Descriptor width.
  uint16_t canvas_height; ///< Logical Screen Descriptor height.

  gimg_gif_rgb_t global_palette[GIMG_GIF_MAX_PALETTE]; ///< Global Color Table.
  uint16_t global_palette_count; ///< Entries in it; 0 when absent.
  bool has_global_palette;       ///< The screen descriptor carried one.

  uint8_t background_index; ///< Background Color Index (89a 18).
  uint8_t aspect_ratio;     ///< Pixel Aspect Ratio byte, stored unmodified.

  bool has_loop;      ///< A NETSCAPE2.0 Application Extension was seen.
  uint16_t loop_count; ///< Its repeat count; 0 means forever.

  /** "87a" or "89a" as the header spelled it, with a terminating zero. */
  char version[4];

  gimg_gif_frame_t * frames; ///< One per image block, in stream order.
  size_t frame_count;        ///< Length of `frames`.
} gimg_gif_doc_state_t;

/** @brief Read and check the three signature bytes, leaving the version. */
GIMG_Result gimg_gif_verify_signature(GIMG_Stream * stream);

/** @brief Load callback: stream -> document with gimg_gif_doc_state_t. */
GIMG_Result gimg_gif_load(GIMG_Codec * codec, GIMG_Stream * stream,
    const GIMG_Load_Options * options, GIMG_Diagnostics * diagnostics,
    GIMG_Doc ** out_doc);

/** @brief Decode callback: one frame, composited onto the logical screen. */
GIMG_Result gimg_gif_decode(GIMG_Codec * codec, const GIMG_Item * item,
    const GIMG_Decode_Options * options, GIMG_Raster ** out_raster);

/** @brief Release the document state attached by load. */
void gimg_gif_free_doc_state(GIMG_Codec * codec, void * codec_private);

/**
 * @brief Expand one frame's LZW code stream to one palette index per pixel.
 *
 * De-interlaces on the way out when the frame says it is interlaced, so the
 * result is always in top-to-bottom order.
 *
 * @param frame The frame whose `lzw` bytes are expanded.
 * @param out Receives frame->width * frame->height index bytes.
 * @param out_size Length of `out`, which must be exactly that product.
 * @return GIMG_OK, GIMG_ERR_CORRUPT if the code stream does not yield that
 *         many pixels, or GIMG_ERR_OOM.
 */
GIMG_Result gimg_gif_expand_lzw(const gimg_gif_frame_t * frame,
    const GIMG_Allocator * alloc, unsigned char * out, size_t out_size);

#endif // GHOTI_IO_GIMG_SRC_CODEC_GIF_GIF_INTERNAL_H

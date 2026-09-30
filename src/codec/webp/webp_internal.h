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
 * Internal WebP codec structures. Phase A: RIFF container, VP8X, chunk walk,
 * metadata carriage and canvas geometry. Phase B: VP8L lossless decode.
 * Phase C: ALPH. Phase D: VP8 lossy keyframe decode. Phase E: ANIM/ANMF.
 * Phase F: VP8L lossless encode. Phase G: VP8 lossy stub encode.
 */

#ifndef GHOTI_IO_GIMG_SRC_CODEC_WEBP_WEBP_INTERNAL_H
#define GHOTI_IO_GIMG_SRC_CODEC_WEBP_WEBP_INTERNAL_H

#include <ghoti.io/image/macros.h>

#include <ghoti.io/image/allocator.h>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/stream.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** "RIFF" / "WEBP" at the head; twelve bytes total before the first chunk. */
#define GIMG_WEBP_SIGNATURE_LEN 12

/** Hard cap on reported chunks (top-level plus ANMF-nested bitstreams). */
#define GIMG_WEBP_MAX_CHUNKS 256u

/** FourCCs as little-endian uint32 (on-disk order). */
#define GIMG_WEBP_FOURCC(a, b, c, d) \
  ((uint32_t)(uint8_t)(a) | ((uint32_t)(uint8_t)(b) << 8) | \
      ((uint32_t)(uint8_t)(c) << 16) | ((uint32_t)(uint8_t)(d) << 24))

#define GIMG_WEBP_RIFF GIMG_WEBP_FOURCC('R', 'I', 'F', 'F')
#define GIMG_WEBP_WEBP GIMG_WEBP_FOURCC('W', 'E', 'B', 'P')
#define GIMG_WEBP_VP8X GIMG_WEBP_FOURCC('V', 'P', '8', 'X')
#define GIMG_WEBP_VP8 GIMG_WEBP_FOURCC('V', 'P', '8', ' ')
#define GIMG_WEBP_VP8L GIMG_WEBP_FOURCC('V', 'P', '8', 'L')
#define GIMG_WEBP_ALPH GIMG_WEBP_FOURCC('A', 'L', 'P', 'H')
#define GIMG_WEBP_ANIM GIMG_WEBP_FOURCC('A', 'N', 'I', 'M')
#define GIMG_WEBP_ANMF GIMG_WEBP_FOURCC('A', 'N', 'M', 'F')
#define GIMG_WEBP_ICCP GIMG_WEBP_FOURCC('I', 'C', 'C', 'P')
#define GIMG_WEBP_EXIF GIMG_WEBP_FOURCC('E', 'X', 'I', 'F')
#define GIMG_WEBP_XMP GIMG_WEBP_FOURCC('X', 'M', 'P', ' ')

/** VP8X feature flags (byte 0 of the 10-byte payload). */
#define GIMG_WEBP_VP8X_ANIMATION (1u << 1)
#define GIMG_WEBP_VP8X_XMP (1u << 2)
#define GIMG_WEBP_VP8X_EXIF (1u << 3)
#define GIMG_WEBP_VP8X_ALPHA (1u << 4)
#define GIMG_WEBP_VP8X_ICCP (1u << 5)

/** ANMF header is 16 bytes before the nested bitstream. */
#define GIMG_WEBP_ANMF_HEADER_SIZE 16u

/** Hard cap on ANMF frames when limits->max_frame_count is unset. */
#define GIMG_WEBP_DEFAULT_MAX_FRAMES 1024u

/**
 * @brief One ANMF frame: geometry, timing, dispose/blend and bitstream spans.
 *
 * Offsets are canvas pixels (the on-disk 24-bit fields store half-pixels;
 * readers multiply by two, matching libwebp). Bitstream offsets are absolute
 * file offsets of the nested chunk *payload* (past the eight-byte header),
 * or zero when that chunk is absent. Decode composites onto the VP8X canvas;
 * the item itself carries no offset.
 */
typedef struct {
  uint32_t x;
  uint32_t y;
  uint32_t width;
  uint32_t height;
  uint32_t duration_ms;
  uint8_t dispose_background; ///< 1 = dispose to transparent background.
  uint8_t blend_source;       ///< 1 = replace (NO_BLEND); 0 = alpha OVER.
  size_t alph_payload_off;
  uint32_t alph_payload_size;
  size_t vp8_payload_off;
  uint32_t vp8_payload_size;
  size_t vp8l_payload_off;
  uint32_t vp8l_payload_size;
} gimg_webp_frame_t;

extern const unsigned char gimg_webp_signature[GIMG_WEBP_SIGNATURE_LEN];

int gimg_webp_probe(
    GIMG_Stream * stream, const unsigned char * peek, size_t peeked);

/**
 * @brief One chunk as webpinfo lists it: fourcc, chunk-header offset and
 * webpinfo length (header + payload).
 *
 * Nested VP8/VP8L/ALPH inside an ANMF are recorded as their own entries at
 * absolute file offsets, matching `webpinfo`. `offset` is the start of the
 * eight-byte chunk header. `size` is what webpinfo prints as length: eight
 * plus the payload byte count (not including the pad byte).
 */
typedef struct {
  uint32_t fourcc;
  uint32_t size;   ///< webpinfo length = 8 + payload + pad.
  size_t offset;   ///< Absolute file offset of the chunk fourcc.
  uint32_t payload_size; ///< On-disk payload alone (for readers of the bytes).
} gimg_webp_chunk_t;

/**
 * @brief Per-document state after a successful load.
 *
 * Picture payloads are retained. Decode covers simple VP8L, VP8X+VP8L,
 * VP8 keyframes, VP8+ALPH, and ANIM/ANMF frame compositing.
 */
typedef struct {
  const GIMG_Allocator * allocator;
  unsigned char * file_bytes;
  size_t file_size;
  gimg_webp_chunk_t * chunks;
  size_t chunk_count;
  gimg_webp_frame_t * frames;
  size_t frame_count;
  uint32_t canvas_width;
  uint32_t canvas_height;
  uint8_t vp8x_flags; ///< 0 when no VP8X chunk.
  int has_vp8x;
  int has_alpha;      ///< From VP8X, VP8L alpha bit, or an ALPH chunk.
  int is_animation;   ///< From VP8X or an ANIM/ANMF chunk.
  int is_lossy;       ///< At least one VP8  bitstream present.
  int is_lossless;    ///< At least one VP8L bitstream present.
  int has_anim_chunk;
  uint8_t bgcolor_rgba[4]; ///< From ANIM; meaningful when has_anim_chunk.
  uint16_t loop_count;     ///< From ANIM; 0 = forever.
  const unsigned char * iccp;
  size_t iccp_size;
  const unsigned char * exif;
  size_t exif_size;
  const unsigned char * xmp;
  size_t xmp_size;
} gimg_webp_doc_state_t;

GIMG_Result gimg_webp_load(GIMG_Codec * codec, GIMG_Stream * stream,
    const GIMG_Load_Options * options, GIMG_Diagnostics * diagnostics,
    GIMG_Doc ** out_doc);

GIMG_Result gimg_webp_decode(GIMG_Codec * codec, const GIMG_Item * item,
    const GIMG_Decode_Options * options, GIMG_Raster ** out_raster);

GIMG_Result gimg_webp_save(GIMG_Codec * codec, const GIMG_Doc * doc,
    GIMG_Stream * stream, const char * format_name,
    const GIMG_Save_Options * options, GIMG_Save_Report * report);

void gimg_webp_free_doc_state(GIMG_Codec * codec, void * codec_private);

/**
 * @brief Peek canvas dimensions from a VP8 or VP8L bitstream header.
 * @return 1 on success, 0 if the payload is too short or not a key frame.
 */
int gimg_webp_peek_vp8_dims(const unsigned char * data, size_t size,
    uint32_t * out_w, uint32_t * out_h);
int gimg_webp_peek_vp8l_dims(const unsigned char * data, size_t size,
    uint32_t * out_w, uint32_t * out_h, int * out_alpha);

/**
 * @brief Decode a VP8L bitstream payload to an owned RGBA8 raster.
 *
 * @param data  VP8L chunk payload (starts with magic 0x2f).
 * @param size  Payload byte count.
 */
GIMG_Result gimg_webp_vp8l_decode(const unsigned char * data, size_t size,
    const GIMG_Allocator * alloc, GIMG_Raster ** out_raster);

/**
 * @brief Encode an RGBA8 buffer to a VP8L bitstream payload (starts with 0x2f).
 *
 * @param exact  Non-zero preserves RGB under fully transparent pixels.
 * @param effort 0–9; >=1 applies the subtract-green transform.
 */
GIMG_Result gimg_webp_vp8l_encode(const uint8_t * rgba, uint32_t width,
    uint32_t height, size_t stride, int has_alpha_hint, int exact, int effort,
    const GIMG_Allocator * alloc, unsigned char ** out_bytes,
    size_t * out_size);

/**
 * @brief Encode an opaque RGBA8 buffer to a VP8 keyframe bitstream payload.
 *
 * Stub Phase G encoder: Intra16 DC, single quantizer, Y2/UV DC residuals
 * only. No alpha. @a effort 0–9 maps to a coarse Q ladder.
 */
GIMG_Result gimg_webp_vp8_encode(const uint8_t * rgba, uint32_t width,
    uint32_t height, size_t stride, int effort, const GIMG_Allocator * alloc,
    unsigned char ** out_bytes, size_t * out_size);

/**
 * @brief Decode headerless VP8L image data used inside an ALPH chunk.
 *
 * Unlike @ref gimg_webp_vp8l_decode, there is no 0x2f frame header: the
 * bitstream starts at the transform/Huffman stream for a canvas of the given
 * size. On success, @a out_alpha receives the green channel of each pixel
 * (libwebp's alpha-in-green convention), @a width * @a height bytes. The
 * caller owns the buffer. ALPH spatial filtering is applied by
 * @ref gimg_webp_alpha_decode, not here.
 */
GIMG_Result gimg_webp_vp8l_decode_alpha(const unsigned char * data, size_t size,
    uint32_t width, uint32_t height, const GIMG_Allocator * alloc,
    uint8_t ** out_alpha);

/**
 * @brief Decode an ALPH chunk payload to an owned alpha plane.
 *
 * @param data   ALPH payload (header byte + compressed or raw samples).
 * @param size   Payload byte count.
 * @param width  Canvas / picture width.
 * @param height Canvas / picture height.
 * @param out_alpha Receives a @a width * @a height buffer of A samples.
 */
GIMG_Result gimg_webp_alpha_decode(const unsigned char * data, size_t size,
    uint32_t width, uint32_t height, const GIMG_Allocator * alloc,
    uint8_t ** out_alpha);

/**
 * @brief Decode a VP8 keyframe bitstream payload to an owned RGBA8 raster.
 *
 * Keyframes are decoded for every intra mode in RFC 6386 sections 11
 * and 12, including the coefficient partitions of section 9.5. Segment
 * adjustments (sections 9.3 and 10) select the quantizer and the
 * loop-filter level, and section 15 then filters the frame. An
 * interframe needs the reference pictures from an earlier keyframe in
 * the same @c gimg_vp8_seq; alone, it returns @c GIMG_ERR_CORRUPT.
 * Reconstruction follows that RFC. Lossy files outside the subset are
 * still checked with `dwebp` in the pinned oracle image.
 *
 * @param data  VP8 chunk payload (frame tag at byte 0).
 * @param size  Payload byte count.
 */
GIMG_Result gimg_webp_vp8_decode(const unsigned char * data, size_t size,
    const GIMG_Allocator * alloc, GIMG_Raster ** out_raster);

/** Reference pictures and probabilities carried across one VP8 sequence. */
typedef struct gimg_vp8_seq gimg_vp8_seq;

GIMG_Result gimg_vp8_seq_create(const GIMG_Allocator * alloc,
    gimg_vp8_seq ** out);
void gimg_vp8_seq_destroy(const GIMG_Allocator * alloc, gimg_vp8_seq * seq);
/** Drop reference pictures. The next frame has to be a keyframe. */
void gimg_vp8_seq_reset(const GIMG_Allocator * alloc, gimg_vp8_seq * seq);

/**
 * @brief Decode one VP8 frame, updating @a seq.
 *
 * A keyframe replaces the sequence dimensions and the three reference
 * pictures. An interframe reads them. @a seq is not freed.
 */
GIMG_Result gimg_webp_vp8_decode_frame(const unsigned char * data, size_t size,
    const GIMG_Allocator * alloc, gimg_vp8_seq * seq, GIMG_Raster ** out_raster);

/**
 * @brief Decode one still picture (VP8 and/or VP8L, optional ALPH) to RGBA8.
 *
 * Used by both the still-image path and per-ANMF frame decode. Exactly one of
 * @a vp8 / @a vp8l must be non-NULL.
 */
GIMG_Result gimg_webp_decode_picture(const unsigned char * vp8, size_t vp8_size,
    const unsigned char * vp8l, size_t vp8l_size, const unsigned char * alph,
    size_t alph_size, const GIMG_Allocator * alloc, gimg_vp8_seq * seq,
    GIMG_Raster ** out_raster);

/**
 * @brief Parse the 16-byte ANMF header into @a frame geometry and flags.
 * @return GIMG_OK, or GIMG_ERR_CORRUPT when the rectangle is empty.
 */
GIMG_Result gimg_webp_parse_anmf_header(const unsigned char * header16,
    gimg_webp_frame_t * frame);

/**
 * @brief Composite frames [0, index] onto the canvas and return that raster.
 *
 * Matches libwebp `anim_dump`: dispose-to-background clears the previous
 * frame rectangle to transparent; blend OVER is non-premultiplied alpha.
 */
GIMG_Result gimg_webp_decode_animation_frame(const gimg_webp_doc_state_t * st,
    size_t index, GIMG_Raster ** out_raster);

/** VP8L transform types (bitstream order). */
enum {
  GIMG_WEBP_VP8L_PREDICTOR = 0,
  GIMG_WEBP_VP8L_CROSS_COLOR = 1,
  GIMG_WEBP_VP8L_SUBTRACT_GREEN = 2,
  GIMG_WEBP_VP8L_COLOR_INDEXING = 3
};

/**
 * @brief One inverse-transform descriptor produced while reading the stream.
 *
 * @c data owns palette / predictor / colour-code pixels for types that need
 * them. @c xsize_/@c ysize_ are the dimensions the transform applies to (the
 * full canvas width for colour indexing, even when the coded width is packed).
 */
typedef struct {
  int type;
  int bits;
  int xsize;
  int ysize;
  uint32_t * data;
} gimg_webp_vp8l_xform_t;

/**
 * @brief Apply one inverse transform to rows [@a row_start, @a row_end).
 *
 * @a in and @a out may alias only when the transform allows it (libwebp
 * colour-indexing packed unpack does; predictor and cross-colour expect
 * distinct output storage for a full-image pass).
 */
void gimg_webp_vp8l_inverse_xform(const gimg_webp_vp8l_xform_t * xform,
    int row_start, int row_end, const uint32_t * in, uint32_t * out);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GIMG_SRC_CODEC_WEBP_WEBP_INTERNAL_H */

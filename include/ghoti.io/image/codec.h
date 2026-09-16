/**
 * @file
 *
 * Codec registry, probing, load/save API (spec §7).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GIMG_CODEC_H
#define GHOTI_IO_GIMG_CODEC_H

#include <ghoti.io/image/allocator.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opaque codec descriptor (name, probe, capabilities). */
typedef struct GIMG_Codec GIMG_Codec;

/**
 * @brief Codec capability bits (bitmask for codec->capabilities).
 */
#define GIMG_CAP_READ (1u << 0)
#define GIMG_CAP_WRITE (1u << 1)
#define GIMG_CAP_ANIMATION (1u << 2)
#define GIMG_CAP_PALETTE (1u << 3)
#define GIMG_CAP_ICC (1u << 4)
#define GIMG_CAP_16BPC (1u << 5)
#define GIMG_CAP_CMYK (1u << 6)

/**
 * @brief Probe result: likely format name and confidence.
 *
 * Lifetime: format_name is valid only until the next call that mutates the
 * codec registry (e.g. gimg_codec_register()). Do not store the pointer
 * long-term; copy the string if you need to keep it.
 */
typedef struct {
  const char * format_name; ///< e.g. "png", "jpeg"; NULL if no match.
  unsigned int confidence;  ///< 0–100; 0 = no match.
  uint8_t _reserved[4];
} GIMG_Probe_Result;

/**
 * @brief Create a stub codec for registration (uses default allocator).
 */
GIMG_API GIMG_Result gimg_codec_create_stub(const char * name,
    const void * magic_bytes, size_t magic_len, GIMG_Codec ** out_codec);

/**
 * @brief Create a stub codec with a specific allocator.
 * @param allocator Allocator for codec and its name/magic copies (NULL =
 * default).
 */
GIMG_API GIMG_Result gimg_codec_create_stub_with_allocator(
    const GIMG_Allocator * allocator, const char * name,
    const void * magic_bytes, size_t magic_len, GIMG_Codec ** out_codec);

/**
 * @brief Register a codec (by name, probe, capabilities).
 * @param codec Codec to register (library takes ownership of pointer).
 * @return GIMG_OK or GIMG_ERR_OOM / duplicate name.
 */
GIMG_API GIMG_Result gimg_codec_register(GIMG_Codec * codec);

/**
 * @brief Get number of registered codecs.
 */
GIMG_API size_t gimg_codec_count(void);

/**
 * @brief Get codec by index (0 .. count-1).
 */
GIMG_API GIMG_Codec * gimg_codec_by_index(size_t index);

/**
 * @brief Get codec by name (NULL if not found).
 */
GIMG_API GIMG_Codec * gimg_codec_by_name(const char * name);

/**
 * @brief Get codec display name.
 */
GIMG_API const char * gimg_codec_name(const GIMG_Codec * codec);

/**
 * @brief Get codec capability bitmask (GIMG_CAP_*).
 */
GIMG_API unsigned int gimg_codec_capabilities(const GIMG_Codec * codec);

/**
 * @brief Probe stream to identify format (peek where possible).
 * @param stream Stream to probe.
 * @param result Filled with format name and confidence.
 * @return GIMG_OK; result->format_name NULL if no codec matched.
 */
GIMG_API GIMG_Result gimg_probe(
    GIMG_Stream * stream, GIMG_Probe_Result * result);

/**
 * @brief Load options (limits, strictness, etc.).
 * @see api_options
 */
typedef struct {
  const GIMG_Limits * limits; ///< NULL = use defaults.
  GIMG_Strictness strictness;
  uint8_t _reserved[8];
} GIMG_Load_Options;

/**
 * @brief Load document from stream (stub: returns UNSUPPORTED or minimal doc).
 */
GIMG_API GIMG_Result gimg_doc_load(GIMG_Stream * stream,
    const GIMG_Load_Options * options, GIMG_Diagnostics * diagnostics,
    GIMG_Doc ** out_doc);

/** @brief EXIF IFD1 thumbnail format: 0 = default (6), 1 = uncompressed,
 * 6 = JPEG, 7 = TIFF TechNote 2 JPEG. Used when saving with a second doc item.
 */
#define GIMG_EXIF_THUMB_FORMAT_DEFAULT 0
#define GIMG_EXIF_THUMB_FORMAT_UNCOMPRESSED 1
#define GIMG_EXIF_THUMB_FORMAT_JPEG 6
#define GIMG_EXIF_THUMB_FORMAT_TIFF_JPEG 7

/** @brief JPEG chroma subsampling: 0 = 4:2:0 (default), 1 = 4:2:2, 2 = 4:4:4. */
#define GIMG_JPEG_CHROMA_420 0
#define GIMG_JPEG_CHROMA_422 1
#define GIMG_JPEG_CHROMA_444 2
/** FDCT method: Loeffler (libjpeg-compatible, default) or reference. */
#define GIMG_JPEG_FDCT_LOEFFLER 0
#define GIMG_JPEG_FDCT_REF     1
/** Quantization method: reciprocal-based (libjpeg-compatible, default) or
 * integer division (for speed/quality comparison). */
#define GIMG_JPEG_QUANT_RECIP  0
#define GIMG_JPEG_QUANT_DIV    1

/**
 * @brief One scan in a progressive JPEG scan script (ISO/IEC 10918-1 Annex B).
 * Ss, Se = spectral selection (coefficient indices 0–63, zigzag order).
 * Ah, Al = successive approximation (Ah=0 for initial pass; refinement when Ah>0).
 */
typedef struct {
  uint8_t Ss; ///< First coefficient index in this scan (0–63).
  uint8_t Se; ///< Last coefficient index in this scan (0–63; must be >= Ss).
  uint8_t Ah; ///< Successive approximation high (0 = initial encoding).
  uint8_t Al; ///< Successive approximation low / bit position.
} GIMG_JPEG_Progressive_Scan;

/**
 * @brief Progressive JPEG scan script. When saving with jpeg_progressive=1:
 * - If NULL or scan_count==0: encoder uses default progression (e.g. DC + AC bands).
 * - If non-NULL and scan_count>0: encoder uses this sequence of scans.
 * Caller keeps the array valid for the duration of gimg_doc_save().
 */
typedef struct {
  unsigned int scan_count;
  const GIMG_JPEG_Progressive_Scan * scans;
} GIMG_JPEG_Progressive_Config;

/**
 * @brief Save options (metadata policy, interlace, quality, etc.).
 * @see api_options
 */
typedef struct {
  GIMG_Meta_Policy metadata_policy;
  unsigned int interlaced; ///< 0 = non-interlaced (default), 1 = Adam7 (PNG).
  unsigned int quality; ///< JPEG quality 1–100 (100 = finest). 0 = unspecified,
                        ///< codec default (e.g. 85).
  uint8_t exif_thumbnail_format;  ///< IFD1 thumbnail: 0 = default (6), 1, 6, 7.
  uint8_t exif_thumbnail_quality; ///< Thumbnail JPEG quality 1–100 when
                                  ///< format 6 or 7; 0 = default (85).
  uint8_t jpeg_chroma_subsampling; ///< GIMG_JPEG_CHROMA_420 (default), 422, 444.
  /** FDCT method: GIMG_JPEG_FDCT_LOEFFLER (0, default) = libjpeg-compatible
   * Loeffler integer DCT for exact match; GIMG_JPEG_FDCT_REF (1) = reference
   * implementation (float-based) for speed/quality comparison. */
  uint8_t jpeg_fdct_method;
  /** Quantization: GIMG_JPEG_QUANT_RECIP (0, default) = reciprocal-based
   * (libjpeg match); GIMG_JPEG_QUANT_DIV (1) = integer division. */
  uint8_t jpeg_quant_method;
  uint8_t jpeg_progressive;        ///< 0 = baseline (default), 1 = progressive.
  /** When jpeg_progressive==1: NULL or scan_count 0 = default progression;
   * otherwise use this scan script. Ignored for non-JPEG or baseline. */
  const GIMG_JPEG_Progressive_Config * jpeg_progressive_config;
  /** Restart interval in MCUs (0 = none). When non-zero, DRI segment is
   * written and RST markers (0xFF 0xD0..0xD7) are injected every N MCUs. */
  uint16_t jpeg_restart_interval;
  /** JPEG output precision (save): 0 = derive from the raster, 8 or 12 = write
   * at that precision.  T.81 Table B.2 allows only 8 and 12 in a DCT-based
   * frame, so 16 returns GIMG_ERR_UNSUPPORTED and a 16-bit raster is written
   * at 12 when this is 0.  When raster depth differs from the chosen
   * precision, the encoder uses library bit-depth conversion
   * (gimg_ops_convert_bit_depth or gimg_bitdepth_*). Ignored for non-JPEG. */
  uint8_t jpeg_precision;
  uint8_t _jpeg_pad[1];
} GIMG_Save_Options;

/**
 * @brief Save report (warnings, bytes written, etc.).
 */
typedef struct {
  size_t bytes_written;
  GIMG_Diagnostics * diagnostics;
  uint8_t _reserved[8];
} GIMG_Save_Report;

/**
 * @brief Save document to stream (stub: fails until codecs exist).
 */
GIMG_API GIMG_Result gimg_doc_save(const GIMG_Doc * doc, GIMG_Stream * stream,
    const char * format_name, const GIMG_Save_Options * options,
    GIMG_Save_Report * report);

/**
 * @brief JPEG chroma upsampling method (decode only).
 * @see api_options
 */
#define GIMG_JPEG_CHROMA_UPSAMPLE_SIMPLE  0  /**< Box filter (replicate). */
#define GIMG_JPEG_CHROMA_UPSAMPLE_FANCY  1  /**< Triangle filter (smooth). */

/**
 * @brief Decode options.
 * @see api_options
 */
typedef struct {
  const GIMG_Limits * limits;
  /** JPEG: chroma upsampling when decoding 4:2:0/4:2:2. GIMG_JPEG_CHROMA_UPSAMPLE_SIMPLE (0) or FANCY (1). When options is NULL, FANCY is used (default). */
  uint8_t jpeg_chroma_upsampling;
  /** JPEG decode-to precision: 0 = use file precision (8→GRAY8/RGBA8;
   * 12/16→GRAY16/RGB16 with 12-bit left-justified); 8, 12, or 16 = decode to
   * that bit depth (conversion via library when different from file). */
  uint8_t jpeg_precision;
  uint8_t _reserved[6];
} GIMG_Decode_Options;

/**
 * @brief Decode item to raster (stub: returns UNSUPPORTED or minimal raster).
 */
GIMG_API GIMG_Result gimg_item_decode(const GIMG_Item * item,
    const GIMG_Decode_Options * options, GIMG_Raster ** out_raster);

/**
 * @brief Ensure the item has an attached raster: if already present, no-op;
 * otherwise decode via the document's codec and attach the raster to the item.
 * The document owns the attached raster. Use for load -> modify -> save flows.
 * @param item Item (must belong to a document that was loaded with a codec).
 * @param options Decode options (limits, etc.); NULL for defaults.
 * @return GIMG_OK, GIMG_ERR_UNSUPPORTED (no codec / not loaded), or decode
 * error.
 */
GIMG_API GIMG_Result gimg_item_ensure_decoded(
    GIMG_Item * item, const GIMG_Decode_Options * options);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GIMG_CODEC_H

/**
 * @file
 *
 * Codec registry, probing, load/save API (spec §7).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_IMAGE_CODEC_H
#define GHOTI_IO_IMAGE_CODEC_H

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

/**
 * @brief Save options (metadata policy, interlace, etc.).
 * @see api_options
 */
typedef struct {
  GIMG_Meta_Policy metadata_policy;
  unsigned int interlaced; ///< 0 = non-interlaced (default), 1 = Adam7 (PNG).
  uint8_t _reserved[4];
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
 * @brief Decode options.
 * @see api_options
 */
typedef struct {
  const GIMG_Limits * limits;
  uint8_t _reserved[8];
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
 * @return GIMG_OK, GIMG_ERR_UNSUPPORTED (no codec / not loaded), or decode error.
 */
GIMG_API GIMG_Result gimg_item_ensure_decoded(GIMG_Item * item,
    const GIMG_Decode_Options * options);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_IMAGE_CODEC_H

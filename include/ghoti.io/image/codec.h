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
typedef struct GIMG_CODEC GIMG_CODEC;

/**
 * @brief Probe result: likely format name and confidence.
 */
typedef struct {
  const char * format_name; ///< e.g. "png", "jpeg"; NULL if no match.
  unsigned int confidence;  ///< 0–100; 0 = no match.
  uint8_t _reserved[4];
} GIMG_PROBE_RESULT;

/**
 * @brief Create a stub codec for registration (uses default allocator).
 */
GIMG_API GIMG_RESULT gimg_codec_create_stub(const char * name,
    const void * magic_bytes, size_t magic_len, GIMG_CODEC ** out_codec);

/**
 * @brief Create a stub codec with a specific allocator.
 * @param allocator Allocator for codec and its name/magic copies (NULL =
 * default).
 */
GIMG_API GIMG_RESULT gimg_codec_create_stub_with_allocator(
    const GIMG_ALLOCATOR * allocator, const char * name,
    const void * magic_bytes, size_t magic_len, GIMG_CODEC ** out_codec);

/**
 * @brief Register a codec (by name, probe, capabilities).
 * @param codec Codec to register (library takes ownership of pointer).
 * @return GIMG_OK or GIMG_ERR_OOM / duplicate name.
 */
GIMG_API GIMG_RESULT gimg_codec_register(GIMG_CODEC * codec);

/**
 * @brief Get number of registered codecs.
 */
GIMG_API size_t gimg_codec_count(void);

/**
 * @brief Get codec by index (0 .. count-1).
 */
GIMG_API GIMG_CODEC * gimg_codec_by_index(size_t index);

/**
 * @brief Get codec by name (NULL if not found).
 */
GIMG_API GIMG_CODEC * gimg_codec_by_name(const char * name);

/**
 * @brief Get codec display name.
 */
GIMG_API const char * gimg_codec_name(const GIMG_CODEC * codec);

/**
 * @brief Probe stream to identify format (peek where possible).
 * @param stream Stream to probe.
 * @param result Filled with format name and confidence.
 * @return GIMG_OK; result->format_name NULL if no codec matched.
 */
GIMG_API GIMG_RESULT gimg_probe(
    GIMG_STREAM * stream, GIMG_PROBE_RESULT * result);

/**
 * @brief Load options (limits, strictness, etc.).
 */
typedef struct {
  const GIMG_LIMITS * limits; ///< NULL = use defaults.
  GIMG_STRICTNESS strictness;
  uint8_t _reserved[8];
} GIMG_LOAD_OPTIONS;

/**
 * @brief Load document from stream (stub: returns UNSUPPORTED or minimal doc).
 */
GIMG_API GIMG_RESULT gimg_doc_load(GIMG_STREAM * stream,
    const GIMG_LOAD_OPTIONS * options, GIMG_DIAGNOSTICS * diagnostics,
    GIMG_DOC ** out_doc);

/**
 * @brief Save options (metadata policy, etc.).
 */
typedef struct {
  GIMG_META_POLICY metadata_policy;
  uint8_t _reserved[8];
} GIMG_SAVE_OPTIONS;

/**
 * @brief Save report (warnings, bytes written, etc.).
 */
typedef struct {
  size_t bytes_written;
  GIMG_DIAGNOSTICS * diagnostics;
  uint8_t _reserved[8];
} GIMG_SAVE_REPORT;

/**
 * @brief Save document to stream (stub: fails until codecs exist).
 */
GIMG_API GIMG_RESULT gimg_doc_save(const GIMG_DOC * doc, GIMG_STREAM * stream,
    const char * format_name, const GIMG_SAVE_OPTIONS * options,
    GIMG_SAVE_REPORT * report);

/**
 * @brief Decode options.
 */
typedef struct {
  const GIMG_LIMITS * limits;
  uint8_t _reserved[8];
} GIMG_DECODE_OPTIONS;

/**
 * @brief Decode item to raster (stub: returns UNSUPPORTED or minimal raster).
 */
GIMG_API GIMG_RESULT gimg_item_decode(const GIMG_ITEM * item,
    const GIMG_DECODE_OPTIONS * options, GIMG_RASTER ** out_raster);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_IMAGE_CODEC_H

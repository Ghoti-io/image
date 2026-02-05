/**
 * @file
 *
 * Core types, result codes, and error model for the Ghoti.io Image library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_IMAGE_CORE_H
#define GHOTI_IO_IMAGE_CORE_H

#include <ghoti.io/image/macros.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Result code for image library operations.
 */
typedef enum {
  GIMG_OK = 0,          ///< Operation succeeded.
  GIMG_ERR_IO,          ///< I/O error (read/write/seek failed).
  GIMG_ERR_FORMAT,      ///< Unrecognized or invalid format.
  GIMG_ERR_UNSUPPORTED, ///< Feature or format not supported.
  GIMG_ERR_LIMIT,       ///< Resource or size limit exceeded.
  GIMG_ERR_CORRUPT,     ///< Corrupt or invalid data.
  GIMG_ERR_OOM,         ///< Out of memory.
  GIMG_ERR_INTERNAL,    ///< Internal library error.
  GIMG_RESULT_COUNT
} GIMG_Result;

/**
 * @brief Severity of a diagnostic (spec §13.2).
 */
typedef enum {
  GIMG_DIAG_WARNING = 0,
  GIMG_DIAG_ERROR,
  GIMG_DIAG_SEVERITY_COUNT
} GIMG_Diag_Severity;

/**
 * @brief Single diagnostic: codec, offset, chunk/tag, severity, action.
 */
typedef struct {
  const char * codec_name;
  size_t offset;
  uint32_t chunk_or_tag_id;
  GIMG_Diag_Severity severity;
  const char *
      recommended_action; ///< Optional; e.g. "increase max_chunk_size".
  uint8_t _reserved[8];
} GIMG_Diagnostic;

/**
 * @brief Diagnostics payload (list of diagnostics; no silent truncation).
 */
typedef struct {
  GIMG_Diagnostic * items;
  size_t count;
  size_t capacity;
} GIMG_Diagnostics;

/**
 * @brief Strictness level (spec §7.4).
 */
typedef enum {
  GIMG_STRICT = 0, ///< Warnings as errors.
  GIMG_NORMAL,     ///< Safe recoveries + warnings.
  GIMG_PERMISSIVE, ///< More heuristics + warnings.
  GIMG_STRICTNESS_COUNT
} GIMG_Strictness;

/**
 * @brief Get a human-readable string for a result code.
 * @param result The result code.
 * @return Static string describing the result, or "unknown" for invalid values.
 */
GIMG_API const char * gimg_result_string(GIMG_Result result);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_IMAGE_CORE_H

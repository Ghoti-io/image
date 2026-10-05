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
 * Core types, result codes, and error model for the Ghoti.io Image library.
 */

#ifndef GHOTI_IO_GIMG_CORE_H
#define GHOTI_IO_GIMG_CORE_H

#include <ghoti.io/image/allocator.h>
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
  const char * codec_name;        ///< Name of the codec that raised the diagnostic.
  size_t offset;                  ///< Byte offset in the input where the problem was found.
  uint32_t chunk_or_tag_id;       ///< Identifier of the chunk or tag concerned.
  GIMG_Diag_Severity severity;    ///< Warning or error.
  const char *
      recommended_action; ///< Optional; e.g. "increase max_chunk_size".
  uint8_t _reserved[8]; ///< Zero; room to grow.
} GIMG_Diagnostic;

/**
 * @brief Diagnostics payload (list of diagnostics; no silent truncation).
 *
 * Optional allocator: if non-NULL, used for growing the list and for clear/destroy.
 * If NULL (e.g. zero-initialized or after destroy), the default allocator is used.
 * Call gimg_diagnostics_init() to set an allocator; call gimg_diagnostics_clear()
 * or gimg_diagnostics_destroy() when done to avoid leaks.
 */
typedef struct {
  GIMG_Diagnostic * items;        ///< The diagnostics, in the order they were appended.
  size_t count;                   ///< Number of diagnostics in use.
  size_t capacity;                ///< Number of diagnostics @a items has room for.
  const GIMG_Allocator * allocator; ///< Allocator for the list, or NULL for the default.
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
 * @brief Initialize diagnostics with an optional allocator.
 * @param d Diagnostics to initialize (may be zero-initialized).
 * @param allocator Allocator for list growth and for clear/destroy; NULL = default.
 * Append and clear/destroy use this allocator. Safe to call on already-initialized
 * diagnostics (overwrites allocator only; does not clear existing items).
 */
GIMG_API void gimg_diagnostics_init(GIMG_Diagnostics * d,
    const GIMG_Allocator * allocator);

/**
 * @brief Append one diagnostic (grows the list using the diagnostics' allocator).
 * @param diagnostics Diagnostics to append to (may be zero-initialized; uses default allocator if init was not called).
 * @param codec_name Codec name (e.g. "png"); stored by reference.
 * @param offset Stream offset when relevant (e.g. chunk start).
 * @param chunk_or_tag_id Chunk type or tag (e.g. PNG 4-byte type as uint32_t).
 * @param severity Warning or error.
 * @param recommended_action Optional message (e.g. "increase max_chunk_size");
 *   stored by reference, may be NULL.
 * @return GIMG_OK or GIMG_ERR_OOM if realloc failed.
 */
GIMG_API GIMG_Result gimg_diagnostics_append(GIMG_Diagnostics * diagnostics,
    const char * codec_name, size_t offset, uint32_t chunk_or_tag_id,
    GIMG_Diag_Severity severity, const char * recommended_action);

/**
 * @brief Free the diagnostics list and set count/capacity to zero. Idempotent if already empty.
 * Uses the allocator stored in d (or default if never initialized). After clear, append may be used again.
 */
GIMG_API void gimg_diagnostics_clear(GIMG_Diagnostics * d);

/**
 * @brief Free the diagnostics list and zero the whole struct. Same as clear plus zeroing allocator.
 * After destroy, the struct may be discarded or reused (e.g. with gimg_diagnostics_init again).
 */
GIMG_API void gimg_diagnostics_destroy(GIMG_Diagnostics * d);

/**
 * @brief Get a human-readable string for a result code.
 * @param result The result code.
 * @return Static string describing the result, or "unknown" for invalid values.
 */
GIMG_API const char * gimg_result_string(GIMG_Result result);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GIMG_CORE_H

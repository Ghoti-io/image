/**
 * @file
 *
 * Stream I/O abstraction for image library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_IMAGE_STREAM_H
#define GHOTI_IO_IMAGE_STREAM_H

#include <ghoti.io/image/allocator.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/macros.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opaque stream. */
typedef struct GIMG_Stream GIMG_Stream;

/**
 * @brief Read up to size bytes into buffer.
 * @param stream Stream.
 * @param buffer Output buffer.
 * @param size Maximum bytes to read.
 * @param out_bytes_read On success, bytes actually read (may be less than
 * size).
 * @return GIMG_OK, GIMG_ERR_IO, or stream's stored error.
 */
GIMG_API GIMG_Result gimg_stream_read(
    GIMG_Stream * stream, void * buffer, size_t size, size_t * out_bytes_read);

/**
 * @brief Peek at up to size bytes without consuming.
 * @param stream Stream.
 * @param buffer Output buffer.
 * @param size Maximum bytes to peek.
 * @param out_bytes_available Bytes actually available (may be less than size).
 * @return GIMG_OK or error.
 */
GIMG_API GIMG_Result gimg_stream_peek(GIMG_Stream * stream, void * buffer,
    size_t size, size_t * out_bytes_available);

/**
 * @brief Skip up to count bytes.
 * @param stream Stream.
 * @param count Bytes to skip.
 * @param out_bytes_skipped Bytes actually skipped.
 * @return GIMG_OK or error.
 */
GIMG_API GIMG_Result gimg_stream_skip(
    GIMG_Stream * stream, size_t count, size_t * out_bytes_skipped);

/**
 * @brief Seek to offset (if stream supports seeking).
 * @return GIMG_OK or GIMG_ERR_UNSUPPORTED / GIMG_ERR_IO.
 */
GIMG_API GIMG_Result gimg_stream_seek(GIMG_Stream * stream, size_t offset);

/**
 * @brief Current position (if stream supports tell).
 * @return Byte offset or (size_t)-1 if not supported.
 */
GIMG_API size_t gimg_stream_tell(const GIMG_Stream * stream);

/**
 * @brief Total size in bytes (if known). (size_t)-1 if unknown.
 */
GIMG_API size_t gimg_stream_size(const GIMG_Stream * stream);

/**
 * @brief Get last error stored on stream.
 */
GIMG_API GIMG_Result gimg_stream_error(const GIMG_Stream * stream);

/**
 * @brief Create a memory stream from a buffer (borrowed; not freed).
 * Uses default allocator for the stream object.
 */
GIMG_API GIMG_Result gimg_stream_create_memory(
    const void * data, size_t size, GIMG_Stream ** out_stream);

/**
 * @brief Create a memory stream with a specific allocator.
 * @param allocator Allocator for the stream object (NULL = default).
 */
GIMG_API GIMG_Result gimg_stream_create_memory_with_allocator(
    const GIMG_Allocator * allocator, const void * data, size_t size,
    GIMG_Stream ** out_stream);

/**
 * @brief Destroy stream and release resources.
 */
GIMG_API void gimg_stream_destroy(GIMG_Stream * stream);

//
// Limit options (spec §6.3). Used by load/decode paths.
//

typedef struct GIMG_Limits {
  size_t max_decoded_pixels;  ///< 0 = no limit.
  size_t max_memory;          ///< 0 = no limit.
  size_t max_metadata_size;   ///< 0 = no limit.
  size_t max_frame_count;     ///< 0 = no limit.
  size_t max_chunk_size;      ///< 0 = no limit (bomb protection).
  unsigned int max_recursion; ///< 0 = default (e.g. TIFF IFD depth).
  uint8_t _reserved[4];
} GIMG_Limits;

/**
 * @brief Initialize limits to defaults (no limits).
 */
GIMG_API void gimg_limits_default(GIMG_Limits * limits);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_IMAGE_STREAM_H

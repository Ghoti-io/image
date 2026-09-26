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
 * Stream I/O abstraction for image library.
 */

#ifndef GHOTI_IO_GIMG_STREAM_H
#define GHOTI_IO_GIMG_STREAM_H

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
 * @brief Read exactly size bytes, looping on short reads until all bytes are
 * read or an error/EOF occurs. Use with chunked or non-seekable streams.
 * @return GIMG_OK if size bytes read, GIMG_ERR_IO or GIMG_ERR_FORMAT on error/EOF.
 */
GIMG_API GIMG_Result gimg_stream_read_exact(
    GIMG_Stream * stream, void * buffer, size_t size);

/**
 * @brief Write size bytes from buffer to stream.
 * @param stream Stream (must support write, e.g. memory output stream).
 * @param buffer Input buffer.
 * @param size Bytes to write.
 * @param out_bytes_written On success, bytes actually written.
 * @return GIMG_OK, GIMG_ERR_IO, GIMG_ERR_UNSUPPORTED if stream is read-only.
 */
GIMG_API GIMG_Result gimg_stream_write(GIMG_Stream * stream,
    const void * buffer, size_t size, size_t * out_bytes_written);

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
 * @brief Create a read-only memory stream that does not support seek/tell.
 * Use for non-seekable sources (e.g. pipes); gimg_stream_tell returns
 * (size_t)-1 and gimg_stream_seek returns GIMG_ERR_UNSUPPORTED.
 */
GIMG_API GIMG_Result gimg_stream_create_memory_no_seek(
    const void * data, size_t size, GIMG_Stream ** out_stream);

/**
 * @brief Create a non-seekable memory stream with a specific allocator.
 */
GIMG_API GIMG_Result gimg_stream_create_memory_no_seek_with_allocator(
    const GIMG_Allocator * allocator, const void * data, size_t size,
    GIMG_Stream ** out_stream);

/**
 * @brief Create a read-only memory stream that limits each read to at most
 * max_bytes_per_read bytes (e.g. 1 for byte-at-a-time). Use to exercise
 * partial read handling; seek/tell supported.
 */
GIMG_API GIMG_Result gimg_stream_create_memory_chunked(
    const void * data, size_t size, size_t max_bytes_per_read,
    GIMG_Stream ** out_stream);

/**
 * @brief Create a chunked memory stream with a specific allocator.
 */
GIMG_API GIMG_Result gimg_stream_create_memory_chunked_with_allocator(
    const GIMG_Allocator * allocator, const void * data, size_t size,
    size_t max_bytes_per_read, GIMG_Stream ** out_stream);

/**
 * @brief Create a non-seekable, chunked memory stream (short reads, no seek).
 */
GIMG_API GIMG_Result gimg_stream_create_memory_no_seek_chunked(
    const void * data, size_t size, size_t max_bytes_per_read,
    GIMG_Stream ** out_stream);

/**
 * @brief Create a non-seekable chunked memory stream with a specific allocator.
 */
GIMG_API GIMG_Result gimg_stream_create_memory_no_seek_chunked_with_allocator(
    const GIMG_Allocator * allocator, const void * data, size_t size,
    size_t max_bytes_per_read, GIMG_Stream ** out_stream);

/**
 * @brief Create a writable memory stream (for save/output).
 * Data is appended on write; buffer grows as needed.
 * @param out_stream On success, a stream that supports gimg_stream_write.
 */
GIMG_API GIMG_Result gimg_stream_create_memory_output(
    GIMG_Stream ** out_stream);

/**
 * @brief Create a writable memory stream with a specific allocator.
 */
GIMG_API GIMG_Result gimg_stream_create_memory_output_with_allocator(
    const GIMG_Allocator * allocator, GIMG_Stream ** out_stream);

/**
 * @brief Get the buffer and size written to a memory output stream.
 * Only valid for streams created with gimg_stream_create_memory_output*.
 * The buffer is owned by the stream and invalid after gimg_stream_destroy.
 */
GIMG_API void gimg_stream_output_buffer(const GIMG_Stream * stream,
    const void ** out_data, size_t * out_size);

/**
 * @brief Destroy stream and release resources.
 */
GIMG_API void gimg_stream_destroy(GIMG_Stream * stream);

//
// Limit options (spec §6.3). Used by load/decode paths.
//

typedef struct GIMG_Limits {
  /**
   * 0 = no limit. Read by every codec, at whichever stage knows the size:
   * BMP and JPEG while parsing the frame header, PNG and GIF while decoding.
   * A cap equal to the image's own pixel count passes and one below it is
   * refused, so it bounds what it names rather than something near it.
   */
  size_t max_decoded_pixels;
  /**
   * 0 = no limit.
   * @warning **Read by BMP only.** No other codec consults it, so a caller
   * who sets this to bound a decode gets a bound on BMP and nothing on PNG,
   * JPEG or GIF. Use max_decoded_pixels, which every codec reads, to bound
   * the size of what comes back.
   */
  size_t max_memory;
  /**
   * 0 = no limit.
   * @warning **Not honoured.** No code reads this field; it is declared so
   * the shape of the struct is settled. Metadata is not capped by anything
   * today - an Exif blob, an ICC profile or a text chunk is taken at whatever
   * size the file gives, subject only to max_chunk_size where the format has
   * chunks.
   */
  size_t max_metadata_size;
  /**
   * 0 = no limit.
   * @warning **Read by PNG and GIF only.** Those are the two formats whose
   * multi-image form (APNG, and GIF's own frames) this was written for. A
   * BMP bitmap array also loads as several items and is not capped.
   */
  size_t max_frame_count;
  /**
   * 0 = no limit (bomb protection). Read by PNG, JPEG and GIF - the three
   * formats built out of length-prefixed segments - and by whichever codec a
   * BMP wrapping a PNG or a JPEG hands its payload to. A DIB has no chunk
   * structure, so a plain BMP is unaffected.
   */
  size_t max_chunk_size;
  /**
   * 0 = default.
   * @warning **Not honoured.** No code reads this field. It was declared for
   * a TIFF codec that does not exist; the one nesting this library does have,
   * a BMP that wraps a PNG or a JPEG, is one level deep by construction and
   * needs no depth counter.
   */
  unsigned int max_recursion;
  uint8_t _reserved[4];
} GIMG_Limits;

/**
 * @brief Initialize limits to defaults (no limits).
 */
GIMG_API void gimg_limits_default(GIMG_Limits * limits);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GIMG_STREAM_H

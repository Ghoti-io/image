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

/** @brief What gimg_stream_size() answers when the length is not known. */
#define GIMG_STREAM_SIZE_UNKNOWN ((size_t)-1)

/**
 * @brief Total size in bytes, or ::GIMG_STREAM_SIZE_UNKNOWN if not known.
 *
 * A stream that cannot seek cannot know where its end is, so the
 * non-seekable constructors answer ::GIMG_STREAM_SIZE_UNKNOWN here even
 * though a length was passed to them - a caller reading from a pipe has no
 * such length to pass, and a stream that answered one would let a decoder
 * take a shortcut that is not available on the source it stands for.
 *
 * Callers must test against ::GIMG_STREAM_SIZE_UNKNOWN. Zero is a length,
 * not an absence: it is what an empty stream is.
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
 * (size_t)-1, gimg_stream_seek returns GIMG_ERR_UNSUPPORTED, and
 * gimg_stream_size returns ::GIMG_STREAM_SIZE_UNKNOWN.
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

/**
 * @brief A limit field's value for "no limit".
 *
 * Zero does not mean this. Zero means the library's default for that field,
 * so that a caller who passes no limits, or a zeroed struct, is protected,
 * and a caller who wants none has to say so.
 */
#define GIMG_LIMIT_NONE ((size_t)-1)

/** @brief The default for GIMG_Limits.max_decoded_pixels: 2^28, 16384 squared. */
#define GIMG_DEFAULT_MAX_DECODED_PIXELS ((size_t)1 << 28)

/** @brief The default for GIMG_Limits.max_memory: one gibibyte. */
#define GIMG_DEFAULT_MAX_MEMORY ((size_t)1 << 30)

/**
 * @brief Resource limits applied while loading and decoding.
 *
 * A field of 0 means the library's default for that field, and passing no
 * limits at all means the default for every field. ::GIMG_LIMIT_NONE removes
 * a limit. The defaults are there because an image file's header can name a
 * picture of any size in a few bytes: without them a hundred-byte file can
 * ask for gigabytes.
 * @see api_options
 */
typedef struct GIMG_Limits {
  /**
   * 0 = ::GIMG_DEFAULT_MAX_DECODED_PIXELS. Read by every codec, at whichever stage knows the size:
   * BMP and JPEG while parsing the frame header, PNG, GIF and TIFF while
   * decoding, WebP from the canvas product at load. ICO reaches it through
   * whichever codec owns an entry's payload. A cap equal to the image's own
   * pixel count passes and one below it is refused, so it bounds what it
   * names rather than something near it.
   */
  size_t max_decoded_pixels;
  /**
   * 0 = ::GIMG_DEFAULT_MAX_MEMORY. The largest single allocation, in bytes,
   * that a load or a decode may make.
   *
   * Enforced beneath every codec, in the library's own allocation path, for
   * the whole of gimg_doc_load() and gimg_item_decode() on the calling
   * thread: the decoded raster, every working buffer, every copied block of
   * metadata. A request over it fails the call with GIMG_ERR_LIMIT. With the
   * default, a picture whose raster is over a gibibyte - 2^28 pixels at
   * sixteen bits a channel, for one - needs this raised as well as
   * max_decoded_pixels.
   *
   * It bounds each allocation, not their sum. A file that makes the library
   * hold many allocations at once, each within the cap, is bounded by
   * max_decoded_pixels, max_frame_count and max_metadata_size.
   */
  size_t max_memory;
  /**
   * 0 = a built-in four-mebibyte guard. Read by every codec, and the only cap
   * that bounds a metadata *total*: PNG's decompressed iCCP, JPEG's combined
   * COM segments, GIF's combined comments, BMP's embedded or resolved profile,
   * TIFF's ICC, XMP, JPEGTables and ImageDescription, and WebP's ICCP, EXIF
   * and XMP chunks. WebP joined that list on 2026-10-02: it was the sixth
   * codec and the only one that did not read this cap, so those three chunks
   * were bounded by the length of the file and nothing else.
   *
   * max_chunk_size bounds one segment as it sits in the file, which is a
   * different question - a file of many comment segments, each within that
   * cap, accumulated without bound before this was honoured, and a zlib bomb
   * in an iCCP chunk is small in the file and large after it.
   *
   * Two over-size cases, and they are not the same event. Past a cap **you**
   * set is GIMG_ERR_LIMIT, because you asked to be told. Past the built-in
   * guard, with no cap set, the object is dropped and the image still loads
   * untagged, because a profile larger than any real profile says the file is
   * describing something other than its own colour and the picture is not
   * wrong.
   */
  size_t max_metadata_size;
  /**
   * 0 = each codec's own built-in bound, where it has one, and otherwise
   * none.
   * @warning **Read by PNG, GIF, TIFF and WebP.** The formats whose multi-image
   * form this was written for - APNG, GIF's own frames, TIFF's IFD chain
   * (SubIFD pages too), and WebP ANMF frames. A BMP bitmap array also loads
   * as several items and is not capped.
   */
  size_t max_frame_count;
  /**
   * 0 = no cap on a single segment beyond the length of the stream. Read by
   * PNG, JPEG, GIF and WebP - the
   * formats built out of length-prefixed segments - and by whichever codec a
   * BMP wrapping a PNG or a JPEG, a TIFF whose strips are JPEG, or an icon
   * whose entry is a PNG hands its payload to. A DIB, a TIFF's tag table and
   * an icon directory have no chunk structure, so those three are otherwise
   * unaffected.
   *
   * WebP joined on 2026-10-02. A RIFF file is nothing but length-prefixed
   * chunks, so it was the format this cap most obviously described and the
   * one it did not reach: a one-byte cap refused a PNG, a JPEG and a GIF and
   * let every WebP through. It is enforced in webp_push_chunk(), which both
   * the top-level RIFF walk and the nested walk inside an ANMF payload call,
   * so a cap cannot be stepped around by nesting a bitstream in a frame.
   */
  size_t max_chunk_size;
  uint8_t _reserved[8]; ///< Zero; room to grow.
} GIMG_Limits;

/**
 * @brief Initialize limits to the defaults: every field zero, which selects
 * the library's default for that field. Set a field to ::GIMG_LIMIT_NONE to
 * remove its limit.
 */
GIMG_API void gimg_limits_default(GIMG_Limits * limits);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GIMG_STREAM_H

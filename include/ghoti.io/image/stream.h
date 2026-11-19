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
 * @brief A stream the caller supplies: a socket, a decompressor, a file read
 * by the caller's own code - anything the library is not to open itself.
 *
 * Every function but @a read (for an input stream) or @a write (for an output
 * one) is optional.  A source with no @a seek is read forward only, like a pipe
 * (gimg_stream_tell() answers (size_t)-1 for it, as for a memory stream that
 * cannot seek, and formats that find their data by offset - BMP, TIFF - are
 * refused on it); one with @a seek is treated as seekable, and one with @a size
 * as having a known length, which lets a decoder refuse a header that names more
 * than the source holds before it allocates.
 *
 * A read asks the callback again until the request is filled or it reports the
 * end of the stream, so a short read is never mistaken for the end.
 *
 * The callbacks are called from the thread that called the library function
 * and never from two at once on one stream.
 */
typedef struct GIMG_Stream_Callbacks {
  /** Read up to @p size bytes; set @p out_read (zero at end of stream). */
  GIMG_Result (*read)(
      void * ctx, void * buffer, size_t size, size_t * out_read);
  /** Write @p size bytes; set @p out_written. */
  GIMG_Result (*write)(
      void * ctx, const void * buffer, size_t size, size_t * out_written);
  /** Move to an absolute offset. */
  GIMG_Result (*seek)(void * ctx, size_t offset);
  /** The total length, or ::GIMG_STREAM_SIZE_UNKNOWN. */
  size_t (*size)(void * ctx);
  /** Called once when the stream is destroyed. */
  void (*close)(void * ctx);
  /** Passed to every callback. */
  void * ctx;
} GIMG_Stream_Callbacks;

/** The most gimg_stream_peek() can return from a stream of the caller's. */
#define GIMG_STREAM_PEEK_MAX 64u

/**
 * @brief Create a stream from the caller's callbacks (copied).
 * @return GIMG_OK, GIMG_ERR_INVALID when neither @a read nor @a write is set
 *   (or both are), or GIMG_ERR_OOM.
 */
GIMG_API GIMG_Result gimg_stream_create_callbacks(
    const GIMG_Stream_Callbacks * callbacks, GIMG_Stream ** out_stream);

/** @brief As gimg_stream_create_callbacks() with an allocator. */
GIMG_API GIMG_Result gimg_stream_create_callbacks_with_allocator(
    const GIMG_Allocator * allocator, const GIMG_Stream_Callbacks * callbacks,
    GIMG_Stream ** out_stream);

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

/**
 * @brief The default for GIMG_Limits.max_memory: two gibibytes of live
 * allocations. It is a sum, so it has to hold the largest raster the default
 * pixel cap admits (2^28 pixels at four bytes, one gibibyte) and the working
 * set beside it.
 */
#define GIMG_DEFAULT_MAX_MEMORY ((size_t)1 << 31)

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
   * 0 = ::GIMG_DEFAULT_MAX_MEMORY. The most memory, in bytes, that the
   * library may hold live at once in the blocks a load or a decode takes: the
   * peak of the sum of its own allocations within one gimg_doc_load() or
   * gimg_item_decode() call.
   *
   * Enforced beneath every codec, in the library's own allocation path, for
   * the whole of the call on the calling thread: the decoded raster, every
   * working buffer, every copied block of metadata. Each block is counted
   * from the allocation that takes it (a request is counted at the size
   * asked for, with no allowance for the allocator's own overhead) until it
   * is freed, so a decode that holds a raster and then a second buffer is
   * charged for both and one that frees the first before taking the second
   * is charged for the larger. An allocation that would take the live total
   * past the cap fails, and the call returns GIMG_ERR_LIMIT; the total
   * never exceeds the cap. Growing a block with the allocator's realloc is
   * charged for the growth; an allocator that moves the block holds old and
   * new together for a moment, which is its own and is not counted.
   *
   * It also bounds gimg_doc_save(), under GIMG_Save_Options::limits when the
   * options carry it, else the limits the document was loaded with, else (a
   * document built in memory) the defaults: the writer's working
   * buffers, the file composed in memory before it is written, the straight
   * copies a premultiplied raster is saved through, an item the save has to
   * decode for itself, and the growth of a memory stream given as the
   * destination, all counted together as above. A save refused by the cap
   * returns GIMG_ERR_LIMIT and writes nothing to the stream.
   * gimg_item_ensure_decoded() is a gimg_item_decode().
   *
   * What it does not bound: memory the call did not take (the stream's own
   * buffers, the caller's allocator's overhead, blocks taken before the call
   * such as a document's earlier decodes or the rasters a save is given,
   * which are not counted again), and the operations in ops.h and the
   * raster constructors when a caller makes them directly: they take no
   * limits, so what they allocate is the caller's to size. The returned
   * raster is counted while the call runs; once the call returns it is the
   * caller's. Each call gets the whole cap afresh. With the default, a
   * picture whose raster and working set pass two gibibytes needs this
   * raised as well as max_decoded_pixels.
   *
   * An inner load made by the library (a thumbnail, an embedded image) can
   * tighten the cap for itself and is also held to this one.
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
   * 0 = each codec's own built-in bound: 65536 frames for APNG and GIF, 1024
   * for WebP, 4096 directories for TIFF. GIMG_LIMIT_NONE lifts it.
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

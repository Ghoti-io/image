/**
 * @file
 *
 * Internal stream structures.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GIMG_SRC_STREAM_STREAM_INTERNAL_H
#define GHOTI_IO_GIMG_SRC_STREAM_STREAM_INTERNAL_H

#include <ghoti.io/image/macros.h>

#include <ghoti.io/image/allocator.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/stream.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Stream implementation (memory stream).
 * For read streams: data is input buffer, size is length, position is read
 * offset. For output streams: data is write buffer (owned, may realloc), size
 * is capacity, position is bytes written.
 */
struct GIMG_Stream {
  const GIMG_Allocator * allocator;
  unsigned char * data;  ///< Read: const in use; output: owned, growable.
  size_t size;           ///< Read: total bytes; output: capacity.
  size_t position;       ///< Read: read offset; output: bytes written.
  GIMG_Result error;     ///< Stored error state.
  bool can_seek;         ///< True if seek/tell/size supported.
  bool writable;         ///< True if stream supports write (output stream).
  size_t max_read_chunk; ///< Max bytes to return per read (0 = no limit). Used
                         ///< for chunked/short-read testing.
};

#endif // GHOTI_IO_GIMG_SRC_STREAM_STREAM_INTERNAL_H

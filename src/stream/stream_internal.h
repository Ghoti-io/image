/**
 * @file
 *
 * Internal stream structures.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GIMG_STREAM_INTERNAL_H
#define GHOTI_IO_GIMG_STREAM_INTERNAL_H

#include <ghoti.io/image/allocator.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/stream.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Stream implementation (memory stream).
 */
struct GIMG_STREAM {
  const GIMG_ALLOCATOR * allocator;
  const unsigned char * data;
  size_t size;
  size_t position;
  GIMG_RESULT error;  ///< Stored error state.
  bool can_seek;      ///< True if seek/tell/size supported.
};

#endif // GHOTI_IO_GIMG_STREAM_INTERNAL_H

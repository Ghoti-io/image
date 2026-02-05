/**
 * @file
 *
 * Internal codec registry structures.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GIMG_CODEC_INTERNAL_H
#define GHOTI_IO_GIMG_CODEC_INTERNAL_H

#include <ghoti.io/image/allocator.h>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/stream.h>
#include <stddef.h>

/**
 * @brief Magic signature for probing.
 */
typedef struct {
  const unsigned char * bytes;
  size_t length;
  size_t offset; ///< Offset in stream where magic appears (usually 0).
} gimg_codec_magic_t;

/**
 * @brief Codec descriptor (registry entry).
 */
struct GIMG_CODEC {
  const GIMG_ALLOCATOR * allocator;
  char * name;
  gimg_codec_magic_t * magics;
  size_t magic_count;
  unsigned int capabilities;  ///< Read/write etc. (bitmask for later).
};

#endif // GHOTI_IO_GIMG_CODEC_INTERNAL_H

/**
 * @file
 *
 * Internal document and item structures.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GIMG_DOC_INTERNAL_H
#define GHOTI_IO_GIMG_DOC_INTERNAL_H

#include <ghoti.io/image/allocator.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
#include <stddef.h>
#include <stdint.h>

struct GIMG_Codec;

/**
 * @brief Single item (page/frame/level/thumbnail).
 */
struct GIMG_Item {
  size_t index;               ///< Index in parent doc.
  GIMG_Doc * doc;             ///< Parent document (for decode dispatch).
  uint16_t frame_delay_num;   ///< Frame delay numerator (fcTL delay_num).
  uint16_t frame_delay_den;   ///< Frame delay denominator (fcTL delay_den).
  GIMG_Dispose_Op dispose_op; ///< Dispose op (fcTL dispose_op).
  GIMG_Blend_Op blend_op;     ///< Blend op (fcTL blend_op).
};

/**
 * @brief Document container.
 */
struct GIMG_Doc {
  const GIMG_Allocator * allocator;
  GIMG_Item * items;
  size_t item_count;
  struct GIMG_Codec * loaded_by_codec; ///< Codec that loaded this doc (NULL if
                                       ///< created, not loaded).
  void * codec_private; ///< Format-specific state; owned and freed by codec.
  GIMG_Meta_Raw *
      meta_raw; ///< Optional raw metadata (eXIf, etc.); owned by doc.
};

#endif // GHOTI_IO_GIMG_DOC_INTERNAL_H

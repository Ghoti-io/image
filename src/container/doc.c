/**
 * @file
 *
 * Document and item create/destroy and accessors.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <string.h>

#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/raster.h>

#include "../codec/codec_internal.h"
#include "../core/alloc_internal.h"
#include "doc_internal.h"

GIMG_API GIMG_Result gimg_doc_create(GIMG_Doc ** out_doc) {
  return gimg_doc_create_with_allocator(NULL, out_doc);
}

GIMG_API GIMG_Result gimg_doc_create_with_allocator(
    const GIMG_Allocator * allocator, GIMG_Doc ** out_doc) {
  if (!out_doc) {
    return GIMG_ERR_INTERNAL;
  }
  allocator = gimg_alloc_or_default(allocator);
  GIMG_Doc * doc = (GIMG_Doc *)gimg_malloc(allocator, sizeof(GIMG_Doc));
  if (!doc) {
    return GIMG_ERR_OOM;
  }
  // Zero the whole structure: the fields below are assigned individually,
  // so anything added to GIMG_Doc later would otherwise start as whatever
  // malloc returned.
  memset(doc, 0, sizeof(*doc));
  doc->allocator = allocator;
  doc->item_count = 1;
  doc->items = (GIMG_Item *)gimg_malloc(allocator, sizeof(GIMG_Item));
  if (!doc->items) {
    gimg_free(allocator, doc);
    return GIMG_ERR_OOM;
  }
  doc->loaded_by_codec = NULL;
  doc->codec_private = NULL;
  doc->meta_raw = NULL;
  doc->meta_common = NULL;
  doc->items[0].index = 0;
  doc->items[0].doc = doc;
  doc->items[0].frame_delay_num = 0;
  doc->items[0].frame_delay_den = 0;
  doc->items[0].dispose_op = GIMG_DISPOSE_NONE;
  doc->items[0].blend_op = GIMG_BLEND_SOURCE;
  doc->items[0].raster = NULL;
  *out_doc = doc;
  return GIMG_OK;
}

GIMG_API void gimg_doc_destroy(GIMG_Doc * doc) {
  if (!doc) {
    return;
  }
  if (doc->meta_common) {
    gimg_meta_common_destroy(doc->meta_common);
    doc->meta_common = NULL;
  }
  if (doc->meta_raw) {
    gimg_meta_raw_destroy(doc->meta_raw);
    doc->meta_raw = NULL;
  }
  if (doc->loaded_by_codec && doc->codec_private) {
    GIMG_Codec * c = (GIMG_Codec *)doc->loaded_by_codec;
    if (c->free_doc_private) {
      c->free_doc_private(c, doc->codec_private);
    }
  }
  if (doc->items) {
    for (size_t i = 0; i < doc->item_count; i++) {
      if (doc->items[i].raster) {
        gimg_raster_destroy(doc->items[i].raster);
        doc->items[i].raster = NULL;
      }
    }
  }
  const GIMG_Allocator * alloc = doc->allocator;
  gimg_free(alloc, doc->items);
  gimg_free(alloc, doc);
}

GIMG_API size_t gimg_doc_item_count(const GIMG_Doc * doc) {
  return doc ? doc->item_count : 0;
}

GIMG_API GIMG_Item * gimg_doc_item(const GIMG_Doc * doc, size_t index) {
  if (!doc || !doc->items || index >= doc->item_count) {
    return NULL;
  }
  return &doc->items[index];
}

GIMG_API GIMG_Result gimg_doc_set_item_count(GIMG_Doc * doc, size_t count) {
  if (!doc || count < 1) {
    return GIMG_ERR_INTERNAL;
  }
  const GIMG_Allocator * alloc = doc->allocator;
  if (count == doc->item_count) {
    return GIMG_OK;
  }
  GIMG_Item * new_items =
      (GIMG_Item *)gimg_malloc(alloc, count * sizeof(GIMG_Item));
  if (!new_items) {
    return GIMG_ERR_OOM;
  }
  size_t copy_count = count < doc->item_count ? count : doc->item_count;
  if (copy_count > 0) {
    memcpy(new_items, doc->items, copy_count * sizeof(GIMG_Item));
  }
  for (size_t i = copy_count; i < count; i++) {
    new_items[i].index = i;
    new_items[i].doc = doc;
    new_items[i].frame_delay_num = 0;
    new_items[i].frame_delay_den = 0;
    new_items[i].dispose_op = GIMG_DISPOSE_NONE;
    new_items[i].blend_op = GIMG_BLEND_SOURCE;
    new_items[i].raster = NULL;
  }
  if (count < doc->item_count) {
    for (size_t i = count; i < doc->item_count; i++) {
      if (doc->items[i].raster) {
        gimg_raster_destroy(doc->items[i].raster);
      }
    }
  }
  for (size_t i = 0; i < count; i++) {
    new_items[i].doc = doc;
    new_items[i].index = i;
  }
  gimg_free(alloc, doc->items);
  doc->items = new_items;
  doc->item_count = count;
  return GIMG_OK;
}

GIMG_API int gimg_doc_loop_count(const GIMG_Doc * doc, uint32_t * out_count) {
  if (!doc || !doc->has_loop_count) {
    return 0;
  }
  if (out_count) {
    *out_count = doc->loop_count;
  }
  return 1;
}

GIMG_API void gimg_doc_set_loop_count(GIMG_Doc * doc, uint32_t count) {
  if (!doc) {
    return;
  }
  doc->loop_count = count;
  doc->has_loop_count = 1;
}

GIMG_API void gimg_doc_clear_loop_count(GIMG_Doc * doc) {
  if (!doc) {
    return;
  }
  doc->loop_count = 0;
  doc->has_loop_count = 0;
}

GIMG_API int gimg_doc_background_color(
    const GIMG_Doc * doc, uint8_t * out_rgba) {
  if (!doc || !doc->has_background) {
    return 0;
  }
  if (out_rgba) {
    memcpy(out_rgba, doc->background, sizeof(doc->background));
  }
  return 1;
}

GIMG_API void gimg_doc_set_background_color(
    GIMG_Doc * doc, const uint8_t * rgba) {
  if (!doc || !rgba) {
    return;
  }
  memcpy(doc->background, rgba, sizeof(doc->background));
  doc->has_background = 1;
}

GIMG_API void gimg_doc_clear_background_color(GIMG_Doc * doc) {
  if (!doc) {
    return;
  }
  memset(doc->background, 0, sizeof(doc->background));
  doc->has_background = 0;
}

GIMG_API int gimg_doc_pixel_aspect_ratio(
    const GIMG_Doc * doc, uint32_t * out_num, uint32_t * out_den) {
  if (!doc || !doc->has_aspect) {
    return 0;
  }
  if (out_num) {
    *out_num = doc->aspect_num;
  }
  if (out_den) {
    *out_den = doc->aspect_den;
  }
  return 1;
}

GIMG_API void gimg_doc_set_pixel_aspect_ratio(
    GIMG_Doc * doc, uint32_t num, uint32_t den) {
  // A zero term is not a ratio, and storing one would hand the caller a
  // division by zero later dressed up as an answer.
  if (!doc || num == 0u || den == 0u) {
    return;
  }
  doc->aspect_num = num;
  doc->aspect_den = den;
  doc->has_aspect = 1;
}

GIMG_API void gimg_doc_clear_pixel_aspect_ratio(GIMG_Doc * doc) {
  if (!doc) {
    return;
  }
  doc->aspect_num = 0;
  doc->aspect_den = 0;
  doc->has_aspect = 0;
}

GIMG_API void gimg_item_frame_delay(
    const GIMG_Item * item, uint16_t * num, uint16_t * den) {
  if (!item) {
    if (num) {
      *num = 0;
    }
    if (den) {
      *den = 0;
    }
    return;
  }
  if (num) {
    *num = item->frame_delay_num;
  }
  if (den) {
    *den = item->frame_delay_den;
  }
}

GIMG_API void gimg_item_set_frame_delay(
    GIMG_Item * item, uint16_t num, uint16_t den) {
  if (item) {
    item->frame_delay_num = num;
    item->frame_delay_den = den;
  }
}

GIMG_API GIMG_Dispose_Op gimg_item_dispose_op(const GIMG_Item * item) {
  return item ? item->dispose_op : GIMG_DISPOSE_NONE;
}

GIMG_API void gimg_item_set_dispose_op(GIMG_Item * item, GIMG_Dispose_Op op) {
  if (item && (unsigned)op < (unsigned)GIMG_DISPOSE_OP_COUNT) {
    item->dispose_op = op;
  }
}

GIMG_API GIMG_Blend_Op gimg_item_blend_op(const GIMG_Item * item) {
  return item ? item->blend_op : GIMG_BLEND_SOURCE;
}

GIMG_API void gimg_item_set_blend_op(GIMG_Item * item, GIMG_Blend_Op op) {
  if (item && (unsigned)op < (unsigned)GIMG_BLEND_OP_COUNT) {
    item->blend_op = op;
  }
}

GIMG_API GIMG_Raster * gimg_item_raster(const GIMG_Item * item) {
  return item ? item->raster : NULL;
}

GIMG_API void gimg_item_set_raster(GIMG_Item * item, GIMG_Raster * raster) {
  if (!item) {
    return;
  }
  if (item->raster) {
    gimg_raster_destroy(item->raster);
  }
  item->raster = raster;
}

GIMG_API GIMG_Meta_Raw * gimg_doc_meta_raw(const GIMG_Doc * doc) {
  return doc ? doc->meta_raw : NULL;
}

GIMG_API GIMG_Result gimg_doc_ensure_meta_raw(
    GIMG_Doc * doc, GIMG_Meta_Raw ** out_raw) {
  if (!doc || !out_raw) {
    return GIMG_ERR_INTERNAL;
  }
  *out_raw = NULL;
  if (doc->meta_raw) {
    *out_raw = doc->meta_raw;
    return GIMG_OK;
  }
  GIMG_Result r =
      gimg_meta_raw_create_with_allocator(doc->allocator, &doc->meta_raw);
  if (r != GIMG_OK) {
    return r;
  }
  *out_raw = doc->meta_raw;
  return GIMG_OK;
}

GIMG_API GIMG_Meta_Common * gimg_doc_meta_common(const GIMG_Doc * doc) {
  return doc ? doc->meta_common : NULL;
}

GIMG_API GIMG_Result gimg_doc_ensure_meta_common(
    GIMG_Doc * doc, GIMG_Meta_Common ** out_meta) {
  if (!doc || !out_meta) {
    return GIMG_ERR_INTERNAL;
  }
  *out_meta = NULL;
  if (doc->meta_common) {
    *out_meta = doc->meta_common;
    return GIMG_OK;
  }
  GIMG_Result r = gimg_meta_common_create_with_allocator(
      doc->allocator, &doc->meta_common);
  if (r != GIMG_OK) {
    return r;
  }
  *out_meta = doc->meta_common;
  return GIMG_OK;
}

GIMG_API GIMG_Result gimg_doc_copy(const GIMG_Doc * src, GIMG_Doc ** out_doc) {
  return gimg_doc_copy_with_allocator(NULL, src, out_doc);
}

// Document copy: duplicate structure and attached rasters for save variants or
// moving to another doc. Copied: item count; per-item frame_delay, dispose_op,
// blend_op; attached rasters (via gimg_raster_copy); meta_common (deep);
// meta_raw (deep); the document's loop count, background colour and pixel
// aspect ratio. Not copied: loaded_by_codec,
// codec_private — the result is
// a synthetic document. Items without an attached raster (e.g. loaded but not
// decoded) yield items with no raster in the copy. Caller owns the new doc.
GIMG_API GIMG_Result gimg_doc_copy_with_allocator(
    const GIMG_Allocator * allocator, const GIMG_Doc * src,
    GIMG_Doc ** out_doc) {
  if (!src || !out_doc) {
    return GIMG_ERR_INTERNAL;
  }
  *out_doc = NULL;
  GIMG_Result r = gimg_doc_create_with_allocator(allocator, out_doc);
  if (r != GIMG_OK) {
    return r;
  }
  GIMG_Doc * doc = *out_doc;
  size_t n = gimg_doc_item_count(src);
  if (n > 1) {
    r = gimg_doc_set_item_count(doc, n);
    if (r != GIMG_OK) {
      gimg_doc_destroy(doc);
      *out_doc = NULL;
      return r;
    }
  }
  uint8_t background[4];
  if (gimg_doc_background_color(src, background)) {
    gimg_doc_set_background_color(doc, background);
  }
  uint32_t aspect_num = 0, aspect_den = 0;
  if (gimg_doc_pixel_aspect_ratio(src, &aspect_num, &aspect_den)) {
    gimg_doc_set_pixel_aspect_ratio(doc, aspect_num, aspect_den);
  }
  uint32_t loop = 0;
  if (gimg_doc_loop_count(src, &loop)) {
    // Unlike codec_private, this is a property of the animation rather than of
    // the codec that read it, so it survives into the synthetic copy - which
    // is what makes "load, copy, save" preserve how many times to play.
    gimg_doc_set_loop_count(doc, loop);
  }
  for (size_t i = 0; i < n; i++) {
    const GIMG_Item * si = gimg_doc_item(src, i);
    GIMG_Item * di = gimg_doc_item(doc, i);
    if (!si || !di) {
      continue;
    }
    uint16_t num = 0, den = 0;
    gimg_item_frame_delay(si, &num, &den);
    gimg_item_set_frame_delay(di, num, den);
    gimg_item_set_dispose_op(di, gimg_item_dispose_op(si));
    gimg_item_set_blend_op(di, gimg_item_blend_op(si));
    GIMG_Raster * sr = gimg_item_raster(si);
    if (sr) {
      GIMG_Raster * copy_r = NULL;
      r = gimg_raster_copy_with_allocator(doc->allocator, sr, &copy_r);
      if (r != GIMG_OK) {
        gimg_doc_destroy(doc);
        *out_doc = NULL;
        return r;
      }
      gimg_item_set_raster(di, copy_r);
    }
  }
  if (src->meta_common) {
    r = gimg_meta_common_create_with_allocator(doc->allocator,
        &doc->meta_common);
    if (r != GIMG_OK) {
      gimg_doc_destroy(doc);
      *out_doc = NULL;
      return r;
    }
    gimg_meta_common_set_orientation(doc->meta_common,
        gimg_meta_common_orientation(src->meta_common));
    uint32_t x = 0, y = 0;
    gimg_meta_common_dpi(src->meta_common, &x, &y);
    gimg_meta_common_set_dpi(doc->meta_common, x, y);
    const char * desc = gimg_meta_common_description(src->meta_common);
    if (desc) {
      r = gimg_meta_common_set_description(doc->meta_common, desc);
      if (r != GIMG_OK) {
        gimg_doc_destroy(doc);
        *out_doc = NULL;
        return r;
      }
    }
  }
  if (src->meta_raw) {
    r = gimg_meta_raw_copy_with_allocator(doc->allocator, src->meta_raw,
        &doc->meta_raw);
    if (r != GIMG_OK) {
      gimg_doc_destroy(doc);
      *out_doc = NULL;
      return r;
    }
  }
  return GIMG_OK;
}

GIMG_API GIMG_Result gimg_doc_from_raster(const GIMG_Raster * raster,
    GIMG_Doc ** out_doc) {
  return gimg_doc_from_raster_with_allocator(NULL, raster, out_doc);
}

GIMG_API GIMG_Result gimg_doc_from_raster_with_allocator(
    const GIMG_Allocator * allocator, const GIMG_Raster * raster,
    GIMG_Doc ** out_doc) {
  if (!raster || !out_doc) {
    return GIMG_ERR_INTERNAL;
  }
  *out_doc = NULL;
  GIMG_Result r = gimg_doc_create_with_allocator(allocator, out_doc);
  if (r != GIMG_OK) {
    return r;
  }
  GIMG_Raster * copy_r = NULL;
  r = gimg_raster_copy_with_allocator(
      (*out_doc)->allocator, raster, &copy_r);
  if (r != GIMG_OK) {
    gimg_doc_destroy(*out_doc);
    *out_doc = NULL;
    return r;
  }
  gimg_item_set_raster(gimg_doc_item(*out_doc, 0), copy_r);
  return GIMG_OK;
}

GIMG_API GIMG_Result gimg_item_copy(const GIMG_Item * src_item,
    GIMG_Item * dst_item) {
  if (!src_item || !dst_item) {
    return GIMG_ERR_INTERNAL;
  }
  gimg_item_set_frame_delay(dst_item,
      (uint16_t)(src_item->frame_delay_num),
      (uint16_t)(src_item->frame_delay_den));
  gimg_item_set_dispose_op(dst_item, src_item->dispose_op);
  gimg_item_set_blend_op(dst_item, src_item->blend_op);
  GIMG_Raster * sr = gimg_item_raster(src_item);
  if (sr) {
    const GIMG_Allocator * alloc = dst_item->doc ? dst_item->doc->allocator
                                                  : NULL;
    GIMG_Raster * copy_r = NULL;
    GIMG_Result r = gimg_raster_copy_with_allocator(alloc, sr, &copy_r);
    if (r != GIMG_OK) {
      return r;
    }
    gimg_item_set_raster(dst_item, copy_r);
  }
  else {
    gimg_item_set_raster(dst_item, NULL);
  }
  return GIMG_OK;
}

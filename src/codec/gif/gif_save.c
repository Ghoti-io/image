/**
 * @file
 *
 * GIF save: one image block per frame, each the whole logical screen.
 *
 * BUILDING A PALETTE, AND WHY NOTHING IS QUANTIZED
 * ================================================
 *
 * A GIF pixel is an index into a table of at most 256 colours (89a 18).  An
 * image with more colours than that cannot be written without deciding which
 * to keep, and that decision is quantization - an image-processing choice, not
 * a codec's.  The PNG writer in this library draws the line in the same place
 * and for the same reason: when an image already has 256 colours or fewer
 * there is nothing to choose, because exactly one palette reproduces it, and
 * building that table is a way of storing what is already there.
 *
 * So a raster of more than 256 colours is refused with
 * GIMG_ERR_UNSUPPORTED.  A caller that wants a photograph as a GIF reduces
 * the colours first, with an operation that says what it did.
 *
 * Transparency is the same argument one bit down.  GIF designates a single
 * index transparent and treats every other pixel as opaque; a half-covered
 * edge has nowhere to go.  A partially transparent pixel is therefore refused
 * unless the caller sets `gif_alpha_threshold` and says which way to round.
 *
 * WHAT IS NOT OPTIMIZED
 * =====================
 *
 * Every frame is written at full canvas size with disposal left unspecified.
 * A GIF writer aiming at small files writes each frame as the smallest
 * rectangle that changed and marks the untouched pixels transparent; this one
 * does not, so an animation written here is larger than one an optimizing
 * encoder produces and shows the same pixels.  See documentation/formats/gif.md.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <string.h>

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>

#include "../../container/doc_internal.h"
#include "../../core/alloc_internal.h"
#include "../../core/safe_math_internal.h"
#include "../codec_internal.h"
#include "gif_internal.h"

// ---------------------------------------------------------------------------
// Colour lookup
// ---------------------------------------------------------------------------

/** Slots in the colour lookup: twice the largest palette, so it stays sparse. */
#define GIMG_GIF_LUT_SLOTS 512u

/**
 * Open-addressed map from an RGB8 colour to a palette index.
 *
 * The writer needs an index for every pixel and a palette holds up to 256
 * entries, so a linear scan costs 256 comparisons per pixel.  The same shape
 * as the PNG writer's, and for the same reason; a `used` array rather than a
 * sentinel key, because black is a real colour and would make a poor "empty".
 */
typedef struct {
  uint32_t key[GIMG_GIF_LUT_SLOTS];
  uint8_t value[GIMG_GIF_LUT_SLOTS];
  uint8_t used[GIMG_GIF_LUT_SLOTS];
  size_t count;
} gif_color_lut_t;

static void gif_lut_init(gif_color_lut_t * lut) {
  memset(lut->used, 0, sizeof(lut->used));
  lut->count = 0;
}

/** Fibonacci hashing: one multiply, then take the high bits. */
static size_t gif_lut_slot(uint32_t key) {
  return (size_t)((key * UINT32_C(2654435761)) >> 23) &
      (GIMG_GIF_LUT_SLOTS - 1u);
}

static bool gif_lut_get(
    const gif_color_lut_t * lut, uint32_t key, uint8_t * out_value) {
  size_t i = gif_lut_slot(key);
  for (size_t probe = 0; probe < GIMG_GIF_LUT_SLOTS; probe++) {
    if (!lut->used[i]) {
      return false;
    }
    if (lut->key[i] == key) {
      *out_value = lut->value[i];
      return true;
    }
    i = (i + 1u) & (GIMG_GIF_LUT_SLOTS - 1u);
  }
  return false;
}

/** Insert, or leave an existing entry alone.  False when the table is full. */
static bool gif_lut_put(gif_color_lut_t * lut, uint32_t key, uint8_t value,
    size_t limit) {
  size_t i = gif_lut_slot(key);
  for (size_t probe = 0; probe < GIMG_GIF_LUT_SLOTS; probe++) {
    if (!lut->used[i]) {
      if (lut->count >= limit) {
        return false;
      }
      lut->used[i] = 1;
      lut->key[i] = key;
      lut->value[i] = value;
      lut->count++;
      return true;
    }
    if (lut->key[i] == key) {
      return true;
    }
    i = (i + 1u) & (GIMG_GIF_LUT_SLOTS - 1u);
  }
  return false;
}

static uint32_t gif_rgb_key(const unsigned char * p) {
  return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

// ---------------------------------------------------------------------------
// Turning a raster into indices
// ---------------------------------------------------------------------------

/** What one frame turned into: indices, the table they point at, and alpha. */
typedef struct {
  unsigned char * indices;
  gimg_gif_rgb_t palette[GIMG_GIF_MAX_PALETTE];
  uint16_t palette_count;
  bool has_transparency;
  uint8_t transparent_index;
} gif_frame_plan_t;

/**
 * Collect a raster's distinct colours into a palette and index every pixel.
 *
 * Transparent pixels all take one reserved index, so a frame that uses
 * transparency has 255 colours available rather than 256.
 */
static GIMG_Result gif_plan_frame(const GIMG_Raster * raster,
    const GIMG_Allocator * alloc, uint16_t alpha_threshold,
    gif_frame_plan_t * plan) {
  const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
  if (!fmt || fmt->layout != GIMG_LAYOUT_INTERLEAVED ||
      fmt->channel_count != 4u || fmt->bits_per_channel[0] != 8u) {
    // Every decode in this library hands back RGBA8; anything else reaching
    // here is a raster the caller built, and converting it is not this
    // codec's job.
    return GIMG_ERR_UNSUPPORTED;
  }
  const uint32_t width = gimg_raster_width(raster);
  const uint32_t height = gimg_raster_height(raster);
  if (width == 0u || height == 0u || width > 0xFFFFu || height > 0xFFFFu) {
    return GIMG_ERR_UNSUPPORTED;
  }

  size_t pixels = 0;
  if (!gcu_safe_mul_size((size_t)width, (size_t)height, &pixels)) {
    return GIMG_ERR_LIMIT;
  }
  plan->indices = (unsigned char *)gimg_malloc(alloc, pixels);
  if (!plan->indices) {
    return GIMG_ERR_OOM;
  }

  const uint8_t * base =
      (const uint8_t *)gimg_raster_pixels_const(raster);
  const size_t stride = gimg_raster_stride_bytes(raster);

  // A first pass settles whether any pixel is transparent, because that
  // decides which index the colours start at: the transparent one has to be a
  // real entry in the table and cannot also be a colour.
  bool needs_transparent = false;
  for (uint32_t y = 0; y < height && !needs_transparent; y++) {
    const uint8_t * row = base + (size_t)y * stride;
    for (uint32_t x = 0; x < width; x++) {
      const uint8_t a = row[(size_t)x * 4u + 3u];
      if (a == 0u) {
        needs_transparent = true;
        break;
      }
      if (a != 255u) {
        if (alpha_threshold == 0u) {
          gimg_free(alloc, plan->indices);
          plan->indices = NULL;
          return GIMG_ERR_UNSUPPORTED;
        }
        if (a < alpha_threshold) {
          needs_transparent = true;
          break;
        }
      }
    }
  }

  gif_color_lut_t lut;
  gif_lut_init(&lut);
  plan->has_transparency = needs_transparent;
  plan->transparent_index = 0u;
  plan->palette_count = 0u;

  // Index 0 is the transparent one when there is one, so that a decoder which
  // ignores the control block shows the background rather than a stray colour.
  // Sixteen bits, not eight: a full table assigns index 255 and then has to
  // count past it to say the table holds 256.  As a uint8_t that increment
  // wrapped to zero, and a 256-colour image was written with a one-entry
  // palette - the only size a GIF can hold that this made unreachable.
  uint16_t next_index = needs_transparent ? 1u : 0u;
  const size_t color_limit =
      needs_transparent ? GIMG_GIF_MAX_PALETTE - 1u : GIMG_GIF_MAX_PALETTE;
  if (needs_transparent) {
    plan->palette[0].r = 0u;
    plan->palette[0].g = 0u;
    plan->palette[0].b = 0u;
  }

  for (uint32_t y = 0; y < height; y++) {
    const uint8_t * row = base + (size_t)y * stride;
    for (uint32_t x = 0; x < width; x++) {
      const uint8_t * px = row + (size_t)x * 4u;
      const uint8_t a = px[3];
      bool transparent = a == 0u;
      if (!transparent && a != 255u) {
        if (alpha_threshold == 0u) {
          gimg_free(alloc, plan->indices);
          plan->indices = NULL;
          return GIMG_ERR_UNSUPPORTED;
        }
        transparent = a < alpha_threshold;
      }
      if (transparent) {
        plan->indices[(size_t)y * width + x] = 0u;
        continue;
      }
      const uint32_t key = gif_rgb_key(px);
      uint8_t index = 0u;
      if (!gif_lut_get(&lut, key, &index)) {
        if (!gif_lut_put(&lut, key, (uint8_t)next_index, color_limit)) {
          // A 257th colour, or a 256th alongside transparency.  Refused rather
          // than quantized; see the file comment.
          gimg_free(alloc, plan->indices);
          plan->indices = NULL;
          return GIMG_ERR_UNSUPPORTED;
        }
        index = (uint8_t)next_index;
        plan->palette[index].r = px[0];
        plan->palette[index].g = px[1];
        plan->palette[index].b = px[2];
        next_index++;
      }
      plan->indices[(size_t)y * width + x] = index;
    }
  }

  plan->palette_count = next_index;
  if (plan->palette_count == 0u) {
    // Every pixel was transparent: the table still needs its one entry.
    plan->palette_count = 1u;
  }
  return GIMG_OK;
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

/**
 * Write a whole buffer, or fail.
 *
 * gimg_stream_write() refuses a NULL out_bytes_written with
 * GIMG_ERR_INTERNAL, and reports a partial write by returning OK with a
 * smaller count.  Neither is something a caller wants to remember at thirty
 * call sites, and a short write that reads as success is how a truncated file
 * gets produced without an error.
 */
static GIMG_Result gif_write(
    GIMG_Stream * stream, const void * data, size_t size) {
  size_t written = 0;
  const GIMG_Result r = gimg_stream_write(stream, data, size, &written);
  if (r != GIMG_OK) {
    return r;
  }
  return written == size ? GIMG_OK : GIMG_ERR_IO;
}

/** Colour-table size field: the exponent e such that the table has 2^(e+1). */
static uint8_t gif_table_bits(uint16_t count) {
  uint8_t bits = 0u;
  while ((1u << (bits + 1u)) < count) {
    bits++;
  }
  return bits;
}

/** The smallest LZW code size that can name every entry (89a 22). */
static uint8_t gif_min_code_size(uint16_t count) {
  uint8_t size = 2u;
  while ((1u << size) < count) {
    size++;
  }
  return size;
}

static GIMG_Result gif_write_u16(GIMG_Stream * stream, uint16_t v) {
  unsigned char b[2] = {(unsigned char)(v & 0xFFu),
      (unsigned char)((v >> 8) & 0xFFu)};
  return gif_write(stream, b, sizeof(b));
}

/** Write a colour table padded to the 2^(bits+1) entries the field implies. */
static GIMG_Result gif_write_table(GIMG_Stream * stream,
    const gimg_gif_rgb_t * palette, uint16_t count, uint8_t bits) {
  const uint32_t entries = 1u << (bits + 1u);
  for (uint32_t i = 0; i < entries; i++) {
    unsigned char rgb[3] = {0, 0, 0};
    if (i < count) {
      rgb[0] = palette[i].r;
      rgb[1] = palette[i].g;
      rgb[2] = palette[i].b;
    }
    GIMG_Result r = gif_write(stream, rgb, sizeof(rgb));
    if (r != GIMG_OK) {
      return r;
    }
  }
  return GIMG_OK;
}

/** Chop a payload into the length-prefixed chain the format uses (89a 15). */
static GIMG_Result gif_write_sub_blocks(
    GIMG_Stream * stream, const unsigned char * data, size_t size) {
  size_t offset = 0;
  while (offset < size) {
    size_t piece = size - offset;
    if (piece > 255u) {
      piece = 255u;
    }
    unsigned char len = (unsigned char)piece;
    GIMG_Result r = gif_write(stream, &len, 1u);
    if (r != GIMG_OK) {
      return r;
    }
    r = gif_write(stream, data + offset, piece);
    if (r != GIMG_OK) {
      return r;
    }
    offset += piece;
  }
  const unsigned char terminator = 0u;
  return gif_write(stream, &terminator, 1u);
}

/**
 * Write one Comment Extension: the introducer, the label, and the text as a
 * sub-block chain (89a 24).
 */
static GIMG_Result gif_write_comment(
    GIMG_Stream * stream, const unsigned char * text, size_t len) {
  const unsigned char head[2] = {0x21u, 0xFEu};
  GIMG_Result r = gif_write(stream, head, sizeof(head));
  if (r != GIMG_OK) {
    return r;
  }
  return gif_write_sub_blocks(stream, text, len);
}

/**
 * Write the document's comments, before the first image (89a 24 places no
 * constraint; every writer puts them here and a reader looking for a
 * file-level comment looks here).
 *
 * The raw block is preferred when present, because it holds every comment the
 * source carried rather than just the one that was normalized.  The common
 * description is the fallback, which is what a caller who built a document by
 * hand - or edited the description of a loaded one - will have set.
 *
 * A caller who edits the description of a document that also carries a raw
 * block is asking two things at once; the raw block wins, because it is the
 * more specific statement.  Dropping the raw block (GIMG_META_KEEP_COMMON_ONLY)
 * is how the caller says the description is the one they mean.
 */
static GIMG_Result gif_write_comments(GIMG_Stream * stream,
    const GIMG_Doc * doc, const GIMG_Save_Options * options,
    const GIMG_Allocator * alloc) {
  const GIMG_Meta_Policy policy =
      options ? options->metadata_policy : GIMG_META_PRESERVE_ALL;
  if (policy == GIMG_META_DROP_ALL) {
    return GIMG_OK;
  }

  if (policy != GIMG_META_KEEP_COMMON_ONLY) {
    GIMG_Meta_Raw * raw = gimg_doc_meta_raw(doc);
    size_t size = 0;
    if (raw &&
        gimg_meta_raw_get(raw, "gif", GIMG_GIF_RAW_COMMENT, NULL, &size) ==
            GIMG_OK &&
        size > 0u) {
      unsigned char * block = (unsigned char *)gimg_malloc(alloc, size);
      if (!block) {
        return GIMG_ERR_OOM;
      }
      GIMG_Result r =
          gimg_meta_raw_get(raw, "gif", GIMG_GIF_RAW_COMMENT, block, &size);
      size_t offset = 0;
      while (r == GIMG_OK &&
          offset + GIMG_GIF_RAW_COMMENT_PREFIX <= size) {
        const size_t len = ((size_t)block[offset] << 24) |
            ((size_t)block[offset + 1] << 16) |
            ((size_t)block[offset + 2] << 8) | (size_t)block[offset + 3];
        offset += GIMG_GIF_RAW_COMMENT_PREFIX;
        if (len > size - offset) {
          // A block whose framing does not add up is not this writer's to
          // guess at; what has been written stays, and the rest is dropped
          // rather than read past the end of the buffer.
          break;
        }
        if (len > 0u) {
          r = gif_write_comment(stream, block + offset, len);
        }
        offset += len;
      }
      gimg_free(alloc, block);
      return r;
    }
  }

  if (policy == GIMG_META_KEEP_RAW_ONLY) {
    return GIMG_OK;
  }
  GIMG_Meta_Common * common = gimg_doc_meta_common(doc);
  const char * desc = common ? gimg_meta_common_description(common) : NULL;
  if (desc && *desc) {
    return gif_write_comment(
        stream, (const unsigned char *)desc, strlen(desc));
  }
  return GIMG_OK;
}

/** Reorder rows into the four-pass interlace order (89a 20). */
static void gif_interlace_rows(const unsigned char * src, unsigned char * dst,
    uint32_t width, uint32_t height) {
  static const struct {
    uint32_t start;
    uint32_t step;
  } passes[4] = {{0u, 8u}, {4u, 8u}, {2u, 4u}, {1u, 2u}};
  size_t out_row = 0;
  for (size_t p = 0; p < 4u; p++) {
    for (uint32_t y = passes[p].start; y < height; y += passes[p].step) {
      memcpy(dst + out_row * width, src + (size_t)y * width, width);
      out_row++;
    }
  }
}

/** Compress one frame's indices with the GIF LZW profile. */
static GIMG_Result gif_compress_indices(const GIMG_Allocator * alloc,
    const unsigned char * indices, size_t count, uint8_t min_code_size,
    unsigned char ** out_data, size_t * out_size) {
  *out_data = NULL;
  *out_size = 0;

  gcomp_options_t * opts = NULL;
  if (gcomp_options_create(&opts) != GCOMP_OK) {
    return GIMG_ERR_OOM;
  }
  if (gcomp_options_set_string(opts, "lzw.format", "gif") != GCOMP_OK ||
      gcomp_options_set_uint64(opts, "lzw.lit_width", min_code_size) !=
          GCOMP_OK) {
    gcomp_options_destroy(opts);
    return GIMG_ERR_INTERNAL;
  }

  size_t bound = 0;
  if (gcomp_encode_bound(NULL, "lzw", opts, count, &bound) != GCOMP_OK) {
    gcomp_options_destroy(opts);
    return GIMG_ERR_INTERNAL;
  }
  unsigned char * buffer = (unsigned char *)gimg_malloc(alloc, bound);
  if (!buffer) {
    gcomp_options_destroy(opts);
    return GIMG_ERR_OOM;
  }
  size_t written = 0;
  const gcomp_status_t gs = gcomp_encode_buffer(gcomp_registry_default(),
      "lzw", opts, indices, count, buffer, bound, &written);
  gcomp_options_destroy(opts);
  if (gs != GCOMP_OK) {
    gimg_free(alloc, buffer);
    return GIMG_ERR_INTERNAL;
  }
  *out_data = buffer;
  *out_size = written;
  return GIMG_OK;
}

GIMG_Result gimg_gif_save(GIMG_Codec * codec, const GIMG_Doc * doc,
    GIMG_Stream * stream, const char * format_name,
    const GIMG_Save_Options * options, GIMG_Save_Report * report) {
  (void)format_name;
  if (!codec || !doc || !stream || !report) {
    return GIMG_ERR_INTERNAL;
  }
  const size_t frame_count = gimg_doc_item_count(doc);
  if (frame_count == 0u) {
    return GIMG_ERR_UNSUPPORTED;
  }

  const GIMG_Allocator * alloc = gimg_alloc_or_default(codec->allocator);
  const uint16_t alpha_threshold = options ? options->gif_alpha_threshold : 0u;
  const bool interlace = options && options->gif_interlace;

  // The canvas is the first frame's size.  A document whose frames disagree is
  // refused rather than padded: this writer produces full-canvas frames, so a
  // differing size is a question about placement that the caller has not been
  // asked.
  GIMG_Raster * first = gimg_item_raster(gimg_doc_item((GIMG_Doc *)doc, 0));
  bool first_owned = false;
  if (!first) {
    if (gimg_item_decode(gimg_doc_item((GIMG_Doc *)doc, 0), NULL, &first) !=
            GIMG_OK ||
        !first) {
      return GIMG_ERR_UNSUPPORTED;
    }
    first_owned = true;
  }
  const uint32_t canvas_w = gimg_raster_width(first);
  const uint32_t canvas_h = gimg_raster_height(first);
  if (first_owned) {
    gimg_raster_destroy(first);
  }
  if (canvas_w == 0u || canvas_h == 0u || canvas_w > 0xFFFFu ||
      canvas_h > 0xFFFFu) {
    return GIMG_ERR_UNSUPPORTED;
  }

  GIMG_Result r = gif_write(stream, "GIF89a", 6u);
  if (r != GIMG_OK) {
    return r;
  }
  // No Global Color Table: each frame carries its own, because two frames of
  // an animation rarely share one and a global table that fits neither is
  // wasted bytes.  89a 18 makes it optional.
  r = gif_write_u16(stream, (uint16_t)canvas_w);
  if (r == GIMG_OK) {
    r = gif_write_u16(stream, (uint16_t)canvas_h);
  }
  if (r != GIMG_OK) {
    return r;
  }
  {
    // Packed field, background index, aspect ratio.  No global table, so the
    // background index names nothing and is written as zero.
    unsigned char tail[3] = {0x70u, 0x00u, 0x00u};
    r = gif_write(stream, tail, sizeof(tail));
    if (r != GIMG_OK) {
      return r;
    }
  }

  if (frame_count > 1u) {
    const uint16_t loops = options ? options->gif_loop_count : 0u;
    unsigned char ext[14] = {0x21u, 0xFFu, 0x0Bu, 'N', 'E', 'T', 'S', 'C', 'A',
        'P', 'E', '2', '.', '0'};
    r = gif_write(stream, ext, sizeof(ext));
    if (r != GIMG_OK) {
      return r;
    }
    unsigned char body[5] = {0x03u, 0x01u, (unsigned char)(loops & 0xFFu),
        (unsigned char)((loops >> 8) & 0xFFu), 0x00u};
    r = gif_write(stream, body, sizeof(body));
    if (r != GIMG_OK) {
      return r;
    }
  }

  r = gif_write_comments(stream, doc, options, alloc);
  if (r != GIMG_OK) {
    return r;
  }

  for (size_t i = 0; i < frame_count; i++) {
    GIMG_Item * item = gimg_doc_item((GIMG_Doc *)doc, i);
    if (!item) {
      return GIMG_ERR_INTERNAL;
    }
    GIMG_Raster * raster = gimg_item_raster(item);
    bool owned = false;
    if (!raster) {
      if (gimg_item_decode(item, NULL, &raster) != GIMG_OK || !raster) {
        return GIMG_ERR_UNSUPPORTED;
      }
      owned = true;
    }
    if (gimg_raster_width(raster) != canvas_w ||
        gimg_raster_height(raster) != canvas_h) {
      if (owned) {
        gimg_raster_destroy(raster);
      }
      return GIMG_ERR_UNSUPPORTED;
    }

    gif_frame_plan_t plan;
    memset(&plan, 0, sizeof(plan));
    r = gif_plan_frame(raster, alloc, alpha_threshold, &plan);
    if (owned) {
      gimg_raster_destroy(raster);
    }
    if (r != GIMG_OK) {
      return r;
    }

    const uint8_t bits = gif_table_bits(plan.palette_count);
    const uint8_t min_code_size = gif_min_code_size(plan.palette_count);

    // A Graphic Control Extension is written when the frame needs one: to
    // carry a delay, to name the transparent index, or both.
    uint16_t delay_num = 0, delay_den = 0;
    gimg_item_frame_delay(item, &delay_num, &delay_den);
    uint16_t delay_cs = 0;
    if (delay_den != 0u) {
      // The item model carries a fraction; GIF counts hundredths (89a 23).
      delay_cs = (uint16_t)(((uint32_t)delay_num * 100u) / delay_den);
    }
    if (plan.has_transparency || delay_cs != 0u || frame_count > 1u) {
      unsigned char gce[8] = {0x21u, 0xF9u, 0x04u, 0x00u, 0x00u, 0x00u, 0x00u,
          0x00u};
      // Every frame here is the whole canvas, already composited, so each one
      // has to start from an empty screen rather than on top of the last.
      // Disposal 2 clears the frame's rectangle - the whole canvas - after it
      // is shown (89a 23), which makes the frames independent.
      //
      // Leaving this at 0 looks right until a frame has a transparent pixel
      // where the frame before it had an opaque one: the pixel this codec
      // meant to be see-through instead shows the previous frame. Frame 0 is
      // correct either way, and the error grows with the frame index, which is
      // what it looked like - 2937 wrong pixels in frame 1 of one animation,
      // 27776 by frame 3.
      const unsigned char disposal =
          (unsigned char)(frame_count > 1u ? (2u << 2) : 0u);
      gce[3] = (unsigned char)(disposal |
          (unsigned char)(plan.has_transparency ? 0x01u : 0x00u));
      gce[4] = (unsigned char)(delay_cs & 0xFFu);
      gce[5] = (unsigned char)((delay_cs >> 8) & 0xFFu);
      gce[6] = plan.transparent_index;
      r = gif_write(stream, gce, sizeof(gce));
      if (r != GIMG_OK) {
        gimg_free(alloc, plan.indices);
        return r;
      }
    }

    unsigned char desc = 0x2Cu;
    r = gif_write(stream, &desc, 1u);
    if (r == GIMG_OK) {
      r = gif_write_u16(stream, 0u);
    }
    if (r == GIMG_OK) {
      r = gif_write_u16(stream, 0u);
    }
    if (r == GIMG_OK) {
      r = gif_write_u16(stream, (uint16_t)canvas_w);
    }
    if (r == GIMG_OK) {
      r = gif_write_u16(stream, (uint16_t)canvas_h);
    }
    if (r == GIMG_OK) {
      const unsigned char packed =
          (unsigned char)(0x80u | (interlace ? 0x40u : 0x00u) | bits);
      r = gif_write(stream, &packed, 1u);
    }
    if (r == GIMG_OK) {
      r = gif_write_table(stream, plan.palette, plan.palette_count, bits);
    }
    if (r != GIMG_OK) {
      gimg_free(alloc, plan.indices);
      return r;
    }

    const size_t pixels = (size_t)canvas_w * (size_t)canvas_h;
    unsigned char * rows = plan.indices;
    unsigned char * woven = NULL;
    if (interlace) {
      woven = (unsigned char *)gimg_malloc(alloc, pixels);
      if (!woven) {
        gimg_free(alloc, plan.indices);
        return GIMG_ERR_OOM;
      }
      gif_interlace_rows(plan.indices, woven, canvas_w, canvas_h);
      rows = woven;
    }

    unsigned char * codes = NULL;
    size_t codes_size = 0;
    r = gif_compress_indices(
        alloc, rows, pixels, min_code_size, &codes, &codes_size);
    gimg_free(alloc, woven);
    gimg_free(alloc, plan.indices);
    if (r != GIMG_OK) {
      return r;
    }

    r = gif_write(stream, &min_code_size, 1u);
    if (r == GIMG_OK) {
      r = gif_write_sub_blocks(stream, codes, codes_size);
    }
    gimg_free(alloc, codes);
    if (r != GIMG_OK) {
      return r;
    }
  }

  const unsigned char trailer = GIMG_GIF_BLOCK_TRAILER;
  r = gif_write(stream, &trailer, 1u);
  if (r != GIMG_OK) {
    return r;
  }
  report->bytes_written = gimg_stream_tell(stream);
  return GIMG_OK;
}

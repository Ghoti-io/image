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
 * GIF load: walk the block stream and record what each block says.
 *
 * The format is a sequence of blocks with no index and no length field ahead
 * of the whole, so every block has to be walked to reach the next one - an
 * image cannot be skipped without following its sub-block chain to the end.
 * Load therefore reads each image's code stream into memory as it passes, and
 * decode expands it later; nothing is re-read from the stream after load
 * returns, which is what lets a non-seekable stream work.
 *
 * See documentation/formats/gif.md.
 */

#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/stream.h>
#include <string.h>

#include "../../container/doc_internal.h"
#include "../../core/alloc_internal.h"
#include "../../core/safe_math_internal.h"
#include "../codec_internal.h"
#include "gif_internal.h"

static void gif_load_diag(
    GIMG_Diagnostics * d, size_t offset, const char * action) {
  if (!d) {
    return;
  }
  (void)gimg_diagnostics_append(d, "gif", offset, 0u, GIMG_DIAG_ERROR, action);
}

/** Every multi-byte field in a GIF is little-endian (89a 3). */
static uint16_t gif_read_u16(const unsigned char * p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/** Colour table size from a packed field's low three bits (89a 18, 20). */
static uint16_t gif_palette_entries(uint8_t packed) {
  return (uint16_t)(1u << ((packed & 0x07u) + 1u));
}

// ---------------------------------------------------------------------------
// Sub-block chains
// ---------------------------------------------------------------------------

/**
 * Read a chain of Data Sub-blocks (89a 15) and concatenate their payloads.
 *
 * A chain is a run of length-prefixed pieces of at most 255 bytes, ending at a
 * length of zero.  The pieces carry no meaning individually: an LZW code may
 * straddle the boundary between two of them, so what the caller gets back is
 * the joined stream rather than the pieces.
 *
 * Passing NULL for `out_data` skips the chain instead of keeping it, which is
 * what the extensions this codec does not interpret need.
 */
static GIMG_Result gif_read_sub_blocks(GIMG_Stream * stream,
    GIMG_Diagnostics * diagnostics, const GIMG_Allocator * alloc,
    const GIMG_Limits * limits, unsigned char ** out_data, size_t * out_size) {
  if (out_data) {
    *out_data = NULL;
    *out_size = 0;
  }
  unsigned char * data = NULL;
  size_t size = 0;
  size_t capacity = 0;

  for (;;) {
    unsigned char len_byte = 0;
    GIMG_Result r = gimg_stream_read_exact(stream, &len_byte, 1u);
    if (r != GIMG_OK) {
      gif_load_diag(diagnostics, gimg_stream_tell(stream),
          "truncated in a sub-block chain");
      gimg_free(alloc, data);
      return r;
    }
    if (len_byte == 0u) {
      break;
    }
    if (!out_data) {
      size_t skipped = 0;
      r = gimg_stream_skip(stream, (size_t)len_byte, &skipped);
      if (r != GIMG_OK || skipped != (size_t)len_byte) {
        gif_load_diag(diagnostics, gimg_stream_tell(stream),
            "truncated inside a skipped sub-block");
        return r == GIMG_OK ? GIMG_ERR_CORRUPT : r;
      }
      continue;
    }

    size_t needed = 0;
    if (!gcu_safe_add_size(size, (size_t)len_byte, &needed)) {
      gimg_free(alloc, data);
      return GIMG_ERR_LIMIT;
    }
    // A chain has no declared total, so the caller's chunk limit is what
    // bounds it: without one, a file can name an unbounded amount of data in
    // 255-byte pieces and be believed one piece at a time.
    if (limits && limits->max_chunk_size && needed > limits->max_chunk_size) {
      gif_load_diag(diagnostics, gimg_stream_tell(stream),
          "sub-block chain exceeds max_chunk_size");
      gimg_free(alloc, data);
      return GIMG_ERR_LIMIT;
    }
    if (needed > capacity) {
      size_t grown = capacity ? capacity : 1024u;
      while (grown < needed) {
        size_t doubled = 0;
        if (!gcu_safe_mul_size(grown, 2u, &doubled)) {
          gimg_free(alloc, data);
          return GIMG_ERR_LIMIT;
        }
        grown = doubled;
      }
      unsigned char * bigger =
          (unsigned char *)gimg_realloc(alloc, data, grown);
      if (!bigger) {
        gimg_free(alloc, data);
        return GIMG_ERR_OOM;
      }
      data = bigger;
      capacity = grown;
    }
    r = gimg_stream_read_exact(stream, data + size, (size_t)len_byte);
    if (r != GIMG_OK) {
      gif_load_diag(diagnostics, gimg_stream_tell(stream),
          "truncated inside a sub-block");
      gimg_free(alloc, data);
      return r;
    }
    size = needed;
  }

  if (out_data) {
    *out_data = data;
    *out_size = size;
  }
  return GIMG_OK;
}

// ---------------------------------------------------------------------------
// Colour tables
// ---------------------------------------------------------------------------

static GIMG_Result gif_read_palette(GIMG_Stream * stream,
    GIMG_Diagnostics * diagnostics, uint16_t entries, gimg_gif_rgb_t * out) {
  unsigned char raw[GIMG_GIF_MAX_PALETTE * 3u];
  size_t bytes = (size_t)entries * 3u;
  GIMG_Result r = gimg_stream_read_exact(stream, raw, bytes);
  if (r != GIMG_OK) {
    gif_load_diag(
        diagnostics, gimg_stream_tell(stream), "truncated colour table");
    return r;
  }
  for (uint16_t i = 0; i < entries; i++) {
    out[i].r = raw[i * 3u + 0u];
    out[i].g = raw[i * 3u + 1u];
    out[i].b = raw[i * 3u + 2u];
  }
  return GIMG_OK;
}

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------

/** Control block state waiting to attach to the next image (89a 23). */
typedef struct {
  bool present;
  uint8_t disposal;
  bool has_transparency;
  uint8_t transparent_index;
  uint16_t delay_cs;
} gif_pending_control_t;

static GIMG_Result gif_append_frame(const GIMG_Allocator * alloc,
    gimg_gif_doc_state_t * state, gimg_gif_frame_t ** out_frame) {
  size_t next = 0;
  if (!gcu_safe_add_size(state->frame_count, 1u, &next)) {
    return GIMG_ERR_LIMIT;
  }
  size_t bytes = 0;
  if (!gcu_safe_mul_size(next, sizeof(gimg_gif_frame_t), &bytes)) {
    return GIMG_ERR_LIMIT;
  }
  gimg_gif_frame_t * grown =
      (gimg_gif_frame_t *)gimg_realloc(alloc, state->frames, bytes);
  if (!grown) {
    return GIMG_ERR_OOM;
  }
  state->frames = grown;
  memset(&state->frames[state->frame_count], 0, sizeof(gimg_gif_frame_t));
  *out_frame = &state->frames[state->frame_count];
  state->frame_count = next;
  return GIMG_OK;
}

/**
 * Read one Image Descriptor, its Local Color Table if it has one, and its LZW
 * code stream (89a 20, 22).
 */
static GIMG_Result gif_read_image(GIMG_Stream * stream,
    GIMG_Diagnostics * diagnostics, const GIMG_Allocator * alloc,
    const GIMG_Limits * limits, gimg_gif_doc_state_t * state,
    const gif_pending_control_t * control) {
  unsigned char desc[GIMG_GIF_IMAGE_DESCRIPTOR_LEN];
  GIMG_Result r = gimg_stream_read_exact(stream, desc, sizeof(desc));
  if (r != GIMG_OK) {
    gif_load_diag(
        diagnostics, gimg_stream_tell(stream), "truncated image descriptor");
    return r;
  }

  gimg_gif_frame_t * frame = NULL;
  r = gif_append_frame(alloc, state, &frame);
  if (r != GIMG_OK) {
    return r;
  }

  frame->left = gif_read_u16(desc + 0);
  frame->top = gif_read_u16(desc + 2);
  frame->width = gif_read_u16(desc + 4);
  frame->height = gif_read_u16(desc + 6);
  uint8_t packed = desc[8];
  frame->interlaced = (packed & 0x40u) != 0u;
  frame->has_local_palette = (packed & 0x80u) != 0u;

  if (control && control->present) {
    frame->has_control = true;
    frame->disposal = control->disposal;
    frame->has_transparency = control->has_transparency;
    frame->transparent_index = control->transparent_index;
    frame->delay_cs = control->delay_cs;
  }

  // An image of no area carries no pixels and cannot be composited anywhere.
  // The specification does not forbid it, but nothing can be done with one,
  // and admitting it would put a zero-byte raster into the document.
  if (frame->width == 0u || frame->height == 0u) {
    gif_load_diag(
        diagnostics, gimg_stream_tell(stream), "image has zero width or height");
    return GIMG_ERR_CORRUPT;
  }

  size_t pixels = 0;
  if (!gcu_safe_mul_size((size_t)frame->width, (size_t)frame->height,
          &pixels)) {
    return GIMG_ERR_LIMIT;
  }
  if (limits && limits->max_decoded_pixels &&
      pixels > limits->max_decoded_pixels) {
    gif_load_diag(diagnostics, gimg_stream_tell(stream),
        "increase max_decoded_pixels");
    return GIMG_ERR_LIMIT;
  }

  if (frame->has_local_palette) {
    frame->palette_count = gif_palette_entries(packed);
    r = gif_read_palette(
        stream, diagnostics, frame->palette_count, frame->palette);
    if (r != GIMG_OK) {
      return r;
    }
  }
  else if (state->has_global_palette) {
    // Resolved here rather than at decode so that a frame carries the table it
    // is read through, whichever block that table arrived in.
    frame->palette_count = state->global_palette_count;
    memcpy(frame->palette, state->global_palette,
        (size_t)frame->palette_count * sizeof(gimg_gif_rgb_t));
  }

  r = gimg_stream_read_exact(stream, &frame->lzw_min_code_size, 1u);
  if (r != GIMG_OK) {
    gif_load_diag(diagnostics, gimg_stream_tell(stream),
        "truncated before the LZW code size");
    return r;
  }
  // Two is the smallest the format allows even for a two-colour image (89a
  // 22), and the width has to leave room to grow towards twelve.
  if (frame->lzw_min_code_size < 2u ||
      frame->lzw_min_code_size > GIMG_GIF_MAX_LZW_MIN_CODE_SIZE) {
    gif_load_diag(diagnostics, gimg_stream_tell(stream),
        "LZW minimum code size out of range");
    return GIMG_ERR_CORRUPT;
  }

  return gif_read_sub_blocks(stream, diagnostics, alloc, limits, &frame->lzw,
      &frame->lzw_size);
}

/**
 * Read a Graphic Control Extension into `control` (89a 23).
 *
 * The block declares its own length, which every writer sets to 4.  A file
 * that says something else is not silently accepted: the fields are at fixed
 * offsets and a different length means the block is not the one named.
 */
static GIMG_Result gif_read_graphic_control(GIMG_Stream * stream,
    GIMG_Diagnostics * diagnostics, gif_pending_control_t * control) {
  unsigned char len = 0;
  GIMG_Result r = gimg_stream_read_exact(stream, &len, 1u);
  if (r != GIMG_OK) {
    return r;
  }
  if (len != 4u) {
    gif_load_diag(diagnostics, gimg_stream_tell(stream),
        "graphic control extension is not four bytes");
    return GIMG_ERR_CORRUPT;
  }
  unsigned char body[4];
  r = gimg_stream_read_exact(stream, body, sizeof(body));
  if (r != GIMG_OK) {
    gif_load_diag(diagnostics, gimg_stream_tell(stream),
        "truncated graphic control extension");
    return r;
  }
  unsigned char terminator = 0;
  r = gimg_stream_read_exact(stream, &terminator, 1u);
  if (r != GIMG_OK) {
    return r;
  }
  if (terminator != 0u) {
    gif_load_diag(diagnostics, gimg_stream_tell(stream),
        "graphic control extension is not terminated");
    return GIMG_ERR_CORRUPT;
  }

  control->present = true;
  control->disposal = (uint8_t)((body[0] >> 2) & 0x07u);
  control->has_transparency = (body[0] & 0x01u) != 0u;
  control->delay_cs = gif_read_u16(body + 1);
  control->transparent_index = body[3];
  return GIMG_OK;
}

/**
 * Read an Application Extension (89a 26), keeping only the NETSCAPE2.0 loop
 * count that everything uses to make a GIF repeat.
 */
static GIMG_Result gif_read_application(GIMG_Stream * stream,
    GIMG_Diagnostics * diagnostics, const GIMG_Allocator * alloc,
    const GIMG_Limits * limits, gimg_gif_doc_state_t * state) {
  unsigned char len = 0;
  GIMG_Result r = gimg_stream_read_exact(stream, &len, 1u);
  if (r != GIMG_OK) {
    return r;
  }
  // The identifier block is eleven bytes: eight of name and three of code.
  // Anything else is an extension this codec does not know, and its body is a
  // sub-block chain like any other, so skipping is well defined.
  unsigned char ident[11];
  bool netscape = false;
  if (len == sizeof(ident)) {
    r = gimg_stream_read_exact(stream, ident, sizeof(ident));
    if (r != GIMG_OK) {
      gif_load_diag(diagnostics, gimg_stream_tell(stream),
          "truncated application identifier");
      return r;
    }
    netscape = memcmp(ident, "NETSCAPE2.0", sizeof(ident)) == 0 ||
        memcmp(ident, "ANIMEXTS1.0", sizeof(ident)) == 0;
  }
  else {
    size_t skipped = 0;
    r = gimg_stream_skip(stream, (size_t)len, &skipped);
    if (r != GIMG_OK || skipped != (size_t)len) {
      gif_load_diag(diagnostics, gimg_stream_tell(stream),
          "truncated application identifier");
      return r == GIMG_OK ? GIMG_ERR_CORRUPT : r;
    }
  }

  unsigned char * body = NULL;
  size_t body_size = 0;
  r = gif_read_sub_blocks(
      stream, diagnostics, alloc, limits, &body, &body_size);
  if (r != GIMG_OK) {
    return r;
  }
  // Sub-block 1, byte 0 is the sub-block identifier; the loop count follows.
  if (netscape && body_size >= 3u && body[0] == 0x01u) {
    state->has_loop = true;
    state->loop_count = gif_read_u16(body + 1);
  }
  gimg_free(alloc, body);
  return GIMG_OK;
}

/**
 * Append one Comment Extension's bytes to the run kept for the document.
 *
 * Framed as a four-byte big-endian length so that several comments survive in
 * one block and the writer can put each back as its own extension.  See
 * GIMG_GIF_RAW_COMMENT.
 *
 * Takes ownership of neither argument; the caller frees `text`.
 */
static GIMG_Result gif_append_comment(gimg_gif_doc_state_t * state,
    const GIMG_Allocator * alloc, const unsigned char * text, size_t len) {
  size_t framed = 0;
  size_t need = 0;
  if (!gcu_safe_add_size(len, GIMG_GIF_RAW_COMMENT_PREFIX, &framed) ||
      !gcu_safe_add_size(state->comments_size, framed, &need)) {
    return GIMG_ERR_LIMIT;
  }
  // A length that does not fit the prefix cannot be written back, so it is not
  // taken in.  max_chunk_size already bounds a comment far below this; the
  // check is here so the framing cannot be violated by a limit set higher.
  if (len > 0xFFFFFFFFu) {
    return GIMG_ERR_LIMIT;
  }
  unsigned char * grown =
      (unsigned char *)gimg_realloc(alloc, state->comments, need);
  if (!grown) {
    return GIMG_ERR_OOM;
  }
  state->comments = grown;
  unsigned char * at = state->comments + state->comments_size;
  at[0] = (unsigned char)(len >> 24);
  at[1] = (unsigned char)((len >> 16) & 0xFFu);
  at[2] = (unsigned char)((len >> 8) & 0xFFu);
  at[3] = (unsigned char)(len & 0xFFu);
  if (len > 0u && text) {
    memcpy(at + GIMG_GIF_RAW_COMMENT_PREFIX, text, len);
  }
  state->comments_size = need;
  return GIMG_OK;
}

/**
 * Whether a comment's bytes are text a caller can be handed as a C string.
 *
 * The same test the JPEG codec applies to a COM segment: printable ASCII plus
 * tab, newline and carriage return, stopping at the first NUL.  89a 24 calls
 * the field 7-bit ASCII, so anything else is a writer doing something the
 * specification did not describe, and it stays in the raw block rather than
 * being presented as a description.
 */
static bool gif_comment_looks_like_text(
    const unsigned char * text, size_t len, size_t * out_len) {
  size_t used = len;
  for (size_t i = 0; i < len; i++) {
    if (text[i] == 0) {
      used = i;
      break;
    }
  }
  for (size_t i = 0; i < used; i++) {
    const unsigned char c = text[i];
    if (c != 0x09u && c != 0x0Au && c != 0x0Du && (c < 0x20u || c > 0x7Eu)) {
      return false;
    }
  }
  *out_len = used;
  return true;
}

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------

void gimg_gif_free_doc_state(GIMG_Codec * codec, void * codec_private) {
  (void)codec;
  gimg_gif_doc_state_t * state = (gimg_gif_doc_state_t *)codec_private;
  if (!state) {
    return;
  }
  const GIMG_Allocator * alloc = state->allocator;
  for (size_t i = 0; i < state->frame_count; i++) {
    gimg_free(alloc, state->frames[i].lzw);
  }
  gimg_free(alloc, state->frames);
  gimg_free(alloc, state->comments);
  gimg_free(alloc, state->cache_canvas);
  if (state->cache_lock_ready) {
    GCU_MUTEX_DESTROY(state->cache_lock);
  }
  gimg_free(alloc, state);
}

GIMG_Result gimg_gif_load(GIMG_Codec * codec, GIMG_Stream * stream,
    const GIMG_Load_Options * options, GIMG_Diagnostics * diagnostics,
    GIMG_Doc ** out_doc) {
  if (!codec || !stream || !out_doc) {
    return GIMG_ERR_INTERNAL;
  }
  *out_doc = NULL;

  const GIMG_Allocator * alloc = gimg_alloc_or_default(codec->allocator);
  const GIMG_Limits * limits = options ? options->limits : NULL;

  GIMG_Result r = gimg_gif_verify_signature(stream);
  if (r != GIMG_OK) {
    return r;
  }

  gimg_gif_doc_state_t * state = (gimg_gif_doc_state_t *)gimg_calloc(
      alloc, 1u, sizeof(gimg_gif_doc_state_t));
  if (!state) {
    return GIMG_ERR_OOM;
  }
  state->allocator = alloc;
  // The lock is created here, before any path that can free the state, so
  // that the teardown has one rule rather than one per exit.  A failure to
  // create it is not a failure to load: the flag stays false and decode does
  // without the cache, which costs time and nothing else.
  state->cache_lock_ready = (GCU_MUTEX_CREATE(state->cache_lock) == 0);

  // The three version bytes are read to consume them and to refuse a header
  // that stops inside them; which three they are is not kept.  A file saying
  // "89a" may use nothing 89a added and one saying "87a" is read the same way,
  // so there is no decision the spelling could inform - see
  // documentation/formats/gif.md.
  unsigned char version[GIMG_GIF_HEADER_LEN - GIMG_GIF_SIGNATURE_LEN];
  r = gimg_stream_read_exact(stream, version, sizeof(version));
  if (r != GIMG_OK) {
    gif_load_diag(diagnostics, GIMG_GIF_SIGNATURE_LEN, "truncated header");
    gimg_gif_free_doc_state(codec, state);
    return r;
  }

  unsigned char lsd[GIMG_GIF_LSD_LEN];
  r = gimg_stream_read_exact(stream, lsd, sizeof(lsd));
  if (r != GIMG_OK) {
    gif_load_diag(diagnostics, GIMG_GIF_HEADER_LEN,
        "truncated logical screen descriptor");
    gimg_gif_free_doc_state(codec, state);
    return r;
  }
  state->canvas_width = gif_read_u16(lsd + 0);
  state->canvas_height = gif_read_u16(lsd + 2);
  uint8_t screen_packed = lsd[4];
  state->background_index = lsd[5];
  state->aspect_ratio = lsd[6];
  state->has_global_palette = (screen_packed & 0x80u) != 0u;

  if (state->has_global_palette) {
    state->global_palette_count = gif_palette_entries(screen_packed);
    r = gif_read_palette(stream, diagnostics, state->global_palette_count,
        state->global_palette);
    if (r != GIMG_OK) {
      gimg_gif_free_doc_state(codec, state);
      return r;
    }
  }

  gif_pending_control_t control;
  memset(&control, 0, sizeof(control));

  bool saw_trailer = false;
  while (!saw_trailer) {
    unsigned char introducer = 0;
    r = gimg_stream_read_exact(stream, &introducer, 1u);
    if (r != GIMG_OK) {
      // A stream that simply stops is common enough in the wild that the
      // frames already read are worth more than the failure; the loop ends
      // here and the check below decides whether anything was salvaged.
      break;
    }

    if (introducer == GIMG_GIF_BLOCK_TRAILER) {
      saw_trailer = true;
      break;
    }
    if (introducer == GIMG_GIF_BLOCK_IMAGE) {
      if (limits && limits->max_frame_count &&
          state->frame_count >= limits->max_frame_count) {
        gif_load_diag(diagnostics, gimg_stream_tell(stream),
            "increase max_frame_count");
        gimg_gif_free_doc_state(codec, state);
        return GIMG_ERR_LIMIT;
      }
      r = gif_read_image(
          stream, diagnostics, alloc, limits, state, &control);
      if (r != GIMG_OK) {
        gimg_gif_free_doc_state(codec, state);
        return r;
      }
      // A control block governs the one image that follows it and no more
      // (89a 23), so it is spent here rather than left standing.
      memset(&control, 0, sizeof(control));
      continue;
    }
    if (introducer == GIMG_GIF_BLOCK_EXTENSION) {
      unsigned char label = 0;
      r = gimg_stream_read_exact(stream, &label, 1u);
      if (r != GIMG_OK) {
        gimg_gif_free_doc_state(codec, state);
        return r;
      }
      if (label == GIMG_GIF_EXT_GRAPHIC_CONTROL) {
        r = gif_read_graphic_control(stream, diagnostics, &control);
      }
      else if (label == GIMG_GIF_EXT_APPLICATION) {
        r = gif_read_application(stream, diagnostics, alloc, limits, state);
      }
      else if (label == GIMG_GIF_EXT_COMMENT) {
        // Kept, not walked past: a comment is the only place a GIF has to put
        // text, and discarding it loses the one piece of metadata the format
        // carries.  All of them are kept, because a file may hold several and
        // only the first becomes the normalized description.
        unsigned char * text = NULL;
        size_t len = 0;
        r = gif_read_sub_blocks(
            stream, diagnostics, alloc, limits, &text, &len);
        if (r == GIMG_OK) {
          r = gif_append_comment(state, alloc, text, len);
        }
        gimg_free(alloc, text);
      }
      else {
        // Plain Text is rendered by no modern decoder at all, and a GIF that
        // uses it looks to every one of them the way it looks here.  Other
        // extension labels are not ours to interpret.
        r = gif_read_sub_blocks(
            stream, diagnostics, alloc, limits, NULL, NULL);
      }
      if (r != GIMG_OK) {
        gimg_gif_free_doc_state(codec, state);
        return r;
      }
      continue;
    }

    gif_load_diag(diagnostics, gimg_stream_tell(stream),
        "unrecognized block introducer");
    gimg_gif_free_doc_state(codec, state);
    return GIMG_ERR_CORRUPT;
  }

  if (state->frame_count == 0u) {
    gif_load_diag(
        diagnostics, gimg_stream_tell(stream), "no image blocks in the file");
    gimg_gif_free_doc_state(codec, state);
    return GIMG_ERR_CORRUPT;
  }

  // A canvas of nothing still has to hold the images that were read: writers
  // exist that leave the logical screen at zero and let the frames define it.
  for (size_t i = 0; i < state->frame_count; i++) {
    const gimg_gif_frame_t * f = &state->frames[i];
    uint32_t right = (uint32_t)f->left + (uint32_t)f->width;
    uint32_t bottom = (uint32_t)f->top + (uint32_t)f->height;
    if (right > state->canvas_width) {
      state->canvas_width = (uint16_t)(right > 0xFFFFu ? 0xFFFFu : right);
    }
    if (bottom > state->canvas_height) {
      state->canvas_height = (uint16_t)(bottom > 0xFFFFu ? 0xFFFFu : bottom);
    }
  }

  GIMG_Doc * doc = NULL;
  r = gimg_doc_create_with_allocator(alloc, &doc);
  if (r == GIMG_OK) {
    r = gimg_doc_set_item_count(doc, state->frame_count);
  }
  if (r != GIMG_OK) {
    if (doc) {
      gimg_doc_destroy(doc);
    }
    gimg_gif_free_doc_state(codec, state);
    return r;
  }

  for (size_t i = 0; i < state->frame_count; i++) {
    const gimg_gif_frame_t * f = &state->frames[i];
    GIMG_Item * item = gimg_doc_item(doc, i);
    if (!item) {
      continue;
    }
    // GIF counts delay in hundredths of a second (89a 23); the item model
    // carries a numerator over a denominator, so the denominator says so.
    gimg_item_set_role(item, GIMG_ITEM_FRAME, i);
    gimg_item_set_frame_delay(item, f->delay_cs, 100u);
    gimg_item_set_dispose_op(item,
        f->disposal == GIMG_GIF_DISPOSAL_BACKGROUND ? GIMG_DISPOSE_BACKGROUND
            : f->disposal == GIMG_GIF_DISPOSAL_PREVIOUS ? GIMG_DISPOSE_PREVIOUS
                                                        : GIMG_DISPOSE_NONE);
    // Every GIF pixel replaces what is under it; a transparent index leaves
    // the previous frame showing, which the compositor in decode does by not
    // writing that pixel rather than by blending it.
    gimg_item_set_blend_op(item, GIMG_BLEND_SOURCE);
  }

  // The background colour is resolved here rather than reported as the index
  // the file gave: once decode has turned palette indices into RGBA, the table
  // that index referred to is gone, so an index would be a number a caller
  // could not use.  Nothing is reported when there is no global table to
  // resolve against, or when the index names an entry it does not have; 89a 18
  // says the field is to be ignored in the first case, and the second is a
  // file naming an entry that does not exist.
  //
  // The alpha carries the other half of the answer.  A file whose first frame
  // marks the background entry transparent is not naming a colour to put
  // behind the image - it is naming the absence of one, which is how an
  // encoder says "nothing is behind this" in a format that has no way to leave
  // the field out.  That is reported as the entry's colour at alpha 0, so a
  // caller that composites over it gets the no-op the file asked for and a
  // caller that wants to know which entry was named can still see it.
  //
  // The rule is the *first* frame's transparent index rather than any frame's,
  // because that is the control block in force when the screen is first shown,
  // and because it is what ImageMagick does: a two-frame file whose background
  // entry is transparent in frame 0 reports srgba(r,g,b,0) there, and the same
  // file with the flag moved to frame 1 reports the opaque colour.
  if (state->has_global_palette &&
      state->background_index < state->global_palette_count) {
    const gimg_gif_rgb_t * bg = &state->global_palette[state->background_index];
    const bool masked = state->frame_count > 0u &&
        state->frames[0].has_control && state->frames[0].has_transparency &&
        state->frames[0].transparent_index == state->background_index;
    const uint8_t rgba[4] = {bg->r, bg->g, bg->b, masked ? 0x00u : 0xFFu};
    gimg_doc_set_background_color(doc, rgba);
  }

  // Pixel Aspect Ratio (89a 18): zero means the file says nothing, and any
  // other value N means the pixel is (N + 15) / 64 as wide as it is tall.
  if (state->aspect_ratio != 0u) {
    gimg_doc_set_pixel_aspect_ratio(
        doc, (uint32_t)state->aspect_ratio + 15u, 64u);
  }

  // Comments reach the caller two ways, because one of them is lossy on
  // purpose.  Every comment is kept verbatim in the raw block, in stream
  // order; the first one that is text is also normalized into the common
  // description, which is the field a caller reads without knowing what format
  // it loaded.  A caller that wants them all reads the raw block - which is
  // what the description's own documentation says to do.
  if (state->comments && state->comments_size > 0u) {
    GIMG_Meta_Raw * raw = NULL;
    if (gimg_doc_ensure_meta_raw(doc, &raw) == GIMG_OK && raw) {
      (void)gimg_meta_raw_attach(raw, "gif", GIMG_GIF_RAW_COMMENT,
          state->comments, state->comments_size);
    }
    size_t text_len = 0;
    const unsigned char * first =
        state->comments + GIMG_GIF_RAW_COMMENT_PREFIX;
    const size_t first_len = ((size_t)state->comments[0] << 24) |
        ((size_t)state->comments[1] << 16) |
        ((size_t)state->comments[2] << 8) | (size_t)state->comments[3];
    if (first_len > 0u &&
        gif_comment_looks_like_text(first, first_len, &text_len) &&
        text_len > 0u) {
      GIMG_Meta_Common * common = NULL;
      if (gimg_doc_ensure_meta_common(doc, &common) == GIMG_OK && common) {
        char * buf = (char *)gimg_malloc(alloc, text_len + 1u);
        if (buf) {
          memcpy(buf, first, text_len);
          buf[text_len] = '\0';
          (void)gimg_meta_common_set_description(common, buf);
          gimg_free(alloc, buf);
        }
      }
    }
  }

  // The NETSCAPE2.0 count reaches the caller here rather than staying in the
  // codec's private state.  A GIF that carries no such block declares nothing,
  // which is not the same as declaring zero: zero means forever.
  if (state->has_loop) {
    gimg_doc_set_loop_count(doc, state->loop_count);
  }

  doc->loaded_by_codec = codec;
  doc->codec_private = state;
  *out_doc = doc;
  return GIMG_OK;
}

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
 * The metadata-loss matrix: per writer and per metadata policy, which kinds
 * reach the file, and the warning for each kind that does not.
 *
 * Three questions decide whether a kind is reported:
 *
 *  1. Does the document hold it?  Worked out from the common metadata, the raw
 *     blocks (each filed under the format that loaded it), the items, and for
 *     a PNG-loaded document its chunk list.
 *  2. Did the writer write it?  `writes[][]` below records, for each writer
 *     and policy, the kinds it writes when the document brought them in that
 *     writer's own form.  Raw blocks are keyed by format id and every writer
 *     reads only its own, so a block that came from another format is never
 *     written - which is the cross-format loss this module exists to report.
 *  3. Did the policy ask for it to go?  `policy_removes()`.  A removal the
 *     caller asked for is not a loss.
 *
 * The table is what the writers do, not what they ought to do: where a writer
 * leaves out something its policy keeps (the JPEG writer's description under
 * GIMG_META_KEEP_COMMON_ONLY), that is reported as a loss, and the
 * test that saves one document through every writer under every policy is what
 * keeps this table honest.
 */

#include <ghoti.io/image/macros.h>

#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/raster.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../codec/codec_internal.h"
#include "../codec/gif/gif_internal.h"
#include "../codec/jpeg/jpeg_internal.h"
#include "../codec/png/png_internal.h"
#include "../codec/tiff/tiff_internal.h"
#include "../codec/webp/webp_internal.h"
#include "../container/doc_internal.h"
#include "meta_internal.h"
#include "meta_loss.h"

// ---------------------------------------------------------------------------
// Scope
// ---------------------------------------------------------------------------

static _Thread_local gimg_save_loss_t * gimg_loss_scope = NULL;

void gimg_save_loss_begin(gimg_save_loss_t * scope) {
  scope->noted = 0u;
  scope->outer = gimg_loss_scope;
  gimg_loss_scope = scope;
}

void gimg_save_loss_end(gimg_save_loss_t * scope) {
  gimg_loss_scope = scope->outer;
}

void gimg_save_loss_note(gimg_loss_kind_t kind) {
  if (gimg_loss_scope && (unsigned)kind < GIMG_LOSS_COUNT) {
    gimg_loss_scope->noted |= UINT32_C(1) << (unsigned)kind;
  }
}

// ---------------------------------------------------------------------------
// The matrix
// ---------------------------------------------------------------------------

#define K(kind) (UINT32_C(1) << (kind))

typedef enum {
  W_JPEG = 0,
  W_PNG,
  W_TIFF,
  W_WEBP,
  W_GIF,
  W_BMP,
  W_ICO,
  W_COUNT,
  W_NONE = W_COUNT
} writer_t;

/** The family bit a writer reads raw blocks of. */
#define F_JPEG UINT32_C(1)
#define F_PNG UINT32_C(2)
#define F_WEBP UINT32_C(4)
#define F_TIFF UINT32_C(8)
#define F_GIF UINT32_C(16)
/** A block no writer reads: another family's, or an id none of them knows. */
#define F_OTHER UINT32_C(32)

static const uint32_t writer_family[W_COUNT] = {
    F_JPEG, F_PNG, F_TIFF, F_WEBP, F_GIF, 0u, 0u};

static const char * const writer_codec_name[W_COUNT] = {
    "jpeg", "png", "tiff", "webp", "gif", "bmp", "ico"};

/** Kinds that come from raw blocks and are written only from the writer's own. */
#define RAW_KINDS \
  (K(GIMG_LOSS_EXIF) | K(GIMG_LOSS_XMP) | K(GIMG_LOSS_TEXT) | \
      K(GIMG_LOSS_NATIVE) | K(GIMG_LOSS_COMMENTS))

#define STRUCTURAL_KINDS \
  (K(GIMG_LOSS_LOOP_COUNT) | K(GIMG_LOSS_FRAME_DELAY) | K(GIMG_LOSS_HOTSPOT) | \
      K(GIMG_LOSS_ITEM_ROLES) | K(GIMG_LOSS_ITEMS))

/** Policy index, in GIMG_Meta_Policy order. */
#define P_ALL 0
#define P_DROP 1
#define P_GPS 2
#define P_NORM 3
#define P_RAW 4
#define P_COMMON 5
#define P_COUNT 6

/**
 * Kinds each writer writes under each policy, when the document brought them
 * in that writer's own form.  The ICC profile is not here: whether the document
 * holds one is visible only on a decoded raster, so the writers that decide it
 * report it through gimg_save_loss_note().
 */
static const uint32_t writes[W_COUNT][P_COUNT] = {
    // JPEG: everything but the profile and the XMP the GPS policy removes;
    // the resolution is the JFIF density and goes out whatever the policy says.
    // GIMG_META_KEEP_COMMON_ONLY writes no COM, so no description.
    [W_JPEG] = {
        [P_ALL] = K(GIMG_LOSS_EXIF) | K(GIMG_LOSS_XMP) |
            K(GIMG_LOSS_RESOLUTION) | K(GIMG_LOSS_DESCRIPTION) |
            K(GIMG_LOSS_COMMENTS) | K(GIMG_LOSS_NATIVE),
        [P_DROP] = K(GIMG_LOSS_RESOLUTION),
        [P_GPS] = K(GIMG_LOSS_EXIF) | K(GIMG_LOSS_RESOLUTION) |
            K(GIMG_LOSS_DESCRIPTION) | K(GIMG_LOSS_COMMENTS) |
            K(GIMG_LOSS_NATIVE),
        [P_NORM] = K(GIMG_LOSS_EXIF) | K(GIMG_LOSS_XMP) |
            K(GIMG_LOSS_RESOLUTION) | K(GIMG_LOSS_DESCRIPTION) |
            K(GIMG_LOSS_COMMENTS) | K(GIMG_LOSS_NATIVE),
        [P_RAW] = K(GIMG_LOSS_EXIF) | K(GIMG_LOSS_XMP) |
            K(GIMG_LOSS_RESOLUTION) | K(GIMG_LOSS_DESCRIPTION) |
            K(GIMG_LOSS_COMMENTS) | K(GIMG_LOSS_NATIVE),
        [P_COMMON] = K(GIMG_LOSS_RESOLUTION),
    },
    // PNG: ancillary chunks from the chunk list of a PNG-loaded document (and
    // eXIf from a raw block); the common policy writes pHYs and a colour chunk
    // and no text, so no description.
    [W_PNG] = {
        [P_ALL] = K(GIMG_LOSS_EXIF) | K(GIMG_LOSS_XMP) | K(GIMG_LOSS_TEXT) |
            K(GIMG_LOSS_NATIVE) | K(GIMG_LOSS_RESOLUTION) |
            K(GIMG_LOSS_DESCRIPTION),
        [P_DROP] = 0u,
        [P_GPS] = K(GIMG_LOSS_EXIF) | K(GIMG_LOSS_TEXT) | K(GIMG_LOSS_NATIVE) |
            K(GIMG_LOSS_RESOLUTION) | K(GIMG_LOSS_DESCRIPTION),
        [P_NORM] = K(GIMG_LOSS_EXIF) | K(GIMG_LOSS_XMP) | K(GIMG_LOSS_TEXT) |
            K(GIMG_LOSS_NATIVE) | K(GIMG_LOSS_RESOLUTION) |
            K(GIMG_LOSS_DESCRIPTION),
        [P_RAW] = K(GIMG_LOSS_NATIVE),
        [P_COMMON] = K(GIMG_LOSS_RESOLUTION),
    },
    // TIFF: the resolution is written under every policy; XMP and the text
    // tags from the writer's own raw blocks; no Exif directory.
    [W_TIFF] = {
        [P_ALL] = K(GIMG_LOSS_XMP) | K(GIMG_LOSS_TEXT) |
            K(GIMG_LOSS_RESOLUTION) | K(GIMG_LOSS_DESCRIPTION),
        [P_DROP] = K(GIMG_LOSS_RESOLUTION),
        [P_GPS] = K(GIMG_LOSS_TEXT) | K(GIMG_LOSS_RESOLUTION) |
            K(GIMG_LOSS_DESCRIPTION),
        [P_NORM] = K(GIMG_LOSS_XMP) | K(GIMG_LOSS_TEXT) |
            K(GIMG_LOSS_RESOLUTION) | K(GIMG_LOSS_DESCRIPTION),
        [P_RAW] = K(GIMG_LOSS_XMP) | K(GIMG_LOSS_TEXT) |
            K(GIMG_LOSS_RESOLUTION),
        [P_COMMON] = K(GIMG_LOSS_RESOLUTION) | K(GIMG_LOSS_DESCRIPTION),
    },
    // WebP: ICCP, EXIF and XMP chunks; no resolution, description or text.
    [W_WEBP] = {
        [P_ALL] = K(GIMG_LOSS_EXIF) | K(GIMG_LOSS_XMP),
        [P_DROP] = 0u,
        [P_GPS] = K(GIMG_LOSS_EXIF),
        [P_NORM] = K(GIMG_LOSS_EXIF) | K(GIMG_LOSS_XMP),
        [P_RAW] = K(GIMG_LOSS_EXIF) | K(GIMG_LOSS_XMP),
        [P_COMMON] = 0u,
    },
    // GIF: Comment Extensions, from the raw block or the description.
    [W_GIF] = {
        [P_ALL] = K(GIMG_LOSS_COMMENTS) | K(GIMG_LOSS_DESCRIPTION),
        [P_DROP] = 0u,
        [P_GPS] = K(GIMG_LOSS_COMMENTS) | K(GIMG_LOSS_DESCRIPTION),
        [P_NORM] = K(GIMG_LOSS_COMMENTS) | K(GIMG_LOSS_DESCRIPTION),
        [P_RAW] = K(GIMG_LOSS_COMMENTS),
        [P_COMMON] = K(GIMG_LOSS_DESCRIPTION),
    },
    // BMP: the resolution, where the policy writes common metadata.
    [W_BMP] = {
        [P_ALL] = K(GIMG_LOSS_RESOLUTION),
        [P_DROP] = 0u,
        [P_GPS] = K(GIMG_LOSS_RESOLUTION),
        [P_NORM] = K(GIMG_LOSS_RESOLUTION),
        [P_RAW] = 0u,
        [P_COMMON] = K(GIMG_LOSS_RESOLUTION),
    },
    // ICO: no metadata at all.
    [W_ICO] = {0u},
};

/** Whether the writer writes @p kind under any policy at all. */
static bool writer_ever_writes(writer_t w, gimg_loss_kind_t kind) {
  for (int p = 0; p < P_COUNT; p++) {
    if (writes[w][p] & K(kind)) {
      return true;
    }
  }
  return false;
}

static int policy_index(GIMG_Meta_Policy policy) {
  switch (policy) {
  case GIMG_META_DROP_ALL:
    return P_DROP;
  case GIMG_META_STRIP_GPS:
    return P_GPS;
  case GIMG_META_NORMALIZE_EXIF:
    return P_NORM;
  case GIMG_META_KEEP_RAW_ONLY:
    return P_RAW;
  case GIMG_META_KEEP_COMMON_ONLY:
    return P_COMMON;
  default:
    return P_ALL;
  }
}

/**
 * Whether the policy asks for @p kind to be left out, which makes its absence
 * the caller's doing and not a loss.
 *
 *  - DROP_ALL asks for all metadata to go.  Playback structure and items are
 *    not metadata and are not named by it.
 *  - KEEP_COMMON_ONLY keeps the common fields (resolution, description) and
 *    the colour, and removes the raw kinds.
 *  - KEEP_RAW_ONLY keeps raw blocks and removes the common fields and the
 *    colour read from the raster; the PNG page also lists Exif, XMP and text
 *    among what it omits.
 *  - STRIP_GPS removes the XMP packet, which can carry a location and is not
 *    parsed.
 */
static bool policy_removes(
    int p, writer_t w, gimg_loss_kind_t kind) {
  const uint32_t bit = K(kind);
  if (bit & STRUCTURAL_KINDS) {
    return false;
  }
  switch (p) {
  case P_DROP:
    return true;
  case P_COMMON:
    return (bit & RAW_KINDS) != 0u;
  case P_RAW: {
    uint32_t removed = K(GIMG_LOSS_RESOLUTION) | K(GIMG_LOSS_DESCRIPTION) |
        K(GIMG_LOSS_ICC);
    if (w == W_PNG) {
      removed |= K(GIMG_LOSS_EXIF) | K(GIMG_LOSS_XMP) | K(GIMG_LOSS_TEXT);
    }
    return (bit & removed) != 0u;
  }
  case P_GPS:
    return kind == GIMG_LOSS_XMP;
  default:
    return false;
  }
}

// ---------------------------------------------------------------------------
// Messages
//
// A diagnostic keeps the pointer it is given, so every message is a string
// literal.  `fmt_has_no` is for a format that has no place for the kind at all,
// `own_form` for a writer that carries the kind only when it arrived in that
// format's own form, `not_written` for a limit of the writer in a format that
// could hold it, and `not_under_common` for a policy that keeps the kind and a
// writer that leaves it out anyway.
// ---------------------------------------------------------------------------

#define NO_PLACE(L, N, F) L " dropped: the " F " format has no " N
#define NOT_WRITTEN(L, F) L " dropped: the " F " writer does not write it"
#define OWN_FORM(L, N, F) \
  L " dropped: the " F " writer carries only " N " that arrived in " F " form"
#define NOT_UNDER_COMMON(L, N, F) \
  L " dropped: the " F \
    " writer writes no " N " under GIMG_META_KEEP_COMMON_ONLY"

#define WEBP_PROFILE(F) \
  "ICC profile dropped: a WebP document's profile is not on its raster, " \
  "where the " F " writer reads it"

typedef struct {
  const char * lost;         ///< The usual reason.
  const char * under_common; ///< A kind the policy keeps and the writer leaves.
} message_t;

static const message_t messages[GIMG_LOSS_COUNT][W_COUNT] = {
    [GIMG_LOSS_EXIF] = {
        [W_JPEG] = {OWN_FORM("Exif", "Exif", "JPEG"), NULL},
        [W_PNG] = {OWN_FORM("Exif", "Exif", "PNG"), NULL},
        [W_TIFF] = {NOT_WRITTEN("Exif", "TIFF"), NULL},
        [W_WEBP] = {OWN_FORM("Exif", "Exif", "WebP"), NULL},
        [W_GIF] = {NO_PLACE("Exif", "Exif", "GIF"), NULL},
        [W_BMP] = {NO_PLACE("Exif", "Exif", "BMP"), NULL},
        [W_ICO] = {NO_PLACE("Exif", "Exif", "ICO"), NULL},
    },
    [GIMG_LOSS_XMP] = {
        [W_JPEG] = {OWN_FORM("XMP", "XMP", "JPEG"), NULL},
        [W_PNG] = {OWN_FORM("XMP", "XMP", "PNG"), NULL},
        [W_TIFF] = {OWN_FORM("XMP", "XMP", "TIFF"), NULL},
        [W_WEBP] = {OWN_FORM("XMP", "XMP", "WebP"), NULL},
        [W_GIF] = {NO_PLACE("XMP", "XMP", "GIF"), NULL},
        [W_BMP] = {NO_PLACE("XMP", "XMP", "BMP"), NULL},
        [W_ICO] = {NO_PLACE("XMP", "XMP", "ICO"), NULL},
    },
    [GIMG_LOSS_ICC] = {
        [W_JPEG] = {WEBP_PROFILE("JPEG"), NULL},
        [W_PNG] = {WEBP_PROFILE("PNG"), NULL},
        [W_TIFF] = {WEBP_PROFILE("TIFF"), NULL},
        [W_WEBP] = {NULL,
            NOT_UNDER_COMMON("ICC profile", "profile", "WebP")},
        [W_BMP] = {WEBP_PROFILE("BMP"), NULL},
        [W_GIF] = {NO_PLACE("ICC profile", "ICC profile", "GIF"), NULL},
        [W_ICO] = {NO_PLACE("ICC profile", "ICC profile", "ICO"), NULL},
    },
    [GIMG_LOSS_RESOLUTION] = {
        [W_WEBP] = {NO_PLACE("Resolution", "resolution", "WebP"), NULL},
        [W_GIF] = {NO_PLACE("Resolution", "resolution", "GIF"), NULL},
        [W_BMP] = {"Resolution dropped: a BMP that wraps a JPEG or PNG stream "
                   "has no resolution of its own",
            NULL},
        [W_ICO] = {NO_PLACE("Resolution", "resolution", "ICO"), NULL},
    },
    [GIMG_LOSS_DESCRIPTION] = {
        [W_JPEG] = {NULL,
            NOT_UNDER_COMMON("Description", "description", "JPEG")},
        [W_PNG] = {NULL,
            NOT_UNDER_COMMON("Description", "description", "PNG")},
        [W_WEBP] = {NO_PLACE("Description", "description", "WebP"), NULL},
        [W_BMP] = {NO_PLACE("Description", "description", "BMP"), NULL},
        [W_ICO] = {NO_PLACE("Description", "description", "ICO"), NULL},
    },
    [GIMG_LOSS_COMMENTS] = {
        [W_JPEG] = {OWN_FORM("Additional comments", "comments", "JPEG"), NULL},
        [W_PNG] = {NOT_WRITTEN("Additional comments", "PNG"), NULL},
        [W_TIFF] = {NOT_WRITTEN("Additional comments", "TIFF"), NULL},
        [W_WEBP] = {NO_PLACE("Additional comments", "comments", "WebP"), NULL},
        [W_GIF] = {OWN_FORM("Additional comments", "comments", "GIF"), NULL},
        [W_BMP] = {NO_PLACE("Additional comments", "comments", "BMP"), NULL},
        [W_ICO] = {NO_PLACE("Additional comments", "comments", "ICO"), NULL},
    },
    [GIMG_LOSS_TEXT] = {
        [W_JPEG] = {NOT_WRITTEN("Text fields", "JPEG"), NULL},
        [W_PNG] = {OWN_FORM("Text fields", "text fields", "PNG"), NULL},
        [W_TIFF] = {OWN_FORM("Text fields", "text fields", "TIFF"), NULL},
        [W_WEBP] = {NO_PLACE("Text fields", "text fields", "WebP"), NULL},
        [W_GIF] = {NOT_WRITTEN("Text fields", "GIF"), NULL},
        [W_BMP] = {NO_PLACE("Text fields", "text fields", "BMP"), NULL},
        [W_ICO] = {NO_PLACE("Text fields", "text fields", "ICO"), NULL},
    },
    [GIMG_LOSS_NATIVE] = {
        [W_JPEG] = {OWN_FORM("Format-specific metadata",
                        "format-specific metadata", "JPEG"),
            NULL},
        [W_PNG] = {OWN_FORM("Format-specific metadata",
                       "format-specific metadata", "PNG"),
            NULL},
        [W_TIFF] = {NOT_WRITTEN("Format-specific metadata", "TIFF"), NULL},
        [W_WEBP] = {NOT_WRITTEN("Format-specific metadata", "WebP"), NULL},
        [W_GIF] = {NOT_WRITTEN("Format-specific metadata", "GIF"), NULL},
        [W_BMP] = {NO_PLACE("Format-specific metadata",
                       "format-specific metadata", "BMP"),
            NULL},
        [W_ICO] = {NO_PLACE("Format-specific metadata",
                       "format-specific metadata", "ICO"),
            NULL},
    },
    [GIMG_LOSS_LOOP_COUNT] = {
        [W_JPEG] = {NO_PLACE("Loop count", "animation loop count", "JPEG"),
            NULL},
        [W_PNG] = {"Loop count dropped: a PNG of one frame is not an "
                   "animation and has no place for one",
            NULL},
        [W_TIFF] = {NO_PLACE("Loop count", "animation loop count", "TIFF"),
            NULL},
        [W_WEBP] = {"Loop count dropped: a WebP of one frame is not an "
                    "animation and has no place for one",
            NULL},
        [W_BMP] = {NO_PLACE("Loop count", "animation loop count", "BMP"),
            NULL},
        [W_ICO] = {NO_PLACE("Loop count", "animation loop count", "ICO"),
            NULL},
    },
    [GIMG_LOSS_FRAME_DELAY] = {
        [W_JPEG] = {NO_PLACE("Frame delays", "frame delays", "JPEG"), NULL},
        [W_PNG] = {"Frame delays dropped: a PNG of one frame is not an "
                   "animation and has no place for one",
            NULL},
        [W_TIFF] = {NO_PLACE("Frame delays", "frame delays", "TIFF"), NULL},
        [W_WEBP] = {"Frame delays dropped: a WebP of one frame is not an "
                    "animation and has no place for one",
            NULL},
        [W_BMP] = {NO_PLACE("Frame delays", "frame delays", "BMP"), NULL},
        [W_ICO] = {NO_PLACE("Frame delays", "frame delays", "ICO"), NULL},
    },
    [GIMG_LOSS_HOTSPOT] = {
        [W_JPEG] = {NO_PLACE("Cursor hotspot", "cursor hotspot", "JPEG"),
            NULL},
        [W_PNG] = {NO_PLACE("Cursor hotspot", "cursor hotspot", "PNG"), NULL},
        [W_TIFF] = {NO_PLACE("Cursor hotspot", "cursor hotspot", "TIFF"),
            NULL},
        [W_WEBP] = {NO_PLACE("Cursor hotspot", "cursor hotspot", "WebP"),
            NULL},
        [W_GIF] = {NO_PLACE("Cursor hotspot", "cursor hotspot", "GIF"), NULL},
        [W_BMP] = {NO_PLACE("Cursor hotspot", "cursor hotspot", "BMP"), NULL},
    },
    [GIMG_LOSS_ITEM_ROLES] = {
        [W_JPEG] = {"Item roles dropped: the JPEG writer stores the second "
                    "item as the Exif thumbnail whatever its role",
            NULL},
        [W_PNG] = {"Item roles dropped: the PNG writer writes every item as "
                   "an animation frame",
            NULL},
        [W_TIFF] = {"Item roles dropped: the TIFF writer keeps the level "
                    "role only; a thumbnail or alternate becomes a plain page",
            NULL},
        [W_WEBP] = {"Item roles dropped: the WebP writer writes every item "
                    "as an animation frame",
            NULL},
        [W_GIF] = {"Item roles dropped: the GIF writer writes every item as "
                   "an animation frame",
            NULL},
    },
    [GIMG_LOSS_ITEMS] = {
        [W_JPEG] = {"Items dropped: the JPEG writer writes the first item and "
                    "at most one Exif thumbnail",
            NULL},
        [W_BMP] = {"Items dropped: the BMP writer writes the first item only",
            NULL},
        [W_ICO] = {"Items dropped: the ICO writer writes only image and "
                   "alternate items",
            NULL},
    },
};

// ---------------------------------------------------------------------------
// What the document holds
// ---------------------------------------------------------------------------

typedef struct {
  uint32_t present;                 ///< Bit per kind.
  uint32_t family[GIMG_LOSS_COUNT]; ///< Families the raw kinds came from.
  /**
   * A WebP ICCP chunk the document holds and no raster carries.  The WebP
   * decoder leaves the profile in the raw block, and every writer but WebP's
   * reads the profile from the raster, so it does not cross.
   */
  bool webp_profile_off_raster;
} inventory_t;

static writer_t writer_of(const char * name) {
  if (!name) {
    return W_NONE;
  }
  for (int w = 0; w < W_COUNT; w++) {
    if (strcmp(name, writer_codec_name[w]) == 0) {
      return (writer_t)w;
    }
  }
  return W_NONE;
}

static void add_raw(inventory_t * inv, gimg_loss_kind_t kind, uint32_t family) {
  inv->present |= K(kind);
  inv->family[kind] |= family;
}

/** Count the framed comments in a raw block (`prefix` bytes of length each). */
static size_t count_framed(const gimg_meta_raw_block_t * b, size_t prefix) {
  size_t count = 0;
  size_t offset = 0;
  const unsigned char * p = (const unsigned char *)b->data;
  while (p && offset + prefix <= b->size) {
    size_t len = 0;
    for (size_t i = 0; i < prefix; i++) {
      len = (len << 8) | p[offset + i];
    }
    offset += prefix;
    if (len > b->size - offset) {
      break;
    }
    if (len > 0u) {
      count++;
    }
    offset += len;
  }
  return count;
}

static void classify_raw(
    inventory_t * inv, const gimg_meta_raw_block_t * b, bool has_description) {
  if (!b->format_id || b->size == 0u) {
    return;
  }
  const uint32_t id = b->tag_or_chunk_id;
  if (strcmp(b->format_id, "jpeg") == 0) {
    switch (id) {
    case GIMG_JPEG_RAW_APP0:
    case GIMG_JPEG_RAW_APP0_JFXX: // Density and thumbnail: both rebuilt.
    case GIMG_JPEG_RAW_APP14:     // The colour transform, not metadata.
    case GIMG_JPEG_RAW_APP2_ICC:  // The profile is judged on the raster.
    case GIMG_JPEG_RAW_APP2_ICC_CHUNKS:
      return;
    case GIMG_JPEG_RAW_APP1_EXIF:
      add_raw(inv, GIMG_LOSS_EXIF, F_JPEG);
      return;
    case GIMG_JPEG_RAW_APP1_XMP:
      add_raw(inv, GIMG_LOSS_XMP, F_JPEG);
      return;
    case GIMG_JPEG_RAW_COM:
      // The first comment is the description; only more than that, or a
      // comment with no description to carry it, is a kind of its own.
      if (count_framed(b, 2u) > 1u || !has_description) {
        add_raw(inv, GIMG_LOSS_COMMENTS, F_JPEG);
      }
      return;
    default:
      add_raw(inv, GIMG_LOSS_NATIVE, F_JPEG);
      return;
    }
  }
  if (strcmp(b->format_id, "png") == 0) {
    if (id == (uint32_t)GIMG_PNG_eXIf) {
      add_raw(inv, GIMG_LOSS_EXIF, F_PNG);
    }
    else {
      // No writer reads a PNG raw block other than eXIf; the chunk list is
      // what the PNG writer writes from.
      add_raw(inv, GIMG_LOSS_NATIVE, F_OTHER);
    }
    return;
  }
  if (strcmp(b->format_id, "webp") == 0) {
    if (id == GIMG_WEBP_EXIF) {
      add_raw(inv, GIMG_LOSS_EXIF, F_WEBP);
    }
    else if (id == GIMG_WEBP_XMP) {
      add_raw(inv, GIMG_LOSS_XMP, F_WEBP);
    }
    else if (id == GIMG_WEBP_ICCP) {
      inv->webp_profile_off_raster = true; // Settled in take_inventory().
    }
    else {
      add_raw(inv, GIMG_LOSS_NATIVE, F_OTHER);
    }
    return;
  }
  if (strcmp(b->format_id, "tiff") == 0) {
    if (id == GIMG_TIFF_TAG_XMP) {
      add_raw(inv, GIMG_LOSS_XMP, F_TIFF);
      return;
    }
    for (size_t i = 0; i < GIMG_TIFF_INFO_COUNT; i++) {
      if (id == gimg_tiff_info_tags[i]) {
        add_raw(inv, GIMG_LOSS_TEXT, F_TIFF);
        return;
      }
    }
    add_raw(inv, GIMG_LOSS_NATIVE, F_OTHER);
    return;
  }
  if (strcmp(b->format_id, "gif") == 0) {
    if (id == GIMG_GIF_RAW_COMMENT) {
      if (count_framed(b, GIMG_GIF_RAW_COMMENT_PREFIX) > 1u || !has_description) {
        add_raw(inv, GIMG_LOSS_COMMENTS, F_GIF);
      }
      return;
    }
    add_raw(inv, GIMG_LOSS_NATIVE, F_OTHER);
    return;
  }
  // Any other format id is a caller's own and no writer reads it.
  add_raw(inv, GIMG_LOSS_NATIVE, F_OTHER);
}

static bool is_role_of_interest(GIMG_Item_Role role) {
  return role == GIMG_ITEM_THUMBNAIL || role == GIMG_ITEM_LEVEL ||
      role == GIMG_ITEM_ALTERNATE;
}

static void take_inventory(const GIMG_Doc * doc, inventory_t * inv) {
  memset(inv, 0, sizeof(*inv));

  bool has_description = false;
  if (doc->meta_common) {
    const char * desc = gimg_meta_common_description(doc->meta_common);
    if (desc && desc[0] != '\0') {
      inv->present |= K(GIMG_LOSS_DESCRIPTION);
      has_description = true;
    }
    uint32_t x = 0, y = 0;
    gimg_meta_common_dpi(doc->meta_common, &x, &y);
    if (x > 0u && y > 0u) {
      inv->present |= K(GIMG_LOSS_RESOLUTION);
    }
  }
  if (doc->meta_raw) {
    for (size_t i = 0; i < doc->meta_raw->count; i++) {
      classify_raw(inv, &doc->meta_raw->blocks[i], has_description);
    }
  }
  if (inv->webp_profile_off_raster && doc->item_count > 0u && doc->items[0].raster) {
    const GCOL_Color_Info * ci =
        gimg_raster_color_info_const(doc->items[0].raster);
    if (ci && ci->icc_bytes && ci->icc_size > 0u) {
      inv->webp_profile_off_raster = false; // The caller put it there.
    }
  }
  // A PNG-loaded document keeps its text, XMP and private chunks in the chunk
  // list, not in raw blocks.
  const uint32_t png = gimg_png_doc_loss_kinds(doc);
  for (int k = 0; k < GIMG_LOSS_COUNT; k++) {
    if (png & K(k)) {
      add_raw(inv, (gimg_loss_kind_t)k, F_PNG);
    }
  }

  uint32_t loops = 0;
  if (gimg_doc_loop_count(doc, &loops)) {
    inv->present |= K(GIMG_LOSS_LOOP_COUNT);
  }
  for (size_t i = 0; i < doc->item_count; i++) {
    const GIMG_Item * item = &doc->items[i];
    uint16_t num = 0, den = 0;
    gimg_item_frame_delay(item, &num, &den);
    if (num != 0u) {
      inv->present |= K(GIMG_LOSS_FRAME_DELAY);
    }
    uint16_t hx = 0, hy = 0;
    gimg_item_hotspot(item, &hx, &hy);
    if (hx || hy) {
      inv->present |= K(GIMG_LOSS_HOTSPOT);
    }
  }
  if (gimg_ico_doc_is_cursor(doc)) {
    inv->present |= K(GIMG_LOSS_HOTSPOT);
  }
}

// ---------------------------------------------------------------------------
// Items
// ---------------------------------------------------------------------------

/** Whether the JPEG writer stores the second item as the Exif thumbnail. */
static bool jpeg_thumbnail_slot_applies(
    const GIMG_Doc * doc, const GIMG_Save_Options * options) {
  if (doc->loaded_by_codec != NULL &&
      (doc->loaded_by_codec->name == NULL ||
          strcmp(doc->loaded_by_codec->name, "jpeg") != 0)) {
    return false; // In another format's document it is frame two.
  }
  const unsigned fmt = options ? options->exif_thumbnail_format
                               : (unsigned)GIMG_EXIF_THUMB_FORMAT_DEFAULT;
  return fmt == GIMG_EXIF_THUMB_FORMAT_DEFAULT ||
      fmt == GIMG_EXIF_THUMB_FORMAT_UNCOMPRESSED ||
      fmt == GIMG_EXIF_THUMB_FORMAT_JPEG ||
      fmt == GIMG_EXIF_THUMB_FORMAT_TIFF_JPEG;
}

/**
 * The item kinds: items with no place in the file, and roles that become
 * something else.  Sets bits in @p out.
 */
static uint32_t item_losses(const GIMG_Doc * doc, writer_t w, int p,
    const GIMG_Save_Options * options) {
  const size_t n = doc->item_count;
  uint32_t out = 0u;
  switch (w) {
  case W_BMP:
    if (n > 1u) {
      out |= K(GIMG_LOSS_ITEMS);
    }
    break;
  case W_JPEG: {
    if (n < 2u) {
      break;
    }
    // Item 1 is the thumbnail in a JPEG's own document or one built by hand,
    // and is written only where the Exif is (the policies that drop APP1 drop
    // it, which is the caller's request).  Anything past it has no place.
    const bool slot = jpeg_thumbnail_slot_applies(doc, options);
    const bool slot_written = slot && p != P_DROP && p != P_COMMON;
    if (!slot) {
      out |= K(GIMG_LOSS_ITEMS);
    }
    else if (n > 2u) {
      out |= K(GIMG_LOSS_ITEMS);
    }
    if (slot_written &&
        is_role_of_interest(gimg_item_role(&doc->items[1])) &&
        gimg_item_role(&doc->items[1]) != GIMG_ITEM_THUMBNAIL) {
      out |= K(GIMG_LOSS_ITEM_ROLES);
    }
    break;
  }
  case W_ICO:
    for (size_t i = 0; i < n; i++) {
      const GIMG_Item_Role role = gimg_item_role(&doc->items[i]);
      if (role != GIMG_ITEM_IMAGE && role != GIMG_ITEM_ALTERNATE) {
        out |= K(GIMG_LOSS_ITEMS);
      }
    }
    break;
  case W_TIFF:
    for (size_t i = 1; i < n; i++) {
      const GIMG_Item_Role role = gimg_item_role(&doc->items[i]);
      if (is_role_of_interest(role) && role != GIMG_ITEM_LEVEL) {
        out |= K(GIMG_LOSS_ITEM_ROLES);
      }
    }
    break;
  case W_PNG:
  case W_WEBP:
  case W_GIF:
    for (size_t i = 1; i < n; i++) {
      if (is_role_of_interest(gimg_item_role(&doc->items[i]))) {
        out |= K(GIMG_LOSS_ITEM_ROLES);
      }
    }
    break;
  default:
    break;
  }
  return out;
}

/** Whether the writer carries the playback kind for this document. */
static bool writes_playback(writer_t w, gimg_loss_kind_t kind, size_t n) {
  switch (w) {
  case W_GIF:
    return true;
  case W_PNG:
  case W_WEBP:
    return n > 1u && (kind == GIMG_LOSS_LOOP_COUNT ||
                         kind == GIMG_LOSS_FRAME_DELAY);
  default:
    return false;
  }
}

// ---------------------------------------------------------------------------
// Report
// ---------------------------------------------------------------------------

static void append_warning(GIMG_Diagnostics * d, const char * codec_name,
    const char * text) {
  (void)gimg_diagnostics_append(d, codec_name, 0u, 0u, GIMG_DIAG_WARNING, text);
}

void gimg_save_loss_report(const gimg_save_loss_t * scope, const GIMG_Doc * doc,
    const char * codec_name, const GIMG_Save_Options * options,
    GIMG_Save_Report * report) {
  if (!doc || !report || !report->diagnostics) {
    return;
  }
  const writer_t w = writer_of(codec_name);
  if (w == W_NONE) {
    return;
  }
  const int p = policy_index(
      options ? options->metadata_policy : GIMG_META_PRESERVE_ALL);

  inventory_t inv;
  take_inventory(doc, &inv);

  uint32_t lost = 0u;
  uint32_t under_common = 0u; // Of those, the ones the policy keeps.

  for (int k = 0; k < GIMG_LOSS_COUNT; k++) {
    const gimg_loss_kind_t kind = (gimg_loss_kind_t)k;
    const uint32_t bit = K(kind);
    if (!(inv.present & bit) || (bit & STRUCTURAL_KINDS) ||
        kind == GIMG_LOSS_ICC) {
      continue;
    }
    bool is_lost = false;
    if (bit & RAW_KINDS) {
      const uint32_t own = inv.family[kind] & writer_family[w];
      const uint32_t foreign = inv.family[kind] & ~writer_family[w];
      is_lost = foreign != 0u || (own != 0u && !(writes[w][p] & bit));
      if (is_lost && foreign == 0u && writer_ever_writes(w, kind)) {
        under_common |= bit;
      }
    }
    else {
      uint32_t mask = writes[w][p];
      if (w == W_BMP && options && options->bmp_wrapper != GIMG_BMP_WRAPPER_NONE) {
        mask = 0u; // The wrapped stream is saved from a bare raster.
      }
      is_lost = !(mask & bit);
      if (is_lost && writer_ever_writes(w, kind)) {
        under_common |= bit;
      }
    }
    if (is_lost && !policy_removes(p, w, kind)) {
      lost |= bit;
    }
  }

  // What the writer reported from the raster.
  const uint32_t noted = scope ? scope->noted : 0u;
  if ((noted & K(GIMG_LOSS_ICC)) &&
      !policy_removes(p, w, GIMG_LOSS_ICC)) {
    lost |= K(GIMG_LOSS_ICC);
    if (p == P_COMMON) {
      under_common |= K(GIMG_LOSS_ICC);
    }
  }

  // A profile that arrived in a WebP chunk and is not on the raster reaches
  // only the WebP writer, which reads the chunk - and not under the policy that
  // keeps colour but drops the chunks.
  if (inv.webp_profile_off_raster &&
      (w != W_WEBP || p == P_COMMON) && !policy_removes(p, w, GIMG_LOSS_ICC)) {
    lost |= K(GIMG_LOSS_ICC);
    if (w == W_WEBP) {
      under_common |= K(GIMG_LOSS_ICC);
    }
  }

  // Playback and items.
  const size_t n = doc->item_count;
  for (int k = GIMG_LOSS_LOOP_COUNT; k <= GIMG_LOSS_FRAME_DELAY; k++) {
    const gimg_loss_kind_t kind = (gimg_loss_kind_t)k;
    if ((inv.present & K(kind)) && !writes_playback(w, kind, n)) {
      lost |= K(kind);
    }
  }
  if ((inv.present & K(GIMG_LOSS_HOTSPOT)) && w != W_ICO) {
    lost |= K(GIMG_LOSS_HOTSPOT);
  }
  lost |= item_losses(doc, w, p, options);

  for (int k = 0; k < GIMG_LOSS_COUNT; k++) {
    if (!(lost & K(k))) {
      continue;
    }
    const message_t * m = &messages[k][w];
    const char * text =
        (under_common & K(k)) && m->under_common ? m->under_common : m->lost;
    if (!text) {
      text = m->under_common;
    }
    if (text) {
      append_warning(report->diagnostics, writer_codec_name[w], text);
    }
  }
}

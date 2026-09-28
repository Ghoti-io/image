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
 * The one place that decides whether a metadata object is too big (internal).
 *
 * Every codec retains things that are not pixels - an ICC profile, an Exif
 * blob, XMP, a description, a comment - and each one is allocated at a size
 * the file declares. Before this existed the four codecs gave three different
 * answers to the same question: PNG, JPEG and GIF bounded them with
 * GIMG_Limits.max_chunk_size, BMP with max_memory, and TIFF with a private
 * four-megabyte constant that no caller could reach, while TIFF's
 * ImageDescription had no bound at all. max_metadata_size named the question
 * and nothing read it.
 *
 * One predicate, so the answer cannot drift between codecs again. The
 * format-level caps stay where they are: max_chunk_size bounds a segment
 * because the format is built out of segments, and this bounds what is kept
 * out of one.
 */

#ifndef GHOTI_IO_GIMG_LIMITS_INTERNAL_H
#define GHOTI_IO_GIMG_LIMITS_INTERNAL_H

#include <ghoti.io/image/macros.h>

#include <ghoti.io/image/stream.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The cap applied when the caller states none.
 *
 * Four mebibytes, which is what the TIFF codec already enforced privately on
 * an ICC profile and is far past any real one: the largest profile in the
 * corpus is 6,668 bytes, and a CMYK output profile with full A2B tables is
 * still under a megabyte. It is a bomb guard, not a policy.
 */
#define GIMG_METADATA_SIZE_DEFAULT (4u * 1024u * 1024u)

/**
 * @brief What to do about a metadata object of a declared size.
 *
 * The two over-size cases are not the same event and the BMP loader was
 * already telling them apart, which is the distinction kept here: a size past
 * the built-in guard means the file is describing something other than its own
 * colour, and the picture is handed back without it because the picture is not
 * wrong; a size past a cap the *caller* set means they asked to be told, and
 * they are told.
 */
typedef enum {
  GIMG_METADATA_KEEP = 0,    ///< Within every cap; retain it.
  GIMG_METADATA_IMPLAUSIBLE, ///< Past the built-in guard; drop, do not refuse.
  GIMG_METADATA_REFUSED      ///< Past the caller's cap; GIMG_ERR_LIMIT.
} gimg_metadata_verdict_t;

/**
 * @brief Whether a metadata object of @p bytes may be retained.
 *
 * @param limits The caller's limits, or NULL for the defaults.
 * @param bytes The size the file declared.
 * @return Which of the three cases this is.
 */
static inline gimg_metadata_verdict_t gimg_metadata_verdict(
    const GIMG_Limits * limits, size_t bytes) {
  if (limits && limits->max_metadata_size != 0u) {
    return (bytes > limits->max_metadata_size) ? GIMG_METADATA_REFUSED
                                               : GIMG_METADATA_KEEP;
  }
  return (bytes > GIMG_METADATA_SIZE_DEFAULT) ? GIMG_METADATA_IMPLAUSIBLE
                                              : GIMG_METADATA_KEEP;
}

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GIMG_LIMITS_INTERNAL_H

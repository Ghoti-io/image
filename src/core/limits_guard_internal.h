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
 * The scope a call opens when its caller hands it a GIMG_Limits.
 *
 * gimg_doc_load(), gimg_item_decode() and gimg_doc_save() open the cumulative
 * max_memory scope of budget.c themselves.  An operation or a raster
 * constructor that the caller calls directly has no such entry point, so the
 * variants that take limits open the same scope here and hold the call to it.
 * A call given no limits opens nothing: inside a load, a decode or a save it is
 * counted by that call's scope, and anywhere else it is uncapped, as it always
 * was.
 *
 * What the limits mean here, field by field:
 *
 *  - max_memory: the scope.  The most bytes this call may hold live in blocks
 *    it takes, the result raster included; the source, which the caller owns,
 *    is not counted.  Nested in another scope the call is held to both caps.
 *  - max_decoded_pixels: the most pixels a raster this call makes may have.
 *  - the other three fields concern files and are not read.
 *
 * A zero field is the library default for it and GIMG_LIMIT_NONE lifts it, as
 * it does for a load (see gimg_limits_resolve()).
 */

#ifndef GHOTI_IO_GIMG_SRC_CORE_LIMITS_GUARD_INTERNAL_H
#define GHOTI_IO_GIMG_SRC_CORE_LIMITS_GUARD_INTERNAL_H

#include <ghoti.io/image/macros.h>

#include <ghoti.io/image/core.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <stdbool.h>
#include <stdint.h>

#include "budget_internal.h"

typedef struct {
  bool active;            ///< false: no limits were given; nothing is held.
  GIMG_Limits limits;     ///< The caller's limits, defaults filled in.
  gimg_budget_scope_t scope;
} gimg_limits_guard_t;

/** Open the guard for @p limits (NULL opens nothing). */
static inline void gimg_limits_guard_open(
    gimg_limits_guard_t * g, const GIMG_Limits * limits) {
  g->active = limits != NULL;
  if (g->active) {
    gimg_limits_resolve(limits, &g->limits);
    gimg_budget_enter(&g->scope, g->limits.max_memory);
  }
}

/** Whether a raster of @p width by @p height pixels is within the guard. */
static inline bool gimg_limits_guard_pixels_ok(
    const gimg_limits_guard_t * g, uint32_t width, uint32_t height) {
  return !g->active ||
      (uint64_t)width * (uint64_t)height <=
      (uint64_t)g->limits.max_decoded_pixels;
}

/**
 * Close the guard.  Returns whether an allocation was refused while it was
 * open (always false for a guard that held nothing).  For a call that makes
 * something other than a raster, and so has to undo it itself.
 */
static inline bool gimg_limits_guard_leave(gimg_limits_guard_t * g) {
  return g->active && gimg_budget_leave(&g->scope);
}

/**
 * Close the guard and settle the call's result.  A refusal of any allocation
 * while it was open is GIMG_ERR_LIMIT, whatever the code made of it, and a
 * raster the call made in spite of one is destroyed and @p out_raster (which
 * may be NULL) set to NULL, so a refused call leaves nothing behind.
 */
static inline GIMG_Result gimg_limits_guard_close(gimg_limits_guard_t * g,
    GIMG_Result r, GIMG_Raster ** out_raster) {
  const bool refused = gimg_limits_guard_leave(g);
  if (refused && r != GIMG_ERR_LIMIT) {
    if (out_raster) {
      gimg_raster_destroy(*out_raster);
      *out_raster = NULL;
    }
    return GIMG_ERR_LIMIT;
  }
  return r;
}

#endif // GHOTI_IO_GIMG_SRC_CORE_LIMITS_GUARD_INTERNAL_H

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

#include <ghoti.io/image/macros.h>

#include <stdint.h>
#include <string.h>

#include "budget_internal.h"

// Zero means no scope is open: allocations outside a load or a decode - a
// caller's own rasters, the operations, a save - are not capped here.
static _Thread_local size_t gimg_budget_cap = 0;
static _Thread_local bool gimg_budget_hit = false;

void gimg_budget_enter(gimg_budget_scope_t * scope, size_t cap) {
  scope->prev_cap = gimg_budget_cap;
  scope->prev_hit = gimg_budget_hit;
  if (cap == 0) {
    cap = GIMG_LIMIT_NONE;
  }
  if (gimg_budget_cap != 0 && gimg_budget_cap < cap) {
    cap = gimg_budget_cap;
  }
  gimg_budget_cap = cap;
  gimg_budget_hit = false;
}

bool gimg_budget_leave(gimg_budget_scope_t * scope) {
  const bool hit = gimg_budget_hit;
  gimg_budget_cap = scope->prev_cap;
  gimg_budget_hit = scope->prev_hit || hit;
  return hit;
}

bool gimg_budget_allows(size_t size) {
  if (gimg_budget_cap == 0 || size <= gimg_budget_cap) {
    return true;
  }
  gimg_budget_hit = true;
  return false;
}

void gimg_limits_resolve(const GIMG_Limits * in, GIMG_Limits * out) {
  if (in) {
    *out = *in;
  }
  else {
    memset(out, 0, sizeof(*out));
  }
  if (out->max_decoded_pixels == 0) {
    out->max_decoded_pixels = GIMG_DEFAULT_MAX_DECODED_PIXELS;
  }
  if (out->max_memory == 0) {
    out->max_memory = GIMG_DEFAULT_MAX_MEMORY;
  }
}

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

#include <stdlib.h>

// The scope the load or decode on this thread is running in.
//
// depth is how many scopes are open.  cap is the most live bytes the
// innermost may hold, as an absolute figure on the running total, so that a
// scope nested in another is held to both.  live is the sum of the blocks
// taken in scope and not yet released, and table is how it knows each
// block's size at the moment it is released.  Nothing here is shared between
// threads: a scope belongs to the thread that opened it.
//
// A block that outlives the scope - the raster a decode returns - stays in
// the table until the outermost scope closes, which empties it; a free of a
// block the table does not hold (one taken before the scope, or by another
// thread) subtracts nothing, so a later free cannot take the total below
// what was added.
typedef struct {
  void * key;
  size_t size;
} gimg_budget_slot_t;

static _Thread_local size_t gimg_budget_depth = 0;
static _Thread_local size_t gimg_budget_cap = GIMG_LIMIT_NONE;
static _Thread_local bool gimg_budget_hit = false;
static _Thread_local size_t gimg_budget_live = 0;
static _Thread_local gimg_budget_slot_t * gimg_budget_table = NULL;
static _Thread_local size_t gimg_budget_slots = 0;  // a power of two, or 0
static _Thread_local size_t gimg_budget_count = 0;

static size_t slot_of(const void * key, size_t slots) {
  uintptr_t h = (uintptr_t)key;
  h ^= h >> 17;
  h *= (uintptr_t)0x9E3779B97F4A7C15ull;
  h ^= h >> 29;
  return (size_t)h & (slots - 1u);
}

// The slot holding key, or the empty slot where it would go.
static size_t find_slot(const void * key) {
  size_t i = slot_of(key, gimg_budget_slots);
  while (gimg_budget_table[i].key && gimg_budget_table[i].key != key) {
    i = (i + 1u) & (gimg_budget_slots - 1u);
  }
  return i;
}

static bool grow_table(void) {
  const size_t slots = gimg_budget_slots ? gimg_budget_slots * 2u : 64u;
  gimg_budget_slot_t * fresh = calloc(slots, sizeof(*fresh));
  if (!fresh) {
    return false;
  }
  gimg_budget_slot_t * old = gimg_budget_table;
  const size_t old_slots = gimg_budget_slots;
  gimg_budget_table = fresh;
  gimg_budget_slots = slots;
  for (size_t i = 0; i < old_slots; i++) {
    if (old[i].key) {
      gimg_budget_table[find_slot(old[i].key)] = old[i];
    }
  }
  free(old);
  return true;
}

static void erase_slot(size_t i) {
  const size_t mask = gimg_budget_slots - 1u;
  gimg_budget_table[i].key = NULL;
  gimg_budget_count--;
  // Close the gap so that lookups need no tombstones.
  size_t j = i;
  for (;;) {
    j = (j + 1u) & mask;
    if (!gimg_budget_table[j].key) {
      return;
    }
    const size_t home = slot_of(gimg_budget_table[j].key, gimg_budget_slots);
    if (((j - home) & mask) >= ((j - i) & mask)) {
      gimg_budget_table[i] = gimg_budget_table[j];
      gimg_budget_table[j].key = NULL;
      i = j;
    }
  }
}

static bool tracking(void) {
  return gimg_budget_depth != 0 && gimg_budget_cap != GIMG_LIMIT_NONE;
}

static size_t held(const void * ptr) {
  if (!ptr || gimg_budget_count == 0) {
    return 0;
  }
  const size_t i = find_slot(ptr);
  return gimg_budget_table[i].key ? gimg_budget_table[i].size : 0u;
}

static void record(void * ptr, size_t size) {
  const size_t i = find_slot(ptr);
  if (gimg_budget_table[i].key) {
    // A stale entry at a reused address.
    gimg_budget_live -= gimg_budget_table[i].size;
    gimg_budget_table[i].size = size;
  }
  else {
    gimg_budget_table[i].key = ptr;
    gimg_budget_table[i].size = size;
    gimg_budget_count++;
  }
  gimg_budget_live += size;
}

static void forget(void * ptr) {
  if (!ptr || gimg_budget_count == 0) {
    return;
  }
  const size_t i = find_slot(ptr);
  if (gimg_budget_table[i].key) {
    gimg_budget_live -= gimg_budget_table[i].size;
    erase_slot(i);
  }
}

void gimg_budget_enter(gimg_budget_scope_t * scope, size_t cap) {
  scope->prev_cap = gimg_budget_cap;
  scope->prev_hit = gimg_budget_hit;
  if (cap == 0) {
    cap = GIMG_LIMIT_NONE;
  }
  // The new scope may hold `cap` bytes more than the live total at entry,
  // and never more than the scope around it allows.
  size_t absolute = GIMG_LIMIT_NONE;
  if (cap != GIMG_LIMIT_NONE) {
    absolute = cap > GIMG_LIMIT_NONE - gimg_budget_live
        ? GIMG_LIMIT_NONE
        : gimg_budget_live + cap;
  }
  if (gimg_budget_cap < absolute) {
    absolute = gimg_budget_cap;
  }
  gimg_budget_cap = absolute;
  gimg_budget_hit = false;
  gimg_budget_depth++;
}

bool gimg_budget_leave(gimg_budget_scope_t * scope) {
  const bool hit = gimg_budget_hit;
  gimg_budget_cap = scope->prev_cap;
  gimg_budget_hit = scope->prev_hit || hit;
  if (--gimg_budget_depth == 0) {
    free(gimg_budget_table);
    gimg_budget_table = NULL;
    gimg_budget_slots = 0;
    gimg_budget_count = 0;
    gimg_budget_live = 0;
  }
  return hit;
}

bool gimg_budget_allows(size_t size) {
  if (!tracking()) {
    return true;
  }
  if (size > gimg_budget_cap - (gimg_budget_live < gimg_budget_cap
          ? gimg_budget_live : gimg_budget_cap)) {
    gimg_budget_hit = true;
    return false;
  }
  // Room for the block's entry, so that recording it cannot fail once the
  // allocator has handed it over.
  if ((gimg_budget_count + 1u) * 2u > gimg_budget_slots && !grow_table()) {
    return false;
  }
  return true;
}

bool gimg_budget_allows_realloc(const void * old, size_t size) {
  if (!tracking()) {
    return true;
  }
  const size_t before = held(old);
  if (size > before) {
    const size_t room = gimg_budget_cap -
        (gimg_budget_live < gimg_budget_cap ? gimg_budget_live
                                            : gimg_budget_cap);
    if (size - before > room) {
      gimg_budget_hit = true;
      return false;
    }
  }
  if ((gimg_budget_count + 1u) * 2u > gimg_budget_slots && !grow_table()) {
    return false;
  }
  return true;
}

void gimg_budget_took(void * ptr, size_t size) {
  if (ptr && tracking()) {
    record(ptr, size);
  }
}

void gimg_budget_resized(void * old, void * ptr, size_t size) {
  if (!ptr) {
    return;
  }
  forget(old);
  if (tracking()) {
    record(ptr, size);
  }
}

void gimg_budget_released(void * ptr) {
  forget(ptr);
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

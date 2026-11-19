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
 * The allocation cap of the load or decode running on this thread.
 *
 * GIMG_Limits.max_memory was read by one codec out of seven, because each
 * codec had to remember to compare its own sizes with it.  It is enforced
 * here instead, underneath all of them: gimg_doc_load() and gimg_item_decode()
 * open a scope carrying the cap, and every allocation the library makes on
 * that thread until the scope closes is held to it.  A codec does not opt in
 * and cannot forget.
 *
 * What it bounds is the sum: the bytes the library holds at once, in blocks
 * it took inside the scope and has not yet released.  Blocks carry no size,
 * so the scope keeps a table of the live ones, keyed by address, filled by
 * gimg_malloc/calloc/realloc and drained by gimg_free.  Nothing is added to
 * the blocks themselves, so a block that leaves the scope - the raster a
 * decode returns - is the caller's, is freed through the same allocator
 * without the scope's knowing, and cannot take the total below zero: a free
 * of an address the table does not hold subtracts nothing.  A scope whose cap
 * is GIMG_LIMIT_NONE keeps no table and costs nothing.
 *
 * A request over the cap fails as an allocation failure does, so every codec
 * unwinds through arms the allocation-failure sweep already runs.  The scope
 * remembers that it happened, and the entry point reports GIMG_ERR_LIMIT in
 * place of whatever the codec made of the NULL.
 */

#ifndef GHOTI_IO_GIMG_SRC_CORE_BUDGET_INTERNAL_H
#define GHOTI_IO_GIMG_SRC_CORE_BUDGET_INTERNAL_H

#include <ghoti.io/image/macros.h>

#include <ghoti.io/image/stream.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** What a scope puts back when it closes. */
typedef struct {
  size_t prev_cap;
  bool prev_hit;
} gimg_budget_scope_t;

/**
 * Open a scope with @p cap as the most live bytes the library may hold in
 * blocks taken from here on.  Scopes nest - a thumbnail is loaded inside a
 * load - and an inner one can tighten the cap but never loosen it.
 */
void gimg_budget_enter(gimg_budget_scope_t * scope, size_t cap);

/**
 * Close the scope.  Returns whether an allocation was refused inside it; a
 * refusal also counts against the scope around it.
 */
bool gimg_budget_leave(gimg_budget_scope_t * scope);

/**
 * Whether a new block of @p size may be taken without the live total passing
 * the cap; records a refusal.  Also makes room for the block's entry, so
 * that gimg_budget_took() cannot fail.  A false return without a recorded
 * refusal is that room not being had, which the caller treats as the
 * allocator failing.
 */
bool gimg_budget_allows(size_t size);

/** As gimg_budget_allows(), for @p old resized to @p size (old may be NULL). */
bool gimg_budget_allows_realloc(const void * old, size_t size);

/** A new block of @p size was taken at @p ptr. */
void gimg_budget_took(void * ptr, size_t size);

/** @p old was resized to @p size and now lives at @p ptr (not NULL). */
void gimg_budget_resized(void * old, void * ptr, size_t size);

/** @p ptr is being released. */
void gimg_budget_released(void * ptr);

/**
 * Resolve a caller's limits into the ones a codec is handed: never NULL, and
 * with the library default in place of each zero that has one.
 * max_decoded_pixels and max_memory are resolved.  The other three keep
 * their zero, which each codec reads as its own built-in guard.
 */
void gimg_limits_resolve(const GIMG_Limits * in, GIMG_Limits * out);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GIMG_SRC_CORE_BUDGET_INTERNAL_H

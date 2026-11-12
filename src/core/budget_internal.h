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
 * What it bounds is one allocation, not their sum.  Blocks carry no size, so
 * a running total could only be kept by wrapping every block the library
 * hands out, including pixel buffers whose ownership passes to the caller.
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
 * Open a scope with @p cap as the largest single allocation.  Scopes nest -
 * a thumbnail is loaded inside a load - and an inner one can tighten the cap
 * but never loosen it.
 */
void gimg_budget_enter(gimg_budget_scope_t * scope, size_t cap);

/**
 * Close the scope.  Returns whether an allocation was refused inside it; a
 * refusal also counts against the scope around it.
 */
bool gimg_budget_leave(gimg_budget_scope_t * scope);

/** Whether an allocation of @p size may go ahead; records a refusal. */
bool gimg_budget_allows(size_t size);

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

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
 * The library's default allocator, which is cutil's.
 *
 * The stdlib-backed implementation this file used to carry was the same one
 * cutil has, so it forwards rather than repeating it. One behavior came along
 * with the move and is worth noting: this allocator has always guaranteed a
 * non-NULL return for a zero-size request, because NULL has to mean failure
 * and nothing else. It used to satisfy that with a plain malloc, so a
 * zero-item calloc returned uninitialized memory; cutil's returns a zeroed
 * byte instead.
 */

#include <ghoti.io/cutil/allocator.h>
#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/allocator.h>

GIMG_API const GIMG_Allocator * gimg_allocator_default(void) {
  return gcu_allocator_default();
}

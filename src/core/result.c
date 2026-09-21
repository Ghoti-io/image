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
 * Result code and error string implementation.
 */

#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/core.h>

static const char * const gimg_result_strings[] = {
    "ok",
    "io error",
    "format error",
    "unsupported",
    "limit exceeded",
    "corrupt data",
    "out of memory",
    "internal error",
};

GIMG_API const char * gimg_result_string(GIMG_Result result) {
  if ((unsigned int)result >= (unsigned int)GIMG_RESULT_COUNT) {
    return "unknown";
  }
  return gimg_result_strings[result];
}

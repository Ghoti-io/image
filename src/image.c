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
 * Main implementation for the Ghoti.io Image library.
 *
 * This file contains version information accessors and any shared
 * utilities used across the library modules.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/image.h>

GIMG_API uint32_t gimg_version_major(void) {
  return GIMG_VERSION_MAJOR;
}

GIMG_API uint32_t gimg_version_minor(void) {
  return GIMG_VERSION_MINOR;
}

GIMG_API uint32_t gimg_version_patch(void) {
  return GIMG_VERSION_PATCH;
}

GIMG_API const char * gimg_version_string(void) {
  static char version_string[32];
  static bool initialized = false;

  if (!initialized) {
    snprintf(version_string, sizeof(version_string), "%u.%u.%u",
        GIMG_VERSION_MAJOR, GIMG_VERSION_MINOR, GIMG_VERSION_PATCH);
    initialized = true;
  }

  return version_string;
}

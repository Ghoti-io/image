/**
 * @file
 *
 * Main implementation for the Ghoti.io Image library.
 *
 * This file contains version information accessors and any shared
 * utilities used across the library modules.
 *
 * Copyright 2026 by Corey Pennycuff
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

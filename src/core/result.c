/**
 * @file
 *
 * Result code and error string implementation.
 *
 * Copyright 2026 by Corey Pennycuff
 */

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

GIMG_API const char * gimg_result_string(GIMG_RESULT result) {
  if ((unsigned int)result >= (unsigned int)GIMG_RESULT_COUNT) {
    return "unknown";
  }
  return gimg_result_strings[result];
}

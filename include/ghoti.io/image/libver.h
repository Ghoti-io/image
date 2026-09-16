/**
 * @file libver.h
 *
 * Version numbering and the symbol namespace for the Ghoti.io Image library.
 *
 * Every exported symbol carries a per-version token so that two versions of
 * this library can be loaded into one process without the dynamic linker
 * binding one caller to the other version's implementation.
 *
 * See CONVENTIONS.md section 4.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GIMG_LIBVER_H
#define GHOTI_IO_GIMG_LIBVER_H

/**
 * GHOTIIO_IMAGE_NAME and GHOTIIO_IMAGE_VERSION come from here.  They are generated at
 * build time from the Makefile's BRANCH, so that the token inside every
 * exported symbol is the same one that names the .pc file, the install
 * directory and the shared library.
 */
#include <ghoti.io/image/libver_gen.h>

/**
 * Produce the namespaced form of an identifier.
 *
 * @param NAME The identifier to prefix with GHOTIIO_IMAGE_NAME.
 */
#define GHOTIIO_IMAGE(NAME) GHOTIIO_IMAGE_RENAME(GHOTIIO_IMAGE_NAME, _##NAME)

/** Helper.  Concatenation needs two levels of expansion. */
#define GHOTIIO_IMAGE_RENAME_INNER(a, b) a##b

/** Helper.  Concatenation needs two levels of expansion. */
#define GHOTIIO_IMAGE_RENAME(a, b) GHOTIIO_IMAGE_RENAME_INNER(a, b)

#endif // GHOTI_IO_GIMG_LIBVER_H

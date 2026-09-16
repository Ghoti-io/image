/**
 * @file
 *
 * Main header for the Ghoti.io Image library.
 *
 * Raster image decoding/encoding, metadata, transformations, color management,
 * and container semantics for multi-image formats.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GIMG_IMAGE_H
#define GHOTI_IO_GIMG_IMAGE_H

#include <ghoti.io/image/allocator.h>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/color.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/ops.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <stdint.h>

/*
 * Library version information comes from libver.h, which takes it from the
 * generated libver_gen.h: GIMG_VERSION_MAJOR / _MINOR / _PATCH, plus
 * GIMG_VERSION_STRING and the packed GIMG_VERSION_NUMBER. It used to be
 * written out here as three zeros that no build step ever updated.
 */

/**
 * @brief Get the major version number
 * @return The major version number
 */
GIMG_API uint32_t gimg_version_major(void);

/**
 * @brief Get the minor version number
 * @return The minor version number
 */
GIMG_API uint32_t gimg_version_minor(void);

/**
 * @brief Get the patch version number
 * @return The patch version number
 */
GIMG_API uint32_t gimg_version_patch(void);

/**
 * @brief Get the version string
 * @return A string representation of the version (e.g., "0.0.0")
 */
GIMG_API const char * gimg_version_string(void);

#endif // GHOTI_IO_GIMG_IMAGE_H

/**
 * @file
 *
 * Document and item types for multi-image containers.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_IMAGE_DOC_H
#define GHOTI_IO_IMAGE_DOC_H

#include <ghoti.io/image/allocator.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/macros.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opaque document (container with one or more items). */
typedef struct GIMG_Doc GIMG_Doc;
/** @brief Opaque image item (page/frame/level/thumbnail). */
typedef struct GIMG_Item GIMG_Item;

/**
 * @brief Number of items in the document (always >= 1).
 */
GIMG_API size_t gimg_doc_item_count(const GIMG_Doc * doc);

/**
 * @brief Get item by index (0 = primary).
 * @param doc Document.
 * @param index Zero-based index.
 * @return Item pointer or NULL if index out of range.
 */
GIMG_API GIMG_Item * gimg_doc_item(const GIMG_Doc * doc, size_t index);

/**
 * @brief Create a minimal document with one item (uses default allocator).
 */
GIMG_API GIMG_Result gimg_doc_create(GIMG_Doc ** out_doc);

/**
 * @brief Create a minimal document with one item using a specific allocator.
 * @param allocator Allocator for doc and items (NULL = default).
 */
GIMG_API GIMG_Result gimg_doc_create_with_allocator(
    const GIMG_Allocator * allocator, GIMG_Doc ** out_doc);

/**
 * @brief Destroy document and its items.
 * @param doc Document to destroy (no-op if NULL).
 */
GIMG_API void gimg_doc_destroy(GIMG_Doc * doc);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_IMAGE_DOC_H

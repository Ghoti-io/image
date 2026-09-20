/**
 * @file
 *
 * JPEG color: what a decoded raster is told about the meaning of its samples.
 *
 * Copyright 2026 by Corey Pennycuff
 *
 * --- Internal algorithms and design ---
 *
 * Two things a JPEG can say end up on GIMG_Color_Info: the ICC profile its
 * APP2 segments carry, and - for a four-component frame - the polarity of its
 * ink amounts.  Both are properties of the file, not of the coding process
 * that produced it, but each decode path used to attach them for itself and
 * no two of them agreed: the baseline path set both, the hierarchical path
 * set only the polarity, and the extended and progressive-extended paths set
 * only the profile.  So the same CMYK image came back saying 0 is full ink
 * when it was baseline and saying nothing at all when it was progressive or
 * twelve-bit, which is a difference a consumer would render as an inverted
 * picture.
 *
 * The polarity is GIMG_CMYK_POLARITY_INK for every four-component frame this
 * decoder emits.  That is the Adobe convention the format is written in and
 * what libjpeg assumes (jdapimin.c); a file that meant the other one has no
 * way to say so, so this is a statement about JPEG rather than a guess about
 * the image.
 */

#include <ghoti.io/image/color.h>
#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/raster.h>

#include "jpeg_internal.h"

void gimg_jpeg_attach_color(const gimg_jpeg_doc_state_t * state,
    uint8_t num_comp, GIMG_Raster * raster) {
  if (!raster) {
    return;
  }
  bool have_icc = state && state->app2_icc && state->app2_icc_len > 0u;
  if (num_comp != 4u && !have_icc) {
    return;
  }

  GIMG_Color_Info color_info;
  gimg_color_info_default(&color_info);
  if (num_comp == 4u) {
    color_info.cmyk_polarity = GIMG_CMYK_POLARITY_INK;
  }
  if (have_icc) {
    // A profile assembled from several APP2 segments is stored bare; one that
    // arrived in a single segment is stored with that segment's 14-byte
    // ICC_PROFILE header still in front of it.
    if (state->app2_icc_num_chunks > 0) {
      color_info.icc_bytes = state->app2_icc;
      color_info.icc_size = state->app2_icc_len;
    }
    else {
      color_info.icc_bytes = state->app2_icc + 14;
      color_info.icc_size = state->app2_icc_len - 14u;
    }
  }
  (void)gimg_raster_set_color_info(raster, &color_info);
}

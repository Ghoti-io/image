/*
 * Reference coefficient dumper: decode a JPEG with libjpeg-turbo
 * (jpeg_read_coefficients), compute FNV-1a hash over the first MCU's
 * coefficient blocks (same layout and formula as our decoder's
 * DUMP_JPEG_COEF_AFTER_SCAN), and print the hash.
 *
 * Used only for debugging our progressive decoder. The image library
 * does not link to libjpeg-turbo. Build with:
 *   cc -o dump_jpeg_coef_ref dump_jpeg_coef_ref.c $(pkg-config --cflags --libs libjpeg)
 *
 * Usage: dump_jpeg_coef_ref <file.jpg>
 * Output: COEF_HASH 0x<hex>
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>

#define FNV_OFFSET 0xcbf29ce484222325ULL
#define FNV_PRIME  0x100000001b3ULL

static void fnv1a_update(uint64_t *h, const unsigned char *p, size_t n)
{
  for (size_t i = 0; i < n; i++) {
    *h ^= (uint64_t)p[i];
    *h *= FNV_PRIME;
  }
}

int main(int argc, char **argv)
{
  if (argc != 2) {
    fprintf(stderr, "Usage: %s <file.jpg>\n", argv[0]);
    return 1;
  }

  const char *path = argv[1];
  FILE *fp = fopen(path, "rb");
  if (!fp) {
    perror(path);
    return 1;
  }

  struct jpeg_decompress_struct cinfo;
  struct jpeg_error_mgr jerr;
  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_decompress(&cinfo);
  jpeg_stdio_src(&cinfo, fp);
  jpeg_read_header(&cinfo, TRUE);

  if (!jpeg_has_multiple_scans(&cinfo)) {
    fprintf(stderr, "%s: not a progressive/multi-scan JPEG\n", path);
    jpeg_destroy_decompress(&cinfo);
    fclose(fp);
    return 1;
  }

  jvirt_barray_ptr *coef_arrays = jpeg_read_coefficients(&cinfo);
  if (!coef_arrays) {
    fprintf(stderr, "%s: jpeg_read_coefficients failed\n", path);
    jpeg_destroy_decompress(&cinfo);
    fclose(fp);
    return 1;
  }

  uint64_t fnv = FNV_OFFSET;
  int num_comp = cinfo.num_components;

  for (int c = 0; c < num_comp; c++) {
    jpeg_component_info *comp = &cinfo.comp_info[c];
    int h_samp = comp->h_samp_factor;
    int v_samp = comp->v_samp_factor;

    // First MCU: rows 0..v_samp-1, blocks 0..h_samp-1 per row (same order as our decoder)
    JBLOCKARRAY rows = (*cinfo.mem->access_virt_barray)(
        (j_common_ptr)&cinfo, coef_arrays[c], 0, (JDIMENSION)v_samp, TRUE);

    for (int by = 0; by < v_samp; by++) {
      JBLOCKROW row = rows[by];
      for (int bx = 0; bx < h_samp; bx++) {
        const unsigned char *p = (const unsigned char *)&row[bx];
        size_t n = (size_t)64 * sizeof(JCOEF);
        fnv1a_update(&fnv, p, n);
      }
    }
  }

  printf("COEF_HASH 0x%llx\n", (unsigned long long)fnv);

  if (getenv("DUMP_JPEG_VERSION")) {
    printf("JPEG_LIB_VERSION %d\n", JPEG_LIB_VERSION);
  }

  // Optional: dump first MCU DCs (Y0..Y3 Cb0 Cr0 for 4:2:0) to compare with DUMP_JPEG_DC scan0.
  if (getenv("DUMP_FIRST_MCU_DC")) {
    for (int c = 0; c < num_comp; c++) {
      jpeg_component_info *comp = &cinfo.comp_info[c];
      int h_samp = comp->h_samp_factor;
      int v_samp = comp->v_samp_factor;
      JBLOCKARRAY rows = (*cinfo.mem->access_virt_barray)(
          (j_common_ptr)&cinfo, coef_arrays[c], 0, (JDIMENSION)v_samp, TRUE);
      printf("COMP%d_DC", c);
      for (int by = 0; by < v_samp; by++) {
        JBLOCKROW row = rows[by];
        for (int bx = 0; bx < h_samp; bx++) {
          const JCOEF *block = (const JCOEF *)&row[bx];
          printf(" %d", (int)block[0]);
        }
      }
      printf("\n");
    }
  }

  // Optional: dump first block for 8x8 single-component (compare to DUMP_JPEG_COEF_BLOCK).
  if (getenv("DUMP_FIRST_BLOCK") && num_comp == 1 &&
      cinfo.comp_info[0].width_in_blocks == 1 &&
      cinfo.comp_info[0].height_in_blocks == 1) {
    JBLOCKARRAY rows = (*cinfo.mem->access_virt_barray)(
        (j_common_ptr)&cinfo, coef_arrays[0], 0, 1, TRUE);
    JBLOCKROW row = rows[0];
    const JCOEF *block = (const JCOEF *)&row[0];
    printf("FIRST_BLOCK");
    for (int i = 0; i < 64; i++) {
      printf(" %d", (int)block[i]);
    }
    printf("\n");
  }

  /* Optional: dump first MCU all coefficients (natural order per block) for
   * diff against our decoder (DUMP_JPEG_COEF_FIRST_MCU=1). Format: one line
   * per block "COMP c BLOCK b 0..63: v0 v1 ...". */
  if (getenv("DUMP_FIRST_MCU")) {
    for (int c = 0; c < num_comp; c++) {
      jpeg_component_info *comp = &cinfo.comp_info[c];
      int h_samp = comp->h_samp_factor;
      int v_samp = comp->v_samp_factor;
      JBLOCKARRAY rows = (*cinfo.mem->access_virt_barray)(
          (j_common_ptr)&cinfo, coef_arrays[c], 0, (JDIMENSION)v_samp, TRUE);
      int blk = 0;
      for (int by = 0; by < v_samp; by++) {
        JBLOCKROW row = rows[by];
        for (int bx = 0; bx < h_samp; bx++) {
          const JCOEF *block = (const JCOEF *)&row[bx];
          printf("REF_COMP%d_BLOCK%d", c, blk);
          for (int i = 0; i < 64; i++) {
            printf(" %d", (int)block[i]);
          }
          printf("\n");
          blk++;
        }
      }
    }
  }

  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  fclose(fp);

  return 0;
}

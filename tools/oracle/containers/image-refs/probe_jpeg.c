/* What libjpeg says it is, from the header the oracle tools compile against.
 *
 * A compiled probe rather than a dpkg query or a grep of jconfig.h: the
 * version reported has to come from the same headers the driver will use, with
 * nothing between the check and the fact it checks. LIBJPEG_TURBO_VERSION is
 * spelled unquoted, so it needs stringifying twice - once to expand it, once
 * to quote it.
 */
#include <stdio.h>
#include <jconfig.h>

#define STR(x) #x
#define XSTR(x) STR(x)

int main(void) {
#ifdef LIBJPEG_TURBO_VERSION
  printf("libjpeg-turbo %s\n", XSTR(LIBJPEG_TURBO_VERSION));
#else
  printf("libjpeg %d\n", JPEG_LIB_VERSION);
#endif
  return 0;
}

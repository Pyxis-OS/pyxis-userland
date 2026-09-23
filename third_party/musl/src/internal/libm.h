#ifndef PYXIS_MUSL_LIBM_H
#define PYXIS_MUSL_LIBM_H

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

/* Only the x86-64 long-double representation from musl's libm.h is needed.
 * Keep its field names/layout so the imported numerical code stays unchanged. */
#if LDBL_MANT_DIG != 64 || LDBL_MAX_EXP != 16384 || \
    __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error Unsupported long double representation
#endif

union ldshape {
	long double f;
	struct {
		uint64_t m;
		uint16_t se;
	} i;
};

_Static_assert(sizeof(long double) == 16, "long double must use the x86-64 ABI");
_Static_assert(offsetof(union ldshape, i.se) == 8,
  "long double sign/exponent must follow its 64-bit significand");

#endif

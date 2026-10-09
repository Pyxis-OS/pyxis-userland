#ifndef LIBC_PYXIS_RESPONSE_H
#define LIBC_PYXIS_RESPONSE_H

#include <abi/provider.h>
#include <stdint.h>

#define PYXIS_RESPONSE_PROVIDER UINT64_C(1)
#define PYXIS_RESPONSE_HTTP UINT64_C(2)
#define PYXIS_RESPONSE_URL UINT64_C(4)
#define PYXIS_RESPONSE_MEDIA_TYPE UINT64_C(8)

/* A copied description, not authority or a transport lifetime. */
struct pyxis_response_info {
  uint64_t flags;
  uint64_t status;
  uint64_t redirect_count;
  char url[PROVIDER_HTTP_URI_MAX_BYTES + 1];
  char media_type[PROVIDER_MEDIA_TYPE_MAX_BYTES + 1];
};

#endif

#ifndef USERSPACE_CONTENT_SERVICE_H
#define USERSPACE_CONTENT_SERVICE_H

#include <stdint.h>

/* The attached READ capability supplies content. The client field identifies
 * the manual example caller; wide mode fills the remaining payload bytes. */
#define CONTENT_PRINT UINT64_C(1)
#define CONTENT_OK UINT64_C(0)
#define CONTENT_INVALID UINT64_C(1)
#define CONTENT_IO_ERROR UINT64_C(2)

struct content_request {
  uint64_t operation;
  uint64_t client;
};

#endif

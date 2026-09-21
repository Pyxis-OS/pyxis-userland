#ifndef USERSPACE_CONTENT_SERVICE_H
#define USERSPACE_CONTENT_SERVICE_H

#include <stdint.h>

/* The attached READ capability supplies content; message bytes describe the
 * operation only. The kernel does not interpret this application protocol. */
#define CONTENT_PRINT UINT64_C(1)
#define CONTENT_OK UINT64_C(0)
#define CONTENT_INVALID UINT64_C(1)
#define CONTENT_IO_ERROR UINT64_C(2)

struct content_request {
  uint64_t operation;
};

struct content_reply {
  uint64_t status;
  uint64_t size;
};

#endif

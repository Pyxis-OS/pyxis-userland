#ifndef USERSPACE_NUMBER_SERVICE_H
#define USERSPACE_NUMBER_SERVICE_H

#include <stdint.h>

/* Application protocol shared by client and server. The kernel transports
 * these bytes without interpreting the operation, status or value. */
#define NUMBER_DOUBLE UINT64_C(1)
#define NUMBER_OK UINT64_C(0)
#define NUMBER_INVALID UINT64_C(1)

struct number_request {
  uint64_t operation;
  uint64_t value;
};

struct number_reply {
  uint64_t status;
  uint64_t value;
};

#endif

#ifndef HTTPFS_TRANSACTION_H
#define HTTPFS_TRANSACTION_H

#include <http_types.h>

#define HTTP_PREPARE_CREDENTIALS (UINT64_C(1) << 0)
#define HTTP_PREPARE_JSON (UINT64_C(1) << 1)

struct http_framing_budget {
  uint64_t header_bytes, fields, informational;
};

struct http_prepare_context {
  struct http_framing_budget remaining;
};

enum http_transaction_phase {
  HTTP_TRANSACTION_PREPARING,
  HTTP_TRANSACTION_STAGING,
  HTTP_TRANSACTION_OPENING,
  HTTP_TRANSACTION_READING,
  HTTP_TRANSACTION_COMPLETE,
  HTTP_TRANSACTION_FAILED,
  HTTP_TRANSACTION_ABORTED,
};

#endif

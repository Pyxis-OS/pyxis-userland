#ifndef HTTPFS_PROVIDER_H
#define HTTPFS_PROVIDER_H

#include "../libhttp/http.h"
#include <abi/provider.h>

enum httpfs_root {
  HTTPFS_ROOT_HTTP,
  HTTPFS_ROOT_HTTPS,
  HTTPFS_ROOT_JSON_HTTP,
  HTTPFS_ROOT_JSON_HTTPS,
  HTTPFS_ROOT_COUNT,
};

#define HTTPFS_ROOT_OBJECT_BASE UINT64_C(1)
#define HTTPFS_TRANSACTION_OBJECT_BASE (HTTPFS_ROOT_OBJECT_BASE + HTTPFS_ROOT_COUNT)

enum call_status httpfs_staging_validate(handle_t staging, uint64_t *filesystem_type);
/* The launcher supplies a private leaf. RAM requires a positive admitted body
 * cap; zero is reserved for explicitly selected NPFS staging. This cap applies
 * to uploads; GET snapshots grow until their native FILE backing refuses. */
/* Takes ownership of every nonzero root controller, including on error. Root
 * object IDs are BASE + enum httpfs_root; transactions start after the roots.
 * client->tls is ready or NULL, so unavailable TLS roots have no controller. */
enum call_status httpfs_serve(handle_t service, handle_t receiver,
    handle_t root_controllers[HTTPFS_ROOT_COUNT], const struct http_client *client,
    bool custom_trust, handle_t staging, bool ram_backing, uint64_t body_cap);

#endif

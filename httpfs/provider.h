#ifndef HTTPFS_PROVIDER_H
#define HTTPFS_PROVIDER_H

#include "../libhttp/http.h"

enum call_status httpfs_staging_validate(handle_t staging, uint64_t *filesystem_type);
/* The launcher supplies a private leaf. RAM requires a positive admitted body
 * cap; zero is reserved for explicitly selected NPFS staging. */
/* Takes ownership of the published service controller until RETIRE or return. */
enum call_status httpfs_serve(handle_t service, handle_t receiver, handle_t controller,
    const struct http_client *client, bool custom_trust, handle_t staging, uint64_t body_cap);

#endif

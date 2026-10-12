#ifndef HTTPFS_PROVIDER_H
#define HTTPFS_PROVIDER_H

#include "../libhttp/http.h"

enum call_status httpfs_staging_validate(handle_t staging);
enum call_status httpfs_serve(handle_t service, handle_t receiver,
    const struct http_client *client, bool custom_trust, handle_t staging);

#endif

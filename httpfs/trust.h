#ifndef HTTPFS_TRUST_H
#define HTTPFS_TRUST_H

#include "../libtls/tls.h"

#define HTTPFS_PUBLIC_BUNDLE "app://share/ca-certificates/cacert.pem"

enum call_status httpfs_trust_load(struct tls_runtime *runtime, const char *uri,
    struct tls_result *result, enum call_status *cleanup_status);

#endif

#ifndef USERSPACE_HTTP_PREPARE_H
#define USERSPACE_HTTP_PREPARE_H

#include <abi/http_prepare.h>
#include <resource.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Borrows one captured PROVIDER root and opaque counted target/header bytes.
 * Issues an immutable configured HTTP endpoint without network I/O. Requires
 * OPEN_READ for GET, OPEN_WRITE for write methods, and CALL; returned authority
 * never exceeds the root's masks.
 * timeout_ns is a nonzero fresh duration for each subsequent use. deadline_ns
 * is this issuance call's nonzero absolute deadline, copied unchanged. No fallback or
 * retry. CALL_OK publishes an owned typed result, including resource failures;
 * other statuses clear ownership. delivery survives validation failure.
 * Optional unsupported is true only for an explicitly delivered empty
 * BAD_OPERATION reply with no grants; no other refusal authorizes fallback.
 * Private request storage is erased before returning. */
enum call_status http_prepare_endpoint(handle_t root, uint64_t method, uint64_t response_flags,
    const void *target, size_t target_size, const void *headers, size_t headers_size,
    uint64_t timeout_ns, uint64_t deadline_ns, struct resource_result *result, bool *unsupported);

#ifdef __cplusplus
}
#endif

#endif

#ifndef USERSPACE_RESOURCE_H
#define USERSPACE_RESOURCE_H

#include <abi/describe.h>
#include <abi/provider.h>
#include <abi/syscall.h>
#include <stdbool.h>

/* Owns resource and info.error_body, independently. Declared description fields
 * are bounded but untrusted; info's interface and masks are kernel-attested.
 * A failed outcome remains a failure even when resource is valid. */
struct resource_result {
  handle_t resource;
  struct describe_reply info;
  uint64_t delivery;
};

#ifdef __cplusplus
extern "C" {
#endif

bool resource_description_valid(const struct resource_description *description);
enum call_status resource_outcome_status(uint64_t outcome);
enum call_status resource_result_close(struct resource_result *result);

/* Adopts resource only on success; failure preserves the caller's ownership.
 * Describe does not invoke the issuer; any returned body is owned copied bytes. */
enum call_status resource_result_adopt(handle_t resource, struct resource_result *result);

/* Borrows target and never rediscovers a name. PROVIDER receives the opaque
 * remainder. A held FILE requires an empty remainder and the intent's rights;
 * CREATE/EXCLUSIVE without a name are unsupported. A native DIRECTORY walks
 * components within the held subtree; .. cannot escape its root. Directory
 * children inherit no more than the actual parent mask; empty remainder copies
 * that mask. Final FILE access follows intent. CREATE is exclusive at native
 * publication: ALREADY_EXISTS after a missing lookup is final, never re-opened.
 * Exported directories and flags on a directory result are unsupported.
 * Exported truncation uses the original endpoint deadline; native lookup,
 * create and resize keep their synchronous wait/cancellation contracts.
 * APPEND is consumer intent, not atomic append.
 * No automatic retry. */
enum call_status resource_open_held(handle_t target, const char *remainder,
    uint64_t intent, uint64_t flags, uint64_t deadline_ns,
    struct resource_result *result);

struct path_context;
/* Exact name:// lookup captures one CAP or sealed TEXT FILE from the caller's
 * environment. Relative paths copy the complete retained cwd chain, so .. can
 * traverse its ancestors but never escape its boundary. No URI expansion or
 * legacy HTTP fallback. Returned ownership and outcomes match held OPEN. */
enum call_status resource_open_path(const struct path_context *context, const char *path,
    uint64_t intent, uint64_t flags, uint64_t deadline_ns,
    struct resource_result *result);

/* Libc FILE admission: same captured native traversal and resource flags, but
 * reject every exported/non-FILE held target before any OPEN or mutation.
 * A held native FILE already exists: CREATE is ignored and EXCLUSIVE fails.
 * Ownership follows resource_open_path(); no provider call is issued. */
enum call_status resource_open_native_file_path(const struct path_context *context,
    const char *path, uint64_t intent, uint64_t flags, struct resource_result *result);

/* Metadata admission, without mutation: retain all delegated directory rights
 * and obtainable child FILE READ/WRITE rights in one captured traversal.
 * Held FILE rights are preserved. A provider with OPEN_WRITE is unsupported
 * before OPEN; a provider without it may supply a READ result with its actual
 * received FILE rights. Success owns result as above; outcomes remain fallible. */
enum call_status resource_open_metadata_path(const struct path_context *context,
    const char *path, struct resource_result *result);

/* The metadata route above, confined to native FILE and DIRECTORY grants.
 * Provider/exported targets are refused before OPEN or any network I/O. */
enum call_status resource_open_native_metadata_path(const struct path_context *context,
    const char *path, struct resource_result *result);

#ifdef __cplusplus
}
#endif

#endif

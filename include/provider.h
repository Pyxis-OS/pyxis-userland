#ifndef USERSPACE_PROVIDER_H
#define USERSPACE_PROVIDER_H

#include <abi/provider.h>
#include <abi/handle.h>
#include <abi/syscall.h>
#include <stddef.h>
#include <resource.h>

struct provider_metadata {
  uint64_t protocol;
  size_t media_type_size;
  char media_type[RESOURCE_MEDIA_TYPE_MAX_BYTES + 1];
};

struct provider_result {
  enum call_status status;
  uint64_t delivery;
  struct provider_metadata metadata;
};

/* Generic typed OPEN, with no HTTP context or fallback. CALL_OK publishes an
 * owned result, including resource failures; callers inspect its outcome.
 * Other statuses are transport/local/admission failures and clear ownership.
 * Issuers select actual attenuated authority; it is checked against the kernel,
 * never widened to intent. Error bodies come from native DESCRIBE snapshots.
 * The absolute deadline is copied unchanged and never restarted. No retry. */
enum call_status provider_open_typed(handle_t provider, const char *target,
    uint64_t intent, uint64_t flags, uint64_t deadline_ns,
    struct resource_result *result);

/* Name-based REMOVE uses independent REMOVE authority and the same typed
 * outcomes. Success has no owned resource; failure may own one for description
 * and error-body access. A delivered operation whose reply is lost reports
 * OUTCOME_UNKNOWN with delivery preserved. The deadline is never restarted;
 * no retry, name lookup or fallback occurs here. */
enum call_status provider_remove_typed(handle_t provider, const char *target,
    uint64_t deadline_ns, struct resource_result *result);

/* Existing FILE consumers adapt the same typed OPEN, with no second wire ABI.
 * CALL_OK makes result->status meaningful. A successful outcome must be a FILE
 * with the intent's access and usable transport; other types are unsupported.
 * Success transfers an owned file. Failures close resource and body grants.
 * Copied media type is counted UTF-8. delivery survives validation failure. */
enum call_status provider_open(handle_t provider, const char *target, uint64_t intent,
    uint64_t flags, uint64_t deadline_ns, struct provider_result *result, handle_t *file);

#endif

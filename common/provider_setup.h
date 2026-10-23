#ifndef USERSPACE_PROVIDER_SETUP_H
#define USERSPACE_PROVIDER_SETUP_H

#include <clock.h>
#include <endpoint.h>
#include <handle.h>
#include <stdbool.h>
#include <string.h>

#define PROVIDER_SETUP_TIMEOUT_NS UINT64_C(10000000000)

#define PROVIDER_SETUP_ROOT_MAX 4
#define PROVIDER_SETUP_ROLE_MAX 31
#define PROVIDER_SETUP_ROOTS_ENTRY "publication.roots"
#define PROVIDER_SETUP_DEFAULT_ROLE "default"

/* Grants correspond to role entries, never environment names. The launcher
 * fixes destination names, masks and optionality before starting the leaf.
 * Success carries 1..4 unique roles; failure carries no roles or grants.
 * Cleanup failure is never optional. Every in-tree provider uses this layout.
 * Ready submission needs a bounded clock; attempted changes only on submission.
 * A cleaned-up failure can still report when its clock is unavailable. */
struct provider_setup {
  uint64_t status;
  uint64_t cleanup_status;
  uint64_t root_count;
  char roles[PROVIDER_SETUP_ROOT_MAX][PROVIDER_SETUP_ROLE_MAX + 1];
};

struct provider_setup_root {
  const char *role;
  struct endpoint_grant grant;
};

static inline enum call_status provider_setup_report_roots(handle_t publication,
    handle_t clock, enum call_status status, enum call_status cleanup_status,
    const struct provider_setup_root *roots, size_t root_count, bool *attempted)
{
  if (root_count > PROVIDER_SETUP_ROOT_MAX || (root_count && !roots) ||
      (status == CALL_OK ? !root_count || cleanup_status != CALL_OK : root_count)) {
    return CALL_BAD_REQUEST;
  }
  struct provider_setup setup = {.status = status, .cleanup_status = cleanup_status,
    .root_count = root_count};
  struct endpoint_grant grants[PROVIDER_SETUP_ROOT_MAX];
  for (size_t i = 0; i < root_count; ++i) {
    size_t length = roots[i].role ? strlen(roots[i].role) : 0;
    if (!length || length > PROVIDER_SETUP_ROLE_MAX) {
      return CALL_BAD_REQUEST;
    }
    for (size_t j = 0; j < i; ++j) {
      if (!strcmp(roots[j].role, roots[i].role)) {
        return CALL_BAD_REQUEST;
      }
    }
    memcpy(setup.roles[i], roots[i].role, length);
    grants[i] = roots[i].grant;
  }
  uint64_t now;
  uint64_t deadline = 0;
  enum call_status clock_status = clock_now(clock, &now);
  if (clock_status == CALL_OK && now > UINT64_MAX - PROVIDER_SETUP_TIMEOUT_NS) {
    clock_status = CALL_LIMIT;
  }
  if (clock_status == CALL_OK) {
    deadline = now + PROVIDER_SETUP_TIMEOUT_NS;
  } else if (root_count) {
    /* The caller must release failed setup before reporting it. */
    return clock_status;
  }
  struct endpoint_packet response = {0};
  if (attempted) {
    *attempted = true;
  }
  enum call_status result = endpoint_request(publication, &setup, sizeof(setup),
      grants, root_count, deadline, &response);
  for (size_t i = 0; i < response.grant_count; ++i) {
    if (handle_close(response.grants[i].handle) != 0 && result == CALL_OK) {
      result = CALL_BAD_HANDLE;
    }
  }
  if (result == CALL_OK && (response.result != CALL_OK || response.size != 0 ||
      response.grant_count != 0)) {
    result = response.result != CALL_OK && response.result < CALL_STATUS_COUNT ?
        response.result : CALL_BAD_REQUEST;
  }
  return result;
}

static inline enum call_status provider_setup_report(handle_t publication,
    handle_t clock, enum call_status status, enum call_status cleanup_status,
    const struct endpoint_grant *grant, bool *attempted)
{
  struct provider_setup_root root = {.role = PROVIDER_SETUP_DEFAULT_ROLE};
  if (grant) {
    root.grant = *grant;
  }
  return provider_setup_report_roots(publication, clock, status, cleanup_status,
      grant ? &root : NULL, grant ? 1 : 0, attempted);
}

/* The launcher side, shared by the shell's service start and installer-init. */

/* Waits until RECEIVER holds the provider's report, at most the setup timeout
 * read from CLOCK. CALL_IO: PROCESS completed first. CALL_ABANDONED: the
 * optional ARMED interrupt fired. */
enum call_status provider_setup_receive(handle_t clock, handle_t process, handle_t receiver,
    handle_t armed, struct endpoint_packet *packet);

enum provider_setup_outcome {
  PROVIDER_SETUP_INVALID, /* Not a setup report of this layout. */
  PROVIDER_SETUP_READY,   /* Success with 1..4 roots, one grant each. */
  PROVIDER_SETUP_FAILED,  /* A reported failure, with no roots or grants. */
};

/* Reads PACKET as a setup report into SETUP. */
enum provider_setup_outcome provider_setup_read(const struct endpoint_packet *packet,
    struct provider_setup *setup);

/* Matches each reported root of a READY SETUP to one of ROLES. SELECTED[i] is
 * the PACKET grant index for ROLES[i], or SIZE_MAX when the provider did not
 * report that role. An unknown or repeated role, or a grant that is not an
 * exported CALL handle holding exactly its reported rights, is refused. */
enum call_status provider_setup_match(const struct provider_setup *setup,
    const struct endpoint_packet *packet, const char *const *roles, size_t role_count,
    size_t *selected);

#endif

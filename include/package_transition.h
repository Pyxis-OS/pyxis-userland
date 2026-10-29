#ifndef USERSPACE_PACKAGE_TRANSITION_H
#define USERSPACE_PACKAGE_TRANSITION_H

#include <abi/handle.h>
#include <abi/syscall.h>
#include <stdbool.h>
#include <stdint.h>

/* The installed boot identity is a committed ESP attempt whose payload hashes
 * match the files the kernel actually loaded. Pool is mount.pool from the
 * selected boot entry. Only boot-init supplies an observed identity. */
struct package_boot_identity {
  uint32_t pool;
  char attempt[33];
  uint64_t kernel_size, archive_size;
  char kernel_sha256[65], archive_sha256[65];
};

struct package_transition {
  uint64_t from_set, to_set;
  struct package_boot_identity target;
};

bool package_boot_identity_valid(const struct package_boot_identity *identity);
bool package_boot_identity_equal(const struct package_boot_identity *a,
    const struct package_boot_identity *b);

/* Missing records return CALL_OK with PRESENT=false. Malformed records refuse;
 * they never become an empty or already-completed transition. */
enum call_status package_boot_load(handle_t packages, bool *present,
    uint64_t *set, struct package_boot_identity *identity);
enum call_status package_transition_load(handle_t packages, bool *present,
    struct package_transition *transition);

/* Both records are native, synchronized files. A pending record is created
 * once before any ESP publication or a confirmed ordinary set switch; an
 * existing one always refuses. Ordinary switches retain the current identity. */
enum call_status package_boot_publish(handle_t packages, uint64_t set,
    const struct package_boot_identity *identity);
enum call_status package_transition_publish(handle_t packages,
    const struct package_transition *transition);
enum call_status package_transition_remove(handle_t packages);

/* The same durable pointer switch used by pkg and boot-init. A failure after
 * rename starts leaves current uncertain; the pending record must stay. */
enum call_status package_set_activate(handle_t packages, uint64_t set);

#endif

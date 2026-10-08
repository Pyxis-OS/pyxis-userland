#ifndef USERSPACE_SPACE_H
#define USERSPACE_SPACE_H

#include <abi/handle.h>
#include <abi/launcher.h>
#include <abi/space.h>
#include <abi/syscall.h>

/* SET_TITLE authority for the caller's space. Nonempty printable ASCII, at
 * most SPACE_TITLE_MAX bytes. Copies the title; caller retains the string. */
enum call_status space_set_title(handle_t space, const char *title);

/* SET_AFFINITY authority for the caller's space, before its first launch.
 * CPUS holds ceil(CPU_COUNT / 64) words; bit N of word N / 64 selects boot CPU
 * N. See <abi/space.h> for the ceiling, BSP and failure rules. */
enum call_status space_set_affinity(handle_t space, const uint64_t *cpus, uint64_t cpu_count);

/* A new space's fixed name, initial title and creation flags. Trusted local
 * mux startup uses SPACE_CREATE_TERMINAL_CONTROL; ordinary creation uses zero. */
struct space_definition {
  const char *name;
  const char *title;
  uint64_t flags;
};

/* CREATE authority on the space factory. Appends a space that never starts;
 * its tab shows REASON, 1..SPACE_REASON_MAX printable ASCII bytes. An
 * untrustworthy response returns CALL_OUTCOME_UNKNOWN: the space may exist. */
enum call_status space_create_unstarted(handle_t factory, const struct space_definition *space,
    const char *reason);

/* CREATE authority on the space factory. Appends a space whose ceiling is CPUS
 * (SET_AFFINITY's encoding) and launches REQUEST's native image as its first
 * process. Streams must be NONE; the kernel adds the space's console, input
 * devices, display and space handle. Success returns an owned WAIT handle.
 * Source grants survive. An untrustworthy response returns
 * CALL_OUTCOME_UNKNOWN. Use program_create_space for scripts. */
enum call_status space_create_started(handle_t factory, const struct space_definition *space,
    const uint64_t *cpus, uint64_t cpu_count, const struct launch_request *request,
    handle_t *child);

#endif

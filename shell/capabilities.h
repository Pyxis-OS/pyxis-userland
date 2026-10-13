#ifndef USERSPACE_SHELL_CAPABILITIES_H
#define USERSPACE_SHELL_CAPABILITIES_H

#include "shell.h"
#include <http_transaction.h>

#define SHELL_CAPABILITY_LIMIT 16
#define SHELL_CAPABILITY_NAME_MAX 63

enum shell_capability_type {
  SHELL_CAPABILITY_REQUEST,
  SHELL_CAPABILITY_RESPONSE,
  SHELL_CAPABILITY_STATUS,
};

/* Copies share local refusal/deadline state only; every facet below is a
 * separately owned native handle. No control facet is hidden in this owner. */
struct shell_capability_state {
  size_t references;
  uint64_t deadline_ns;
  enum call_status failure;
  bool uncertain;
};

struct shell_capability {
  char name[SHELL_CAPABILITY_NAME_MAX + 1];
  enum shell_capability_type type;
  struct http_prepared value;
  struct shell_capability_state *state;
};

struct shell_capability_table;

/* Stable entry addresses until explicit drop/exit. Reserve allocates every
 * local owner before preparation or launch; failed preparation stays named. */
enum call_status shell_capability_reserve(struct shell *shell, const char *name,
    struct shell_capability **entry);
/* Accepts a portable name or its @name selector; neither is an argv path. */
enum call_status shell_capability_lookup(struct shell *shell, const char *name,
    struct shell_capability **entry);
enum call_status shell_capability_prepare(struct shell *shell,
    struct shell_capability *entry, const char *uri, uint64_t method,
    const char *headers, size_t headers_size, uint64_t timeout_seconds);
/* Success is a 2xx header handoff; error responses stay readable. No framing
 * completion is claimed. A lost/failed commit cannot be replayed locally. */
enum command_result shell_capability_commit(struct shell *shell,
    struct shell_capability *entry);
/* Latches producer/cleanup failure in every copy and aborts known unfinished
 * controlled work. Later manual commit cannot bypass this local failure. */
void shell_capability_fail(struct shell *shell, struct shell_capability *entry,
    enum call_status status);
handle_t shell_capability_body(const struct shell_capability *entry);
enum command_result shell_request_command(struct shell *shell,
    char **arguments, size_t count);
void shell_capabilities_close(struct shell *shell);

#endif

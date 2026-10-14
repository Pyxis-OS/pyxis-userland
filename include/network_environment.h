#ifndef USERSPACE_NETWORK_ENVIRONMENT_H
#define USERSPACE_NETWORK_ENVIRONMENT_H

#include <abi/handle.h>
#include <abi/launcher.h>
#include <abi/syscall.h>
#include <stddef.h>
#include <environment.h>

struct network_environment {
  struct launch_environment_entry *variables;
  size_t count;
  char dns_server[sizeof("255.255.255.255")];
};

/* Owns the array and chosen DNS text; other strings borrow source or fallback.
 * A nonzero chosen kernel DNS replaces DNS_SERVER. Otherwise preserve its
 * inherited value, or use fallback when absent. An absent authority is allowed.
 * Keep this structure in place until the launcher has copied its metadata. */
enum call_status network_environment_read(struct network_environment *environment,
    handle_t authority, const struct launch_environment_entry *source, size_t count,
    const char *fallback);
enum call_status network_environment_from_capture(struct network_environment *environment,
    handle_t authority, const struct environment_capture *source, const char *fallback);
void network_environment_free(struct network_environment *environment);
/* Initial provider startup waits at most ten seconds for the setup owner's
 * first DNS choice; timeout permits the existing fallback environment. */
enum call_status network_environment_wait(handle_t authority, handle_t clock);

#endif

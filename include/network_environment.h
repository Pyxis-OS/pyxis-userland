#ifndef USERSPACE_NETWORK_ENVIRONMENT_H
#define USERSPACE_NETWORK_ENVIRONMENT_H

#include <abi/handle.h>
#include <abi/startup.h>
#include <abi/syscall.h>
#include <stddef.h>

struct network_environment {
  struct startup_variable *variables;
  size_t count;
  char dns_server[sizeof("255.255.255.255")];
};

/* Owns the array and chosen DNS text; other strings borrow source or fallback.
 * A nonzero chosen kernel DNS replaces DNS_SERVER. Otherwise preserve its
 * inherited value, or use fallback when absent. An absent authority is allowed.
 * Keep this structure in place until the launcher has copied its metadata. */
enum call_status network_environment_read(struct network_environment *environment,
    handle_t authority, const struct startup_variable *source, size_t count,
    const char *fallback);
void network_environment_free(struct network_environment *environment);

#endif

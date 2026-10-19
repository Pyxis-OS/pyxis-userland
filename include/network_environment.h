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
  handle_t dns_udp;
  char dns_server[sizeof("255.255.255.255")];
};

/* Owns the array, canonical DNS text and attenuated dns_udp factory. The
 * resolver is available only when its selected interface is permitted by udp.
 * Missing/denied scoped paths omit DNS_SERVER; text grants no packet authority.
 * Without a registry retain the inherited startup choice only with held udp. */
enum call_status network_environment_read(struct network_environment *environment,
    handle_t registry, handle_t udp, const struct launch_environment_entry *source, size_t count);
enum call_status network_environment_from_capture(struct network_environment *environment,
    handle_t registry, handle_t udp, const struct environment_capture *source);
void network_environment_free(struct network_environment *environment);
/* Initial provider startup waits at most ten seconds for the setup owner's
 * first DNS choice; timeout leaves DNS unavailable. */
enum call_status network_environment_wait(handle_t authority, handle_t clock);

#endif

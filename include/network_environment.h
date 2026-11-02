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
  handle_t dns_tcp; /* Same scope and server, CONNECT only; HANDLE_INVALID when no TCP is held. */
  char dns_server[sizeof("255.255.255.255")];
};

/* Owns the array, canonical DNS text and the attenuated dns_udp and dns_tcp
 * factories (dns_tcp follows dns_udp's scope from a held tcp factory and is
 * simply absent when none is held or the scope is refused). The
 * Numeric DNS without an interface retains udp's immutable initial scope; a
 * selected DNS interface instead requires a permitted one-interface derivative.
 * Missing/denied scoped paths omit DNS_SERVER; text grants no packet authority.
 * Without a registry retain the inherited startup choice only with held udp. */
enum call_status network_environment_read(struct network_environment *environment,
    handle_t registry, handle_t udp, handle_t tcp,
    const struct launch_environment_entry *source, size_t count);
enum call_status network_environment_from_capture(struct network_environment *environment,
    handle_t registry, handle_t udp, handle_t tcp, const struct environment_capture *source);
void network_environment_free(struct network_environment *environment);
/* Initial provider startup waits at most ten seconds for the setup owner's
 * first DNS choice; timeout leaves DNS unavailable. */
enum call_status network_environment_wait(handle_t authority, handle_t clock);

#endif

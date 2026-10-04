#ifndef SESSION_UDP_SERVER_H
#define SESSION_UDP_SERVER_H

#include <stdbool.h>
#include <stdint.h>

/* Trusted setup creates a wildcard endpoint. The echo child receives only
 * inspect/send/receive authority; it cannot create endpoints or configure net0. */
int launch_udp_broadcast(uint16_t port, const char *count, bool unassigned);

#endif

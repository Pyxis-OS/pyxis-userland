#ifndef SESSION_TCP_SERVER_H
#define SESSION_TCP_SERVER_H

#include <stdint.h>

/* Trusted handoff: creates a bound listener and launches a sequential echo
 * server with listener authority only. A NULL count serves until failure. */
int launch_tcp_server(uint32_t address, uint16_t port, const char *count);

#endif

#ifndef SESSION_REMOTE_SERVER_H
#define SESSION_REMOTE_SERVER_H

#include "config.h"
#include "../network-runtime/network.h"
#include <stdint.h>

/* Borrows the retained bootstrap program images through launch admission. */
int launch_remote_server(const struct session_config *config,
    const struct network_config *network, uint16_t port, handle_t retention_account,
    handle_t image, handle_t mux_image);

#endif

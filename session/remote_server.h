#ifndef SESSION_REMOTE_SERVER_H
#define SESSION_REMOTE_SERVER_H

#include "config.h"
#include "network.h"
#include <stdint.h>

int launch_remote_server(const struct session_config *config,
    const struct network_config *network, uint16_t port);

#endif

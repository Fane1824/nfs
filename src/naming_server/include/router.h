// src/naming_server/include/router.h

#ifndef ROUTER_H
#define ROUTER_H

#include "network.h"
#include "protocol.h"
#include "errors.h"

// Initialize the router module
void router_init();

// Clean up the router module
void router_cleanup();

// Forward a client request to a storage server
ErrorCode router_forward_request(NetworkSocket *client_sock, MessageHeader *header);

#endif // ROUTER_H
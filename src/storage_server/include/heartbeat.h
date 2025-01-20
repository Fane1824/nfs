// src/storage_server/include/heartbeat.h

#ifndef HEARTBEAT_H
#define HEARTBEAT_H

#include "network.h"

// Start the heartbeat thread
void start_heartbeat(const char *naming_server_host, const char *naming_server_port, const char *host, const char *port);
#endif // HEARTBEAT_H
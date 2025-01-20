// src/naming_server/include/health.h

#ifndef HEALTH_H
#define HEALTH_H

#include "errors.h"
#include <time.h>

// Structure to represent a storage server
typedef struct {
    char host[256];
    char port[32];
    time_t last_heartbeat;
    int load;
    int active; // 1 if active, 0 if inactive
} StorageServer;

// Initialize the health monitoring system
void health_init();

// Clean up the health monitoring system
void health_cleanup();

// Receive a heartbeat from a storage server
void health_receive_heartbeat(const char *host, const char *port, int load);

// Get the list of all storage servers
ErrorCode health_get_servers(StorageServer **servers_out, int *count_out);

// Check for inactive servers periodically
void *health_monitor(void *arg);

#endif // HEALTH_H
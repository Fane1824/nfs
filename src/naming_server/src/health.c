// src/naming_server/src/health.c

#include "health.h"
#include "network.h"
#include "protocol.h"
#include "errors.h"
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define MAX_SERVERS 100
#define HEARTBEAT_TIMEOUT 15 // Seconds

static StorageServer servers_list[MAX_SERVERS];
static int server_count = 0;
static pthread_mutex_t servers_mutex = PTHREAD_MUTEX_INITIALIZER;

void health_init() {
    pthread_mutex_init(&servers_mutex, NULL);
    // Start the health monitor thread
    pthread_t monitor_thread;
    pthread_create(&monitor_thread, NULL, health_monitor, NULL);
    pthread_detach(monitor_thread);
}

void health_cleanup() {
    pthread_mutex_destroy(&servers_mutex);
}

void health_receive_heartbeat(const char *host, const char *port, int load) {
    pthread_mutex_lock(&servers_mutex);

    // Check if the server is already in the list
    for (int i = 0; i < server_count; i++) {
        if (strcmp(servers_list[i].host, host) == 0 && strcmp(servers_list[i].port, port) == 0) {
            // Update existing server's info
            servers_list[i].last_heartbeat = time(NULL);
            servers_list[i].load = load;
            servers_list[i].active = 1;
            pthread_mutex_unlock(&servers_mutex);
            return;
        }
    }

    // Add new server if there's space
    if (server_count < MAX_SERVERS) {
        strncpy(servers_list[server_count].host, host, sizeof(servers_list[server_count].host) - 1);
        strncpy(servers_list[server_count].port, port, sizeof(servers_list[server_count].port) - 1);
        servers_list[server_count].last_heartbeat = time(NULL);
        servers_list[server_count].load = load;
        servers_list[server_count].active = 1;
        server_count++;
    } else {
        fprintf(stderr, "Server list full. Cannot add new server %s:%s\n", host, port);
    }

    pthread_mutex_unlock(&servers_mutex);
}

ErrorCode health_get_servers(StorageServer **servers_out, int *count_out) {
    pthread_mutex_lock(&servers_mutex);

    int active_count = 0;
    for (int i = 0; i < server_count; i++) {
        if (servers_list[i].active) {
            active_count++;
        }
    }

    if (active_count == 0) {
        pthread_mutex_unlock(&servers_mutex);
        return ERR_NOT_FOUND;
    }

    StorageServer *active_servers = malloc(sizeof(StorageServer) * active_count);
    if (!active_servers) {
        pthread_mutex_unlock(&servers_mutex);
        return ERR_INTERNAL_ERROR;
    }

    int idx = 0;
    for (int i = 0; i < server_count; i++) {
        if (servers_list[i].active) {
            active_servers[idx++] = servers_list[i];
        }
    }

    *servers_out = active_servers;
    *count_out = active_count;

    pthread_mutex_unlock(&servers_mutex);
    return ERR_SUCCESS;
}

void *health_monitor(void *arg) {
    while (1) {
        sleep(HEARTBEAT_TIMEOUT);
        pthread_mutex_lock(&servers_mutex);
        time_t now = time(NULL);
        for (int i = 0; i < server_count; i++) {
            if (servers_list[i].active && (now - servers_list[i].last_heartbeat) > HEARTBEAT_TIMEOUT) {
                servers_list[i].active = 0;
                fprintf(stderr, "Storage server %s:%s is inactive\n", servers_list[i].host, servers_list[i].port);
                // Optionally trigger replication or failover mechanisms here
            }
        }
        pthread_mutex_unlock(&servers_mutex);
    }
    return NULL;
}
// src/storage_server/src/heartbeat.c

#include "heartbeat.h"
#include "network.h"
#include "protocol.h"
#include "errors.h"
#include "storage.h"
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define HEARTBEAT_INTERVAL 5  // Interval in seconds

static char ns_host[256];
static char ns_port[32];
static char server_host[256];
static char server_port[32];

static pthread_mutex_t heartbeat_mutex = PTHREAD_MUTEX_INITIALIZER;

static void *send_heartbeat(void *arg) {
    while (1) {
        sleep(HEARTBEAT_INTERVAL);

        // Prepare heartbeat message
        HeartbeatMessage hb;
        memset(&hb, 0, sizeof(hb));
        strncpy(hb.host, server_host, sizeof(hb.host) - 1);
        strncpy(hb.port, server_port, sizeof(hb.port) - 1);
        hb.load = storage_get_load();  // Get current load

        // Prepare message header
        MessageHeader header;
        memset(&header, 0, sizeof(header));
        header.type = MSG_TYPE_HEARTBEAT;
        header.payload_size = htonl(sizeof(hb));

        // Send heartbeat to naming server
        pthread_mutex_lock(&heartbeat_mutex);

        // Establish a new connection for heartbeat
        NetworkSocket *ns_sock = network_socket_create(ns_host, ns_port);
        if (!ns_sock) {
            fprintf(stderr, "Failed to connect to Naming Server for heartbeat at %s:%s\n", ns_host, ns_port);
            pthread_mutex_unlock(&heartbeat_mutex);
            continue;
        }

        ssize_t sent = network_socket_send(ns_sock, &header, sizeof(header));
        if (sent != sizeof(header)) {
            fprintf(stderr, "Failed to send heartbeat header to naming server\n");
            network_socket_close(ns_sock);
            pthread_mutex_unlock(&heartbeat_mutex);
            continue;
        }

        sent = network_socket_send(ns_sock, &hb, sizeof(hb));
        if (sent != sizeof(hb)) {
            fprintf(stderr, "Failed to send heartbeat payload to naming server\n");
            network_socket_close(ns_sock);
            pthread_mutex_unlock(&heartbeat_mutex);
            continue;
        }

        // Close the connection after sending the heartbeat
        network_socket_close(ns_sock);
        pthread_mutex_unlock(&heartbeat_mutex);
    }
    return NULL;
}

void start_heartbeat(const char *naming_server_host, const char *naming_server_port, const char *host, const char *port) {
    strncpy(ns_host, naming_server_host, sizeof(ns_host) - 1);
    strncpy(ns_port, naming_server_port, sizeof(ns_port) - 1);
    strncpy(server_host, host, sizeof(server_host) - 1);
    strncpy(server_port, port, sizeof(server_port) - 1);

    pthread_t thread;
    if (pthread_create(&thread, NULL, send_heartbeat, NULL) != 0) {
        fprintf(stderr, "Failed to create heartbeat thread\n");
    } else {
        pthread_detach(thread);
    }
}
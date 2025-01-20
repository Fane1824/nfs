#include "replication.h"
#include "network.h"
#include "storage.h"
#include "protocol.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct SecondaryServer {
    char *host;
    char *port;
    NetworkSocket *sock;
    int is_alive;
    pthread_mutex_t lock;
    struct SecondaryServer *next;
} SecondaryServer;

static SecondaryServer *secondaries = NULL;
static pthread_mutex_t replication_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t health_thread;
static int running = 0;

ErrorCode replication_init() {
    running = 1;
    // Start health check thread
    if (pthread_create(&health_thread, NULL, (void *(*)(void *))replication_check_health, NULL) != 0) {
        return ERR_INTERNAL_ERROR;
    }
    return ERR_SUCCESS;
}

void replication_cleanup() {
    running = 0;
    pthread_join(health_thread, NULL);

    pthread_mutex_lock(&replication_lock);
    SecondaryServer *current = secondaries;
    while (current) {
        SecondaryServer *next = current->next;
        network_socket_close(current->sock);
        pthread_mutex_destroy(&current->lock);
        free(current->host);
        free(current->port);
        free(current);
        current = next;
    }
    secondaries = NULL;
    pthread_mutex_unlock(&replication_lock);
    pthread_mutex_destroy(&replication_lock);
}

ErrorCode replication_add_secondary(const char *host, const char *port) {
    SecondaryServer *server = malloc(sizeof(SecondaryServer));
    if (!server) return ERR_INTERNAL_ERROR;

    server->host = strdup(host);
    server->port = strdup(port);
    server->sock = network_socket_create(host, port);
    if (!server->sock) {
        free(server->host);
        free(server->port);
        free(server);
        return ERR_NETWORK_FAILURE;
    }
    server->is_alive = 1;
    pthread_mutex_init(&server->lock, NULL);

    pthread_mutex_lock(&replication_lock);
    server->next = secondaries;
    secondaries = server;
    pthread_mutex_unlock(&replication_lock);

    return ERR_SUCCESS;
}

ErrorCode replication_remove_secondary(const char *host, const char *port) {
    pthread_mutex_lock(&replication_lock);
    SecondaryServer *current = secondaries;
    SecondaryServer *prev = NULL;
    while (current) {
        if (strcmp(current->host, host) == 0 && strcmp(current->port, port) == 0) {
            if (prev) {
                prev->next = current->next;
            } else {
                secondaries = current->next;
            }
            network_socket_close(current->sock);
            pthread_mutex_destroy(&current->lock);
            free(current->host);
            free(current->port);
            free(current);
            pthread_mutex_unlock(&replication_lock);
            return ERR_SUCCESS;
        }
        prev = current;
        current = current->next;
    }
    pthread_mutex_unlock(&replication_lock);
    return ERR_NOT_FOUND;
}

ErrorCode replication_replicate_write(const char *filepath, uint64_t offset, const uint8_t *buffer, size_t length) {
    pthread_mutex_lock(&replication_lock);
    SecondaryServer *current = secondaries;
    while (current) {
        pthread_mutex_lock(&current->lock);
        if (current->is_alive) {
            // Prepare write request
            WriteRequest request = {0};
            request.header.type = MSG_TYPE_REPLICATE_WRITE;
            strncpy(request.filepath, filepath, sizeof(request.filepath) - 1);
            request.offset = offset;
            request.length = length;

            // Send request header
            ssize_t sent = network_socket_send(current->sock, &request, sizeof(request));
            if (sent != sizeof(request)) {
                current->is_alive = 0;
                pthread_mutex_unlock(&current->lock);
                current = current->next;
                continue;
            }
            // Send data
            sent = network_socket_send(current->sock, buffer, length);
            if (sent != (ssize_t)length) {
                current->is_alive = 0;
                pthread_mutex_unlock(&current->lock);
                current = current->next;
                continue;
            }
        }
        pthread_mutex_unlock(&current->lock);
        current = current->next;
    }
    pthread_mutex_unlock(&replication_lock);

    return ERR_SUCCESS;
}

ErrorCode replication_replicate_delete(const char *filepath) {
    pthread_mutex_lock(&replication_lock);
    SecondaryServer *current = secondaries;
    while (current) {
        pthread_mutex_lock(&current->lock);
        if (current->is_alive) {
            // Prepare delete request
            DeleteRequest request = {0};
            request.header.type = MSG_TYPE_REPLICATE_DELETE;
            strncpy(request.filepath, filepath, sizeof(request.filepath) - 1);

            // Send request
            ssize_t sent = network_socket_send(current->sock, &request, sizeof(request));
            if (sent != sizeof(request)) {
                current->is_alive = 0;
                pthread_mutex_unlock(&current->lock);
                current = current->next;
                continue;
            }
        }
        pthread_mutex_unlock(&current->lock);
        current = current->next;
    }
    pthread_mutex_unlock(&replication_lock);

    return ERR_SUCCESS;
}

void replication_check_health() {
    while (running) {
        pthread_mutex_lock(&replication_lock);
        SecondaryServer *current = secondaries;
        while (current) {
            pthread_mutex_lock(&current->lock);
            if (!current->is_alive) {
                // Try to reconnect
                network_socket_close(current->sock);
                current->sock = network_socket_create(current->host, current->port);
                if (current->sock) {
                    current->is_alive = 1;
                }
            }
            pthread_mutex_unlock(&current->lock);
            current = current->next;
        }
        pthread_mutex_unlock(&replication_lock);
        sleep(5); // Check every 5 seconds
    }
}
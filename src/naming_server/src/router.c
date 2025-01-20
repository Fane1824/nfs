// src/naming_server/src/router.c

#include "router.h"
#include "health.h"
#include "network.h"
#include "protocol.h"
#include "errors.h"
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define MAX_CONNECTIONS 100

typedef struct StorageConnection {
    char host[256];
    char port[32];
    NetworkSocket *sock;
    int in_use;
    pthread_mutex_t lock;
} StorageConnection;

static void send_error_response(NetworkSocket *client_sock, ErrorCode code);

static StorageConnection connections[MAX_CONNECTIONS];
static int connection_count = 0;
static pthread_mutex_t connections_mutex = PTHREAD_MUTEX_INITIALIZER;

// Initialize the router
void router_init() {
    pthread_mutex_init(&connections_mutex, NULL);
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        connections[i].sock = NULL;
        connections[i].in_use = 0;
        pthread_mutex_init(&connections[i].lock, NULL);
    }
}

// Clean up the router
void router_cleanup() {
    pthread_mutex_lock(&connections_mutex);
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (connections[i].sock) {
            network_socket_close(connections[i].sock);
            connections[i].sock = NULL;
        }
        pthread_mutex_destroy(&connections[i].lock);
    }
    pthread_mutex_unlock(&connections_mutex);
    pthread_mutex_destroy(&connections_mutex);
}

// Get a storage connection from the pool or create a new one
static StorageConnection *get_storage_connection(const char *host, const char *port) {
    pthread_mutex_lock(&connections_mutex);
    // Search for an existing connection
    for (int i = 0; i < connection_count; i++) {
        StorageConnection *conn = &connections[i];
        if (strcmp(conn->host, host) == 0 && strcmp(conn->port, port) == 0 && !conn->in_use) {
            conn->in_use = 1;
            pthread_mutex_unlock(&connections_mutex);
            return conn;
        }
    }

    // If no existing connection, create a new one
    if (connection_count >= MAX_CONNECTIONS) {
        pthread_mutex_unlock(&connections_mutex);
        return NULL;
    }

    StorageConnection *conn = &connections[connection_count++];
    strncpy(conn->host, host, sizeof(conn->host) - 1);
    strncpy(conn->port, port, sizeof(conn->port) - 1);
    conn->sock = network_socket_create(host, port);
    if (!conn->sock) {
        connection_count--;
        pthread_mutex_unlock(&connections_mutex);
        return NULL;
    }
    conn->in_use = 1;
    pthread_mutex_unlock(&connections_mutex);
    return conn;
}

// Release a storage connection back to the pool
static void release_storage_connection(StorageConnection *conn) {
    pthread_mutex_lock(&conn->lock);
    conn->in_use = 0;
    pthread_mutex_unlock(&conn->lock);
}

// Select a storage server using load balancing
static ErrorCode select_storage_server(char *host, char *port) {
    StorageServer *servers = NULL;
    int server_count = 0;
    ErrorCode err = health_get_servers(&servers, &server_count);
    if (err != ERR_SUCCESS || server_count == 0) {
        if (servers) free(servers);
        return ERR_NOT_FOUND;
    }

    // Simple load balancing: select the server with the lowest load
    int min_load = -1;
    int selected_index = -1;
    for (int i = 0; i < server_count; i++) {
        if (servers[i].active) {
            if (min_load == -1 || servers[i].load < min_load) {
                min_load = servers[i].load;
                selected_index = i;
            }
        }
    }

    if (selected_index == -1) {
        free(servers);
        return ERR_NOT_FOUND;
    }

    strncpy(host, servers[selected_index].host, sizeof(servers[selected_index].host) - 1);
    strncpy(port, servers[selected_index].port, sizeof(servers[selected_index].port) - 1);

    free(servers);
    return ERR_SUCCESS;
}

// Forward a client request to a storage server
ErrorCode router_forward_request(NetworkSocket *client_sock, MessageHeader *header) {
    ErrorCode err;
    char host[256], port[32];

    // Select a storage server
    err = select_storage_server(host, port);
    if (err != ERR_SUCCESS) {
        send_error_response(client_sock, err);
        return err;
    }

    // Get a connection to the storage server
    StorageConnection *storage_conn = get_storage_connection(host, port);
    if (!storage_conn) {
        send_error_response(client_sock, ERR_NETWORK_FAILURE);
        return ERR_NETWORK_FAILURE;
    }

    // Forward the request header
    ssize_t sent = network_socket_send(storage_conn->sock, header, sizeof(MessageHeader));
    if (sent != sizeof(MessageHeader)) {
        release_storage_connection(storage_conn);
        send_error_response(client_sock, ERR_NETWORK_FAILURE);
        return ERR_NETWORK_FAILURE;
    }

    // Forward the request and response based on the message type
    switch (header->type) {
        case MSG_TYPE_READ: {
            // Forward ReadRequest
            ReadRequest request;
            ssize_t received = network_socket_receive(client_sock, &request, sizeof(ReadRequest));
            if (received != sizeof(ReadRequest)) {
                release_storage_connection(storage_conn);
                return ERR_PROTOCOL_ERROR;
            }
            sent = network_socket_send(storage_conn->sock, &request, sizeof(ReadRequest));
            if (sent != sizeof(ReadRequest)) {
                release_storage_connection(storage_conn);
                return ERR_NETWORK_FAILURE;
            }

            // Relay response
            MessageHeader response_header;
            received = network_socket_receive(storage_conn->sock, &response_header, sizeof(MessageHeader));
            if (received != sizeof(MessageHeader)) {
                release_storage_connection(storage_conn);
                return ERR_NETWORK_FAILURE;
            }
            sent = network_socket_send(client_sock, &response_header, sizeof(MessageHeader));
            if (sent != sizeof(MessageHeader)) {
                release_storage_connection(storage_conn);
                return ERR_NETWORK_FAILURE;
            }

            // Relay data
            if (response_header.payload_size > 0) {
                uint8_t buffer[4096];
                size_t total_received = 0;
                while (total_received < response_header.payload_size) {
                    size_t to_receive = sizeof(buffer);
                    if (response_header.payload_size - total_received < to_receive) {
                        to_receive = response_header.payload_size - total_received;
                    }
                    received = network_socket_receive(storage_conn->sock, buffer, to_receive);
                    if (received <= 0) {
                        release_storage_connection(storage_conn);
                        return ERR_NETWORK_FAILURE;
                    }
                    sent = network_socket_send(client_sock, buffer, received);
                    if (sent != received) {
                        release_storage_connection(storage_conn);
                        return ERR_NETWORK_FAILURE;
                    }
                    total_received += received;
                }
            }
            break;
        }
        case MSG_TYPE_WRITE: {
            // Forward WriteRequest
            WriteRequest request;
            ssize_t received = network_socket_receive(client_sock, &request, sizeof(WriteRequest));
            if (received != sizeof(WriteRequest)) {
                release_storage_connection(storage_conn);
                return ERR_PROTOCOL_ERROR;
            }
            uint8_t *data = malloc(request.length);
            if (!data) {
                release_storage_connection(storage_conn);
                return ERR_INTERNAL_ERROR;
            }
            received = network_socket_receive(client_sock, data, request.length);
            if (received != request.length) {
                free(data);
                release_storage_connection(storage_conn);
                return ERR_PROTOCOL_ERROR;
            }
            sent = network_socket_send(storage_conn->sock, &request, sizeof(WriteRequest));
            if (sent != sizeof(WriteRequest)) {
                free(data);
                release_storage_connection(storage_conn);
                return ERR_NETWORK_FAILURE;
            }
            sent = network_socket_send(storage_conn->sock, data, request.length);
            if (sent != request.length) {
                free(data);
                release_storage_connection(storage_conn);
                return ERR_NETWORK_FAILURE;
            }
            free(data);

            // Relay response
            ErrorCode response_code;
            received = network_socket_receive(storage_conn->sock, &response_code, sizeof(ErrorCode));
            if (received != sizeof(ErrorCode)) {
                release_storage_connection(storage_conn);
                return ERR_NETWORK_FAILURE;
            }
            sent = network_socket_send(client_sock, &response_code, sizeof(ErrorCode));
            if (sent != sizeof(ErrorCode)) {
                release_storage_connection(storage_conn);
                return ERR_NETWORK_FAILURE;
            }
            break;
        }
        default:
            release_storage_connection(storage_conn);
            return ERR_PROTOCOL_ERROR;
    }

    // Release the connection back to the pool
    release_storage_connection(storage_conn);

    return ERR_SUCCESS;
}

// Function to send an error response to the client
static void send_error_response(NetworkSocket *client_sock, ErrorCode code) {
    MessageHeader response = {.type = MSG_TYPE_ERROR, .payload_size = sizeof(ErrorCode)};
    network_socket_send(client_sock, &response, sizeof(response));
    network_socket_send(client_sock, &code, sizeof(code));
}
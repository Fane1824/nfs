// src/naming_server/src/main.c
#include "directory.h"
#include "cache.h"
#include "network.h"
#include "protocol.h"
#include "health.h"
#include "router.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <errno.h>
#include <fcntl.h>

#define DEFAULT_CACHE_SIZE 1024
#define MAX_CLIENTS 100

// typedef struct {
//     char ip[INET_ADDRSTRLEN];
//     uint16_t port;
//     uint32_t num_paths;
//     char **paths;
// } StorageServerInfo;

// static StorageServerInfo *storage_servers = NULL;
// static size_t ss_count = 0;
// static pthread_mutex_t ss_mutex = PTHREAD_MUTEX_INITIALIZER;

static volatile int running = 1;
static NetworkSocket *server_sock = NULL;
static pthread_t thread_ids[MAX_CLIENTS];
static int thread_count = 0;
static pthread_mutex_t thread_mutex = PTHREAD_MUTEX_INITIALIZER;

static void handle_signal(int sig) {
    printf("\nReceived signal %d, shutting down...\n", sig);
    running = 0;
    
    // Force socket shutdown to break accept
    if (server_sock) {
        shutdown(network_socket_get_fd(server_sock), SHUT_RDWR);
        close(network_socket_get_fd(server_sock));
    }

    // Wait for threads to finish
    pthread_mutex_lock(&thread_mutex);
    for (int i = 0; i < thread_count; i++) {
        pthread_cancel(thread_ids[i]);
        pthread_join(thread_ids[i], NULL);
    }
    pthread_mutex_unlock(&thread_mutex);
}

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s [OPTIONS]\n"
            "Options:\n"
            "  -p, --port PORT       Port to listen on (required)\n"
            "  -c, --cache-size N    Cache size in entries (default: 1024)\n"
            "  -h, --help            Show this help\n", prog);
}

// static void handle_client_connection(NetworkSocket *client_sock) {
//     MessageHeader header;
//     ssize_t received = network_socket_receive(client_sock, &header, sizeof(header));
//     if (received != sizeof(header)) {
//         return;
//     }

//     switch (header.type) {
//         case MSG_TYPE_GET_LOCATION: {
//             // // TODO: Implement storage server selection logic
//             // struct {
//             //     char host[256];
//             //     char port[32];
//             // } response = {"localhost", "9000"}; // For now just return default
//             // network_socket_send(client_sock, &response, sizeof(response));
//             // break;
//             char host[256], port[32];
//             if (get_active_server(host, port) == ERR_SUCCESS) {
//                 struct {
//                     char host[256];
//                     char port[32];
//                 } response;
//                 strncpy(response.host, host, sizeof(response.host) - 1);
//                 strncpy(response.port, port, sizeof(response.port) - 1);
//                 network_socket_send(client_sock, &response, sizeof(response));
//             }
//             else {
//                 ErrorCode error = ERR_NOT_FOUND;
//                 network_socket_send(client_sock, &error, sizeof(error));
//             }
//             break;
//         }
//         case MSG_TYPE_CREATE:
//         case MSG_TYPE_DELETE: {
//             char filepath[256];
//             received = network_socket_receive(client_sock, filepath, sizeof(filepath));
//             if (received <= 0) break;
            
//             ErrorCode result = (header.type == MSG_TYPE_CREATE) ? 
//                                 directory_create(filepath) :
//                                 directory_delete(filepath);
            
//             network_socket_send(client_sock, &result, sizeof(result));
//             break;
//         }
//         case MSG_TYPE_HEARTBEAT: {
//             HeartbeatMessage hb;
//             received = network_socket_receive(client_sock, &hb, sizeof(hb));
//             if (received == sizeof(hb)) {
//                 health_receive_heartbeat(hb.host, hb.port, hb.load);
//             } else {
//                 fprintf(stderr, "Failed to receive heartbeat message\n");
//             }
//             break;
//         }
//         default:
//             break;
//     }
// }

void handle_client_request(NetworkSocket *sock, MessageHeader *header) {
    uint32_t request_id = header->request_id;

    // Receive the payload (file path)
    uint32_t payload_size = ntohl(header->payload_size);
    char *path = malloc(payload_size + 1);
    if (network_socket_receive(sock, path, payload_size) != payload_size) {
        fprintf(stderr, "Failed to receive path\n");
        free(path);
        return;
    }
    path[payload_size] = '\0';
    printf("path: %s\n", path); //!debug
    fflush(stdout);

    // Lookup the directory entry
    DirectoryEntry *entry = NULL;
    ErrorCode err = directory_lookup(path, &entry);
    printf("dir lookup response: %d\n", err); //!debug
    if (err == ERR_SUCCESS && entry != NULL && entry->metadata != NULL) {
        FileMetadata *metadata = entry->metadata;

        // Prepare response header
        MessageHeader resp_header = {
            .request_id = request_id,
            .type = MSG_TYPE_LOCATION,
            .payload_size = htonl(INET_ADDRSTRLEN + sizeof(uint16_t))
        };

        // Send response header
        printf("Sending response of size %d\n", resp_header.payload_size); //!debug
        network_socket_send(sock, &resp_header, sizeof(resp_header));

        // Send Storage Server IP and port
        network_socket_send(sock, metadata->storage_server_ip, INET_ADDRSTRLEN);
        uint16_t port_net = htons(metadata->storage_server_port);
        network_socket_send(sock, &port_net, sizeof(port_net));

        printf("Provided storage server info for path: %d\n", metadata->storage_server_port); //!debug
    } else {
        // Send error response
        MessageHeader err_header;
        err_header.request_id = request_id;
        err_header.type = MSG_TYPE_ERROR;
        err_header.payload_size = htonl(sizeof(uint32_t));

        network_socket_send(sock, &err_header, sizeof(err_header));

        uint32_t error_code = htonl(ERR_FILE_NOT_FOUND);
        network_socket_send(sock, &error_code, sizeof(error_code));

        fprintf(stderr, "File not found: %s\n", path);
    }

    free(path);
}

void handle_storage_server_registration(NetworkSocket *sock, MessageHeader *header, const char *ip) {
    uint32_t request_id = header->request_id;

    SSRegisterMessage reg_msg;
    if (network_socket_receive(sock, &reg_msg, sizeof(reg_msg)) != sizeof(reg_msg)) {
        fprintf(stderr, "Failed to receive registration message\n");
        return;
    }

    reg_msg.port = ntohs(reg_msg.port);
    reg_msg.num_paths = ntohl(reg_msg.num_paths);

    printf("Received registration from %s:%d with %d paths\n", ip, reg_msg.port, reg_msg.num_paths);

    // Receive the paths
    // char **paths = malloc(reg_msg.num_paths * sizeof(char *));
    for (uint32_t i = 0; i < reg_msg.num_paths; i++) {
        printf("Receiving path %d\n", i); //!debug
        uint32_t path_len_net;
        if (network_socket_receive(sock, &path_len_net, sizeof(path_len_net)) != sizeof(path_len_net)) {
            fprintf(stderr, "Failed to receive path length\n");
            return;
        }
        uint32_t path_len = ntohl(path_len_net);
        char *path = malloc(path_len+1);
        if (!path) {
            fprintf(stderr, "Memory allocation failed\n");
            return;
        }
        if (network_socket_receive(sock, path, path_len) != path_len) {
            fprintf(stderr, "Failed to receive path\n");
            free(path);
            return;
        }
        path[path_len] = '\0';
        printf("Received path: %s\n", path); //!debug

        // Create or update the directory entry
        FileMetadata *metadata = malloc(sizeof(FileMetadata));
        metadata->storage_server_ip = strdup(ip);
        metadata->storage_server_port = reg_msg.port;
        // Initialize other metadata fields if necessary
        printf("Registering path: %s\n", path); //!debug

        ErrorCode err = directory_register_file(path, metadata);
        if (err != ERR_SUCCESS) {
            fprintf(stderr, "Failed to register path: %s\n", path);
            // Handle error if needed
        }

        printf("Done registering path: %s\n", path); //! debug
        free(path);
    }

    // Send acknowledgment
    MessageHeader ack_header = {
        .request_id = header->request_id,
        .type = MSG_TYPE_SS_REGISTER_ACK,
        .payload_size = 0
    };
    if (network_socket_send(sock, &ack_header, sizeof(ack_header)) != sizeof(ack_header)) {
        fprintf(stderr, "Failed to send ack to storage server\n");
        return;
    }
    printf("request_id: %d\n", request_id); //!debug
    printf("Registered Storage Server %s:%d\n", ip, reg_msg.port);
}

void handle_heartbeat(NetworkSocket *sock, MessageHeader *header, const char *ip) {
    // Receive the heartbeat message
    HeartbeatMessage hb;
    if (network_socket_receive(sock, &hb, sizeof(hb)) != sizeof(hb)) {
        fprintf(stderr, "Failed to receive heartbeat message from storage server %s\n", ip);
        return;
    }

    printf("Received heartbeat from %s:%s\n", hb.host, hb.port);

    // Update the health of the storage server
    health_receive_heartbeat(hb.host, hb.port, hb.load);
}

void *client_handler(void *arg) {
    NetworkSocket *client_sock = (NetworkSocket *)arg;
    char client_ip[INET_ADDRSTRLEN] = "Unknown";

    // Retrieve client IP address
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    if (getpeername(network_socket_get_fd(client_sock), (struct sockaddr *)&addr, &addr_len) == 0) {
        inet_ntop(AF_INET, &addr.sin_addr, client_ip, INET_ADDRSTRLEN);
    }

    while (running) {
        MessageHeader header;
        ssize_t rc = network_socket_receive(client_sock, &header, sizeof(header));
        if (rc != sizeof(header)) {
            // Error or client closed connection
            break;
        }

        switch (header.type) {
            case MSG_TYPE_GET_LOCATION:
                handle_client_request(client_sock, &header);
                break;
            case MSG_TYPE_SS_REGISTER:
                handle_storage_server_registration(client_sock, &header, client_ip);
                break;
            case MSG_TYPE_HEARTBEAT:
                handle_heartbeat(client_sock, &header, client_ip);
                break;
            default:
                // Handle unknown message types
                break;
        }
    }

    network_socket_close(client_sock);
    return NULL;
}

int main(int argc, char *argv[]) {
    char *port = NULL;
    size_t cache_size = DEFAULT_CACHE_SIZE;

    // Parse command line options
    static struct option long_options[] = {
        {"port", required_argument, 0, 'p'},
        {"cache-size", required_argument, 0, 'c'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "p:c:h", long_options, NULL)) != -1) {
        switch (opt) {
            case 'p':
                port = optarg;
                break;
            case 'c':
                cache_size = atoi(optarg);
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    if (!port) {
        fprintf(stderr, "Error: Port must be specified\n");
        print_usage(argv[0]);
        return 1;
    }

    // Set up signal handlers
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    // Initialize subsystems
    if (directory_init() != ERR_SUCCESS) {
        fprintf(stderr, "Failed to initialize directory manager\n");
        return 1;
    }

    if (cache_init(cache_size) != ERR_SUCCESS) {
        fprintf(stderr, "Failed to initialize cache\n");
        directory_cleanup();
        return 1;
    }

    // Create server socket
    server_sock = network_socket_create(NULL, port);
    if (!server_sock) {
        fprintf(stderr, "Failed to create server socket\n");
        cache_cleanup();
        directory_cleanup();
        return 1;
    }
    
    // Start health monitoring
    health_init();

    // Start router
    router_init();

    printf("Naming server started on port %s\n", port);

    // Main server loop
    while (running) {
        NetworkSocket *client_sock = network_socket_accept(server_sock);
        if (!client_sock) {
            if (!running) break;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(100000); // Sleep 100ms
                continue;
            }
            fprintf(stderr, "Failed to accept client connection\n");
            continue;
        }

        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, client_handler, client_sock) != 0) {
            fprintf(stderr, "Failed to create thread\n");
            network_socket_close(client_sock);
        } else {
            pthread_mutex_lock(&thread_mutex);
            if (thread_count < MAX_CLIENTS) {
                thread_ids[thread_count++] = thread_id;
            }
            pthread_mutex_unlock(&thread_mutex);
            pthread_detach(thread_id);
        }
    }

    // Cleanup
    network_socket_close(server_sock);
    printf("Socket closed\n");
    cache_cleanup();
    printf("Cache cleaned up\n");
    // directory_cleanup();
    // printf("Directory cleaned up\n");
    health_cleanup();
    printf("Health monitoring cleaned up\n");
    router_cleanup();
    printf("Naming server shut down cleanly\n");
    return 0;
}
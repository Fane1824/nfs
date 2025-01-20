// src/storage_server/src/main.c
#include "storage.h"
#include "replication.h"
#include "protocol.h"
#include "network.h"
#include "heartbeat.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <dirent.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <errno.h>
#include <fcntl.h>

#define MAX_BUFFER_SIZE 4096

static volatile int running = 1;
static NetworkSocket *client_sock = NULL;
static NetworkSocket *ns_sock = NULL;
static char *server_data_dir = NULL;

static void handle_signal(int sig) {
    printf("\nReceived signal %d, shutting down...\n", sig);
    running = 0;
    // Force socket shutdown to break accept
    if (client_sock) {
        shutdown(network_socket_get_fd(client_sock), SHUT_RDWR);
        close(network_socket_get_fd(client_sock));
    }
}

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s [OPTIONS]\n"
            "Options:\n"
            "  -p, --port PORT             Port for client connections\n"
            "  -n, --ns-host HOST          Naming server host\n"
            "  -N, --ns-port PORT          Naming server port\n"
            "  -d, --data-dir DIR          Data directory path\n"
            "  -b, --backup HOST:PORT      Backup server (can be specified multiple times)\n"
            "  -h, --help                  Show this help\n", prog);
}

// Helper to send error response
static void send_error_response(NetworkSocket *sock, ErrorCode code) {
    MessageHeader response = {.type = MSG_TYPE_ERROR};
    network_socket_send(sock, &response, sizeof(response));
    network_socket_send(sock, &code, sizeof(code));
}

// Stream callback for forwarding data to client
static void stream_to_client(const uint8_t *data, size_t length, void *user_data) {
    NetworkSocket *sock = (NetworkSocket *)user_data;
    network_socket_send(sock, data, length);
}

static void handle_client_request(NetworkSocket *sock) {
    MessageHeader header;
    ssize_t received = network_socket_receive(sock, &header, sizeof(header));
    if (received != sizeof(header)) return;

    printf("Received message type: %d\n", header.type);

    switch (header.type) {
        case MSG_TYPE_READ: {
            ReadRequest request;
            printf("Size of request: %ld\n", sizeof(request)); //!debug
            received = network_socket_receive(sock, &request, sizeof(request));
            if (received != sizeof(request)) {
                printf("Failed to receive complete ReadRequest. Received: %ld bytes\n", received);
                break;
            }
            request.length = ntohl(request.length);
            request.offset = ntohl(request.offset);
            printf("ReadRequest - Filepath: %s, Offset: %lu, Length: %zu\n", 
                   request.filepath, request.offset, request.length);


            uint8_t buffer[MAX_BUFFER_SIZE];
            size_t bytes_read;
            // Prepend server_data_dir to the filepath
            char full_filepath[256];
            snprintf(full_filepath, sizeof(full_filepath), "%s/%s", server_data_dir, request.filepath);
            
            // Update the storage_read function call with the full filepath
            ErrorCode result = storage_read(full_filepath, request.offset, buffer, request.length, &bytes_read);
            printf("storage_read result: %d, bytes_read: %zu\n", result, bytes_read);

            if (result == ERR_SUCCESS) {
                MessageHeader response = {.type = MSG_TYPE_READ};
                network_socket_send(sock, &response, sizeof(response));
                network_socket_send(sock, buffer, bytes_read);
                printf("Read response sent successfully\n");
            } else {
                printf("Read error occurred: %d\n", result);
                send_error_response(sock, result);
            }
            break;

        }

        case MSG_TYPE_WRITE: {
            WriteRequest request;
            received = network_socket_receive(sock, &request, sizeof(request));
            if (received != sizeof(request)) {
                printf("Failed to receive complete WriteRequest. Received: %ld bytes\n", received);
                break;
            }

            request.length = ntohl(request.length);
            request.offset = ntohl(request.offset);
            
            uint8_t *buffer = malloc(request.length);
            if (!buffer) {
                printf("Failed to allocate write buffer\n");
                send_error_response(sock, ERR_INTERNAL_ERROR);
                break;
            }

            printf("debug\n");

            received = network_socket_receive(sock, buffer, request.length);
            if (received != request.length) {
                printf("Failed to receive write data. Expected: %u, Received: %ld\n", request.length, received);
                free(buffer);
                send_error_response(sock, ERR_NETWORK_FAILURE);
                break;
            }

            // Prepend server_data_dir
            char full_filepath[256];
            snprintf(full_filepath, sizeof(full_filepath), "%s/%s", server_data_dir, request.filepath);

            ErrorCode result = storage_write(full_filepath, request.offset, buffer, request.length);
            printf("storage_write result: %d\n", result);

            if (result == ERR_SUCCESS) {
                MessageHeader response = {.type = MSG_TYPE_WRITE};
                network_socket_send(sock, &response, sizeof(response));
                printf("Write response sent successfully\n");
            } else {
                printf("Write error occurred: %d\n", result);
                send_error_response(sock, result);
            }

            free(buffer);
            break;
        }

        case MSG_TYPE_STREAM: {
            StreamRequest request;
            received = network_socket_receive(sock, &request, sizeof(request));
            if (received != sizeof(request)) {
                printf("Failed to receive complete StreamRequest\n");
                send_error_response(sock, ERR_PROTOCOL_ERROR);
                break;
            }

            // Construct full filepath
            char full_filepath[512];
            snprintf(full_filepath, sizeof(full_filepath), "%s/%s", 
                     server_data_dir, request.filepath);

            // Send success response header
            MessageHeader response = {
                .type = MSG_TYPE_STREAM,
                .request_id = request.header.request_id,
                .payload_size = 0
            };
            network_socket_send(sock, &response, sizeof(response));

            // Stream callback that sends data to client
            void stream_to_client(const uint8_t *data, size_t length, void *user_data) {
                NetworkSocket *client_sock = (NetworkSocket *)user_data;
                network_socket_send(client_sock, data, length);
            }

            // Start streaming
            ErrorCode result = storage_stream(full_filepath, stream_to_client, sock);
            
            if (result != ERR_SUCCESS) {
                send_error_response(sock, result);
            }
            break;
        }

        case MSG_TYPE_REPLICATE_WRITE: {
            WriteRequest request;
            received = network_socket_receive(sock, &request, sizeof(request));
            if (received != sizeof(request)) {
                printf("Failed to receive complete Replicate WriteRequest. Received: %ld bytes\n", received);
                break;
            }

            request.length = ntohl(request.length);
            request.offset = ntohl(request.offset);

            uint8_t *buffer = malloc(request.length);
            if (!buffer) {
                printf("Failed to allocate replicate write buffer\n");
                break;
            }

            received = network_socket_receive(sock, buffer, request.length);
            if (received != request.length) {
                printf("Failed to receive replicate write data. Expected: %u, Received: %ld\n", request.length, received);
                free(buffer);
                break;
            }

            // Prepend server_data_dir
            char full_filepath[256];
            snprintf(full_filepath, sizeof(full_filepath), "%s/%s", server_data_dir, request.filepath);

            ErrorCode result = storage_write(full_filepath, request.offset, buffer, request.length);
            printf("Replicate storage_write result: %d\n", result);

            free(buffer);
            break;
        }

        case MSG_TYPE_REPLICATE_DELETE: {
            DeleteRequest request;
            received = network_socket_receive(sock, &request, sizeof(request));
            if (received != sizeof(request)) {
                printf("Failed to receive complete Replicate DeleteRequest. Received: %ld bytes\n", received);
                break;
            }

            // Prepend server_data_dir
            char full_filepath[256];
            snprintf(full_filepath, sizeof(full_filepath), "%s/%s", server_data_dir, request.filepath);

            ErrorCode result = storage_delete_file(full_filepath);
            if (result == ERR_SUCCESS) {
                printf("Replicated delete successful for file: %s\n", full_filepath);
            } else {
                printf("Replicated delete failed for file: %s with error code: %d\n", full_filepath, result);
            }
            break;
        }

        case MSG_TYPE_DELETE: {
            DeleteRequest request;
            received = network_socket_receive(sock, &request, sizeof(request));
            if (received != sizeof(request)) {
                printf("Failed to receive complete DeleteRequest. Received: %ld bytes\n", received);
                break;
            }

            // Prepend server_data_dir
            char full_filepath[256];
            snprintf(full_filepath, sizeof(full_filepath), "%s/%s", server_data_dir, request.filepath);

            ErrorCode result = storage_delete_file(full_filepath);
            if (result == ERR_SUCCESS) {
                MessageHeader response = {.type = MSG_TYPE_DELETE};
                network_socket_send(sock, &response, sizeof(response));
                printf("Delete response sent successfully\n");
            } else {
                printf("Delete error occurred: %d\n", result);
                send_error_response(sock, result);
            }
            break;
        }

        case MSG_TYPE_GET_FILE_INFO: {
            GetFileInfoRequest request;
            ssize_t received = network_socket_receive(sock, &request, sizeof(request));
            if (received != sizeof(request)) {
                send_error_response(sock, ERR_PROTOCOL_ERROR);
                break;
            }

            char filepath[512];
            snprintf(filepath, sizeof(filepath), "%s/%s", server_data_dir, request.filepath);

            uint64_t file_size;
            uint32_t permissions;
            ErrorCode err = storage_get_file_info(filepath, &file_size, &permissions);

            if (err != ERR_SUCCESS) {
                send_error_response(sock, err);
                break;
            }

            MessageHeader response_header = {
                .request_id = header.request_id,
                .type = MSG_TYPE_GET_FILE_INFO_RESPONSE,
                .payload_size = htonl(sizeof(GetFileInfoResponse))
            };

            GetFileInfoResponse response;
            response.file_size = htonl(file_size);
            response.permissions = htonl(permissions);

            network_socket_send(sock, &response_header, sizeof(response_header));
            network_socket_send(sock, &response, sizeof(response));
            break;
        }

        default:
            // Unknown message type
            send_error_response(sock, ERR_PROTOCOL_ERROR);
            break;
    }
}

static ErrorCode register_with_naming_server(const char *host, const char *port, const char *data_dir, uint16_t client_port) {
    printf("Attempting to register with naming server...\n");

    ns_sock = network_socket_create(host, port);
    if (!ns_sock){
        fprintf(stderr, "Failed to connect to Naming Server at %s:%s\n", host, port);
        return ERR_NETWORK_FAILURE;
    }

    printf("Connected to naming server, scanning directory %s...\n", data_dir);

    // Prepare the list of accessible paths
    DIR *dir = opendir(data_dir);
    if (!dir) {
        perror("opendir");
        network_socket_close(ns_sock);
        return ERR_INTERNAL_ERROR;
    }

    char **paths = NULL;
    uint32_t num_paths = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.')
            continue; // Skip hidden files
        printf("Found file: %s\n", entry->d_name); //!debug

        if (strlen(entry->d_name) > 255) {
            fprintf(stderr, "Path too long: %s\n", entry->d_name);
            continue;
        }

        paths = realloc(paths, (num_paths + 1) * sizeof(char *));
        if (!paths) {
            fprintf(stderr, "Memory allocation failed\n");
            closedir(dir);
            network_socket_close(ns_sock);
            return ERR_INTERNAL_ERROR;
        }
        
        paths[num_paths] = strdup(entry->d_name);
        if (!paths[num_paths]) {
            fprintf(stderr, "String duplication failed\n");
            closedir(dir);
            network_socket_close(ns_sock);
            return ERR_INTERNAL_ERROR;
        }
        num_paths++;
    }
    closedir(dir);

    printf("Found %d files to register\n", num_paths);

    // Initialize request_id (use a static variable)
    static uint32_t request_id_counter = 1;
    uint32_t request_id = request_id_counter++;

    // Send registration message header
    printf("Sending header to naming server...\n"); //!debug
    MessageHeader header = {request_id, MSG_TYPE_SS_REGISTER, sizeof(SSRegisterMessage)};
    if (network_socket_send(ns_sock, &header, sizeof(header)) != sizeof(header)) {
        fprintf(stderr, "Failed to send header to Naming Server\n");
        network_socket_close(ns_sock);
        return ERR_NETWORK_FAILURE;
    }

    // Send registration message
    printf("Sending registration message...\n"); //!debug
    SSRegisterMessage reg_msg = {htons(client_port), htonl(num_paths)};
    if (network_socket_send(ns_sock, &reg_msg, sizeof(reg_msg)) != sizeof(reg_msg)) {
        fprintf(stderr, "Failed to send registration message to Naming Server\n");
        network_socket_close(ns_sock);
        return -1;
    }

    // Send the paths
    printf("Sending %d paths...\n", num_paths); //!debug
    for (uint32_t i = 0; i < num_paths; i++) {
        uint32_t path_len = htonl(strlen(paths[i]) + 1);
        printf("Sending path %d: %s (len=%d)\n", i, paths[i], ntohl(path_len)); //!debug

        if (network_socket_send(ns_sock, &path_len, sizeof(path_len)) != sizeof(path_len)) {
            fprintf(stderr, "Failed to send path length\n");
            network_socket_close(ns_sock);
            return -1;
        }
        if (network_socket_send(ns_sock, paths[i], strlen(paths[i]) + 1) != strlen(paths[i]) + 1) {
            fprintf(stderr, "Failed to send path\n");
            network_socket_close(ns_sock);
            return -1;
        }
        free(paths[i]);
    }
    free(paths);

    // Receive acknowledgment
    printf("Waiting for acknowledgment...\n"); //!debug
    MessageHeader ack_header;
    int rec = network_socket_receive(ns_sock, &ack_header, sizeof(ack_header));
    if (rec != sizeof(ack_header)) {
        fprintf(stderr, "Failed to receive acknowledgment\n");
        printf("Size of ack_header: %ld\n", sizeof(ack_header)); //!debug
        printf("Size of received: %ld\n", rec); //!debug
        network_socket_close(ns_sock);
        return -1;
    }
    if (ack_header.request_id != request_id || ack_header.type != MSG_TYPE_SS_REGISTER_ACK) {
        fprintf(stderr, "Invalid acknowledgment from Naming Server\n");
        network_socket_close(ns_sock);
        return -1;
    }

    printf("Successfully registered with Naming Server\n");
    network_socket_close(ns_sock);
    return ERR_SUCCESS;
}

int main(int argc, char *argv[]) {
    char *port = NULL;
    char *ns_host = NULL;
    char *ns_port = NULL;
    char *data_dir = NULL;
    char *backup_servers[10] = {NULL};
    int backup_count = 0;

    static struct option long_options[] = {
        {"port", required_argument, 0, 'p'},
        {"ns-host", required_argument, 0, 'n'},
        {"ns-port", required_argument, 0, 'N'},
        {"data-dir", required_argument, 0, 'd'},
        {"backup", required_argument, 0, 'b'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "p:n:N:d:b:h", long_options, NULL)) != -1) {
        switch (opt) {
            case 'p':
                port = optarg;
                break;
            case 'n':
                ns_host = optarg;
                break;
            case 'N':
                ns_port = optarg;
                break;
            case 'd':
                data_dir = optarg;
                break;
            case 'b':
                if (backup_count < 10) {
                    backup_servers[backup_count++] = optarg;
                }
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    if (!port || !ns_host || !ns_port || !data_dir) {
        fprintf(stderr, "Error: Required parameters missing\n");
        print_usage(argv[0]);
        return 1;
    }

    server_data_dir = data_dir;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    // Initialize storage system
    if (storage_init() != ERR_SUCCESS) {
        fprintf(stderr, "Failed to initialize storage\n");
        return 1;
    }

    // Initialize replication system
    if (replication_init() != ERR_SUCCESS) {
        fprintf(stderr, "Failed to initialize replication\n");
        storage_cleanup();
        return 1;
    }

    // Set up backup servers
    for (int i = 0; i < backup_count; i++) {
        char *host = strdup(backup_servers[i]);
        char *port = strchr(host, ':');
        if (port) {
            *port++ = '\0';
            replication_add_secondary(host, port);
        }
        free(host);
    }

    // Register with naming server
    if (register_with_naming_server(ns_host, ns_port, data_dir, atoi(port)) != ERR_SUCCESS) {
        fprintf(stderr, "Failed to register with naming server\n");
        goto cleanup;
    }

    // Start heartbeat thread
    start_heartbeat(ns_host, ns_port, "localhost", port);

    // Create client socket
    client_sock = network_socket_create(NULL, port);
    if (!client_sock) {
        fprintf(stderr, "Failed to create client socket\n");
        goto cleanup;
    }

    printf("Storage server started on port %s\n", port);
    printf("Connected to naming server at %s:%s\n", ns_host, ns_port);
    printf("Using data directory: %s\n", data_dir);

    // Main server loop
    while (running) {
        NetworkSocket *conn = network_socket_accept(client_sock);
        if (!conn) {
            if (!running) break;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(100000); // Sleep 100ms
                continue;
            }
            fprintf(stderr, "Accept failed\n");
            continue;
        }
        handle_client_request(conn);
        network_socket_close(conn);
    }

cleanup:
    if (client_sock) network_socket_close(client_sock);
    printf("Client socket closed\n");
    if (ns_sock) network_socket_close(ns_sock);
    printf("Naming server socket closed\n");
    replication_cleanup();
    printf("Replication system shut down\n");
    storage_cleanup();
    printf("Storage server shut down cleanly\n");
    return 0;
}
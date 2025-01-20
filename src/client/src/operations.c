#include "client.h"
#include "network.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <stdio.h> //! debug
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>

uint32_t generate_request_id(Client *client) {
    static uint32_t request_counter = 1;
    int request_id;
    pthread_mutex_lock(&client->mutex);
    request_id = request_counter++;
    pthread_mutex_unlock(&client->mutex);
    return request_id;
}

// Internal function to connect to naming server
static ErrorCode connect_to_naming_server(Client *client, const char *host, const char *port) {
    client->naming_server_sock = network_socket_create(host, port);
    if (!client->naming_server_sock) {
        return ERR_NETWORK_FAILURE;
    }
    return ERR_SUCCESS;
}

// Helper function to get storage server info
static ErrorCode get_storage_server(Client *client, const char *filepath, char *host, char *port) {
    // Prepare location request
    MessageHeader request = {
        .request_id = generate_request_id(client),
        .type = MSG_TYPE_GET_LOCATION,
        // .payload_size = htonl(strlen(filepath) + 1)
    };
    // request.request_id = generate_request_id(client);
    // request.type = MSG_TYPE_GET_LOCATION;
    uint32_t path_len = strlen(filepath) + 1; // Include null terminator
    request.payload_size = htonl(path_len);

    pthread_mutex_lock(&client->mutex);

    // Send the header
    ssize_t sent = network_socket_send(client->naming_server_sock, &request, sizeof(request));
    if (sent != sizeof(request)) {
        pthread_mutex_unlock(&client->mutex);
        return ERR_NETWORK_FAILURE;
    }

    // Send the file path
    sent = network_socket_send(client->naming_server_sock, filepath, path_len);
    if (sent != path_len) {
        pthread_mutex_unlock(&client->mutex);
        return ERR_NETWORK_FAILURE;
    }

    // Receive the response header
    MessageHeader response_header;
    ssize_t received = network_socket_receive(client->naming_server_sock, &response_header, sizeof(response_header));
    if (received != sizeof(response_header)) {
        pthread_mutex_unlock(&client->mutex);
        return ERR_NETWORK_FAILURE;
    }
    uint32_t payload_size = ntohl(response_header.payload_size);

    // Check for error response
    if (response_header.type == MSG_TYPE_ERROR) {
        ErrorCode error_code;
        received = network_socket_receive(client->naming_server_sock, &error_code, sizeof(error_code));
        pthread_mutex_unlock(&client->mutex);
        if (received != sizeof(error_code))
            return ERR_NETWORK_FAILURE;
        return error_code;
    }

    
    // Receive storage server IP and port
    char response_host[32];
    received = network_socket_receive(client->naming_server_sock, response_host, INET_ADDRSTRLEN);
    if (received != INET_ADDRSTRLEN) {
        pthread_mutex_unlock(&client->mutex);
        return ERR_NETWORK_FAILURE;
    }
    response_host[16] = '\0';

    // Receive storage server port
    uint16_t response_port_net;
    received = network_socket_receive(client->naming_server_sock, &response_port_net, sizeof(response_port_net));
    uint16_t response_port = ntohs(response_port_net);
    // printf("storage_server: %s:%d\n", response_host, response_port); //!debug
    // printf("received: %ld\nsizeof(response_port_net): %ld\n", received, sizeof(response_port_net)); //!debug
    if (received != sizeof(response_port_net)) {
        pthread_mutex_unlock(&client->mutex);
        return ERR_NETWORK_FAILURE;
    }

    pthread_mutex_unlock(&client->mutex);

    // Copy the received host and port
    strcpy(host, response_host);
    sprintf(port, "%d", response_port);

    return ERR_SUCCESS;
}

// Helper to ensure storage server connection
static ErrorCode ensure_storage_connection(Client *client, const char *filepath) {
    // if (client->storage_server_sock)
    //     return ERR_SUCCESS;

    char host[256], port[32];
    ErrorCode err = get_storage_server(client, filepath, host, port);
    if (err != ERR_SUCCESS)
        return err;

    client->storage_server_sock = network_socket_create(host, port);
    if (!client->storage_server_sock)
        return ERR_NETWORK_FAILURE;

    return ERR_SUCCESS;
}

// Initialize the client library
ErrorCode client_init(Client **client, const char *naming_server_host, const char *naming_server_port) {
    if (!client) return ERR_INVALID_ARGUMENT;
    Client *new_client = malloc(sizeof(Client));
    if (!new_client) return ERR_INTERNAL_ERROR;

    pthread_mutex_init(&new_client->mutex, NULL);
    ErrorCode err = connect_to_naming_server(new_client, naming_server_host, naming_server_port);
    if (err != ERR_SUCCESS) {
        pthread_mutex_destroy(&new_client->mutex);
        free(new_client);
        return err;
    }

    *client = new_client;
    return ERR_SUCCESS;
}

// Clean up the client library
void client_cleanup(Client *client) {
    if (client) {
        network_socket_close(client->naming_server_sock);
        pthread_mutex_destroy(&client->mutex);
        free(client);
    }
}

// Synchronous file operations
ErrorCode client_read(Client *client, const char *filepath, uint64_t offset, uint8_t *buffer, size_t length, size_t *bytes_read) {
    if (!client || !filepath || !buffer || !bytes_read) return ERR_INVALID_ARGUMENT;

    ErrorCode err = ensure_storage_connection(client, filepath);
    if (err != ERR_SUCCESS)
        return err;

    // Prepare primary MessageHeader for READ
    MessageHeader header = {
        .type = MSG_TYPE_READ,
        .request_id = generate_request_id(client),
        .payload_size = sizeof(ReadRequest)
    };

    // Prepare ReadRequest with its own MessageHeader
    ReadRequest read_request;
    read_request.header.type = MSG_TYPE_READ;
    read_request.header.request_id = header.request_id; // Use the same request ID
    read_request.header.payload_size = htonl(sizeof(ReadRequest));
    strncpy(read_request.filepath, filepath, sizeof(read_request.filepath) - 1);
    read_request.offset = htonl(offset);
    read_request.length = htonl(length);

    // Send primary MessageHeader
    pthread_mutex_lock(&client->mutex);
    ssize_t sent = network_socket_send(client->storage_server_sock, &header, sizeof(header));
    if (sent != sizeof(header)) {
        pthread_mutex_unlock(&client->mutex);
        return ERR_NETWORK_FAILURE;
    }

    // Send ReadRequest
    sent = network_socket_send(client->storage_server_sock, &read_request, sizeof(read_request));
    if (sent != sizeof(read_request)) {
        pthread_mutex_unlock(&client->mutex);
        return ERR_NETWORK_FAILURE;
    }
    printf("Sent request with %ld bytes\n", sent); //!debug

    // Receive response header
    MessageHeader response;
    ssize_t received = network_socket_receive(client->storage_server_sock,
                                            &response, sizeof(response));
    if (received != sizeof(response)) {
        pthread_mutex_unlock(&client->mutex);
        return ERR_NETWORK_FAILURE;
    }

    // Check for error response
    if (response.type == MSG_TYPE_ERROR) {
        ErrorCode error_code;
        received = network_socket_receive(client->storage_server_sock,
                                       &error_code, sizeof(error_code));
        pthread_mutex_unlock(&client->mutex);
        return error_code;
    }

    // Receive data
    received = network_socket_receive(client->storage_server_sock, 
                                   buffer, length);
    pthread_mutex_unlock(&client->mutex);

    if (received < 0)
        return ERR_NETWORK_FAILURE;

    *bytes_read = received;
    return ERR_SUCCESS;
}

ErrorCode client_write(Client *client, const char *filepath, uint64_t offset, const uint8_t *buffer, size_t length) {
    if (!client || !filepath || !buffer) return ERR_INVALID_ARGUMENT;

    ErrorCode err = ensure_storage_connection(client, filepath);
    if (err != ERR_SUCCESS)
        return err;

    // Prepare write request
    WriteRequest request = {0};
    request.header.request_id = generate_request_id(client);
    request.header.type = MSG_TYPE_WRITE;
    strncpy(request.filepath, filepath, sizeof(request.filepath) - 1);
    request.offset = offset;
    request.length = length;

    // Send request header
    pthread_mutex_lock(&client->mutex);
    ssize_t sent = network_socket_send(client->storage_server_sock,
                                     &request, sizeof(request));
    if (sent != sizeof(request)) {
        pthread_mutex_unlock(&client->mutex);
        return ERR_NETWORK_FAILURE;
    }

    // Send data
    sent = network_socket_send(client->storage_server_sock, buffer, length);
    if (sent != (ssize_t)length) {
        pthread_mutex_unlock(&client->mutex);
        return ERR_NETWORK_FAILURE;
    }

    // Receive response
    ErrorCode response_code;
    ssize_t received = network_socket_receive(client->storage_server_sock,
                                            &response_code, sizeof(response_code));
    pthread_mutex_unlock(&client->mutex);

    if (received != sizeof(response_code))
        return ERR_NETWORK_FAILURE;

    return response_code;
}

ErrorCode client_create(Client *client, const char *filepath, uint32_t mode) {
    if (!client || !filepath) return ERR_INVALID_ARGUMENT;

    // Prepare create request
    CreateRequest request = {0};
    request.header.request_id = generate_request_id(client);
    request.header.type = MSG_TYPE_CREATE;
    strncpy(request.filepath, filepath, sizeof(request.filepath) - 1);
    request.mode = mode;

    // Send request to naming server
    pthread_mutex_lock(&client->mutex);
    ssize_t sent = network_socket_send(client->naming_server_sock, &request, sizeof(request));
    pthread_mutex_unlock(&client->mutex);
    if (sent != sizeof(request)) return ERR_NETWORK_FAILURE;

    // Receive response (error code)
    ErrorCode response_code;
    ssize_t received = network_socket_receive(client->naming_server_sock, &response_code, sizeof(response_code));
    printf("Received: %d\n", response_code); //! debug
    if (received != sizeof(response_code)) return ERR_NETWORK_FAILURE;

    return response_code;
}

ErrorCode client_delete(Client *client, const char *filepath) {
    if (!client || !filepath) return ERR_INVALID_ARGUMENT;

    // Prepare delete request
    DeleteRequest request = {0};
    request.header.request_id = generate_request_id(client);
    request.header.type = MSG_TYPE_DELETE;
    strncpy(request.filepath, filepath, sizeof(request.filepath) - 1);

    // Send request to naming server
    pthread_mutex_lock(&client->mutex);
    ssize_t sent = network_socket_send(client->naming_server_sock, &request, sizeof(request));
    pthread_mutex_unlock(&client->mutex);
    if (sent != sizeof(request)) return ERR_NETWORK_FAILURE;

    // Receive response (error code)
    ErrorCode response_code;
    ssize_t received = network_socket_receive(client->naming_server_sock, &response_code, sizeof(response_code));
    if (received != sizeof(response_code)) return ERR_NETWORK_FAILURE;

    return response_code;
}

// Async operation wrapper
struct AsyncOperation {
    Client *client;
    char *filepath;
    uint64_t offset;
    uint8_t *buffer;
    size_t length;
    client_callback_t callback;
    void *user_data;
};

// Thread function for read_async
static void *read_async_thread(void *arg) {
    struct AsyncOperation *op = arg;
    size_t bytes_read = 0;
    ErrorCode code = client_read(op->client, op->filepath, op->offset, op->buffer, op->length, &bytes_read);
    op->callback(code, op->user_data);
    free(op->filepath);
    free(op);
    return NULL;
}

// Asynchronous file operations
ErrorCode client_read_async(Client *client, const char *filepath, uint64_t offset, uint8_t *buffer, size_t length, client_callback_t callback, void *user_data) {
    if (!client || !filepath || !buffer || !callback) return ERR_INVALID_ARGUMENT;

    struct AsyncOperation *op = malloc(sizeof(struct AsyncOperation));
    if (!op) return ERR_INTERNAL_ERROR;

    op->client = client;
    op->filepath = strdup(filepath);
    op->offset = offset;
    op->buffer = buffer;
    op->length = length;
    op->callback = callback;
    op->user_data = user_data;

    pthread_t thread;
    if (pthread_create(&thread, NULL, read_async_thread, op) != 0) {
        free(op->filepath);
        free(op);
        return ERR_INTERNAL_ERROR;
    }
    pthread_detach(thread);
    return ERR_SUCCESS;
}

// Thread function for write_async
static void *write_async_thread(void *arg) {
    struct AsyncOperation *op = arg;
    ErrorCode code = client_write(op->client, op->filepath, op->offset, op->buffer, op->length);
    op->callback(code, op->user_data);
    free(op->filepath);
    free(op);
    return NULL;
}

ErrorCode client_write_async(Client *client, const char *filepath, uint64_t offset, const uint8_t *buffer, size_t length, client_callback_t callback, void *user_data) {
    if (!client || !filepath || !buffer || !callback) return ERR_INVALID_ARGUMENT;

    struct AsyncOperation *op = malloc(sizeof(struct AsyncOperation));
    if (!op) return ERR_INTERNAL_ERROR;

    op->client = client;
    op->filepath = strdup(filepath);
    op->offset = offset;
    // Casting away constness; ensure safe usage
    op->buffer = (uint8_t *)buffer;
    op->length = length;
    op->callback = callback;
    op->user_data = user_data;

    pthread_t thread;
    if (pthread_create(&thread, NULL, write_async_thread, op) != 0) {
        free(op->filepath);
        free(op);
        return ERR_INTERNAL_ERROR;
    }
    pthread_detach(thread);
    return ERR_SUCCESS;
}

// Streaming support for audio files
ErrorCode client_stream(Client *client, const char *filepath, void (*stream_callback)(const uint8_t *data, size_t length, void *user_data), void *user_data) {
    if (!client || !filepath || !stream_callback) return ERR_INVALID_ARGUMENT;

    // Implement streaming operation
    // For brevity, details are omitted

    return ERR_SUCCESS;
}

// Operations.c - Add streaming implementation

ErrorCode client_stream_audio(Client *client, const char *filepath, 
                            void (*stream_callback)(const uint8_t *data, size_t length, void *user_data),
                            void *user_data) {
    if (!client || !filepath || !stream_callback) 
        return ERR_INVALID_ARGUMENT;

    ErrorCode err = ensure_storage_connection(client, filepath);
    if (err != ERR_SUCCESS)
        return err;

    // Prepare stream request
    MessageHeader header = {
        .type = MSG_TYPE_STREAM,
        .request_id = generate_request_id(client),
        .payload_size = sizeof(StreamRequest)
    };

    StreamRequest request = {0};
    request.header = header;
    strncpy(request.filepath, filepath, sizeof(request.filepath) - 1);

    pthread_mutex_lock(&client->mutex);

    // Send header
    ssize_t sent = network_socket_send(client->storage_server_sock, &header, sizeof(header));
    if (sent != sizeof(header)) {
        pthread_mutex_unlock(&client->mutex);
        return ERR_NETWORK_FAILURE;
    }

    // Send request
    sent = network_socket_send(client->storage_server_sock, &request, sizeof(request));
    if (sent != sizeof(request)) {
        pthread_mutex_unlock(&client->mutex);
        return ERR_NETWORK_FAILURE;
    }

    // Receive initial response header
    MessageHeader response;
    ssize_t received = network_socket_receive(client->storage_server_sock, &response, sizeof(response));
    if (received != sizeof(response)) {
        pthread_mutex_unlock(&client->mutex);
        return ERR_NETWORK_FAILURE;
    }

    // Check for error response
    if (response.type == MSG_TYPE_ERROR) {
        ErrorCode error_code;
        received = network_socket_receive(client->storage_server_sock, &error_code, sizeof(error_code));
        pthread_mutex_unlock(&client->mutex);
        return error_code;
    }

    // Start receiving audio stream data
    uint8_t buffer[8192]; // 8KB buffer for audio data
    while ((received = network_socket_receive(client->storage_server_sock, buffer, sizeof(buffer))) > 0) {
        stream_callback(buffer, received, user_data);
    }

    pthread_mutex_unlock(&client->mutex);
    return ERR_SUCCESS;
}

// In operations.c

ErrorCode client_get_file_info(Client *client, const char *filepath, uint64_t *file_size, uint32_t *permissions) {
    if (!client || !filepath || !file_size || !permissions)
        return ERR_INVALID_ARGUMENT;

    ErrorCode err = ensure_storage_connection(client, filepath);
    if (err != ERR_SUCCESS)
        return err;

    // Prepare the request header
    MessageHeader header = {
        .request_id = htonl(generate_request_id(client)),
        .type = MSG_TYPE_GET_FILE_INFO,
        .payload_size = htonl(sizeof(GetFileInfoRequest))
    };

    // Prepare the request payload
    GetFileInfoRequest request;
    memset(&request, 0, sizeof(request));
    strncpy(request.filepath, filepath, sizeof(request.filepath) - 1);

    pthread_mutex_lock(&client->mutex);

    // Send the header
    ssize_t sent = network_socket_send(client->storage_server_sock, &header, sizeof(header));
    if (sent != sizeof(header)) {
        pthread_mutex_unlock(&client->mutex);
        return ERR_NETWORK_FAILURE;
    }

    // Send the request payload
    sent = network_socket_send(client->storage_server_sock, &request, sizeof(request));
    if (sent != sizeof(request)) {
        pthread_mutex_unlock(&client->mutex);
        return ERR_NETWORK_FAILURE;
    }

    // Receive the response header
    MessageHeader response_header;
    ssize_t received = network_socket_receive(client->storage_server_sock, &response_header, sizeof(response_header));
    if (received != sizeof(response_header)) {
        pthread_mutex_unlock(&client->mutex);
        return ERR_NETWORK_FAILURE;
    }

    if (response_header.type == MSG_TYPE_ERROR) {
        ErrorCode error_code;
        received = network_socket_receive(client->storage_server_sock, &error_code, sizeof(error_code));
        pthread_mutex_unlock(&client->mutex);
        return error_code;
    }

    if (response_header.type != MSG_TYPE_GET_FILE_INFO_RESPONSE) {
        pthread_mutex_unlock(&client->mutex);
        return ERR_PROTOCOL_ERROR;
    }

    // Receive the response payload
    GetFileInfoResponse response;
    received = network_socket_receive(client->storage_server_sock, &response, sizeof(response));
    if (received != sizeof(response)) {
        pthread_mutex_unlock(&client->mutex);
        return ERR_NETWORK_FAILURE;
    }

    *file_size = ntohl(response.file_size);
    *permissions = ntohl(response.permissions);

    pthread_mutex_unlock(&client->mutex);
    return ERR_SUCCESS;
}

// In operations.c

// Declare the client_get_file_info function
ErrorCode client_get_file_info(Client *client, const char *filepath, uint64_t *file_size, uint32_t *permissions);

// Add this structure to manage the mpv pipe
typedef struct {
    FILE *mpv_pipe;
    int error_occurred;
} MpvStreamContext;

// Modified stream callback for mpv
static void mpv_stream_callback(const uint8_t *data, size_t length, void *user_data) {
    MpvStreamContext *ctx = (MpvStreamContext *)user_data;
    if (!ctx || !ctx->mpv_pipe) return;

    size_t written = fwrite(data, 1, length, ctx->mpv_pipe);
    if (written != length) {
        ctx->error_occurred = 1;
    }
    fflush(ctx->mpv_pipe);
}

// New function to handle audio streaming to mpv
// In operations.c - Fix the struct access syntax
ErrorCode client_stream_audio_mpv(Client *client, const char *filepath) {
    if (!client || !filepath) 
        return ERR_INVALID_ARGUMENT;

    // Create pipe to mpv command
    const char *mpv_cmd = "mpv - --no-terminal";
    
    MpvStreamContext ctx = {0};
    ctx.mpv_pipe = popen(mpv_cmd, "w");
    
    if (!ctx.mpv_pipe) {
        fprintf(stderr, "Failed to start mpv\n");
        return ERR_INTERNAL_ERROR;
    }

    // Stream the audio data
    ErrorCode err = client_stream_audio(client, filepath, mpv_stream_callback, &ctx);

    // Clean up - Fix: Change ctx-> to ctx.
    pclose(ctx.mpv_pipe);

    if (ctx.error_occurred) {
        return ERR_IO_ERROR;
    }

    return err;
}
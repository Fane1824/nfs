#ifndef CLIENT_H
#define CLIENT_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include "errors.h"
#include "protocol.h"
#include "network.h"

// Opaque client handle
typedef struct Client {
    NetworkSocket *naming_server_sock;
    NetworkSocket *storage_server_sock; // Current storage server connection
    pthread_mutex_t mutex;
} Client;

// Initialize the client library
ErrorCode client_init(Client **client, const char *naming_server_host, const char *naming_server_port);

// Clean up the client library
void client_cleanup(Client *client);

// Synchronous file operations
ErrorCode client_read(Client *client, const char *filepath, uint64_t offset, uint8_t *buffer, size_t length, size_t *bytes_read);
ErrorCode client_write(Client *client, const char *filepath, uint64_t offset, const uint8_t *buffer, size_t length);
ErrorCode client_create(Client *client, const char *filepath, uint32_t mode);
ErrorCode client_delete(Client *client, const char *filepath);

// Asynchronous operation callback
typedef void (*client_callback_t)(ErrorCode code, void *user_data);

// Asynchronous file operations
ErrorCode client_read_async(Client *client, const char *filepath, uint64_t offset, uint8_t *buffer, size_t length, client_callback_t callback, void *user_data);
ErrorCode client_write_async(Client *client, const char *filepath, uint64_t offset, const uint8_t *buffer, size_t length, client_callback_t callback, void *user_data);

// Streaming support for audio files
ErrorCode client_stream(Client *client, const char *filepath, void (*stream_callback)(const uint8_t *data, size_t length, void *user_data), void *user_data);

#endif // CLIENT_H
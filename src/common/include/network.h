#ifndef NETWORK_H
#define NETWORK_H

#include <stddef.h>
#include <sys/types.h>

typedef struct NetworkSocket NetworkSocket;

int network_socket_get_fd(NetworkSocket *sock);
NetworkSocket *network_socket_create(const char *host, const char *port);
void network_socket_close(NetworkSocket *sock);
NetworkSocket *network_socket_accept(NetworkSocket *server_sock);

ssize_t network_socket_send(NetworkSocket *sock, const void *buffer, size_t length);
ssize_t network_socket_receive(NetworkSocket *sock, void *buffer, size_t length);

// Asynchronous operations
typedef void (*network_callback_t)(NetworkSocket *sock, void *user_data, ssize_t result);

int network_socket_send_async(NetworkSocket *sock, const void *buffer, size_t length, network_callback_t callback, void *user_data);
int network_socket_receive_async(NetworkSocket *sock, void *buffer, size_t length, network_callback_t callback, void *user_data);

#endif // NETWORK_H
#define __USE_GNU
#define _GNU_SOURCE
#include "network.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <fcntl.h>

struct NetworkSocket {
    int fd;
    pthread_mutex_t mutex;
};

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int network_socket_get_fd(NetworkSocket *sock) {
    if (!sock) return -1;
    return sock->fd;
}

NetworkSocket *network_socket_create(const char *host, const char *port) {
    struct addrinfo hints = {0}, *res, *p;
    int sockfd = -1;
    NetworkSocket *sock = NULL;

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    // Server mode if host is NULL
    if (!host) {
        hints.ai_flags = AI_PASSIVE;
    }

    if (getaddrinfo(host, port, &hints, &res) != 0) return NULL;

    for (p = res; p != NULL; p = p->ai_next) {
        sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sockfd == -1) continue;

        if (!host) {
            // Server socket setup
            int yes = 1;
            if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1) {
                close(sockfd);
                continue;
            }
            if (bind(sockfd, p->ai_addr, p->ai_addrlen) == 0) {
                if (listen(sockfd, SOMAXCONN) == 0)
                    break;
            }
        }
        else {
            // Client socket setup
            if (connect(sockfd, p->ai_addr, p->ai_addrlen) == 0)
                break;
        }

        close(sockfd);
        sockfd = -1;
    }

    freeaddrinfo(res);

    if (sockfd == -1) return NULL;

    if (host) {
    // if (1) {
        // Only set non-blocking mode for client sockets
        if (set_nonblocking(sockfd) == -1) {
            close(sockfd);
            return NULL;
        }
    }

    sock = malloc(sizeof(NetworkSocket));
    if (!sock) {
        close(sockfd);
        return NULL;
    }

    pthread_mutex_init(&sock->mutex, NULL);
    sock->fd = sockfd;
    return sock;
}

NetworkSocket *network_socket_accept(NetworkSocket *server_sock) {
    NetworkSocket *client_sock;
    int client_fd;

    pthread_mutex_lock(&server_sock->mutex);
    client_fd = accept(server_sock->fd, NULL, NULL);
    pthread_mutex_unlock(&server_sock->mutex);

    if (client_fd == -1)
        return NULL;

    client_sock = malloc(sizeof(NetworkSocket));
    if (!client_sock) {
        close(client_fd);
        return NULL;
    }

    pthread_mutex_init(&client_sock->mutex, NULL);
    client_sock->fd = client_fd;
    return client_sock;
}

void network_socket_close(NetworkSocket *sock) {
    if (sock) {
        close(sock->fd);
        pthread_mutex_destroy(&sock->mutex);
        free(sock);
    }
}

ssize_t network_socket_send(NetworkSocket *sock, const void *buffer, size_t length) {
    pthread_mutex_lock(&sock->mutex);

    size_t total_sent = 0;
    const uint8_t *buf = buffer;

    while (total_sent < length) {
        ssize_t sent = send(sock->fd, buf + total_sent, length - total_sent, 0);
        if (sent <= 0) {
            // Error occurred
            pthread_mutex_unlock(&sock->mutex);
            return -1;
        }
        total_sent += sent;
    }

    pthread_mutex_unlock(&sock->mutex);
    return total_sent;
}

ssize_t network_socket_receive(NetworkSocket *sock, void *buffer, size_t length) {
     pthread_mutex_lock(&sock->mutex);
    
    size_t total_received = 0;
    uint8_t *buf = buffer;
    
    while (total_received < length) {
        ssize_t received = recv(sock->fd, buf + total_received, length - total_received, 0);
        
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Socket would block, retry after small delay
                usleep(1000); // 1ms delay
                continue;
            }
            pthread_mutex_unlock(&sock->mutex);
            return -1;
        }
        
        if (received == 0) {
            // Connection closed by peer
            pthread_mutex_unlock(&sock->mutex);
            return total_received;
        }
        
        total_received += received;
    }
    
    pthread_mutex_unlock(&sock->mutex);
    return total_received;
}

// Asynchronous operations
struct async_op {
    NetworkSocket *sock;
    void *buffer;
    size_t length;
    network_callback_t callback;
    void *user_data;
};

static void *send_async_thread(void *arg) {
    struct async_op *op = arg;
    ssize_t result = network_socket_send(op->sock, op->buffer, op->length);
    op->callback(op->sock, op->user_data, result);
    free(op);
    return NULL;
}

int network_socket_send_async(NetworkSocket *sock, const void *buffer, size_t length, network_callback_t callback, void *user_data) {
    struct async_op *op = malloc(sizeof(struct async_op));
    if (!op) return -1;
    op->sock = sock;
    op->buffer = (void *)buffer;
    op->length = length;
    op->callback = callback;
    op->user_data = user_data;

    pthread_t thread;
    if (pthread_create(&thread, NULL, send_async_thread, op) != 0) {
        free(op);
        return -1;
    }
    pthread_detach(thread);
    return 0;
}

static void *receive_async_thread(void *arg) {
    struct async_op *op = arg;
    ssize_t result = network_socket_receive(op->sock, op->buffer, op->length);
    op->callback(op->sock, op->user_data, result);
    free(op);
    return NULL;
}

int network_socket_receive_async(NetworkSocket *sock, void *buffer, size_t length, network_callback_t callback, void *user_data) {
    struct async_op *op = malloc(sizeof(struct async_op));
    if (!op) return -1;
    op->sock = sock;
    op->buffer = buffer;
    op->length = length;
    op->callback = callback;
    op->user_data = user_data;

    pthread_t thread;
    if (pthread_create(&thread, NULL, receive_async_thread, op) != 0) {
        free(op);
        return -1;
    }
    pthread_detach(thread);
    return 0;
}
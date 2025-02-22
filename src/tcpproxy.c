#include "tcpproxy.h"
#include <pthread.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <netinet/ip.h>
#include <fcntl.h>
#include <sys/sendfile.h>
#include <liburing.h>
#include <poll.h>

#define THREAD_POOL_SIZE 4
#define QUEUE_DEPTH 256
#define BUFFER_SIZE 94192  
#define SERVER_PORT 8080

int set_non_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl F_GETFL");
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl F_SETFL");
        return -1;
    }
    return 0;
}

void get_original_dst(int client_fd, struct sockaddr_in *original_dst) {
    socklen_t len = sizeof(*original_dst);
    if (getsockopt(client_fd, SOL_IP, SO_ORIGINAL_DST, original_dst, &len) == -1) {
        perror("getsockopt SO_ORIGINAL_DST failed");
    } else {
        printf("Conexión redirigida originalmente a: %s:%d\n",
               inet_ntoa(original_dst->sin_addr), ntohs(original_dst->sin_port));
    }
}

void configure_socket(int fd) {
    int opt = 1;
    // Reusar dirección
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Aumentar buffers de socket
    int buffer_size = 262144;    // 256KB
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size));

    // TCP keepalive
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));

    // TCP_NODELAY (deshabilitar algoritmo de Nagle)
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
}

void *handle_client(void *arg) {
    int client_fd = *(int *)arg;
    free(arg);

    char buffer[BUFFER_SIZE];
    struct sockaddr_in orig_dst;
    int n;

    get_original_dst(client_fd, &orig_dst);

    int dest_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (dest_fd < 0) {
        perror("socket");
        close(client_fd);
        return NULL;
    }

    configure_socket(dest_fd); // Configurar socket destino

    if (connect(dest_fd, (struct sockaddr *)&orig_dst, sizeof(orig_dst)) < 0) {
        perror("connect");
        close(client_fd);
        close(dest_fd);
        return NULL;
    }

    set_non_blocking(client_fd);
    set_non_blocking(dest_fd);

    struct io_uring ring;
    io_uring_queue_init(QUEUE_DEPTH, &ring, 0);

    struct io_uring_sqe *sqe;
    struct io_uring_cqe *cqe;

    // Add client_fd to the ring
    sqe = io_uring_get_sqe(&ring);
    io_uring_prep_poll_add(sqe, client_fd, POLLIN);
    io_uring_sqe_set_data(sqe, (void *)(intptr_t)client_fd);
    io_uring_submit(&ring);

    // Add dest_fd to the ring
    sqe = io_uring_get_sqe(&ring);
    io_uring_prep_poll_add(sqe, dest_fd, POLLIN);
    io_uring_sqe_set_data(sqe, (void *)(intptr_t)dest_fd);
    io_uring_submit(&ring);

    while (1) {
        io_uring_wait_cqe(&ring, &cqe);
        int src_fd = (int)(intptr_t)io_uring_cqe_get_data(cqe);
        int dst_fd = (src_fd == client_fd) ? dest_fd : client_fd;

        if (cqe->res < 0) {
            // Error en la operación io_uring
            fprintf(stderr, "io_uring error: %s\n", strerror(-cqe->res));
            close(client_fd);
            close(dest_fd);
            io_uring_queue_exit(&ring);
            return NULL;
        }

        if (cqe->res & POLLIN) {
            while ((n = read(src_fd, buffer, BUFFER_SIZE)) > 0) {
                int total_written = 0;
                while (total_written < n) {
                    // Verificar si el socket destino tiene errores pendientes
                    int socket_error = 0;
                    socklen_t optlen = sizeof(socket_error);
                    if (getsockopt(dst_fd, SOL_SOCKET, SO_ERROR, &socket_error, &optlen) < 0) {
                        perror("getsockopt SO_ERROR");
                        close(client_fd);
                        close(dest_fd);
                        io_uring_queue_exit(&ring);
                        return NULL;
                    }

                    if (socket_error != 0) {
                        fprintf(stderr, "Socket error on dst_fd: %s\n", strerror(socket_error));
                        close(client_fd);
                        close(dest_fd);
                        io_uring_queue_exit(&ring);
                        return NULL;
                    }

                    int written = write(dst_fd, buffer + total_written, n - total_written);
                    if (written == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            // Modify io_uring to monitor for write readiness
                            sqe = io_uring_get_sqe(&ring);
                            io_uring_prep_poll_add(sqe, dst_fd, POLLOUT);
                            io_uring_sqe_set_data(sqe, (void *)(intptr_t)dst_fd);
                            io_uring_submit(&ring);
                            break;
                        } else if (errno == EPIPE || errno == ECONNRESET) {
                            // Connection closed by peer - clean up gracefully
                            printf("Connection closed by peer\n");
                            close(client_fd);
                            close(dest_fd);
                            io_uring_queue_exit(&ring);
                            return NULL;
                        } else {
                            perror("write");
                            close(client_fd);
                            close(dest_fd);
                            io_uring_queue_exit(&ring);
                            return NULL;
                        }
                    } else {
                        total_written += written;
                    }
                }
            }

            if (n == 0) {
                // EOF - Clean shutdown
                printf("Connection closed normally\n");
                close(client_fd);
                close(dest_fd);
                io_uring_queue_exit(&ring);
                return NULL;
            } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                // Error en lectura
                perror("read");
                close(client_fd);
                close(dest_fd);
                io_uring_queue_exit(&ring);
                return NULL;
            }
        } else if (cqe->res & POLLOUT) {
            // Handle write events (resume writing)
            // ... (similar write logic as above, pero starting from where you left off) ...
        }

        // Re-arm the event
        sqe = io_uring_get_sqe(&ring);
        io_uring_prep_poll_add(sqe, src_fd, POLLIN);
        io_uring_sqe_set_data(sqe, (void *)(intptr_t)src_fd);
        io_uring_submit(&ring);

        io_uring_cqe_seen(&ring, cqe);
    }

    close(client_fd);
    close(dest_fd);
    io_uring_queue_exit(&ring);
    return NULL;
}

int create_server_socket() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("Error al crear socket");
        exit(EXIT_FAILURE);
    }
    return server_fd;
}

void configure_server_socket(int server_fd) {
    int opt = 1;
    // Reusar dirección
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
}

void bind_server_socket(int server_fd, struct sockaddr_in *server_addr) {
    memset(server_addr, 0, sizeof(*server_addr));
    server_addr->sin_family = AF_INET;
    server_addr->sin_addr.s_addr = INADDR_ANY;
    server_addr->sin_port = htons(SERVER_PORT);

    if (bind(server_fd, (struct sockaddr *)server_addr, sizeof(*server_addr)) == -1) {
        perror("Error en bind");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
}

void start_listening(int server_fd) {
    if (listen(server_fd, SOMAXCONN) == -1) {
        perror("Error en listen");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
}

void handle_connections(int server_fd) {
    struct io_uring ring;
    io_uring_queue_init(QUEUE_DEPTH, &ring, 0);

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    while (1) {
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd == -1) {
            perror("Error en accept");
            continue;
        }

        printf("Nueva conexión de %s:%d\n",
               inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

        int *pclient_fd = malloc(sizeof(int));
        *pclient_fd = client_fd;
        pthread_t thread;
        pthread_create(&thread, NULL, handle_client, pclient_fd);
        pthread_detach(thread);
    }

    io_uring_queue_exit(&ring);
}

void start_proxy() {
    int server_fd;
    server_fd = create_server_socket();
    configure_server_socket(server_fd);
    struct sockaddr_in server_addr;
    bind_server_socket(server_fd, &server_addr);
    start_listening(server_fd);
    printf("Servidor escuchando en el puerto %d...\n", SERVER_PORT);
    handle_connections(server_fd);
    close(server_fd);
}
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

        if (cqe->res & POLLIN) {
            while ((n = read(src_fd, buffer, BUFFER_SIZE)) > 0) {
                int total_written = 0;
                while (total_written < n) {
                    int written = write(dst_fd, buffer + total_written, n - total_written);
                    if (written == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            // Modify io_uring to monitor for write readiness
                            sqe = io_uring_get_sqe(&ring);
                            io_uring_prep_poll_add(sqe, dst_fd, POLLOUT);
                            io_uring_sqe_set_data(sqe, (void *)(intptr_t)dst_fd);
                            io_uring_submit(&ring);
                            break;
                        } else {
                            perror("write");
                            // Handle error
                        }
                    } else {
                        total_written += written;
                    }
                }
            }
        } else if (cqe->res & POLLOUT) {
            // Handle write events (resume writing)
            while ((n = read(src_fd, buffer, BUFFER_SIZE)) > 0) {
                int total_written = 0;
                while (total_written < n) {
                    int written = write(dst_fd, buffer + total_written, n - total_written);
                    if (written == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            // Modify io_uring to monitor for write readiness
                            sqe = io_uring_get_sqe(&ring);
                            io_uring_prep_poll_add(sqe, dst_fd, POLLOUT);
                            io_uring_sqe_set_data(sqe, (void *)(intptr_t)dst_fd);
                            io_uring_submit(&ring);
                            break;
                        } else {
                            perror("write");
                            // Handle error
                        }
                    } else {
                        total_written += written;
                    }
                }
            }
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
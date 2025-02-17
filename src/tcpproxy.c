
#include "tcpproxy.h"

/// @brief  Get the original destination of a connection
void get_original_dst(int client_fd) {
    struct sockaddr_in original_dst;
    socklen_t len = sizeof(original_dst);
    if (getsockopt(client_fd, SOL_IP, SO_ORIGINAL_DST, &original_dst, &len) == -1) {
        perror("getsockopt SO_ORIGINAL_DST failed");
    } else {
        printf("Conexión redirigida originalmente a: %s:%d\n",
               inet_ntoa(original_dst.sin_addr), ntohs(original_dst.sin_port));
    }
}
/// @brief  Create a server socket
int create_server_socket() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("Error al crear socket");
        exit(EXIT_FAILURE);
    }
    return server_fd;
}
/// @brief  Configure the server socket to reuse the address    
void configure_server_socket(int server_fd) {
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
}
/// @brief  Bind the server socket to the specified address
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
/// @brief 
/// @param server_fd 
void start_listening(int server_fd) {
    if (listen(server_fd, SOMAXCONN) == -1) {
        perror("Error en listen");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
}

/// @brief  Create an epoll instance
int create_epoll_instance() {
    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("Error en epoll_create1");
        exit(EXIT_FAILURE);
    }
    return epoll_fd;
}
/// @brief  Register the server socket in the epoll instance
void register_server_socket_in_epoll(int epoll_fd, int server_fd) {
    struct epoll_event event;
    event.events = EPOLLIN;
    event.data.fd = server_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event) == -1) {
        perror("Error en epoll_ctl");
        close(server_fd);
        close(epoll_fd);
        exit(EXIT_FAILURE);
    }
}
/// @brief  Handle incoming connections
/// @param epoll_fd 
/// @param server_fd 
void handle_connections(int epoll_fd, int server_fd) {
    struct epoll_event events[MAX_EVENTS];
    char response[] = "Hello World\n";
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    while (1) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds == -1) {
            perror("Error en epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == server_fd) {
                int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
                if (client_fd == -1) {
                    perror("Error en accept");
                    continue;
                }

                printf("Nueva conexión de %s:%d\n",
                       inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

                get_original_dst(client_fd);
                send(client_fd, response, strlen(response), 0);
                close(client_fd);
            }
        }
    }
}


void start_proxy() {
    int server_fd, epoll_fd;
    server_fd = create_server_socket();
    configure_server_socket(server_fd);
    struct sockaddr_in server_addr;
    bind_server_socket(server_fd, &server_addr);
    start_listening(server_fd);
    epoll_fd = create_epoll_instance();
    register_server_socket_in_epoll(epoll_fd, server_fd);
    printf("Servidor escuchando en el puerto %d...\n", SERVER_PORT);
    handle_connections(epoll_fd, server_fd);
    close(server_fd);
    close(epoll_fd);
}
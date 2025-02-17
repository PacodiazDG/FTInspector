#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <linux/netfilter_ipv4.h> // Para SO_ORIGINAL_DST

#define SERVER_PORT 8080
#define MAX_EVENTS 10
#define BUFFER_SIZE 1024

void get_original_dst(int client_fd);
int create_server_socket();
void configure_server_socket(int server_fd);
void bind_server_socket(int server_fd, struct sockaddr_in *server_addr);
void start_listening(int server_fd);
int create_epoll_instance();
void register_server_socket_in_epoll(int epoll_fd, int server_fd);
void handle_connections(int epoll_fd, int server_fd);
void start_proxy();
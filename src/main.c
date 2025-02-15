#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define SO_ORIGINAL_DST 80
#define BUFFER_SIZE 4096

void get_original_dst(int sock, char *ip_str, int *port) {
  struct sockaddr_in addr;
  socklen_t addr_len = sizeof(addr);
  getsockopt(sock, SOL_IP, SO_ORIGINAL_DST, &addr, &addr_len);
  *port = ntohs(addr.sin_port);
  inet_ntop(AF_INET, &addr.sin_addr, ip_str, INET_ADDRSTRLEN);
}

void *forward_data(void *args) {
  int *sockets = (int *)args;
  int src = sockets[0], dst = sockets[1];
  char buffer[BUFFER_SIZE];
  ssize_t bytes;
  while ((bytes = recv(src, buffer, BUFFER_SIZE, 0)) > 0) {
    if ((unsigned char)buffer[0] == 0x16) {
      printf("hello client\n");
    //  perror("Error al enviar datos");
    //  close(src);
    //  close(dst);
   //   free(args);
    //  return NULL;
    }
    ssize_t sent_bytes = send(dst, buffer, bytes, 0);
    if (sent_bytes == -1) {
      perror("Error al enviar datos");
      close(src);
      close(dst);
      free(args);
      return NULL;
    }
  }
  close(src);
  close(dst);
  free(args);
  return NULL;
}

void *handle_client(void *arg) {
  int client_sock = *(int *)arg;
  free(arg);
  char dst_ip[INET_ADDRSTRLEN];
  int dst_port;

  get_original_dst(client_sock, dst_ip, &dst_port);
  printf("Conectando a %s:%d\n", dst_ip, dst_port);

  int forward_sock = socket(AF_INET, SOCK_STREAM, 0);
  if (forward_sock < 0) {
    perror("Error creando socket de reenvío");
    close(client_sock);
    return NULL;
  }

  struct sockaddr_in forward_addr = {.sin_family = AF_INET,
                                     .sin_port = htons(dst_port),
                                     .sin_addr.s_addr = inet_addr(dst_ip)};

  if (connect(forward_sock, (struct sockaddr *)&forward_addr,
              sizeof(forward_addr)) < 0) {
    perror("Error conectando con el destino");
    close(client_sock);
    close(forward_sock);
    return NULL;
  }

  pthread_t thread1, thread2;
  int *client_to_server = malloc(2 * sizeof(int));
  int *server_to_client = malloc(2 * sizeof(int));
  client_to_server[0] = client_sock;
  client_to_server[1] = forward_sock;
  server_to_client[0] = forward_sock;
  server_to_client[1] = client_sock;

  pthread_create(&thread1, NULL, forward_data, client_to_server);
  pthread_create(&thread2, NULL, forward_data, server_to_client);

  pthread_join(thread1, NULL);
  pthread_join(thread2, NULL);

  return NULL;
}

void start_proxy(const char *listen_addr, int listen_port) {
  int server_sock = socket(AF_INET, SOCK_STREAM, 0);
  if (server_sock < 0) {
    perror("Error creando socket del proxy");
    exit(EXIT_FAILURE);
  }

  int opt = 1;
  setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in server_addr = {.sin_family = AF_INET,
                                    .sin_port = htons(listen_port),
                                    .sin_addr.s_addr = inet_addr(listen_addr)};

  if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) <
      0) {
    perror("Error en bind");
    close(server_sock);
    exit(EXIT_FAILURE);
  }

  if (listen(server_sock, 5) < 0) {
    perror("Error en listen");
    close(server_sock);
    exit(EXIT_FAILURE);
  }

  printf("Proxy escuchando en %s:%d\n", listen_addr, listen_port);

  while (1) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_sock =
        accept(server_sock, (struct sockaddr *)&client_addr, &client_len);
    if (client_sock < 0) {
      perror("Error aceptando conexión");
      continue;
    }
    printf("Conexión aceptada de %s:%d\n", inet_ntoa(client_addr.sin_addr),
           ntohs(client_addr.sin_port));
    int *client_sock_ptr = malloc(sizeof(int));
    *client_sock_ptr = client_sock;
    pthread_t client_thread;
    pthread_create(&client_thread, NULL, handle_client, client_sock_ptr);
    pthread_detach(client_thread);
  }
}

int main() {
  start_proxy("0.0.0.0", 8080);
  return 0;
}

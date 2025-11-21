#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "config.h"
#include "http.h"      // HttpRequest, BUFFER_SIZE, read_http_request, send_error, send_file, etc.
#include "worker.h"    // void worker_loop(int listen_fd, server_config_t *config);

int main(void) {
    server_config_t config;
    if (load_config("server.conf", &config) != 0) {
        printf("Erro a ler server.conf\n");
        exit(1);
    }

    // cria socket TCP IPv4 (listen socket partilhado por todos os workers)
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(config.port);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 128) < 0) {
        perror("listen failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("MASTER: listening on port %d, spawning %d workers...\n",
           config.port, config.num_workers);

    // cria processos worker (prefork)
    for (int i = 0; i < config.num_workers; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork failed");
            close(server_fd);
            exit(EXIT_FAILURE);
        }
        if (pid == 0) {
            // filho = worker
            worker_loop(server_fd, &config);
            // nunca devia sair daqui
            exit(0);
        }
        // pai continua o ciclo para criar mais workers
    }

    // MASTER: opcionalmente pode fazer wait aos filhos ou ficar num loop
    // simples à espera de sinais (nesta fase podes só fazer pause)
    while (1) {
        pause();
    }

    close(server_fd);
    return 0;
}

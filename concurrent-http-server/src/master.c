#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "config.h"
#include "http.h"
#include "worker.h"
#include "shared_mem.h"
#include "semaphores.h"

int main(void) {
    server_config_t config;
    if (load_config("server.conf", &config) != 0) {
        printf("Erro a ler server.conf\n");
        exit(1);
    }

    // cria socket TCP IPv4 (listen socket do master)
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

    // cria memória partilhada
    shared_data_t *shared = create_shared_memory();
    if (!shared) {
        fprintf(stderr, "Erro a criar memória partilhada\n");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // inicializa semáforos
    semaphores_t sems;
    if (init_semaphores(&sems, config.max_queue_size) != 0) {
        fprintf(stderr, "Erro a criar semáforos\n");
        destroy_shared_memory(shared);
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
            destroy_semaphores(&sems);
            destroy_shared_memory(shared);
            exit(EXIT_FAILURE);
        }
        if (pid == 0) {
            // filho = worker
            worker_loop(shared, &sems, &config);
            // nunca devia sair daqui
            exit(0);
        }
        // pai continua para criar mais workers
    }

    // loop principal do MASTER: aceita e enfileira ligações
    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            perror("accept failed (master)");
            continue;
        }

        // producer: coloca FD na queue com semáforos
        sem_wait(sems.empty);
        sem_wait(sems.mutex);

        int idx = shared->queue.rear;
        shared->queue.sockets[idx] = client_fd;
        shared->queue.rear = (shared->queue.rear + 1) % MAX_QUEUE_SIZE;
        shared->queue.count++;

        sem_post(sems.mutex);
        sem_post(sems.full);
    }

    // nunca chega aqui em execução normal
    destroy_semaphores(&sems);
    destroy_shared_memory(shared);
    close(server_fd);
    return 0;
}

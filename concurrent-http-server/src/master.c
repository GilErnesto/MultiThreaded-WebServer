#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#include <errno.h>

#include "config.h"
#include "http.h"
#include "worker.h"
#include "stats.h"
#include "master.h"

static void sigchld_handler(int sig) {
    (void)sig;
    int saved_errno = errno;

    // Recolhe todos os filhos terminados
    while (waitpid(-1, NULL, WNOHANG) > 0) {
        // nada
    }

    errno = saved_errno;
}

int master_main(void) {
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

    // Registar handler para SIGCHLD
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;

    if (sigaction(SIGCHLD, &sa, NULL) < 0) {
        perror("sigaction SIGCHLD");
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
            // filho = worker - passa o server_fd para fazer accept()
            worker_loop(shared, &sems, &config, server_fd);
            exit(0);
        }
        // pai continua para criar mais workers
    }
    
    // Master não precisa do server_fd
    close(server_fd);

    // PROCESSO DE ESTATÍSTICAS
    pid_t stats_pid = fork();
    if (stats_pid == 0) {
        // FILHO ESPECIAL = PROCESSO DE ESTATÍSTICAS
        while (1) {
            sleep(30);

            sem_wait(sems.stats);

            printf("\n===== ESTATÍSTICAS =====\n");
            printf("Total requests:      %ld\n", shared->stats.total_requests);
            printf("Bytes transferred:   %ld\n", shared->stats.bytes_transferred);
            printf("Status 200:          %ld\n", shared->stats.status_200);
            printf("Status 400:          %ld\n", shared->stats.status_400);
            printf("Status 403:          %ld\n", shared->stats.status_403);
            printf("Status 404:          %ld\n", shared->stats.status_404);
            printf("Status 500:          %ld\n", shared->stats.status_500);
            printf("Status 501:          %ld\n", shared->stats.status_501);
            printf("Active connections:  %d\n", shared->stats.active_connections);
            printf("========================\n");

            sem_post(sems.stats);
        }
        exit(0);
    }

    // LOOP PRINCIPAL DO MASTER: espera por filhos
    while (1) {
        pause();
    }

    destroy_semaphores(&sems);
    destroy_shared_memory(shared);
    return 0;
}

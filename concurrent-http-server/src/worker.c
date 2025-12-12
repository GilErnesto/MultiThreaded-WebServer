#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>

#include "worker.h"
#include "stats.h"
#include "logger.h"
#include "thread_pool.h"

volatile sig_atomic_t worker_shutdown = 0;

static void worker_shutdown_handler(int sig) {
    (void)sig;
    worker_shutdown = 1;
}

void worker_loop(shared_data_t *shared, semaphores_t *sems, server_config_t *config, int server_fd) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = worker_shutdown_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("sigaction SIGTERM (worker)");
    }
    printf("[WORKER PID=%d] Reabrindo semáforos...\n", getpid());
    fflush(stdout);
    
    if (reopen_semaphores(sems) != 0) {
        fprintf(stderr, "Erro ao reabrir semáforos no worker\n");
        exit(1);
    }
    
    printf("[WORKER PID=%d] Semáforos reabertos com sucesso\n", getpid());
    fflush(stdout);
    
    printf("[WORKER PID=%d] Criando logger...\n", getpid());
    fflush(stdout);
    logger_t *logger = create_logger(sems, config);
    if (!logger) {
        fprintf(stderr, "[WORKER PID=%d] Erro ao criar logger\n", getpid());
        exit(1);
    }

    printf("[WORKER PID=%d] Worker process started with %d threads\n", getpid(), config->threads_per_worker);
    fflush(stdout);

    // inicia pool de threads (não retorna)
    thread_pool_start(shared, sems, config, logger, server_fd);

    destroy_logger(logger);
}
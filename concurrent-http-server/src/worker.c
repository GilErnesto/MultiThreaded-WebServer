#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "worker.h"
#include "stats.h"
#include "logger.h"
#include "thread_pool.h"

void worker_loop(shared_data_t *shared, semaphores_t *sems, server_config_t *config, int server_fd) {
    
    printf("[WORKER PID=%d] Reabrindo semáforos...\n", getpid());
    fflush(stdout);
    
    // Reabrir semáforos após fork()
    if (reopen_semaphores(sems) != 0) {
        fprintf(stderr, "Erro ao reabrir semáforos no worker\n");
        exit(1);
    }
    
    printf("[WORKER PID=%d] Semáforos reabertos com sucesso\n", getpid());
    fflush(stdout);
    
    // Criar logger
    logger_t *logger = create_logger(sems, config);
    if (!logger) {
        fprintf(stderr, "Erro ao criar logger\n");
        exit(1);
    }

    printf("Worker process started with %d threads\n", config->threads_per_worker);

    // Iniciar pool de threads - esta função nunca retorna
    thread_pool_start(shared, sems, config, logger, server_fd);

    // Nunca chega aqui
    destroy_logger(logger);
}
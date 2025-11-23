#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "worker.h"
#include "stats.h"
#include "logger.h"
#include "thread_pool.h"

void worker_loop(shared_data_t *shared, semaphores_t *sems, server_config_t *config) {
    
    // Criar logger
    logger_t *logger = create_logger(sems, config);
    if (!logger) {
        fprintf(stderr, "Erro ao criar logger\n");
        exit(1);
    }

    printf("Worker process started with %d threads\n", config->threads_per_worker);

    // Iniciar pool de threads - esta função nunca retorna
    thread_pool_start(shared, sems, config, logger);

    // Nunca chega aqui
    destroy_logger(logger);
}
#ifndef WORKER_H
#define WORKER_H

#include "config.h"
#include "shared_mem.h"
#include "semaphores.h"

// cada processo worker entra aqui
void worker_loop(shared_data_t *shared, semaphores_t *sems, server_config_t *config);

#endif

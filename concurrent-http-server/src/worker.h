#ifndef WORKER_H
#define WORKER_H

#include "config.h"
#include "stats.h"
#include <signal.h>

extern volatile sig_atomic_t worker_shutdown;

void worker_loop(shared_data_t *shared, semaphores_t *sems, server_config_t *config, int server_fd);

#endif

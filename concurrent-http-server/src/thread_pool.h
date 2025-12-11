#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include "stats.h"
#include "config.h"
#include "logger.h"

void thread_pool_start(shared_data_t *shared, 
                      semaphores_t *sems, 
                      server_config_t *config, 
                      logger_t *logger,
                      int server_fd);

#endif

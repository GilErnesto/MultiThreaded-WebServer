#ifndef LOGGER_H
#define LOGGER_H

#include <stdio.h>
#include "stats.h"
#include "config.h"

typedef struct {
    semaphores_t    *sems;
    server_config_t *config;
    int              log_fd;  // file descriptor instead of FILE* for O_APPEND
} logger_t;

logger_t* create_logger(semaphores_t *sems, server_config_t *config);
void destroy_logger(logger_t *logger);
void log_request(logger_t *logger,
                const char *method,
                const char *path,
                const char *version,
                int status_code,
                long bytes_sent);

#endif

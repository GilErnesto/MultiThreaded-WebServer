#ifndef WORKER_H
#define WORKER_H

#include "config.h"

void worker_loop(int listen_fd, server_config_t *config);

#endif

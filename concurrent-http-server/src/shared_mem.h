#ifndef SHARED_MEM_H
#define SHARED_MEM_H

#define MAX_QUEUE_SIZE 100

typedef struct {
    int sockets[MAX_QUEUE_SIZE];
    int front;
    int rear;
    int count;
} connection_queue_t;

typedef struct {
    long total_requests;
    long bytes_transferred;
    long status_200;
    long status_400;
    long status_403;
    long status_404;
    long status_500;
    long status_501;
    int active_connections;
} server_stats_t;

typedef struct {
    connection_queue_t queue;
    server_stats_t stats;
} shared_data_t;

shared_data_t* create_shared_memory();
void destroy_shared_memory(shared_data_t* data);

#endif

#ifndef STATS_H
#define STATS_H

#include <semaphore.h>
#include <time.h>

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
    long status_503;
    int active_connections;
    double total_response_time;  // soma dos tempos de resposta em segundos
    long completed_requests;     // para calcular média
    time_t server_start_time;    // timestamp de início do servidor
} server_stats_t;

typedef struct {
    connection_queue_t queue;
    server_stats_t stats;
} shared_data_t;

shared_data_t* create_shared_memory();
void destroy_shared_memory(shared_data_t* data);

typedef struct {
    sem_t *empty;  // lugares livres
    sem_t *full;   // lugares ocupados
    sem_t *mutex;  // exclusão mútua
    sem_t *stats;
    sem_t *log;
} semaphores_t;

int init_semaphores(semaphores_t *s, int max_queue_size);
int reopen_semaphores(semaphores_t *s);
void destroy_semaphores(semaphores_t *s);

int enqueue_connection(shared_data_t *shared, semaphores_t *sems, int client_fd);
int dequeue_connection(shared_data_t *shared, semaphores_t *sems);

#endif

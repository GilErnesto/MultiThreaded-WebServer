#ifndef STATS_H
#define STATS_H

#include <semaphore.h>

#define MAX_QUEUE_SIZE 100

// Queue de conexões
typedef struct {
    int sockets[MAX_QUEUE_SIZE];
    int front;
    int rear;
    int count;
} connection_queue_t;

// Estatísticas do servidor
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

// Estrutura de dados partilhada
typedef struct {
    connection_queue_t queue;
    server_stats_t stats;
} shared_data_t;

// Funções para gestão de memória partilhada
shared_data_t* create_shared_memory();
void destroy_shared_memory(shared_data_t* data);

// Funções para gestão de semáforos
typedef struct {
    sem_t *empty;  // lugares livres na queue
    sem_t *full;   // lugares ocupados (há trabalho)
    sem_t *mutex;  // exclusão mútua na queue
    sem_t *stats;  // para estatísticas
    sem_t *log;    // para logging
} semaphores_t;

int init_semaphores(semaphores_t *s, int max_queue_size);
void destroy_semaphores(semaphores_t *s);

#endif

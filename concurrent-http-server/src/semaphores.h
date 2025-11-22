#ifndef SEMAPHORES_H
#define SEMAPHORES_H

#include <semaphore.h>

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

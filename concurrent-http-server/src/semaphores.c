#include "semaphores.h"
#include <fcntl.h>
#include <stdio.h>

#define SEM_EMPTY_NAME "/web_sem_empty"
#define SEM_FULL_NAME  "/web_sem_full"
#define SEM_MUTEX_NAME "/web_sem_mutex"
#define SEM_STATS_NAME "/web_sem_stats"
#define SEM_LOG_NAME   "/web_sem_log"

int init_semaphores(semaphores_t *s, int max_queue_size) {
    // remove semáforos antigos se existirem
    sem_unlink(SEM_EMPTY_NAME);
    sem_unlink(SEM_FULL_NAME);
    sem_unlink(SEM_MUTEX_NAME);
    sem_unlink(SEM_STATS_NAME);
    sem_unlink(SEM_LOG_NAME);

    s->empty = sem_open(SEM_EMPTY_NAME, O_CREAT, 0666, max_queue_size);
    s->full  = sem_open(SEM_FULL_NAME,  O_CREAT, 0666, 0);
    s->mutex = sem_open(SEM_MUTEX_NAME, O_CREAT, 0666, 1);
    s->stats = sem_open(SEM_STATS_NAME, O_CREAT, 0666, 1);
    s->log   = sem_open(SEM_LOG_NAME,   O_CREAT, 0666, 1);

    if (s->empty == SEM_FAILED || s->full == SEM_FAILED ||
        s->mutex == SEM_FAILED || s->stats == SEM_FAILED ||
        s->log == SEM_FAILED) {

        perror("sem_open");
        return -1;
    }

    return 0;
}

void destroy_semaphores(semaphores_t *s) {
    if (!s) return;

    if (s->empty) sem_close(s->empty);
    if (s->full)  sem_close(s->full);
    if (s->mutex) sem_close(s->mutex);
    if (s->stats) sem_close(s->stats);
    if (s->log)   sem_close(s->log);

    sem_unlink(SEM_EMPTY_NAME);
    sem_unlink(SEM_FULL_NAME);
    sem_unlink(SEM_MUTEX_NAME);
    sem_unlink(SEM_STATS_NAME);
    sem_unlink(SEM_LOG_NAME);
}

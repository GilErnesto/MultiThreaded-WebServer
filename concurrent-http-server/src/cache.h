#ifndef CACHE_H
#define CACHE_H

#include <stddef.h>
#include <pthread.h>

#define CACHE_MAX_ENTRIES 128

typedef struct {
    char   path[512];
    char  *data;
    size_t size;
    unsigned long last_used;
    int    in_use;
} cache_entry_t;

typedef struct {
    cache_entry_t entries[CACHE_MAX_ENTRIES];
    size_t        max_bytes;
    size_t        used_bytes;
    unsigned long counter;      // para LRU simples
    pthread_rwlock_t lock;
} cache_t;

void cache_init(cache_t *cache, size_t max_bytes);
void cache_destroy(cache_t *cache);

// devolve 1 em hit, 0 em miss
int cache_get(cache_t *cache, const char *path, const char **data, size_t *size);

// tenta inserir (ignora ficheiros maiores que max_bytes)
void cache_put(cache_t *cache, const char *path, const char *data, size_t size);

#endif

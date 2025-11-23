#include "cache.h"

#include <string.h>
#include <stdlib.h>

static int find_entry(cache_t *cache, const char *path) {
    for (int i = 0; i < CACHE_MAX_ENTRIES; i++) {
        if (cache->entries[i].in_use &&
            strcmp(cache->entries[i].path, path) == 0) {
            return i;
        }
    }
    return -1;
}

static int evict_victim(cache_t *cache, size_t needed) {
    // se houver espaço suficiente, não é preciso evict já
    if (cache->used_bytes + needed <= cache->max_bytes)
        return -1;

    // escolhe LRU
    int victim = -1;
    unsigned long best = (unsigned long)-1;

    for (int i = 0; i < CACHE_MAX_ENTRIES; i++) {
        if (cache->entries[i].in_use &&
            cache->entries[i].last_used < best) {
            best = cache->entries[i].last_used;
            victim = i;
        }
    }

    if (victim >= 0) {
        cache_entry_t *e = &cache->entries[victim];
        cache->used_bytes -= e->size;
        free(e->data);
        e->data = NULL;
        e->size = 0;
        e->in_use = 0;
    }

    return victim;
}

void cache_init(cache_t *cache, size_t max_bytes) {
    memset(cache, 0, sizeof(*cache));
    cache->max_bytes = max_bytes;
    pthread_rwlock_init(&cache->lock, NULL);
}

void cache_destroy(cache_t *cache) {
    pthread_rwlock_wrlock(&cache->lock);
    for (int i = 0; i < CACHE_MAX_ENTRIES; i++) {
        if (cache->entries[i].in_use) {
            free(cache->entries[i].data);
            cache->entries[i].data = NULL;
            cache->entries[i].size = 0;
            cache->entries[i].in_use = 0;
        }
    }
    cache->used_bytes = 0;
    pthread_rwlock_unlock(&cache->lock);
    pthread_rwlock_destroy(&cache->lock);
}

int cache_get(cache_t *cache, const char *path, const char **data, size_t *size) {
    int hit = 0;

    pthread_rwlock_rdlock(&cache->lock);

    int idx = find_entry(cache, path);
    if (idx >= 0) {
        cache_entry_t *e = &cache->entries[idx];
        *data = e->data;
        *size = e->size;
        hit = 1;
    }

    pthread_rwlock_unlock(&cache->lock);

    if (hit) {
        // atualizar last_used com lock de escrita
        pthread_rwlock_wrlock(&cache->lock);
        int idx2 = find_entry(cache, path);
        if (idx2 >= 0) {
            cache->counter++;
            cache->entries[idx2].last_used = cache->counter;
        }
        pthread_rwlock_unlock(&cache->lock);
    }

    return hit;
}

void cache_put(cache_t *cache, const char *path, const char *data, size_t size) {
    if (size == 0) return;
    if (size > cache->max_bytes) return; // ficheiro demasiado grande para a cache

    pthread_rwlock_wrlock(&cache->lock);

    // se já existe, substitui
    int idx = find_entry(cache, path);
    if (idx >= 0) {
        cache_entry_t *e = &cache->entries[idx];
        cache->used_bytes -= e->size;
        free(e->data);
        e->data = NULL;
        e->size = 0;
        e->in_use = 0;
    }

    // garante espaço (pode fazer evict múltiplos)
    while (cache->used_bytes + size > cache->max_bytes) {
        if (evict_victim(cache, size) < 0)
            break;
    }

    if (cache->used_bytes + size > cache->max_bytes) {
        // mesmo após evict não há espaço → desiste
        pthread_rwlock_unlock(&cache->lock);
        return;
    }

    // arranja slot livre
    int free_idx = -1;
    for (int i = 0; i < CACHE_MAX_ENTRIES; i++) {
        if (!cache->entries[i].in_use) {
            free_idx = i;
            break;
        }
    }
    if (free_idx < 0) {
        // sem slots → tenta evict um e usar
        free_idx = evict_victim(cache, size);
        if (free_idx < 0) {
            pthread_rwlock_unlock(&cache->lock);
            return;
        }
    }

    cache_entry_t *e = &cache->entries[free_idx];
    e->data = malloc(size);
    if (!e->data) {
        pthread_rwlock_unlock(&cache->lock);
        return;
    }

    memcpy(e->data, data, size);
    strncpy(e->path, path, sizeof(e->path) - 1);
    e->path[sizeof(e->path) - 1] = '\0';
    e->size = size;
    cache->used_bytes += size;
    cache->counter++;
    e->last_used = cache->counter;
    e->in_use = 1;

    pthread_rwlock_unlock(&cache->lock);
}

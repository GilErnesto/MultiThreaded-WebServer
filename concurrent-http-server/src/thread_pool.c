#include "thread_pool.h"
#include "http.h"
#include "cache.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

// Estrutura de argumentos para threads
typedef struct {
    shared_data_t   *shared;
    semaphores_t    *sems;
    server_config_t *config;
    logger_t        *logger;
    cache_t         *cache;
} thread_args_t;

// Declaração forward das funções
static void* worker_thread(void *arg);
static long handle_client(int client_fd, thread_args_t *args);

void thread_pool_start(shared_data_t *shared, 
                      semaphores_t *sems, 
                      server_config_t *config, 
                      logger_t *logger) 
{
    int num_threads = config->threads_per_worker;
    if (num_threads <= 0) num_threads = 1;

    // Inicializar cache
    cache_t *cache = malloc(sizeof(cache_t));
    if (!cache) {
        perror("malloc cache");
        exit(1);
    }
    
    size_t max_bytes = (size_t)config->cache_size_mb * 1024 * 1024;
    if (max_bytes == 0) max_bytes = 1 * 1024 * 1024; // mínimo 1MB
    cache_init(cache, max_bytes);

    // Preparar argumentos para as threads
    thread_args_t args;
    args.shared = shared;
    args.sems = sems;
    args.config = config;
    args.logger = logger;
    args.cache = cache;

    // Criar array de threads
    pthread_t *threads = malloc(num_threads * sizeof(pthread_t));
    if (!threads) {
        perror("malloc threads");
        exit(1);
    }

    // Criar threads
    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&threads[i], NULL, worker_thread, &args) != 0) {
            perror("pthread_create");
            exit(1);
        }
    }

    // O processo worker mantém-se vivo; as threads fazem o trabalho
    // Bloquear para sempre
    for (;;) {
        pause();
    }

    // Cleanup (nunca chega aqui em operação normal)
    cache_destroy(cache);
    free(cache);
    free(threads);
}

// Função executada por cada thread do pool
static void* worker_thread(void *arg) {
    thread_args_t *args = (thread_args_t*)arg;
    shared_data_t *shared = args->shared;
    semaphores_t  *sems   = args->sems;

    for (;;) {
        // Consumidor: espera por trabalho na queue
        sem_wait(sems->full);
        sem_wait(sems->mutex);

        int idx = shared->queue.front;
        int client_fd = shared->queue.sockets[idx];
        shared->queue.front = (shared->queue.front + 1) % MAX_QUEUE_SIZE;
        shared->queue.count--;

        sem_post(sems->mutex);
        sem_post(sems->empty);

        // Incrementar conexões ativas e total de pedidos
        sem_wait(sems->stats);
        shared->stats.active_connections++;
        shared->stats.total_requests++;
        sem_post(sems->stats);

        // Processar o pedido HTTP e obter bytes transferidos
        long bytes = handle_client(client_fd, args);

        // Atualizar bytes transferidos e decrementar conexões ativas
        sem_wait(sems->stats);
        shared->stats.bytes_transferred += bytes;
        shared->stats.active_connections--;
        sem_post(sems->stats);
    }

    return NULL; // nunca chega aqui
}

// Trata um pedido HTTP num socket já aceite
static long handle_client(int client_fd, thread_args_t *args) {
    char buffer[BUFFER_SIZE];
    ssize_t bytes = read_http_request(client_fd, buffer, sizeof(buffer));
    if (bytes <= 0) {
        close(client_fd);
        return 0;
    }

    buffer[bytes] = '\0';

    HttpRequest req;
    if (parse_http_request(buffer, &req) != 0) {
        long sent = send_error(client_fd,
                   "HTTP/1.1 400 Bad Request",
                   "<h1>400 Bad Request</h1>");
        
        sem_wait(args->sems->stats);
        args->shared->stats.status_400++;
        sem_post(args->sems->stats);
        
        log_request(args->logger, NULL, NULL, NULL, 400, sent);
        
        close(client_fd);
        return sent;
    }

    // directory traversal
    if (strstr(req.path, "..") != NULL) {
        long sent = send_error(client_fd,
                   "HTTP/1.1 403 Forbidden",
                   "<h1>403 Forbidden</h1>");
        
        sem_wait(args->sems->stats);
        args->shared->stats.status_403++;
        sem_post(args->sems->stats);
        
        log_request(args->logger, req.method, req.path, req.version, 403, sent);
        
        close(client_fd);
        return sent;
    }

    // apenas GET e HEAD
    if (strcmp(req.method, "GET") != 0 && strcmp(req.method, "HEAD") != 0) {
        long sent = send_error(client_fd,
                   "HTTP/1.1 501 Not Implemented",
                   "<h1>501 Not Implemented</h1>");
        
        sem_wait(args->sems->stats);
        args->shared->stats.status_501++;
        sem_post(args->sems->stats);
        
        log_request(args->logger, req.method, req.path, req.version, 501, sent);
        
        close(client_fd);
        return sent;
    }

    // constrói caminho real a partir da DOCUMENT_ROOT
    char fullpath[1024];
    if (strcmp(req.path, "/") == 0) {
        snprintf(fullpath, sizeof(fullpath), "%s/index.html",
                 args->config->document_root);
    } else {
        snprintf(fullpath, sizeof(fullpath), "%s%s",
                 args->config->document_root, req.path);
    }

    int send_body = strcmp(req.method, "HEAD") != 0;
    
    // Verificar se o arquivo existe para determinar o status code antes de enviar
    long sent;
    if (access(fullpath, F_OK) != 0) {
        // Arquivo não existe - 404
        sent = send_file_with_cache(client_fd, fullpath, send_body, args->cache);
        sem_wait(args->sems->stats);
        args->shared->stats.status_404++;
        sem_post(args->sems->stats);
        log_request(args->logger, req.method, req.path, req.version, 404, sent);
    } else if (access(fullpath, R_OK) != 0) {
        // Sem permissão - 403
        sent = send_file_with_cache(client_fd, fullpath, send_body, args->cache);
        sem_wait(args->sems->stats);
        args->shared->stats.status_403++;
        sem_post(args->sems->stats);
        log_request(args->logger, req.method, req.path, req.version, 403, sent);
    } else {
        // Arquivo existe e é legível - 200
        sent = send_file_with_cache(client_fd, fullpath, send_body, args->cache);
        sem_wait(args->sems->stats);
        args->shared->stats.status_200++;
        sem_post(args->sems->stats);
        log_request(args->logger, req.method, req.path, req.version, 200, sent);
    }
    
    close(client_fd);
    return sent;
}

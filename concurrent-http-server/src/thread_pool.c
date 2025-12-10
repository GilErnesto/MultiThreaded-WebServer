#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "thread_pool.h"
#include "http.h"
#include "cache.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <time.h>

// Estrutura de argumentos para threads
typedef struct {
    shared_data_t   *shared;
    semaphores_t    *sems;
    server_config_t *config;
    logger_t        *logger;
    cache_t         *cache;
    int             server_fd;
} thread_args_t;

// Declaração forward das funções
static void* worker_thread(void *arg);
static long handle_client(int client_fd, thread_args_t *args);

void thread_pool_start(shared_data_t *shared, 
                      semaphores_t *sems, 
                      server_config_t *config, 
                      logger_t *logger,
                      int server_fd) 
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
    args.server_fd = server_fd;

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
        // Consome uma conexão da fila (modelo producer-consumer)
        int client_fd = dequeue_connection(shared, sems);
        if (client_fd < 0) {
            perror("dequeue_connection failed (worker thread)");
            continue;
        }

        // Incrementar conexões ativas
        sem_wait(sems->stats);
        shared->stats.active_connections++;
        sem_post(sems->stats);

        // Marca o início do processamento do pedido
        struct timespec start_time, end_time;
        clock_gettime(CLOCK_MONOTONIC, &start_time);

        // Processar o pedido HTTP e obter bytes transferidos
        long bytes = handle_client(client_fd, args);

        // Marca o fim do processamento
        clock_gettime(CLOCK_MONOTONIC, &end_time);
        
        // Calcula tempo de resposta em segundos
        double response_time = (end_time.tv_sec - start_time.tv_sec) + 
                              (end_time.tv_nsec - start_time.tv_nsec) / 1000000000.0;

        // Atualizar estatísticas: bytes, tempo de resposta, e decrementar conexões ativas
        sem_wait(sems->stats);
        shared->stats.bytes_transferred += bytes;
        shared->stats.total_response_time += response_time;
        shared->stats.completed_requests++;
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

    // Endpoints especiais
    // Endpoint /stats - retorna estatísticas do servidor
    if (strcmp(req.path, "/stats") == 0 && strcmp(req.method, "GET") == 0) {
        sem_wait(args->sems->stats);
        
        double avg_response_time = 0.0;
        if (args->shared->stats.completed_requests > 0) {
            avg_response_time = args->shared->stats.total_response_time / args->shared->stats.completed_requests;
        }
        
        char stats_body[2048];
        snprintf(stats_body, sizeof(stats_body),
            "<!DOCTYPE html>\n"
            "<html><head><title>Server Statistics</title></head>\n"
            "<body>\n"
            "<h1>Server Statistics</h1>\n"
            "<table border='1'>\n"
            "<tr><td>Total Requests</td><td>%ld</td></tr>\n"
            "<tr><td>Completed Requests</td><td>%ld</td></tr>\n"
            "<tr><td>Bytes Transferred</td><td>%ld</td></tr>\n"
            "<tr><td>Average Response Time</td><td>%.4f s</td></tr>\n"
            "<tr><td>Active Connections</td><td>%d</td></tr>\n"
            "<tr><td>Queue Size</td><td>%d</td></tr>\n"
            "<tr><td>Status 200 (OK)</td><td>%ld</td></tr>\n"
            "<tr><td>Status 400 (Bad Request)</td><td>%ld</td></tr>\n"
            "<tr><td>Status 403 (Forbidden)</td><td>%ld</td></tr>\n"
            "<tr><td>Status 404 (Not Found)</td><td>%ld</td></tr>\n"
            "<tr><td>Status 500 (Internal Error)</td><td>%ld</td></tr>\n"
            "<tr><td>Status 501 (Not Implemented)</td><td>%ld</td></tr>\n"
            "<tr><td>Status 503 (Service Unavailable)</td><td>%ld</td></tr>\n"
            "</table>\n"
            "</body></html>\n",
            args->shared->stats.total_requests,
            args->shared->stats.completed_requests,
            args->shared->stats.bytes_transferred,
            avg_response_time,
            args->shared->stats.active_connections,
            args->shared->queue.count,
            args->shared->stats.status_200,
            args->shared->stats.status_400,
            args->shared->stats.status_403,
            args->shared->stats.status_404,
            args->shared->stats.status_500,
            args->shared->stats.status_501,
            args->shared->stats.status_503
        );
        sem_post(args->sems->stats);
        
        char response[4096];
        int resp_len = snprintf(response, sizeof(response),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "\r\n"
            "%s",
            strlen(stats_body), stats_body
        );
        
        long sent = send(client_fd, response, resp_len, 0);
        
        sem_wait(args->sems->stats);
        args->shared->stats.status_200++;
        sem_post(args->sems->stats);
        
        log_request(args->logger, req.method, req.path, req.version, 200, sent);
        close(client_fd);
        return sent;
    }
    
    // Endpoint /cause500 - para testes (gera erro 500 intencionalmente)
    if (strcmp(req.path, "/cause500") == 0) {
        long sent = send_error(client_fd,
                   "HTTP/1.1 500 Internal Server Error",
                   "<h1>500 Internal Server Error</h1><p>This error was intentionally triggered for testing.</p>");
        
        sem_wait(args->sems->stats);
        args->shared->stats.status_500++;
        sem_post(args->sems->stats);
        
        log_request(args->logger, req.method, req.path, req.version, 500, sent);
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
        // Raiz → index.html
        snprintf(fullpath, sizeof(fullpath), "%s/index.html",
                 args->config->document_root);
    } else {
        // Caminho normal
        snprintf(fullpath, sizeof(fullpath), "%s%s",
                 args->config->document_root, req.path);
        
        // Se o caminho termina em '/', tentar servir index.html desse diretório
        size_t len = strlen(fullpath);
        if (len > 0 && fullpath[len - 1] == '/') {
            // Verificar se há espaço suficiente para adicionar "index.html"
            if (len + 10 < sizeof(fullpath)) {
                strncat(fullpath, "index.html", sizeof(fullpath) - len - 1);
            }
        }
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

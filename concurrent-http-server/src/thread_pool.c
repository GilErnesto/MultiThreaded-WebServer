#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "thread_pool.h"
#include "http.h"
#include "cache.h"
#include "worker.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <time.h>

// fila local com condition variables
typedef struct {
    int *queue;
    int capacity;
    int size;
    int front;
    int rear;
    int stopping;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} local_queue_t;

typedef struct {
    shared_data_t   *shared;
    semaphores_t    *sems;
    server_config_t *config;
    logger_t        *logger;
    cache_t         *cache;
    int             server_fd;
    local_queue_t   *local_queue;
} thread_args_t;

static void* worker_thread(void *arg);
static void* dispatcher_thread(void *arg);
static long handle_client_request(int client_fd, HttpRequest *req, thread_args_t *args, int *keep_alive);

static local_queue_t* create_local_queue(int capacity) {
    local_queue_t *q = malloc(sizeof(local_queue_t));
    if (!q) return NULL;
    
    q->queue = malloc(capacity * sizeof(int));
    if (!q->queue) {
        free(q);
        return NULL;
    }
    
    q->capacity = capacity;
    q->size = 0;
    q->front = 0;
    q->rear = 0;
    q->stopping = 0;
    
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
    
    return q;
}

static void destroy_local_queue(local_queue_t *q) {
    if (!q) return;
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
    free(q->queue);
    free(q);
}

static void local_queue_push(local_queue_t *q, int fd) {
    pthread_mutex_lock(&q->mutex);
    
    // aguarda se fila cheia
    while (q->size >= q->capacity && !q->stopping) {
        pthread_cond_wait(&q->not_full, &q->mutex);
    }

    if (q->stopping) {
        pthread_mutex_unlock(&q->mutex);
        return;
    }
    
    q->queue[q->rear] = fd;
    q->rear = (q->rear + 1) % q->capacity;
    q->size++;
    
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
}

static int local_queue_pop(local_queue_t *q) {
    pthread_mutex_lock(&q->mutex);
    
    // aguarda se fila vazia
    while (q->size == 0 && !q->stopping) {
        pthread_cond_wait(&q->not_empty, &q->mutex);
    }

    if (q->stopping && q->size == 0) {
        pthread_mutex_unlock(&q->mutex);
        return -1;
    }
    
    int fd = q->queue[q->front];
    q->front = (q->front + 1) % q->capacity;
    q->size--;
    
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mutex);
    
    return fd;
}

void thread_pool_start(shared_data_t *shared, 
                      semaphores_t *sems, 
                      server_config_t *config, 
                      logger_t *logger,
                      int server_fd) 
{
    int num_threads = config->threads_per_worker;
    if (num_threads <= 0) num_threads = 1;

    cache_t *cache = malloc(sizeof(cache_t));
    if (!cache) {
        perror("malloc cache");
        exit(1);
    }
    
    size_t max_bytes = (size_t)config->cache_size_mb * 1024 * 1024;
    if (max_bytes == 0) max_bytes = 1 * 1024 * 1024;
    cache_init(cache, max_bytes);

    local_queue_t *local_queue = create_local_queue(config->max_queue_size);
    if (!local_queue) {
        perror("create_local_queue");
        exit(1);
    }

    thread_args_t args;
    args.shared = shared;
    args.sems = sems;
    args.config = config;
    args.logger = logger;
    args.cache = cache;
    args.server_fd = server_fd;
    args.local_queue = local_queue;

    pthread_t *threads = malloc((num_threads + 1) * sizeof(pthread_t));
    if (!threads) {
        perror("malloc threads");
        exit(1);
    }

    // dispatcher: aceita conexões e distribui para fila local
    if (pthread_create(&threads[0], NULL, dispatcher_thread, &args) != 0) {
        perror("pthread_create dispatcher");
        exit(1);
    }

    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&threads[i+1], NULL, worker_thread, &args) != 0) {
            perror("pthread_create worker");
            exit(1);
        }
    }

    while (!worker_shutdown) {
        pause();
    }

    pthread_mutex_lock(&local_queue->mutex);
    local_queue->stopping = 1;
    pthread_cond_broadcast(&local_queue->not_empty);
    pthread_cond_broadcast(&local_queue->not_full);
    pthread_mutex_unlock(&local_queue->mutex);

    pthread_join(threads[0], NULL);
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i+1], NULL);
    }

    destroy_local_queue(local_queue);
    cache_destroy(cache);
    free(cache);
    free(threads);
}

static void* dispatcher_thread(void *arg) {
    thread_args_t *args = (thread_args_t*)arg;
    local_queue_t *local_queue = args->local_queue;
    int server_fd = args->server_fd;
    printf("[WORKER PID=%d] Dispatcher thread started, will accept on fd=%d\n", getpid(), server_fd);
    
    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR && worker_shutdown) {
                break;
            }
            if (errno == EINTR) continue;
            if (errno == EBADF || errno == EINVAL) break;
            perror("accept failed (dispatcher)");
            continue;
        }
        local_queue_push(local_queue, client_fd);
    }

    pthread_mutex_lock(&local_queue->mutex);
    local_queue->stopping = 1;
    pthread_cond_broadcast(&local_queue->not_empty);
    pthread_cond_broadcast(&local_queue->not_full);
    pthread_mutex_unlock(&local_queue->mutex);
    return NULL;
}

static void* worker_thread(void *arg) {
    thread_args_t *args = (thread_args_t*)arg;
    shared_data_t *shared = args->shared;
    semaphores_t  *sems   = args->sems;
    local_queue_t *local_queue = args->local_queue;

    for (;;) {
        int client_fd = local_queue_pop(local_queue);
        if (client_fd < 0) {
            break;
        }
        
        if (client_fd < 3) {
            fprintf(stderr, "[WORKER THREAD PID=%d] Invalid fd from queue: %d\n", getpid(), client_fd);
            continue;
        }

        // Keep-Alive: até 50 requests por conexão
        const int max_requests = 50;
        int requests_count = 0;
        int keep_alive = 1;
        
        // incrementa active_connections uma vez por conexão
        sem_wait(sems->stats);
        shared->stats.active_connections++;
        sem_post(sems->stats);
        
        // timeout: 5s entre requests
        struct timeval timeout;
        timeout.tv_sec = 5;
        timeout.tv_usec = 0;
        if (setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
            if (errno != EBADF) {
                perror("setsockopt SO_RCVTIMEO (keep-alive)");
            }
        }

        while (keep_alive && requests_count < max_requests) {
            char buffer[BUFFER_SIZE];
            ssize_t bytes_read = read_http_request(client_fd, buffer, sizeof(buffer));
            
            if (bytes_read <= 0) {
                break;
            }
            
            buffer[bytes_read] = '\0';
            
            HttpRequest req;
            if (parse_http_request(buffer, &req) != 0) {
                long sent = send_error(client_fd,
                           "HTTP/1.1 400 Bad Request",
                           "<h1>400 Bad Request</h1>");
                
                sem_wait(sems->stats);
                shared->stats.status_400++;
                shared->stats.total_requests++;
                shared->stats.bytes_transferred += sent;
                sem_post(sems->stats);
                
                log_request(args->logger, NULL, NULL, NULL, 400, sent);
                keep_alive = 0;
                break;
            }
            
            // HTTP/1.0 não suporta Keep-Alive
            if (strcmp(req.version, "HTTP/1.0") == 0) {
                keep_alive = 0;
            }
            
            struct timespec start_time, end_time;
            clock_gettime(CLOCK_MONOTONIC, &start_time);
            
            sem_wait(sems->stats);
            shared->stats.total_requests++;
            sem_post(sems->stats);

            long bytes_sent = handle_client_request(client_fd, &req, args, &keep_alive);

            clock_gettime(CLOCK_MONOTONIC, &end_time);
            
            double response_time = (end_time.tv_sec - start_time.tv_sec) + 
                                  (end_time.tv_nsec - start_time.tv_nsec) / 1000000000.0;

            sem_wait(sems->stats);
            shared->stats.bytes_transferred += bytes_sent;
            shared->stats.total_response_time += response_time;
            shared->stats.completed_requests++;
            sem_post(sems->stats);
            
            requests_count++;
        }
        
        close(client_fd);
        
        sem_wait(sems->stats);
        shared->stats.active_connections--;
        sem_post(sems->stats);
    }

    return NULL;
}

static long handle_client_request(int client_fd, HttpRequest *req, thread_args_t *args, int *keep_alive) {
    // previne  "../"
    if (strstr(req->path, "..") != NULL) {
        sem_wait(args->sems->stats);
        args->shared->stats.status_403++;
        sem_post(args->sems->stats);
        
        long sent = send_error(client_fd,
                   "HTTP/1.1 403 Forbidden",
                   "<h1>403 Forbidden</h1>");
        
        log_request(args->logger, req->method, req->path, req->version, 403, sent);
        
        *keep_alive = 0;
        return sent;
    }

    // endpoint de teste: /cause400
    if (strcmp(req->path, "/cause400") == 0) {
        sem_wait(args->sems->stats);
        args->shared->stats.status_400++;
        sem_post(args->sems->stats);
        
        long sent = send_error(client_fd,
                   "HTTP/1.1 400 Bad Request",
                   "<h1>400 Bad Request</h1><p>This error was intentionally triggered for testing.</p>");
        
        log_request(args->logger, req->method, req->path, req->version, 400, sent);
        *keep_alive = 0;
        return sent;
    }
    
    // endpoint de teste: /cause501
    if (strcmp(req->path, "/cause501") == 0) {
        sem_wait(args->sems->stats);
        args->shared->stats.status_501++;
        sem_post(args->sems->stats);
        
        long sent = send_error(client_fd,
                   "HTTP/1.1 501 Not Implemented",
                   "<h1>501 Not Implemented</h1><p>This error was intentionally triggered for testing.</p>");
        
        log_request(args->logger, req->method, req->path, req->version, 501, sent);
        *keep_alive = 0;
        return sent;
    }
    
    // endpoint: /stats (retorna JSON)
    if (strcmp(req->path, "/stats") == 0 && (strcmp(req->method, "GET") == 0 || strcmp(req->method, "HEAD") == 0)) {
        sem_wait(args->sems->stats);
        
        double avg_response_time_ms = 0.0;
        if (args->shared->stats.completed_requests > 0) {
            avg_response_time_ms = (args->shared->stats.total_response_time / 
                                   args->shared->stats.completed_requests) * 1000.0;
        }
        
        time_t current_time = time(NULL);
        long uptime_seconds = (long)(current_time - args->shared->stats.server_start_time);
        
        char json_body[2048];
        snprintf(json_body, sizeof(json_body),
            "{\n"
            "  \"total_requests\": %ld,\n"
            "  \"total_bytes\": %ld,\n"
            "  \"requests_by_status\": {\n"
            "    \"200\": %ld,\n"
            "    \"400\": %ld,\n"
            "    \"403\": %ld,\n"
            "    \"404\": %ld,\n"
            "    \"500\": %ld,\n"
            "    \"501\": %ld,\n"
            "    \"503\": %ld\n"
            "  },\n"
            "  \"active_connections\": %d,\n"
            "  \"avg_response_time_ms\": %.2f,\n"
            "  \"uptime_seconds\": %ld\n"
            "}",
            args->shared->stats.total_requests,
            args->shared->stats.bytes_transferred,
            args->shared->stats.status_200,
            args->shared->stats.status_400,
            args->shared->stats.status_403,
            args->shared->stats.status_404,
            args->shared->stats.status_500,
            args->shared->stats.status_501,
            args->shared->stats.status_503,
            args->shared->stats.active_connections,
            avg_response_time_ms,
            uptime_seconds
        );
        
        // incrementa status_200 antes de enviar resposta (evitar deadlock)
        args->shared->stats.status_200++;
        sem_post(args->sems->stats);
        
        int send_body = (strcmp(req->method, "GET") == 0);
        long sent = send_json_response(client_fd, json_body, send_body);
        
        log_request(args->logger, req->method, req->path, req->version, 200, sent);
        return sent;
    }
    
    // endpoint: /dashboard (interface web)
    if (strcmp(req->path, "/dashboard") == 0 && (strcmp(req->method, "GET") == 0 || strcmp(req->method, "HEAD") == 0)) {
        char html_body[16384];
        generate_dashboard_html(html_body, sizeof(html_body));
        
        // incrementa status_200 antes de enviar resposta (evitar deadlock)
        sem_wait(args->sems->stats);
        args->shared->stats.status_200++;
        sem_post(args->sems->stats);
        
        int send_body = (strcmp(req->method, "GET") == 0);
        long sent = send_html_response(client_fd, html_body, send_body);
        
        log_request(args->logger, req->method, req->path, req->version, 200, sent);
        return sent;
    }
    
    // endpoint de teste: /cause500
    if (strcmp(req->path, "/cause500") == 0) {
        sem_wait(args->sems->stats);
        args->shared->stats.status_500++;
        sem_post(args->sems->stats);
        
        long sent = send_error(client_fd,
                   "HTTP/1.1 500 Internal Server Error",
                   "<h1>500 Internal Server Error</h1><p>This error was intentionally triggered for testing.</p>");
        
        log_request(args->logger, req->method, req->path, req->version, 500, sent);
        *keep_alive = 0;
        return sent;
    }

    if (strcmp(req->method, "GET") != 0 && strcmp(req->method, "HEAD") != 0) {
        sem_wait(args->sems->stats);
        args->shared->stats.status_501++;
        sem_post(args->sems->stats);
        
        long sent = send_error(client_fd,
                   "HTTP/1.1 501 Not Implemented",
                   "<h1>501 Not Implemented</h1>");
        
        log_request(args->logger, req->method, req->path, req->version, 501, sent);
        
        *keep_alive = 0;
        return sent;
    }

    // Virtual Host: escolhe document_root baseado no hostname
    const char* vroot = resolve_vhost_root(req->hostname, args->config);
    
    char fullpath[1024];
    if (strcmp(req->path, "/") == 0) {
        snprintf(fullpath, sizeof(fullpath), "%s/index.html", vroot);
    } else {
        snprintf(fullpath, sizeof(fullpath), "%s%s", vroot, req->path);
        
        size_t len = strlen(fullpath);
        if (len > 0 && fullpath[len - 1] == '/') {
            if (len + 10 < sizeof(fullpath)) {
                strncat(fullpath, "index.html", sizeof(fullpath) - len - 1);
            }
        }
    }

    int send_body = strcmp(req->method, "HEAD") != 0;
    
    long sent;
    
    // Range Request: envia apenas parte do ficheiro
    if (req->has_range) {
        if (access(fullpath, F_OK) != 0) {
            sem_wait(args->sems->stats);
            args->shared->stats.status_404++;
            sem_post(args->sems->stats);
            sent = send_error(client_fd, "HTTP/1.1 404 Not Found", "<h1>404 Not Found</h1>");
            log_request(args->logger, req->method, req->path, req->version, 404, sent);
        } else if (access(fullpath, R_OK) != 0) {
            sem_wait(args->sems->stats);
            args->shared->stats.status_403++;
            sem_post(args->sems->stats);
            sent = send_error(client_fd, "HTTP/1.1 403 Forbidden", "<h1>403 Forbidden</h1>");
            log_request(args->logger, req->method, req->path, req->version, 403, sent);
        } else {
            sem_wait(args->sems->stats);
            args->shared->stats.status_200++;  // ou criar stats.status_206
            sem_post(args->sems->stats);
            sent = send_file_range(client_fd, fullpath, send_body, req->range_start, req->range_end);
            log_request(args->logger, req->method, req->path, req->version, 206, sent);
        }
    } else {
        if (access(fullpath, F_OK) != 0) {
            sem_wait(args->sems->stats);
            args->shared->stats.status_404++;
            sem_post(args->sems->stats);
            sent = send_file_with_cache(client_fd, fullpath, send_body, args->cache);
            log_request(args->logger, req->method, req->path, req->version, 404, sent);
        } else if (access(fullpath, R_OK) != 0) {
            sem_wait(args->sems->stats);
            args->shared->stats.status_403++;
            sem_post(args->sems->stats);
            sent = send_file_with_cache(client_fd, fullpath, send_body, args->cache);
            log_request(args->logger, req->method, req->path, req->version, 403, sent);
        } else {
            sem_wait(args->sems->stats);
            args->shared->stats.status_200++;
            sem_post(args->sems->stats);
            sent = send_file_with_cache(client_fd, fullpath, send_body, args->cache);
            log_request(args->logger, req->method, req->path, req->version, 200, sent);
        }
    }
    
    return sent;
}

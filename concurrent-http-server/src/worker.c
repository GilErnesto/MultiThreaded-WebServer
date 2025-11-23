#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <stdlib.h>

#include "worker.h"
#include "http.h"
#include "cache.h"

// argumentos comuns a todas as threads do processo worker
typedef struct {
    shared_data_t   *shared;
    semaphores_t    *sems;
    server_config_t *config;
    FILE            *log_fp;
    cache_t         *cache;
} worker_thread_args_t;

static void log_request(worker_thread_args_t *args,
                        const char *method,
                        const char *path,
                        const char *version,
                        int status_code,
                        long bytes_sent)
{
    char timebuf[64];
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    strftime(timebuf, sizeof(timebuf), "%d/%b/%Y:%H:%M:%S %z", lt);

    // Formato aproximado de Apache Combined Log:
    // %h %l %u [%t] "%r" %>s %b "%{Referer}i" "%{User-agent}i"
    //
    // Aqui não temos IP, user, referer, user-agent → usamos "-"
    sem_wait(args->sems->log);
    fprintf(args->log_fp,
            "- - - [%s] \"%s %s %s\" %d %ld \"-\" \"-\"\n",
            timebuf,
            method  ? method  : "-",
            path    ? path    : "-",
            version ? version : "-",
            status_code,
            bytes_sent);
    fflush(args->log_fp);
    sem_post(args->sems->log);
}

// trata um pedido HTTP num socket já aceite
static long handle_client(int client_fd, worker_thread_args_t *args) {
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
        
        log_request(args, NULL, NULL, NULL, 400, sent);
        
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
        
        log_request(args, req.method, req.path, req.version, 403, sent);
        
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
        
        log_request(args, req.method, req.path, req.version, 501, sent);
        
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
        sent = send_file(client_fd, fullpath, send_body);
        sem_wait(args->sems->stats);
        args->shared->stats.status_404++;
        sem_post(args->sems->stats);
        log_request(args, req.method, req.path, req.version, 404, sent);
    } else if (access(fullpath, R_OK) != 0) {
        // Sem permissão - 403
        sent = send_file(client_fd, fullpath, send_body);
        sem_wait(args->sems->stats);
        args->shared->stats.status_403++;
        sem_post(args->sems->stats);
        log_request(args, req.method, req.path, req.version, 403, sent);
    } else {
        // Arquivo existe e é legível - 200
        sent = send_file(client_fd, fullpath, send_body);
        sem_wait(args->sems->stats);
        args->shared->stats.status_200++;
        sem_post(args->sems->stats);
        log_request(args, req.method, req.path, req.version, 200, sent);
    }
    
    close(client_fd);
    return sent;
}

// função executada por cada thread do pool
static void* worker_thread(void *arg) {
    worker_thread_args_t *args = (worker_thread_args_t*)arg;
    shared_data_t *shared = args->shared;
    semaphores_t  *sems   = args->sems;

    for (;;) {
        // consumidor: espera por trabalho na queue
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

void worker_loop(shared_data_t *shared, semaphores_t *sems, server_config_t *config) {
    // criar pool de threads neste processo worker
    int n = config->threads_per_worker;
    if (n <= 0) n = 1;

    // abre ficheiro de log em modo append
    FILE *log_fp = fopen(config->log_file, "a");
    if (!log_fp) {
        perror("fopen log_file");
        exit(1); // este processo worker morre se não conseguir loggar
    }

    // inicializa cache deste processo worker
    static cache_t cache;
    size_t max_bytes = (size_t)config->cache_size_mb * 1024 * 1024;
    if (max_bytes == 0) max_bytes = 1 * 1024 * 1024; // mínimo 1MB
    cache_init(&cache, max_bytes);

    // ligamos o módulo HTTP a esta cache
    http_set_cache(&cache);

    
    worker_thread_args_t args;
    args.shared = shared;
    args.sems   = sems;
    args.config = config;
    args.log_fp = log_fp;
    args.cache  = &cache;
    
    pthread_t threads[n];
    for (int i = 0; i < n; i++) {
        if (pthread_create(&threads[i], NULL, worker_thread, &args) != 0) {
            perror("pthread_create");
        }
    }

    // o processo worker mantém-se vivo; as threads fazem o trabalho
    // aqui basta bloquear para sempre
    for (;;) {
        pause();
    }
    // (em prática nunca chega aqui; se algum dia fizer shutdown limpo:
    // cache_destroy(&cache);
    // fclose(log_fp); )

}

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "worker.h"
#include "http.h"

// argumentos comuns a todas as threads do processo worker
typedef struct {
    shared_data_t   *shared;
    semaphores_t    *sems;
    server_config_t *config;
} worker_thread_args_t;

// trata um pedido HTTP num socket já aceite
static void handle_client(int client_fd, worker_thread_args_t *args) {
    char buffer[BUFFER_SIZE];
    ssize_t bytes = read_http_request(client_fd, buffer, sizeof(buffer));
    if (bytes <= 0) {
        close(client_fd);
        return;
    }

    buffer[bytes] = '\0';

    HttpRequest req;
    if (parse_http_request(buffer, &req) != 0) {
        send_error(client_fd,
                   "HTTP/1.1 400 Bad Request",
                   "<h1>400 Bad Request</h1>");
        close(client_fd);
        return;
    }

    // directory traversal
    if (strstr(req.path, "..") != NULL) {
        send_error(client_fd,
                   "HTTP/1.1 403 Forbidden",
                   "<h1>403 Forbidden</h1>");
        close(client_fd);
        return;
    }

    // apenas GET e HEAD
    if (strcmp(req.method, "GET") != 0 && strcmp(req.method, "HEAD") != 0) {
        send_error(client_fd,
                   "HTTP/1.1 501 Not Implemented",
                   "<h1>501 Not Implemented</h1>");
        close(client_fd);
        return;
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
    send_file(client_fd, fullpath, send_body);
    close(client_fd);
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

        // aqui processa o pedido HTTP
        handle_client(client_fd, args);
    }

    return NULL; // nunca chega aqui
}

void worker_loop(shared_data_t *shared, semaphores_t *sems, server_config_t *config) {
    // criar pool de threads neste processo worker
    int n = config->threads_per_worker;
    if (n <= 0) n = 1;

    pthread_t threads[n];

    worker_thread_args_t args;
    args.shared = shared;
    args.sems   = sems;
    args.config = config;

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
}

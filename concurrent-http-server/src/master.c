#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>

#include "config.h"
#include "http.h"
#include "worker.h"
#include "stats.h"
#include "master.h"
#include "logger.h"
#include "cache.h"

// Estrutura de argumentos para threads
typedef struct {
    shared_data_t   *shared;
    semaphores_t    *sems;
    server_config_t *config;
    logger_t        *logger;
    cache_t         *cache;
    int             server_fd;
} thread_args_t;

// Variáveis globais para shutdown gracioso
static volatile sig_atomic_t shutdown_requested = 0;
static int global_server_fd = -1;
static pthread_t *global_threads = NULL;
static int global_num_threads = 0;
static shared_data_t *global_shared = NULL;
static semaphores_t global_sems;
static logger_t *global_logger = NULL;
static cache_t *global_cache = NULL;

// Declaração forward
static long handle_request(int client_fd, thread_args_t *args);

// Função worker para threads
static void* worker_thread_func(void *arg) {
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
        long bytes = handle_request(client_fd, args);

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

    return NULL;
}

static void sigchld_handler(int sig) {
    (void)sig;
    int saved_errno = errno;

    // Recolhe todos os filhos terminados
    while (waitpid(-1, NULL, WNOHANG) > 0) {
        // nada
    }

    errno = saved_errno;
}

// Handler para SIGINT e SIGTERM - shutdown gracioso
static void shutdown_handler(int sig) {
    (void)sig;
    shutdown_requested = 1;
    
    // Fecha server_fd para quebrar accept()
    if (global_server_fd >= 0) {
        shutdown(global_server_fd, SHUT_RDWR);
        close(global_server_fd);
        global_server_fd = -1;
    }
}

// Função de cleanup completo
static void cleanup_resources(void) {
    printf("\n[SHUTDOWN] Cleaning up resources...\n");
    
    // Cancela todas as threads trabalhadoras
    if (global_threads && global_num_threads > 0) {
        printf("[SHUTDOWN] Cancelling %d worker threads...\n", global_num_threads);
        for (int i = 0; i < global_num_threads; i++) {
            pthread_cancel(global_threads[i]);
        }
        for (int i = 0; i < global_num_threads; i++) {
            pthread_join(global_threads[i], NULL);
        }
        free(global_threads);
        global_threads = NULL;
    }
    
    // Destroi logger
    if (global_logger) {
        printf("[SHUTDOWN] Destroying logger...\n");
        destroy_logger(global_logger);
        global_logger = NULL;
    }
    
    // Destroi cache
    if (global_cache) {
        printf("[SHUTDOWN] Destroying cache...\n");
        cache_destroy(global_cache);
        global_cache = NULL;
    }
    
    // Fecha e desfaz os semáforos
    printf("[SHUTDOWN] Destroying semaphores...\n");
    if (global_sems.empty) {
        sem_close(global_sems.empty);
        sem_unlink("/web_sem_empty");
    }
    if (global_sems.full) {
        sem_close(global_sems.full);
        sem_unlink("/web_sem_full");
    }
    if (global_sems.mutex) {
        sem_close(global_sems.mutex);
        sem_unlink("/web_sem_mutex");
    }
    if (global_sems.stats) {
        sem_close(global_sems.stats);
        sem_unlink("/web_sem_stats");
    }
    if (global_sems.log) {
        sem_close(global_sems.log);
        sem_unlink("/web_sem_log");
    }
    
    // Destroi shared memory
    if (global_shared) {
        printf("[SHUTDOWN] Destroying shared memory...\n");
        munmap(global_shared, sizeof(shared_data_t));
        shm_unlink("/webserver_shm");
        global_shared = NULL;
    }
    
    // Fecha server socket se ainda aberto
    if (global_server_fd >= 0) {
        close(global_server_fd);
        global_server_fd = -1;
    }
    
    printf("[SHUTDOWN] Cleanup complete. Exiting.\n");
}

int master_main(void) {
    server_config_t config;
    if (load_config("server.conf", &config) != 0) {
        printf("Erro a ler server.conf\n");
        exit(1);
    }
    
    // Registar atexit para cleanup
    atexit(cleanup_resources);

    // Registar handlers para SIGINT e SIGTERM
    struct sigaction sa_shutdown;
    memset(&sa_shutdown, 0, sizeof(sa_shutdown));
    sa_shutdown.sa_handler = shutdown_handler;
    sigemptyset(&sa_shutdown.sa_mask);
    sa_shutdown.sa_flags = 0;
    
    if (sigaction(SIGINT, &sa_shutdown, NULL) == -1) {
        perror("sigaction SIGINT");
        exit(1);
    }
    if (sigaction(SIGTERM, &sa_shutdown, NULL) == -1) {
        perror("sigaction SIGTERM");
        exit(1);
    }

    // cria socket TCP IPv4 (listen socket do master)
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    global_server_fd = server_fd;
    if (server_fd < 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(config.port);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 128) < 0) {
        perror("listen failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // Registar handler para SIGCHLD
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;

    if (sigaction(SIGCHLD, &sa, NULL) < 0) {
        perror("sigaction SIGCHLD");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // cria memória partilhada
    shared_data_t *shared = create_shared_memory();
    global_shared = shared;
    if (!shared) {
        fprintf(stderr, "Erro a criar memória partilhada\n");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // inicializa semáforos
    semaphores_t sems;
    if (init_semaphores(&sems, config.max_queue_size) != 0) {
        fprintf(stderr, "Erro a criar semáforos\n");
        destroy_shared_memory(shared);
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    global_sems = sems;

    printf("MASTER: listening on port %d\n", config.port);
    printf("Creating %d threads for request processing...\n", 
           config.num_workers * config.threads_per_worker);

    // Criar logger no processo principal
    logger_t *logger = create_logger(&sems, &config);
    global_logger = logger;
    if (!logger) {
        fprintf(stderr, "Erro ao criar logger\n");
        close(server_fd);
        destroy_semaphores(&sems);
        destroy_shared_memory(shared);
        exit(EXIT_FAILURE);
    }
    
    // Criar o pool de threads worker diretamente no master
    // Isso permite que as threads compartilhem FDs com o master
    int total_threads = config.num_workers * config.threads_per_worker;
    pthread_t *worker_threads = malloc(total_threads * sizeof(pthread_t));
    global_threads = worker_threads;
    global_num_threads = total_threads;
    
    // Preparar argumentos para as threads
    thread_args_t *thread_args = malloc(sizeof(thread_args_t));
    thread_args->shared = shared;
    thread_args->sems = &sems;
    thread_args->config = &config;
    thread_args->logger = logger;
    thread_args->cache = NULL;  // será criado por cada thread
    thread_args->server_fd = -1;
    
    // Inicializar cache global
    cache_t *cache = malloc(sizeof(cache_t));
    size_t max_bytes = (size_t)config.cache_size_mb * 1024 * 1024;
    if (max_bytes == 0) max_bytes = 1 * 1024 * 1024;
    cache_init(cache, max_bytes);
    thread_args->cache = cache;
    global_cache = cache;
    
    // Criar threads workers
    for (int i = 0; i < total_threads; i++) {
        if (pthread_create(&worker_threads[i], NULL, worker_thread_func, thread_args) != 0) {
            perror("pthread_create");
            exit(EXIT_FAILURE);
        }
    }

    // PROCESSO DE ESTATÍSTICAS
    pid_t stats_pid = fork();
    if (stats_pid == 0) {
        // FILHO ESPECIAL = PROCESSO DE ESTATÍSTICAS
        close(server_fd); // não precisa do socket
        
        while (1) {
            sleep(30);

            sem_wait(sems.stats);

            double avg_response_time = 0.0;
            if (shared->stats.completed_requests > 0) {
                avg_response_time = shared->stats.total_response_time / shared->stats.completed_requests;
            }

            printf("\n===== ESTATÍSTICAS =====\n");
            printf("Total requests:        %ld\n", shared->stats.total_requests);
            printf("Completed requests:    %ld\n", shared->stats.completed_requests);
            printf("Bytes transferred:     %ld\n", shared->stats.bytes_transferred);
            printf("Avg response time:     %.4f s\n", avg_response_time);
            printf("Status 200:            %ld\n", shared->stats.status_200);
            printf("Status 400:            %ld\n", shared->stats.status_400);
            printf("Status 403:            %ld\n", shared->stats.status_403);
            printf("Status 404:            %ld\n", shared->stats.status_404);
            printf("Status 500:            %ld\n", shared->stats.status_500);
            printf("Status 501:            %ld\n", shared->stats.status_501);
            printf("Status 503:            %ld\n", shared->stats.status_503);
            printf("Active connections:    %d\n", shared->stats.active_connections);
            printf("Queue size:            %d\n", shared->queue.count);
            printf("========================\n");

            sem_post(sems.stats);
        }
        exit(0);
    }

    // LOOP PRINCIPAL DO MASTER: aceita conexões e enfileira-as
    printf("MASTER: accepting connections and queuing them...\n");
    printf("Press Ctrl+C to shutdown gracefully...\n");
    
    while (!shutdown_requested) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (shutdown_requested) break; // shutdown pedido
            if (errno == EINTR) continue; // interrompido por sinal
            perror("accept failed");
            continue;
        }
        
        // Tenta enfileirar a conexão
        // Primeiro verifica se a fila está cheia (sem bloquear)
        int sem_val;
        sem_getvalue(sems.empty, &sem_val);
        
        if (sem_val == 0) {
            // Fila cheia - envia 503 Service Unavailable e fecha conexão
            const char *response = 
                "HTTP/1.1 503 Service Unavailable\r\n"
                "Content-Type: text/html\r\n"
                "Content-Length: 60\r\n"
                "Connection: close\r\n"
                "\r\n"
                "<html><body><h1>503 Service Unavailable</h1></body></html>";
            
            send(client_fd, response, strlen(response), 0);
            close(client_fd);
            
            // Atualiza estatísticas
            sem_wait(sems.stats);
            shared->stats.status_503++;
            sem_post(sems.stats);
            
            printf("MASTER: Queue full, sent 503 to client\n");
            continue;
        }
        
        // Incrementar total de requests
        sem_wait(sems.stats);
        shared->stats.total_requests++;
        sem_post(sems.stats);
        
        // Enfileira a conexão
        if (enqueue_connection(shared, &sems, client_fd) != 0) {
            perror("enqueue_connection failed");
            close(client_fd);
        }
    }

    // Cleanup será feito por cleanup_resources() via atexit
    printf("\nShutdown requested, exiting main loop...\n");
    return 0;
}

// Processa um pedido HTTP
static long handle_request(int client_fd, thread_args_t *args) {
    char buffer[BUFFER_SIZE];
    ssize_t bytes = read_http_request(client_fd, buffer, sizeof(buffer));
    
    if (bytes <= 0) {
        close(client_fd);
        return 0;
    }

    buffer[bytes] = '\0';

    HttpRequest req;
    if (parse_http_request(buffer, &req) != 0) {
        long sent = send_error(client_fd, "HTTP/1.1 400 Bad Request", "<h1>400 Bad Request</h1>");
        sem_wait(args->sems->stats);
        args->shared->stats.status_400++;
        sem_post(args->sems->stats);
        log_request(args->logger, NULL, NULL, NULL, 400, sent);
        close(client_fd);
        return sent;
    }

    // Verifica se o método é GET ou HEAD
    int is_head = (strcmp(req.method, "HEAD") == 0);
    int is_get  = (strcmp(req.method, "GET") == 0);
    
    if (!is_get && !is_head) {
        long sent = send_error(client_fd, "HTTP/1.1 501 Not Implemented", "<h1>501 Not Implemented</h1>");
        sem_wait(args->sems->stats);
        args->shared->stats.status_501++;
        sem_post(args->sems->stats);
        log_request(args->logger, req.method, req.path, req.version, 501, sent);
        close(client_fd);
        return sent;
    }

    // directory traversal
    if (strstr(req.path, "..") != NULL) {
        long sent = send_error(client_fd, "HTTP/1.1 403 Forbidden", "<h1>403 Forbidden</h1>");
        sem_wait(args->sems->stats);
        args->shared->stats.status_403++;
        sem_post(args->sems->stats);
        log_request(args->logger, req.method, req.path, req.version, 403, sent);
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

    // constrói caminho real
    char fullpath[1024];
    if (strcmp(req.path, "/") == 0) {
        snprintf(fullpath, sizeof(fullpath), "%s/index.html", args->config->document_root);
    } else {
        snprintf(fullpath, sizeof(fullpath), "%s%s", args->config->document_root, req.path);
        size_t len = strlen(fullpath);
        if (len > 0 && fullpath[len - 1] == '/' && len + 10 < sizeof(fullpath)) {
            strncat(fullpath, "index.html", sizeof(fullpath) - len - 1);
        }
    }

    int send_body = strcmp(req.method, "HEAD") != 0;
    long sent;
    
    if (access(fullpath, F_OK) != 0) {
        sent = send_file_with_cache(client_fd, fullpath, send_body, args->cache);
        sem_wait(args->sems->stats);
        args->shared->stats.status_404++;
        sem_post(args->sems->stats);
        log_request(args->logger, req.method, req.path, req.version, 404, sent);
    } else if (access(fullpath, R_OK) != 0) {
        sent = send_file_with_cache(client_fd, fullpath, send_body, args->cache);
        sem_wait(args->sems->stats);
        args->shared->stats.status_403++;
        sem_post(args->sems->stats);
        log_request(args->logger, req.method, req.path, req.version, 403, sent);
    } else {
        sent = send_file_with_cache(client_fd, fullpath, send_body, args->cache);
        sem_wait(args->sems->stats);
        args->shared->stats.status_200++;
        sem_post(args->sems->stats);
        log_request(args->logger, req.method, req.path, req.version, 200, sent);
    }
    
    close(client_fd);
    return sent;
}

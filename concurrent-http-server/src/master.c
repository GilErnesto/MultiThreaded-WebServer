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

#include "config.h"
#include "worker.h"
#include "stats.h"
#include "master.h"

static volatile sig_atomic_t shutdown_requested = 0;
static int global_server_fd = -1;
static pid_t *global_worker_pids = NULL;
static int global_num_workers = 0;
static shared_data_t *global_shared = NULL;
static semaphores_t global_sems;

static void sigchld_handler(int sig) {
    (void)sig;
    int saved_errno = errno;

    while (waitpid(-1, NULL, WNOHANG) > 0) {
    }

    errno = saved_errno;
}

// handler para shutdown gracioso
static void shutdown_handler(int sig) {
    (void)sig;
    shutdown_requested = 1;
    
    if (global_worker_pids && global_num_workers > 0) {
        for (int i = 0; i < global_num_workers; i++) {
            if (global_worker_pids[i] > 0) {
                kill(global_worker_pids[i], SIGTERM);
            }
        }
    }
    
    if (global_server_fd >= 0) {
        shutdown(global_server_fd, SHUT_RDWR);
        close(global_server_fd);
        global_server_fd = -1;
    }
}

static void cleanup_resources(void) {
    printf("\n[SHUTDOWN] Cleaning up resources...\n");
    
    if (global_worker_pids && global_num_workers > 0) {
        printf("[SHUTDOWN] Terminating %d worker processes...\n", global_num_workers);
        for (int i = 0; i < global_num_workers; i++) {
            if (global_worker_pids[i] > 0) {
                kill(global_worker_pids[i], SIGTERM);
            }
        }
        for (int i = 0; i < global_num_workers; i++) {
            if (global_worker_pids[i] > 0) {
                waitpid(global_worker_pids[i], NULL, 0);
            }
        }
        free(global_worker_pids);
        global_worker_pids = NULL;
    }
    
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
    
    if (global_shared) {
        printf("[SHUTDOWN] Destroying shared memory...\n");
        munmap(global_shared, sizeof(shared_data_t));
        shm_unlink("/webserver_shm");
        global_shared = NULL;
    }
    
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
    
    atexit(cleanup_resources);

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
    
    // SO_REUSEPORT permite múltiplos processos aceitarem no mesmo socket
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEPORT failed");
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

    shared_data_t *shared = create_shared_memory();
    global_shared = shared;
    if (!shared) {
        fprintf(stderr, "Erro a criar memória partilhada\n");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    semaphores_t sems;
    if (init_semaphores(&sems, config.max_queue_size) != 0) {
        fprintf(stderr, "Erro a criar semáforos\n");
        destroy_shared_memory(shared);
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    global_sems = sems;

    printf("MASTER: listening on port %d\n", config.port);
    printf("Creating %d worker processes with %d threads each...\n", 
           config.num_workers, config.threads_per_worker);

    pid_t *worker_pids = malloc(config.num_workers * sizeof(pid_t));
    global_worker_pids = worker_pids;
    global_num_workers = config.num_workers;
    
    if (!worker_pids) {
        perror("malloc worker_pids");
        close(server_fd);
        destroy_semaphores(&sems);
        destroy_shared_memory(shared);
        exit(EXIT_FAILURE);
    }
    
    for (int i = 0; i < config.num_workers; i++) {
        pid_t pid = fork();
        
        if (pid < 0) {
            perror("fork worker");
            exit(EXIT_FAILURE);
        }
        
        if (pid == 0) {
            // processo worker
            printf("[WORKER %d] Process started (PID=%d)\n", i, getpid());
            fflush(stdout);
            
            free(worker_pids);
            global_worker_pids = NULL;
            global_num_workers = 0;
            
            worker_loop(shared, &sems, &config, server_fd);
            
            fprintf(stderr, "[WORKER %d] ERRO: worker_loop retornou!\n", i);
            _exit(1);
        }
        
        worker_pids[i] = pid;
        printf("[MASTER] Created worker %d with PID %d\n", i, pid);
    }

    pid_t stats_pid = fork();
    if (stats_pid == 0) {
        // processo de estatísticas
        close(server_fd);
        
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

    printf("MASTER: Workers will accept connections using SO_REUSEPORT...\n");
    printf("Press Ctrl+C to shutdown gracefully...\n");

    while (!shutdown_requested) {
        pause();
    }

    printf("\nShutdown requested, exiting main loop...\n");
    return 0;
}

#define _POSIX_C_SOURCE 200112L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

typedef struct {
    struct addrinfo *addr;
    char host[256];
    char path[256];
    int requests_per_thread;
} thread_args_t;

static pthread_mutex_t stats_mutex = PTHREAD_MUTEX_INITIALIZER;
static long total_success = 0;
static long total_fail = 0;

static void inc_success(void) {
    pthread_mutex_lock(&stats_mutex);
    total_success++;
    pthread_mutex_unlock(&stats_mutex);
}

static void inc_fail(void) {
    pthread_mutex_lock(&stats_mutex);
    total_fail++;
    pthread_mutex_unlock(&stats_mutex);
}

static void *worker_thread(void *arg) {
    thread_args_t *targs = (thread_args_t *)arg;
    char request[1024];
    char buffer[4096];

    for (int i = 0; i < targs->requests_per_thread; i++) {
        int sock = socket(targs->addr->ai_family, targs->addr->ai_socktype, targs->addr->ai_protocol);
        if (sock < 0) {
            inc_fail();
            continue;
        }

        if (connect(sock, targs->addr->ai_addr, targs->addr->ai_addrlen) < 0) {
            close(sock);
            inc_fail();
            continue;
        }

        int req_len = snprintf(request, sizeof(request),
                               "GET %s HTTP/1.0\r\n"
                               "Host: %s\r\n"
                               "Connection: close\r\n"
                               "\r\n",
                               targs->path, targs->host);
        if (req_len <= 0 || req_len >= (int)sizeof(request)) {
            close(sock);
            inc_fail();
            continue;
        }

        ssize_t sent = send(sock, request, req_len, 0);
        if (sent != req_len) {
            close(sock);
            inc_fail();
            continue;
        }

        int ok = 0;
        ssize_t n = recv(sock, buffer, sizeof(buffer) - 1, 0);
        if (n > 0) {
            buffer[n] = '\0';
            if (strncmp(buffer, "HTTP/1.1 200", 12) == 0 ||
                strncmp(buffer, "HTTP/1.0 200", 12) == 0) {
                ok = 1;
            }
            while ((n = recv(sock, buffer, sizeof(buffer), 0)) > 0) {
                /* descartar restante resposta */
            }
        }


        close(sock);

        if (ok) {
            inc_success();
        } else {
            inc_fail();
        }
    }

    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 6) {
        fprintf(stderr,
                "Uso: %s <host> <porto> <path> <num_threads> <reqs_por_thread>\n",
                argv[0]);
        return 1;
    }

    const char *host = argv[1];
    const char *port_str = argv[2];
    const char *path = argv[3];
    char *endptr = NULL;

    long num_threads_l = strtol(argv[4], &endptr, 10);
    if (*argv[4] == '\0' || *endptr != '\0' || num_threads_l <= 0) {
        fprintf(stderr, "num_threads inválido: %s\n", argv[4]);
        return 1;
    }

    long reqs_per_thread_l = strtol(argv[5], &endptr, 10);
    if (*argv[5] == '\0' || *endptr != '\0' || reqs_per_thread_l <= 0) {
        fprintf(stderr, "reqs_por_thread inválido: %s\n", argv[5]);
        return 1;
    }

    int num_threads = (int)num_threads_l;
    int requests_per_thread = (int)reqs_per_thread_l;

    struct addrinfo hints;
    struct addrinfo *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;      // força IPv4
    hints.ai_socktype = SOCK_STREAM;


    int ret = getaddrinfo(host, port_str, &hints, &res);
    if (ret != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ret));
        return 1;
    }

    thread_args_t targs;
    targs.addr = res;
    strncpy(targs.host, host, sizeof(targs.host) - 1);
    targs.host[sizeof(targs.host) - 1] = '\0';
    strncpy(targs.path, path, sizeof(targs.path) - 1);
    targs.path[sizeof(targs.path) - 1] = '\0';
    targs.requests_per_thread = requests_per_thread;

    pthread_t *threads = malloc(sizeof(pthread_t) * (size_t)num_threads);
    if (!threads) {
        fprintf(stderr, "malloc falhou\n");
        freeaddrinfo(res);
        return 1;
    }

    for (int i = 0; i < num_threads; i++) {
        int err = pthread_create(&threads[i], NULL, worker_thread, &targs);
        if (err != 0) {
            fprintf(stderr, "pthread_create falhou: %s\n", strerror(err));
            free(threads);
            freeaddrinfo(res);
            return 1;
        }
    }

    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    free(threads);
    freeaddrinfo(res);

    long total_reqs = (long)num_threads * (long)requests_per_thread;

    printf("Threads: %d\n", num_threads);
    printf("Pedidos por thread: %d\n", requests_per_thread);
    printf("Total de pedidos: %ld\n", total_reqs);
    printf("Sucessos: %ld\n", total_success);
    printf("Falhas: %ld\n", total_fail);

    return (total_fail == 0) ? 0 : 1;
}

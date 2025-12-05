#include "logger.h"

#include <stdlib.h>
#include <time.h>
#include <string.h>

logger_t* create_logger(semaphores_t *sems, server_config_t *config) {
    logger_t *logger = malloc(sizeof(logger_t));
    if (!logger) {
        perror("malloc logger");
        return NULL;
    }

    // Abre ficheiro de log em modo append
    FILE *log_fp = fopen(config->log_file, "a");
    if (!log_fp) {
        perror("fopen log_file");
        free(logger);
        return NULL;
    }

    logger->sems = sems;
    logger->config = config;
    logger->log_fp = log_fp;

    return logger;
}

void destroy_logger(logger_t *logger) {
    if (!logger) return;
    
    if (logger->log_fp) {
        fclose(logger->log_fp);
    }
    
    free(logger);
}

void log_request(logger_t *logger,
                const char *method,
                const char *path,
                const char *version,
                int status_code,
                long bytes_sent)
{
    if (!logger || !logger->log_fp) return;
    
    char timebuf[64];
    time_t now = time(NULL);
    struct tm tm_buf;
    struct tm *lt = localtime_r(&now, &tm_buf);
    if (!lt) return;  // Erro ao converter tempo
    strftime(timebuf, sizeof(timebuf), "%d/%b/%Y:%H:%M:%S %z", lt);

    // Formato aproximado de Apache Combined Log:
    // %h %l %u [%t] "%r" %>s %b "%{Referer}i" "%{User-agent}i"
    //
    // Aqui não temos IP, user, referer, user-agent → usamos "-"
    sem_wait(logger->sems->log);
    fprintf(logger->log_fp,
            "- - - [%s] \"%s %s %s\" %d %ld \"-\" \"-\"\n",
            timebuf,
            method  ? method  : "-",
            path    ? path    : "-",
            version ? version : "-",
            status_code,
            bytes_sent);
    fflush(logger->log_fp);
    sem_post(logger->sems->log);
}

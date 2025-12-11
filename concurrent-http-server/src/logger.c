#include "logger.h"

#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <sys/stat.h>
#include <stdio.h>
#include <unistd.h>

#define MAX_LOG_SIZE (10 * 1024 * 1024)

// faz rotação do log se exceder 10MB
static void check_and_rotate_log(logger_t *logger) {
    if (!logger || !logger->log_fp || !logger->config) return;
    
    struct stat st;
    if (fstat(fileno(logger->log_fp), &st) != 0) {
        return;
    }
    
    if (st.st_size < MAX_LOG_SIZE) {
        return;
    }
    
    printf("[LOG] Log file exceeded 10MB, performing rotation...\n");
    
    fclose(logger->log_fp);
    char old_path[512];
    snprintf(old_path, sizeof(old_path), "%s.old", logger->config->log_file);
    
    // Remove .old anterior se existir
    unlink(old_path);
    
    // Renomeia atual para .old
    if (rename(logger->config->log_file, old_path) != 0) {
        perror("rename log file");
    }
    
    // Reabrir ficheiro (agora vazio)
    logger->log_fp = fopen(logger->config->log_file, "a");
    if (!logger->log_fp) {
        perror("fopen log_file after rotation");
    } else {
        printf("[LOG] Log rotation completed. Old log saved to %s\n", old_path);
    }
}

logger_t* create_logger(semaphores_t *sems, server_config_t *config) {
    logger_t *logger = malloc(sizeof(logger_t));
    if (!logger) {
        perror("malloc logger");
        return NULL;
    }

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
    struct tm *lt = gmtime_r(&now, &tm_buf);
    if (!lt) return;
    strftime(timebuf, sizeof(timebuf), "%d/%b/%Y:%H:%M:%S +0000", lt);

    sem_wait(logger->sems->log);
    check_and_rotate_log(logger);
    
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

#include "logger.h"

#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <sys/stat.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#define MAX_LOG_SIZE (10 * 1024 * 1024)

// faz rotação do log se exceder 10MB
static void check_and_rotate_log(logger_t *logger) {
    if (!logger || logger->log_fd < 0 || !logger->config) return;
    
    struct stat st;
    if (fstat(logger->log_fd, &st) != 0) {
        return;
    }
    
    if (st.st_size < MAX_LOG_SIZE) {
        return;
    }
    
    printf("[LOG] Log file exceeded 10MB, performing rotation...\n");
    
    close(logger->log_fd);
    char old_path[512];
    snprintf(old_path, sizeof(old_path), "%s.old", logger->config->log_file);
    
    // Remove .old anterior se existir
    unlink(old_path);
    
    // Renomeia atual para .old
    if (rename(logger->config->log_file, old_path) != 0) {
        perror("rename log file");
    }
    
    // Reabrir ficheiro (agora vazio) com O_APPEND para escritas atómicas
    logger->log_fd = open(logger->config->log_file, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (logger->log_fd < 0) {
        perror("open log_file after rotation");
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

    // Use open() with O_APPEND for atomic appends instead of FILE*
    int log_fd = open(config->log_file, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_fd < 0) {
        perror("open log_file");
        free(logger);
        return NULL;
    }

    logger->sems = sems;
    logger->config = config;
    logger->log_fd = log_fd;

    return logger;
}

void destroy_logger(logger_t *logger) {
    if (!logger) return;
    
    if (logger->log_fd >= 0) {
        close(logger->log_fd);
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
    if (!logger || logger->log_fd < 0) return;
    
    char timebuf[64];
    time_t now = time(NULL);
    struct tm tm_buf;
    struct tm *lt = gmtime_r(&now, &tm_buf);
    if (!lt) return;
    strftime(timebuf, sizeof(timebuf), "%d/%b/%Y:%H:%M:%S +0000", lt);

    char log_line[1024];
    int len = snprintf(log_line, sizeof(log_line),
                       "- - - [%s] \"%s %s %s\" %d %ld \"-\" \"-\"\n",
                       timebuf,
                       method  ? method  : "-",
                       path    ? path    : "-",
                       version ? version : "-",
                       status_code,
                       bytes_sent);
    
    if (len <= 0 || len >= (int)sizeof(log_line)) return;

    sem_wait(logger->sems->log);
    check_and_rotate_log(logger);
    
    // write() with O_APPEND is atomic for appends up to PIPE_BUF bytes
    // No need for fflush(), kernel handles atomicity
    ssize_t written = write(logger->log_fd, log_line, len);
    if (written < 0) {
        perror("write log");
    }
    
    sem_post(logger->sems->log);
}

#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

// remove espaços em branco no início e fim de uma string
static void trim(char *str) {
    char *end;
    while (isspace((unsigned char)*str)) str++;
    if (*str == 0) return;
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    *(end + 1) = 0;
}

int load_config(const char* filename, server_config_t* config) {
    FILE* fp = fopen(filename, "r");
    if (!fp) return -1;

    config->num_vhosts = 0;
    config->default_vhost[0] = '\0';

    char line[512], key[128], value[256];

    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;

        if (sscanf(line, "%127[^=]=%255[^\n]", key, value) == 2) {
            trim(key);
            trim(value);

            if (strcmp(key, "PORT") == 0)
                config->port = atoi(value);

            else if (strcmp(key, "DOCUMENT_ROOT") == 0)
                strncpy(config->document_root, value, sizeof(config->document_root) - 1);

            else if (strcmp(key, "NUM_WORKERS") == 0)
                config->num_workers = atoi(value);

            else if (strcmp(key, "THREADS_PER_WORKER") == 0)
                config->threads_per_worker = atoi(value);

            else if (strcmp(key, "MAX_QUEUE_SIZE") == 0)
                config->max_queue_size = atoi(value);

            else if (strcmp(key, "LOG_FILE") == 0)
                strncpy(config->log_file, value, sizeof(config->log_file) - 1);

            else if (strcmp(key, "CACHE_SIZE_MB") == 0)
                config->cache_size_mb = atoi(value);

            else if (strcmp(key, "TIMEOUT_SECONDS") == 0)
                config->timeout_seconds = atoi(value);

            else if (strcmp(key, "DEFAULT_VHOST") == 0)
                strncpy(config->default_vhost, value, sizeof(config->default_vhost) - 1);

            // parsing de virtual hosts: VHOST_hostname=document_root
            else if (strncmp(key, "VHOST_", 6) == 0) {
                if (config->num_vhosts < MAX_VHOSTS) {
                    const char* hostname = key + 6;
                    strncpy(config->vhosts[config->num_vhosts].hostname, hostname, 
                           sizeof(config->vhosts[config->num_vhosts].hostname) - 1);
                    strncpy(config->vhosts[config->num_vhosts].document_root, value,
                           sizeof(config->vhosts[config->num_vhosts].document_root) - 1);
                    config->num_vhosts++;
                }
            }
        }
    }

    fclose(fp);
    
    if (config->port <= 0 || config->port > 65535) {
        fprintf(stderr, "ERROR: PORT não configurado ou inválido (deve estar entre 1-65535)\n");
        return -1;
    }
    if (config->num_workers <= 0) {
        fprintf(stderr, "ERROR: NUM_WORKERS deve ser > 0\n");
        return -1;
    }
    if (config->threads_per_worker <= 0) {
        fprintf(stderr, "ERROR: THREADS_PER_WORKER deve ser > 0\n");
        return -1;
    }
    if (config->max_queue_size <= 0) {
        fprintf(stderr, "ERROR: MAX_QUEUE_SIZE deve ser > 0\n");
        return -1;
    }
    if (config->cache_size_mb < 0) {
        fprintf(stderr, "ERROR: CACHE_SIZE_MB deve ser >= 0\n");
        return -1;
    }
    if (config->timeout_seconds <= 0) {
        fprintf(stderr, "ERROR: TIMEOUT_SECONDS deve ser > 0\n");
        return -1;
    }
    if (config->document_root[0] == '\0') {
        fprintf(stderr, "ERROR: DOCUMENT_ROOT não configurado\n");
        return -1;
    }
    if (config->log_file[0] == '\0') {
        fprintf(stderr, "ERROR: LOG_FILE não configurado\n");
        return -1;
    }
    
    return 0;
}

// resolve o document_root baseado no hostname (virtual host support)
const char* resolve_vhost_root(const char* hostname, server_config_t *config) {
    if (!hostname || hostname[0] == '\0') {
           // sem hostname → usa default_vhost ou DOCUMENT_ROOT
        if (config->default_vhost[0] != '\0') {
            hostname = config->default_vhost;
        } else {
            return config->document_root;
        }
    }

    // remove porta do hostname se existir (ex: "example.com:8080" → "example.com")
    char clean_hostname[256];
    strncpy(clean_hostname, hostname, sizeof(clean_hostname) - 1);
    clean_hostname[sizeof(clean_hostname) - 1] = '\0';
    
    char *colon = strchr(clean_hostname, ':');
    if (colon) *colon = '\0';

    // procura o hostname nos vhosts configurados
    for (int i = 0; i < config->num_vhosts; i++) {
        if (strcasecmp(config->vhosts[i].hostname, clean_hostname) == 0) {
            return config->vhosts[i].document_root;
        }
    }

    // se não encontrou, tenta usar o default_vhost
    if (config->default_vhost[0] != '\0') {
        for (int i = 0; i < config->num_vhosts; i++) {
            if (strcasecmp(config->vhosts[i].hostname, config->default_vhost) == 0) {
                return config->vhosts[i].document_root;
            }
        }
    }

    return config->document_root;
}

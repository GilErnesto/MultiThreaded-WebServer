#ifndef CONFIG_H
#define CONFIG_H

#define MAX_VHOSTS 10

typedef struct {
    char hostname[256];      // ex: "example.com", "api.example.com"
    char document_root[512]; // ex: "/var/www/example.com", "/var/www/api"
} vhost_t;

typedef struct {
    int port;
    char document_root[256];
    int num_workers;
    int threads_per_worker;
    int max_queue_size;
    char log_file[256];
    int cache_size_mb;
    int timeout_seconds;
    vhost_t vhosts[MAX_VHOSTS];  // virtual hosts configurados
    int num_vhosts;              // número de vhosts ativos
    char default_vhost[256];     // hostname por omissão
} server_config_t;

int load_config(const char* filename, server_config_t* config);
const char* resolve_vhost_root(const char* hostname, server_config_t *config);

#endif

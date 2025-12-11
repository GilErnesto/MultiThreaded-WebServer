#ifndef HTTP_H
#define HTTP_H

#include <sys/types.h>
#include <stddef.h>
#include "cache.h"

#define BUFFER_SIZE 1024

typedef struct {
    char method[16];
    char path[512];
    char version[16];
    long range_start;  // -1 se não há range
    long range_end;    // -1 se não há range ou open-ended
    int has_range;     // 0 ou 1
    char hostname[256]; // extraído do header Host (para virtual hosts)
} HttpRequest;

const char* get_mime_type(const char* path);
long send_error(int client_fd, const char* status_line, const char* body);
int parse_http_request(const char *buffer, HttpRequest *req);
ssize_t read_http_request(int client_fd, char *buffer, size_t size);
long send_file(int client_fd, const char* fullpath, int send_body);
long send_file_with_cache(int client_fd, const char* fullpath, int send_body, cache_t *cache);
long send_file_range(int client_fd, const char* fullpath, int send_body, long range_start, long range_end);
long send_json_response(int client_fd, const char* json_body, int send_body);
long send_html_response(int client_fd, const char* html_body, int send_body);
void generate_dashboard_html(char *buffer, size_t buffer_size);

#endif

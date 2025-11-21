#ifndef HTTP_H
#define HTTP_H
#include <sys/types.h>
#include <stddef.h>

// tamanho padrão de buffer
#define BUFFER_SIZE 1024

// estrutura do pedido HTTP
typedef struct {
    char method[16];
    char path[512];
    char version[16];
} HttpRequest;

//  PROTÓTIPOS

const char* get_mime_type(const char* path);
void send_error(int client_fd, const char* status_line, const char* body);
int parse_http_request(const char *buffer, HttpRequest *req);
ssize_t read_http_request(int client_fd, char *buffer, size_t size);
void send_file(int client_fd, const char* fullpath, int send_body);
#endif

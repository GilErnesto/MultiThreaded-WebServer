#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <time.h>

#define PORT 8080
#define BUFFER_SIZE 1024

typedef struct {
    char method[16];
    char path[512];
    char version[16];
} HttpRequest;

const char* get_mime_type(const char* path) {
    const char* ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";

    if (strcmp(ext, ".html") == 0) return "text/html";
    if (strcmp(ext, ".css")  == 0) return "text/css";
    if (strcmp(ext, ".js")   == 0) return "application/javascript";
    if (strcmp(ext, ".png")  == 0) return "image/png";
    if (strcmp(ext, ".jpg")  == 0) return "image/jpeg";

    return "application/octet-stream";
}

// envia uma resposta simples de erro (400, 403, 404, 500, etc.)
void send_error(int client_fd, const char* status_line, const char* body) {
    char headers[256];
    size_t body_len = strlen(body);

    char date_header[128];
    time_t now = time(NULL);
    struct tm *gmt = gmtime(&now);
    strftime(date_header, sizeof(date_header), "%a, %d %b %Y %H:%M:%S GMT", gmt);

    snprintf(headers, sizeof(headers),
    "%s\r\n"
    "Content-Type: text/html\r\n"
    "Content-Length: %zu\r\n"
    "Server: ConcurrentHTTP/1.0\r\n"
    "Date: %s\r\n"
    "Connection: close\r\n"
    "\r\n",
    status_line, body_len, date_header);

    
    send(client_fd, headers, strlen(headers), 0);
    send(client_fd, body, body_len, 0);
}

int parse_http_request(const char *buffer, HttpRequest *req) {

    char local[BUFFER_SIZE];
    strncpy(local, buffer, sizeof(local) - 1);
    local[sizeof(local) - 1] = '\0';

    char *line_end = strstr(local, "\r\n");
    if (!line_end) {
        return -1;
    }
    *line_end = '\0';

    // request line: METHOD PATH VERSION
    char method[16], path[512], version[16];
    if (sscanf(local, "%15s %511s %15s", method, path, version) != 3) {
        return -1;
    }

    // validações mínimas da versão
    if (strcmp(version, "HTTP/1.1") != 0 && strcmp(version, "HTTP/1.0") != 0) {
        return -1;
    }

    // copia para a struct
    strncpy(req->method, method, sizeof(req->method) - 1);
    req->method[sizeof(req->method) - 1] = '\0';

    strncpy(req->path, path, sizeof(req->path) - 1);
    req->path[sizeof(req->path) - 1] = '\0';

    strncpy(req->version, version, sizeof(req->version) - 1);
    req->version[sizeof(req->version) - 1] = '\0';

    return 0;
}

// lê o pedido HTTP até ao fim dos headers (\r\n\r\n) ou encher o buffer
ssize_t read_http_request(int client_fd, char *buffer, size_t size) {
    size_t total = 0;

    while (total < size - 1) {
        ssize_t n = recv(client_fd, buffer + total, size - 1 - total, 0);
        if (n <= 0) {
            break;
        }
        total += n;
        buffer[total] = '\0';

        // fim dos headers: \r\n\r\n
        if (strstr(buffer, "\r\n\r\n") != NULL) {
            break;
        }
    }

    buffer[total] = '\0';
    return (ssize_t)total;
}

// envia um ficheiro como resposta HTTP 200, opcionalmente sem corpo (HEAD)
void send_file(int client_fd, const char* fullpath, int send_body) {
    // verifica existência e permissões primeiro para distinguir 404/403
    if (access(fullpath, F_OK) != 0) {
        send_error(client_fd,
                   "HTTP/1.1 404 Not Found",
                   "<h1>404 Not Found</h1>");
        return;
    }

    if (access(fullpath, R_OK) != 0) {
        send_error(client_fd,
                   "HTTP/1.1 403 Forbidden",
                   "<h1>403 Forbidden</h1>");
        return;
    }

    FILE* file = fopen(fullpath, "rb");
    if (!file) {
        // erro inesperado ao abrir → 500
        send_error(client_fd,
                   "HTTP/1.1 500 Internal Server Error",
                   "<h1>500 Internal Server Error</h1>");
        return;
    }

    // tamanho do ficheiro
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        send_error(client_fd,
                   "HTTP/1.1 500 Internal Server Error",
                   "<h1>500 Internal Server Error</h1>");
        return;
    }

    long file_size = ftell(file);
    if (file_size < 0) {
        fclose(file);
        send_error(client_fd,
                   "HTTP/1.1 500 Internal Server Error",
                   "<h1>500 Internal Server Error</h1>");
        return;
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        send_error(client_fd,
                   "HTTP/1.1 500 Internal Server Error",
                   "<h1>500 Internal Server Error</h1>");
        return;
    }

    char date_header[128];
    time_t now = time(NULL);
    struct tm *gmt = gmtime(&now);
    strftime(date_header, sizeof(date_header), "%a, %d %b %Y %H:%M:%S GMT", gmt);

    // headers 200 OK
    const char* mime = get_mime_type(fullpath);
    char headers[256];
    snprintf(headers, sizeof(headers),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Server: ConcurrentHTTP/1.0\r\n"
        "Date: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        mime, file_size, date_header);


    send(client_fd, headers, strlen(headers), 0);

    // HEAD → não envia corpo
    if (!send_body) {
        fclose(file);
        return;
    }

    // corpo
    char file_buffer[1024];
    size_t bytes_read;
    while ((bytes_read = fread(file_buffer, 1, sizeof(file_buffer), file)) > 0) {
        send(client_fd, file_buffer, bytes_read, 0);
    }

    fclose(file);
}

int main(void) {
    // cria socket TCP IPv4
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    // reutilização rápida do porto
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // bind ao porto escolhido em todas as interfaces
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // começa a ouvir ligações
    if (listen(server_fd, 10) < 0) {
        perror("listen failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("Server listening on port %d...\n", PORT);

    // loop principal: aceita e trata pedidos sequencialmente
    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            perror("accept failed");
            continue;
        }

        char buffer[BUFFER_SIZE];
        ssize_t bytes = read_http_request(client_fd, buffer, sizeof(buffer));
        if (bytes <= 0) {
            close(client_fd);
            continue;
        }

        // garantir string terminada
        buffer[bytes] = '\0';

        HttpRequest req;
        if (parse_http_request(buffer, &req) != 0) {
            // pedido inválido → 400
            send_error(client_fd,
                       "HTTP/1.1 400 Bad Request",
                       "<h1>400 Bad Request</h1>");
            close(client_fd);
            continue;
        }

        // sanitização básica do caminho para evitar directory traversal
        if (strstr(req.path, "..") != NULL) {
            send_error(client_fd,
                       "HTTP/1.1 403 Forbidden",
                       "<h1>403 Forbidden</h1>");
            close(client_fd);
            continue;
        }

        // só GET e HEAD são suportados
        if (strcmp(req.method, "GET") != 0 && strcmp(req.method, "HEAD") != 0) {
            // método não implementado → 501
            send_error(client_fd,
                       "HTTP/1.1 501 Not Implemented",
                       "<h1>501 Not Implemented</h1>");
            close(client_fd);
            continue;
        }

        // constrói caminho real a partir da DOCUMENT_ROOT "./www"
        char fullpath[1024];
        if (strcmp(req.path, "/") == 0) {
            snprintf(fullpath, sizeof(fullpath), "./www/index.html");
        } else {
            snprintf(fullpath, sizeof(fullpath), "./www%s", req.path);
        }

        // decide se envia corpo (HEAD não envia)
        int send_body = strcmp(req.method, "HEAD") != 0;
        send_file(client_fd, fullpath, send_body);
        close(client_fd);
    }

    close(server_fd);
    return 0;
}

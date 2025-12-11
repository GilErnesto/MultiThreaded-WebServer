#include "http.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <errno.h>

// ------------------------
//  MIME TYPES
// ------------------------

const char* get_mime_type(const char* path) {
    const char* ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";

    if (strcmp(ext, ".html") == 0) return "text/html";
    if (strcmp(ext, ".css")  == 0) return "text/css";
    if (strcmp(ext, ".js")   == 0) return "application/javascript";
    if (strcmp(ext, ".png")  == 0) return "image/png";
    if (strcmp(ext, ".jpg")  == 0) return "image/jpeg";
    if (strcmp(ext, ".jpeg") == 0) return "image/jpeg";

    return "application/octet-stream";
}


//  ERROS HTTP

// Tenta ler página de erro de ficheiro, caso contrário usa HTML inline
static long send_error_page(int client_fd, const char* status_line, const char* error_file, const char* fallback_body) {
    char error_path[512];
    snprintf(error_path, sizeof(error_path), "./www/%s", error_file);
    
    // Tenta ler a página de erro do ficheiro
    FILE* file = fopen(error_path, "rb");
    char* body = NULL;
    size_t body_len = 0;
    
    if (file) {
        fseek(file, 0, SEEK_END);
        long file_size = ftell(file);
        if (file_size > 0 && file_size < 100000) { // Máximo 100KB para páginas de erro
            fseek(file, 0, SEEK_SET);
            body = malloc(file_size + 1);
            if (body && fread(body, 1, file_size, file) == (size_t)file_size) {
                body[file_size] = '\0';
                body_len = file_size;
            } else {
                free(body);
                body = NULL;
            }
        }
        fclose(file);
    }
    
    // Se não conseguiu ler o ficheiro, usa fallback
    if (!body) {
        body = (char*)fallback_body;
        body_len = strlen(fallback_body);
    }
    
    char date_header[128];
    time_t now = time(NULL);
    struct tm tm_buf;
    struct tm *gmt = gmtime_r(&now, &tm_buf);
    if (!gmt) {
        strncpy(date_header, "Thu, 01 Jan 1970 00:00:00 GMT", sizeof(date_header));
    } else {
        strftime(date_header, sizeof(date_header), "%a, %d %b %Y %H:%M:%S GMT", gmt);
    }

    char headers[512];
    snprintf(headers, sizeof(headers),
        "%s\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Server: ConcurrentHTTP/1.0\r\n"
        "Date: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        status_line, body_len, date_header);

    long bytes_sent = 0;
    bytes_sent += send(client_fd, headers, strlen(headers), 0);
    bytes_sent += send(client_fd, body, body_len, 0);
    
    if (body != fallback_body) {
        free(body);
    }
    
    return bytes_sent;
}

long send_error(int client_fd, const char* status_line, const char* body) {
    // Mapeia status_line para ficheiro de erro apropriado
    const char* error_file = NULL;
    
    if (strstr(status_line, "403")) {
        error_file = "403.html";
    } else if (strstr(status_line, "404")) {
        error_file = "404.html";
    } else if (strstr(status_line, "500")) {
        error_file = "500.html";
    } else if (strstr(status_line, "503")) {
        error_file = "503.html";
    }
    
    if (error_file) {
        return send_error_page(client_fd, status_line, error_file, body);
    }
    
    // Para outros erros (400, 501), usa inline
    char headers[256];
    size_t body_len = strlen(body);

    char date_header[128];
    time_t now = time(NULL);
    struct tm tm_buf;
    struct tm *gmt = gmtime_r(&now, &tm_buf);
    if (!gmt) {
        strncpy(date_header, "Thu, 01 Jan 1970 00:00:00 GMT", sizeof(date_header));
    } else {
        strftime(date_header, sizeof(date_header), "%a, %d %b %Y %H:%M:%S GMT", gmt);
    }

    snprintf(headers, sizeof(headers),
        "%s\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %zu\r\n"
        "Server: ConcurrentHTTP/1.0\r\n"
        "Date: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        status_line, body_len, date_header);

    long bytes_sent = 0;
    bytes_sent += send(client_fd, headers, strlen(headers), 0);
    bytes_sent += send(client_fd, body, body_len, 0);
    return bytes_sent;
}


//  PARSE HTTP REQUEST LINE

int parse_http_request(const char *buffer, HttpRequest *req) {
    char local[BUFFER_SIZE];
    strncpy(local, buffer, sizeof(local) - 1);
    local[sizeof(local) - 1] = '\0';

    char *line_end = strstr(local, "\r\n");
    if (!line_end) return -1;
    *line_end = '\0';

    char method[16], path[512], version[16];
    if (sscanf(local, "%15s %511s %15s", method, path, version) != 3)
        return -1;

    if (strcmp(version, "HTTP/1.1") != 0 &&
        strcmp(version, "HTTP/1.0") != 0)
        return -1;

    strncpy(req->method, method, sizeof(req->method) - 1);
    req->method[sizeof(req->method) - 1] = '\0';

    strncpy(req->path, path, sizeof(req->path) - 1);
    req->path[sizeof(req->path) - 1] = '\0';

    strncpy(req->version, version, sizeof(req->version) - 1);
    req->version[sizeof(req->version) - 1] = '\0';

    return 0;
}


//  READ HTTP REQUEST

ssize_t read_http_request(int client_fd, char *buffer, size_t size) {
    // Validar file descriptor
    if (client_fd < 0) {
        return -1;
    }
    
    // Configurar timeout de 30 segundos para leitura
    struct timeval timeout;
    timeout.tv_sec = 30;  // TIMEOUT_SECONDS padrão
    timeout.tv_usec = 0;
    
    if (setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        // Apenas loga o erro se for um erro diferente de EBADF
        if (errno != EBADF) {
            perror("setsockopt SO_RCVTIMEO");
        }
        // Continua mesmo com erro - não é crítico
    }
    
    size_t total = 0;

    while (total < size - 1) {
        ssize_t n = recv(client_fd, buffer + total, size - 1 - total, 0);
        if (n < 0) {
            // Verifica se foi timeout
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                fprintf(stderr, "Connection timeout on recv\n");
            } else {
                perror("recv");
            }
            break;
        }
        if (n == 0) {
            // Cliente fechou conexão
            break;
        }

        total += n;
        buffer[total] = '\0';

        if (strstr(buffer, "\r\n\r\n") != NULL)
            break;
    }

    buffer[total] = '\0';
    return (ssize_t)total;
}

long send_file(int client_fd, const char* fullpath, int send_body) {
    // valida existência e permissões
    if (access(fullpath, F_OK) != 0) {
        return send_error(client_fd,
                          "HTTP/1.1 404 Not Found",
                          "<h1>404 Not Found</h1>");
    }

    if (access(fullpath, R_OK) != 0) {
        return send_error(client_fd,
                          "HTTP/1.1 403 Forbidden",
                          "<h1>403 Forbidden</h1>");
    }

    FILE* file = fopen(fullpath, "rb");
    if (!file) {
        return send_error(client_fd,
                          "HTTP/1.1 500 Internal Server Error",
                          "<h1>500 Internal Server Error</h1>");
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return send_error(client_fd,
                          "HTTP/1.1 500 Internal Server Error",
                          "<h1>500 Internal Server Error</h1>");
    }

    long file_size = ftell(file);
    if (file_size < 0) {
        fclose(file);
        return send_error(client_fd,
                          "HTTP/1.1 500 Internal Server Error",
                          "<h1>500 Internal Server Error</h1>");
    }

    fseek(file, 0, SEEK_SET);

    // lê ficheiro para memória (para poder meter na cache)
    char *buf = malloc(file_size);
    if (!buf) {
        fclose(file);
        return send_error(client_fd,
                          "HTTP/1.1 500 Internal Server Error",
                          "<h1>500 Internal Server Error</h1>");
    }

    size_t read_total = 0;
    while (read_total < (size_t)file_size) {
        size_t n = fread(buf + read_total, 1, (size_t)file_size - read_total, file);
        if (n == 0) break;
        read_total += n;
    }
    fclose(file);

    if (read_total != (size_t)file_size) {
        free(buf);
        return send_error(client_fd,
                          "HTTP/1.1 500 Internal Server Error",
                          "<h1>500 Internal Server Error</h1>");
    }

    char date_header[128];
    time_t now = time(NULL);
    struct tm tm_buf;
    struct tm *gmt = gmtime_r(&now, &tm_buf);
    if (!gmt) {
        strncpy(date_header, "Thu, 01 Jan 1970 00:00:00 GMT", sizeof(date_header));
    } else {
        strftime(date_header, sizeof(date_header), "%a, %d %b %Y %H:%M:%S GMT", gmt);
    }

    const char* mime = get_mime_type(fullpath);
    long total_sent = 0;

    char headers[512];
    int hlen = snprintf(headers, sizeof(headers),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Server: ConcurrentHTTP/1.0\r\n"
        "Date: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        mime, file_size, date_header);

    if (hlen < 0) {
        free(buf);
        return 0;
    }

    send(client_fd, headers, hlen, 0);
    total_sent += hlen;

    if (send_body) {
        send(client_fd, buf, (size_t)file_size, 0);
        total_sent += file_size;
    }

    free(buf);
    return total_sent;
}

// ------------------------
//  SERVE FILE WITH CACHE
// ------------------------
long send_file_with_cache(int client_fd, const char* fullpath, int send_body, cache_t *cache) {
    const char *cached_data = NULL;
    size_t cached_size = 0;
    long total_sent = 0;

    // 1) Tentar cache primeiro
    if (cache && cache_get(cache, fullpath, &cached_data, &cached_size)) {
        // Cache hit → enviar a partir da memória
        char date_header[128];
        time_t now = time(NULL);
        struct tm tm_buf;
        struct tm *gmt = gmtime_r(&now, &tm_buf);
        if (!gmt) {
            strncpy(date_header, "Thu, 01 Jan 1970 00:00:00 GMT", sizeof(date_header));
        } else {
            strftime(date_header, sizeof(date_header), "%a, %d %b %Y %H:%M:%S GMT", gmt);
        }

        const char* mime = get_mime_type(fullpath);

        char headers[512];
        int hlen = snprintf(headers, sizeof(headers),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %zu\r\n"
            "Server: ConcurrentHTTP/1.0\r\n"
            "Date: %s\r\n"
            "Connection: close\r\n"
            "\r\n",
            mime, cached_size, date_header);

        if (hlen < 0) return 0;

        send(client_fd, headers, hlen, 0);
        total_sent += hlen;

        if (send_body) {
            send(client_fd, cached_data, cached_size, 0);
            total_sent += (long)cached_size;
        }

        return total_sent;
    }

    // 2) Cache miss → usar send_file normal e adicionar à cache
    long bytes_sent = send_file(client_fd, fullpath, send_body);
    
    // 3) Se foi sucesso (arquivo existe), adicionar à cache
    if (bytes_sent > 0 && cache && access(fullpath, F_OK) == 0 && access(fullpath, R_OK) == 0) {
        FILE* file = fopen(fullpath, "rb");
        if (file) {
            fseek(file, 0, SEEK_END);
            long file_size = ftell(file);
            if (file_size > 0 && file_size < 1024*1024) { // Só cachear ficheiros < 1MB
                fseek(file, 0, SEEK_SET);
                char *file_data = malloc(file_size);
                if (file_data && fread(file_data, 1, file_size, file) == (size_t)file_size) {
                    cache_put(cache, fullpath, file_data, file_size);
                }
                free(file_data);
            }
            fclose(file);
        }
    }
    
    return bytes_sent;
}

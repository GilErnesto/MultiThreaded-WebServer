#include "http.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>

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

// tenta ler página de erro do disco, senão usa fallback
static long send_error_page(int client_fd, const char* status_line, const char* error_file, const char* fallback_body) {
    char error_path[512];
    snprintf(error_path, sizeof(error_path), "./www/%s", error_file);
    
    FILE* file = fopen(error_path, "rb");
    char* body = NULL;
    size_t body_len = 0;
    
    if (file) {
        fseek(file, 0, SEEK_END);
        long file_size = ftell(file);
        if (file_size > 0 && file_size < 100000) {
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

    // inicializa campos de range
    req->has_range = 0;
    req->range_start = -1;
    req->range_end = -1;
    
    // inicializa hostname
    req->hostname[0] = '\0';

    // procura header Host: para virtual host support
    const char *host_header = strcasestr(buffer, "Host:");
    if (host_header) {
        const char *host_value = host_header + 5;
        while (*host_value == ' ' || *host_value == '\t') host_value++;
        
        // copia hostname até encontrar \r, \n, ou espaço
        int i = 0;
        while (host_value[i] != '\0' && host_value[i] != '\r' && 
               host_value[i] != '\n' && host_value[i] != ' ' && 
               i < (int)sizeof(req->hostname) - 1) {
            req->hostname[i] = host_value[i];
            i++;
        }
        req->hostname[i] = '\0';
    }

    // procura header Range: bytes=START-END no buffer completo
    const char *range_header = strcasestr(buffer, "Range:");
    if (range_header) {
        // encontra o valor após "Range:"
        const char *range_value = range_header + 6;
        while (*range_value == ' ' || *range_value == '\t') range_value++;
        
        // verifica se começa com "bytes="
        if (strncasecmp(range_value, "bytes=", 6) == 0) {
            range_value += 6;
            
            // parseia range: START-END, START-, ou -SUFFIX
            long start = -1, end = -1;
            
            if (*range_value == '-') {
                // suffix range: -500 (últimos 500 bytes)
                if (sscanf(range_value, "-%ld", &end) == 1 && end > 0) {
                    req->has_range = 1;
                    req->range_start = -1;  // indica suffix
                    req->range_end = end;   // quantidade de bytes do fim
                }
            } else {
                // range normal: START-END ou START-
                int parsed = sscanf(range_value, "%ld-%ld", &start, &end);
                if (parsed >= 1 && start >= 0) {
                    req->has_range = 1;
                    req->range_start = start;
                    req->range_end = (parsed == 2) ? end : -1;  // -1 se open-ended
                }
            }
        }
    }

    return 0;
}

ssize_t read_http_request(int client_fd, char *buffer, size_t size) {
    if (client_fd < 0) {
        return -1;
    }
    
    // timeout de 30s na leitura
    struct timeval timeout;
    timeout.tv_sec = 30;
    timeout.tv_usec = 0;
    
    if (setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        if (errno != EBADF) {
            perror("setsockopt SO_RCVTIMEO");
        }
    }
    
    size_t total = 0;

    while (total < size - 1) {
        ssize_t n = recv(client_fd, buffer + total, size - 1 - total, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                fprintf(stderr, "Connection timeout on recv\n");
            } else {
                perror("recv");
            }
            break;
        }
        if (n == 0) {
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

// envia conteúdo parcial (HTTP 206 Partial Content)
long send_file_range(int client_fd, const char* fullpath, int send_body, long range_start, long range_end) {
    // valida acesso ao ficheiro
    if (access(fullpath, F_OK) != 0) {
        return send_error(client_fd, "HTTP/1.1 404 Not Found", "<h1>404 Not Found</h1>");
    }
    if (access(fullpath, R_OK) != 0) {
        return send_error(client_fd, "HTTP/1.1 403 Forbidden", "<h1>403 Forbidden</h1>");
    }

    // abre ficheiro e determina tamanho
    FILE* file = fopen(fullpath, "rb");
    if (!file) {
        return send_error(client_fd, "HTTP/1.1 500 Internal Server Error", "<h1>500 Internal Server Error</h1>");
    }

    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    if (file_size < 0) {
        fclose(file);
        return send_error(client_fd, "HTTP/1.1 500 Internal Server Error", "<h1>500 Internal Server Error</h1>");
    }

    // processa suffix range (-500 = últimos 500 bytes)
    if (range_start == -1 && range_end > 0) {
        range_start = (file_size > range_end) ? (file_size - range_end) : 0;
        range_end = file_size - 1;
    }

    // processa open-ended range (500- = de 500 até ao fim)
    if (range_end == -1) {
        range_end = file_size - 1;
    }

    // valida range
    if (range_start < 0 || range_end >= file_size || range_start > range_end) {
        fclose(file);
        // HTTP 416 Range Not Satisfiable
        char error_body[256];
        snprintf(error_body, sizeof(error_body),
                 "<h1>416 Range Not Satisfiable</h1><p>Requested range not satisfiable. File size: %ld bytes</p>",
                 file_size);
        
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
                 "HTTP/1.1 416 Range Not Satisfiable\r\n"
                 "Content-Type: text/html\r\n"
                 "Content-Length: %zu\r\n"
                 "Content-Range: bytes */%ld\r\n"
                 "Server: ConcurrentHTTP/1.0\r\n"
                 "Date: %s\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 strlen(error_body), file_size, date_header);
        
        long bytes_sent = 0;
        bytes_sent += send(client_fd, headers, strlen(headers), 0);
        bytes_sent += send(client_fd, error_body, strlen(error_body), 0);
        return bytes_sent;
    }

    // calcula tamanho do conteúdo a enviar
    long content_length = range_end - range_start + 1;

    // posiciona no início do range
    fseek(file, range_start, SEEK_SET);

    // lê conteúdo do range para memória
    char *buf = malloc(content_length);
    if (!buf) {
        fclose(file);
        return send_error(client_fd, "HTTP/1.1 500 Internal Server Error", "<h1>500 Internal Server Error</h1>");
    }

    size_t read_total = fread(buf, 1, content_length, file);
    fclose(file);

    if (read_total != (size_t)content_length) {
        free(buf);
        return send_error(client_fd, "HTTP/1.1 500 Internal Server Error", "<h1>500 Internal Server Error</h1>");
    }

    // prepara headers HTTP 206
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
        "HTTP/1.1 206 Partial Content\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Content-Range: bytes %ld-%ld/%ld\r\n"
        "Server: ConcurrentHTTP/1.0\r\n"
        "Date: %s\r\n"
        "Accept-Ranges: bytes\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        mime, content_length, range_start, range_end, file_size, date_header);

    if (hlen < 0) {
        free(buf);
        return 0;
    }

    send(client_fd, headers, hlen, 0);
    total_sent += hlen;

    if (send_body) {
        send(client_fd, buf, content_length, 0);
        total_sent += content_length;
    }

    free(buf);
    return total_sent;
}

long send_file_with_cache(int client_fd, const char* fullpath, int send_body, cache_t *cache) {
    const char *cached_data = NULL;
    size_t cached_size = 0;
    long total_sent = 0;

    // tenta cache primeiro
    if (cache && cache_get(cache, fullpath, &cached_data, &cached_size)) {
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

    // cache miss
    long bytes_sent = send_file(client_fd, fullpath, send_body);
    
    // adiciona à cache se o ficheiro existe e é pequeno
    if (bytes_sent > 0 && cache && access(fullpath, F_OK) == 0 && access(fullpath, R_OK) == 0) {
        FILE* file = fopen(fullpath, "rb");
        if (file) {
            fseek(file, 0, SEEK_END);
            long file_size = ftell(file);
            if (file_size > 0 && file_size < 1024*1024) {
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

// envia resposta JSON (para endpoint /stats)
long send_json_response(int client_fd, const char* json_body, int send_body) {
    char date_header[128];
    time_t now = time(NULL);
    struct tm tm_buf;
    struct tm *gmt = gmtime_r(&now, &tm_buf);
    if (!gmt) {
        strncpy(date_header, "Thu, 01 Jan 1970 00:00:00 GMT", sizeof(date_header));
    } else {
        strftime(date_header, sizeof(date_header), "%a, %d %b %Y %H:%M:%S GMT", gmt);
    }

    size_t body_len = strlen(json_body);
    
    char headers[512];
    int hlen = snprintf(headers, sizeof(headers),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Server: ConcurrentHTTP/1.0\r\n"
        "Date: %s\r\n"
        "Connection: keep-alive\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n",
        body_len, date_header);

    if (hlen < 0) return 0;

    long total_sent = 0;
    total_sent += send(client_fd, headers, hlen, 0);
    
    if (send_body) {
        total_sent += send(client_fd, json_body, body_len, 0);
    }
    
    return total_sent;
}

// envia resposta HTML (para endpoint /dashboard)
long send_html_response(int client_fd, const char* html_body, int send_body) {
    char date_header[128];
    time_t now = time(NULL);
    struct tm tm_buf;
    struct tm *gmt = gmtime_r(&now, &tm_buf);
    if (!gmt) {
        strncpy(date_header, "Thu, 01 Jan 1970 00:00:00 GMT", sizeof(date_header));
    } else {
        strftime(date_header, sizeof(date_header), "%a, %d %b %Y %H:%M:%S GMT", gmt);
    }

    size_t body_len = strlen(html_body);
    
    char headers[512];
    int hlen = snprintf(headers, sizeof(headers),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Server: ConcurrentHTTP/1.0\r\n"
        "Date: %s\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        body_len, date_header);

    if (hlen < 0) return 0;

    long total_sent = 0;
    total_sent += send(client_fd, headers, hlen, 0);
    
    if (send_body) {
        total_sent += send(client_fd, html_body, body_len, 0);
    }
    
    return total_sent;
}

// gera página HTML do dashboard com JavaScript inline para auto-refresh
void generate_dashboard_html(char *buffer, size_t buffer_size) {
    snprintf(buffer, buffer_size,
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head>\n"
        "    <meta charset=\"utf-8\">\n"
        "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
        "    <title>Server Dashboard - ConcurrentHTTP</title>\n"
        "    <style>\n"
        "        * { margin: 0; padding: 0; box-sizing: border-box; }\n"
        "        body {\n"
        "            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Arial, sans-serif;\n"
        "            min-height: 100vh;\n"
        "            padding: 20px;\n"
        "            color: #333;\n"
        "        }\n"
        "        .container {\n"
        "            max-width: 1200px;\n"
        "            margin: 0 auto;\n"
        "        }\n"
        "        h1 {\n"
        "            text-align: center;\n"
        "            color: black;\n"
        "            margin-bottom: 30px;\n"
        "            font-size: 2.5em;\n"
        "            text-shadow: 2px 2px 4px rgba(0,0,0,0.3);\n"
        "        }\n"
        "        .stats-grid {\n"
        "            display: grid;\n"
        "            grid-template-columns: repeat(auto-fit, minmax(280px, 1fr));\n"
        "            gap: 20px;\n"
        "            margin-bottom: 20px;\n"
        "        }\n"
        "        .stat-card {\n"
        "            background: white;\n"
        "            border-radius: 12px;\n"
        "            padding: 25px;\n"
        "            box-shadow: 0 4px 6px rgba(0,0,0,0.1);\n"
        "            transition: transform 0.2s, box-shadow 0.2s;\n"
        "        }\n"
        "        .stat-card:hover {\n"
        "            transform: translateY(-5px);\n"
        "            box-shadow: 0 8px 12px rgba(0,0,0,0.15);\n"
        "        }\n"
        "        .stat-label {\n"
        "            font-size: 0.9em;\n"
        "            color: #666;\n"
        "            text-transform: uppercase;\n"
        "            letter-spacing: 0.5px;\n"
        "            margin-bottom: 8px;\n"
        "        }\n"
        "        .stat-value {\n"
        "            font-size: 2.2em;\n"
        "            font-weight: bold;\n"
        "            color: black;\n"
        "        }\n"
        "        .stat-card.primary .stat-value { color: black; }\n"
        "        .stat-card.success .stat-value { color: black; }\n"
        "        .stat-card.warning .stat-value { color: black; }\n"
        "        .stat-card.danger .stat-value { color: black; }\n"
        "        .status-table {\n"
        "            background: white;\n"
        "            border-radius: 12px;\n"
        "            padding: 25px;\n"
        "            box-shadow: 0 4px 6px rgba(0,0,0,0.1);\n"
        "            overflow-x: auto;\n"
        "        }\n"
        "        .status-table h2 {\n"
        "            margin-bottom: 15px;\n"
        "            color: #333;\n"
        "        }\n"
        "        table {\n"
        "            width: 100%%;\n"
        "            border-collapse: collapse;\n"
        "        }\n"
        "        th, td {\n"
        "            padding: 12px;\n"
        "            text-align: left;\n"
        "            border-bottom: 1px solid #e5e7eb;\n"
        "        }\n"
        "        th {\n"
        "            background: #f9fafb;\n"
        "            font-weight: 600;\n"
        "            color: #666;\n"
        "        }\n"
        "        .update-indicator {\n"
        "            text-align: center;\n"
        "            color: black;\n"
        "            margin-top: 20px;\n"
        "            font-size: 0.9em;\n"
        "            opacity: 0.8;\n"
        "        }\n"
        "        .loading {\n"
        "            text-align: center;\n"
        "            color: black;\n"
        "            font-size: 1.2em;\n"
        "            margin-top: 50px;\n"
        "        }\n"
        "    </style>\n"
        "</head>\n"
        "<body>\n"
        "    <div class=\"container\">\n"
        "        <h1>Server Dashboard</h1>\n"
        "        <div id=\"stats-container\" class=\"loading\">Loading statistics...</div>\n"
        "        <div class=\"update-indicator\" id=\"update-indicator\">Auto-refresh every 2 seconds</div>\n"
        "    </div>\n"
        "    <script>\n"
        "        function formatBytes(bytes) {\n"
        "            if (bytes < 1024) return bytes + ' B';\n"
        "            if (bytes < 1024*1024) return (bytes/1024).toFixed(2) + ' KB';\n"
        "            if (bytes < 1024*1024*1024) return (bytes/(1024*1024)).toFixed(2) + ' MB';\n"
        "            return (bytes/(1024*1024*1024)).toFixed(2) + ' GB';\n"
        "        }\n"
        "        function formatUptime(seconds) {\n"
        "            const d = Math.floor(seconds / 86400);\n"
        "            const h = Math.floor((seconds %% 86400) / 3600);\n"
        "            const m = Math.floor((seconds %% 3600) / 60);\n"
        "            const s = seconds %% 60;\n"
        "            let result = '';\n"
        "            if (d > 0) result += d + 'd ';\n"
        "            if (h > 0 || d > 0) result += h + 'h ';\n"
        "            if (m > 0 || h > 0 || d > 0) result += m + 'm ';\n"
        "            result += s + 's';\n"
        "            return result;\n"
        "        }\n"
        "        function updateStats() {\n"
        "            const indicator = document.getElementById('update-indicator');\n"
        "            indicator.textContent = 'Updating...';\n"
        "            fetch('/stats')\n"
        "                .then(r => r.json())\n"
        "                .then(data => {\n"
        "                    const totalStatus = data.requests_by_status['200'] + data.requests_by_status['400'] + \n"
        "                                      data.requests_by_status['403'] + data.requests_by_status['404'] + \n"
        "                                      data.requests_by_status['500'] + data.requests_by_status['501'] + \n"
        "                                      data.requests_by_status['503'];\n"
        "                    document.getElementById('stats-container').innerHTML = `\n"
        "                        <div class=\"stats-grid\">\n"
        "                            <div class=\"stat-card primary\">\n"
        "                                <div class=\"stat-label\">Total Requests</div>\n"
        "                                <div class=\"stat-value\">${data.total_requests.toLocaleString()}</div>\n"
        "                            </div>\n"
        "                            <div class=\"stat-card success\">\n"
        "                                <div class=\"stat-label\">Total Bytes</div>\n"
        "                                <div class=\"stat-value\">${formatBytes(data.total_bytes)}</div>\n"
        "                            </div>\n"
        "                            <div class=\"stat-card warning\">\n"
        "                                <div class=\"stat-label\">Avg Response</div>\n"
        "                                <div class=\"stat-value\">${data.avg_response_time_ms.toFixed(2)} ms</div>\n"
        "                            </div>\n"
        "                            <div class=\"stat-card danger\">\n"
        "                                <div class=\"stat-label\">Active Connections</div>\n"
        "                                <div class=\"stat-value\">${data.active_connections}</div>\n"
        "                            </div>\n"
        "                            <div class=\"stat-card primary\">\n"
        "                                <div class=\"stat-label\">Uptime</div>\n"
        "                                <div class=\"stat-value\" style=\"font-size: 1.5em;\">${formatUptime(data.uptime_seconds)}</div>\n"
        "                            </div>\n"
        "                        </div>\n"
        "                        <div class=\"status-table\">\n"
        "                            <h2>HTTP Status Codes</h2>\n"
        "                            <table>\n"
        "                                <thead>\n"
        "                                    <tr>\n"
        "                                        <th>Status Code</th>\n"
        "                                        <th>Count</th>\n"
        "                                        <th>Percentage</th>\n"
        "                                    </tr>\n"
        "                                </thead>\n"
        "                                <tbody>\n"
        "                                    <tr><td>200 OK</td><td>${data.requests_by_status['200']}</td><td>${totalStatus > 0 ? ((data.requests_by_status['200']/totalStatus)*100).toFixed(1) : 0}%%</td></tr>\n"
        "                                    <tr><td>400 Bad Request</td><td>${data.requests_by_status['400']}</td><td>${totalStatus > 0 ? ((data.requests_by_status['400']/totalStatus)*100).toFixed(1) : 0}%%</td></tr>\n"
        "                                    <tr><td>403 Forbidden</td><td>${data.requests_by_status['403']}</td><td>${totalStatus > 0 ? ((data.requests_by_status['403']/totalStatus)*100).toFixed(1) : 0}%%</td></tr>\n"
        "                                    <tr><td>404 Not Found</td><td>${data.requests_by_status['404']}</td><td>${totalStatus > 0 ? ((data.requests_by_status['404']/totalStatus)*100).toFixed(1) : 0}%%</td></tr>\n"
        "                                    <tr><td>500 Internal Error</td><td>${data.requests_by_status['500']}</td><td>${totalStatus > 0 ? ((data.requests_by_status['500']/totalStatus)*100).toFixed(1) : 0}%%</td></tr>\n"
        "                                    <tr><td>501 Not Implemented</td><td>${data.requests_by_status['501']}</td><td>${totalStatus > 0 ? ((data.requests_by_status['501']/totalStatus)*100).toFixed(1) : 0}%%</td></tr>\n"
        "                                    <tr><td>503 Service Unavailable</td><td>${data.requests_by_status['503']}</td><td>${totalStatus > 0 ? ((data.requests_by_status['503']/totalStatus)*100).toFixed(1) : 0}%%</td></tr>\n"
        "                                </tbody>\n"
        "                            </table>\n"
        "                        </div>\n"
        "                    `;\n"
        "                    indicator.textContent = 'Auto-refresh every 2 seconds • Last update: ' + new Date().toLocaleTimeString();\n"
        "                })\n"
        "                .catch(err => {\n"
        "                    console.error('Error fetching stats:', err);\n"
        "                    document.getElementById('stats-container').innerHTML = '<div class=\"loading\">Error loading statistics</div>';\n"
        "                    indicator.textContent = 'Error - Retrying in 2 seconds...';\n"
        "                });\n"
        "        }\n"
        "        updateStats();\n"
        "        setInterval(updateStats, 2000);\n"
        "    </script>\n"
        "</body>\n"
        "</html>"
    );
}

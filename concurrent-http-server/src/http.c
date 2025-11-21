#include "http.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>

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

// ------------------------
//  ERROS HTTP
// ------------------------
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

// ------------------------
//  PARSE HTTP REQUEST LINE
// ------------------------
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

// ------------------------
//  READ HTTP REQUEST
// ------------------------
ssize_t read_http_request(int client_fd, char *buffer, size_t size) {
    size_t total = 0;

    while (total < size - 1) {
        ssize_t n = recv(client_fd, buffer + total, size - 1 - total, 0);
        if (n <= 0) break;

        total += n;
        buffer[total] = '\0';

        if (strstr(buffer, "\r\n\r\n") != NULL)
            break;
    }

    buffer[total] = '\0';
    return (ssize_t)total;
}

// ------------------------
//  SERVE FILE (200 OK)
// ------------------------
void send_file(int client_fd, const char* fullpath, int send_body) {

    // valida existência
    if (access(fullpath, F_OK) != 0) {
        send_error(client_fd,
            "HTTP/1.1 404 Not Found",
            "<h1>404 Not Found</h1>");
        return;
    }

    // valida permissões
    if (access(fullpath, R_OK) != 0) {
        send_error(client_fd,
            "HTTP/1.1 403 Forbidden",
            "<h1>403 Forbidden</h1>");
        return;
    }

    FILE* file = fopen(fullpath, "rb");
    if (!file) {
        send_error(client_fd,
            "HTTP/1.1 500 Internal Server Error",
            "<h1>500 Internal Server Error</h1>");
        return;
    }

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

    fseek(file, 0, SEEK_SET);

    char date_header[128];
    time_t now = time(NULL);
    struct tm *gmt = gmtime(&now);
    strftime(date_header, sizeof(date_header), "%a, %d %b %Y %H:%M:%S GMT", gmt);

    const char* mime = get_mime_type(fullpath);

    char headers[512];
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

    // HEAD → sem corpo
    if (!send_body) {
        fclose(file);
        return;
    }

    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), file)) > 0)
        send(client_fd, buf, n, 0);

    fclose(file);
}

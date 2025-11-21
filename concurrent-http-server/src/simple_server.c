#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define PORT 8080
#define BUFFER_SIZE 1024

typedef struct {
    char method[16];
    char path[512];
    char version[16];
} HttpRequest;

int parse_http_request(const char *buffer, HttpRequest *req) {

    char local[BUFFER_SIZE];
    strncpy(local, buffer, sizeof(local) - 1);
    local[sizeof(local) - 1] = '\0';
    char *line_end = strstr(local, "\r\n");
    if (!line_end) {
        return -1; 
    }
    *line_end = '\0';

    // request line
    char method[16], path[512], version[16];
    if (sscanf(local, "%15s %511s %15s", method, path, version) != 3) {
        return -1;
    }

    // validações
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


void send_file(int client_fd, const char* fullpath) {
    FILE* file = fopen(fullpath, "rb");
    if (!file) {
        const char* not_found =
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: text/html\r\n"
            "\r\n"
            "<h1>404 Not Found</h1>";
        send(client_fd, not_found, strlen(not_found), 0);
        return;
    }

    // tamanho do ficheiro
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    // headers
    char headers[256];
    snprintf(headers, sizeof(headers),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %ld\r\n"
        "\r\n", file_size);
    send(client_fd, headers, strlen(headers), 0);

    // corpo
    char file_buffer[1024];
    size_t bytes_read;
    while ((bytes_read = fread(file_buffer, 1, sizeof(file_buffer), file)) > 0) {
        send(client_fd, file_buffer, bytes_read, 0);
    }

    fclose(file);
}

int main(void) {
    // socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    // reuse addr
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // bind
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

    // listen
    if (listen(server_fd, 10) < 0) {
        perror("listen failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("Server listening on port %d...\n", PORT);

    // loop principal
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
            close(client_fd);
            continue;
        }

        // por agora só GET e HEAD
        if (strcmp(req.method, "GET") != 0 && strcmp(req.method, "HEAD") != 0) {
            close(client_fd);
            continue;
        }

        // Build full file path a partir de req.path
        char fullpath[1024];
        if (strcmp(req.path, "/") == 0) {
            snprintf(fullpath, sizeof(fullpath), "./www/index.html");
        } else {
            snprintf(fullpath, sizeof(fullpath), "./www%s", req.path);
        }

        // enviar headers
        send_file(client_fd, fullpath);
        close(client_fd);
    }

    close(server_fd);
    return 0;
}

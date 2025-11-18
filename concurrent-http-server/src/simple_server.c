#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define PORT 8080
#define BUFFER_SIZE 1024

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
        ssize_t bytes = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
        if (bytes <= 0) {
            close(client_fd);
            continue;
        }

        // garantir string terminada
        buffer[bytes] = '\0';

        // primeira linha do pedido
        char method[16], path[512], version[16];
        if (sscanf(buffer, "%15s %511s %15s", method, path, version) != 3) {
            close(client_fd);
            continue;
        }

        // só GET
        if (strcmp(method, "GET") != 0) {
            close(client_fd);
            continue;
        }

        // caminho real
        char fullpath[1024];
        if (strcmp(path, "/") == 0) {
            snprintf(fullpath, sizeof(fullpath), "./www/index.html");
        } else {
            snprintf(fullpath, sizeof(fullpath), "./www%s", path);
        }

        send_file(client_fd, fullpath);
        close(client_fd);
    }

    close(server_fd);
    return 0;
}

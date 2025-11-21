#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "config.h"
#include "http.h"

void worker_loop(int listen_fd, server_config_t *config) {
    while (1) {
        // usar o listen_fd passado pelo master
        int client_fd = accept(listen_fd, NULL, NULL);
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

        buffer[bytes] = '\0';

        HttpRequest req;
        if (parse_http_request(buffer, &req) != 0) {
            send_error(client_fd,
                       "HTTP/1.1 400 Bad Request",
                       "<h1>400 Bad Request</h1>");
            close(client_fd);
            continue;
        }

        if (strstr(req.path, "..") != NULL) {
            send_error(client_fd,
                       "HTTP/1.1 403 Forbidden",
                       "<h1>403 Forbidden</h1>");
            close(client_fd);
            continue;
        }

        if (strcmp(req.method, "GET") != 0 && strcmp(req.method, "HEAD") != 0) {
            send_error(client_fd,
                       "HTTP/1.1 501 Not Implemented",
                       "<h1>501 Not Implemented</h1>");
            close(client_fd);
            continue;
        }

        char fullpath[1024];
        if (strcmp(req.path, "/") == 0) {
            snprintf(fullpath, sizeof(fullpath), "%s/index.html",
                     config->document_root);
        } else {
            snprintf(fullpath, sizeof(fullpath), "%s%s",
                     config->document_root, req.path);
        }

        int send_body = strcmp(req.method, "HEAD") != 0;
        send_file(client_fd, fullpath, send_body);
        close(client_fd);
    }
}

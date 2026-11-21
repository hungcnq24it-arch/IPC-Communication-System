
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "../common/protocol.h"

static int server_fd = -1;
static int client_count = 0;
static pthread_mutex_t count_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Send all bytes */
int send_all(int fd, const char *data, size_t len) {
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, 0);

        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }

        if (n == 0) return -1;
        sent += n;
    }

    return 0;
}

/* Receive one line */
int recv_line(int fd, char *buffer, size_t size) {
    size_t i = 0;

    while (i < size - 1) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);

        if (n == 0) return 0;

        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }

        if (c == '\n') break;

        buffer[i++] = c;
    }

    buffer[i] = '\0';
    return 1;
}

/* Process client command */
void process_command(int fd, char *command) {
    char response[BUFFER_SIZE];

    if (strcmp(command, "PING") == 0) {
        snprintf(response, sizeof(response),
                 "PONG - Server is alive\n");
    }

    else if (strcmp(command, "TIME") == 0) {
        time_t now = time(NULL);
        struct tm tm_info;

        localtime_r(&now, &tm_info);

        strftime(response, sizeof(response),
                 "Server time: %Y-%m-%d %H:%M:%S\n",
                 &tm_info);
    }

    else if (strncmp(command, "ECHO ", 5) == 0) {
        snprintf(response, sizeof(response),
                 "Server received: %s\n", command + 5);
    }

    else if (strncmp(command, "CALC ", 5) == 0) {
        double a, b, result;
        char op;

        if (sscanf(command + 5, "%lf %c %lf",
                   &a, &op, &b) != 3) {
            snprintf(response, sizeof(response),
                     "ERROR: Invalid expression\n");
        } else {
            int valid = 1;

            switch (op) {
                case '+': result = a + b; break;
                case '-': result = a - b; break;
                case '*': result = a * b; break;

                case '/':
                    if (b == 0) {
                        snprintf(response, sizeof(response),
                                 "ERROR: Division by zero\n");
                        valid = 0;
                    } else {
                        result = a / b;
                    }
                    break;

                default:
                    snprintf(response, sizeof(response),
                             "ERROR: Unsupported operator\n");
                    valid = 0;
            }

            if (valid) {
                snprintf(response, sizeof(response),
                         "RESULT: %.2f\n", result);
            }
        }
    }

    else if (strcmp(command, "STATUS") == 0) {
        int count;

        pthread_mutex_lock(&count_mutex);
        count = client_count;
        pthread_mutex_unlock(&count_mutex);

        snprintf(response, sizeof(response),
                 "Server running | Active clients: %d\n",
                 count);
    }

    else {
        snprintf(response, sizeof(response),
                 "ERROR: Unknown command\n");
    }

    send_all(fd, response, strlen(response));
}

/* Handle each client in a separate thread */
void *handle_client(void *arg) {
    int client_fd = *(int *)arg;
    free(arg);

    char buffer[BUFFER_SIZE];

    pthread_mutex_lock(&count_mutex);
    client_count++;
    pthread_mutex_unlock(&count_mutex);

    printf("[+] Client connected. Active: %d\n", client_count);

    while (1) {
        int result = recv_line(client_fd, buffer, sizeof(buffer));

        if (result <= 0) break;

        printf("[CLIENT] %s\n", buffer);

        if (strcmp(buffer, "EXIT") == 0) {
            send_all(client_fd, "BYE\n", 4);
            break;
        }

        process_command(client_fd, buffer);
    }

    close(client_fd);

    pthread_mutex_lock(&count_mutex);
    client_count--;
    int count = client_count;
    pthread_mutex_unlock(&count_mutex);

    printf("[-] Client disconnected. Active: %d\n", count);

    return NULL;
}

/* Stop server */
void handle_signal(int sig) {
    (void)sig;

    if (server_fd >= 0) {
        close(server_fd);
        server_fd = -1;
    }
}

int main(int argc, char *argv[]) {
    int port = DEFAULT_PORT;

    if (argc > 1) {
        port = atoi(argv[1]);

        if (port < 1 || port > 65535) {
            fprintf(stderr, "Invalid port\n");
            return 1;
        }
    }

    struct sockaddr_in server_addr;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;

    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
               &opt, sizeof(opt));

    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(server_fd,
             (struct sockaddr *)&server_addr,
             sizeof(server_addr)) < 0) {
        perror("bind");
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 10) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    signal(SIGINT, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    printf("=================================\n");
    printf("       IPC SERVER STARTED        \n");
    printf("=================================\n");
    printf("Port: %d\n", port);
    printf("Waiting for clients...\n");

    while (server_fd >= 0) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(
            server_fd,
            (struct sockaddr *)&client_addr,
            &addr_len
        );

        if (client_fd < 0) {
            if (errno == EINTR || server_fd < 0)
                break;

            perror("accept");
            continue;
        }

        int *arg = malloc(sizeof(int));

        if (!arg) {
            close(client_fd);
            continue;
        }

        *arg = client_fd;

        pthread_t thread;

        if (pthread_create(&thread, NULL,
                           handle_client, arg) != 0) {
            perror("pthread_create");
            free(arg);
            close(client_fd);
            continue;
        }

        pthread_detach(thread);
    }

    if (server_fd >= 0)
        close(server_fd);

    printf("\nServer stopped.\n");

    return 0;
}


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "../common/protocol.h"

/* Send all data */
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

int main(int argc, char *argv[]) {
    const char *ip = "127.0.0.1";
    int port = DEFAULT_PORT;

    if (argc >= 2)
        ip = argv[1];

    if (argc >= 3) {
        port = atoi(argv[2]);

        if (port < 1 || port > 65535) {
            fprintf(stderr, "Invalid port\n");
            return 1;
        }
    }

    signal(SIGPIPE, SIG_IGN);

    int sock = socket(AF_INET, SOCK_STREAM, 0);

    if (sock < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in server_addr;

    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip,
                  &server_addr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid IP address\n");
        close(sock);
        return 1;
    }

    printf("Connecting to %s:%d...\n", ip, port);

    if (connect(sock,
                (struct sockaddr *)&server_addr,
                sizeof(server_addr)) < 0) {
        perror("connect");
        close(sock);
        return 1;
    }

    printf("\n=================================\n");
    printf("          IPC CLIENT             \n");
    printf("=================================\n");
    printf("Connected successfully!\n");

    char command[BUFFER_SIZE];
    char response[BUFFER_SIZE];

    while (1) {
        printf("\n");
        printf("========= CLIENT MENU =========\n");
        printf("1. PING   - Test connection\n");
        printf("2. TIME   - Get server time\n");
        printf("3. ECHO   - Send message\n");
        printf("4. CALC   - Calculate\n");
        printf("5. STATUS - Server status\n");
        printf("6. EXIT   - Disconnect\n");
        printf("===============================\n");

        printf("Enter command: ");
        fflush(stdout);

        if (!fgets(command, sizeof(command), stdin))
            break;

        command[strcspn(command, "\n")] = '\0';

        if (strlen(command) == 0)
            continue;

        char message[BUFFER_SIZE + 1];

        snprintf(message, sizeof(message),
                 "%s\n", command);

        if (send_all(sock, message, strlen(message)) < 0) {
            printf("Connection lost.\n");
            break;
        }

        if (recv_line(sock, response, sizeof(response)) <= 0) {
            printf("Server disconnected.\n");
            break;
        }
 
        printf("\n[SERVER RESPONSE]\n%s\n", response);

        if (strcmp(command, "EXIT") == 0)
            break;
    }

    close(sock);

    printf("\nDisconnected from server.\n");

    return 0;
}

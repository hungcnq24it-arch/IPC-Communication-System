
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define DEFAULT_PORT 8080
#define BUFFER_SIZE 4096

/* Get time in nanoseconds */
long long get_time_ns() {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (long long)ts.tv_sec * 1000000000LL
           + ts.tv_nsec;
}

/* Send complete message */
int send_all(int fd, const char *data, size_t len) {
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(fd, data + sent,
                         len - sent, 0);

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
    int requests = 1000;

    if (argc >= 2)
        requests = atoi(argv[1]);

    if (argc >= 3)
        ip = argv[2];

    if (argc >= 4)
        port = atoi(argv[3]);

    if (requests < 1 || requests > 1000000 ||
        port < 1 || port > 65535) {
        printf("Usage: ./benchmark [requests] [ip] [port]\n");
        return 1;
    }

    /* Create TCP connection */
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
        printf("Invalid IP address\n");
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

    printf("Connected successfully!\n");

    char response[BUFFER_SIZE];

    /* Warm-up */
    for (int i = 0; i < 10; i++) {

        const char *request = "PING\n";

        if (send_all(sock, request,
                     strlen(request)) < 0) {
            perror("send");
            close(sock);
            return 1;
        }

        if (recv_line(sock, response,
                      sizeof(response)) <= 0) {
            printf("Warm-up failed\n");
            close(sock);
            return 1;
        }
    }

    long long total_latency = 0;
    long long min_latency = 0;
    long long max_latency = 0;

    int success = 0;
    int failed = 0;

    printf("\nStarting benchmark...\n");
    printf("Total requests: %d\n\n", requests);

    long long total_start = get_time_ns();

    for (int i = 0; i < requests; i++) {

        long long start = get_time_ns();

        const char *request = "PING\n";

        if (send_all(sock, request,
                     strlen(request)) < 0) {
            failed++;
            break;
        }

        int result = recv_line(sock, response,
                               sizeof(response));

        long long end = get_time_ns();

        if (result <= 0 ||
            strcmp(response, "PONG - Worker is alive") != 0) {
            failed++;
            break;
        }

        long long latency = end - start;

        total_latency += latency;

        if (success == 0 || latency < min_latency)
            min_latency = latency;

        if (success == 0 || latency > max_latency)
            max_latency = latency;

        success++;
    }

    long long total_end = get_time_ns();

    close(sock);

    double total_seconds =
        (total_end - total_start) / 1000000000.0;

    double avg_latency_ms = success > 0
        ? (double)total_latency / success / 1000000.0
        : 0;

    double min_latency_ms =
        (double)min_latency / 1000000.0;

    double max_latency_ms =
        (double)max_latency / 1000000.0;

    double throughput = total_seconds > 0
        ? success / total_seconds
        : 0;

    double success_rate =
        (double)success / requests * 100.0;

    printf("\n");
    printf("====================================\n");
    printf("       IPC BENCHMARK RESULTS        \n");
    printf("====================================\n");

    printf("Total Requests : %d\n", requests);
    printf("Successful     : %d\n", success);
    printf("Failed         : %d\n", failed);

    printf("Success Rate   : %.2f%%\n", success_rate);

    printf("------------------------------------\n");

    printf("Average Latency: %.4f ms\n",
           avg_latency_ms);

    printf("Minimum Latency: %.4f ms\n",
           min_latency_ms);

    printf("Maximum Latency: %.4f ms\n",
           max_latency_ms);

    printf("------------------------------------\n");

    printf("Total Time     : %.4f seconds\n",
           total_seconds);

    printf("Throughput     : %.2f requests/sec\n",
           throughput);

    printf("====================================\n");

    return success == requests ? 0 : 1;
}

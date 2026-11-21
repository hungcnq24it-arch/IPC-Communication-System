
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>

#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <mqueue.h>
#include <semaphore.h>

#include "../common/protocol.h"
#include "../shared_memory/ipc.h"

static int server_fd = -1;
static int client_count = 0;

static pthread_mutex_t count_mutex =
    PTHREAD_MUTEX_INITIALIZER;

static int shm_fd = -1;
static SharedData *shared = NULL;

static mqd_t mq = (mqd_t)-1;

static sem_t *mutex_sem = SEM_FAILED;
static sem_t *response_sem = SEM_FAILED;

static pid_t worker_pid = -1;

static unsigned int next_request_id = 0;

/* Handle server shutdown */
void handle_signal(int sig) {
    (void)sig;

    if (server_fd >= 0) {
        close(server_fd);
        server_fd = -1;
    }
}

/* Send all bytes */
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

/* Create IPC resources */
int initialize_ipc(void) {

    /* Remove old resources from previous runs */
    mq_unlink(MQ_NAME);
    shm_unlink(SHM_NAME);

    sem_unlink(SEM_MUTEX_NAME);
    sem_unlink(SEM_RESPONSE_NAME);

    /* Shared memory */
    shm_fd = shm_open(
        SHM_NAME,
        O_CREAT | O_EXCL | O_RDWR,
        0600
    );

    if (shm_fd < 0) {
        perror("shm_open");
        return -1;
    }

    if (ftruncate(shm_fd, sizeof(SharedData)) < 0) {
        perror("ftruncate");
        return -1;
    }

    shared = mmap(
        NULL,
        sizeof(SharedData),
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        shm_fd,
        0
    );

    if (shared == MAP_FAILED) {
        shared = NULL;
        perror("mmap");
        return -1;
    }

    memset(shared, 0, sizeof(SharedData));

    /* Message Queue */
    struct mq_attr attr;

    memset(&attr, 0, sizeof(attr));

    attr.mq_maxmsg = 10;
    attr.mq_msgsize = sizeof(MQMessage);

    mq = mq_open(
        MQ_NAME,
        O_CREAT | O_EXCL | O_WRONLY,
        0600,
        &attr
    );

    if (mq == (mqd_t)-1) {
        perror("mq_open");
        return -1;
    }

    /* Transaction mutex */
    mutex_sem = sem_open(
        SEM_MUTEX_NAME,
        O_CREAT | O_EXCL,
        0600,
        1
    );

    if (mutex_sem == SEM_FAILED) {
        perror("sem_open mutex");
        return -1;
    }

    /* Worker response semaphore */
    response_sem = sem_open(
        SEM_RESPONSE_NAME,
        O_CREAT | O_EXCL,
        0600,
        0
    );

    if (response_sem == SEM_FAILED) {
        perror("sem_open response");
        return -1;
    }

    printf("[IPC] Shared Memory initialized\n");
    printf("[IPC] Message Queue initialized\n");
    printf("[IPC] Semaphores initialized\n");

    return 0;
}

/* Start independent Worker process */
int start_worker(void) {

    worker_pid = fork();

    if (worker_pid < 0) {
        perror("fork");
        return -1;
    }

    if (worker_pid == 0) {

        execl("./worker", "worker", (char *)NULL);

        perror("execl worker");
        _exit(1);
    }

    printf("[SERVER] Worker started. PID: %d\n",
           worker_pid);

    return 0;
}

/* Send request to Worker and get result */
int dispatch_request(const char *request,
                     char *response,
                     size_t response_size) {

    if (sem_wait(mutex_sem) < 0) {
        if (errno == EINTR)
            return -1;

        perror("sem_wait mutex");
        return -1;
    }

    unsigned int id = ++next_request_id;

    shared->request_id = id;

    snprintf(shared->request,
             sizeof(shared->request),
             "%s", request);

    shared->response[0] = '\0';
    shared->status = 0;

    MQMessage message;

    message.request_id = id;

    /* Notify Worker */
    if (mq_send(mq, (char *)&message,
                sizeof(message), 0) < 0) {

        perror("mq_send");
        sem_post(mutex_sem);
        return -1;
    }

    printf("[SERVER] Request %u dispatched\n", id);

    /* Wait for Worker */
    while (sem_wait(response_sem) < 0) {

        if (errno == EINTR)
            continue;

        perror("sem_wait response");
        sem_post(mutex_sem);
        return -1;
    }

    if (shared->request_id != id ||
        shared->status != 1) {

        snprintf(response, response_size,
                 "ERROR: Worker processing failed");
    } else {

        snprintf(response, response_size,
                 "%s", shared->response);
    }

    printf("[SERVER] Request %u completed\n", id);

    /* Release shared transaction */
    sem_post(mutex_sem);

    return 0;
}

/* Process one TCP client */
void *handle_client(void *arg) {

    int client_fd = *(int *)arg;

    free(arg);

    pthread_mutex_lock(&count_mutex);
    client_count++;
    int current_count = client_count;
    pthread_mutex_unlock(&count_mutex);

    printf("[+] Client connected. Active: %d\n",
           current_count);

    char request[BUFFER_SIZE];
    char response[BUFFER_SIZE];
    char output[BUFFER_SIZE + 1];

    while (1) {

        int result = recv_line(
            client_fd,
            request,
            sizeof(request)
        );

        if (result <= 0)
            break;

        printf("[CLIENT] %s\n", request);

        if (strcmp(request, "EXIT") == 0) {

            send_all(client_fd, "BYE\n", 4);
            break;
        }

        if (strcmp(request, "STATUS") == 0) {

            pthread_mutex_lock(&count_mutex);
            current_count = client_count;
            pthread_mutex_unlock(&count_mutex);

            snprintf(response, sizeof(response),
                     "IPC Server running | Active clients: %d",
                     current_count);
        }

        else {

            if (dispatch_request(
                    request,
                    response,
                    sizeof(response)) < 0) {

                snprintf(response, sizeof(response),
                         "ERROR: IPC communication failed");
            }
        }

        snprintf(output, sizeof(output),
                 "%s\n", response);

        if (send_all(client_fd, output,
                     strlen(output)) < 0)
            break;
    }

    close(client_fd);

    pthread_mutex_lock(&count_mutex);
    client_count--;
    current_count = client_count;
    pthread_mutex_unlock(&count_mutex);

    printf("[-] Client disconnected. Active: %d\n",
           current_count);

    return NULL;
}

/* Release IPC resources */
void cleanup_ipc(void) {

    printf("[IPC] Cleaning resources...\n");

    if (worker_pid > 0) {

        kill(worker_pid, SIGTERM);

        /* Wake Worker if it is blocked in mq_receive */
        if (mq != (mqd_t)-1) {
            MQMessage stop_message = {0};

            mq_send(mq, (char *)&stop_message,
                    sizeof(stop_message), 0);
        }

        waitpid(worker_pid, NULL, 0);
    }

    if (mq != (mqd_t)-1)
        mq_close(mq);

    mq_unlink(MQ_NAME);

    if (shared != NULL)
        munmap(shared, sizeof(SharedData));

    if (shm_fd >= 0)
        close(shm_fd);

    shm_unlink(SHM_NAME);

    if (mutex_sem != SEM_FAILED)
        sem_close(mutex_sem);

    if (response_sem != SEM_FAILED)
        sem_close(response_sem);

    sem_unlink(SEM_MUTEX_NAME);
    sem_unlink(SEM_RESPONSE_NAME);

    printf("[IPC] Cleanup completed\n");
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

    signal(SIGINT, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    /* Initialize IPC */
    if (initialize_ipc() < 0) {
        cleanup_ipc();
        return 1;
    }

    /* Start Worker */
    if (start_worker() < 0) {
        cleanup_ipc();
        return 1;
    }

    /* Create TCP server */
    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd < 0) {
        perror("socket");
        cleanup_ipc();
        return 1;
    }

    int opt = 1;

    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
               &opt, sizeof(opt));

    struct sockaddr_in server_addr;

    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(server_fd,
             (struct sockaddr *)&server_addr,
             sizeof(server_addr)) < 0) {

        perror("bind");
        close(server_fd);
        server_fd = -1;
        cleanup_ipc();
        return 1;
    }

    if (listen(server_fd, 10) < 0) {

        perror("listen");
        close(server_fd);
        server_fd = -1;
        cleanup_ipc();
        return 1;
    }

    printf("\n=================================\n");
    printf("      INTEGRATED IPC SERVER      \n");
    printf("=================================\n");
    printf("TCP Port: %d\n", port);
    printf("Worker PID: %d\n", worker_pid);
    printf("Waiting for clients...\n\n");

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

        if (pthread_create(
                &thread,
                NULL,
                handle_client,
                arg) != 0) {

            perror("pthread_create");

            free(arg);
            close(client_fd);
            continue;
        }

        pthread_detach(thread);
    }

    if (server_fd >= 0) {
        close(server_fd);
        server_fd = -1;
    }

    cleanup_ipc();

    printf("[SERVER] Shutdown completed.\n");

    return 0;
}


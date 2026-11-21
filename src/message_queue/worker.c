
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include <fcntl.h>
#include <unistd.h>
#include <signal.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <mqueue.h>
#include <semaphore.h>

#include "../shared_memory/ipc.h"

static volatile sig_atomic_t running = 1;

void stop_worker(int sig) {
    (void)sig;
    running = 0;
}

/* Process a client command */
void process_request(const char *request, char *response,
                    size_t response_size) {

    if (strcmp(request, "PING") == 0) {
        snprintf(response, response_size,
                 "PONG - Worker is alive");
    }

    else if (strcmp(request, "TIME") == 0) {
        time_t now = time(NULL);
        struct tm info;

        localtime_r(&now, &info);

        strftime(response, response_size,
                 "Server time: %Y-%m-%d %H:%M:%S",
                 &info);
    }

    else if (strncmp(request, "ECHO ", 5) == 0) {
        snprintf(response, response_size,
                 "Worker received: %s", request + 5);
    }

    else if (strncmp(request, "CALC ", 5) == 0) {
        double a, b, result;
        char op;

        if (sscanf(request + 5, "%lf %c %lf",
                   &a, &op, &b) != 3) {

            snprintf(response, response_size,
                     "ERROR: Invalid expression");
            return;
        }

        switch (op) {
            case '+':
                result = a + b;
                break;

            case '-':
                result = a - b;
                break;

            case '*':
                result = a * b;
                break;

            case '/':
                if (b == 0) {
                    snprintf(response, response_size,
                             "ERROR: Division by zero");
                    return;
                }

                result = a / b;
                break;

            default:
                snprintf(response, response_size,
                         "ERROR: Unsupported operator");
                return;
        }

        snprintf(response, response_size,
                 "RESULT: %.2f", result);
    }

    else if (strcmp(request, "STATUS") == 0) {
        snprintf(response, response_size,
                 "IPC Worker is running");
    }

    else {
        snprintf(response, response_size,
                 "ERROR: Unknown command");
    }
}

int main(void) {

    signal(SIGINT, stop_worker);
    signal(SIGTERM, stop_worker);

    printf("[WORKER] Starting...\n");

    /* Open shared memory */
    int shm_fd = shm_open(SHM_NAME, O_RDWR, 0600);

    if (shm_fd < 0) {
        perror("shm_open");
        return 1;
    }

    SharedData *shared = mmap(
        NULL,
        sizeof(SharedData),
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        shm_fd,
        0
    );

    if (shared == MAP_FAILED) {
        perror("mmap");
        close(shm_fd);
        return 1;
    }

    /* Open message queue */
    mqd_t mq = mq_open(MQ_NAME, O_RDONLY);

    if (mq == (mqd_t)-1) {
        perror("mq_open");
        munmap(shared, sizeof(SharedData));
        close(shm_fd);
        return 1;
    }

    /* Open response semaphore */
    sem_t *response_sem = sem_open(
        SEM_RESPONSE_NAME, 0
    );

    if (response_sem == SEM_FAILED) {
        perror("sem_open");
        mq_close(mq);
        munmap(shared, sizeof(SharedData));
        close(shm_fd);
        return 1;
    }

    printf("[WORKER] Ready and waiting for requests...\n");

    while (running) {

        MQMessage message;

        ssize_t bytes = mq_receive(
            mq,
            (char *)&message,
            sizeof(message),
            NULL
        );

        if (bytes < 0) {
            if (errno == EINTR)
                continue;

            perror("mq_receive");
            break;
        }

        if (!running)
            break;

        printf("[WORKER] Received request ID: %u\n",
               message.request_id);

        if (shared->request_id != message.request_id) {
            snprintf(shared->response,
                     sizeof(shared->response),
                     "ERROR: Request ID mismatch");

            shared->status = -1;
        } else {
            process_request(
                shared->request,
                shared->response,
                sizeof(shared->response)
            );

            shared->status = 1;
        }

        printf("[WORKER] Response: %s\n",
               shared->response);

        /* Notify server that processing is complete */
        if (sem_post(response_sem) < 0) {
            perror("sem_post");
            break;
        }
    }

    printf("[WORKER] Stopping...\n");

    sem_close(response_sem);
    mq_close(mq);
    munmap(shared, sizeof(SharedData));
    close(shm_fd);

    return 0;
}

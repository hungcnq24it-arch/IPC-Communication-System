
#ifndef IPC_H
#define IPC_H

#include <sys/types.h>

#define SHM_NAME "/ipc_shared_memory"
#define MQ_NAME "/ipc_message_queue"

#define SEM_MUTEX_NAME "/ipc_sem_mutex"
#define SEM_RESPONSE_NAME "/ipc_sem_response"

#define IPC_BUFFER_SIZE 4096

typedef struct {
    unsigned int request_id;

    char request[IPC_BUFFER_SIZE];
    char response[IPC_BUFFER_SIZE];

    int status;

} SharedData;

typedef struct {
    unsigned int request_id;
} MQMessage;

#endif

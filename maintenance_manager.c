/*
    Realizado por:
    João Bernardo de Jesus Santos, nº2020218995
    Gonçalo Fernandes Diogo de Almeida, nº2020218868
*/

#include <time.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <semaphore.h>
#include "maintenance_manager.h"

pthread_t * mm_thread;
int interval_of_mm, time_bw_mm, mqid, edge_server_number, * id;

void clean_mm_resources(){
    int i;
    for(i = 0; i < edge_server_number; i++) pthread_join(mm_thread[i], NULL);
    free(id);
    free(mm_thread);
}

void * maintenance(void *t){
    Message msg;
    int es_id = *((int *) t);
    int mm_msg_type = es_id * 2 + 1, es_msg_type = es_id * 2;

    // Maintenance of the Edge Servers
    while(1){
        msg.msg_type = mm_msg_type;
        strcpy(msg.msg_text, "START");
        msgsnd(mqid, &msg, sizeof(Message), 0);
        msgrcv(mqid, &msg, sizeof(Message), es_msg_type, 0);
        sleep(interval_of_mm);
        msg.msg_type = mm_msg_type;
        strcpy(msg.msg_text, "END");
        msgsnd(mqid, &msg, sizeof(Message), 0);
        sleep(time_bw_mm);
    }
    pthread_exit(NULL);
}

void maintenance_manager(int mq_id, int es_num) {
    Message msg;
    srand(time(NULL));
    int i, es_msg_type;
    time_bw_mm = rand() % 5 + 1;
    interval_of_mm = rand() % 5 + 1;
    mqid = mq_id;
    edge_server_number = es_num;

    // The Maintenance Manager is informed of the creation of the Edge Servers
    for(i = 0; i < edge_server_number; i++)
    {
        es_msg_type = (i + 1) * 2;
        // Waits for a message with its id
        msgrcv(mqid, &msg, sizeof(Message), es_msg_type, 0);
    }

    mm_thread = (pthread_t *) malloc(sizeof(pthread_t) * edge_server_number);
    id = (int *) malloc(sizeof(int) * edge_server_number);

    for(i = 0; i < edge_server_number; i++){
        id[i] = i + 1;
        pthread_create(&mm_thread[i], NULL, maintenance, &id[i]);
    }

    clean_mm_resources();
}
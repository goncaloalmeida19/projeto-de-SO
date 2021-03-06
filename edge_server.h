/*
    Realizado por:
        João Bernardo de Jesus Santos, nº2020218995
        Gonçalo Fernandes Diogo de Almeida, nº2020218868
*/

#ifndef EDGE_SERVER_H
#define EDGE_SERVER_H

//#define DEBUG_ES //uncomment this line to print edge_server debug messages

typedef struct{
	int id, done, thousand_inst, vcpu;
}VCPUTask;

int mqid;
int **unnamed_pipe;

pthread_mutex_t *dispatcher_mutex;
pthread_cond_t *dispatcher_cond;

double get_current_time();
int edge_server(int es_n);

#endif

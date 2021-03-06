/*
    Realizado por:
        João Bernardo de Jesus Santos, nº2020218995
        Gonçalo Fernandes Diogo de Almeida, nº2020218868
*/

#include <time.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
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
#include "log.h"
#include "edge_server.h"
#include "shared_memory.h"
#include "maintenance_manager.h"


int edge_server_n, maintenance_start = 0, es_leave_flag = 0;
int vcpu_capacity[2], vcpu_start[2], ids[2];
Message mm_msg;
VCPUTask current_task;
char es_name[NAME_LEN];
pthread_mutex_t tasks_mutex = PTHREAD_MUTEX_INITIALIZER, maintenance_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t free_cond = PTHREAD_COND_INITIALIZER, tasks_cond = PTHREAD_COND_INITIALIZER;
pthread_t vcpu_thread[2], maintenance_thread, performance_thread, task_thread;
struct sigaction es_new_action;

double get_current_time(){
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return ts.tv_sec+((double)ts.tv_nsec)/1000000000;
}

double task_time_sec(int capacity, int thousand_inst){
	return thousand_inst/1000.0/capacity;
}

void *vcpu(void *vcpu_id){
	int id = *((int*)vcpu_id);
	VCPUTask t;
	
	//block all signals in this thread
	pthread_sigmask(SIG_BLOCK, &block_set, NULL);
	
	while(1){
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
		
		//Wait for task to arrive
		pthread_mutex_lock(&tasks_mutex);
		while(!vcpu_start[id] || es_leave_flag == 1){
			//check if the simulator is going to end
			if(es_leave_flag == 1){
				pthread_mutex_unlock(&tasks_mutex);
				#ifdef DEBUG_ES
				printf("%s: VCPU %d leaving...\n", es_name, id);
				#endif
				pthread_exit(NULL);
			}
			pthread_cond_wait(&tasks_cond, &tasks_mutex);
		}
		t = current_task;
		pthread_mutex_unlock(&tasks_mutex);
		
		#ifdef DEBUG_ES
		printf("%s: starting task %d\n", es_name, t.id);
		#endif
		
		//Execute task
		usleep(1000000*task_time_sec(vcpu_capacity[id], t.thousand_inst));
		
		//Task done
		char msg[MSG_LEN];
		sprintf(msg, "%s VCPU %d:TASK %d COMPLETED", es_name, id, t.id);
		log_write(msg);
		
		shm_w_lock();
		EdgeServer this = get_edge_server(edge_server_n);
		this.n_tasks_done++;
		set_edge_server(&this, edge_server_n);
		shm_w_unlock();
		pthread_mutex_lock(&tasks_mutex);
		vcpu_start[id] = 0;
		pthread_cond_broadcast(&free_cond);
		pthread_mutex_unlock(&tasks_mutex);
		
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    }
    pthread_exit(NULL);
}


int ready_to_receive_task(){
	shm_r_lock();
	int performance_level = get_edge_server(edge_server_n).performance_level;
	shm_r_unlock();
	return performance_level != 0 && (!vcpu_start[0] || (performance_level == 2 && !vcpu_start[1]));
}

void *receive_tasks(){
	VCPUTask t;
	EdgeServer this;
	int first = 1;
	
	pthread_sigmask(SIG_BLOCK, &block_set, NULL);
	
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	shm_w_lock();
	this = get_edge_server(edge_server_n);
	for(int i = 0; i < 2; i++){ 
		this.vcpu[i].next_available_time = 0;
		vcpu_start[i] = 0;
	}
	set_edge_server(&this, edge_server_n);
	shm_w_unlock();
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	
	while(1){
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
		pthread_mutex_lock(&tasks_mutex);
		while(!ready_to_receive_task() || es_leave_flag == 1){
			//check if the simulator is going to end
			if(es_leave_flag == 1){
				pthread_mutex_unlock(&tasks_mutex);
				#ifdef DEBUG_ES
				printf("%s: receive tasks leaving...\n", es_name);
				#endif
				pthread_exit(NULL);
			}
			pthread_cond_wait(&free_cond, &tasks_mutex);
		}
		pthread_mutex_unlock(&tasks_mutex);

		if(first) first = 0;
		else{
			pthread_mutex_lock(dispatcher_mutex);
			pthread_cond_signal(dispatcher_cond);
			pthread_mutex_unlock(dispatcher_mutex);
		}
		#ifdef DEBUG_ES
		printf("%s: Waiting for task in unnamed_pipe %d\n", es_name, edge_server_n-1);
		#endif
		
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

		read(unnamed_pipe[edge_server_n-1][0], &t, sizeof(VCPUTask));
		
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
		
		#ifdef DEBUG_ES
		printf("%s: Received task %d from unnamed_pipe %d\n", es_name, t.id, edge_server_n-1);
		#endif
		pthread_mutex_lock(&tasks_mutex);
		current_task = t;
		double ct = get_current_time();
		#ifdef DEBUG_ES
		printf("%s: Sending task %d to %d\n", es_name, t.id, t.vcpu);
		#endif
		shm_w_lock();
		this = get_edge_server(edge_server_n);
		this.vcpu[t.vcpu].next_available_time = ct + task_time_sec(this.vcpu[t.vcpu].processing_capacity, current_task.thousand_inst);
		set_edge_server(&this, edge_server_n);
		shm_w_unlock();
		vcpu_start[t.vcpu] = 1;
		pthread_cond_broadcast(&tasks_cond);
		pthread_mutex_unlock(&tasks_mutex);
		
		#ifdef DEBUG_ES
		printf("%s: SENDING SIGNAL\n", es_name);
		#endif
		//notify task manager that the server state has been updated
		pthread_mutex_lock(dispatcher_mutex);
		pthread_cond_signal(dispatcher_cond);
		pthread_mutex_unlock(dispatcher_mutex);
		
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
		
	}
	pthread_exit(NULL);
}

int vcpus_finished_tasks(){
	shm_r_lock();
	int performance_level = get_edge_server(edge_server_n).performance_level;
	shm_r_unlock();
	return !vcpu_start[0] && !(performance_level == 2 && vcpu_start[1]);
}

void * enter_maintenance(void * t){
    Message msg;
    char log[MSG_LEN];
    int pl, mm_msg_type = edge_server_n * 2 + 1, es_msg_type = edge_server_n * 2;
	pthread_sigmask(SIG_BLOCK, &block_set, NULL);
    while(1){
        msgrcv(mqid, &msg, sizeof(Message) - sizeof(long), mm_msg_type, 0);
        if(strcmp(msg.msg_text, "START") == 0){
        	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
        	
			pthread_mutex_lock(&maintenance_mutex);
			maintenance_start = 1;
			pthread_mutex_unlock(&maintenance_mutex);
        	
        	shm_w_lock();
        	EdgeServer this = get_edge_server(edge_server_n);
        	this.performance_level = 0;
        	set_edge_server(&this, edge_server_n);
        	shm_w_unlock();
        	pthread_mutex_lock(&tasks_mutex);
        	// Wait until vcpus finish all tasks
        	while(!vcpus_finished_tasks() || es_leave_flag == 1){
        		//check if the simulator is going to end
				if(es_leave_flag == 1){
					pthread_mutex_unlock(&tasks_mutex);
					msg.msg_type = es_msg_type;
        			strcpy(msg.msg_text, "ES_ABORT");
        			msgsnd(mqid, &msg, sizeof(Message) - sizeof(long), 0);
					sprintf(log, "%s: MAINTENANCE ABORTED (SIMULATOR CLOSING)", es_name); 
        			log_write(log);
					pthread_exit(NULL);
				}
        		pthread_cond_wait(&free_cond, &tasks_mutex);
        	}
        	pthread_mutex_unlock(&tasks_mutex);
			
			//confirm that the maintenance start message has been received
        	msg.msg_type = es_msg_type;
        	strcpy(msg.msg_text, "ES_START");
        	msgsnd(mqid, &msg, sizeof(Message) - sizeof(long), 0);
        	sprintf(log, "%s: CHANGED PERFORMANCE TO 0", es_name); 
        	log_write(log);
        	sprintf(log, "%s: STARTING MAINTENANCE", es_name);
        	log_write(log);
        }else if(strcmp(msg.msg_text, "END") == 0){
        	//end maintenance, reactivating the vcpus and restoring the performance level
        	pthread_mutex_lock(&maintenance_mutex);
        	maintenance_start = 0;
        	pthread_mutex_unlock(&maintenance_mutex);
        	
        	shm_w_lock();
        	EdgeServer this = get_edge_server(edge_server_n);
        	pl = get_performance_change_flag();
        	this.performance_level = pl;
        	this.n_maintenances++;
        	set_edge_server(&this, edge_server_n);
        	shm_w_unlock();
            
        	pthread_mutex_lock(&tasks_mutex);
        	pthread_cond_signal(&free_cond);
        	pthread_mutex_unlock(&tasks_mutex);
        	
        	//confirm that the maintenance end message has been received
        	msg.msg_type = es_msg_type;
        	strcpy(msg.msg_text, "ES_END");
        	msgsnd(mqid, &msg, sizeof(Message) - sizeof(long), 0);
        	sprintf(log, "%s: ENDING MAINTENANCE", es_name);
        	log_write(log);
        	
        	sprintf(log, "%s: CHANGED PERFORMANCE BACK TO %d", es_name, pl); 
        	log_write(log);
        	
        	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
        }
    }
    pthread_exit(NULL);
}

int performance_changed(){
	shm_r_lock();
	int new_flag = get_performance_change_flag();
	int old_perf = get_edge_server(edge_server_n).performance_level;
	shm_r_unlock();
	return new_flag != old_perf;
}

int maintenance_ongoing(){
	pthread_mutex_lock(&maintenance_mutex);
	int maint = maintenance_start;
	pthread_mutex_unlock(&maintenance_mutex);
	return maint;
}

int check_es_leave_flag(){
	pthread_mutex_lock(&tasks_mutex);
	int leave = es_leave_flag;
	pthread_mutex_unlock(&tasks_mutex);
	return leave;
}

void * check_performance(void * t){
    int performance_change_flag;
    char msg[MSG_LEN];
    pthread_sigmask(SIG_BLOCK, &block_set, NULL);
    while(1){
    	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    	
    	pthread_mutex_lock(performance_changed_mutex);
    	while(maintenance_ongoing() || !performance_changed() || check_es_leave_flag()){
    		//check if the simulator is going to end
			if(check_es_leave_flag()){
				pthread_mutex_unlock(performance_changed_mutex);
				#ifdef DEBUG_ES
				printf("%s: check performance leaving...\n", es_name);
				#endif
				pthread_exit(NULL);
			}
    		pthread_cond_wait(performance_changed_cond, performance_changed_mutex);
    	}
    	pthread_mutex_unlock(performance_changed_mutex);

        shm_w_lock();
        performance_change_flag = get_performance_change_flag();
        EdgeServer this = get_edge_server(edge_server_n);
        this.performance_level = performance_change_flag;
        set_edge_server(&this, edge_server_n);
        shm_w_unlock();
        
        sprintf(msg, "%s: CHANGED PERFORMANCE TO %d", es_name, performance_change_flag); 
        log_write(msg);
                
        pthread_mutex_lock(&tasks_mutex);
        pthread_cond_broadcast(&free_cond);
        pthread_mutex_unlock(&tasks_mutex);
        
        pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    }
    pthread_exit(NULL);
}

void clean_es_resources(){
	close(unnamed_pipe[edge_server_n-1][0]);
    pthread_mutex_destroy(&tasks_mutex);
    pthread_mutex_destroy(&maintenance_mutex);
    pthread_cond_destroy(&tasks_cond);
    pthread_cond_destroy(&free_cond);
}

void es_termination_handler(int signum) {
    if(signum == SIGUSR1){ // handling of SIGUSR1
    	#ifdef DEBUG_ES
    	printf("%s: sigusr1\n", es_name);
    	#endif
    	
    	//notify possible waiting threads
    	es_leave_flag = 1;
    	pthread_mutex_lock(&tasks_mutex);
    	pthread_cond_broadcast(&tasks_cond);
    	pthread_mutex_unlock(&tasks_mutex);
    	
    	pthread_mutex_lock(&tasks_mutex);
    	pthread_cond_broadcast(&free_cond);
    	pthread_mutex_unlock(&tasks_mutex);
    	
    	pthread_mutex_lock(performance_changed_mutex);
    	pthread_cond_broadcast(performance_changed_cond);
    	pthread_mutex_unlock(performance_changed_mutex);
    	
    	
    	for(int i = 0; i < 2; i++) pthread_cancel(vcpu_thread[i]);
    	pthread_cancel(task_thread);
		pthread_cancel(maintenance_thread);
		pthread_cancel(performance_thread);
    	
    	for(int i = 0; i < 2; i++) pthread_join(vcpu_thread[i], NULL);
    	pthread_join(task_thread, NULL);
		pthread_join(maintenance_thread, NULL);
		pthread_join(performance_thread, NULL);
        clean_es_resources();
        #ifdef DEBUG_ES
        printf("%s: died\n", es_name);
        #endif
        exit(0);
    }
}


int edge_server(int es_n){
	char msg[MSG_LEN];
	edge_server_n = es_n;

    //define a handler for SIGUSR1
    es_new_action.sa_flags = SA_RESTART;
    es_new_action.sa_mask = block_set;
    es_new_action.sa_handler = &es_termination_handler;
    sigaction(SIGUSR1,&es_new_action,NULL);
    
    //ignore SIGINT and SIGTSTP (these are handled by the system manager)
    es_new_action.sa_handler = SIG_IGN;
    sigaction(SIGINT, &es_new_action, NULL);
    sigaction(SIGTSTP, &es_new_action, NULL);
	
	shm_w_lock();
	EdgeServer this = get_edge_server(edge_server_n);
	strcpy(es_name, this.name);
    for(int i = 0; i < 2; i++) vcpu_capacity[i] = this.vcpu[i].processing_capacity;
	this.performance_level = 1;
    this.n_maintenances = 0;
    this.n_tasks_done = 0;
	set_edge_server(&this, edge_server_n);
	shm_w_unlock();
	
	sprintf(msg, "%s READY", es_name);
	log_write(msg);
	
    //notify the maintenance manager of the creation of the edge_server
    int es_msg_type = edge_server_n * 2;
    mm_msg.msg_type = es_msg_type;
    strcpy(mm_msg.msg_text, "ES CREATED");
    if (msgsnd(mqid, &mm_msg, sizeof(Message)-sizeof(long), 0) < 0){
        char inf[MSG_LEN];
        sprintf(inf, "IT WAS NOT POSSIBLE TO NOTIFY THE MAINTENANCE MANAGER OF THE CREATION OF THE EDGE SERVER %s", es_name);
        log_write(inf);
    }
	
	for(int i = 0; i < 2; i++){
		ids[i] = i;
		pthread_create(&vcpu_thread[i], NULL, vcpu, &ids[i]);
	}
	
	pthread_create(&maintenance_thread, NULL, enter_maintenance, NULL);
	pthread_create(&performance_thread, NULL, check_performance, NULL);
	pthread_create(&task_thread, NULL, receive_tasks, NULL);
	
	sigprocmask(SIG_UNBLOCK, &block_set, NULL);
	
	sigprocmask(SIG_BLOCK, &block_set_no_sigusr1, NULL);
	
    for(int i = 0; i < 2; i++) pthread_join(vcpu_thread[i], NULL);
    pthread_join(task_thread, NULL);
	pthread_join(maintenance_thread, NULL);
	pthread_join(performance_thread, NULL);
	
	clean_es_resources();
	return 0;
}

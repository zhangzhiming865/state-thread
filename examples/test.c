/*
 * test.c
 *
 *  Created on: Jul 19, 2019
 *      Author: zhangzm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <signal.h>
#include <pwd.h>
#include <pthread.h>
#include <stdarg.h>
#include <sys/time.h>
#include "st.h"

extern __thread int self_index;
static inline void log_out_msg(const char* fun, char* fmt, ...)
{
	char buf[4096];
	struct timeval now;
	gettimeofday(&now, NULL);
	int len = sprintf(buf, "time %ld:%ld %d.%012lx %s() ",
			now.tv_sec, now.tv_usec, self_index, (uint64_t)st_thread_self(), fun);
	va_list arglist;
	va_start( arglist, fmt );
	len += vsprintf(buf+len, fmt, arglist);
	va_end(arglist);
	int ret = write(1, buf, len);
	(void)ret;
}

#define LOG_ERROR(msg, ...) log_out_msg(__func__, msg, ##__VA_ARGS__)
#define LOG_INFO(msg, ...) log_out_msg(__func__, msg, ##__VA_ARGS__)

void* thread_fun_schedule(void* arg)
{
	int value = 1;
	int count = 0;
	while(1){
		st_usleep(1);
		LOG_INFO("pthread is %lx self index is %d, st thread is %p\n",
				pthread_self(), self_index, st_thread_self());
		value++;
		schedule_to_vp(value);
		if(count++ > 10001){
			break;
		}
	}
	schedule_to_vp(1);
	return NULL;
}

void* thread_fun_cond(void* arg)
{
	st_cond_t cond = arg;
	LOG_INFO("start schedule_to_vp\n");
	schedule_to_vp(1);
	LOG_INFO("start st_cond_wait\n");
	st_cond_wait(cond, NULL);
	LOG_INFO("end st_cond_wait\n");
	return NULL;
}

void* thread_fun_lock(void* arg)
{
	st_mutex_t mutex = arg;
	schedule_to_vp(1);
	LOG_INFO("start st_mutex_lock\n");
	st_mutex_lock(mutex);
	LOG_INFO("end st_mutex_lock\n");
	st_usleep(100000);
	LOG_INFO("end st_mutex_lockx\n");
	return NULL;
}

void* thread_fun_interrupt(void* arg)
{
	st_thread_t main_thread = arg;
	schedule_to_vp(1);
	LOG_INFO("start st_thread_interrupt\n");
	st_thread_interrupt(main_thread);
	LOG_INFO("end st_thread_interrupt\n");
	return NULL;
}

int thread_nb = 0;
uint64_t loop_count = 100000;

int main(int argc, char* argv[])
{
	if(argc < 3){
		LOG_ERROR("error: cmd <thread nb> <loop count>\n");
		return -1;
	}
	thread_nb = atoi(argv[1]);
	loop_count = atoi(argv[2]);

	int ret = st_init(thread_nb);
	if(ret != 0){
		LOG_ERROR("error st init\n");
		return ret;
	}
	st_thread_t th;

	LOG_INFO("start schedule to 1\n");
	schedule_to_vp(1);
	LOG_INFO("start schedule to 0\n");
	schedule_to_vp(0);
	LOG_INFO("end schedule to 0\n");

	LOG_INFO("start create thread\n");
	th = st_thread_create(thread_fun_schedule, NULL, 1, 0);
	LOG_INFO("end create thread\n");
	st_thread_join(th, NULL);
	LOG_INFO("end join thread\n");

	st_cond_t cond = st_cond_new();
	st_mutex_t mutex = st_mutex_new();
	st_mutex_lock(mutex);

	LOG_INFO("start st_thread_create for cond\n");
	th = st_thread_create(thread_fun_cond, cond, 1, 0);
	LOG_INFO("start usleep 100000\n");
	st_usleep(100000);
	LOG_INFO("start signal %p\n", cond);
	st_cond_signal(cond);
	st_thread_join(th, NULL);
	LOG_INFO("end st_thread_create for cond\n");

	th = st_thread_create(thread_fun_lock, mutex, 1, 0);
	st_usleep(100000);
	st_mutex_unlock(mutex);
	st_thread_join(th, NULL);
	LOG_INFO("end st_thread_create for mutex\n");

	th = st_thread_create(thread_fun_interrupt, st_thread_self(), 1, 0);
	st_sleep(-1);
	st_thread_join(th, NULL);
	LOG_INFO("end st_thread_create for thread_fun_interrupt\n");

	int i = 0;
	struct timeval start, end;
	gettimeofday(&start, NULL);
	while(i < loop_count){
		schedule_to_vp(i++);
	}
	gettimeofday(&end, NULL);
	uint64_t usec = (end.tv_sec - start.tv_sec)*1000000 + end.tv_usec - start.tv_usec;
	LOG_INFO("use time %ldus %ldPS\n", usec, loop_count*1000000 / usec);
}




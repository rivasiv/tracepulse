//gcc writespeed.c -o writespeed -lpthread

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define VERSION "1.02"
#define BUFFERS_NUM 100
#define BUF_LEN 1024*1024
#define NUM_THREADS 8

int d[BUFFERS_NUM][BUF_LEN] = {0};
pthread_t thread[NUM_THREADS] = {0};
int thread_num[NUM_THREADS] = {0};

static void* thread_func(void *arg)
{
        int rv;
	int i;
	int *p = (int*)arg;
        int thread_id = *p;
	int fd = 0;
	char fname[256] = {0};

	sprintf(fname, "%d", thread_id);
	strcat(fname, "_thread_file");

	printf("running thread #%d \n", thread_id);

	fd = open(fname, O_CREAT|O_WRONLY|O_TRUNC);
	if (!fd)
	{
		printf("can't create file\n");
		exit(1);
	}

	//ssize_t write(int fd, const void *buf, size_t count);
	while(1)
	{
		for (i = 0; i < BUFFERS_NUM; i++)
			write(fd, d[i], BUF_LEN);
	}
}

int main(int argc, char *argv[])
{
	int rv = 0;
	int i, j;
	char fname[512] = {0};

	printf("version: %s\n", VERSION);

	if (argc < 2)
	{
		printf("writepspeed filaname\n");
		exit(1);
	}
	else
		strcpy(fname, argv[1]);

	//data init
	for (i = 0; i < BUFFERS_NUM; i++)
		for (j = 0; j < BUF_LEN; j++)
			d[i][j] = i;

        for (i = 0; i < NUM_THREADS; i++)
	{
		thread_num[i] = i;
                rv = pthread_create(&thread[i], NULL, thread_func, &thread_num[i]);
	}

        while(1)
                sleep(1);

	return rv;
}

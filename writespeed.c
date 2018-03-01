#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#define BUFFERS_NUM 100
#define BUF_LEN 1024*1024

int d[BUFFERS_NUM][BUF_LEN] = {0};

int main(int argc, char *argv[])
{
	int rv = 0;
	int i, j;
	int fd = 0;
	char fname[512] = {0};

	if (argc < 2)
	{
		printf("writepspeed filaname\n");
		exit(1);
	}
	else
		strcpy(fname, argv[1]);	

	//init
	for (i = 0; i < BUFFERS_NUM; i++)
		for (j = 0; j < BUF_LEN; j++)
			d[i][j] = i;

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

	return rv;
}

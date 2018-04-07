//gcc odwriter.c -g -lodp-linux -lpthread -o odwriter

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <odp_api.h>

#define SHM_PKT_POOL_SIZE      (512*2048)
#define SHM_PKT_POOL_BUF_SIZE  1856

#define BUFFER_SIZE 1024*1024*1
#define MAX_PACKET_SIZE 1600

#define VERSION "1.09"

//OPTIONS
#define NUM_THREADS 4
#define NUM_INPUT_Q 4
#define FILE_SAVING
//#define MODE_SCHED
#define MODE_QUEUE

#ifdef MODE_SCHED
	#undef MODE_QUEUE
#endif

typedef struct buffer_s
{
	unsigned char *buf;
	unsigned int size;
	struct buffer_s *next;
} buffer_t;

odp_instance_t odp_instance;
odp_pool_param_t params;
odp_pool_t pool;
odp_pktio_param_t pktio_param;
odp_pktio_t pktio;
odp_pktin_queue_param_t pktin_param;
odp_queue_t inq[NUM_INPUT_Q] = {0};	//keep handles to queues here

pthread_t writert;
pthread_t thread[NUM_THREADS] = {0};
int thread_num[NUM_THREADS] = {0};

buffer_t *queue_head = NULL;
buffer_t *queue_tail = NULL;
int queue_num = 0;
pthread_mutex_t mutex_lock = PTHREAD_MUTEX_INITIALIZER;

int queue_add(buffer_t *buf)
{
        if (!queue_head)
        {
                queue_head = buf;
                queue_tail = buf;
        }
        else
        {
                queue_tail->next = buf;
                queue_tail = buf;
        }
        buf->next = NULL;
        queue_num++;

        return queue_num;
}

buffer_t* queue_de()
{
        buffer_t *deq = NULL;

        if (queue_head)
        {
                deq = queue_head;
                if (queue_head != queue_tail)
                {
                        queue_head = queue_head->next;
                }
                else
                {
                        queue_head = queue_tail = NULL;
                }
                queue_num--;
                return deq;
        }
        else
                return NULL;
}

static void* thread_writer(void *arg)
{
	int thread_id = 0;
	int fd;
	char fname[256] = {0};
	unsigned char *buf = NULL;
	unsigned int position = 0;
        buffer_t *buffer = NULL;

	sprintf(fname, "%d", thread_id);
	strcat(fname, "_thread_file");
	fd = creat(fname, S_IRWXU);
	if (!fd)
	{
		printf("failed to create file. exiting from thread\n");
		return;
	}

	printf("writer thread started\n");

	while(1)
	{
		pthread_mutex_lock(&mutex_lock);
		buffer = queue_de();
		pthread_mutex_unlock(&mutex_lock);
		if (buffer)
		{
			write(fd, buffer->buf, buffer->size);
			free(buffer->buf);
			free(buffer);
		}
		else
			usleep(1000);		// - just for some case
	}
}

static void* thread_func(void *arg)
{
	int rv;
	odp_event_t ev;
	odp_packet_t pkt;
	int pkt_len;
	unsigned long old_pkts_cnt = 0, pkts_cnt = 0, pkts_diff = 0;
	unsigned long old_bytes_cnt = 0, bytes_cnt = 0;
	double mps = 0.0f;
	int *p = (int*)arg;
	int thread_id = *p;
	time_t now, old = 0;
	int bufs_in_queue;

	unsigned char *buf = NULL;
	unsigned int position = 0;

	printf("init odp thread\n");
	rv = odp_init_local(odp_instance, ODP_THREAD_WORKER);

	while (1)
        {
#ifdef FILE_SAVING
		if (!buf)
			buf = malloc(BUFFER_SIZE);
		if (!buf)
		{
			printf("failed to allocate ram. exiting from thread\n");
			return;
		}
#endif

#ifdef MODE_QUEUE
		ev = odp_queue_deq(inq[thread_id]);
#elif defined MODE_SCHED
		ev = odp_schedule(NULL, ODP_SCHED_NO_WAIT);
#endif
		pkt = odp_packet_from_event(ev);
		if (!odp_packet_is_valid(pkt))
			continue;

		pkt_len = (int)odp_packet_len(pkt);
		if (pkt_len)
		{
#ifdef FILE_SAVING
			if (position < BUFFER_SIZE-MAX_PACKET_SIZE)
			{
				memcpy(buf+position, odp_packet_l2_ptr(pkt, NULL), pkt_len);
				odp_schedule_release_atomic();
				position += pkt_len;
			}
			if (position >= BUFFER_SIZE-MAX_PACKET_SIZE)
			{
				buffer_t *buffer = malloc(sizeof(buffer_t));
				if (!buffer) 
					{printf("no ram!\n"); exit(1);}
				buffer->buf = buf;
				buffer->size = position;
				pthread_mutex_lock(&mutex_lock);
				bufs_in_queue = queue_add(buffer);
				//printf("bufs in queue: %d \n", bufs_in_queue);
				pthread_mutex_unlock(&mutex_lock);				
				buf = NULL;
				position = 0;
			}
#endif
			pkts_cnt++;
			bytes_cnt += pkt_len;
			now = time(0);
			if (now > old)
			{
				//pkts_diff = pkts_cnt - old_pkts_cnt;
				//printf("pkts_diff: %lu \n", pkts_diff);
				mps = (double)(pkts_cnt - old_pkts_cnt);
				//printf("mps: %f \n", mps);
				mps = mps/(1000*1000);
				printf("#%d: total pkts: %lu , Mp/s: %f, Mb: %lu, Mb/s: %lu\n",
					thread_id, pkts_cnt, mps,
					bytes_cnt/(1024*1024), 
					(bytes_cnt - old_bytes_cnt)/(1024*1024));
				old = now;
				old_bytes_cnt = bytes_cnt;
				old_pkts_cnt = pkts_cnt;
			}
		}
		odp_packet_free(pkt);
	}
}

int main(int argc, char *argv[])
{
	int rv = 0;
	char portnum[2] = "0";
	int i, opt;

	printf("version: %s\n", VERSION);

	while ((opt = getopt(argc, argv, "hp:")) != -1) 
	{
		switch (opt) 
		{
			case 'h':
				fprintf(stderr, "Usage: %s [-p port] [-h] help. By default port 0 will be used \n",
					 argv[0]);
				exit(EXIT_FAILURE);
			break;
			case 'p':
				strncpy(portnum, optarg, 1);
			break;
			default: /* '?' */
				fprintf(stderr, "Usage: %s [-p port] . By default port 0 will be used \n",
					 argv[0]);
				exit(EXIT_FAILURE);
		}
	}

	rv = odp_init_global(&odp_instance, NULL, NULL);
	if (rv) exit(1);
	rv = odp_init_local(odp_instance, ODP_THREAD_CONTROL);
	if (rv) exit(1);
	
	odp_pool_param_init(&params);
	params.pkt.seg_len = SHM_PKT_POOL_BUF_SIZE;
	params.pkt.len     = SHM_PKT_POOL_BUF_SIZE;
	params.pkt.num     = SHM_PKT_POOL_SIZE/SHM_PKT_POOL_BUF_SIZE;
	params.type        = ODP_POOL_PACKET;
	pool = odp_pool_create("packet_pool", &params);
	if (pool == ODP_POOL_INVALID) exit(1);

	odp_pktio_param_init(&pktio_param);
#ifdef MODE_SCHED
	pktio_param.in_mode = ODP_PKTIN_MODE_SCHED;
	printf("setting sched mode\n");
#elif defined MODE_QUEUE
	pktio_param.in_mode = ODP_PKTIN_MODE_QUEUE;
	printf("setting queue mode\n");
#endif
	pktio = odp_pktio_open(portnum, pool, &pktio_param);
	if (pktio == ODP_PKTIO_INVALID) exit(1);

	odp_pktin_queue_param_init(&pktin_param);
	pktin_param.op_mode     = ODP_PKTIO_OP_MT;
	pktin_param.hash_enable = 1;
	pktin_param.num_queues  = NUM_INPUT_Q;
#ifdef MODE_SCHED
	pktin_param.queue_param.sched.sync = ODP_SCHED_SYNC_ATOMIC;
	pktin_param.queue_param.sched.prio = ODP_SCHED_PRIO_DEFAULT;
#endif
	odp_pktin_queue_config(pktio, &pktin_param);
	odp_pktout_queue_config(pktio, NULL);
	rv = odp_pktio_start(pktio);
	if (rv) exit(1);

	printf("odp init result: %d \n", rv);
#ifdef MODE_QUEUE
	rv = odp_pktin_event_queue(pktio, inq, NUM_INPUT_Q);
	printf("num of input queues configured: %d \n", rv);
#endif
	for (i = 0; i < NUM_THREADS; i++)
	{
                thread_num[i] = i;
                rv = pthread_create(&thread[i], NULL, thread_func, &thread_num[i]);
	}

        rv = pthread_create(&writert, NULL, thread_writer, NULL);

	while(1)
		sleep(1);

	return rv;
}

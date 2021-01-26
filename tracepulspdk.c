//gcc tracepulspdk.c -o tracepulspdk -ltrace -I/root/libtrace/ -I/root/libtrace/lib

//gcc tracepulse.c -o tracepulse -ltrace -I/mnt/raw/gdwk/libtrace/ && sudo ./tracepulse 4
//sudo ./tracepulse 4 ring:eth0 erf:1.erf
//tracepulse 4 odp:"01:00.1" erf:trace.erf.gz

//combiner: we use combiner_ordered. so output data stored in ordered way.
//	    there are 3 combiner types: ordered, unordered, sorted.
//hasher:   3 hashers: balanced, unidirectional, bidirectional, we use balanced one,
//	    so the data spread packets across the threads in a balanced way

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <errno.h>


#include "spdk/stdinc.h"
#include "spdk/bdev.h"
#include "spdk/accel_engine.h"
#include "spdk/conf.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/queue.h"
#include "/root/spdk/module/bdev/nvme/bdev_nvme.h"  // This looks weird need to understood why it's here  // Probably would need to move to separate app/script //python?

#include <libtrace_parallel.h>	//resides just in /usr/local/include
#include "lib/libtrace_int.h"	//present only in libtrace sources
#include "config.h"		//required by libtrace_int.h

#define NVME_MAX_BDEVS_PER_RPC 32
#define DEVICE_NAME "PBlaze5"     //TBD: May want to do this as input param or dynamic recognition. 
#define NUM_THREADS 4
#define BUFFER_SIZE 1048576
#define EE_HEADER_SIZE 11
#define THREAD_OFFSET 0x100000000	//4Gb of offset for every thread 
#define THREAD_LIMIT 0x100000000	//space for every thread to write

//#define O_FILENAME "erf:pkts.erf"
#define DEBUG        1
#define DEBUG_THREAD 1

//TBD: This should be replaced to something can work with thread concurency
#ifdef DEBUG
 #define debug(x...) printf(x)
#else
 #define debug(x...)
#endif
#ifdef DEBUG_THREAD
 #define debug_thread(x...) printf(x)
#else
 #define debug_thread(x...)
#endif

typedef struct libtrace_thread_t libtrace_thread_t;


//storage for reporter thread (the only one)
struct r_store
{
	uint64_t pkts;		//received packets
	uint64_t bytes;		//received bytes
	libtrace_out_t *output; //output descriptor
};

struct sigaction sigact;
char in_uri[512] = {0};
char out_uri[512] = {0};
static int compress_level = -1;
static trace_option_compresstype_t compress_type = TRACE_OPTION_COMPRESSTYPE_NONE;

typedef struct global_s
{
	char *pci_nvme_addr;
} global_t;


// this limits us to exact hw //TBD : make it dynamic 
static global_t global = 
{
	.pci_nvme_addr = "0001:01:00.0",
};

/* Used to pass messages between fio threads */
struct pls_msg {
	spdk_msg_fn	cb_fn;
	void		*cb_arg;
};

//each thread contains its own target
typedef struct pls_target_s
{
	struct spdk_bdev	*bd;
	struct spdk_bdev_desc	*desc;
	struct spdk_io_channel	*ch;
	TAILQ_ENTRY(pls_target_s) link;
} pls_target_t;

/* A polling function */
struct pls_poller 
{
	spdk_poller_fn		cb_fn;
	void			*cb_arg;
	uint64_t		period_microseconds;
	TAILQ_ENTRY(pls_poller)	link;
};

//Added to track how many threads are allocated by libtrace. Increased on each new thread created
static int libtrace_thread_alloc_num = 0;

//local storage for each processing thread. should be allocated for every thread
typedef struct pls_thread_s
{
	uint64_t pkts;		//received packets
	uint64_t bytes;		//received bytes
	int idx;
	bool read_complete;		//flag, false when read callback not finished, else - tru
        unsigned char *buf;		//spdk dma buffer
	uint64_t position;		//position for buffer
	uint64_t offset;		//just for stats
	atomic_ulong a_offset;		//atomic offset for id 0 thread
	pthread_t pthread_desc;
	    int    thread_number;        /*Indicates the number thread is created with*/
     	struct spdk_thread *thread; /* spdk thread context */
        struct spdk_ring *ring; /* ring for passing messages to this thread */
	pls_target_t pls_target;
	TAILQ_HEAD(, pls_poller) pollers; /* List of registered pollers on this thread */
} pls_thread_t;

const char *names[NVME_MAX_BDEVS_PER_RPC];

// Control thread context
pls_thread_t pls_ctrl_thread;
// Poller thread context
pls_thread_t pls_read_thread;
//pls_thread_t pls_thread[NUM_THREADS];

int init_spdk(void);
int deinit_spdk(void);
void sigterminating(void *arg);

/*
 * Reports bdev cretion
 */
static void pls_bdev_init_done(void *cb_arg, int rc)
{
	printf("bdev init is done\n");
	*(bool *)cb_arg = true;
}

//this callback called when write is completed
static void pls_bdev_write_done_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	static unsigned int cnt = 0;
	pls_thread_t *t = (pls_thread_t*)cb_arg;

	//printf("bdev write is done\n");
	if (success)
	{
		debug("write completed successfully\n");
		//cnt++;
		__atomic_fetch_add(&cnt, 1, __ATOMIC_SEQ_CST);
	}
	else
		printf("write failed\n");

	if (cnt % 1000 == 0)
		printf("have %u successful write callabacks. thread #%d, offset: 0x%lx \n",
			 cnt, t->idx, t->offset);

	debug("before freeing ram in callback at addr: %p \n", t->buf); 
	spdk_dma_free(t->buf);
	debug("after freeing ram in callback at addr: %p \n", t->buf); 
	t->buf = NULL;

	//important to free bdev_io request, or it will lead to pool overflow (65K)
	spdk_bdev_free_io(bdev_io);
}

//this callback called when read is completed
static void pls_bdev_read_done_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	static unsigned int cnt = 0;
	pls_thread_t *t = (pls_thread_t*)cb_arg;

	//printf("bdev read is done\n");
	if (success)
	{
		t->read_complete = true;
		__atomic_fetch_add(&cnt, 1, __ATOMIC_SEQ_CST);
		//debug("read completed successfully\n");
	}
	else
		printf("read failed\n");

	if (cnt % 1000 == 0)
		printf("have %u successful read callabacks. thread #%d, offset: 0x%lx \n",
			 cnt, t->idx, t->offset);

	spdk_bdev_free_io(bdev_io);
}

static size_t pls_poll_thread(pls_thread_t *thread)
{
	struct pls_msg *msg;
	struct pls_poller *p, *tmp;
	size_t count;

	debug_thread("%s() called \n", __func__);

	/* Process new events */
	count = spdk_ring_dequeue(thread->ring, (void **)&msg, 1);
	if (count > 0) {
		msg->cb_fn(msg->cb_arg);
		free(msg);
	}

	/* Call all pollers */
	TAILQ_FOREACH_SAFE(p, &thread->pollers, link, tmp) {
		p->cb_fn(p->cb_arg);
	}

	debug_thread("%s() exited \n", __func__);

	return count;
}

//This is pass message function for spdk_allocate_thread
//typedef void (*spdk_msg_fn)(void *ctx);
static void pls_send_msg(spdk_msg_fn fn, void *ctx, void *thread_ctx)
{
        pls_thread_t *thread = thread_ctx;
        struct pls_msg *msg;
        size_t count;

	    printf("%s() called \n", __func__);

        msg = calloc(1, sizeof(*msg));
        assert(msg != NULL);

        msg->cb_fn = fn;
        msg->cb_arg = ctx;

        count = spdk_ring_enqueue(thread->ring, (void **)&msg, 1, NULL);
        if (count != 1) {
                SPDK_ERRLOG("Unable to send message to thread %p. rc: %lu\n", thread, count);
        }
}

static struct spdk_poller* pls_start_poller(void *thread_ctx, spdk_poller_fn fn,
                      			    void *arg, uint64_t period_microseconds)
{
        pls_thread_t *thread = thread_ctx;
        struct pls_poller *poller;

	printf("%s() called \n", __func__);

        poller = calloc(1, sizeof(*poller));
        if (!poller) 
	{
                SPDK_ERRLOG("Unable to allocate poller\n");
                return NULL;
        }

        poller->cb_fn = fn;
        poller->cb_arg = arg;
        poller->period_microseconds = period_microseconds;

        TAILQ_INSERT_TAIL(&thread->pollers, poller, link);

        return (struct spdk_poller *)poller;
}

static void pls_stop_poller(struct spdk_poller *poller, void *thread_ctx)
{
	struct pls_poller *lpoller;
	pls_thread_t *thread = thread_ctx;

	printf("%s() called \n", __func__);

	lpoller = (struct pls_poller *)poller;

	TAILQ_REMOVE(&thread->pollers, lpoller, link);

	free(lpoller);
}

/* Reports status of created bde over NVME device. 
 *
 *  @{Params} :
 *     @{in} void *cb_ctx      : Context structure with info we wand passthru the creation process
 *     @{in} size_t bdev_count : Count of bdev devices
 *     @{in} int rc            : Return code propagated. Errno codes with '-' used.
 * 
 *   @{Return} : None
 * 
 * */

static void
tracepulspdk_bdev_nvme_attach_controller_done (void *cb_ctx, size_t bdev_count, int rc)
{
	size_t i;

	if (cb_ctx != NULL) {
		printf("Error! %s: SPDK bdev wrong context.", __FUNCTION__);
		return;
	}

	if (rc < 0) {
		printf("Error! %s: SPDK bdev returns error %d(%s).", __FUNCTION__, -errno, strerror(-errno));
		return;
	}

	for (i = 0; i < bdev_count; i++) {
		printf("Error! %s: SPDK bdev %s added!", __FUNCTION__, names[i]);
	}

    return;
}

/*
 *  SPDK thread functions 
 */
static int
pls_reactor_thread_op(struct spdk_thread *thread, enum spdk_thread_op op)
{

    debug("Debug! Entered %s.", __FUNCTION__ );

	switch (op) {
		case SPDK_THREAD_OP_NEW:
			//pls_send_msg(thread);
			printf("\nReachecd scheduler");
			return 0; 
		case SPDK_THREAD_OP_RESCHED:
			printf("\nShouldn't be here");
			return -1;
		default:
			return -ENOTSUP;
	}

	return 0;
}

static bool
pls_reactor_thread_op_supported(enum spdk_thread_op op)
{
    debug("Debug! Entered %s.", __FUNCTION__ );

	switch (op) {
		case SPDK_THREAD_OP_NEW:
			return true;
		case SPDK_THREAD_OP_RESCHED:
			return false;
		default:
			return false;
	}

	return false;
}

/*
 *  End of SPDK thread functions 
 */


int init_spdk(void)
{
	int i, rv = 0;
	size_t cnt;
	bool done = false;
	struct spdk_env_opts opts;
	struct spdk_nvme_transport_id trid = {}; //identifies unique endpoint on NVMe fabric
	struct spdk_nvme_host_id hostid = {};
	size_t count = NVME_MAX_BDEVS_PER_RPC;
	uint32_t prchk_flags = 0;	
	//struct spdk_conf *config;

	printf("%s() called \n", __func__);

	//enable logging (optional)
//TBD Move vflage to make file later 
#define PULSE_SPDK_LOG_ENABLE 
#ifdef PULSE_SPDK_LOG_ENABLE
	spdk_log_set_print_level(SPDK_LOG_DEBUG);
	spdk_log_open(NULL);   // This enable logging all log prior to this set would be dropped.  
	spdk_log_set_flag("log");
#endif /*PULSE_SPDK_LOG_ENABLE*/


	/* Parse the SPDK configuration file */

	//just allocate mem via calloc
	//
#if 0
	config = spdk_conf_allocate();
	if (!config) {
		SPDK_ERRLOG("Unable to allocate configuration file\n");
		return -1;
	}

	//read user file to init spdk_conf struct
	rv = spdk_conf_read(config, "bdev_pls.conf");
	if (rv != 0) {
		SPDK_ERRLOG("Invalid configuration file format\n");
		spdk_conf_free(config);
		return -1;
	}
	if (spdk_conf_first_section(config) == NULL) {
		SPDK_ERRLOG("Invalid configuration file format\n");
		spdk_conf_free(config);
		return -1;
	}
	spdk_conf_set_as_default(config);
#endif

	/* Initialize the environment library */
	spdk_env_opts_init(&opts);
	opts.name = "bdev_pls";

	if (spdk_env_init(&opts) < 0) {
		SPDK_ERRLOG("Unable to initialize SPDK env\n");
		//spdk_conf_free(config);
		return -1;
	}

    // TBD check if we need to use spdk_env_get_current_core and control spdk thread creation per core or it's done in libtrace callbacks
	// Should num of cores used be correcponded with number of threads? 
	rv = spdk_thread_lib_init_ext(pls_reactor_thread_op, pls_reactor_thread_op_supported, 0 /*sizeof(struct pls_thread_ctrl_t*/);
	if (0 != rv)
	{
		printf("Errot! Thread module init failed.");
		return rv;
	}

	spdk_unaffinitize_thread();

	//ring init (calls rte_ring_create() from DPDK inside)
	pls_ctrl_thread.ring = spdk_ring_create(SPDK_RING_TYPE_MP_SC, 4096, SPDK_ENV_SOCKET_ID_ANY);
	if (!pls_ctrl_thread.ring) 
	{
		SPDK_ERRLOG("failed to allocate ring\n");
		return -1;
	}

	// Initializes the calling(current) thread for I/O channel allocation
	/* typedef void (*spdk_thread_pass_msg)(spdk_msg_fn fn, void *ctx,
				     void *thread_ctx); */
#if 1
    struct spdk_cpuset cpumask;
	spdk_cpuset_zero(&cpumask);
	spdk_cpuset_set_cpu(&cpumask, spdk_env_get_current_core(), true);  //Assigns control threas to the base core.
	pls_ctrl_thread.thread = spdk_thread_create("pls_ctrl_thread", &cpumask);	
#else
	pls_ctrl_thread.thread = spdk_allocate_thread(pls_send_msg, pls_start_poller,
                                 pls_stop_poller, &pls_ctrl_thread, "pls_ctrl_thread");
#endif
    if (!pls_ctrl_thread.thread) 
	{
        spdk_ring_free(pls_ctrl_thread.ring);
        SPDK_ERRLOG("failed to allocate thread\n");
        return -1;
    }

	TAILQ_INIT(&pls_ctrl_thread.pollers);

	/* Initialize the acceleration engine. */
	spdk_accel_engine_initialize();

	/* Initialize the bdev layer */
	spdk_bdev_initialize(pls_bdev_init_done, &done);

	/* First, poll until initialization is done. */
	do {
		pls_poll_thread(&pls_ctrl_thread);
	} while (!done);

	/*
	 * Continue polling until there are no more events.
	 * This handles any final events posted by pollers.
	 */
	do {
		cnt = pls_poll_thread(&pls_ctrl_thread);
	} while (cnt > 0);


	//create device
	/*
	int spdk_bdev_nvme_create(struct spdk_nvme_transport_id *trid,       # Transport structure
				struct spdk_nvme_host_id *hostid,                        # NVME over Fabric 
				const char *base_name,                                   # Device name - predefined
				const char **names,                                      # Names of bde returned. Out value 
				uint32_t count,                                          # bde devices count
				const char *hostnqn,                                     # 
				uint32_t prchk_flags,                                    # Optimization flags, looks like default are fine for now. TBD
				spdk_bdev_create_nvme_fn cb_fn,                          # Creation report callback
				void *cb_ctx);                                           # Used mostly as rpc context transfer, since we do not use it == NULL
		*/
	//fill up trid.
	trid.trtype = SPDK_NVME_TRANSPORT_PCIE;
	trid.adrfam = 0;
	memcpy(trid.traddr, global.pci_nvme_addr, strlen(global.pci_nvme_addr));

    // prchk_flags = SPDK_NVME_IO_FLAGS* -- TBD: Do additional checks on flags. Do not found any default one.

	printf("creating bdev device...\n");
	//in names returns names of created devices, in count returns number of devices
	//TBD : shouldn't use spdk_nvme_probe, spdk_nvme_connect, etc instead of create?
	rv = spdk_bdev_nvme_create(&trid, &hostid, DEVICE_NAME, names, count, NULL,
				   prchk_flags, tracepulspdk_bdev_nvme_attach_controller_done, NULL);

	if (rv)
	{
		printf("Error! Can't create bdev nvme device!\n");
		return -1;
	}
	for (i = 0; i < (int)count; i++) 
	{
		printf("#%d: device %s created \n", i, names[i]);
	}

	return rv;
}


/* @{Function} deinit_spdk
 * 
 *   @{Purpose} Handles SPDK deinit logic
 *      @{in} void
 *      @{out} int rv : 0 in success cases, negative ERRNO in failed cases 
 * 
 *   @{note} 
 */
int deinit_spdk(void)
{
	int rv = 0;

	/*TBD*/
    /* rpc_bdev_nvme_detach_controller */   // TBD Is it more resonable to use nvme connect/detuch?
	/*Ctrl-c handler if not to add it SPDK may stuck on hugepage allocation.*/

	// De-init SPDK thread module
    spdk_thread_lib_fini();

    if (libtrace_thread_alloc_num != 0) {
		printf("\nNot all threads were freed.");
	}

	return rv;
}

// PROCESSING THREADS CALLBACKS
// -----------------------------------------------------------------------------
//start callback function
static void* start_cb(libtrace_t *trace, libtrace_thread_t *thread, void *global)
{
	int rv;
	uint64_t offset;
	//uint64_t thread_limit;

	debug("%s(): enter\n", __func__);

	/* Create and initialize a counter struct */
	pls_thread_t *t = (pls_thread_t*)malloc(sizeof(pls_thread_t));
	if (!t)
	{
		printf("<error: can't allocate ram for thread storage!>\n");
		return NULL;
	}
	memset(t, 0x0, sizeof(pls_thread_t));

	//init offset
	offset = t->idx * THREAD_OFFSET; //each thread has a 4 Gb of space
	t->a_offset = offset;
	//thread_limit = offset + THREAD_LIMIT;
	printf("%s() called from thread #%d. offset: 0x%lx\n", __func__, t->idx, offset);

	t->ring = spdk_ring_create(SPDK_RING_TYPE_MP_SC, 4096, SPDK_ENV_SOCKET_ID_ANY);
	if (!t->ring) 
	{
		printf("failed to allocate ring\n");
		rv = -1; return NULL;
	}

	// Initializes the calling(current) thread for I/O channel allocation
	/* typedef void (*spdk_thread_pass_msg)(spdk_msg_fn fn, void *ctx,
				     void *thread_ctx); */
	
#if 0
/**
 * A function that is called each time a new thread is created.
 * The implementor of this function should frequently call
 * spdk_thread_poll() on the thread provided.
 *
 * \param thread The new spdk_thread.
 */
typedef int (*spdk_new_thread_fn)(struct spdk_thread *thread);

#endif
#if 1
    // As per my understanding previously this was assigned to some core only. But now it looks it's still possible to use it withoud assigned core 
	t->thread = spdk_thread_create("pls_worker_thread", 0);	
#else
	t->thread = spdk_allocate_thread(pls_send_msg, pls_start_poller,
                                 pls_stop_poller, (void*)t, "pls_worker_thread");
#endif

        if (!t->thread) 
	    {
            spdk_ring_free(t->ring);
            SPDK_ERRLOG("failed to allocate thread\n");
            return NULL;
        }

	TAILQ_INIT(&t->pollers);

	t->pls_target.bd = spdk_bdev_get_by_name(names[0]); //XXX - we always try to open device with idx 0
	if (!t->pls_target.bd)
	{
		printf("failed to get device\n");
		rv = 1; return NULL;
	}
	else
		printf("got device with name %s\n", names[0]);

	//returns a descriptor
	rv = spdk_bdev_open(t->pls_target.bd, 1, NULL, NULL, &t->pls_target.desc);
	if (rv)
	{
		printf("failed to open device\n");
		return NULL;
	}

    /*for each thread a separate I/O channel must be obtained*/
	printf("SPDK : Open io channel.\n");
	t->pls_target.ch = spdk_bdev_get_io_channel(t->pls_target.desc);
	if (!t->pls_target.ch) 
	{
		printf("Unable to get I/O channel for bdev.\n");
		spdk_bdev_close(t->pls_target.desc);
		rv = -1; return NULL;
	}

	printf("spdk thread init done.\n");

	return t;
}

static void stop_cb(libtrace_t *trace, libtrace_thread_t *thread, void *global, void *tls) 
{
	//pls_thread_t *t = (pls_thread_t*)tls;
	//libtrace_generic_t gen;

	//gen.ptr = t;
	//XXX: 0 - order for result, 0 - no order, but we need to add ordering by timestamp!
	//could be needed RESULT_PACKET
	//Inside it calls:
	//libtrace->combiner.publish(libtrace, t->perpkt_num, &libtrace->combiner, &res);
	//trace_publish_result(trace, thread, 0, gen, RESULT_USER);
}

// the packet callback
static libtrace_packet_t* packet_cb(libtrace_t *trace, libtrace_thread_t *thread,
				    void *global, void *tls, libtrace_packet_t *packet)
{
	int rv;
	uint64_t offset;
	int payloadlen = 0;
	unsigned short len;
	uint64_t nbytes = BUFFER_SIZE;
	pls_thread_t *t = (pls_thread_t*)tls;
	//XXX - this function is not exported!
	//int thread_num = trace_get_perpkt_thread_id(thread);

	payloadlen = trace_get_payload_length(packet);
	t->pkts++;
	t->bytes += payloadlen;

#if 0
	debug("thread #%d: len: %d, pkts: %lu, bytes: %lu \n", 
		thread_num, payloadlen, ts->pkts, ts->bytes);
#endif
	//1. if no buffer - allocate it
	if (!t->buf)
	{	//last param - ptr to phys addr(OUT)
		t->buf = spdk_dma_zmalloc(nbytes, 0x100000, NULL); 
		if (!t->buf) 
		{
			printf("ERROR: write buffer allocation failed\n");
			return NULL;
		}
		debug("allocated spdk dma buffer with addr: %p\n", t->buf);
	}

	//init offset
	offset = t->idx * THREAD_OFFSET; //each thread has a 4 Gb of space
	t->a_offset = offset;
	//thread_limit = offset + THREAD_LIMIT;

#if 1 //TBD: rivasiv - check compilation
	//2. create 0xEE header and copy packet to spdk dma buffer
	//in position we count num of bytes copied into buffer. 
	if (payloadlen)
	{
		if (t->position + payloadlen + EE_HEADER_SIZE < nbytes)
		{
			uint64_t ts = trace_get_erf_timestamp(packet);
			//debug("copying packet\n");
			//creating raw format header. 0xEE - magic byte (1byte)
			t->buf[t->position++] = 0xEE;
			//timestamp in uint64_t saved as big endian (8bytes)
			t->buf[t->position++] = ts >> 56 & 0xFF;
			t->buf[t->position++] = ts >> 48 & 0xFF;
			t->buf[t->position++] = ts >> 40 & 0xFF;
			t->buf[t->position++] = ts >> 32 & 0xFF;
			t->buf[t->position++] = ts >> 24 & 0xFF;
			t->buf[t->position++] = ts >> 16 & 0xFF;
			t->buf[t->position++] = ts >> 8 & 0xFF;
			t->buf[t->position++] = ts & 0xFF;
			
			//packet len (2bytes)
			len = (unsigned short)payloadlen;
			t->buf[t->position++] = len >> 8;
			t->buf[t->position++] = len & 0x00FF;
			//copying libtrace packet 
			memcpy(t->buf+t->position, packet, payloadlen);
#ifdef DUMP_PACKET
			hexdump(t->buf+t->position-EE_HEADER_SIZE , payloadlen+EE_HEADER_SIZE);
#endif
			t->position += payloadlen;
		}
		else
		{

			//TBD revize work with offsets
			printf("writing %lu bytes from thread# #%d, offset: 0x%lx\n", nbytes, t->idx, offset);

			t->offset = offset;
			t->a_offset = offset;
			rv = spdk_bdev_write(t->pls_target.desc, t->pls_target.ch, 
				t->buf, offset, /*position*/ nbytes, pls_bdev_write_done_cb, t);
			if (rv)
				printf("#%d spdk_bdev_write failed, offset: 0x%lx, size: %lu\n",
					t->idx, offset, nbytes);

			offset += nbytes;
			//offset += position;

			//need to wait for bdev write completion first
			while(t->buf)
			{
				usleep(10);
			}
			//position = 0;

			//allocate new buffer and place packet to it
			t->buf = spdk_dma_zmalloc(nbytes, 0x100000, NULL);
			if (!t->buf) 
			{
				printf("ERROR: write buffer allocation failed\n");
				return NULL;
			}
			debug("allocated spdk dma buffer with addr: %p\n", t->buf);

			//memcpy(t->buf+position, odp_packet_l2_ptr(pkt, NULL), pkt_len);
			//position += pkt_len;
		}
    }
#endif	
        // forwarding the packet to the reporter
        trace_publish_result(trace, thread, 0, (libtrace_generic_t){.pkt = packet}, RESULT_PACKET);

	//by returning NULL we say to libtrace that we are keeping the packet
	return NULL;
}
// -----------------------------------------------------------------------------

// REPORTER THREAD CALLBACKS
// -----------------------------------------------------------------------------
/* Starting callback for the reporter thread */
static void *start_reporter_cb(libtrace_t *trace, libtrace_thread_t *thread, void *global) 
{
	debug("%s(): enter\n", __func__);

	//char uri[512] = {0};
	struct r_store *rs = (struct r_store*)malloc(sizeof(struct r_store));
	if (!rs)
	{
		printf("<error: can't allocate ram for thread storage!>\n");
		return NULL;
	}
	memset(rs, 0x0, sizeof(struct r_store));

	//create output --------------------
	//strcpy(uri, O_FILENAME); 

	rs->output = trace_create_output(out_uri);
	if (trace_is_err_output(rs->output)) 
	{
		trace_perror_output(rs->output, "%s", out_uri);
		return NULL;
	}
	if (compress_level != -1) 
	{
		if (trace_config_output(rs->output, TRACE_OPTION_OUTPUT_COMPRESS,
					&compress_level)==-1) 
		{
			trace_perror_output(rs->output, "Unable to set compression level");
		}
	}

	if (trace_config_output(rs->output, TRACE_OPTION_OUTPUT_COMPRESSTYPE,
				&compress_type) == -1) 
	{
		trace_perror_output(rs->output, "Unable to set compression type");
	}

	trace_start_output(rs->output);
	if (trace_is_err_output(rs->output)) 
	{
		trace_perror_output(rs->output, "%s", out_uri);
		return NULL;
	}

	debug("%s(): exit\n", __func__);

	return rs;
}

// The result callback is invoked for each result that reaches the reporter thread
// (so anytime when someone calls trace_publish_result())
static void result_reporter_cb(libtrace_t *trace, libtrace_thread_t *sender,
        		       void *global, void *tls, libtrace_result_t *result)
{
	struct r_store *rs = (struct r_store*)tls;
	libtrace_packet_t *pkt;
	int payloadlen = 0;

	debug("%s()\n", __func__);

	pkt = (libtrace_packet_t *)result->value.pkt;
	if (pkt)
	{
		payloadlen = trace_get_payload_length(pkt);
		rs->pkts++;
		rs->bytes += payloadlen;
		debug("pkt in reporter from t #: %d, len: %d, total pkts: %lu, total bytes: %lu \n", 
			trace_get_perpkt_thread_id(sender), payloadlen, rs->pkts, rs->bytes);

		//writing to file
		if (result->type == RESULT_PACKET)
		{
			/* Write the packet to disk */
			trace_write_packet(rs->output, pkt);

			trace_free_packet(trace, pkt);
		}
	}
}

//called once in the end for reporter thread?
static void stop_reporter_cb(libtrace_t *trace, libtrace_thread_t *thread, 
			     void *global, void *tls) 
{
	struct r_store *rs = (struct r_store*)tls;

	debug("%s()\n", __func__);

	trace_destroy_output(rs->output);
}

// -----------------------------------------------------------------------------
//add this to Ctrl-C signal processing
void sigterminating(void *arg)
{
	libtrace_t *input = (libtrace_t*)arg;

	trace_pstop(input);
}

static void signal_handler(int sig)
{
    if (sig == SIGUSR1) 
	printf("Caught signal SIGUSR1 !\n");
    else if (sig == SIGUSR2)
	printf("Caught signal SIGUSR2 !\n");

/*TBD*/	

#if 0
    rv = deinit_spdk();
	//librtace cleanup should be handled 
#endif
}

int main(int argc, char *argv[])
{
	int rv = 0;
	int threads_num = 1;		//1 thread by default
	libtrace_t *input;
	//char *def_uri = "ring:eth0";	//default uri
	libtrace_callback_set_t *processing = NULL, *reporter = NULL;

	if (argc != 4)
	{
		printf("syntax is: num_treads INPUT OUTPUT\n");
		exit(1);
	}
	else
	{
		threads_num = atoi(argv[1]);
		strcpy(in_uri, argv[2]);
		strcpy(out_uri, argv[3]);
	}

	//signal handling
	sigact.sa_handler = signal_handler;
	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = 0;
	sigaction(SIGUSR1, &sigact, (struct sigaction *)NULL);

    /*Init SPDK*/ 
    rv = init_spdk();
	if (rv)
	{
		printf("Error! SPDK init failed. Exit.\n");
		//exit with error anyway, so no need to check status of deinit.
		deinit_spdk();
		exit(1);
	}

	//we create 2 callback sets: for processing and reporter threads
	processing = trace_create_callback_set();
	trace_set_starting_cb(processing, start_cb);
	trace_set_stopping_cb(processing, stop_cb);
	trace_set_packet_cb(processing, packet_cb);

	reporter = trace_create_callback_set();
	trace_set_starting_cb(reporter, start_reporter_cb);
	trace_set_stopping_cb(reporter, stop_reporter_cb);
	trace_set_result_cb(reporter, result_reporter_cb);

	/* Create the input trace object */
	input = trace_create(in_uri);
	if (trace_is_err(input)) 
	{
		trace_perror(input, "error creating trace");
		return 1;
	}

	/* Set the number of processing threads to use.
	If not set, libtrace will create one thread for each core it detects on your system. */
	printf("set %d threads \n", threads_num);
	trace_set_perpkt_threads(input, threads_num);

	/* Send every result to the reporter immediately, i.e. do not buffer them. */
        //trace_set_reporter_thold(input, 1);

	//there are 3 possible combiners: ordered, unordered, sorted. we use ordered.
	trace_set_combiner(input, &combiner_unordered, (libtrace_generic_t){0});	//XXX - strange syntax

	/* Try to balance our load across all processing threads. If
	we were doing flow analysis, we should use 
	HASHER_BIDIRECTIONAL instead to ensure that all packets for
	a given flow end up on the same processing thread. */

	trace_set_hasher(input, HASHER_BALANCE, NULL, NULL);

	/* Start the parallel trace using our callback sets. The NULL 
	* parameter here is where we can provide global data for the
	input trace
	Second param is global data available for all callbacks 
	Third param - callback set for processing threads
	Fourth param - callback set for reporter thread */
	if (trace_pstart(input, NULL, processing, reporter)) 
	{
		trace_perror(input, "Starting parallel trace");
		return 1;
	}

	/* This will wait for all the threads to complete */
	trace_join(input);

	/* Clean up everything that we've created */
	trace_destroy(input);
	trace_destroy_callback_set(processing);
	trace_destroy_callback_set(reporter);

    /*DeInit SPDK*/ 
    rv = deinit_spdk();
	if (rv)
	{
		printf("Error! SPDK deinit failed.\n");
		exit(1);
	}

	return rv;
}

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/queue.h>
#include <sys/resource.h>

#include <errno.h>
#include <assert.h>
#include <dlfcn.h>
#include <sched.h>
#include <unistd.h>

#include <rte_common.h>
#include <rte_memory.h>
#include <rte_memzone.h>
#include <rte_tailq.h>
#include <rte_eal.h>
#include <rte_atomic.h>
#include <rte_branch_prediction.h>
#include <rte_log.h>
#include <rte_per_lcore.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_ring.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_debug.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_interrupts.h>
#include <rte_pci.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_string_fns.h>

#include <linux/udp.h>

#include "directsock.h"
#include "ringbuffer.h"
#include "../mp_common.h"

#define DSOCKET_ERR(msg, ...) \
do {\
	fprintf(stderr, "directsocket Library:" msg, ##__VA_ARGS__); exit(1);\
}while(0)

#define DPRINTF(fmt, ...) fprintf(stderr, "[DSOCK] " fmt, ##__VA_ARGS__)

#define DS_ALIGN_CACHE __attribute__((aligned(64)))

#define CACHE_ALLOC_SIZE 2048
#define MAX_CPU_CNT 64
static int __fd_base = 0;

ringBuffer_typedef(struct rte_mbuf*, mbuf_ring);
struct dsocket{
	TAILQ_ENTRY(dsocket) link;
	int fd;
	int free;

	struct sockaddr_in addr;
	int domain;
	int type;
	int protocol;

	mbuf_ring rx_queue;
};

struct dsocket_cache{
	TAILQ_HEAD(_free_dc_head, dsocket) freelist;
	int fd_base;
	int fd_step;
	struct dsocket *cache;
	size_t cache_len;
} DS_ALIGN_CACHE;

static struct dsocket_cache dcache[MAX_CPU_CNT];
static struct dsocket *port2sock[65536];

static inline int get_cpus(void)
{
        return sysconf(_SC_NPROCESSORS_ONLN);
}

static inline int get_dsock_fd_base(void)
{
	int ret;
	if(__fd_base)
		return __fd_base;
	struct rlimit lim;
	ret = getrlimit(RLIMIT_NOFILE, &lim);
	if(ret)
		DSOCKET_ERR("getrlimit");
	__fd_base = lim.rlim_max << 1;
	DPRINTF("fd_base: %d\n", __fd_base);
	return __fd_base;
}

static void dsocket_cache_init(void)
{
	int i, j;
	for(i = 0; i < MAX_CPU_CNT; i++){
		dcache[i].fd_step = MAX_CPU_CNT;
		dcache[i].fd_base = __fd_base + i;
		dcache[i].cache = malloc(sizeof(struct dsocket) * CACHE_ALLOC_SIZE);
		dcache[i].cache_len = CACHE_ALLOC_SIZE;
		TAILQ_INIT(&dcache[i].freelist);
		struct dsocket *c = dcache[i].cache;
		int fd = dcache[i].fd_base;
		for(j = 0;j < (int)dcache[i].cache_len; j++){
			c[j].free = 1;	
			TAILQ_INSERT_TAIL(&dcache[i].freelist, &c[j], link);
			c[j].fd = fd;
			fd += dcache[i].fd_step;
		}
	}
}

static inline struct dsocket *get_dsocket(int cpuid)
{
	//XXX
	if(TAILQ_EMPTY(&dcache[cpuid].freelist))
		return NULL;
	//remove first
	struct dsocket *s = TAILQ_FIRST(&dcache[cpuid].freelist);
	TAILQ_REMOVE(&dcache[cpuid].freelist, s, link);
	assert(s->free);
	s->free = 0;
	DPRINTF("alloc dsocket: %d %p\n", s->fd, s);
	return s;
}

static inline void put_dsocket(int cpuid, struct dsocket *s)
{
	assert(!s->free);
	DPRINTF("put dsocket: %d %p\n", s->fd, s);
	s->free = 1;
	/* hot cache */
	TAILQ_INSERT_HEAD(&dcache[cpuid].freelist, s, link);
}

static inline int is_dsocket(int fd)
{
	return fd >= __fd_base;
}

static inline struct dsocket *lookup_dsocket(int cpuid, int fd)
{
	struct dsocket *s = NULL;
	if(!is_dsocket(fd))
		return NULL;
	unsigned int idx = (fd - dcache[cpuid].fd_base) / dcache[cpuid].fd_step;
	assert(idx < dcache[cpuid].cache_len);
	s = &dcache[cpuid].cache[idx];
	assert(s->fd == fd);
	return s;
}

//TODO
static struct rte_ring *rx_ring;
static struct rte_ring *tx_ring;
static struct rte_ring *ctl_ring;
static unsigned int client_id;
static struct rte_mempool *mpool;
static struct rte_mempool *ctl_pool;

static int (*real_socket)(int, int, int) = NULL;
static int (*real_bind)(int, const struct sockaddr*, socklen_t) = NULL;
static ssize_t (*real_recvfrom)(int , void *, size_t, int,
		struct sockaddr *, socklen_t *);

static inline void ds_send_rpc_conn(int conn)
{
	struct rpc_proto *cmd = NULL;
	rte_mempool_get(ctl_pool, &cmd);
	assert(cmd != NULL);
	cmd->type = conn ? RPC_PROTO_TYPE_CONN : RPC_PROTO_TYPE_DISCONN;
	DPRINTF("ds_send_rpc_conn\n");
	if(rte_ring_enqueue(ctl_ring, cmd))
		DSOCKET_ERR("ds_send_rpc_conn fail\n");

}

static inline void ds_send_rpc_bind(struct dsocket *s)
{
	struct rpc_proto *cmd = NULL;
	rte_mempool_get(ctl_pool, &cmd);
	assert(cmd != NULL);
	cmd->type = RPC_PROTO_TYPE_BIND;
	cmd->protocol = s->type;
	cmd->addr = s->addr;
	DPRINTF("ds_send_rpc_bind\n");
	if(rte_ring_enqueue(ctl_ring, cmd))
		DSOCKET_ERR("ds_send_rpc_bind fail\n");
}

__attribute__((constructor))
void directsock_init(void)
{
	int retval;

	char *token;
	char *arg = getenv("DS_PARAM");
	if(!arg)
		DSOCKET_ERR("env DS_PARAM not defined\n");
	int argc = 0;
	int i;
	char *argv[128];
	char tmp[] = "dummy";
	argv[0] = tmp;
	argc++;
	token = strtok(arg, " ");
	do {
		argv[argc++] = token;
	}
	while ((token = strtok(NULL, " ")) && argc < 128);
	for(i = 0; i < argc; i++)
		DPRINTF("token: \"%s\"\n", argv[i]);
	if ((retval = rte_eal_init(argc, argv)) < 0)
		DSOCKET_ERR("rte_eal_init\n");
	if (rte_pmd_init_all() < 0 || rte_eal_pci_probe() < 0)
		DSOCKET_ERR("rte_pmd_init_all\n");

	if (rte_eth_dev_count() == 0)
		rte_exit(EXIT_FAILURE, "No Ethernet ports - bye\n");

	arg = getenv("DS_CLIENT_ID");
	if(!arg)
		DPRINTF("no DS_CLIENT_ID\n");
	unsigned long temp;
	temp = strtoul(arg, NULL, 10);
	client_id = (unsigned int)temp;
	DPRINTF("client id: %d\n", client_id);
	rx_ring = rte_ring_lookup(get_rx_queue_name(client_id));
	if (rx_ring == NULL)
		rte_exit(EXIT_FAILURE, "Cannot get RX ring - is server process running?\n");
	tx_ring = rte_ring_lookup(get_tx_queue_name(client_id));
	if (tx_ring == NULL)
		rte_exit(EXIT_FAILURE, "Cannot get TX ring - is server process running?\n");
	mpool = rte_mempool_lookup(PKTMBUF_POOL_NAME);
	if (mpool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot get mempool for mbufs\n");
	ctl_pool = rte_mempool_lookup(MP_CONTROL_POOL_NAME);
	if (ctl_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot get mempool for ctl_pool\n");
	ctl_ring = rte_ring_lookup(get_ctl_queue_name(client_id));
	if (ctl_ring == NULL)
		rte_exit(EXIT_FAILURE, "Cannot get CTL ring - is server process running?\n");

	get_dsock_fd_base();
	dsocket_cache_init();
	
	real_socket = dlsym(RTLD_NEXT, "socket");
	real_bind = dlsym(RTLD_NEXT, "bind");
	real_recvfrom = dlsym(RTLD_NEXT, "recvfrom");

	ds_send_rpc_conn(1);
	DPRINTF("init done\n");
}

__attribute__((destructor))
void directsocket_uninit(void)
{
	ds_send_rpc_conn(0);
}

int socket(int domain, int type, int protocol)
{
	if(domain != AF_INET)
		return real_socket(domain, type, protocol);
	if(type != SOCK_DGRAM)
		return real_socket(domain, type, protocol);

	struct dsocket *s = get_dsocket(0);
	s->domain = domain;
	s->type = type;
	s->protocol = protocol;
	bufferInit(s->rx_queue, 1023, struct rte_mbuf*);
	return s->fd;
}

int bind(int sockfd, const struct sockaddr *addr,
		                socklen_t addrlen)
{
	if(!is_dsocket(sockfd))
		return real_bind(sockfd, addr, addrlen);

	if(!addr)
		goto err;

	if(addrlen > sizeof(struct sockaddr_in))
		goto err;

	struct dsocket *s = lookup_dsocket(0, sockfd);
	if(!s)
		goto err;
	unsigned short port = ntohs(s->addr.sin_port);
	if(!port2sock[port])
		return -EBUSY;
	memcpy(&s->addr, addr, addrlen);
	//TODO
	ds_send_rpc_bind(s);
	port2sock[port] = s;
	DPRINTF("sock %d, bind port %d\n", s->fd, port);
	return 0;
err:
	errno = EINVAL;
	return -EINVAL;
}

static int __poll_fd(void)
{
	struct rte_mbuf *m = NULL;
	while(1){
		if (rte_ring_dequeue(rx_ring, &m)) {
			struct udphdr *hdr = rte_pktmbuf_mtod(m, struct udphdr*);
			uint16_t port = ntohs(hdr->dest);
			struct dsocket *s = port2sock[port];
			if(!s)
				rte_pktmbuf_free(m);
			bufferWrite(&s->rx_queue, m);
			break;
		}
	}
}

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
		struct sockaddr *src_addr, socklen_t *addrlen)
{
	if(!is_dsocket(sockfd))
		return real_recvfrom(sockfd, buf, len, flags, src_addr, addrlen);
	struct dsocket *s = lookup_dsocket(0, sockfd);
	struct rte_mbuf *m = NULL;
	if(!s)
		goto err;
	if(isBufferEmpty(&s->rx_queue)){
		if(flags & MSG_DONTWAIT)
			return -EAGAIN;
		//polling
		do{
			__poll_fd();
		}while(isBufferEmpty(&s->rx_queue));
	}
	bufferRead(&s->rx_queue, m);
	return 0;
err:
	errno = EINVAL;
	return -EINVAL;
}



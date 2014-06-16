#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>

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

#include "directsock.h"

#define DSOCKET_ERR(msg, ...) \
do {\
	fprintf(stderr, "directsocket Library:" msg, ##__VA_ARGS__); exit(1);\
}while(0)

#define DPRINTF(fmt, ...) fprintf(stderr, "[DSOCK] " fmt, ##__VA_ARGS__)

#define DS_ALIGN_CACHE __attribute__((aligned(64)))

#define CACHE_ALLOC_SIZE 2048
#define MAX_CPU_CNT 32
static int __fd_base = 0;

struct dsocket{
	TAILQ_ENTRY(dsocket) link;
	int fd;
	int free;
};

struct dsocket_cache{
	TAILQ_HEAD(_free_dc_head, dsocket) freelist;
	int fd_base;
	int fd_step;
	struct dsocket *cache;
	size_t cache_len;
} DS_ALIGN_CACHE;

static struct dsocket_cache dcache[MAX_CPU_CNT];

static inline int get_cpus()
{
        return sysconf(_SC_NPROCESSORS_ONLN);
}

static inline int get_dsock_fd_base()
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

static void dsocket_cache_init()
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
		for(j = 0;j < dcache[i].cache_len; j++){
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
	//TODO
	return NULL;
}


__attribute__((constructor))
void directsock_init(void)
{
	get_dsock_fd_base();
	dsocket_cache_init();
}

__attribute__((destructor))
void directsocket_uninit(void)
{
}

int socket(int domain, int type, int protocol)
{
	int fd = -1;
	static int (*real_socket)(int, int, int) = NULL;
	if (!real_socket)
		real_socket = dlsym(RTLD_NEXT, "socket");
	if(domain != AF_INET)
		return real_socket(domain, type, protocol);
	if(type != SOCK_DGRAM)
		return real_socket(domain, type, protocol);

	struct dsocket *s = get_dsocket(0);
	return s->fd;
}

int bind(int sockfd, const struct sockaddr *addr,
		                socklen_t addrlen)
{
	static int (*real_bind)(int, const struct sockaddr*, socklen_t) = NULL;
	if (!real_bind)
		real_bind = dlsym(RTLD_NEXT, "bind");
	if(!is_dsocket(sockfd)){
		return real_bind(sockfd, addr, addrlen);
	}

	//TODO
	DPRINTF("bind\n");
	errno = EINVAL;
	return -EINVAL;
}


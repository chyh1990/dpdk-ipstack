#ifndef _UTIL_H__
#define _UTIL_H__
//#define DPRINTF(fmt, ...)

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <linux/icmp.h>
#include <linux/ip.h>

#include <rte_prefetch.h>
#include <rte_per_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_lcore.h>
#include <rte_ether.h>

#ifndef DPRINTF
#define DPRINTF(fmt, ...) fprintf(stderr, "[CPU %d] " fmt, rte_lcore_id(), ##__VA_ARGS__)
#endif

static inline unsigned short cksum(void *_buf, int size)
{
	unsigned short *buffer = _buf;
	unsigned long cksum=0;
	while(size >1)
	{
		cksum+=*buffer++;
		size -=sizeof(unsigned short);
	}
	if(size)
		cksum += *(unsigned char*)buffer;

	cksum = (cksum >> 16) + (cksum & 0xffff);
	cksum += (cksum >>16);
	return (unsigned short)(~cksum);
}

static inline unsigned short ip_fast_csum(const void *iph, unsigned int ihl)
{
	unsigned int sum;

	asm("  movl (%1), %0\n"
	    "  subl $4, %2\n"
	    "  jbe 2f\n"
	    "  addl 4(%1), %0\n"
	    "  adcl 8(%1), %0\n"
	    "  adcl 12(%1), %0\n"
	    "1: adcl 16(%1), %0\n"
	    "  lea 4(%1), %1\n"
	    "  decl %2\n"
	    "  jne      1b\n"
	    "  adcl $0, %0\n"
	    "  movl %0, %2\n"
	    "  shrl $16, %0\n"
	    "  addw %w2, %w0\n"
	    "  adcl $0, %0\n"
	    "  notl %0\n"
	    "2:"
	    /* Since the input registers which are loaded with iph and ih
	       are modified, we must also specify them as outputs, or gcc
	       will assume they contain their original values. */
	    : "=r" (sum), "=r" (iph), "=r" (ihl)
	    : "1" (iph), "2" (ihl)
	       : "memory");
	return (unsigned short)sum;
}

static inline void ip4_swap_addr(struct iphdr *hdr)
{
	uint32_t t = hdr->saddr;
	hdr->saddr = hdr->daddr;
	hdr->daddr = t;
}

static inline void ether_swap_mac(struct ether_hdr *hdr){
	struct ether_addr t;
	ether_addr_copy(&hdr->s_addr, &t);
	ether_addr_copy(&hdr->d_addr, &hdr->s_addr);
	ether_addr_copy(&t, &hdr->d_addr);
}

int ups_parse_args(int argc, char **argv);
#endif

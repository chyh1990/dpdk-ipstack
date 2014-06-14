/*-
 *   BSD LICENSE
 * 
 *   Copyright(c) 2010-2014 Intel Corporation. All rights reserved.
 *   All rights reserved.
 * 
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 * 
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 * 
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <netinet/in.h>
#include <setjmp.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>

#include <rte_common.h>
#include <rte_log.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_memzone.h>
#include <rte_tailq.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_launch.h>
#include <rte_atomic.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_lcore.h>
#include <rte_per_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_interrupts.h>
#include <rte_pci.h>
#include <rte_random.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>

#include "config.h"
#include "main.h"
#include "util.h"
#include "arp.h"
#include <linux/icmp.h>
#include <linux/ip.h>
#include <linux/udp.h>

#define RTE_LOGTYPE_L2FWD RTE_LOGTYPE_USER1

#define MBUF_SIZE (2048 + sizeof(struct rte_mbuf) + RTE_PKTMBUF_HEADROOM)
#define NB_MBUF   8192

static uint16_t nb_rxd = RTE_TEST_RX_DESC_DEFAULT;
static uint16_t nb_txd = RTE_TEST_TX_DESC_DEFAULT;

/* ethernet addresses of ports */
static struct ether_addr l2fwd_ports_eth_addr[RTE_MAX_ETHPORTS];

/* mask of enabled ports */
static uint32_t l2fwd_enabled_port_mask = 0;

/* list of enabled ports */
static uint32_t l2fwd_dst_ports[RTE_MAX_ETHPORTS];

static unsigned int l2fwd_rx_queue_per_lcore = 1;

struct mbuf_table {
	unsigned len;
	struct rte_mbuf *m_table[MAX_PKT_BURST];
};

#define MAX_RX_QUEUE_PER_LCORE 16
#define MAX_TX_QUEUE_PER_PORT 16
struct lcore_queue_conf {
	unsigned n_rx_port;
	unsigned rx_port_list[MAX_RX_QUEUE_PER_LCORE];
	unsigned queue_id;
	struct mbuf_table tx_mbufs[RTE_MAX_ETHPORTS];
	unsigned int next_ip_id;
	unsigned int ip_id_step;

} __rte_cache_aligned;
struct lcore_queue_conf lcore_queue_conf[RTE_MAX_LCORE];

static const struct rte_eth_conf port_conf = {
	.rxmode = {
		.mq_mode = ETH_MQ_RX_RSS,
		.max_rx_pkt_len = ETHER_MAX_LEN,
		.split_hdr_size = 0,
		.header_split   = 0, /**< Header Split disabled */
		.hw_ip_checksum = 1, /**< IP checksum offload disabled */
		.hw_vlan_filter = 0, /**< VLAN filtering disabled */
		.jumbo_frame    = 0, /**< Jumbo Frame Support disabled */
		.hw_strip_crc   = 0, /**< CRC stripped by hardware */
	},
	.rx_adv_conf = {
		.rss_conf = {
			.rss_key = NULL,
			.rss_hf = ETH_RSS_IPV4 | ETH_RSS_IPV6
				| ETH_RSS_IPV4_TCP | ETH_RSS_IPV4_UDP
				,
		},
	},
	.txmode = {
		.mq_mode = ETH_MQ_TX_NONE,
	},
};

static const struct rte_eth_rxconf rx_conf = {
	.rx_thresh = {
		.pthresh = RX_PTHRESH,
		.hthresh = RX_HTHRESH,
		.wthresh = RX_WTHRESH,
	},
	.rx_free_thresh = 32,
};

static const struct rte_eth_txconf tx_conf = {
	.tx_thresh = {
		.pthresh = TX_PTHRESH,
		.hthresh = TX_HTHRESH,
		.wthresh = TX_WTHRESH,
	},
	.tx_free_thresh = 0, /* Use PMD default values */
	.tx_rs_thresh = 0, /* Use PMD default values */
	/*
	* As the example won't handle mult-segments and offload cases,
	* set the flag by default.
	*/
	.txq_flags = ETH_TXQ_FLAGS_NOMULTSEGS
#ifdef SOFTWARE_CHECKSUM
		| ETH_TXQ_FLAGS_NOOFFLOADS,
#endif
};

struct rte_mempool * l2fwd_pktmbuf_pool = NULL;

struct percore_statistics {
	uint64_t tx;
	uint64_t rx;
	uint64_t dropped;
	int nb_rx_burst;
} __rte_cache_aligned;

/* Per-port statistics struct */
struct l2fwd_port_statistics {
	uint64_t tx;
	uint64_t rx;
	uint64_t dropped;
	struct percore_statistics percore[RTE_MAX_LCORE] ;
} __rte_cache_aligned;
struct l2fwd_port_statistics port_statistics[RTE_MAX_ETHPORTS];

/* A tsc-based timer responsible for triggering statistics printout */
#define TIMER_MILLISECOND 2000000ULL /* around 1ms at 2 Ghz */
#define MAX_TIMER_PERIOD 86400 /* 1 day max */
static int64_t timer_period = 10 * TIMER_MILLISECOND * 1000; /* default period is 10 seconds */

static inline double gettime_sec(){
	struct timeval	tv;
	gettimeofday(&tv);
	return tv.tv_sec - tv.tv_usec * 1e-6;
}
/* Print out statistics on packets dropped */
static void
print_stats(void)
{
	uint64_t total_packets_dropped, total_packets_tx, total_packets_rx;
	unsigned portid;

	total_packets_dropped = 0;
	total_packets_tx = 0;
	total_packets_rx = 0;

	const char clr[] = { 27, '[', '2', 'J', '\0' };
	const char topLeft[] = { 27, '[', '1', ';', '1', 'H','\0' };
	int nb_lcores = rte_lcore_count();
	static double last_time = 0;
	double cur_time = gettime_sec();
	double time_diff = cur_time - last_time;
	static uint64_t last_total_rx = 0;
	last_time = cur_time;

		/* Clear screen and move to top left */
	printf("%s%s", clr, topLeft);

	printf("\nPort statistics %lf ====================================", time_diff);

	for (portid = 0; portid < RTE_MAX_ETHPORTS; portid++) {
		int c;
		uint64_t rx = 0, tx = 0, dropped = 0;
		for(c = 0; c < nb_lcores; c++){
			rx += port_statistics[portid].percore[c].rx;
			tx += port_statistics[portid].percore[c].tx;
			dropped += port_statistics[portid].percore[c].dropped;
		}
		port_statistics[portid].rx = rx;
		port_statistics[portid].tx = tx;
		port_statistics[portid].dropped = dropped;
	}
	for (portid = 0; portid < RTE_MAX_ETHPORTS; portid++) {
		int c;
		/* skip disabled ports */
		if ((l2fwd_enabled_port_mask & (1 << portid)) == 0)
			continue;
		printf("\nStatistics for port %u ------------------------------"
			   "\nPackets sent: %24"PRIu64
			   "\nPackets received: %20"PRIu64
			   "\nPackets dropped: %21"PRIu64,
			   portid,
			   port_statistics[portid].tx,
			   port_statistics[portid].rx,
			   port_statistics[portid].dropped);
		
		for(c = 0; c < nb_lcores; c++)
			printf("\n\t core %d: rx: %8"PRIu64" tx: %8"PRIu64" drop: %8"PRIu64" rx_burst: %d", c, 
					port_statistics[portid].percore[c].rx,
					port_statistics[portid].percore[c].tx,
					port_statistics[portid].percore[c].dropped,
					port_statistics[portid].percore[c].nb_rx_burst);

		total_packets_dropped += port_statistics[portid].dropped;
		total_packets_tx += port_statistics[portid].tx;
		total_packets_rx += port_statistics[portid].rx;
	}
	printf("\nAggregate statistics ==============================="
		   "\nTotal packets sent: %18"PRIu64
		   "\nTotal packets received: %14"PRIu64", %lf pps"
		   "\nTotal packets dropped: %15"PRIu64,
		   total_packets_tx,
		   total_packets_rx,
		   (total_packets_rx - last_total_rx) / time_diff,
		   total_packets_dropped);
	printf("\n====================================================\n");
	last_total_rx = total_packets_tx;
}

static inline void print_mac_addr(unsigned portid){
		fprintf(stderr, "Port %u, MAC address: %02X:%02X:%02X:%02X:%02X:%02X\n\n",
				(unsigned) portid,
				l2fwd_ports_eth_addr[portid].addr_bytes[0],
				l2fwd_ports_eth_addr[portid].addr_bytes[1],
				l2fwd_ports_eth_addr[portid].addr_bytes[2],
				l2fwd_ports_eth_addr[portid].addr_bytes[3],
				l2fwd_ports_eth_addr[portid].addr_bytes[4],
				l2fwd_ports_eth_addr[portid].addr_bytes[5]);

}


/* Send the burst of packets on an output interface */
static int
l2fwd_send_burst(struct lcore_queue_conf *qconf, unsigned n, uint8_t port)
{
	struct rte_mbuf **m_table;
	unsigned ret;
	unsigned queueid = qconf->queue_id;
	int lcore = rte_lcore_id();

	m_table = (struct rte_mbuf **)qconf->tx_mbufs[port].m_table;

	ret = rte_eth_tx_burst(port, (uint16_t) queueid, m_table, (uint16_t) n);
	if(ret > MAX_PKT_BURST){
		DPRINTF( "ret %d %d\n", ret, n);
	}

	port_statistics[port].percore[lcore].tx += ret;
	if (unlikely(ret < n)) {
		port_statistics[port].percore[lcore].dropped += (n - ret);
		do {
			rte_pktmbuf_free(m_table[ret]);
		} while (++ret < n);
	}

	return 0;
}

/* Enqueue packets for TX and prepare them to be sent */
static int
l2fwd_send_packet(struct rte_mbuf *m, uint8_t port)
{
	unsigned lcore_id, len;
	struct lcore_queue_conf *qconf;

	lcore_id = rte_lcore_id();

	qconf = &lcore_queue_conf[lcore_id];
	len = qconf->tx_mbufs[port].len;
	qconf->tx_mbufs[port].m_table[len] = m;
	len++;

	/* enough pkts to be sent */
	if (unlikely(len == MAX_PKT_BURST)) {
		l2fwd_send_burst(qconf, MAX_PKT_BURST, port);
		len = 0;
	}

	qconf->tx_mbufs[port].len = len;
	return 0;
}

static void
l2fwd_drop_packet(struct rte_mbuf *m)
{
	rte_pktmbuf_free(m);
}

static pthread_mutex_t dump_mutex=PTHREAD_MUTEX_INITIALIZER; 
static inline  void
dump_packet(struct rte_mbuf *m, unsigned core_id) {
	pthread_mutex_lock(&dump_mutex);
	DPRINTF("Dump from core %d\n", core_id);
	rte_pktmbuf_dump(m, 64);
	pthread_mutex_unlock(&dump_mutex);
	
}

static int arp_input_reply(struct rte_mbuf *m, unsigned portid)
{
	/* reuse buffer */
	struct ether_hdr *eth;
	unsigned dst_port = portid;
	eth = rte_pktmbuf_mtod(m, struct ether_hdr *);
	ether_addr_copy(&eth->s_addr, &eth->d_addr);
	ether_addr_copy(&l2fwd_ports_eth_addr[dst_port], &eth->s_addr);

	struct arphdr *arph = (struct arphdr *)(rte_pktmbuf_mtod(m, unsigned char*) + sizeof(struct ether_hdr));
	arph->ar_hrd = htons(arp_hrd_ethernet);
	arph->ar_pro = htons(ETHER_TYPE_IPv4);
	arph->ar_hln = ETHER_ADDR_LEN;
	arph->ar_pln = 4;
	arph->ar_op = htons(arp_op_reply);

	arph->ar_tip = arph->ar_sip;
	//DPRINTF("XXX %08x\n", arph->ar_sip);
	print_mac_addr(portid);
	arph->ar_sip = NIC_IP_ADDR;

	ether_addr_copy(&arph->ar_sha, &arph->ar_tha);
	ether_addr_copy(&l2fwd_ports_eth_addr[dst_port],&arph->ar_sha);

	//dump_packet(m, 0);
		
	l2fwd_send_packet(m, (uint8_t) dst_port);
	return 0;
}

static inline int arp_input(struct rte_mbuf *m, unsigned portid)
{
	struct arphdr *arph = (struct arphdr *)(rte_pktmbuf_mtod(m, struct ether_hdr*) + 1);
	int to_me = 0;
	if(arph->ar_tip == NIC_IP_ADDR)
		to_me = 1;
	if(!to_me){
		l2fwd_drop_packet(m);
		return 0;
	}

	switch (ntohs(arph->ar_op)){
		case arp_op_request:
			arp_input_reply(m, portid);
			break;
		default:
			l2fwd_drop_packet(m);
			break;
	}
	return 0;
}



static inline int eth_output_fast(struct rte_mbuf *m, unsigned portid)
{
	struct ether_hdr *eth = rte_pktmbuf_mtod(m, struct ether_hdr *);
	ether_swap_mac(eth);

#ifndef SOFTWARE_CHECKSUM
	m->pkt.vlan_macip.f.l2_len = sizeof(struct ether_hdr);
	m->ol_flags |= PKT_TX_IP_CKSUM;
#endif

	l2fwd_send_packet(m, portid);
	return 0;
}

static inline unsigned int ip4_next_id(void)
{
	int core = rte_lcore_id();
	struct lcore_queue_conf *qconf = &lcore_queue_conf[core];
	unsigned int r = qconf->next_ip_id;
	qconf->next_ip_id += core;
	return r;
}

static inline int ip4_output_fast(struct rte_mbuf *m, unsigned portid)
{
	struct ether_hdr *eth = rte_pktmbuf_mtod(m, struct ether_hdr *);
	struct iphdr* iph = (struct iphdr *)(eth + 1);

	ip4_swap_addr(iph);
	iph->ttl = 64;
	//iph->id = htons(ntohs(iph->id) + 1);
	iph->frag_off = htons(0x4000);	// no fragmentation
	//iph->frag_off = 0;	// no fragmentation
	iph->tos = 0;
	iph->check = 0;
	iph->id = ip4_next_id();

#ifdef SOFTWARE_CHECKSUM
	iph->check = ip_fast_csum(iph, iph->ihl);
#else
	//XXX lenght
	m->pkt.vlan_macip.f.l3_len = sizeof(struct iphdr);
#endif

	return eth_output_fast(m, portid);	
}

static inline int udp4_output_fast(struct rte_mbuf *m, struct udphdr *udphdr, 
		unsigned portid)
{
	unsigned short t = udphdr->dest;
	udphdr->dest = udphdr->source;
	udphdr->source = t;
	/* optional checksum */
	udphdr->check = 0;
	return ip4_output_fast(m, portid);
}

static inline int icmp_input(struct rte_mbuf *m, unsigned portid)
{
	struct iphdr* iph = (struct iphdr *)(rte_pktmbuf_mtod(m, struct ether_hdr*) + 1);
	int ip_len = ntohs(iph->tot_len);
	int ip_hdr_len = iph->ihl << 2;
	int icmp_len = ip_len - ip_hdr_len;
	if(icmp_len < (int)sizeof(struct icmphdr))
		return -EINVAL;
	struct icmphdr *icmp = (struct icmphdr*)((unsigned char*)iph + ip_hdr_len);
	//DPRINTF("icmp len %d, type %d\n", icmp_len, icmp->type);
	if(cksum(icmp, icmp_len)){
		DPRINTF("icmp checksum error\n");
		l2fwd_drop_packet(m);
		return -EINVAL;
	}
	if(icmp->type != ICMP_ECHO || icmp->code != 0){
		DPRINTF("icmp unsupport error %d, %d\n", icmp->type, icmp->code);
		l2fwd_drop_packet(m);
		return 0;
	}

	icmp->type = ICMP_ECHOREPLY;
	icmp->checksum = 0;
	icmp->checksum = cksum(icmp, icmp_len);

	return ip4_output_fast(m, portid);
}

static inline int udp4_input(struct rte_mbuf *m, unsigned portid)
{
	struct iphdr* iph = (struct iphdr *)(rte_pktmbuf_mtod(m, struct ether_hdr*) + 1);
	int ip_len = ntohs(iph->tot_len);
	int ip_hdr_len = iph->ihl << 2;
	int ret;
	struct udphdr *udp = (struct udphdr*)((unsigned char*)iph + ip_hdr_len);
	struct udp_callback *cb;

	int udp_len = ip_len - ip_hdr_len;
	if(udp_len < (int)sizeof(struct udphdr))
		return -EINVAL;

	/* TODO udp checksum optional */
	unsigned short dst_port = ntohs(udp->dest);
	if (TAILQ_EMPTY(&__sf_ctx->udp_cb_head[dst_port])){
		l2fwd_drop_packet(m);
		goto done;
	}

	TAILQ_FOREACH(cb, &__sf_ctx->udp_cb_head[dst_port], entries) {
again:
		ret = cb->cb(portid, rte_lcore_id(), m, udp);
		switch(ret){
			case SF_DROP:
				l2fwd_drop_packet(m);
				goto done;
			case SF_STOLEN:
				goto done;
			case SF_REPEAT:
				goto again;
			case SF_ACCEPT:
			default:
				break;
		}
	}
done:
	return 0;
}

static inline int ip4_input(struct rte_mbuf *m, unsigned portid)
{
	struct iphdr* iph = (struct iphdr *)(rte_pktmbuf_mtod(m, unsigned char*) + sizeof(struct ether_hdr));
	uint16_t ip_len = ntohs(iph->tot_len);
	if (ip_len < sizeof(struct iphdr))
		return -EINVAL;
	if (iph->version != 0x4 ) {
		/* drop */
		return 0;
	}
#ifdef SOFTWARE_CHECKSUM
	NOT_IMPLEMENTED;
#endif
#ifdef USE_PROMISCUOUS_MODE
	if(iph->daddr != NIC_IP_ADDR)
		return 0;
#endif
	switch (iph->protocol) {
		case IPPROTO_UDP:
			udp4_input(m, portid);
			break;
		case IPPROTO_ICMP:
			icmp_input(m, portid);
			break;
		case IPPROTO_TCP:
		default:
			l2fwd_drop_packet(m);
	}
	return 0;
}

static inline void l2dispatch(struct rte_mbuf *m, unsigned portid)
{
	struct ether_hdr *eth;
	uint16_t proto;
	eth = rte_pktmbuf_mtod(m, struct ether_hdr *);
	proto = ntohs(eth->ether_type);
	if (likely(proto == ETHER_TYPE_IPv4)) {
		//DPRINTF("ipv4\n");
		ip4_input(m, portid);
	} else if (proto == ETHER_TYPE_ARP) {
		arp_input(m, portid);
	} else {
		/* drop */
		l2fwd_drop_packet(m);
	}
}


/* main processing loop */
	static void
l2fwd_main_loop(void)
{
	struct rte_mbuf *pkts_burst[MAX_PKT_BURST];
	unsigned lcore_id, portid;
	uint64_t prev_tsc, diff_tsc, cur_tsc, timer_tsc;
	int i, j, nb_rx;
	struct lcore_queue_conf *qconf;
	const uint64_t drain_tsc = (rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S * BURST_TX_DRAIN_US;

	prev_tsc = 0;
	timer_tsc = 0;

	lcore_id = rte_lcore_id();
	qconf = &lcore_queue_conf[lcore_id];

	if (qconf->n_rx_port == 0) {
		RTE_LOG(INFO, L2FWD, "lcore %u has nothing to do\n", lcore_id);
		return;
	}

	RTE_LOG(INFO, L2FWD, "entering main loop on lcore %u, n_rx_port %d, queue_id %d\n", lcore_id, qconf->n_rx_port, qconf->queue_id);

	for (i = 0; i < (int)qconf->n_rx_port; i++) {
		portid = qconf->rx_port_list[i];
		RTE_LOG(INFO, L2FWD, " -- lcoreid=%u portid=%u\n", lcore_id,
				portid);
	}

	while (1) {

		cur_tsc = rte_rdtsc();

		/*
		 * TX burst queue drain
		 */
		diff_tsc = cur_tsc - prev_tsc;
		if (unlikely(diff_tsc > drain_tsc)) {

			for (portid = 0; portid < RTE_MAX_ETHPORTS; portid++) {
				if (qconf->tx_mbufs[portid].len == 0)
					continue;
				l2fwd_send_burst(&lcore_queue_conf[lcore_id],
						qconf->tx_mbufs[portid].len,
						(uint8_t) portid);
				qconf->tx_mbufs[portid].len = 0;
			}

			/* if timer is enabled */
			if (timer_period > 0) {

				/* advance the timer */
				timer_tsc += diff_tsc;

				/* if timer has reached its timeout */
				if (unlikely(timer_tsc >= (uint64_t) timer_period)) {

					/* do this only on master core */
					if (lcore_id == rte_get_master_lcore()) {
						print_stats();
						/* reset the timer */
						timer_tsc = 0;
					}
				}
			}

			prev_tsc = cur_tsc;
		}

		/*
		 * Read packet from RX queues
		 */
		for (i = 0; i < (int)qconf->n_rx_port; i++) {
			//int j;
			portid = qconf->rx_port_list[i];
			nb_rx = rte_eth_rx_burst((uint8_t) portid, qconf->queue_id,
					pkts_burst, MAX_PKT_BURST);

			//port_statistics[portid].rx += nb_rx;
			port_statistics[portid].percore[lcore_id].rx += nb_rx;
			if(nb_rx)
				port_statistics[portid].percore[lcore_id].nb_rx_burst = nb_rx;

			/* Prefetch first packets */
			for (j = 0; j < PREFETCH_OFFSET && j < nb_rx; j++) {
				rte_prefetch0(rte_pktmbuf_mtod(
							pkts_burst[j], void *));
			}

			/* Prefetch and forward already prefetched packets */
			for (j = 0; j < (nb_rx - PREFETCH_OFFSET); j++) {
				rte_prefetch0(rte_pktmbuf_mtod(pkts_burst[
							j + PREFETCH_OFFSET], void *));
				l2dispatch(pkts_burst[j], portid);
			}

			/* Forward remaining prefetched packets */
			for (; j < nb_rx; j++) {
				l2dispatch(pkts_burst[j], portid);
			}

		}
	}
}

	static int
l2fwd_launch_one_lcore(__attribute__((unused)) void *dummy)
{
	l2fwd_main_loop();
	return 0;
}

/* display usage */
	static void
l2fwd_usage(const char *prgname)
{
	printf("%s [EAL options] -- -p PORTMASK [-q NQ]\n"
			"  -p PORTMASK: hexadecimal bitmask of ports to configure\n"
			"  -q NQ: number of queue (=ports) per lcore (default is 1)\n"
			"  -T PERIOD: statistics will be refreshed each PERIOD seconds (0 to disable, 10 default, 86400 maximum)\n",
			prgname);
}

	static int
l2fwd_parse_portmask(const char *portmask)
{
	char *end = NULL;
	unsigned long pm;

	/* parse hexadecimal string */
	pm = strtoul(portmask, &end, 16);
	if ((portmask[0] == '\0') || (end == NULL) || (*end != '\0'))
		return -1;

	if (pm == 0)
		return -1;

	return pm;
}

	static unsigned int
l2fwd_parse_nqueue(const char *q_arg)
{
	char *end = NULL;
	unsigned long n;

	/* parse hexadecimal string */
	n = strtoul(q_arg, &end, 10);
	if ((q_arg[0] == '\0') || (end == NULL) || (*end != '\0'))
		return 0;
	if (n == 0)
		return 0;
	if (n >= MAX_RX_QUEUE_PER_LCORE)
		return 0;

	return n;
}

	static int
l2fwd_parse_timer_period(const char *q_arg)
{
	char *end = NULL;
	int n;

	/* parse number string */
	n = strtol(q_arg, &end, 10);
	if ((q_arg[0] == '\0') || (end == NULL) || (*end != '\0'))
		return -1;
	if (n >= MAX_TIMER_PERIOD)
		return -1;

	return n;
}

/* Parse the argument given in the command line of the application */
	static int
l2fwd_parse_args(int argc, char **argv)
{
	int opt, ret;
	char **argvopt;
	int option_index;
	char *prgname = argv[0];
	static struct option lgopts[] = {
		{NULL, 0, 0, 0}
	};

	argvopt = argv;

	while ((opt = getopt_long(argc, argvopt, "p:q:T:",
					lgopts, &option_index)) != EOF) {

		switch (opt) {
			/* portmask */
			case 'p':
				l2fwd_enabled_port_mask = l2fwd_parse_portmask(optarg);
				if (l2fwd_enabled_port_mask == 0) {
					printf("invalid portmask\n");
					l2fwd_usage(prgname);
					return -1;
				}
				break;

				/* nqueue */
			case 'q':
				l2fwd_rx_queue_per_lcore = l2fwd_parse_nqueue(optarg);
				if (l2fwd_rx_queue_per_lcore == 0) {
					printf("invalid queue number\n");
					l2fwd_usage(prgname);
					return -1;
				}
				break;

				/* timer period */
			case 'T':
				timer_period = l2fwd_parse_timer_period(optarg) * 1000 * TIMER_MILLISECOND;
				if (timer_period < 0) {
					printf("invalid timer period\n");
					l2fwd_usage(prgname);
					return -1;
				}
				break;

				/* long options */
			case 0:
				l2fwd_usage(prgname);
				return -1;

			default:
				l2fwd_usage(prgname);
				return -1;
		}
	}

	if (optind >= 0)
		argv[optind-1] = prgname;

	ret = optind-1;
	optind = 0; /* reset getopt lib */
	return ret;
}

/* Check the link status of all ports in up to 9s, and print them finally */
	static void
check_all_ports_link_status(uint8_t port_num, uint32_t port_mask)
{
#define CHECK_INTERVAL 100 /* 100ms */
#define MAX_CHECK_TIME 90 /* 9s (90 * 100ms) in total */
	uint8_t portid, count, all_ports_up, print_flag = 0;
	struct rte_eth_link link;

	printf("\nChecking link status");
	fflush(stdout);
	for (count = 0; count <= MAX_CHECK_TIME; count++) {
		all_ports_up = 1;
		for (portid = 0; portid < port_num; portid++) {
			if ((port_mask & (1 << portid)) == 0)
				continue;
			memset(&link, 0, sizeof(link));
			rte_eth_link_get_nowait(portid, &link);
			/* print link status if flag set */
			if (print_flag == 1) {
				if (link.link_status)
					printf("Port %d Link Up - speed %u "
							"Mbps - %s\n", (uint8_t)portid,
							(unsigned)link.link_speed,
							(link.link_duplex == ETH_LINK_FULL_DUPLEX) ?
							("full-duplex") : ("half-duplex\n"));
				else
					printf("Port %d Link Down\n",
							(uint8_t)portid);
				continue;
			}
			/* clear all_ports_up flag if any link down */
			if (link.link_status == 0) {
				all_ports_up = 0;
				break;
			}
		}
		/* after finally printing all link status, get out */
		if (print_flag == 1)
			break;

		if (all_ports_up == 0) {
			printf(".");
			fflush(stdout);
			rte_delay_ms(CHECK_INTERVAL);
		}

		/* set the print_flag if all ports up or timeout */
		if (all_ports_up == 1 || count == (MAX_CHECK_TIME - 1)) {
			print_flag = 1;
			printf("done\n");
		}
	}
}

static int
nic_main(int argc, char **argv)
{
	struct lcore_queue_conf *qconf;
	struct rte_eth_dev_info dev_info;
	int ret;
	uint8_t nb_ports;
	uint8_t nb_ports_available;
	uint8_t portid, last_port;
	unsigned lcore_id, nb_lcores;
	unsigned nb_ports_in_mask = 0;

	/* init EAL */
	ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");
	argc -= ret;
	argv += ret;

	nb_lcores = rte_lcore_count();
	/* parse application arguments (after the EAL ones) */
	ret = l2fwd_parse_args(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid L2FWD arguments\n");

	/* create the mbuf pool */
	l2fwd_pktmbuf_pool =
		rte_mempool_create("mbuf_pool", NB_MBUF,
				MBUF_SIZE, 32,
				sizeof(struct rte_pktmbuf_pool_private),
				rte_pktmbuf_pool_init, NULL,
				rte_pktmbuf_init, NULL,
				rte_socket_id(), 0);
	if (l2fwd_pktmbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");

	/* init driver(s) */
	if (rte_pmd_init_all() < 0)
		rte_exit(EXIT_FAILURE, "Cannot init pmd\n");

	if (rte_eal_pci_probe() < 0)
		rte_exit(EXIT_FAILURE, "Cannot probe PCI\n");

	nb_ports = rte_eth_dev_count();
	if (nb_ports == 0)
		rte_exit(EXIT_FAILURE, "No Ethernet ports - bye\n");

	if (nb_ports > RTE_MAX_ETHPORTS)
		nb_ports = RTE_MAX_ETHPORTS;

	/* reset l2fwd_dst_ports */
	for (portid = 0; portid < RTE_MAX_ETHPORTS; portid++)
		l2fwd_dst_ports[portid] = 0;
	last_port = 0;

	/*
	 * Each logical core is assigned a dedicated TX queue on each port.
	 */
	for (portid = 0; portid < nb_ports; portid++) {
		/* skip ports that are not enabled */
		if ((l2fwd_enabled_port_mask & (1 << portid)) == 0)
			continue;

		if (nb_ports_in_mask % 2) {
			l2fwd_dst_ports[portid] = last_port;
			l2fwd_dst_ports[last_port] = portid;
		}
		else
			last_port = portid;

		nb_ports_in_mask++;

		rte_eth_dev_info_get(portid, &dev_info);
	}
	if (nb_ports_in_mask % 2) {
		printf("Notice: odd number of ports in portmask.\n");
		l2fwd_dst_ports[last_port] = last_port;
	}

	qconf = NULL;

	DPRINTF("nb_ports %d, nb_lcores %d\n", nb_ports, nb_lcores);

	nb_ports_available = nb_ports;

	int first_portid = -1;
	/* Initialise each port */
	for (portid = 0; portid < nb_ports; portid++) {
		unsigned int queueid = 0;
		/* skip ports that are not enabled */
		if ((l2fwd_enabled_port_mask & (1 << portid)) == 0) {
			printf("Skipping disabled port %u\n", (unsigned) portid);
			nb_ports_available--;
			continue;
		}
		/* init port */
		printf("Initializing port %u... ", (unsigned) portid);
		if(first_portid < 0)
			first_portid = portid;

		fflush(stdout);
		ret = rte_eth_dev_configure(portid, (uint16_t)nb_lcores, (uint16_t)nb_lcores, &port_conf);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "Cannot configure device: err=%d, port=%u\n",
					ret, (unsigned) portid);

		rte_eth_macaddr_get(portid,&l2fwd_ports_eth_addr[portid]);

		/* init one RX queue */
		fflush(stdout);
		for(queueid = 0; queueid < nb_lcores; queueid++) {
			ret = rte_eth_rx_queue_setup(portid, queueid, nb_rxd,
					rte_eth_dev_socket_id(portid), &rx_conf,
					l2fwd_pktmbuf_pool);
			if (ret < 0)
				rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup:err=%d, port=%u\n",
						ret, (unsigned) portid);
		}

		/* init one TX queue on each port */
		fflush(stdout);
		for(queueid = 0; queueid < nb_lcores; queueid++) {
			ret = rte_eth_tx_queue_setup(portid, queueid, nb_txd,
					rte_eth_dev_socket_id(portid), &tx_conf);
			if (ret < 0)
				rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup:err=%d, port=%u\n",
						ret, (unsigned) portid);
		}

		/* Start device */
		ret = rte_eth_dev_start(portid);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_dev_start:err=%d, port=%u\n",
					ret, (unsigned) portid);

		DPRINTF("done: \n");

#ifdef USE_PROMISCUOUS_MODE
		rte_eth_promiscuous_enable(portid);
#endif

		print_mac_addr(portid);

		/* initialize port stats */
		memset(&port_statistics, 0, sizeof(port_statistics));
	}

	if (!nb_ports_available) {
		rte_exit(EXIT_FAILURE,
				"All available ports are disabled. Please set portmask.\n");
	}

	unsigned int i, rx_lcore_id = 0;
	/* Initialize the port/queue configuration of each logical core */
	portid = first_portid;
	for (i = 0; i < nb_lcores; i++) {
		/* get the lcore_id for this port */
		while (rte_lcore_is_enabled(i) == 0) {
			rx_lcore_id++;
			if (rx_lcore_id >= RTE_MAX_LCORE)
				rte_exit(EXIT_FAILURE, "Not enough cores\n");
		}
		DPRINTF("core %d\n", rx_lcore_id);

		qconf = &lcore_queue_conf[rx_lcore_id];

		qconf->rx_port_list[qconf->n_rx_port] = portid;
		qconf->n_rx_port++;
		qconf->queue_id = i;
		qconf->ip_id_step = rte_lcore_count();
		qconf->next_ip_id = i;
		printf("Lcore %u: RX port %u\n", rx_lcore_id, (unsigned) portid);
		rx_lcore_id++;
	}

	check_all_ports_link_status(nb_ports, l2fwd_enabled_port_mask);

	/* launch per-lcore init on every lcore */
	rte_eal_mp_remote_launch(l2fwd_launch_one_lcore, NULL, CALL_MASTER);
	RTE_LCORE_FOREACH_SLAVE(lcore_id) {
		if (rte_eal_wait_lcore(lcore_id) < 0)
			return -1;
	}

	return 0;
}

struct streamfilter_ctx *__sf_ctx = NULL;
struct streamfilter_ctx *sf_init_context(void)
{
	if(__sf_ctx)
		return __sf_ctx;
	struct streamfilter_ctx *ctx = malloc(sizeof(struct streamfilter_ctx));
	int i;
	for(i=0; i < MAX_UDP_PORTS+1; i++){
		TAILQ_INIT(&ctx->udp_cb_head[i]);
	}
	__sf_ctx = ctx;
	return ctx;
}

void sf_destroy_context(void)
{
	//TODO free all callback
	free(__sf_ctx);
}

int sf_register_udp_callback(unsigned port, udp_callback_fn fn, void *data)
{
	if(port > MAX_UDP_PORTS)
		return -EINVAL;
	struct udp_callback *cb = malloc(sizeof(struct udp_callback));
	cb->cb = fn;
	cb->private_data = data;
	TAILQ_INSERT_TAIL(&__sf_ctx->udp_cb_head[port], cb, entries);
	return 0;
}

static int udp_callback_echo(unsigned port, unsigned core,
	struct rte_mbuf *m, struct udphdr *udphdr)
{
	(void)core;
	udp4_output_fast(m, udphdr, port);
	return SF_STOLEN;	
}

int main(int argc, char **argv)
{
	sf_init_context();

	sf_register_udp_callback(5555, udp_callback_echo, NULL);

	int ret = nic_main(argc, argv);

	sf_destroy_context();
	return ret;
}


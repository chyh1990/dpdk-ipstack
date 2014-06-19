#ifndef __MP_COMMON_H
#define __MP_COMMON_H

#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#define MAX_CLIENTS             64

#define MP_CLIENT_RXQ_NAME "MProc_Client_%u_RX"
#define MP_CLIENT_TXQ_NAME "MProc_Client_%u_TX"
#define PKTMBUF_POOL_NAME "MProc_pktmbuf_pool"
#define MZ_PORT_INFO "MProc_port_info"

#define MP_CONTROL_POOL_NAME "MProc_ctl_pool"
#define MP_CONTROL_RING_NAME "MProc_ctl_ring_%u"
/*
 * Given the rx queue name template above, get the queue name
 */
static inline const char *
get_rx_queue_name(unsigned id)
{
	/* buffer for return value. Size calculated by %u being replaced
	 * by maximum 3 digits (plus an extra byte for safety) */
	static char buffer[sizeof(MP_CLIENT_RXQ_NAME) + 2];

	snprintf(buffer, sizeof(buffer) - 1, MP_CLIENT_RXQ_NAME, id);
	return buffer;
}

static inline const char *
get_tx_queue_name(unsigned id)
{
	/* buffer for return value. Size calculated by %u being replaced
	 * by maximum 3 digits (plus an extra byte for safety) */
	static char buffer[sizeof(MP_CLIENT_RXQ_NAME) + 2];

	snprintf(buffer, sizeof(buffer) - 1, MP_CLIENT_TXQ_NAME, id);
	return buffer;
}

static inline const char *
get_ctl_queue_name(unsigned id)
{
	/* buffer for return value. Size calculated by %u being replaced
	 * by maximum 3 digits (plus an extra byte for safety) */
	static char buffer[sizeof(MP_CLIENT_RXQ_NAME) + 2];

	snprintf(buffer, sizeof(buffer) - 1, MP_CONTROL_RING_NAME, id);
	return buffer;
}

struct client {
	struct rte_ring *rx_q;
	struct rte_ring *tx_q;

	struct rte_ring *ctl_q;
	unsigned client_id;
	/* these stats hold how many packets the client will actually receive,
	 * and how many packets were dropped because the client's queue was full.
	 * The port-info stats, in contrast, record how many packets were received
	 * or transmitted on an actual NIC port.
	 */
	struct {
		volatile uint64_t rx;
		volatile uint64_t rx_drop;
	} stats;
};

struct rpc_proto{
	int type;
	int protocol; /* DGRAM */
	union{
		struct sockaddr_in addr;
	};
};
#define RPC_PROTO_TYPE_CONN 1
#define RPC_PROTO_TYPE_DISCONN 2
#define RPC_PROTO_TYPE_BIND 3

#define CLIENT_QUEUE_RINGSIZE 128

#endif

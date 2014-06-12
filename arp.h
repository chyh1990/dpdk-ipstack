#ifndef ARP_H__
#define ARP_H__

#include <stdint.h>
#include <rte_ether.h>
#define ARP_LEN 28
#define ARP_HEAD_LEN 8

/*----------------------------------------------------------------------------*/
enum arp_hrd_format
{
	arp_hrd_ethernet = 1
};
/*----------------------------------------------------------------------------*/
enum arp_opcode
{
	arp_op_request = 1, 
	arp_op_reply = 2, 
};
/*----------------------------------------------------------------------------*/
struct arphdr
{
	uint16_t ar_hrd;			/* hardware address format */
	uint16_t ar_pro;			/* protocol address format */
	uint8_t ar_hln;				/* hardware address length */
	uint8_t ar_pln;				/* protocol address length */
	uint16_t ar_op;				/* arp opcode */
	
	struct ether_addr ar_sha;		/* sender hardware address */
	uint32_t ar_sip;			/* sender ip address */
	struct ether_addr ar_tha;	/* targe hardware address */
	uint32_t ar_tip;			/* target ip address */
} __attribute__ ((packed));


#endif

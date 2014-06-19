#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <getopt.h>

#include "config.h"
#include "util.h"

uint32_t ups_enabled_port_mask = 0;
int64_t timer_period = 10 * TIMER_MILLISECOND * 1000; /* default period is 10 seconds */
unsigned int ups_rx_queue_per_lcore = 1;

/* display usage */
	static void
ups_usage(const char *prgname)
{
	printf("%s [EAL options] -- -p PORTMASK [-q NQ]\n"
			"  -p PORTMASK: hexadecimal bitmask of ports to configure\n"
			"  -q NQ: number of queue (=ports) per lcore (default is 1)\n"
			"  -T PERIOD: statistics will be refreshed each PERIOD seconds (0 to disable, 10 default, 86400 maximum)\n",
			prgname);
}

static int
ups_parse_portmask(const char *portmask)
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
ups_parse_nqueue(const char *q_arg)
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
ups_parse_timer_period(const char *q_arg)
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
int
ups_parse_args(int argc, char **argv)
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
				ups_enabled_port_mask = ups_parse_portmask(optarg);
				if (ups_enabled_port_mask == 0) {
					printf("invalid portmask\n");
					ups_usage(prgname);
					return -1;
				}
				break;

				/* nqueue */
			case 'q':
				ups_rx_queue_per_lcore = ups_parse_nqueue(optarg);
				if (ups_rx_queue_per_lcore == 0) {
					printf("invalid queue number\n");
					ups_usage(prgname);
					return -1;
				}
				break;

				/* timer period */
			case 'T':
				timer_period = ups_parse_timer_period(optarg) * 1000 * TIMER_MILLISECOND;
				if (timer_period < 0) {
					printf("invalid timer period\n");
					ups_usage(prgname);
					return -1;
				}
				break;

				/* long options */
			case 0:
				ups_usage(prgname);
				return -1;

			default:
				ups_usage(prgname);
				return -1;
		}
	}

	if (optind >= 0)
		argv[optind-1] = prgname;

	ret = optind-1;
	optind = 0; /* reset getopt lib */
	return ret;
}


/*
	udp-client: a demo of UDP/IP socket communication

	usage:	client [-h serverhost] [-p port]
*/

#include <stdio.h>
#include <stdlib.h>	/* needed for os x*/
#include <string.h>	/* for strlen */
#include <netdb.h>      /* for gethostbyname() */
#include <sys/socket.h>
#include <netinet/in.h>

#include "port.h"       /* defines default port */

int conn(char *host, int port);
void disconn(void);

main(int argc, char **argv)
{
	extern char *optarg;
	extern int optind;
	int c, err = 0; 
	char *prompt = 0;
	int port = SERVICE_PORT;	/* default: whatever is in port.h */
	char *host = "localhost";	/* default: this host */
	static char usage[] = 
	              "usage: %s [-d] [-h serverhost] [-p port]\n";

	while ((c = getopt(argc, argv, "dh:p:")) != -1)
		switch (c) {
		case 'h':  /* hostname */
			host = optarg;
			break;
		case 'p':  /* port number */
			port = atoi(optarg);
			if (port < 1024 || port > 65535) {
				fprintf(stderr, "invalid port number: %s\n", optarg);
				err = 1;
			}
			break;
		case '?':
			err = 1;
			break;
		}
	if (err || (optind < argc)) {	/* error or extra arguments? */
		fprintf(stderr, usage, argv[0]);
		exit(1);
	}

	printf("connecting to %s, port %d\n", host, port);

	if (!conn(host, port))    /* connect */
		exit(1);   /* something went wrong */

	disconn();    /* disconnect */
	return 0;
}

int fd;  /* fd is the file descriptor for the connected socket */

/* conn: connect to the service running on host:port */
/* return 0 on failure, non-zero on success */
int
conn(char *host, int port)
{
	struct hostent *hp;	/* host information */
	unsigned int alen;	/* address length when we get the port number */
	struct sockaddr_in myaddr;	/* our address */
	struct sockaddr_in servaddr;	/* server address */

	printf("conn(host=\"%s\", port=\"%d\")\n", host, port);

	/* get a tcp/ip socket */
	/* We do this as we did it for the server */
	/* request the Internet address protocol */
	/* and a reliable 2-way byte stream */

	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		perror("cannot create socket");
		return 0;
	}

	/* bind to an arbitrary return address */
	/* because this is the client side, we don't care about the */
	/* address since no application will connect here  --- */
	/* INADDR_ANY is the IP address and 0 is the socket */
	/* htonl converts a long integer (e.g. address) to a network */
	/* representation (agreed-upon byte ordering */

	memset((char *)&myaddr, 0, sizeof(myaddr));
	myaddr.sin_family = AF_INET;
	myaddr.sin_addr.s_addr = htonl(INADDR_ANY);
	myaddr.sin_port = htons(0);

	if (bind(fd, (struct sockaddr *)&myaddr, sizeof(myaddr)) < 0) {
		perror("bind failed");
		return 0;
	}

	/* this part is for debugging only - get the port # that the operating */
	/* system allocated for us. */
        alen = sizeof(myaddr);
        if (getsockname(fd, (struct sockaddr *)&myaddr, &alen) < 0) {
                perror("getsockname failed");
                return 0;
        }
	printf("local port number = %d\n", ntohs(myaddr.sin_port));

	/* fill in the server's address and data */
	/* htons() converts a short integer to a network representation */

	memset((char*)&servaddr, 0, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_port = htons(port);

	/* look up the address of the server given its name */
	hp = gethostbyname(host);
	if (!hp) {
		fprintf(stderr, "could not obtain address of %s\n", host);
		return 0;
	}

	/* put the host's address into the server address structure */
	memcpy((void *)&servaddr.sin_addr, hp->h_addr_list[0], hp->h_length);

	/* connect to server */
	if (connect(fd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
		perror("connect failed");
		return 0;
	}
	return 1;
}

/* disconnect from the service */
void
disconn(void)
{
	printf("disconn()\n");
	shutdown(fd, 2);    /* 2 means future sends & receives are disallowed */
}

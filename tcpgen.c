/* tcpgen.c */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>
#include <poll.h>

#define D(_fmt, ...)                                            \
        do {                                                    \
		fprintf(stdout, "%s [%d] " _fmt "\n",		\
			__FUNCTION__, __LINE__, ##__VA_ARGS__);	\
        } while (0)


#define TCPGEN_PORT	5002
#define MAX_FLOWNUM	128
#define SOCKLISTLEN	1000

#define SRCPORT_MIN	5003
#define SRCPORT_MAX	65000
#define RANDOM_PORT() (rand () % (SRCPORT_MAX - SRCPORT_MIN) + SRCPORT_MIN)

#define POWERLAW(x)	(10 * x * x * x + x * 2) /* 10x^3 + x^2 */

enum {
	FLOWDIST_SAME,
	FLOWDIST_RANDOM,
	FLOWDIST_POWER,
};


struct tcpgen {
	struct in_addr dst;	/* destination address */

	int server_sock;		/* server socket for accept */
	int client_sock[MAX_FLOWNUM];	/* all client socket to send */

	int socklist[SOCKLISTLEN];	/* sock list to follow distribution */
	int socklistlen;		/* len of filled socklist */

	int flow_dist;		/* type of flow distribution */
	int flow_num;		/* number of flows */
	
	int data_len;		/* data size to be written to tcp socket  */

	int server_mode;	/* server mode */
	int client_mode;	/* client mode */

	pthread_t server_t;	/* thread id for server */
	pthread_t client_t;	/* thread id for client */

	int count;		/* number of xmit packets */
	int interval;	       	/* xmit interval */
	int randomized;		/* randomise source port */
	int thread_mode;	/* create threads for each socket (server) */
	int verbose;		/* verbose mode */
} tcpgen;


void
usage (void)
{
	printf ("\n"
		"usage: tcpgen\n"
		"\t -d : destination IP address\n"
		"\t -s : server mode only\n"
		"\t -c : client mode only\n"
		"\t -n : number of flows\n"
		"\t -t : distribution pattern (same, random, power)\n"
		"\t -x : number of xmit packet (default unlimited)\n"
		"\t -i : xmit interval (usec)\n"
		"\t -l : data length (tcp payload)\n"
		"\t -r : randomize source port\n"
		"\t -m : digit for seed of srand\n"
		"\t -p : pthread mode for each session (server mode)\n"
		"\t -D : daemon mode\n"
		"\t -v : verbose mode\n"
		"\n"
		);

	return;
}

int
tcp_client_socket (struct in_addr dst, int dstport, int srcport)
{
	int sock, ret, val = 1;
	struct sockaddr_in saddr;

	sock = socket (AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		perror ("failed to create client socket");
		return 0;
	}

	if (srcport) {
		ret = setsockopt (sock, SOL_SOCKET, SO_REUSEADDR,
				  &val, sizeof (val));
		if (ret < 0) {
			perror ("failed to set SO_REUSEADDR client  scoket");
			return 0;
		}

		memset (&saddr, 0, sizeof (saddr));
		saddr.sin_family = AF_INET;
		saddr.sin_port = srcport;
		saddr.sin_addr.s_addr = INADDR_ANY;

		ret = bind (sock, (struct sockaddr *)&saddr, sizeof (saddr));
		if (ret < 0) {
			perror ("bind failed for client socket");
			return 0;
		}
	}

	memset (&saddr, 0, sizeof (saddr));
	saddr.sin_family = AF_INET;
	saddr.sin_port = htons (dstport);
	saddr.sin_addr = dst;
	
	ret = connect (sock, (struct sockaddr *)&saddr, sizeof (saddr));
	if (ret < 0) {
		perror ("connect fialed");
		return 0;
	}

	return sock;
}

int
tcp_server_socket (int port)
{
	int sock, ret, val = 1;
	struct sockaddr_in saddr;

	sock = socket (AF_INET, SOCK_STREAM, 0);

	memset (&saddr, 0, sizeof (saddr));
	saddr.sin_family = AF_INET;
	saddr.sin_port = htons (port);
	saddr.sin_addr.s_addr = INADDR_ANY;

	ret = setsockopt (sock, SOL_SOCKET, SO_REUSEADDR, &val, sizeof (val));
	if (ret < 0) {
		perror ("failed to set SO_REUSEADDR");
		return 0;
	}

	ret = bind (sock, (struct sockaddr *)&saddr, sizeof (saddr));
	if (ret < 0) {
		perror ("bind failed");
		return 0;
	}

	return sock;
}

void *
server_thread_per_sock (void * param)
{
	/* a thread for a socket */

	int ret, sock = (*((int *) param));
	char buf[9216];

	pthread_detach (pthread_self ());

	while (1) {
		ret = read (sock, buf, sizeof (buf));
		if (ret < 1) {
			D ("close scoket for %d", sock);
			break;
		}
		if (tcpgen.verbose) {
			D ("read %d bytes from socket %d", ret, sock);
		}
	}

	close (sock);
	return NULL;
}

void *
server_thread (void * param)
{
	int n, ret, cfd, sknum = 1;
	socklen_t len;
	char buf[2048];
	pthread_t tid;
	struct sockaddr_in saddr;
	struct pollfd x[MAX_FLOWNUM + 1];
	
	memset (x, 0, sizeof (x));

	if (tcpgen.thread_mode)
		D ("thread mode on");

	tcpgen.server_sock = tcp_server_socket (TCPGEN_PORT);
	if (!tcpgen.server_sock) {
		return NULL;
	}

	/* x[0] is always listen socket. 1~ are accepted socket */
	x[0].fd = tcpgen.server_sock;
	x[0].events = POLLIN | POLLERR;

	D ("Start to listen server socket");
	listen (tcpgen.server_sock, MAX_FLOWNUM);

	/* XXX : each accepted socket should be handled by each thread */
	while (1) {
		poll (x, sknum, -1);

		for (n = 1; n < sknum; n++) {
			if (x[n].revents & POLLERR) {
				D ("connection faield for socket %d", n);
				goto err;
			}
			if (x[n].revents & POLLIN) {
				ret = read (x[n].fd, buf, sizeof (buf));
				if (tcpgen.verbose) {
					D ("read %d byte from socket %d",
					   ret, n);
				}
				x[n].revents = 0;
			}
		}

		if (x[0].revents & POLLIN) {
			/* accept new socket */

			cfd = accept (tcpgen.server_sock,
				      (struct sockaddr *)&saddr, &len);

			if (tcpgen.thread_mode) {
				pthread_create (&tid, NULL,
						server_thread_per_sock, &cfd);
				D ("new thread created for sock %d", cfd);
			} else {
				x[sknum].fd = cfd;
				x[sknum].events = POLLIN | POLLERR;
				D ("new connection is stored to %d", sknum);
			}
			sknum++;
			x[0].revents = 0;
		}
	}

err:
	if (!tcpgen.thread_mode) {
		for (n = 0; n < MAX_FLOWNUM + 1; n++) {
			if (x[n].fd != 0)
				close (x[n].fd);
		}
	}

	close (tcpgen.server_sock);

	return NULL;
}


void
flow_dist_init_same (void)
{
	/* the ratio of flows is uniform */

	int n, i;

	struct flow {
		int fd;
		float throughput;
		float ratio;
	} flows[MAX_FLOWNUM];

	float sum = 0;
	for (n = 0; n < tcpgen.flow_num; n++) {
		flows[n].fd = tcpgen.client_sock[n];
		flows[n].throughput = 10;
		sum += flows[n].throughput;
	}

	for (n = 0; n < tcpgen.flow_num; n++) {
		flows[n].ratio =
			(int) (flows[n].throughput / sum * SOCKLISTLEN);

		if (flows[n].ratio == 0)
			flows[n].ratio = 1;
	}

	int ssum = 0, slen = 0;
	for (n = 0; n < tcpgen.flow_num; n++) {
		D ("Flow %3d ratio is %.2f%%", n,
		   flows[n].ratio / SOCKLISTLEN * 100);
		ssum += flows[n].ratio / SOCKLISTLEN * 100;

		for (i = 0; i < flows[n].ratio; i++) {
			tcpgen.socklist[slen++] = flows[n].fd;
		}
	}

	tcpgen.socklistlen = slen;

	return;
}

void
flow_dist_init_random (void)
{
	/* the ratio of flows is uniform randomly */

	int n, i;

	struct flow {
		int fd;
		float throughput;
		float ratio;
	} flows[MAX_FLOWNUM];

	float sum = 0;
	for (n = 0; n < tcpgen.flow_num; n++) {
		flows[n].fd = tcpgen.client_sock[n];
		flows[n].throughput = rand () % MAX_FLOWNUM;
		sum += flows[n].throughput;
	}

	for (n = 0; n < tcpgen.flow_num; n++) {
		flows[n].ratio =
			(int) (flows[n].throughput / sum * SOCKLISTLEN);

		if (flows[n].ratio == 0)
			flows[n].ratio = 1;
	}

	int ssum = 0, slen = 0;
	for (n = 0; n < tcpgen.flow_num; n++) {
		D ("Flow %3d ratio is %.2f%%", n,
		   flows[n].ratio / SOCKLISTLEN * 100);
		ssum += flows[n].ratio / SOCKLISTLEN * 100;

		for (i = 0; i < flows[n].ratio; i++) {
			tcpgen.socklist[slen++] = flows[n].fd;
		}
	}

	tcpgen.socklistlen = slen;

	return;
}

void
flow_dist_init_power (void)
{
	/* the ratio of flows follows power-law */

	int n, i;

	struct flow {
		int fd;
		float throughput;
		float ratio;
	} flows[MAX_FLOWNUM];

	float sum = 0;
	for (n = 0; n < tcpgen.flow_num; n++) {
		flows[n].fd = tcpgen.client_sock[n];
		flows[n].throughput = POWERLAW (n + 1);
		sum += flows[n].throughput;
	}

	for (n = 0; n < tcpgen.flow_num; n++) {
		flows[n].ratio =
			(int) (flows[n].throughput / sum * SOCKLISTLEN);

		if (flows[n].ratio == 0)
			flows[n].ratio = 1;
	}

	int ssum = 0, slen = 0;
	for (n = 0; n < tcpgen.flow_num; n++) {
		D ("Flow %3d ratio is %.2f%%", n,
		   flows[n].ratio / SOCKLISTLEN * 100);
		ssum += flows[n].ratio / SOCKLISTLEN * 100;

		for (i = 0; i < flows[n].ratio; i++) {
			tcpgen.socklist[slen++] = flows[n].fd;
		}
	}

	tcpgen.socklistlen = slen;

	return;
}

void *
client_thread (void * param)
{
	int n, ret, fd, port, sknum = 0;
	char buf[9216];
	unsigned long xmitted = 0;

	/* create tcp client sockets for each flow */
	for (sknum = 0; sknum < tcpgen.flow_num; sknum++) {
		if (!tcpgen.randomized)
			port = 0;
		else
			port = RANDOM_PORT ();

		fd = tcp_client_socket (tcpgen.dst,TCPGEN_PORT, port);
		if (!fd)
			goto err;

		tcpgen.client_sock[sknum] = fd;
	}

	/* initalize flow distribution */
	switch (tcpgen.flow_dist) {
	case FLOWDIST_SAME :
		flow_dist_init_same ();
		break;
	case FLOWDIST_RANDOM :
		flow_dist_init_random ();
		break;
	case FLOWDIST_POWER :
		flow_dist_init_power ();
		break;
	default :
		D ("invalid flow distribution pattern");
		goto err;
	}

	/* send packets */

	while (1) {
		for (n = 0; n < tcpgen.socklistlen; n++) {
			ret = write (tcpgen.socklist[n], buf, tcpgen.data_len);
			if (ret < 0) {
				D ("failed to write %d byte to socket %d",
				   tcpgen.data_len, tcpgen.socklist[n]);
				goto err;
			}

			if (tcpgen.verbose)
				D ("write %d bytes to socket %d", ret,
				   tcpgen.socklist[n]);

			if (tcpgen.interval)
				usleep (tcpgen.interval);

			xmitted++;
			if (tcpgen.count && tcpgen.count < xmitted)
				goto err;
		}			
	}

err:
	for (n = 0; n < sknum; n++)
		close (tcpgen.client_sock[n]);

	return NULL;
}

int
main (int argc, char ** argv)
{
	int ch, ret, seed = 0, d = 0;

	/* set default value */
	memset (&tcpgen, 0, sizeof (tcpgen));
	tcpgen.flow_dist = FLOWDIST_SAME;
	tcpgen.flow_num = 1;
	tcpgen.data_len = 984; /* 1024 byte packet excluding ether header */

	while ((ch = getopt (argc, argv, "d:scn:t:x:i:l:rm:pDv")) != -1) {
		switch (ch) {
		case 'd' :
			ret = inet_pton (AF_INET, optarg, &tcpgen.dst);
			if (ret < 0) {
				D ("invalid dst address %s", optarg);
				perror ("inet_pton");
				return -1;
			}
			break;
		case 's' :
			tcpgen.server_mode = 1;
			break;
		case 'c' :
			tcpgen.client_mode = 1;
			break;
		case 'n' :
			tcpgen.flow_num = atoi (optarg);
			if (tcpgen.flow_num > MAX_FLOWNUM) {
				D ("max number of flows is %d", MAX_FLOWNUM);
				return -1;
			}
			break;
		case 't' :
			if (strncmp (optarg, "same", 4) == 0)
				tcpgen.flow_dist = FLOWDIST_SAME;
			else if (strncmp (optarg, "random", 6) == 0)
				tcpgen.flow_dist = FLOWDIST_RANDOM;
			else if (strncmp (optarg, "power", 5) == 0)
				tcpgen.flow_dist = FLOWDIST_POWER;
			else {
				D ("invalid distribution patter %s", optarg);
				return -1;
			}
			break;
		case 'x' :
			tcpgen.count = atoi (optarg);
			break;
		case 'i' :
			tcpgen.interval = atoi (optarg);
			break;
		case 'l' :
			tcpgen.data_len = atoi (optarg);
			break;
		case 'r' :
			tcpgen.randomized = 1;
			break;
		case 'm' :
			seed = atoi (optarg);
			break;
		case 'p' :
			tcpgen.thread_mode = 1;
			break;
		case 'D' :
			d = 1;
			break;
		case 'v' :
			tcpgen.verbose = 1;
			break;
		default :
			usage ();
			return -1;
		}
	}

	if (seed)
		srand (seed);
	else
		srand (time (NULL));

	if (d)
		daemon (1, 1);

	if (tcpgen.server_mode)
		pthread_create (&tcpgen.server_t, NULL, server_thread, NULL);

	if (tcpgen.client_mode)
		pthread_create (&tcpgen.client_t, NULL, client_thread, NULL);

	pthread_join (tcpgen.server_t, NULL);
	pthread_join (tcpgen.client_t, NULL);

	D ("tcpgen finished");

	return 0;
}

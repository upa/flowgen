/* flowgen.c */

#include <time.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netdb.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <arpa/inet.h>
#include <pthread.h>

#include <poll.h>

#define POLLTIMEOUT	1000 * 1	/* wait time 1 sec */

#define D(_fmt, ...)                                            \
        do {                                                    \
	fprintf(stdout, "%s [%d] " _fmt "\n", \
		__FUNCTION__, __LINE__, ##__VA_ARGS__);     \
        } while (0)


#define POWERLAW(x)  (10 * x * x * x + x * 2) /* 10x^3 + x^2 */

#ifdef __linux__
#define uh_sport source
#define uh_dport dest
#define uh_ulen len
#define uh_sum check
#endif /* linux */


#define DSTPORT		49152
#define SRCPORT_START	49153
#define SRCPORT_MAX	65534
#define FLOW_MAX	256
#define PORTLISTLEN	1000
#define PACKETMAXLEN	8192

enum {
	FLOWDIST_SAME,
	FLOWDIST_RANDOM,
	FLOWDIST_DWER,
};

void flow_dist_init_same (void);
void flow_dist_init_random (void);
void flow_dist_init_power (void);

void (* flowgen_flow_dist_init[]) (void) = {
	flow_dist_init_same,
	flow_dist_init_random,
	flow_dist_init_power,
};


#define DEFAULT_SRCADDR		"10.1.0.10"
#define DEFAULT_DSTADDR		"10.2.0.10"
#define DEFAULT_FLOWNUM		10
#define DEFAULT_FLOWDIST	FLOWDIST_SAME
#define DEFAULT_PACKETLEN	1010


struct flowgen {

	int socket;			/* raw socket		*/
	int rsocket;			/* receive socket 	*/
	struct sockaddr_in saddr_in;

	struct in_addr saddr;		/* source address	*/
	struct in_addr daddr;		/* destination address	*/

	int	port_list_len;		/* num of filled port list 	*/
	int	port_list[PORTLISTLEN];	/* srcport list  		*/
	int	port_candidates[PORTLISTLEN];

	int	flow_dist;		/* type of flow distribution */
	int	flow_num;		/* number of flows	*/

	int	pkt_len;		/* packet length */
	char 	pkt[PACKETMAXLEN];	/* test packet */

	int	interval;		/* xmit interval */
	int	recv_mode;		/* recv mode */
	int	recv_mode_only;		/* recv mode only */
	int	randomized;		/* randomize source port ? */
	int	count;			/* number of xmit packets */
	int	udp_mode;		/* udp socket instead of raw socket */
	int	verbose;		/* verbose mode */

} flowgen;

int cnt = 0; /* XXX dce debug */

#define IS_V() flowgen.verbose

/* from netmap pkt-gen.c */
static uint16_t
checksum (const void * data, uint16_t len, uint32_t sum)
{
	const uint8_t *addr = data;
	uint32_t i;

	/* Checksum all the pairs of bytes first... */
	for (i = 0; i < (len & ~1U); i += 2) {
		sum += (u_int16_t)ntohs(*((u_int16_t *)(addr + i)));
		if (sum > 0xFFFF)
			sum -= 0xFFFF;
	}
	/*
	 * If there's a single byte left over, checksum it, too.
	 * Network byte order is big-endian, so the remaining byte is
	 * the high byte.
	 */

	if (i < len) {
		sum += addr[i] << 8;
		if (sum > 0xFFFF)
			sum -= 0xFFFF;
	}

	return sum;
}

static u_int16_t
wrapsum (u_int32_t sum)
{
	sum = ~sum & 0xFFFF;
	return (htons(sum));
}

void
usage (char * progname)
{
	printf ("\n"
		"usage: %s"
		"\n"
		"\t" "-s : Source IP address (default 10.1.0.10)\n"
		"\t" "-d : Destination IP address (default 10.2.0.10)\n"
		"\t" "-n : Number of flows (default 10)\n"
		"\t" "-t : Type of flow distribution {same|random|power}"
		" (default same)\n"
		"\t" "-l : Packet size (excluding ether header 14byte)\n"
		"\t" "-m : Seed of srand\n"
		"\t" "-f : daemon mode\n"
		"\t" "-r : Randomize source ports of each flows\n"
		"\t" "-c : Number of xmit packets (defualt unlimited)\n"
		"\t" "-e : Receive mode\n"
		"\t" "-u : using UDP socket instead of raw socket\n"
		"\t" "-w : Run WITH receive thread\n"
		"\n",
		progname);

	return;
}

void
flowgen_default_value_init (void)
{
	memset (&flowgen, 0, sizeof (struct flowgen));

	/* set default ip addresses */
	inet_pton (AF_INET, DEFAULT_SRCADDR, &flowgen.saddr);
	inet_pton (AF_INET, DEFAULT_DSTADDR, &flowgen.daddr);

	/* set default flow related values */
	flowgen.flow_dist = DEFAULT_FLOWDIST;
	flowgen.flow_num = DEFAULT_FLOWNUM;

	flowgen.pkt_len = DEFAULT_PACKETLEN;

	flowgen.count = 0;

	return;
}


void
flowgen_socket_init (void)
{
	int sock, on = 1;


	/* fill sock addr */
	flowgen.saddr_in.sin_addr = flowgen.daddr;
	flowgen.saddr_in.sin_family = AF_INET;
	flowgen.saddr_in.sin_port = htons (DSTPORT);


	if (flowgen.udp_mode) {
		if ((sock = socket (AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
			D ("failed to create udp socket");
			perror ("socket");
			exit (1);
		}
#ifdef UDPCONNECT
		struct sockaddr_in saddr_in;
		memset (&saddr_in, 0, sizeof (saddr_in));
		saddr_in.sin_family = AF_INET;
		saddr_in.sin_addr.s_addr = INADDR_ANY;

		D ("bind");
		if (bind (sock, (struct sockaddr *)&saddr_in,
			  sizeof (struct sockaddr_in)) < 0)
			perror ("bind");

		D ("connect");
		if (connect (sock, (struct sockaddr *)&flowgen.saddr_in, 
			     sizeof (struct sockaddr_in)) < 0)
			perror ("connect");
#endif
		flowgen.socket = sock;
		return;
	}

	/* create raw socket */
	if ((sock = socket (AF_INET, SOCK_RAW, IPPROTO_RAW)) < 0) {
		D ("failed to create raw socket");
		perror ("socket");
		exit (1);
	}

	if (setsockopt (sock, IPPROTO_IP, IP_HDRINCL, &on, sizeof (on)) < 0) {
		D ("failed to set sockopt HDRINCL");
		perror ("setsockopt");
		exit (1);
	}
		
	if (IS_V()) 
		D ("raw socket is %d", sock);

	flowgen.socket = sock;

	return;
}

void
flowgen_packet_init (void)
{
	struct ip * ip;
	struct udphdr * udp;

	/* fill ip header */
	ip = (struct ip *) flowgen.pkt;

	ip->ip_v	= IPVERSION;
	ip->ip_hl	= 5;
	ip->ip_id	= 0;
	ip->ip_tos	= IPTOS_LOWDELAY;
	ip->ip_len	= htons (flowgen.pkt_len);
	ip->ip_off	= 0;
	ip->ip_ttl	= 16;
	ip->ip_p	= IPPROTO_UDP;
	ip->ip_dst	= flowgen.daddr;
	ip->ip_src	= flowgen.saddr;
	ip->ip_sum	= 0;
	ip->ip_sum	= wrapsum (checksum (ip, sizeof (*ip), 0));


	/* fill udp header */
	udp = (struct udphdr *) (ip + 1);

	udp->uh_dport	= htons (DSTPORT);
	udp->uh_sport	= 0;	/* filled when xmitted */
	udp->uh_ulen	= htons (flowgen.pkt_len - sizeof (*ip));
	udp->uh_sum	= 0;	/* no checksum */

	return;
};

void
flowgen_port_candidates_init (void)
{
	int n, i, candidate;
	
	if (!flowgen.randomized) {
		for (n = 0; n < flowgen.flow_num; n++) {
			flowgen.port_candidates[n] = SRCPORT_START + n;
			D ("Flow %2d is src port %d", n,
			   flowgen.port_candidates[n]);
		}
		return;
	}

	/* randomize udp source port  */
	for (n = 0; n < flowgen.flow_num; n++) {
		while (1) {
			candidate = SRCPORT_START +
				rand () % (SRCPORT_MAX - SRCPORT_START);
			for (i = 0; i < n; i++) {
				if (flowgen.port_candidates[i] == candidate)
					break;
			}
			if (i == n)
				break;
		}
		flowgen.port_candidates[n] = candidate;
		D ("Flow %2d is src port %d", n, candidate);
	}

	return;
}


void
flow_dist_init_same (void)
{
	/* throughput of each flow is same */

	int n, port = 0;

	for (n = 0; n < flowgen.flow_num; n++) {
		flowgen.port_list[n] = flowgen.port_candidates[port++];
	}

	flowgen.port_list_len = n;

	return;
}

void
flow_dist_init_random (void)
{
	/* The ratio of flows is random */

	int n, i, plen = 0, port = 0;
	float sum = 0;

	struct flow {
		float throughput;
		float ratio;
	} flows[FLOW_MAX];
		

	for (n = 0; n < flowgen.flow_num; n++) {
		flows[n].throughput = rand () % FLOW_MAX;
		sum += flows[n].throughput;
	}

	for (n = 0; n < flowgen.flow_num; n++) {
		flows[n].ratio =
			(int) (flows[n].throughput / sum * PORTLISTLEN);
		if (flows[n].ratio == 0)
			flows[n].ratio = 1;
	}

	int psum = 0;
	for (n = 0; n < flowgen.flow_num; n++) {

		D ("Flow %2d ratio is %f%%", n,
		   flows[n].ratio / PORTLISTLEN * 100);
		psum += flows[n].ratio / PORTLISTLEN * 100;

		for (i = 0; i < (int)flows[n].ratio; i++) {
			flowgen.port_list[plen++] =
				flowgen.port_candidates[port];
		}
		port++;
	}
	
	D ("Sum is %d%%, port list len is %d\n", psum, plen);


	flowgen.port_list_len = plen;

	return;
}

void
flow_dist_init_power (void)
{

	/* The ratio of lows follows Power Law */

	int n, i, plen = 0, port = 0;
	float sum = 0;

	struct flow {
		float throughput;
		float ratio;
	} flows[FLOW_MAX];
		

	for (n = 0; n < flowgen.flow_num; n++) {
		flows[n].throughput = POWERLAW (n);
		sum += flows[n].throughput;
	}

	for (n = 0; n < flowgen.flow_num; n++) {
		flows[n].ratio =
			(int) (flows[n].throughput / sum * PORTLISTLEN);
		if (flows[n].ratio == 0)
			flows[n].ratio = 1;
	}

	int psum = 0;
	for (n = 0; n < flowgen.flow_num; n++) {

		D ("Flow %2d ratio is %f%%", n,
		   flows[n].ratio / PORTLISTLEN * 100);
		psum += flows[n].ratio / PORTLISTLEN * 100;

		for (i = 0; i < (int)flows[n].ratio; i++) {
			flowgen.port_list[plen++] = 
				flowgen.port_candidates[port];
		}
		port++;
	}
	
	D ("Sum is %d%%, port list len is %d\n", psum, plen);

	flowgen.port_list_len = plen;

	return;

}

void
flowgen_start (void)
{
	int n, ret;
	struct udphdr * udp;

	udp = (struct udphdr *) (flowgen.pkt + sizeof (struct ip));

	if (flowgen.count) {
		D ("xmit %d packets", flowgen.count);
	}

	while (1) {
		for (n = 0; n < flowgen.port_list_len; n++) {
			udp->uh_sport = htons (flowgen.port_list[n]);

			ret = sendto (flowgen.socket, flowgen.pkt,
				      flowgen.pkt_len, 0,
				      (struct sockaddr *) &flowgen.saddr_in,
				      sizeof (struct sockaddr_in));

			if (IS_V()) {
				time_t now;
				now = time (NULL);
				D ("[%lu] %d: send %d bytes port %d",
				   now, ++cnt, ret, flowgen.port_list[n]);
			}

			if (flowgen.count) {
				flowgen.count--;
				if (flowgen.count == 0) 
					exit (1);
			}

			if (ret < 0) {
				perror ("send");
				break;
			}

			if (flowgen.interval)
				usleep (flowgen.interval);
		}
	}

	return;
}

void
flowgen_udp_start (void)
{
	int n, ret, len;
	char * pkt;

#ifdef POLL
	struct pollfd x[1];
	x[0].fd = flowgen.socket;
	x[0].events = POLLOUT;
#endif

	pkt = (char *) (flowgen.pkt + sizeof (struct ip) + 
			sizeof (struct udphdr));
	len = flowgen.pkt_len - sizeof (struct ip) - sizeof (struct udphdr);

	if (flowgen.count) {
		D ("xmit %d packets", flowgen.count);
	}

	while (1) {
		for (n = 0; n < flowgen.port_list_len; n++) {
#ifdef POLL
			poll (x, 1, -1);
#endif

#ifdef UDPCONNECT
			ret = write (flowgen.socket, pkt, len);
#else
			ret = sendto (flowgen.socket, pkt, len, 0,
				      (struct sockaddr *) &flowgen.saddr_in,
				      sizeof (struct sockaddr_in));
#endif

			if (IS_V()) {
				time_t now;
				now = time (NULL);
				D ("[%lu] %d: send %d bytes port %d",
				   now, ++cnt, ret, flowgen.port_list[n]);
			}

			if (flowgen.count) {
				flowgen.count--;
				if (flowgen.count == 0) 
					exit (1);
			}

			if (ret < 0) {
				perror ("send");
				break;
			}

			if (flowgen.interval)
				usleep (flowgen.interval);
		}
	}

	return;
}


void *
flowgen_receive_thread (void * param)
{
	int sock, ret, cnt;
	char buf[2048];
	struct sockaddr_in saddr_in;

	D ("Init receive thread");

	if ((sock = socket (AF_INET, SOCK_DGRAM, 0)) < 0) {
		D ("failed to create receive UDP socket");
		perror ("socket");
		exit (1);
	}
	
	if (IS_V())
		D ("receive UDP socket is %d", sock);


	memset (&saddr_in, 0, sizeof (saddr_in));
	saddr_in.sin_family = AF_INET;
	saddr_in.sin_port = htons (DSTPORT);
	saddr_in.sin_addr.s_addr = INADDR_ANY;
	
	if (bind (sock, (struct sockaddr *)&saddr_in, sizeof (saddr_in)) < 0) {
		D ("failed to bind receive socket");
		perror ("bind");
		exit (1);
	}

	cnt = 0;

	D ("waiting packet...");
	while (1) {

		ret = recv (sock, buf, sizeof (buf), 0);
		
		if (ret < 0) {
			D ("packet recv failed");
			perror ("recv");
			exit (1);
		}

		if (IS_V())
			D ("%d: [%lu] receive %d bytes packet", 
			   ++cnt, time (NULL), ret);
	}

	return NULL;
}

int
main (int argc, char ** argv)
{
	int ch, ret, f_flag = 0;
	unsigned long random_seed = 0;
	char * progname = argv[0];
	pthread_t tid;

	flowgen_default_value_init ();

	while ((ch = getopt (argc, argv, "s:d:n:t:l:c:i:m:ewfhruv")) != -1) {

		switch (ch) {
		case 's' :
			ret = inet_pton (AF_INET, optarg, &flowgen.saddr);
			if (ret < 0) {
				D ("invalid src address %s", optarg);
				perror ("inet_pton");
				exit (1);
			}
			break;
		case 'd' :
			ret = inet_pton (AF_INET, optarg, &flowgen.daddr);
			if (ret < 0) {
				D ("invalid dst address %s", optarg);
				perror ("inet_pton");
				exit (1);
			}
			break;
		case 'n' :
			ret = atoi (optarg);
			if (ret < 1 || FLOW_MAX - 1 < ret) {
				D ("flow num must be larger than 0 "
				   " and smaller than %d", FLOW_MAX);
				exit (1);
			}
			flowgen.flow_num = ret;
			break;
		case 't' :
			if (strncmp (optarg, "same", 4) == 0) {
				flowgen.flow_dist = FLOWDIST_SAME;
			} else if (strncmp (optarg, "random", 6) == 0) {
				flowgen.flow_dist = FLOWDIST_RANDOM;
			} else if (strncmp (optarg, "power", 5) == 0) {
				flowgen.flow_dist = FLOWDIST_DWER;
			} else {
				D ("invalid flow distribution type %s",
				    optarg);
				exit (1);
			}
			break;
		case 'l' :
			ret = atoi (optarg);
			if (ret < 50 || PACKETMAXLEN < ret) {
				D ("packet len muse be larger than 49 "
				    " and smaller than %d", PACKETMAXLEN + 1);
				exit (1);
			}
			flowgen.pkt_len = ret;
			break;
		case 'm' :
			sscanf (optarg, "%lu", &random_seed);
			D ("Random seed is %lu", random_seed);
			break;
		case 'i' :
			flowgen.interval = atoi (optarg);
			break;
		case 'e' :
			flowgen.recv_mode_only = 1;
		case 'w' :
			flowgen.recv_mode = 1;
			break;
		case 'c' :
			flowgen.count = atoi (optarg);
			break;
		case 'f' :
			f_flag = 1;
			break;
		case 'r' :
			flowgen.randomized = 1;
			break;
		case 'u' :
			flowgen.udp_mode = 1;
			break;
		case 'v' :
			flowgen.verbose = 1;
			break;
		case 'h' :
		default :
			usage (progname);
			exit (1);
		}
	}

	if (random_seed)
		srand (random_seed);
	else 
		srand ((unsigned) time (NULL));

	if (f_flag)
		daemon (0, 0);

	if (flowgen.recv_mode_only) {
		flowgen_receive_thread (NULL);
		return 0;
	}
	if (flowgen.recv_mode) {
		pthread_create (&tid, NULL, flowgen_receive_thread, NULL);
		pthread_detach (tid);
	}

	flowgen_socket_init ();
	flowgen_packet_init ();
	flowgen_port_candidates_init ();
	flowgen_flow_dist_init[flowgen.flow_dist] ();


	if (flowgen.udp_mode) {
		flowgen_udp_start ();
	} else {
		flowgen_start ();
	}

	close (flowgen.socket);
	D ("Finished");

	return 0;
}

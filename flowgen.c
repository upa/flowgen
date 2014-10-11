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
#define PORTLISTLEN	4096
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
#define DEFAULT_PACKETLEN	1024


struct flowgen {

	int socket;			/* raw socket		*/
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
} flowgen;


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
	printf ("usage: %s"
		"\n"
		"\t" "-s : Source IP address\n"
		"\t" "-d : Destination IP address\n"
		"\t" "-n : Number of flows\n"
		"\t" "-t : Type of flow distribution. {same|random|power}\n"
		"\t" "-l : Packet size\n"
		"\t" "-f : daemon mode\n"
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

	return;
}


void
flowgen_socket_init (void)
{
	int sock, on = 1;

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
		
	flowgen.socket = sock;

	return;
}

void
flowgen_packet_init (void)
{
	struct ip * ip;
	struct udphdr * udp;


	/* fill sock addr */
	flowgen.saddr_in.sin_addr = flowgen.daddr;
	flowgen.saddr_in.sin_family = AF_INET;


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
	
	srand ((unsigned) time (NULL));	

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
		D ("Flow %2d is %d", n, candidate);
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
		

	srand ((unsigned) time (NULL));

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
		

	srand ((unsigned) time (NULL));

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

	while (1) {
		for (n = 0; n < flowgen.port_list_len; n++) {
			udp->uh_sport = htons (flowgen.port_list[n]);
			ret = sendto (flowgen.socket, flowgen.pkt,
				      flowgen.pkt_len, 0,
				      (struct sockaddr *) &flowgen.saddr_in,
				      sizeof (struct sockaddr_in));
			if (ret < 0) {
				perror ("send");
				break;
			}
		}
	}

	return;
}

int
main (int argc, char ** argv)
{
	int ch, ret, f_flag = 0;
	char * progname = argv[0];


	flowgen_default_value_init ();

	while ((ch = getopt (argc, argv, "s:d:n:t:l:f")) != -1) {

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
				D ("flow num is larger than 0 "
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
			if (ret < 64 || PACKETMAXLEN < ret) {
				D ("packet len is larger than 64 "
				    " and smaller than %d", PACKETMAXLEN + 1);
				exit (1);
			}
			flowgen.pkt_len = ret;
			break;
		case 'f' :
			f_flag = 1;
			break;
		default :
			usage (progname);
			exit (1);
		}
	}

	if (f_flag)
		daemon (0, 0);


	flowgen_socket_init ();
	flowgen_packet_init ();
	flowgen_port_candidates_init ();
	flowgen_flow_dist_init[flowgen.flow_dist] ();

	flowgen_start ();

	return 0;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/time.h>

#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include <linux/netfilter.h>            /* for NF_ACCEPT */
#include <libnetfilter_queue/libnetfilter_queue.h>

#ifdef DEBUG
#include "pcap.h"
#endif

#define MTU				1500
#define XOR_OFFSET		12
#define XOR_SIZE_BIT	16
#define HOOK_IN			1
#define HOOK_OUT		3

#ifdef DEBUG
#define LOG(x, ...) printf(x, ##__VA_ARGS__)
#else
#define LOG(x, ...) if (enable_verbose) { printf(x, ##__VA_ARGS__); }
#endif
#define ERROR(x, ...) fprintf(stderr, x, ##__VA_ARGS__)

int queue_num = 0;
int enable_verbose = 0;
int enable_checksum = 0;
int enable_addzero = 0;
#ifdef DEBUG
pcap_dumpfile dumpfile = NULL;
#endif

void print_help() {
	printf("Usage:\n"
		"\tstrongtcp [--verbose | -v] [--checksum | -c] [--queue num | -q]\n"
#ifdef DEBUG
		"\t\t[--dump file | -d]\n"
#endif
	);
}

void parse_arguments(int argc, char **argv)
{
	int i = 1;
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			print_help();
			exit(0);
		} else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
			enable_verbose = 1;
		} else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--checksum") == 0) {
			enable_checksum = 1;
		} else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--queue") == 0) {
			if (i + 1 < argc) {
				queue_num = atoi(argv[i + 1]);
				i++;
			} else {
				ERROR("Invalid arguments.\n");
				print_help();
				exit(0);
			}
		}
#ifdef DEBUG
		else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--dump") == 0) {
			if (i + 1 < argc)
				dumpfile = pcap_dump_fileinit(argv[i + 1]);
			if(!dumpfile)
			{
				ERROR("Invalid arguments.\n");
				print_help();
				exit(0);
			}
		}
#endif
	}
}

static u_int16_t ip_cksum(u_int16_t *addr, int len)
{
	u_int16_t cksum;
	u_int32_t sum = 0;

	while (len > 1)
	{
		sum += *addr++;
		len -= 2;
	}
	if (len == 1)
	{
		if(enable_addzero)
		{
			u_int8_t tmp = *(u_int8_t *)addr;
			u_int16_t last = (u_int16_t)(tmp<<8);        // add 0
			sum += last;
		}
		else
			sum += *(u_int8_t*) addr;
	}
	sum = (sum >> 16) + (sum & 0xffff);  //把高位的进位，加到低八位，其实是32位加法
	sum += (sum >> 16);  //add carry
	cksum = ~sum;   //取反
	return (cksum);
}

static u_int16_t tcp_cksum(u_char *pkg_data)
{
	struct iphdr *ip4h = (struct iphdr *) pkg_data;
	struct tcphdr *tcph = (struct tcphdr *) (pkg_data + (ip4h->ihl * 4));
	char tcpBuf[MTU] = {0};

	if(ip4h->version == 6)
	{
		struct ip6_hdr *ip6h = (struct ip6_hdr *) pkg_data;
		struct ip6PseudoHeader {
		    struct in6_addr ip6_src;      /* source address */
		    struct in6_addr ip6_dst;      /* destination address */
			u_int32_t tcp_len;
			u_int8_t zero[3]; //always zero
			u_int8_t protocol; // = 6; //for tcp
		} psdh;

		psdh.ip6_src = ip6h->ip6_src;
		psdh.ip6_dst = ip6h->ip6_dst;
		psdh.tcp_len = ip6h->ip6_ctlun.ip6_un1.ip6_un1_plen;
		memset(psdh.zero, 0, 3);
		psdh.protocol = 6;

		memcpy(tcpBuf, &psdh, sizeof(struct ip6PseudoHeader));
		memcpy(tcpBuf + sizeof(struct ip6PseudoHeader), tcph, ntohs(psdh.tcp_len));
		return ip_cksum((u_int16_t *)tcpBuf, sizeof(struct ip6PseudoHeader) + ntohs(psdh.tcp_len));
	}
	else
	{
		struct ip4PseudoHeader {
			u_int32_t ip_src;
			u_int32_t ip_dst;
			u_int8_t zero;//always zero
			u_int8_t protocol;// = 6;//for tcp
			u_int16_t tcp_len;
		} psdh;

		psdh.ip_src = ip4h->saddr;
		psdh.ip_dst = ip4h->daddr;
		psdh.zero = 0;
		psdh.protocol = 6;
		psdh.tcp_len = htons(ntohs(ip4h->tot_len) - (ip4h->ihl * 4));
		memcpy(tcpBuf, &psdh, sizeof(struct ip4PseudoHeader));
		memcpy(tcpBuf + sizeof(struct ip4PseudoHeader), tcph, ntohs(psdh.tcp_len));
		return ip_cksum((u_int16_t *)tcpBuf, sizeof(struct ip4PseudoHeader) + ntohs(psdh.tcp_len));
	}
}

static int cb(struct nfq_q_handle *qh, struct nfgenmsg *nfmsg, struct nfq_data *nfa, void *data)
{

	struct nfqnl_msg_packet_hdr *ph;
	uint32_t id = 0;
	uint32_t pkg_data_len;

	u_char *pkg_data;

	LOG("entering callback\n");

	ph = nfq_get_msg_packet_hdr(nfa);
	if (ph)
	{
		id = ntohl(ph->packet_id);
		LOG("HWP:0x%04x HOK:%u ID:%u ", ntohs(ph->hw_protocol), ph->hook, id);
	}

	pkg_data_len = nfq_get_payload(nfa, &pkg_data);
	// nfq_get_payload() return -1 on error, otherwise > 0.
	if (pkg_data_len > 0)
	{
		LOG("LEN:%d ", pkg_data_len);

#ifdef DEBUG
		if(dumpfile && ph->hook == HOOK_OUT)
		{
			struct timeval tv;
			gettimeofday(&tv, NULL);

			pcaprec_hdr_t pcaprec_hdr;
			pcaprec_hdr.ts_usec = tv.tv_usec;
			pcaprec_hdr.ts_sec = tv.tv_sec;
			pcaprec_hdr.incl_len = pkg_data_len;
			pcaprec_hdr.orig_len = pkg_data_len;

			pcap_dump(pkg_data, pcaprec_hdr, dumpfile);
		}
#endif

		struct tcphdr *tcph;
		struct iphdr *ip4h = (struct iphdr *) pkg_data;
		if(ip4h->version == 6)
		{
			tcph = (struct tcphdr *) (pkg_data + sizeof(struct ip6_hdr));
			LOG("VER:IP6\n");
		}
		else
		{
			tcph = (struct tcphdr *) (pkg_data + (ip4h->ihl * 4));
			LOG("VER:IP4\n");
		}

		u_int32_t xor = (*(u_int32_t*) ((char*)tcph + XOR_OFFSET));

		LOG("FLG XOR:0x%08x\n", ntohl(xor));
		LOG("BEF SEQ:0x%08x ACK:0x%08x SUM:0x%04x\n", ntohl(tcph->seq), ntohl(tcph->ack_seq), ntohs(tcph->check));
		
		tcph->seq ^= xor;
		tcph->ack_seq ^= xor;
		if (enable_checksum) {
			tcph->check = 0;
			tcph->check = tcp_cksum(pkg_data);
		}

		LOG("AFT SEQ:0x%08x ACK:0x%08x SUM:0x%04x\n", ntohl(tcph->seq), ntohl(tcph->ack_seq), ntohs(tcph->check));

#ifdef DEBUG
		if(dumpfile && ph->hook == HOOK_IN)
		{
			struct timeval tv;
			gettimeofday(&tv, NULL);

			pcaprec_hdr_t pcaprec_hdr;
			pcaprec_hdr.ts_usec = tv.tv_usec;
			pcaprec_hdr.ts_sec = tv.tv_sec;
			pcaprec_hdr.incl_len = pkg_data_len;
			pcaprec_hdr.orig_len = pkg_data_len;

			pcap_dump(pkg_data, pcaprec_hdr, dumpfile);
		}
#endif

		return nfq_set_verdict(qh, id, NF_ACCEPT, pkg_data_len, (u_int8_t *) pkg_data);
	}

	ERROR("nfq_get_payload() not return > 0\n");
	return nfq_set_verdict(qh, id, NF_DROP, 0, NULL );
}

int main(int argc, char **argv)
{
	struct nfq_handle *h;
	struct nfq_q_handle *qh;
	int fd;
	int rv;
	char buf[4096] __attribute__ ((aligned));
	
	parse_arguments(argc, argv);

	LOG("re-calc checksum enable: %d\n", enable_checksum);

	if(enable_checksum)
	{
		u_int16_t test[] = {0x1234};
		enable_addzero = ((u_int32_t)((u_int16_t)(*(u_int8_t *)test)<<8)) == 0x00001200;        // need add 0
		LOG("addzero checksum enable: %d\n", enable_addzero);
	}

	LOG("opening library handle\n");
	h = nfq_open();
	if (!h)
	{
		ERROR("error during nfq_open()\n");
		exit(1);
	}

	LOG("unbinding existing nf_queue handler for AF_INET (if any)\n");
	if (nfq_unbind_pf(h, AF_INET) < 0)
	{
		ERROR("error during nfq_unbind_pf()\n");
		exit(1);
	}

	LOG("binding nfnetlink_queue as nf_queue handler for AF_INET\n");
	if (nfq_bind_pf(h, AF_INET) < 0)
	{
		ERROR("error during nfq_bind_pf()\n");
		exit(1);
	}

	LOG("binding this socket to queue '%d'\n", queue_num);
	qh = nfq_create_queue(h, queue_num, &cb, NULL );
	if (!qh)
	{
		ERROR("error during nfq_create_queue()\n");
		exit(1);
	}

	LOG("setting copy_packet mode\n");
	if (nfq_set_mode(qh, NFQNL_COPY_PACKET, 0xffff) < 0)
	{
		ERROR("can't set packet_copy mode\n");
		exit(1);
	}

	fd = nfq_fd(h);
	while ((rv = recv(fd, buf, sizeof(buf), 0)) && rv >= 0)
	{
		LOG("pkt received\n");
		nfq_handle_packet(h, buf, rv);
	}

	LOG("unbinding from queue '%d'\n", queue_num);
	nfq_destroy_queue(qh);

#ifdef INSANE
	/* normally, applications SHOULD NOT issue this command, since
	 * it detaches other programs/sockets from AF_INET, too ! */
	LOG("unbinding from AF_INET\n");
	nfq_unbind_pf(h, AF_INET);
#endif

	LOG("closing library handle\n");
	nfq_close(h);

#ifdef DEBUG
	pcap_dump_close(dumpfile);
#endif

	return 0;
}
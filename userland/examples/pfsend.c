/*
 * (C) 2003-23 - ntop 
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#define _GNU_SOURCE
#include <signal.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <netinet/in_systm.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <net/ethernet.h>     /* the L2 protocols */
#include <sys/time.h>
#include <time.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pcap.h>

#include "pfring.h"
#include "pfutils.c"
#include "third-party/patricia.c"

struct ip_header {
#if BYTE_ORDER == LITTLE_ENDIAN
  u_int32_t	ihl:4,		/* header length */
    version:4;			/* version */
#else
  u_int32_t	version:4,			/* version */
    ihl:4;		/* header length */
#endif
  u_int8_t	tos;			/* type of service */
  u_int16_t	tot_len;			/* total length */
  u_int16_t	id;			/* identification */
  u_int16_t	frag_off;			/* fragment offset field */
  u_int8_t	ttl;			/* time to live */
  u_int8_t	protocol;			/* protocol */
  u_int16_t	check;			/* checksum */
  u_int32_t saddr, daddr;	/* source and dest address */
} __attribute__((packed));

struct ip6_header {
  u_int8_t	priority:4,
		version:4;
  u_int8_t	flow_lbl[3];
  u_int16_t	payload_len;
  u_int8_t	nexthdr;
  u_int8_t	hop_limit;
  u_int32_t     saddr[4];
  u_int32_t     daddr[4];
} __attribute__((packed));

struct udp_header {
  u_int16_t	source;		/* source port */
  u_int16_t	dest;		/* destination port */
  u_int16_t	len;		/* udp length */
  u_int16_t	check;		/* udp checksum */
} __attribute__((packed));

struct tcp_header {
  u_int16_t source;
  u_int16_t dest;
  u_int32_t seq;
  u_int32_t ack_seq;
  u_int16_t flags;
  u_int16_t window;
  u_int16_t check;
  u_int16_t urg_ptr;
} __attribute__((packed));

struct packet *pkt_head = NULL;
pfring  *pd, *twin_pd = NULL;
pfring_stat pfringStats;
char *device = NULL, *twin_device = NULL, *cidr = NULL;
u_int8_t wait_for_packet = 1, do_shutdown = 0;
u_int32_t pkt_loop = 0, pkt_loop_sent = 0, uniq_pkts_per_sec = 0;
u_int32_t uniq_k_pkts_per_pkts = 1, uniq_pkts_per_pkts = 0;
u_int64_t num_pkt_good_sent = 0, last_num_pkt_good_sent = 0;
u_int64_t num_bytes_good_sent = 0, last_num_bytes_good_sent = 0;
u_int32_t mtu = 1500;
struct timeval lastTime, startTime;
int reforge_ip = 0, on_the_fly_reforging = 0;
int send_len = 60;
int daemon_mode = 0;
patricia_tree_t *patricia_v4 = NULL;
#if !(defined(__arm__) || defined(__mips__))
double pps = 0;
double gbps = 0;
ticks inter_frame_ticks = 0;
ticks kbyte_ticks = 0;
u_int8_t inter_frame_ticks_adjusted = 0;
#endif

#define DEFAULT_DEVICE     "eth0"

/* *************************************** */

void print_stats() {
  double deltaMillisec, currentThpt, avgThpt, currentThptBits, currentThptBytes, avgThptBits, avgThptBytes;
  struct timeval now;
  char buf1[64], buf2[64], buf3[64], buf4[64], buf5[64], statsBuf[512], timebuf[128];
  u_int64_t deltaMillisecStart;

  gettimeofday(&now, NULL);
  deltaMillisec = delta_time(&now, &lastTime);
  currentThpt = (double)((num_pkt_good_sent-last_num_pkt_good_sent) * 1000)/deltaMillisec;
  currentThptBytes = (double)((num_bytes_good_sent-last_num_bytes_good_sent) * 1000)/deltaMillisec;
  currentThptBits = currentThptBytes * 8;

  deltaMillisec = delta_time(&now, &startTime);
  avgThpt = (double)(num_pkt_good_sent * 1000)/deltaMillisec;
  avgThptBytes = (double)(num_bytes_good_sent * 1000)/deltaMillisec;
  avgThptBits = avgThptBytes * 8;

  if (!daemon_mode) {
    snprintf(statsBuf, sizeof(statsBuf),
	     "TX rate: [current %s pps/%s Gbps][average %s pps/%s Gbps][total %s pkts]",
	     pfring_format_numbers(currentThpt, buf1, sizeof(buf1), 1),
	     pfring_format_numbers(currentThptBits/(1000*1000*1000), buf2, sizeof(buf2), 1),
	     pfring_format_numbers(avgThpt, buf3, sizeof(buf3), 1),
	     pfring_format_numbers(avgThptBits/(1000*1000*1000),  buf4, sizeof(buf4), 1),
	     pfring_format_numbers(num_pkt_good_sent, buf5, sizeof(buf5), 1));
 
    fprintf(stdout, "%s\n", statsBuf);
  }

  deltaMillisecStart = delta_time(&now, &startTime);
  snprintf(statsBuf, sizeof(statsBuf),
           "Duration:          %s\n"
           "SentPackets:       %lu\n"
           "SentBytes:         %lu\n"
           "CurrentSentPps:    %lu\n"
           "CurrentSentBitps:  %lu\n",
           msec2dhmsm(deltaMillisecStart, timebuf, sizeof(timebuf)),
           (long unsigned int) num_pkt_good_sent,
           (long unsigned int) num_bytes_good_sent,
	   (long unsigned int) currentThpt,
	   (long unsigned int) currentThptBits);
  pfring_set_application_stats(pd, statsBuf);

  /* Adjust rate */
  if (!inter_frame_ticks_adjusted && last_num_pkt_good_sent /* second iteration */) {
    if (gbps > 0) {
      double gbps_delta = gbps - (currentThptBits / 1000000000);
      kbyte_ticks = kbyte_ticks - (kbyte_ticks * (gbps_delta / gbps));
    } else if (pps > 0) {
      double pps_delta = pps - currentThpt;
      inter_frame_ticks = inter_frame_ticks - (inter_frame_ticks * (pps_delta / pps));
    }
    inter_frame_ticks_adjusted = 1;
  }

  memcpy(&lastTime, &now, sizeof(now));
  last_num_pkt_good_sent = num_pkt_good_sent, last_num_bytes_good_sent = num_bytes_good_sent;
}

/* ******************************** */

void my_sigalarm(int sig) {
  if(do_shutdown) return;
  print_stats();
  alarm(1);
  signal(SIGALRM, my_sigalarm);
}

/* ******************************** */

void sigproc(int sig) {
  if(do_shutdown) return;
  fprintf(stdout, "Leaving...\n");
  do_shutdown = 1;
}

/* *************************************** */

void printHelp(void) {
  printf("pfsend - (C) 2011-24 ntop\n");
  printf("Replay synthetic traffic, or a pcap, or a packet in hex format from standard input.\n\n"); 
  printf("pfsend -i out_dev [-a] [-f <.pcap file>] [-g <core_id>] [-h]\n"
         "       [-l <length>] [-n <num>] "
#if !(defined(__arm__) || defined(__mips__))
	 "[-r <rate>] [-p <rate>] "
#endif
	 "[-m <dst MAC>]\n"
	 "       [-w <TX watermark>] [-v]\n\n");
  printf("-a              Active send retry\n");
#if 0
  printf("-b <cpu %%>      CPU pergentage priority (0-99)\n");
#endif
  printf("-f <.pcap file> Send packets as read from a pcap file\n");
  printf("-B <BPF>        Send packets matching the provided BPF filter only\n");
  printf("-g <core_id>    Bind this app to a core\n");
  printf("-G <cidr>       Use packits belonging to the CIDR to the twin device\n");
  printf("-h              Print this help\n");
  printf("-i <device>     Egress device name. Repeat it to specify the twin device (see -G)\n");
  printf("-l <length>     Packet length to send. Ignored with -f\n");
  printf("-n <num>        Num pkts to send (use 0 for infinite)\n");
#if !(defined(__arm__) || defined(__mips__))
  printf("-r <Gbps rate>  Rate to send (example -r 2.5 sends 2.5 Gbit/sec, -r -1 pcap capture rate)\n");
  printf("-p <pps rate>   Rate to send (example -p 100 send 100 pps)\n");
#endif
  printf("-M <src MAC>    Reforge source MAC (format AA:BB:CC:DD:EE:FF)\n");
  printf("-m <dst MAC>    Reforge destination MAC (format AA:BB:CC:DD:EE:FF)\n");
  printf("-b <num>        Reforge source IP with <num> different IPs (balanced traffic)\n");
  printf("-c <num>        Reforge destination IP with <num> different IPs (ignore -b)\n");
  printf("-t <num>        Reforge source port with <num> different ports per IP (-b)\n");
  printf("-S <ip>         Use <ip> as base source IP for -b (default: 10.0.0.1)\n");
  printf("-D <ip>         Use <ip> as destination IP (default: 192.168.0.1)\n");
  printf("-V <version>    Generate IP version <version> packets (default: 4, mixed: 0)\n");
  printf("-8 <num>        Send the same packets <num> times before moving to the next\n");
  printf("-O              On the fly reforging instead of preprocessing (-b)\n");
  printf("-A <num>        Add <num> different packets every second up to -b\n");
  printf("-U <num>        Add K more different packets (-K) every <num> packets up to -b\n");
  printf("-K <num>        Add <num> more different packets every -U packets up to -b\n");
  printf("-z              Randomize generated IPs sequence (requires -b)\n");
  printf("-o <num>        Offset for generated IPs (-b) or packets in pcap (-f)\n");
  printf("-W <ID>[,<ID>]  Forge VLAN packets with the specified VLAN ID (and QinQ ID if specified after comma)\n");
  printf("-L <num>        Forge VLAN packets with <num> different ids\n");
  printf("-0 <char>       Fill packet payload with the specified character (default: 0)\n");
  printf("-F              Force flush for each packet (to avoid bursts, expect low performance)\n");
  printf("-w <watermark>  TX watermark (low value=low latency) [not effective on ZC]\n");
  printf("-d              Daemon mode\n");
  printf("-P <pid file>   Write pid to the specified file (daemon mode only)\n");
  printf("-v              Verbose\n");
  exit(0);
}

/* ******************************************* */

static struct pfring_pkthdr hdr; /* note: this is static to be (re)used by on the fly reforging */

static int reforge_packet(u_char *buffer, u_int buffer_len, u_int idx, u_int use_prev_hdr) {
  struct ip_header *ip_header;

  if (reforge_dst_mac) memcpy(buffer, dstmac, 6);
  if (reforge_src_mac) memcpy(&buffer[6], srcmac, 6);

  if (reforge_ip) {
    if (!use_prev_hdr) {
      memset(&hdr, 0, sizeof(hdr));
      hdr.len = hdr.caplen = buffer_len;

      if (pfring_parse_pkt(buffer, &hdr, 4, 0, 0) < 3)
        return -1;
      if (hdr.extended_hdr.parsed_pkt.ip_version != 4)
        return -1;
    }

    ip_header = (struct ip_header *) &buffer[hdr.extended_hdr.parsed_pkt.offset.l3_offset];

    if (balance_source_ips)
      ip_header->daddr = dstaddr.s_addr;
    else
      ip_header->daddr = htonl((ntohl(dstaddr.s_addr) + ip_offset + (idx % num_ips)) & 0xFFFFFFFF);

    if (balance_source_ips)
      ip_header->saddr = htonl((ntohl(srcaddr.s_addr) + ip_offset + (idx % num_ips)) & 0xFFFFFFFF);
    else
      ip_header->saddr = srcaddr.s_addr;

    ip_header->check = 0;
    ip_header->check = wrapsum(in_cksum((unsigned char *) ip_header, sizeof(struct ip_header), 0));

    if (hdr.extended_hdr.parsed_pkt.l3_proto == IPPROTO_UDP) {
      struct udp_header *udp_header = (struct udp_header *) &buffer[hdr.extended_hdr.parsed_pkt.offset.l4_offset];
      udp_header->check = 0;
      udp_header->check = wrapsum(in_cksum((unsigned char *) udp_header, sizeof(struct udp_header),
                                    in_cksum((unsigned char *) &buffer[hdr.extended_hdr.parsed_pkt.offset.payload_offset], 
                                      buffer_len - hdr.extended_hdr.parsed_pkt.offset.payload_offset,
                                      in_cksum((unsigned char *) &ip_header->saddr, 2 * sizeof(ip_header->saddr),
                                        IPPROTO_UDP + ntohs(udp_header->len)))));
    } else if (hdr.extended_hdr.parsed_pkt.l3_proto == IPPROTO_TCP) {
      struct tcp_header *tcp_header = (struct tcp_header *) &buffer[hdr.extended_hdr.parsed_pkt.offset.l4_offset];
      int tcp_hdr_len = hdr.extended_hdr.parsed_pkt.offset.payload_offset - hdr.extended_hdr.parsed_pkt.offset.l4_offset;
      int payload_len = buffer_len - hdr.extended_hdr.parsed_pkt.offset.payload_offset;
      tcp_header->check = 0;
      tcp_header->check = wrapsum(in_cksum((unsigned char *) tcp_header, tcp_hdr_len,
                                   in_cksum((unsigned char *) &buffer[hdr.extended_hdr.parsed_pkt.offset.payload_offset],
                                     payload_len,
                                     in_cksum((unsigned char *) &ip_header->saddr, 2 * sizeof(ip_header->saddr),
                                       IPPROTO_TCP + ntohs(htons(tcp_hdr_len + payload_len))))));
    }
  }

  return 0;
}

/* *************************************** */

static void randomize_packets() {
  struct packet *tobemoved, *add_before, *prev, *tmp, *last;
  int j, n, moved_pkts = 0;
 
  // keep the first item in pkt_head and detach the second
  last = tobemoved = pkt_head->next;
  pkt_head->next = NULL;
  moved_pkts++;

  while (tobemoved != NULL && !do_shutdown) {
    // detach item
    tmp = tobemoved->next;
    tobemoved->next = NULL;

    // get a random item in the destination list
    n = random() % (min_val(moved_pkts, 200));
    prev = pkt_head;
    add_before = pkt_head->next;
    for (j = 0; j < n && add_before != NULL; j++) {
      prev = add_before; 
      add_before = add_before->next;
    }
      
    // move the detached item
    prev->next = tobemoved;
    tobemoved->next = add_before;
    moved_pkts++;

    last = tobemoved;
    tobemoved = tmp;
  }

  last->next = pkt_head;
}

/* *************************************** */

pfring* open_device(char *name, u_int32_t flags, int watermark) {
  pfring *pfd = pfring_open(name, mtu, flags);
  
  if(pfd == NULL) {
    printf("pfring_open error [%s] (pf_ring not loaded or interface %s is down ?)\n", 
           strerror(errno), name);
    return(NULL);
  } else {
    u_int32_t version;

    pfring_set_application_name(pfd, "pfsend");
    pfring_version(pfd, &version);

    printf("Using PF_RING v.%d.%d.%d on %s\n", (version & 0xFFFF0000) >> 16,
	   (version & 0x0000FF00) >> 8, version & 0x000000FF,
	   name);
  }

  if(watermark > 0) {
    int rc;

    if((rc = pfring_set_tx_watermark(pfd, watermark)) < 0) {
      if (rc == PF_RING_ERROR_NOT_SUPPORTED)
        printf("pfring_set_tx_watermark() now supported on %s\n", name);
      else
        printf("pfring_set_tx_watermark() failed [rc=%d]\n", rc);
    }
  }

  return(pfd);
}

/* ****************************************************** */

int fill_prefix_v4(prefix_t *p, const struct in_addr *a, int b, int mb) {
  memset(p, 0, sizeof(prefix_t));

  if(b < 0 || b > mb)
    return(-1);

  p->add.sin.s_addr = a->s_addr, p->family = AF_INET, p->bitlen = b, p->ref_count = 0;

  return(0);
}

/* *************************************** */

u_int8_t compute_packet_index(u_char *pkt, u_int pkt_len) {
  struct pfring_pkthdr hdr;
  int rc;

  memset(&hdr, 0, sizeof(hdr));
  hdr.len = hdr.caplen = pkt_len;

  rc = pfring_parse_pkt(pkt, &hdr, 3, 0, 0);

  if (rc < 3) {
    //fprintf(stderr, "Parse error (%d)\n", rc);
    return(0); 
  }

  if(hdr.extended_hdr.parsed_pkt.ip_version == 4) {
    prefix_t prefix;
    struct in_addr addr;
    patricia_node_t *node;

    /* Match source IP */
    addr.s_addr = htonl(hdr.extended_hdr.parsed_pkt.ipv4_src);
    fill_prefix_v4(&prefix, &addr, 32, 32);
    node = patricia_search_best(patricia_v4, &prefix);
    if(node) return(1);

#if 0
    /* Match destination IP */
    addr.s_addr = htonl(hdr.extended_hdr.parsed_pkt.ipv4_dst);
    fill_prefix_v4(&prefix, &addr, 32, 32);
    node = patricia_search_best(patricia_v4, &prefix);
    if(node) return(1);
#endif
  } else {
#if 0
    /* TODO IPv6 support */
    hdr.extended_hdr.parsed_pkt.ipv6_src;
    hdr.extended_hdr.parsed_pkt.ipv6_dst;
#endif
  }
  
  return(0);
}

/* *************************************** */

int main(int argc, char* argv[]) {
  char *pcap_in = NULL, path[255] = { 0 };
  int c, i, n, verbose = 0, active_poll = 0;
  u_int mac_a, mac_b, mac_c, mac_d, mac_e, mac_f;
  u_char buffer[MAX_PACKET_SIZE];
  u_int32_t num_to_send = 0;
  int bind_core = -1;
  u_int16_t cpu_percentage = 0;
#if !(defined(__arm__) || defined(__mips__))
  ticks tick_start = 0, tick_prev = 0, tick_delta = 0;
#endif
  u_int64_t curr_uniq_pkts_limit = 0;
  u_int64_t on_the_fly_sent = 0;
  u_int64_t uniq_pkts_per_pkts_cnt = 0;
  ticks hz = 0;
  struct packet *tosend;
  int num_uniq_pkts = 1, watermark = 0;
  u_int num_pcap_pkts = 0;
  int send_full_pcap_once = 1;
  char *pidFileName = NULL;
  int send_error_once = 1;
  int randomize = 0;
  int reforging_idx;
  int stdin_packet_len = 0;
  u_int ip_v = 4;
  int flush = 0;
  char *bpfFilter = NULL;
  u_int32_t flags = 0;
  int rc;

  srandom(time(NULL));

  srcaddr.s_addr = 0x0100000A /* 10.0.0.1 */;
  dstaddr.s_addr = 0x0100A8C0 /* 192.168.0.1 */;

  dstmac[0] = 0x0, dstmac[1] = 0x1, dstmac[2] = 0x2;
  dstmac[3] = 0x3, dstmac[4] = 0x4, dstmac[5] = 0x5;
  srcmac[0] = 0x0, srcmac[1] = 0x1, srcmac[2] = 0x2;
  srcmac[3] = 0x4, srcmac[4] = 0x5, srcmac[5] = 0x6;

  while((c = getopt(argc, argv, "A:b:B:c:dD:hi:n:g:G:l:L:o:Oaf:Fr:vm:M:p:P:S:t:K:U:V:w:W:z8:0:")) != -1) {
    switch(c) {
    case 'A':
      uniq_pkts_per_sec = atoi(optarg);
      break;
    case 'c':
      balance_source_ips = 0;
      /* fall through */
    case 'b':
      num_ips = atoi(optarg);
      if(num_ips == 0) num_ips = 1;
      if (num_uniq_pkts < num_ips * num_ports)
        num_uniq_pkts = num_ips * num_ports;
      reforge_ip = 1;
      break;
    case 'B':
      bpfFilter = strdup(optarg);
      break;
    case 'D':
      inet_aton(optarg, &dstaddr);
      reforge_ip = 1;
      break;
    case 'G':
      cidr = strdup(optarg);
      break;
    case 'h':
      printHelp();
      break;
    case 'i':
      if(device == NULL)
	device = strdup(optarg);
      else
	twin_device = strdup(optarg);
      break;
    case 'f':
      pcap_in = strdup(optarg);
      break;
    case 'F':
      flush = 1;
      break;
    case 'n':
      num_to_send = atoi(optarg);
      send_full_pcap_once = 0;
      break;
    case 'o':
      ip_offset = atoi(optarg);
      break;
    case 'O':
      on_the_fly_reforging = 1;
      break;
    case 'g':
      bind_core = atoi(optarg);
      break;
    case 'l':
      send_len = atoi(optarg);
      if (send_len > MAX_PACKET_SIZE) send_len = MAX_PACKET_SIZE;
      if (send_len > mtu) mtu = send_len;
      break;
    case 'L':
      forge_vlan = 1;
      num_vlan = atoi(optarg);
      if (num_uniq_pkts < num_vlan)
        num_uniq_pkts = num_vlan;
      break;
    case 'v':
      verbose = 1;
      break;
    case 'a':
      active_poll = 1;
      break;
#if !(defined(__arm__) || defined(__mips__))
    case 'r':
      sscanf(optarg, "%lf", &gbps);
      break;
    case 'p':
      sscanf(optarg, "%lf", &pps);
      break;
#endif
    case 'm':
      if(sscanf(optarg, "%02X:%02X:%02X:%02X:%02X:%02X", &mac_a, &mac_b, &mac_c, &mac_d, &mac_e, &mac_f) != 6) {
	printf("Invalid MAC address format (XX:XX:XX:XX:XX:XX)\n");
	return(0);
      } else {
	reforge_dst_mac = 1;
	dstmac[0] = mac_a, dstmac[1] = mac_b, dstmac[2] = mac_c;
	dstmac[3] = mac_d, dstmac[4] = mac_e, dstmac[5] = mac_f;
      }
      break;
    case 'M':
      if(sscanf(optarg, "%02X:%02X:%02X:%02X:%02X:%02X", &mac_a, &mac_b, &mac_c, &mac_d, &mac_e, &mac_f) != 6) {
	printf("Invalid MAC address format (XX:XX:XX:XX:XX:XX)\n");
	return(0);
      } else {
	reforge_src_mac = 1;
	srcmac[0] = mac_a, srcmac[1] = mac_b, srcmac[2] = mac_c;
	srcmac[3] = mac_d, srcmac[4] = mac_e, srcmac[5] = mac_f;
      }
      break;
    case 'S':
      inet_aton(optarg, &srcaddr);
      reforge_ip = 1;
      break;
    case 'd':
      daemon_mode = 1;
      break;
    case 'P':
      pidFileName = strdup(optarg);
      break;
    case 'K':
      uniq_k_pkts_per_pkts = atoi(optarg);
      break;
    case 'U':
      uniq_pkts_per_pkts = atoi(optarg);
      break;
    case 'V':
      ip_v = atoi(optarg);
      break;
    case 'w':
      watermark = atoi(optarg);
      break;
    case 'W':
      {
        char *comma = strchr(optarg, ',');
        if(comma != NULL) {
          comma[0] = '\0'; comma++;
          qinq_vlan_id = atoi(comma);
          forge_qinq_vlan = 1;
        }
        vlan_id = atoi(optarg);
        forge_vlan = 1;
      }
      break;
    case 't':
      num_ports = atoi(optarg);
      if(num_ports == 0) num_ports = 1;
      if (num_uniq_pkts < num_ips * num_ports)
        num_uniq_pkts = num_ips * num_ports;
      reforge_ip = 1;
      break;
    case 'z':
      randomize = 1;
      break;
    case '8':
      pkt_loop = atoi(optarg);
      break;
    case '0':
      packet_content = optarg[0];
      break;
    default:
      printHelp();
    }
  }

  if (device == NULL 
      || num_uniq_pkts < 1
      || optind < argc /* Extra argument */)
    printHelp();

  if((cidr && (twin_device == NULL))
     || ((cidr == NULL) && (twin_device != NULL))) {
    printf("Warning: ignored -G/-i as both of them need to be specified simultaneously\n");
    cidr = NULL, twin_device = NULL;
  }

  if(cidr != NULL) {
    /* 192.168.0.0/16,10.0.0.0/8 */
    char *item, *tmp;

    patricia_v4 = patricia_new(32 /* IPv4 */);
    
    item = strtok_r(cidr, ",", &tmp);

    while(item != NULL) {
      prefix_t prefix;
      char *tmp1, *net = strtok_r(item, "/", &tmp1);
      int slash = 32;
      in_addr_t a;
      
      if(net) {
	char *s = strtok_r(NULL, "/", &tmp1);

	if(s != NULL)
	  slash = atoi(s);
      }

      a = inet_addr(net);
      fill_prefix_v4(&prefix, (const struct in_addr *)&a, slash, 32);
      patricia_lookup(patricia_v4, &prefix);

      item = strtok_r(NULL, ",", &tmp);
    }
  }
  
  if (num_uniq_pkts > 1000000 && !on_the_fly_reforging)
    printf("Warning: please use -O to reduce memory preallocation when many IPs are configured with -b\n");

  bind2node(bind_core);

  if (daemon_mode)
    daemonize();

  if (pidFileName)
    create_pid_file(pidFileName);

  printf("Sending packets on %s\n", device);

  if(bpfFilter != NULL)
    flags |= PF_RING_TX_BPF;

  if((pd = open_device(device, flags, watermark)) == NULL)
    return(-1);

  if(twin_device) {
    if((twin_pd = open_device(twin_device, flags, watermark)) == NULL)
      return(-1);
  }
  
  signal(SIGINT, sigproc);
  signal(SIGTERM, sigproc);
  signal(SIGINT, sigproc);

  if(send_len < 60)
    send_len = 60;

  if (ip_v != 4 && send_len < 62)
    send_len = 62; /* min len with IPv6 */

#if !(defined(__arm__) || defined(__mips__))
  if(gbps != 0 || pps != 0 || uniq_pkts_per_sec) {
    /* computing usleep delay */
    tick_start = getticks();
    usleep(1);
    tick_delta = getticks() - tick_start;

    /* computing CPU freq */
    tick_start = getticks();
    usleep(1001);
    hz = (getticks() - tick_start - tick_delta) * 1000 /*kHz -> Hz*/;
    printf("Estimated CPU freq: %lu Hz\n", (long unsigned int)hz);
  }
#endif

  if(pcap_in) {
    char ebuf[256];
    u_char *pkt;
    struct pcap_pkthdr *h;
    pcap_t *pt = pcap_open_offline(pcap_in, ebuf);
    struct timeval beginning = { 0, 0 };
    u_int64_t avg_send_len = 0;
    u_int32_t num_orig_pcap_pkts = 0;

    on_the_fly_reforging = 0;

    if(pt) {
      struct packet *last = NULL;
      int datalink = pcap_datalink(pt);

      if (datalink == DLT_LINUX_SLL)
        printf("Linux 'cooked' packets detected, stripping 2 bytes from header..\n");

      while (1) {
	struct packet *p;
	int rc = pcap_next_ex(pt, &h, (const u_char **) &pkt);

	if(rc <= 0) {
          if (rc == PCAP_ERROR)
            pcap_perror(pt, "" /* prefix */);
	  break;
	}
        
        num_orig_pcap_pkts++;
        if ((num_orig_pcap_pkts-1) < ip_offset) continue;

	if (num_pcap_pkts == 0) {
	  beginning.tv_sec = h->ts.tv_sec;
	  beginning.tv_usec = h->ts.tv_usec;
	}

	p = (struct packet *) malloc(sizeof(struct packet));
	if(p) {
          int plen, oplen;

          plen = h->caplen;
          if (datalink == DLT_LINUX_SLL)
            plen -= 2;
          else if (datalink == DLT_RAW)
            plen += 14;

          oplen = plen;

          if (plen < 60)
            plen = 60;

          p->len = plen;

	  if(cidr != NULL)
	    p->iface_index = compute_packet_index(pkt, plen);
	  else
	    p->iface_index = 0;

          p->id = num_pcap_pkts;
	  p->ticks_from_beginning = (((h->ts.tv_sec - beginning.tv_sec) * 1000000) + (h->ts.tv_usec - beginning.tv_usec)) * hz / 1000000;
	  p->next = NULL;
	  p->pkt = (u_char *) calloc(1, plen + 4);

	  if(p->pkt == NULL) {
	    printf("Not enough memory\n");
	    break;
	  } else {
            if (datalink == DLT_LINUX_SLL) {
	      memcpy(p->pkt, pkt, 12);
              memcpy(&p->pkt[12], &pkt[14], oplen - 14);
            } else if (datalink == DLT_RAW) {
              struct ether_header *eth_hdr = (struct ether_header *) p->pkt;
              memcpy(eth_hdr->ether_dhost, dstmac, 6);
              memcpy(eth_hdr->ether_shost, srcmac, 6);
              eth_hdr->ether_type = htons(0x0800) /* IPv4 */;
              memcpy(&p->pkt[14], pkt, oplen - 14);
	    } else {
	      memcpy(p->pkt, pkt, oplen);
            }
	    if(reforge_dst_mac || reforge_src_mac || reforge_ip)
              reforge_packet((u_char *) p->pkt, oplen, ip_offset + num_pcap_pkts, 0); 
	  }

	  if(last) {
	    last->next = p;
	    last = p;
	  } else
	    pkt_head = p, last = p;

          if(verbose)
	    printf("Read %d bytes packet from pcap file %s [%lu.%lu Secs =  %lu ticks@%luhz from beginning]\n",
		   oplen, pcap_in, h->ts.tv_sec - beginning.tv_sec, h->ts.tv_usec - beginning.tv_usec,
		   (long unsigned int)p->ticks_from_beginning,
		   (long unsigned int)hz);

          avg_send_len += plen;
	  num_pcap_pkts++;

          if(num_to_send > 0 && num_pcap_pkts >= num_to_send)
            break;

	} else {
	  printf("Not enough memory\n");
	  break;
	}

      } /* while */

      if (num_pcap_pkts == 0) {
        printf("Pcap file %s is empty\n", pcap_in);
        pfring_close(pd);
	if(twin_pd) pfring_close(twin_pd);
        return(-1);
      }

      avg_send_len /= num_pcap_pkts;

      pcap_close(pt);
      printf("Read %d packets from pcap file %s\n",
	     num_pcap_pkts, pcap_in);
      last->next = pkt_head; /* Loop */
      send_len = avg_send_len;
      num_uniq_pkts = num_pcap_pkts;

      if (send_full_pcap_once)
        num_to_send = num_pcap_pkts;
    } else {
      printf("Unable to open file %s\n", pcap_in);
      pfring_close(pd);
      if(twin_pd) pfring_close(twin_pd);
      return(-1);
    }
  } else {
    struct packet *p = NULL, *last = NULL;

    if ((stdin_packet_len = read_packet_hex(buffer, sizeof(buffer))) > 0) {
      send_len = stdin_packet_len;
    }

    for (i = 0; i < num_uniq_pkts; i++) {

      if (stdin_packet_len <= 0) {
        forge_udp_packet(buffer, send_len, i, (ip_v != 4 && ip_v != 6) ? (i&0x1 ? 6 : 4) : ip_v);
      } else {
        if (reforge_packet(buffer, send_len, i, 0) != 0) { 
          fprintf(stderr, "Unable to reforge the provided packet\n");
          return -1;
        }
      }

      p = (struct packet *) malloc(sizeof(struct packet));
      if (p == NULL) { 
	fprintf(stderr, "Unable to allocate memory requested (%s)\n", strerror(errno));
	return (-1);
      }

      if (i == 0) pkt_head = p;

      p->id = i;
      p->len = send_len;
      p->ticks_from_beginning = 0;
      p->next = pkt_head;
      p->pkt = (u_char *) malloc(p->len);

      if (p->pkt == NULL) {
	fprintf(stderr, "Unable to allocate memory requested (%s)\n", strerror(errno));
	return (-1);
      }

      memcpy(p->pkt, buffer, send_len);

      if(cidr != NULL)
	p->iface_index = compute_packet_index(buffer, send_len);
      else
	p->iface_index = 0;
      
      if (last != NULL) last->next = p;
      last = p;

      if (on_the_fly_reforging) {
#if 0
        if (stdin_packet_len <= 0) { /* forge_udp_packet, parsing packet for on the fly reforing */
          memset(&hdr, 0, sizeof(hdr));
          hdr.len = hdr.caplen = p->len;
          if (pfring_parse_pkt(p->pkt, &hdr, 4, 0, 0) < 3) {
            fprintf(stderr, "Unable to reforge the packet (unexpected)\n");
            return -1; 
          }
        }
#endif
        break;
      }
    }
  }

#if !(defined(__arm__) || defined(__mips__))
  if (gbps < 0)
    pps = -1; /* real pcap rate */

  if (gbps > 0) {
    double kbyte_sec = (gbps * 1000000) /* kbps*/ / 8;
    double td = (double) (hz / kbyte_sec);
    kbyte_ticks = (ticks) td;

    printf("Rate set to %.2f Gbit/s, %.2f kB/s\n", gbps, kbyte_sec);

  } else if (pps > 0 || uniq_pkts_per_sec) {
    double td = (double) (hz / pps);
    inter_frame_ticks = (ticks)td;

    printf("Rate set to %.2f pps\n", pps);
  }
#endif

  if(bind_core >= 0)
    bind2core(bind_core);

  if(wait_for_packet && (cpu_percentage > 0)) {
    if(cpu_percentage > 99) cpu_percentage = 99;
    pfring_config(cpu_percentage);
  }

  gettimeofday(&startTime, NULL);
  memcpy(&lastTime, &startTime, sizeof(startTime));

  pfring_set_socket_mode(pd, send_only_mode);

  if(pfring_enable_ring(pd) != 0) {
    printf("Unable to enable ring :-(\n");
    pfring_close(pd);
    if(twin_pd) pfring_close(twin_pd);
    return(-1);
  } else if(twin_pd) {
    pfring_enable_ring(twin_pd);
  }
  
  if(bpfFilter != NULL) {
    rc = pfring_set_bpf_filter(pd, bpfFilter);
    if(rc != 0)
      fprintf(stderr, "pfring_set_bpf_filter(%s) returned %d\n", bpfFilter, rc);
    else if(twin_pd) {
      rc = pfring_set_bpf_filter(twin_pd, bpfFilter);

      if(rc != 0)
	fprintf(stderr, "pfring_set_bpf_filter(%s) returned %d\n", bpfFilter, rc);
    }      
  }

  tosend = pkt_head;
  i = 0;
  reforging_idx = 0;

  if (uniq_pkts_per_sec) { /* init limit */
    curr_uniq_pkts_limit = uniq_pkts_per_sec;
  } else if (uniq_pkts_per_pkts) {
    curr_uniq_pkts_limit = 1;
  }

  pfring_set_application_stats(pd, "Statistics not yet computed: please try again...");
  if(pfring_get_appl_stats_file_name(pd, path, sizeof(path)) != NULL)
    fprintf(stderr, "Dumping statistics on %s\n", path);

#if !(defined(__arm__) || defined(__mips__))
  if(pps != 0 || gbps != 0 || uniq_pkts_per_sec)
    tick_start = tick_prev = getticks();

  if (pps < 0) /* flush for sending at the exact original pcap speed only, otherwise let pf_ring flush when needed) */
    flush = 1;
#endif

  if (randomize) {
    if(reforge_ip == 0) {
      randomize = 0;
      fprintf(stderr, "WARNING: -z requires you to use -b: ignored\n");
    } else {
      if(!on_the_fly_reforging)
	randomize_packets();
    }
  }
  
  if(!verbose) {
    signal(SIGALRM, my_sigalarm);
    alarm(1);
  }

  while((num_to_send == 0) 
	|| (i < num_to_send)) {
    int rc;

  redo:

    if (unlikely(do_shutdown)) 
      break;

    if (on_the_fly_reforging) {
      if (stdin_packet_len <= 0)
        forge_udp_packet(tosend->pkt, tosend->len, reforging_idx + on_the_fly_sent, (ip_v != 4 && ip_v != 6) ? (i&0x1 ? 6 : 4) : ip_v);
      else
        reforge_packet(tosend->pkt, tosend->len, reforging_idx + on_the_fly_sent, 1); 
    }

    rc = pfring_send((tosend->iface_index == 0) ? pd : twin_pd, (char *) tosend->pkt, tosend->len, flush);

    if (unlikely(verbose))
      printf("[%d] pfring_send(%d) returned %d\n", i, tosend->len, rc);

    if (likely(rc >= 0)) {
      num_pkt_good_sent++;
      num_bytes_good_sent += tosend->len + 24 /* 8 Preamble + 4 CRC + 12 IFG */;
    } else if (rc == PF_RING_ERROR_INVALID_ARGUMENT) {
      if (send_error_once) {
        printf("Attempting to send invalid packet [len: %u][MTU: %u]\n",
	       tosend->len, pfring_get_mtu_size(pd));
        send_error_once = 0;
      }
    } else if (rc == PF_RING_ERROR_NOT_SUPPORTED) {
      printf("Transmission is not supporte on the selected interface\n");
      goto close_socket;
    } else /* Other rc < 0 */ {
      /* Not enough space in buffer */
      if(!active_poll)
	usleep(1);
      goto redo;
    }

    if (on_the_fly_reforging) {
      on_the_fly_sent++;
      if (randomize) {
        n = random() & 0xF;
        reforging_idx += n;
      }
    }

    if (pkt_loop && ++pkt_loop_sent < pkt_loop) {
      pkt_loop_sent++;
      /* send the same packet again */
    } else {
      if (pkt_loop) pkt_loop_sent = 0;
      /* move to the next packet */
      tosend = tosend->next;
    }

#if !(defined(__arm__) || defined(__mips__))
    if (gbps > 0) {
      int tx_syncronized = 0;
      /* gbps rate set */
      while((getticks() - tick_start) < ((num_bytes_good_sent >> 10 /* /1024 */) * kbyte_ticks)) {
        if (!tx_syncronized) {
          pfring_flush_tx_packets(pd);
          tx_syncronized = 1;
        }
        if (unlikely(do_shutdown)) break;
      }
    } else if(pps > 0) {
      int tx_syncronized = 0;
      /* pps or gbps rate set */
      while((getticks() - tick_start) < (num_pkt_good_sent * inter_frame_ticks)) {
        if (!tx_syncronized) {
          pfring_flush_tx_packets(pd);
          tx_syncronized = 1;
        }
        if (unlikely(do_shutdown)) break;
      }
    } else if (pps < 0) {
      int tx_syncronized = 0;
      /* real pcap rate */
      if (tosend->ticks_from_beginning == 0)
        tick_start = getticks(); /* first packet, resetting time */
      while((getticks() - tick_start) < tosend->ticks_from_beginning) {
        if (!tx_syncronized) {
          pfring_flush_tx_packets(pd);
          tx_syncronized = 1;
        }
        if (unlikely(do_shutdown)) break;
      }
    }

    if (uniq_pkts_per_sec || uniq_pkts_per_pkts) {

      if (curr_uniq_pkts_limit < num_uniq_pkts) {
        if (uniq_pkts_per_sec) {
          /* add N uniq packets per second */
          if (getticks() - tick_prev > hz) {
            /* 1s elapsed, add N uniq packets */
            curr_uniq_pkts_limit += uniq_pkts_per_sec;
            tick_prev = getticks();
          }
        } else if (uniq_pkts_per_pkts) {
          /* add K more uniq packet every N packets */
          uniq_pkts_per_pkts_cnt++;
          if (uniq_pkts_per_pkts_cnt >= uniq_pkts_per_pkts) {
            uniq_pkts_per_pkts_cnt = 0;
            curr_uniq_pkts_limit += uniq_k_pkts_per_pkts;
          }
        }
      }

      /* check the uniq packets limit */
      if (tosend->id >= curr_uniq_pkts_limit)
        tosend = pkt_head;

      if (on_the_fly_reforging) {
        if (on_the_fly_sent >= curr_uniq_pkts_limit)
          on_the_fly_sent = 0;
      }
    }

#endif

    if(num_to_send > 0) i++;
  } /* for */

  pfring_flush_tx_packets(pd);

  print_stats();
  printf("Sent %llu packets\n", (long long unsigned int) num_pkt_good_sent);

 close_socket:
  pfring_close(pd);
  if(twin_pd) pfring_close(twin_pd);
  if(patricia_v4) patricia_destroy(patricia_v4, NULL);
  
  if (pidFileName)
    remove_pid_file(pidFileName);

  return(0);
}

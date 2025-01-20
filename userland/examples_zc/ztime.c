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
#include <sys/time.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>

#include "pfring.h"
#include "pfring_zc.h"

#include "zutils.c"

#define MAX_CARD_SLOTS      32768

pfring_zc_cluster *zc = NULL;
pfring_zc_queue *zq = NULL;
pfring_zc_pkt_buff *buffer = NULL;

u_int8_t verbose = 0;

/* *************************************** */

void printHelp(void) {
  printf("ztime - (C) 2014-24 ntop\n");
  printf("Using PFRING_ZC v.%s\n", pfring_zc_version());
  printf("An utility to set/adjust the hardware clock of a network adapter (when supported).\n\n");
  printf("Usage:   ztime -i <device> [-c <cluster id>]\n"
	 "                [-h] [-v] [-a] [-t]\n\n");
  printf("-h              Print this help\n");
  printf("-i <device>     Device name\n");
  printf("-c <cluster id> Cluster id\n");
  printf("-s <time>       Set hardware timestamp (when supported). Format example: '2022-09-23 14:30:55.123456789'\n");
  printf("-d <nsec>       Adjust hardware timestamp using a signed nsec delta (when supported)'\n");
  printf("-t              Send a test packet and get the transmission time\n");
  printf("-v              Verbose\n");
}

/* *************************************** */

void send_packet() {
  u_char *data = pfring_zc_pkt_buff_data(buffer, zq);
  struct timespec ts = { 0 };
  int packet_len = 60;
  int rc;

  buffer->len = packet_len;
  forge_udp_packet_fast(data, packet_len, 0);

  rc = pfring_zc_send_pkt_get_time(zq, &buffer, 1, &ts);

  if (rc) {
    fprintf(stderr, "Packet successfully sent at %ld.%ld\n", ts.tv_sec, ts.tv_nsec);
  } else {
    fprintf(stderr, "Failure sending packet\n");
  }
}

/* *************************************** */

int main(int argc, char* argv[]) {
  char *device = NULL, c;
  int cluster_id = DEFAULT_CLUSTER_ID + 12;
  int rc = 0;
  u_int32_t flags = PF_RING_ZC_DEVICE_HW_TIMESTAMP;
  char *init_time = NULL;
  long long shift_time = 0;
  int test_send_get_time = 0;
  int read_time = 1;

  while((c = getopt(argc,argv,"c:d:hi:vDs:t")) != '?') {
    if(((unsigned char)c == 255) || (c == -1)) break;

    switch(c) {
    case 'h':
      printHelp();
      exit(0);
      break;
    case 'c':
      cluster_id = atoi(optarg);
      break;
    case 'd':
      shift_time = atoll(optarg);
      break;
    case 'i':
      device = strdup(optarg);
      break;
    case 's':
      init_time = strdup(optarg);
      break;
    case 't':
      test_send_get_time = 1;
      break;
    case 'v':
      verbose = 1;
      break;
    case 'D':
      pfring_zc_debug();
      break;
    }
  }
  
  if (device == NULL || cluster_id < 0) {
    printHelp();
    exit(-1);
  }

  zc = pfring_zc_create_cluster(
    cluster_id, 
    1536,
    0, 
    (test_send_get_time ? MAX_CARD_SLOTS : 0) + 1,
    -1,
    NULL /* auto hugetlb mountpoint */,
    0 
  );

  if(zc == NULL) {
    fprintf(stderr, "pfring_zc_create_cluster error [%s] Please check that pf_ring.ko is loaded and hugetlb fs is mounted\n",
	    strerror(errno));
    return -1;
  }

  buffer = pfring_zc_get_packet_handle(zc);

  if (buffer == NULL) {
    fprintf(stderr, "pfring_zc_get_packet_handle error\n");
    return -1;
  }

  zq = pfring_zc_open_device(zc, device, test_send_get_time ? tx_only : management_only, flags);

  if(zq == NULL) {
    fprintf(stderr, "pfring_zc_open_device error [%s] Please check that %s is up and not already used\n",
	    strerror(errno), device);
    rc = -1;
    goto cleanup;
  }

  if (init_time) {
    int rc;
    struct timespec ts;

    rc = str2nsec(init_time, &ts);

    if (rc == 0)
      rc = pfring_zc_set_device_clock(zq, &ts);

    if (rc == 0) printf("Device clock correctly initialized\n");
    else printf("Unable to set device clock (%u)\n", rc);
  }

  if (shift_time) {
    int rc;
    struct timespec ts;
    int sign = 0;

    if (shift_time < 0) {
      sign = -1;
      shift_time = -shift_time;
    }

    ts.tv_sec  = shift_time / 1000000000;
    ts.tv_nsec = shift_time % 1000000000;

    rc = pfring_zc_adjust_device_clock(zq, &ts, sign);

    if (rc == 0) printf("Device clock adjusted (%s %ld.%ld)\n", sign ? "-" : "+", ts.tv_sec, ts.tv_nsec);
    else printf("Unable to adjust device clock (%u)\n", rc);
  }

  if (read_time) {
    struct timespec ts;

    rc = pfring_zc_get_device_clock(zq, &ts);

    printf("Device clock is %ld.%ld\n", ts.tv_sec, ts.tv_nsec);
  }

  if (test_send_get_time) {
    send_packet();
  }

  sleep(1);

cleanup:

  pfring_zc_destroy_cluster(zc);

  return rc;
}


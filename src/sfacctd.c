/*  
    pmacct (Promiscuous mode IP Accounting package)
    pmacct is Copyright (C) 2003-2006 by Paolo Lucente
*/

/*
    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/

/* 
    sflow v2/v4/v5 routines are based on sFlow toolkit 3.8 which is 
    Copyright (C) InMon Corporation 2001 ALL RIGHTS RESERVED
*/

/* defines */
#define __SFACCTD_C

/* includes */
#include "pmacct.h"
#include "sflow.h"
#include "sfacctd.h"
#include "pretag_handlers.h"
#include "pretag-data.h"
#include "pmacct-data.h"
#include "plugin_hooks.h"
#include "pkt_handlers.h"
#include "ip_flow.h"
#include "classifier.h"
#include "net_aggr.h"

/* variables to be exported away */
int debug;
struct configuration config; /* global configuration */ 
struct plugins_list_entry *plugins_list = NULL; /* linked list of each plugin configuration */ 
struct channels_list_entry channels_list[MAX_N_PLUGINS]; /* communication channels: core <-> plugins */
int have_num_memory_pools; /* global getopt() stuff */
pid_t failed_plugins[MAX_N_PLUGINS]; /* plugins failed during startup phase */

/* Functions */
void usage_daemon(char *prog_name)
{
  printf("%s\n", SFACCTD_USAGE_HEADER);
  printf("Usage: %s [ -D | -d ] [ -L IP address ] [ -l port ] [ -c primitive [ , ... ] ] [ -P plugin [ , ... ] ]\n", prog_name);
  printf("       %s [ -f config_file ]\n", prog_name);
  printf("       %s [ -h ]\n", prog_name);
  printf("\nGeneral options:\n");
  printf("  -h  \tShow this page\n");
  printf("  -L  \tBind to the specified IP address\n");
  printf("  -l  \tListen on the specified UDP port\n");
  printf("  -f  \tLoad configuration from the specified file\n");
  printf("  -c  \t[ src_mac | dst_mac | vlan | src_host | dst_host | src_net | dst_net | src_port | dst_port |\n\t tos | proto | src_as | dst_as | sum_mac | sum_host | sum_net | sum_as | sum_port | tag |\n\t flows | class | none] \n\tAggregation string (DEFAULT: src_host)\n");
  printf("  -D  \tDaemonize\n"); 
  printf("  -n  \tPath to a file containing Network definitions\n");
  printf("  -o  \tPath to a file containing Port definitions\n");
  printf("  -P  \t[ memory | print | mysql | pgsql | sqlite3 ] \n\tActivate plugin\n"); 
  printf("  -d  \tEnable debug\n");
  printf("  -S  \t[ auth | mail | daemon | kern | user | local[0-7] ] \n\ttLog to the specified syslog facility\n");
  printf("  -F  \tWrite Core Process PID into the specified file\n");
  printf("  -R  \tRenormalize counters using informations into the sFlow datagram headers\n");
  printf("\nMemory Plugin (-P memory) options:\n");
  printf("  -p  \tSocket for client-server communication (DEFAULT: /tmp/collect.pipe)\n");
  printf("  -b  \tNumber of buckets\n");
  printf("  -m  \tNumber of memory pools\n");
  printf("  -s  \tMemory pool size\n");
  printf("\nPostgreSQL (-P pgsql)/MySQL (-P mysql)/SQLite (-P sqlite3) plugin options:\n");
  printf("  -r  \tRefresh time (in seconds)\n");
  printf("  -v  \t[ 1 | 2 | 3 | 4 | 5 ] \n\tTable version\n");
  printf("\n");
  printf("Examples:\n");
  printf("  Daemonize the process and write data into a MySQL database\n");
  printf("  sfacctd -c src_host,dst_host -D -P mysql\n\n");
  printf("  Print flows over the screen and refresh data every 30 seconds\n");
  printf("  sfacctd -c src_host,dst_host,proto -P print -r 30\n");
  printf("\n");
  printf("  See EXAMPLES for further hints\n");
  printf("\n");
  printf("For suggestions, critics, bugs, contact me: %s.\n", MANTAINER);
}


int main(int argc,char **argv, char **envp)
{
  struct plugins_list_entry *list;
  struct plugin_requests req;
  struct packet_ptrs_vector pptrs;
  char config_file[SRVBUFLEN];
  unsigned char sflow_packet[SFLOW_MAX_MSG_SIZE];
  int logf, sd, rc, yes=1, allowed;
  struct host_addr addr;
  struct hosts_table allow;
  struct id_table idt;
  u_int32_t idx;
  u_int16_t ret;
  SFSample spp;

#if defined ENABLE_IPV6
  struct sockaddr_storage server, client;
  struct ipv6_mreq multi_req6;
#else
  struct sockaddr server, client;
#endif
  int clen = sizeof(client), slen;
  struct ip_mreq multi_req4;

  unsigned char dummy_packet[64]; 
  unsigned char dummy_packet_vlan[64]; 
  unsigned char dummy_packet_mpls[128]; 
  unsigned char dummy_packet_vlan_mpls[128]; 
  struct pcap_pkthdr dummy_pkthdr;
  struct pcap_pkthdr dummy_pkthdr_vlan;
  struct pcap_pkthdr dummy_pkthdr_mpls;
  struct pcap_pkthdr dummy_pkthdr_vlan_mpls;

#if defined ENABLE_IPV6
  unsigned char dummy_packet6[92]; 
  unsigned char dummy_packet_vlan6[92]; 
  unsigned char dummy_packet_mpls6[128]; 
  unsigned char dummy_packet_vlan_mpls6[128]; 
  struct pcap_pkthdr dummy_pkthdr6;
  struct pcap_pkthdr dummy_pkthdr_vlan6;
  struct pcap_pkthdr dummy_pkthdr_mpls6;
  struct pcap_pkthdr dummy_pkthdr_vlan_mpls6;
#endif

  /* getopt() stuff */
  extern char *optarg;
  extern int optind, opterr, optopt;
  int errflag, cp; 

  umask(077);
  compute_once();

  /* a bunch of default definitions */ 
  have_num_memory_pools = FALSE;
  reload_map = FALSE;
  renorm_table_entries = 0;
  errflag = 0;

  memset(cfg_cmdline, 0, sizeof(cfg_cmdline));
  memset(&server, 0, sizeof(server));
  memset(&config, 0, sizeof(struct configuration));
  memset(&config_file, 0, sizeof(config_file));
  memset(&failed_plugins, 0, sizeof(failed_plugins));
  memset(&pptrs, 0, sizeof(pptrs));
  memset(&req, 0, sizeof(req));
  memset(&spp, 0, sizeof(spp));
  memset(&renorm_table, 0, sizeof(renorm_table));
  memset(&class, 0, sizeof(class));
  config.acct_type = ACCT_SF;

  rows = 0;

  /* getting commandline values */
  while (!errflag && ((cp = getopt(argc, argv, ARGS_SFACCTD)) != -1)) {
    cfg_cmdline[rows] = malloc(SRVBUFLEN);
    switch (cp) {
    case 'L':
      strcpy(cfg_cmdline[rows], "sfacctd_ip: ");
      strncat(cfg_cmdline[rows], optarg, CFG_LINE_LEN(cfg_cmdline[rows]));
      rows++;
      break;
    case 'l':
      strcpy(cfg_cmdline[rows], "sfacctd_port: ");
      strncat(cfg_cmdline[rows], optarg, CFG_LINE_LEN(cfg_cmdline[rows]));
      rows++;
      break;
    case 'P':
      strcpy(cfg_cmdline[rows], "plugins: ");
      strncat(cfg_cmdline[rows], optarg, CFG_LINE_LEN(cfg_cmdline[rows]));
      rows++;
      break;
    case 'D':
      strcpy(cfg_cmdline[rows], "daemonize: true");
      rows++;
      break;
    case 'd':
      debug = TRUE;
      strcpy(cfg_cmdline[rows], "debug: true");
      rows++;
      break;
    case 'n':
      strcpy(cfg_cmdline[rows], "networks_file: ");
      strncat(cfg_cmdline[rows], optarg, CFG_LINE_LEN(cfg_cmdline[rows]));
      rows++;
      break;
    case 'o':
      strcpy(cfg_cmdline[rows], "ports_file: ");
      strncat(cfg_cmdline[rows], optarg, CFG_LINE_LEN(cfg_cmdline[rows]));
      rows++;
      break;
    case 'f':
      strlcpy(config_file, optarg, sizeof(config_file));
      break;
    case 'F':
      strcpy(cfg_cmdline[rows], "pidfile: ");
      strncat(cfg_cmdline[rows], optarg, CFG_LINE_LEN(cfg_cmdline[rows]));
      rows++;
      break;
    case 'c':
      strcpy(cfg_cmdline[rows], "aggregate: ");
      strncat(cfg_cmdline[rows], optarg, CFG_LINE_LEN(cfg_cmdline[rows]));
      rows++;
      break;
    case 'b':
      strcpy(cfg_cmdline[rows], "imt_buckets: ");
      strncat(cfg_cmdline[rows], optarg, CFG_LINE_LEN(cfg_cmdline[rows]));
      rows++;
      break;
    case 'm':
      strcpy(cfg_cmdline[rows], "imt_mem_pools_number: ");
      strncat(cfg_cmdline[rows], optarg, CFG_LINE_LEN(cfg_cmdline[rows]));
      have_num_memory_pools = TRUE;
      rows++;
      break;
    case 'p':
      strcpy(cfg_cmdline[rows], "imt_path: ");
      strncat(cfg_cmdline[rows], optarg, CFG_LINE_LEN(cfg_cmdline[rows]));
      rows++;
      break;
    case 'r':
      strcpy(cfg_cmdline[rows], "sql_refresh_time: ");
      strncat(cfg_cmdline[rows], optarg, CFG_LINE_LEN(cfg_cmdline[rows]));
      rows++;
      cfg_cmdline[rows] = malloc(SRVBUFLEN);
      strcpy(cfg_cmdline[rows], "print_refresh_time: ");
      strncat(cfg_cmdline[rows], optarg, CFG_LINE_LEN(cfg_cmdline[rows]));
      rows++;
      break;
    case 'v':
      strcpy(cfg_cmdline[rows], "sql_table_version: ");
      strncat(cfg_cmdline[rows], optarg, CFG_LINE_LEN(cfg_cmdline[rows]));
      rows++;
      break;
    case 's':
      strcpy(cfg_cmdline[rows], "imt_mem_pools_size: ");
      strncat(cfg_cmdline[rows], optarg, CFG_LINE_LEN(cfg_cmdline[rows]));
      rows++;
      break;
    case 'S':
      strcpy(cfg_cmdline[rows], "syslog: ");
      strncat(cfg_cmdline[rows], optarg, CFG_LINE_LEN(cfg_cmdline[rows]));
      rows++;
      break;
    case 'R':
      strcpy(cfg_cmdline[rows], "sfacctd_renormalize: true");
      rows++;
      break;
    case 'h':
      usage_daemon(argv[0]);
      exit(0);
      break;
    default:
      usage_daemon(argv[0]);
      exit(1);
      break;
    }
  }

  /* post-checks and resolving conflicts */
  if (strlen(config_file)) {
    if (parse_configuration_file(config_file) != SUCCESS) 
      exit(1);
  }
  else {
    if (parse_configuration_file(NULL) != SUCCESS)
      exit(1);
  }
    
  /* XXX: glue; i'm conscious it's a dirty solution from an engineering viewpoint;
     someday later i'll fix this */
  list = plugins_list;
  while(list) {
    list->cfg.acct_type = ACCT_SF;
    if (!strcmp(list->name, "default") && !strcmp(list->type.string, "core")) 
      memcpy(&config, &list->cfg, sizeof(struct configuration)); 
    list = list->next;
  }

  if (config.daemon) {
    list = plugins_list;
    while (list) {
      if (!strcmp(list->type.string, "print")) printf("WARN: Daemonizing. Hmm, bye bye screen.\n");
      list = list->next;
    }
    if (debug || config.debug)
      printf("WARN: debug is enabled; forking in background. Console logging will get lost.\n"); 
    daemonize();
  }

  initsetproctitle(argc, argv, envp);
  if (config.syslog) {
    logf = parse_log_facility(config.syslog);
    if (logf == ERR) {
      config.syslog = NULL;
      Log(LOG_WARNING, "WARN ( default/core ): specified syslog facility is not supported; logging to console.\n");
    }
    else openlog(NULL, LOG_PID, logf);
    Log(LOG_INFO, "INFO ( default/core ): Start logging ...\n");
  }

  /* Enforcing policies over aggregation methods */
  list = plugins_list;
  while (list) {
    if (strcmp(list->type.string, "core")) {  
      evaluate_sums(&list->cfg.what_to_count, list->name, list->type.string);
      if (!list->cfg.what_to_count) {
	Log(LOG_WARNING, "WARN ( %s/%s ): defaulting to SRC HOST aggregation.\n", list->name, list->type.string);
	list->cfg.what_to_count |= COUNT_SRC_HOST;
      }
      if ((list->cfg.what_to_count & (COUNT_SRC_AS|COUNT_DST_AS|COUNT_SUM_AS)) && !list->cfg.networks_file && list->cfg.nfacctd_as != NF_AS_KEEP) {
        Log(LOG_ERR, "ERROR ( %s/%s ): AS aggregation has been selected but NO 'networks_file' has been specified. Exiting...\n\n", list->name, list->type.string);
        exit(1);
      }
      if ((list->cfg.what_to_count & (COUNT_SRC_NET|COUNT_DST_NET|COUNT_SUM_NET)) && !list->cfg.networks_file && !list->cfg.networks_mask) {
        Log(LOG_ERR, "ERROR ( %s/%s ): NET aggregation has been selected but NO 'networks_file' has been specified. Exiting...\n\n", list->name, list->type.string);
        exit(1);
      }
      if (((list->cfg.what_to_count & (COUNT_SRC_NET|COUNT_SUM_NET)) && (list->cfg.what_to_count & (COUNT_SRC_AS|COUNT_SUM_AS))) ||
          ((list->cfg.what_to_count & COUNT_DST_NET) && (list->cfg.what_to_count & COUNT_DST_AS))) {
        Log(LOG_ERR, "ERROR ( %s/%s ): NET/AS are mutually exclusive. Exiting...\n\n", list->name, list->type.string);
        exit(1);
      }
    } 
    list = list->next;
  }

  /* signal handling we want to inherit to plugins (when not re-defined elsewhere) */
  signal(SIGCHLD, startup_handle_falling_child); /* takes note of plugins failed during startup phase */
  signal(SIGHUP, reload); /* handles reopening of syslog channel */
  signal(SIGUSR1, SIG_IGN); /* ignore this signal */ 
  signal(SIGUSR2, reload_maps); /* sets to true the reload_maps flag */
  signal(SIGPIPE, SIG_IGN); /* we want to exit gracefully when a pipe is broken */

  /* If no IP address is supplied, let's set our default
     behaviour: IPv4 address, INADDR_ANY, port 2100 */
  if (!config.nfacctd_port) config.nfacctd_port = DEFAULT_SFACCTD_PORT;
#if (defined ENABLE_IPV6 && defined V4_MAPPED)
  if (!config.nfacctd_ip) {
    struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)&server;

    sa6->sin6_family = AF_INET6;
    sa6->sin6_port = htons(config.nfacctd_port);
    slen = sizeof(struct sockaddr_in6);
  }
#else
  if (!config.nfacctd_ip) {
    struct sockaddr_in *sa4 = (struct sockaddr_in *)&server;

    sa4->sin_family = AF_INET;
    sa4->sin_addr.s_addr = htonl(0);
    sa4->sin_port = htons(config.nfacctd_port);
    slen = sizeof(struct sockaddr_in);
  }
#endif
  else {
    trim_spaces(config.nfacctd_ip);
    ret = str_to_addr(config.nfacctd_ip, &addr);
    if (!ret) {
      Log(LOG_ERR, "ERROR ( default/core ): 'sfacctd_ip' value is not valid. Exiting.\n");
      exit(1);
    }
    slen = addr_to_sa((struct sockaddr *)&server, &addr, config.nfacctd_port);
  }

  /* socket creation */
  sd = socket(((struct sockaddr *)&server)->sa_family, SOCK_DGRAM, 0);
  if (sd < 0) {
    Log(LOG_ERR, "ERROR ( default/core ): socket() failed.\n");
    exit(1);
  }

  /* bind socket to port */
  rc = Setsocksize(sd, SOL_SOCKET, SO_REUSEADDR, (char *)&yes, sizeof(yes));
  if (rc < 0) Log(LOG_ERR, "WARN ( default/core ): Setsocksize() failed for SO_REUSEADDR.\n");

  if (config.pipe_size) {
    rc = Setsocksize(sd, SOL_SOCKET, SO_RCVBUF, &config.pipe_size, sizeof(config.pipe_size));
    if (rc < 0) Log(LOG_ERR, "WARN ( default/core ): Setsocksize() failed for 'plugin_pipe_size' = '%d'.\n", config.pipe_size); 
  }

  rc = bind(sd, (struct sockaddr *) &server, slen);
  if (rc < 0) {
    Log(LOG_ERR, "ERROR ( default/core): bind() to ip=%s port=%d/udp failed (errno: %d).\n", config.nfacctd_ip, config.nfacctd_port, errno);
    exit(1);
  }

  /* Multicast: memberships handling */
  for (idx = 0; mcast_groups[idx].family && idx < MAX_MCAST_GROUPS; idx++) {
    if (mcast_groups[idx].family == AF_INET) {
      memset(&multi_req4, 0, sizeof(multi_req4));
      multi_req4.imr_multiaddr.s_addr = mcast_groups[idx].address.ipv4.s_addr;
      if (setsockopt(sd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char *)&multi_req4, sizeof(multi_req4)) < 0) {
	Log(LOG_ERR, "ERROR: IPv4 multicast address - ADD membership failed.\n");
	exit(1);
      }
    }
#if defined ENABLE_IPV6
    if (mcast_groups[idx].family == AF_INET6) {
      memset(&multi_req6, 0, sizeof(multi_req6));
      ip6_addr_cpy(&multi_req6.ipv6mr_multiaddr, &mcast_groups[idx].address.ipv6);
      if (setsockopt(sd, IPPROTO_IPV6, IPV6_JOIN_GROUP, (char *)&multi_req6, sizeof(multi_req6)) < 0) {
	Log(LOG_ERR, "ERROR: IPv6 multicast address - ADD membership failed.\n");
	exit(1);
      }
    }
#endif
  }

  if (config.nfacctd_allow_file) load_allow_file(config.nfacctd_allow_file, &allow);
  else memset(&allow, 0, sizeof(allow));

  if (config.pre_tag_map) {
    load_id_file(config.acct_type, config.pre_tag_map, &idt, &req);
    pptrs.v4.idtable = (u_char *) &idt;
  }
  else {
    memset(&idt, 0, sizeof(idt));
    pptrs.v4.idtable = NULL;
  }

  /* plugins glue: creation */
  load_plugins(&req);
  evaluate_packet_handlers();
  pm_setproctitle("%s [%s]", "Core Process", "default");
  if (config.pidfile) write_pid_file(config.pidfile);
  load_networks(config.networks_file, &nt, &nc);

  /* signals to be handled only by pmacctd;
     we set proper handlers after plugin creation */
  signal(SIGINT, my_sigint_handler);
  signal(SIGTERM, my_sigint_handler);
  signal(SIGCHLD, handle_falling_child);
  kill(getpid(), SIGCHLD);

  /* arranging pointers to dummy packet; to speed up things into the
     main loop we mantain two packet_ptrs structures when IPv6 is enabled:
     we will sync here 'pptrs6' for common tables and pointers */
  memset(dummy_packet, 0, sizeof(dummy_packet));
  pptrs.v4.f_data = (u_char *) &spp;
  pptrs.v4.f_agent = (u_char *) &client;
  pptrs.v4.packet_ptr = dummy_packet;
  pptrs.v4.pkthdr = &dummy_pkthdr;
  Assign16(((struct eth_header *)pptrs.v4.packet_ptr)->ether_type, htons(ETHERTYPE_IP)); /* 0x800 */
  pptrs.v4.mac_ptr = (u_char *)((struct eth_header *)pptrs.v4.packet_ptr)->ether_dhost; 
  pptrs.v4.iph_ptr = pptrs.v4.packet_ptr + ETHER_HDRLEN; 
  pptrs.v4.tlh_ptr = pptrs.v4.packet_ptr + ETHER_HDRLEN + sizeof(struct my_iphdr); 
  Assign8(((struct my_iphdr *)pptrs.v4.iph_ptr)->ip_vhl, 5);
  pptrs.v4.pkthdr->caplen = 38; /* eth_header + my_iphdr + my_tlhdr */
  pptrs.v4.pkthdr->len = 100; /* fake len */ 
  pptrs.v4.l3_proto = ETHERTYPE_IP;

  memset(dummy_packet_vlan, 0, sizeof(dummy_packet_vlan));
  pptrs.vlan4.f_data = pptrs.v4.f_data; 
  pptrs.vlan4.idtable = pptrs.v4.idtable; 
  pptrs.vlan4.f_agent = (u_char *) &client;
  pptrs.vlan4.packet_ptr = dummy_packet_vlan;
  pptrs.vlan4.pkthdr = &dummy_pkthdr_vlan;
  Assign16(((struct eth_header *)pptrs.vlan4.packet_ptr)->ether_type, htons(ETHERTYPE_8021Q));
  pptrs.vlan4.mac_ptr = (u_char *)((struct eth_header *)pptrs.vlan4.packet_ptr)->ether_dhost;
  pptrs.vlan4.vlan_ptr = pptrs.vlan4.packet_ptr + ETHER_HDRLEN;
  Assign16(*(pptrs.vlan4.vlan_ptr+2), htons(ETHERTYPE_IP));
  pptrs.vlan4.iph_ptr = pptrs.vlan4.packet_ptr + ETHER_HDRLEN + IEEE8021Q_TAGLEN;
  pptrs.vlan4.tlh_ptr = pptrs.vlan4.packet_ptr + ETHER_HDRLEN + IEEE8021Q_TAGLEN + sizeof(struct my_iphdr);
  Assign8(((struct my_iphdr *)pptrs.vlan4.iph_ptr)->ip_vhl, 5);
  pptrs.vlan4.pkthdr->caplen = 42; /* eth_header + vlan + my_iphdr + my_tlhdr */
  pptrs.vlan4.pkthdr->len = 100; /* fake len */
  pptrs.vlan4.l3_proto = ETHERTYPE_IP;

  memset(dummy_packet_mpls, 0, sizeof(dummy_packet_mpls));
  pptrs.mpls4.f_data = pptrs.v4.f_data; 
  pptrs.mpls4.idtable = pptrs.v4.idtable;
  pptrs.mpls4.f_agent = (u_char *) &client;
  pptrs.mpls4.packet_ptr = dummy_packet_mpls;
  pptrs.mpls4.pkthdr = &dummy_pkthdr_mpls;
  Assign16(((struct eth_header *)pptrs.mpls4.packet_ptr)->ether_type, htons(ETHERTYPE_MPLS));
  pptrs.mpls4.mac_ptr = (u_char *)((struct eth_header *)pptrs.mpls4.packet_ptr)->ether_dhost;
  pptrs.mpls4.mpls_ptr = pptrs.mpls4.packet_ptr + ETHER_HDRLEN;
  pptrs.mpls4.pkthdr->caplen = 78; /* eth_header + upto 10 MPLS labels + my_iphdr + my_tlhdr */
  pptrs.mpls4.pkthdr->len = 100; /* fake len */
  pptrs.mpls4.l3_proto = ETHERTYPE_IP;

  memset(dummy_packet_vlan_mpls, 0, sizeof(dummy_packet_vlan_mpls));
  pptrs.vlanmpls4.f_data = pptrs.v4.f_data; 
  pptrs.vlanmpls4.idtable = pptrs.v4.idtable;
  pptrs.vlanmpls4.f_agent = (u_char *) &client;
  pptrs.vlanmpls4.packet_ptr = dummy_packet_vlan_mpls;
  pptrs.vlanmpls4.pkthdr = &dummy_pkthdr_vlan_mpls;
  Assign16(((struct eth_header *)pptrs.vlanmpls4.packet_ptr)->ether_type, htons(ETHERTYPE_8021Q));
  pptrs.vlanmpls4.mac_ptr = (u_char *)((struct eth_header *)pptrs.vlanmpls4.packet_ptr)->ether_dhost;
  pptrs.vlanmpls4.vlan_ptr = pptrs.vlanmpls4.packet_ptr + ETHER_HDRLEN;
  Assign16(*(pptrs.vlanmpls4.vlan_ptr+2), htons(ETHERTYPE_MPLS));
  pptrs.vlanmpls4.mpls_ptr = pptrs.vlanmpls4.packet_ptr + ETHER_HDRLEN + IEEE8021Q_TAGLEN;
  pptrs.vlanmpls4.pkthdr->caplen = 82; /* eth_header + vlan + upto 10 MPLS labels + my_iphdr + my_tlhdr */
  pptrs.vlanmpls4.pkthdr->len = 100; /* fake len */
  pptrs.vlanmpls4.l3_proto = ETHERTYPE_IP;

#if defined ENABLE_IPV6
  memset(dummy_packet6, 0, sizeof(dummy_packet6));
  pptrs.v6.f_data = pptrs.v4.f_data; 
  pptrs.v6.idtable = pptrs.v4.idtable;
  pptrs.v6.f_agent = (u_char *) &client;
  pptrs.v6.packet_ptr = dummy_packet6;
  pptrs.v6.pkthdr = &dummy_pkthdr6;
  Assign16(((struct eth_header *)pptrs.v6.packet_ptr)->ether_type, htons(ETHERTYPE_IPV6)); 
  pptrs.v6.mac_ptr = (u_char *)((struct eth_header *)pptrs.v6.packet_ptr)->ether_dhost; 
  pptrs.v6.iph_ptr = pptrs.v6.packet_ptr + ETHER_HDRLEN;
  pptrs.v6.tlh_ptr = pptrs.v6.packet_ptr + ETHER_HDRLEN + sizeof(struct ip6_hdr);
  Assign16(((struct ip6_hdr *)pptrs.v6.iph_ptr)->ip6_plen, htons(100));
  Assign16(((struct ip6_hdr *)pptrs.v6.iph_ptr)->ip6_hlim, htons(64));
  pptrs.v6.pkthdr->caplen = 60; /* eth_header + ip6_hdr + my_tlhdr */
  pptrs.v6.pkthdr->len = 100; /* fake len */
  pptrs.v6.l3_proto = ETHERTYPE_IPV6;

  memset(dummy_packet_vlan6, 0, sizeof(dummy_packet_vlan6));
  pptrs.vlan6.f_data = pptrs.v4.f_data; 
  pptrs.vlan6.idtable = pptrs.v4.idtable;
  pptrs.vlan6.f_agent = (u_char *) &client;
  pptrs.vlan6.packet_ptr = dummy_packet_vlan6;
  pptrs.vlan6.pkthdr = &dummy_pkthdr_vlan6;
  Assign16(((struct eth_header *)pptrs.vlan6.packet_ptr)->ether_type, htons(ETHERTYPE_8021Q));
  pptrs.vlan6.mac_ptr = (u_char *)((struct eth_header *)pptrs.vlan6.packet_ptr)->ether_dhost;
  pptrs.vlan6.vlan_ptr = pptrs.vlan6.packet_ptr + ETHER_HDRLEN;
  Assign16(*(pptrs.vlan6.vlan_ptr+2), htons(ETHERTYPE_IPV6));
  pptrs.vlan6.iph_ptr = pptrs.vlan6.packet_ptr + ETHER_HDRLEN + IEEE8021Q_TAGLEN;
  pptrs.vlan6.tlh_ptr = pptrs.vlan6.packet_ptr + ETHER_HDRLEN + IEEE8021Q_TAGLEN + sizeof(struct ip6_hdr);
  Assign16(((struct ip6_hdr *)pptrs.vlan6.iph_ptr)->ip6_plen, htons(100));
  Assign16(((struct ip6_hdr *)pptrs.vlan6.iph_ptr)->ip6_hlim, htons(64));
  pptrs.vlan6.pkthdr->caplen = 64; /* eth_header + vlan + ip6_hdr + my_tlhdr */
  pptrs.vlan6.pkthdr->len = 100; /* fake len */
  pptrs.vlan6.l3_proto = ETHERTYPE_IPV6;

  memset(dummy_packet_mpls6, 0, sizeof(dummy_packet_mpls6));
  pptrs.mpls6.f_data = pptrs.v4.f_data; 
  pptrs.mpls6.idtable = pptrs.v4.idtable;
  pptrs.mpls6.f_agent = (u_char *) &client;
  pptrs.mpls6.packet_ptr = dummy_packet_mpls6;
  pptrs.mpls6.pkthdr = &dummy_pkthdr_mpls6;
  Assign16(((struct eth_header *)pptrs.mpls6.packet_ptr)->ether_type, htons(ETHERTYPE_MPLS));
  pptrs.mpls6.mac_ptr = (u_char *)((struct eth_header *)pptrs.mpls6.packet_ptr)->ether_dhost;
  pptrs.mpls6.mpls_ptr = pptrs.mpls6.packet_ptr + ETHER_HDRLEN;
  pptrs.mpls6.pkthdr->caplen = 100; /* eth_header + upto 10 MPLS labels + ip6_hdr + my_tlhdr */
  pptrs.mpls6.pkthdr->len = 128; /* fake len */
  pptrs.mpls6.l3_proto = ETHERTYPE_IPV6;

  memset(dummy_packet_vlan_mpls6, 0, sizeof(dummy_packet_vlan_mpls6));
  pptrs.vlanmpls6.f_data = pptrs.v4.f_data; 
  pptrs.vlanmpls6.idtable = pptrs.v4.idtable;
  pptrs.vlanmpls6.f_agent = (u_char *) &client;
  pptrs.vlanmpls6.packet_ptr = dummy_packet_vlan_mpls6;
  pptrs.vlanmpls6.pkthdr = &dummy_pkthdr_vlan_mpls6;
  Assign16(((struct eth_header *)pptrs.vlanmpls6.packet_ptr)->ether_type, htons(ETHERTYPE_8021Q));
  pptrs.vlanmpls6.mac_ptr = (u_char *)((struct eth_header *)pptrs.vlanmpls6.packet_ptr)->ether_dhost;
  pptrs.vlanmpls6.vlan_ptr = pptrs.vlanmpls6.packet_ptr + ETHER_HDRLEN;
  Assign16(*(pptrs.vlanmpls6.vlan_ptr+2), htons(ETHERTYPE_MPLS));
  pptrs.vlanmpls6.mpls_ptr = pptrs.vlanmpls6.packet_ptr + ETHER_HDRLEN + IEEE8021Q_TAGLEN;
  pptrs.vlanmpls6.pkthdr->caplen = 104; /* eth_header + vlan + upto 10 MPLS labels + ip6_hdr + my_tlhdr */
  pptrs.vlanmpls6.pkthdr->len = 128; /* fake len */
  pptrs.vlanmpls6.l3_proto = ETHERTYPE_IP;
#endif

  Log(LOG_INFO, "INFO ( default/core ): waiting for data on UDP port '%u'\n", config.nfacctd_port);
  allowed = TRUE;

  /* Main loop */
  for (;;) {
    // memset(&spp, 0, sizeof(spp));
    ret = recvfrom(sd, sflow_packet, SFLOW_MAX_MSG_SIZE, 0, (struct sockaddr *) &client, &clen);
    spp.rawSample = sflow_packet;
    spp.rawSampleLen = ret;
    spp.datap = (u_int32_t *) spp.rawSample;
    spp.endp = sflow_packet + spp.rawSampleLen; 

    if (ret < SFLOW_MIN_MSG_SIZE) continue; 

    /* check if Hosts Allow Table is loaded; if it is, we will enforce rules */
    if (allow.num) allowed = check_allow(&allow, (struct sockaddr *)&client); 
    if (!allowed) continue;

    if (reload_map) {
      load_networks(config.networks_file, &nt, &nc);
      load_id_file(config.acct_type, config.pre_tag_map, &idt, &req);
      reload_map = FALSE;
    }

    switch(spp.datagramVersion = getData32(&spp)) {
    case 5:
      getAddress(&spp, &spp.agent_addr);
      process_SFv5_packet(&spp, &pptrs, &req);
      break;
    case 4:
    case 2:
      getAddress(&spp, &spp.agent_addr);
      process_SFv2v4_packet(&spp, &pptrs, &req);
      break;
    default:
      notify_malf_packet(LOG_INFO, "INFO: Discarding unknown packet", (struct sockaddr *) pptrs.v4.f_agent);
      break;
    }
  }
}

void InterSampleCleanup(SFSample *spp)
{
  u_char *start = (u_char *) spp;
  u_char *ptr = (u_char *) &spp->sampleType;

  memset(ptr, 0, SFSampleSz-(ptr-start));
}

void process_SFv2v4_packet(SFSample *spp, struct packet_ptrs_vector *pptrsv,
		                struct plugin_requests *req)
{
  u_int32_t samplesInPacket, idx;
  u_int32_t sampleType;

  spp->sequenceNo = getData32(spp);
  spp->sysUpTime = getData32(spp);
  samplesInPacket = getData32(spp);

  for (idx = 0; idx < samplesInPacket; idx++) {
    InterSampleCleanup(spp);
    sampleType = getData32(spp);
    switch (sampleType) {
    case SFLFLOW_SAMPLE:
      readv2v4FlowSample(spp, pptrsv, req);
      break;
    case SFLCOUNTERS_SAMPLE:
      readv2v4CountersSample(spp);
      break;
    default:
      notify_malf_packet(LOG_INFO, "INFO: Discarding unknown v2/v4 sample", (struct sockaddr *) pptrsv->v4.f_agent);
      return; /* unexpected sampleType; aborting packet */
    }
    if ((u_char *)spp->datap > spp->endp) return;
  }
}

void process_SFv5_packet(SFSample *spp, struct packet_ptrs_vector *pptrsv,
		struct plugin_requests *req)
{
  u_int32_t samplesInPacket, idx;
  u_int32_t sampleType;

  spp->agentSubId = getData32(spp);
  spp->sequenceNo = getData32(spp);
  spp->sysUpTime = getData32(spp);
  samplesInPacket = getData32(spp);

  for (idx = 0; idx < samplesInPacket; idx++) {
    InterSampleCleanup(spp);
    sampleType = getData32(spp);
    switch (sampleType) {
    case SFLFLOW_SAMPLE:
      readv5FlowSample(spp, FALSE, pptrsv, req);
      break;
    case SFLCOUNTERS_SAMPLE:
      readv5CountersSample(spp);
      break;
    case SFLFLOW_SAMPLE_EXPANDED:
      readv5FlowSample(spp, TRUE, pptrsv, req);
      break;
    case SFLCOUNTERS_SAMPLE_EXPANDED:
      readv5CountersSample(spp);
      break;
    default:
      notify_malf_packet(LOG_INFO, "INFO: Discarding unknown v5 sample", (struct sockaddr *) pptrsv->v4.f_agent);
      return; /* unexpected sampleType; aborting packet */ 
    }
    if ((u_char *)spp->datap > spp->endp) return; 
  }
}

void load_allow_file(char *filename, struct hosts_table *t)
{
  FILE *file;
  char buf[SRVBUFLEN];
  int index = 0;

  if (filename) {
    if ((file = fopen(filename, "r")) == NULL) {
      Log(LOG_ERR, "ERROR ( default/core ): allow file '%s' not found\n", filename);
      exit(1);
    }

    memset(t->table, 0, sizeof(t->table)); 
    while (!feof(file)) {
      if (index >= MAX_MAP_ENTRIES) break; /* XXX: we shouldn't exit silently */ 
      memset(buf, 0, SRVBUFLEN);
      if (fgets(buf, SRVBUFLEN, file)) { 
        if (!sanitize_buf(buf)) {
	  if (str_to_addr(buf, &t->table[index])) index++;
	  else Log(LOG_WARNING, "WARN ( default/core ): 'sfacctd_allow_file': Bad IP address '%s'. Ignored.\n", buf);
        }
      }
    }
    t->num = index;
  }
}

int check_allow(struct hosts_table *allow, struct sockaddr *sa)
{
  int index;

  for (index = 0; index < allow->num; index++) {
    if (((struct sockaddr *)sa)->sa_family == allow->table[index].family) {
      if (allow->table[index].family == AF_INET) {
        if (((struct sockaddr_in *)sa)->sin_addr.s_addr == allow->table[index].address.ipv4.s_addr)
          return TRUE;
      }
#if defined ENABLE_IPV6
      else if (allow->table[index].family == AF_INET6) {
        if (!ip6_addr_cmp(&(((struct sockaddr_in6 *)sa)->sin6_addr), &allow->table[index].address.ipv6))
          return TRUE;
      }
#endif
    }
  }
  
  return FALSE;
}

void compute_once()
{
  struct pkt_data dummy;

  CounterSz = sizeof(dummy.pkt_len);
  PdataSz = sizeof(struct pkt_data);
  ChBufHdrSz = sizeof(struct ch_buf_hdr);
  CharPtrSz = sizeof(char *);
  IP4HdrSz = sizeof(struct my_iphdr);
  IP4TlSz = sizeof(struct my_iphdr)+sizeof(struct my_tlhdr);
  SFSampleSz = sizeof(SFSample);
  SFLAddressSz = sizeof(SFLAddress);
  SFrenormEntrySz = sizeof(struct SF_renorm_entry);
  PptrsSz = sizeof(struct packet_ptrs);
  CSSz = sizeof(struct class_st);

#if defined ENABLE_IPV6
  IP6HdrSz = sizeof(struct ip6_hdr);
  IP6AddrSz = sizeof(struct in6_addr);
  IP6TlSz = sizeof(struct ip6_hdr)+sizeof(struct my_tlhdr);
#endif
}

void notify_malf_packet(short int severity, char *ostr, struct sockaddr *sa)
{
  struct host_addr a;
  u_char errstr[SRVBUFLEN];
  u_char agent_addr[50] /* able to fit an IPv6 string aswell */, any[]="0.0.0.0";
  u_int16_t agent_port;

  sa_to_addr(sa, &a, &agent_port);
  addr_to_str(agent_addr, &a);
  if (!config.nfacctd_ip) config.nfacctd_ip = any;
  snprintf(errstr, SRVBUFLEN, "%s: sfacctd=%s:%u agent=%s:%u \n",
  ostr, config.nfacctd_ip, config.nfacctd_port, agent_addr, agent_port);
  Log(severity, errstr);
}

/*_________________---------------------------__________________
  _________________    lengthCheck            __________________
  -----------------___________________________------------------
*/

int lengthCheck(SFSample *sample, u_char *start, int len)
{
  u_int32_t actualLen = (u_char *)sample->datap - start;
  if (actualLen != len) {
    /* XXX: notify length mismatch */ 
    return ERR;
  }

  return FALSE;
}

/*_________________---------------------------__________________
  _________________     decodeLinkLayer       __________________
  -----------------___________________________------------------
  store the offset to the start of the ipv4 header in the sequence_number field
  or -1 if not found. Decode the 802.1d if it's there.
*/

#define NFT_ETHHDR_SIZ 14
#define NFT_8022_SIZ 3
#define NFT_MAX_8023_LEN 1500

void decodeLinkLayer(SFSample *sample)
{
  u_char *start = (u_char *)sample->header;
  u_char *end = start + sample->headerLen;
  u_char *ptr = start;
  u_int16_t caplen = end - (u_char *)sample->datap;

  /* assume not found */
  sample->gotIPV4 = FALSE;
  sample->gotIPV6 = FALSE;

  if (caplen < NFT_ETHHDR_SIZ) return; /* not enough for an Ethernet header */
  caplen -= NFT_ETHHDR_SIZ;

  memcpy(sample->eth_dst, ptr, 6);
  ptr += 6;

  memcpy(sample->eth_src, ptr, 6);
  ptr += 6;
  sample->eth_type = (ptr[0] << 8) + ptr[1];
  ptr += 2;

  if (sample->eth_type == ETHERTYPE_8021Q) {
    /* VLAN  - next two bytes */
    u_int32_t vlanData = (ptr[0] << 8) + ptr[1];
    u_int32_t vlan = vlanData & 0x0fff;
    u_int32_t priority = vlanData >> 13;

    if (caplen < 2) return;

    ptr += 2;
    /*  _____________________________________ */
    /* |   pri  | c |         vlan-id        | */
    /*  ------------------------------------- */
    /* [priority = 3bits] [Canonical Format Flag = 1bit] [vlan-id = 12 bits] */
    sample->in_vlan = vlan;
    sample->eth_type = (ptr[0] << 8) + ptr[1];

    ptr += 2;
    caplen -= 2;
  }

  if (sample->eth_type <= NFT_MAX_8023_LEN) {
    /* assume 802.3+802.2 header */
    if (caplen < 8) return;

    /* check for SNAP */
    if(ptr[0] == 0xAA &&
       ptr[1] == 0xAA &&
       ptr[2] == 0x03) {
      ptr += 3;
      if(ptr[0] != 0 ||
	 ptr[1] != 0 ||
	 ptr[2] != 0) {
	return; /* no further decode for vendor-specific protocol */
      }
      ptr += 3;
      /* OUI == 00-00-00 means the next two bytes are the ethernet type (RFC 2895) */
      sample->eth_type = (ptr[0] << 8) + ptr[1];
      ptr += 2;
      caplen -= 8;
    }
    else {
      if (ptr[0] == 0x06 &&
	  ptr[1] == 0x06 &&
	  (ptr[2] & 0x01)) {
	/* IP over 8022 */
	ptr += 3;
	/* force the eth_type to be IP so we can inline the IP decode below */
	sample->eth_type = ETHERTYPE_IP;
	caplen -= 3;
      }
      else return;
    }
  }

  if (sample->eth_type == ETHERTYPE_MPLS || sample->eth_type == ETHERTYPE_MPLS_MULTI) {
    decodeMpls(sample);
    caplen -= sample->lstk.depth * 4;
  }

  if (sample->eth_type == ETHERTYPE_IP) {
    sample->gotIPV4 = TRUE;
    sample->offsetToIPV4 = (ptr - start);
  }

#if defined ENABLE_IPV6
  if (sample->eth_type == ETHERTYPE_IPV6) {
    sample->gotIPV6 = TRUE;
    sample->offsetToIPV6 = (ptr - start);
  }
#endif
}


/*_________________---------------------------__________________
  _________________     decodeIPLayer4        __________________
  -----------------___________________________------------------
*/

void decodeIPLayer4(SFSample *sample, u_char *ptr, u_int32_t ipProtocol) {
  u_char *end = sample->header + sample->headerLen;
  if(ptr > (end - 8)) return; // not enough header bytes left
  switch(ipProtocol) {
  case 1: /* ICMP */
    {
      struct SF_icmphdr icmp;
      memcpy(&icmp, ptr, sizeof(icmp));
      sample->dcd_sport = icmp.type;
      sample->dcd_dport = icmp.code;
    }
    break;
  case 6: /* TCP */
    {
      struct SF_tcphdr tcp;
      memcpy(&tcp, ptr, sizeof(tcp));
      sample->dcd_sport = ntohs(tcp.th_sport);
      sample->dcd_dport = ntohs(tcp.th_dport);
      sample->dcd_tcpFlags = tcp.th_flags;
      if(sample->dcd_dport == 80) {
	int bytesLeft;
	int headerBytes = (tcp.th_off_and_unused >> 4) * 4;
	ptr += headerBytes;
	bytesLeft = sample->header + sample->headerLen - ptr;
      }
    }
    break;
  case 17: /* UDP */
    {
      struct SF_udphdr udp;
      memcpy(&udp, ptr, sizeof(udp));
      sample->dcd_sport = ntohs(udp.uh_sport);
      sample->dcd_dport = ntohs(udp.uh_dport);
      sample->udp_pduLen = ntohs(udp.uh_ulen);
    }
    break;
  default: /* some other protcol */
    break;
  }
}

/*_________________---------------------------__________________
  _________________     decodeIPV4            __________________
  -----------------___________________________------------------
*/

void decodeIPV4(SFSample *sample)
{
  if (sample->gotIPV4) {
    u_char *end = sample->header + sample->headerLen;
    u_char *ptr = sample->header + sample->offsetToIPV4;
    u_int16_t caplen = end - ptr;

    /* Create a local copy of the IP header (cannot overlay structure in case it is not quad-aligned...some
       platforms would core-dump if we tried that).  It's OK coz this probably performs just as well anyway. */
    struct SF_iphdr ip;

    if (caplen < IP4HdrSz) return; 

    memcpy(&ip, ptr, sizeof(ip));
    /* Value copy all ip elements into sample */
    sample->dcd_srcIP.s_addr = ip.saddr;
    sample->dcd_dstIP.s_addr = ip.daddr;
    sample->dcd_ipProtocol = ip.protocol;
    sample->dcd_ipTos = ip.tos;
    sample->dcd_ipTTL = ip.ttl;
    /* check for fragments */
    sample->ip_fragmentOffset = ntohs(ip.frag_off) & 0x1FFF;
    if (sample->ip_fragmentOffset == 0) {
      /* advance the pointer to the next protocol layer */
      /* ip headerLen is expressed as a number of quads */
      ptr += (ip.version_and_headerLen & 0x0f) * 4;
      decodeIPLayer4(sample, ptr, ip.protocol);
    }
  }
}

/*_________________---------------------------__________________
  _________________     decodeIPV6            __________________
  -----------------___________________________------------------
*/

#if defined ENABLE_IPV6
void decodeIPV6(SFSample *sample)
{
  u_int16_t payloadLen;
  u_int32_t label;
  u_int32_t nextHeader;
  u_char *end = sample->header + sample->headerLen;

  if(sample->gotIPV6) {
    u_char *ptr = sample->header + sample->offsetToIPV6;
    u_int16_t caplen = end - ptr;

    if (caplen < IP6HdrSz) return;
    
    // check the version
    {
      int ipVersion = (*ptr >> 4);
      if(ipVersion != 6) return;
    }

    // get the tos (priority)
    sample->dcd_ipTos = *ptr++ & 15;
    // 24-bit label
    label = *ptr++;
    label <<= 8;
    label += *ptr++;
    label <<= 8;
    label += *ptr++;
    // payload
    payloadLen = (ptr[0] << 8) + ptr[1];
    ptr += 2;
    // if payload is zero, that implies a jumbo payload

    // next header
    nextHeader = *ptr++;

    // TTL
    sample->dcd_ipTTL = *ptr++;

    {// src and dst address
      sample->ipsrc.type = SFLADDRESSTYPE_IP_V6;
      memcpy(&sample->ipsrc.address, ptr, 16);
      ptr +=16;
      sample->ipdst.type = SFLADDRESSTYPE_IP_V6;
      memcpy(&sample->ipdst.address, ptr, 16);
      ptr +=16;
    }

    // skip over some common header extensions...
    // http://searchnetworking.techtarget.com/originalContent/0,289142,sid7_gci870277,00.html
    while(nextHeader == 0 ||  // hop
	  nextHeader == 43 || // routing
	  nextHeader == 44 || // fragment
	  // nextHeader == 50 || // encryption - don't bother coz we'll not be able to read any further
	  nextHeader == 51 || // auth
	  nextHeader == 60) { // destination options
      u_int32_t optionLen, skip;
      nextHeader = ptr[0];
      optionLen = 8 * (ptr[1] + 1);  // second byte gives option len in 8-byte chunks, not counting first 8
      skip = optionLen - 2;
      ptr += skip;
      if(ptr > end) return; // ran off the end of the header
    }
    
    // now that we have eliminated the extension headers, nextHeader should have what we want to
    // remember as the ip protocol...
    sample->dcd_ipProtocol = nextHeader;
    decodeIPLayer4(sample, ptr, sample->dcd_ipProtocol);
  }
}
#endif

/*_________________---------------------------__________________
  _________________   read data fns           __________________
  -----------------___________________________------------------
*/

u_int32_t getData32(SFSample *sample) 
{
  if ((u_char *)sample->datap > sample->endp) return 0; 
  return ntohl(*(sample->datap)++);
}

u_int32_t getData32_nobswap(SFSample *sample) 
{
  if ((u_char *)sample->datap > sample->endp) return 0;
  return *(sample->datap)++;
}

void skipBytes(SFSample *sample, int skip)
{
  int quads = (skip + 3) / 4;
  sample->datap += quads;
  // if((u_char *)sample->datap > sample->endp) return 0; 
}

u_int32_t getString(SFSample *sample, char *buf, int bufLen)
{
  u_int32_t len, read_len;
  len = getData32(sample);
  // truncate if too long
  read_len = (len >= bufLen) ? (bufLen - 1) : len;
  memcpy(buf, sample->datap, read_len);
  buf[read_len] = '\0';   // null terminate
  skipBytes(sample, len);
  return len;
}

u_int32_t getAddress(SFSample *sample, SFLAddress *address)
{
  address->type = getData32(sample);
  if(address->type == SFLADDRESSTYPE_IP_V4)
    address->address.ip_v4.s_addr = getData32_nobswap(sample);
  else {
#if defined ENABLE_IPV6
    memcpy(&address->address.ip_v6.s6_addr, sample->datap, 16);
#endif
    skipBytes(sample, 16);
  }
  return address->type;
}

char *printTag(u_int32_t tag, char *buf, int bufLen) {
  // should really be: snprintf(buf, buflen,...) but snprintf() is not always available
  sprintf(buf, "%lu:%lu", (tag >> 12), (tag & 0x00000FFF));
  return buf;
}

/*_________________---------------------------__________________
  _________________    readExtendedSwitch     __________________
  -----------------___________________________------------------
*/

void readExtendedSwitch(SFSample *sample)
{
  sample->in_vlan = getData32(sample);
  sample->in_priority = getData32(sample);
  sample->out_vlan = getData32(sample);
  sample->out_priority = getData32(sample);

  sample->extended_data_tag |= SASAMPLE_EXTENDED_DATA_SWITCH;
}

/*_________________---------------------------__________________
  _________________    readExtendedRouter     __________________
  -----------------___________________________------------------
*/

void readExtendedRouter(SFSample *sample)
{
  u_int32_t addrType;
  char buf[51];

  getAddress(sample, &sample->nextHop);
  sample->srcMask = getData32(sample);
  sample->dstMask = getData32(sample);

  sample->extended_data_tag |= SASAMPLE_EXTENDED_DATA_ROUTER;
}

/*_________________---------------------------__________________
  _________________  readExtendedGateway_v2   __________________
  -----------------___________________________------------------
*/

void readExtendedGateway_v2(SFSample *sample)
{
  sample->my_as = getData32(sample);
  sample->src_as = getData32(sample);
  sample->src_peer_as = getData32(sample);
  sample->dst_as_path_len = getData32(sample);
  /* just point at the dst_as_path array */
  if(sample->dst_as_path_len > 0) {
    sample->dst_as_path = sample->datap;
    /* and skip over it in the input */
    skipBytes(sample, sample->dst_as_path_len * 4);
    // fill in the dst and dst_peer fields too
    sample->dst_peer_as = ntohl(sample->dst_as_path[0]);
    sample->dst_as = ntohl(sample->dst_as_path[sample->dst_as_path_len - 1]);
  }
  
  sample->extended_data_tag |= SASAMPLE_EXTENDED_DATA_GATEWAY;
}

/*_________________---------------------------__________________
  _________________  readExtendedGateway      __________________
  -----------------___________________________------------------
*/

void readExtendedGateway(SFSample *sample)
{
  u_int32_t segments;
  int seg;
  char buf[51];

  if(sample->datagramVersion >= 5) getAddress(sample, &sample->bgp_nextHop);

  sample->my_as = getData32(sample);
  sample->src_as = getData32(sample);
  sample->src_peer_as = getData32(sample);
  segments = getData32(sample);
  if (segments > 0) {
    for (seg = 0; seg < segments; seg++) {
      u_int32_t seg_type;
      u_int32_t seg_len;
      int i;

      seg_type = getData32(sample);
      seg_len = getData32(sample);

      for (i = 0; i < seg_len; i++) {
	u_int32_t asNumber;

	asNumber = getData32(sample);
	/* mark the first one as the dst_peer_as */
	if(i == 0 && seg == 0) sample->dst_peer_as = asNumber;
	/* make sure the AS sets are in parentheses */
	/* mark the last one as the dst_as */
	if(seg == (segments - 1) && i == (seg_len - 1)) sample->dst_as = asNumber;
      }
    }
  }

  sample->communities_len = getData32(sample);
  /* just point at the communities array */
  if(sample->communities_len > 0) sample->communities = sample->datap;
  /* and skip over it in the input */
  skipBytes(sample, sample->communities_len * 4);
 
  sample->extended_data_tag |= SASAMPLE_EXTENDED_DATA_GATEWAY;
  sample->localpref = getData32(sample);
}

/*_________________---------------------------__________________
  _________________    readExtendedUser       __________________
  -----------------___________________________------------------
*/

void readExtendedUser(SFSample *sample)
{
  if(sample->datagramVersion >= 5) sample->src_user_charset = getData32(sample);
  sample->src_user_len = getString(sample, sample->src_user, SA_MAX_EXTENDED_USER_LEN);
  if(sample->datagramVersion >= 5) sample->dst_user_charset = getData32(sample);
  sample->dst_user_len = getString(sample, sample->dst_user, SA_MAX_EXTENDED_USER_LEN);

  sample->extended_data_tag |= SASAMPLE_EXTENDED_DATA_USER;
}

/*_________________---------------------------__________________
  _________________    readExtendedUrl        __________________
  -----------------___________________________------------------
*/

void readExtendedUrl(SFSample *sample)
{
  sample->url_direction = getData32(sample);
  sample->url_len = getString(sample, sample->url, SA_MAX_EXTENDED_URL_LEN);
  if(sample->datagramVersion >= 5) sample->host_len = getString(sample, sample->host, SA_MAX_EXTENDED_HOST_LEN);

  sample->extended_data_tag |= SASAMPLE_EXTENDED_DATA_URL;
}


/*_________________---------------------------__________________
  _________________       mplsLabelStack      __________________
  -----------------___________________________------------------
*/

void mplsLabelStack(SFSample *sample, char *fieldName)
{
  u_int32_t lab;

  sample->lstk.depth = getData32(sample);
  /* just point at the lablelstack array */
  if (sample->lstk.depth > 0) sample->lstk.stack = (u_int32_t *)sample->datap;
  /* and skip over it in the input */
  skipBytes(sample, sample->lstk.depth * 4);
}

/*_________________---------------------------__________________
  _________________    readExtendedMpls       __________________
  -----------------___________________________------------------
*/

void readExtendedMpls(SFSample *sample)
{
  char buf[51];

  getAddress(sample, &sample->mpls_nextHop);

  mplsLabelStack(sample, "mpls_input_stack");
  mplsLabelStack(sample, "mpls_output_stack");
  
  sample->extended_data_tag |= SASAMPLE_EXTENDED_DATA_MPLS;
}

/*_________________---------------------------__________________
  _________________    readExtendedNat        __________________
  -----------------___________________________------------------
*/

void readExtendedNat(SFSample *sample)
{
  char buf[51];

  getAddress(sample, &sample->nat_src);
  getAddress(sample, &sample->nat_dst);

  sample->extended_data_tag |= SASAMPLE_EXTENDED_DATA_NAT;
}


/*_________________---------------------------__________________
  _________________    readExtendedMplsTunnel __________________
  -----------------___________________________------------------
*/

void readExtendedMplsTunnel(SFSample *sample)
{
#define SA_MAX_TUNNELNAME_LEN 100
  char tunnel_name[SA_MAX_TUNNELNAME_LEN+1];
  u_int32_t tunnel_id, tunnel_cos;
  
  getString(sample, tunnel_name, SA_MAX_TUNNELNAME_LEN); 
  tunnel_id = getData32(sample);
  tunnel_cos = getData32(sample);

  sample->extended_data_tag |= SASAMPLE_EXTENDED_DATA_MPLS_TUNNEL;
}

/*_________________---------------------------__________________
  _________________    readExtendedMplsVC     __________________
  -----------------___________________________------------------
*/

void readExtendedMplsVC(SFSample *sample)
{
#define SA_MAX_VCNAME_LEN 100
  char vc_name[SA_MAX_VCNAME_LEN+1];
  u_int32_t vll_vc_id, vc_cos;

  getString(sample, vc_name, SA_MAX_VCNAME_LEN); 
  vll_vc_id = getData32(sample);
  vc_cos = getData32(sample);

  sample->extended_data_tag |= SASAMPLE_EXTENDED_DATA_MPLS_VC;
}

/*_________________---------------------------__________________
  _________________    readExtendedMplsFTN    __________________
  -----------------___________________________------------------
*/

void readExtendedMplsFTN(SFSample *sample)
{
#define SA_MAX_FTN_LEN 100
  char ftn_descr[SA_MAX_FTN_LEN+1];
  u_int32_t ftn_mask;

  getString(sample, ftn_descr, SA_MAX_FTN_LEN);
  ftn_mask = getData32(sample);

  sample->extended_data_tag |= SASAMPLE_EXTENDED_DATA_MPLS_FTN;
}

/*_________________---------------------------__________________
  _________________  readExtendedMplsLDP_FEC  __________________
  -----------------___________________________------------------
*/

void readExtendedMplsLDP_FEC(SFSample *sample)
{
  u_int32_t fec_addr_prefix_len = getData32(sample);

  sample->extended_data_tag |= SASAMPLE_EXTENDED_DATA_MPLS_LDP_FEC;
}

/*_________________---------------------------__________________
  _________________  readExtendedVlanTunnel   __________________
  -----------------___________________________------------------
*/

void readExtendedVlanTunnel(SFSample *sample)
{
  SFLLabelStack lstk;

  lstk.depth = getData32(sample);
  /* just point at the lablelstack array */
  if(lstk.depth > 0) lstk.stack = (u_int32_t *)sample->datap;
  /* and skip over it in the input */
  skipBytes(sample, lstk.depth * 4);

  sample->extended_data_tag |= SASAMPLE_EXTENDED_DATA_VLAN_TUNNEL;
}

/*_________________---------------------------__________________
  _________________    readExtendedProcess    __________________
  -----------------___________________________------------------
*/

void readExtendedProcess(SFSample *sample)
{
  u_int32_t num_processes, i;

  num_processes = getData32(sample);
  for (i = 0; i < num_processes; i++) skipBytes(sample, 4);
}

void decodeMpls(SFSample *sample)
{
  struct packet_ptrs dummy_pptrs;
  u_char *ptr = (u_char *)sample->datap, *end = sample->header + sample->headerLen;
  u_int16_t nl = 0, caplen = end - ptr;
  
  memset(&dummy_pptrs, 0, sizeof(dummy_pptrs));
  sample->eth_type = mpls_handler(ptr, &caplen, &nl, &dummy_pptrs);

  if (sample->eth_type == ETHERTYPE_IP) {
    sample->gotIPV4 = TRUE;
    sample->offsetToIPV4 = nl+(ptr-sample->header);
  } 
#if defined ENABLE_IPV6
  else if (sample->eth_type == ETHERTYPE_IPV6) {
    sample->gotIPV6 = TRUE;
    sample->offsetToIPV6 = nl+(ptr-sample->header);
  }
#endif

  if (nl) {
    sample->lstk.depth = nl / 4; 
    sample->lstk.stack = (u_int32_t *) dummy_pptrs.mpls_ptr;
  }
}

void decodePPP(SFSample *sample)
{
  struct packet_ptrs dummy_pptrs;
  struct pcap_pkthdr h;
  u_char *ptr = (u_char *)sample->datap, *end = sample->header + sample->headerLen;
  u_int16_t nl = 0;

  memset(&dummy_pptrs, 0, sizeof(dummy_pptrs));
  h.caplen = end - ptr; 
  dummy_pptrs.packet_ptr = ptr;
  ppp_handler(&h, &dummy_pptrs);
  sample->eth_type = dummy_pptrs.l3_proto;
  
  if (dummy_pptrs.mpls_ptr) {
    if (dummy_pptrs.iph_ptr) nl = dummy_pptrs.iph_ptr - dummy_pptrs.mpls_ptr;
    if (nl) {
      sample->lstk.depth = nl / 4;
      sample->lstk.stack = (u_int32_t *) dummy_pptrs.mpls_ptr;
    }
  }
  if (sample->eth_type == ETHERTYPE_IP) {
    sample->gotIPV4 = TRUE;
    sample->offsetToIPV4 = dummy_pptrs.iph_ptr - sample->header;
  }
#if defined ENABLE_IPV6
  else if (sample->eth_type == ETHERTYPE_IPV6) {
    sample->gotIPV6 = TRUE;
    sample->offsetToIPV6 = dummy_pptrs.iph_ptr - sample->header;
  }
#endif
}

/*_________________---------------------------__________________
  _________________  readFlowSample_header    __________________
  -----------------___________________________------------------
*/

void readFlowSample_header(SFSample *sample)
{
  sample->headerProtocol = getData32(sample);
  sample->sampledPacketSize = getData32(sample);
  if(sample->datagramVersion > 4) sample->stripped = getData32(sample);
  sample->headerLen = getData32(sample);
  
  sample->header = (u_char *)sample->datap; /* just point at the header */
  
  switch(sample->headerProtocol) {
    /* the header protocol tells us where to jump into the decode */
  case SFLHEADER_ETHERNET_ISO8023:
    decodeLinkLayer(sample);
    break;
  case SFLHEADER_IPv4: 
    sample->gotIPV4 = TRUE;
    sample->offsetToIPV4 = 0;
    break;
#if defined ENABLE_IPV6
  case SFLHEADER_IPv6:
    sample->gotIPV6 = TRUE;
    sample->offsetToIPV6 = 0;
    break;
#endif
  case SFLHEADER_MPLS:
    decodeMpls(sample);
    break;
  case SFLHEADER_PPP:
    decodePPP(sample);
    break;
  case SFLHEADER_ISO88024_TOKENBUS:
  case SFLHEADER_ISO88025_TOKENRING:
  case SFLHEADER_FDDI:
  case SFLHEADER_FRAME_RELAY:
  case SFLHEADER_X25:
  case SFLHEADER_SMDS:
  case SFLHEADER_AAL5:
  case SFLHEADER_AAL5_IP:
  default:
    /* XXX: nofity error */ 
    break;
  }
  
  if (sample->gotIPV4) decodeIPV4(sample);
#if defined ENABLE_IPV6
  else if (sample->gotIPV6) decodeIPV6(sample);
#endif

  skipBytes(sample, sample->headerLen);
}

/*_________________---------------------------__________________
  _________________  readFlowSample_ethernet  __________________
  -----------------___________________________------------------
*/

void readFlowSample_ethernet(SFSample *sample)
{
  sample->eth_len = getData32(sample);
  memcpy(sample->eth_src, sample->datap, 6);
  skipBytes(sample, 6);
  memcpy(sample->eth_dst, sample->datap, 6);
  skipBytes(sample, 6);
  sample->eth_type = getData32(sample);

  if (sample->eth_type == ETHERTYPE_IP) sample->gotIPV4 = TRUE;
#if defined ENABLE_IPV6
  if (sample->eth_type == ETHERTYPE_IPV6) sample->gotIPV6 = TRUE;
#endif
}


/*_________________---------------------------__________________
  _________________    readFlowSample_IPv4    __________________
  -----------------___________________________------------------
*/

void readFlowSample_IPv4(SFSample *sample)
{
  sample->headerLen = sizeof(SFLSampled_ipv4);
  sample->header = (u_char *)sample->datap; /* just point at the header */
  skipBytes(sample, sample->headerLen);
  {
    SFLSampled_ipv4 nfKey;

    memcpy(&nfKey, sample->header, sizeof(nfKey));
    sample->sampledPacketSize = ntohl(nfKey.length);
    sample->dcd_srcIP = nfKey.src_ip;
    sample->dcd_dstIP = nfKey.dst_ip;
    sample->dcd_ipProtocol = ntohl(nfKey.protocol);
    sample->dcd_ipTos = ntohl(nfKey.tos);
    sample->dcd_sport = ntohl(nfKey.src_port);
    sample->dcd_dport = ntohl(nfKey.dst_port);
  }

  sample->gotIPV4 = TRUE;
}

/*_________________---------------------------__________________
  _________________    readFlowSample_IPv6    __________________
  -----------------___________________________------------------
*/

void readFlowSample_IPv6(SFSample *sample)
{
  sample->header = (u_char *)sample->datap; /* just point at the header */
  sample->headerLen = sizeof(SFLSampled_ipv6);
  skipBytes(sample, sample->headerLen);

#if defined ENABLE_IPV6
  {
    SFLSampled_ipv6 nfKey6;

    memcpy(&nfKey6, sample->header, sizeof(nfKey6));
    sample->sampledPacketSize = ntohl(nfKey6.length);
    sample->ipsrc.type = SFLADDRESSTYPE_IP_V6;
    memcpy(&sample->ipsrc.address, &nfKey6.src_ip, IP6AddrSz);
    sample->ipdst.type = SFLADDRESSTYPE_IP_V6;
    memcpy(&sample->ipdst.address, &nfKey6.dst_ip, IP6AddrSz);
    sample->dcd_ipProtocol = ntohl(nfKey6.protocol);
    sample->dcd_ipTos = ntohl(nfKey6.priority);
    sample->dcd_sport = ntohl(nfKey6.src_port);
    sample->dcd_dport = ntohl(nfKey6.dst_port);
  }

  sample->gotIPV6 = TRUE;
#endif
}

/*_________________---------------------------__________________
  _________________    readv2v4FlowSample    __________________
  -----------------___________________________------------------
*/

void readv2v4FlowSample(SFSample *sample, struct packet_ptrs_vector *pptrsv, struct plugin_requests *req)
{
  sample->samplesGenerated = getData32(sample);
  {
    u_int32_t samplerId = getData32(sample);
    sample->ds_class = samplerId >> 24;
    sample->ds_index = samplerId & 0x00ffffff;
  }
  
  sample->meanSkipCount = getData32(sample);
  sample->samplePool = getData32(sample);
  sample->dropEvents = getData32(sample);
  sample->inputPort = getData32(sample);
  sample->outputPort = getData32(sample);
  sample->packet_data_tag = getData32(sample);
  
  switch(sample->packet_data_tag) {
    
  case INMPACKETTYPE_HEADER: readFlowSample_header(sample); break;
  case INMPACKETTYPE_IPV4: readFlowSample_IPv4(sample); break;
  case INMPACKETTYPE_IPV6: readFlowSample_IPv6(sample); break;
  default: notify_malf_packet(LOG_INFO, "INFO: Discarding unknown v2/v4 Data Tag", (struct sockaddr *) pptrsv->v4.f_agent); break;
  }

  sample->extended_data_tag = 0;
  {
    u_int32_t x;
    sample->num_extended = getData32(sample);
    for(x = 0; x < sample->num_extended; x++) {
      u_int32_t extended_tag;
      extended_tag = getData32(sample);
      switch(extended_tag) {
      case INMEXTENDED_SWITCH: readExtendedSwitch(sample); break;
      case INMEXTENDED_ROUTER: readExtendedRouter(sample); break;
      case INMEXTENDED_GATEWAY:
	if(sample->datagramVersion == 2) readExtendedGateway_v2(sample);
	else readExtendedGateway(sample);
	break;
      case INMEXTENDED_USER: readExtendedUser(sample); break;
      case INMEXTENDED_URL: readExtendedUrl(sample); break;
      default: notify_malf_packet(LOG_INFO, "INFO: Discarding unknown v2/v4 Extended Data Tag", (struct sockaddr *) pptrsv->v4.f_agent); break;
      }
    }
  }

  finalizeSample(sample, pptrsv, req);
}

/*_________________---------------------------__________________
  _________________    readv5FlowSample         __________________
  -----------------___________________________------------------
*/

void readv5FlowSample(SFSample *sample, int expanded, struct packet_ptrs_vector *pptrsv, struct plugin_requests *req)
{
  u_int32_t num_elements, sampleLength, actualSampleLength;
  u_char *sampleStart;

  sampleLength = getData32(sample);
  sampleStart = (u_char *)sample->datap;
  sample->samplesGenerated = getData32(sample);
  if(expanded) {
    sample->ds_class = getData32(sample);
    sample->ds_index = getData32(sample);
  }
  else {
    u_int32_t samplerId = getData32(sample);
    sample->ds_class = samplerId >> 24;
    sample->ds_index = samplerId & 0x00ffffff;
  }

  sample->meanSkipCount = getData32(sample);
  sample->samplePool = getData32(sample);
  sample->dropEvents = getData32(sample);
  if(expanded) {
    sample->inputPortFormat = getData32(sample);
    sample->inputPort = getData32(sample);
    sample->outputPortFormat = getData32(sample);
    sample->outputPort = getData32(sample);
  }
  else {
    u_int32_t inp, outp;
    inp = getData32(sample);
    outp = getData32(sample);
    inp >>= 30;
    outp >>= 30;
    sample->inputPort = inp & 0x3fffffff;
    sample->outputPort = outp & 0x3fffffff;
  }

  num_elements = getData32(sample);
  {
    int el;
    for (el = 0; el < num_elements; el++) {
      u_int32_t tag, length;
      u_char *start;
      tag = getData32(sample);
      length = getData32(sample);
      start = (u_char *)sample->datap;

      switch(tag) {
      case SFLFLOW_HEADER:     readFlowSample_header(sample); break;
      case SFLFLOW_ETHERNET:   readFlowSample_ethernet(sample); break;
      case SFLFLOW_IPV4:       readFlowSample_IPv4(sample); break;
      case SFLFLOW_IPV6:       readFlowSample_IPv6(sample); break;
      case SFLFLOW_EX_SWITCH:  readExtendedSwitch(sample); break;
      case SFLFLOW_EX_ROUTER:  readExtendedRouter(sample); break;
      case SFLFLOW_EX_GATEWAY: readExtendedGateway(sample); break;
      case SFLFLOW_EX_USER:    readExtendedUser(sample); break;
      case SFLFLOW_EX_URL:     readExtendedUrl(sample); break;
      case SFLFLOW_EX_MPLS:    readExtendedMpls(sample); break;
      case SFLFLOW_EX_NAT:     readExtendedNat(sample); break;
      case SFLFLOW_EX_MPLS_TUNNEL:  readExtendedMplsTunnel(sample); break;
      case SFLFLOW_EX_MPLS_VC:      readExtendedMplsVC(sample); break;
      case SFLFLOW_EX_MPLS_FTN:     readExtendedMplsFTN(sample); break;
      case SFLFLOW_EX_MPLS_LDP_FEC: readExtendedMplsLDP_FEC(sample); break;
      case SFLFLOW_EX_VLAN_TUNNEL:  readExtendedVlanTunnel(sample); break;
      case SFLFLOW_EX_PROCESS:      readExtendedProcess(sample); break;
      default: skipBytes(sample, length); break; 
      }
      if (lengthCheck(sample, start, length) == ERR) return;
    }
  }
  if (lengthCheck(sample, sampleStart, sampleLength) == ERR) return;

  finalizeSample(sample, pptrsv, req);
}

void readv5CountersSample(SFSample *sample)
{
  u_int32_t sampleLength;

  sampleLength = getData32(sample);
  skipBytes(sample, sampleLength);
}

/*
   seems like sFlow v2/v4 does not supply any meaningful information
   about the length of current sample. This is because we still need
   to parse the very first part of the sample
*/ 
void readv2v4CountersSample(SFSample *sample)
{
  skipBytes(sample, 12);
  sample->counterBlockVersion = getData32(sample);

  switch(sample->counterBlockVersion) {
  case INMCOUNTERSVERSION_GENERIC:
  case INMCOUNTERSVERSION_ETHERNET:
  case INMCOUNTERSVERSION_TOKENRING:
  case INMCOUNTERSVERSION_FDDI:
  case INMCOUNTERSVERSION_VG:
  case INMCOUNTERSVERSION_WAN: skipBytes(sample, 88); break;
  case INMCOUNTERSVERSION_VLAN: break;
  default: return; 
  }

  /* now see if there are any specific counter blocks to add */
  switch(sample->counterBlockVersion) {
  case INMCOUNTERSVERSION_GENERIC: /* nothing more */ break;
  case INMCOUNTERSVERSION_ETHERNET: skipBytes(sample, 52); break;
  case INMCOUNTERSVERSION_TOKENRING: skipBytes(sample, 72); break;
  case INMCOUNTERSVERSION_FDDI: break;
  case INMCOUNTERSVERSION_VG: skipBytes(sample, 80); break;
  case INMCOUNTERSVERSION_WAN: break;
  case INMCOUNTERSVERSION_VLAN: skipBytes(sample, 28); break;
  default: return; 
  }
}

void finalizeSample(SFSample *sample, struct packet_ptrs_vector *pptrsv, struct plugin_requests *req)
{
  struct packet_ptrs *pptrs = &pptrsv->v4;
  u_int16_t dcd_sport = htons(sample->dcd_sport), dcd_dport = htons(sample->dcd_dport);
  u_int16_t in_vlan = htons(sample->in_vlan);
  u_int16_t flow_type;

  /*
     We consider packets if:
     - sample->gotIPV4 || sample->gotIPV6 : it belongs to either an IPv4 or IPv6 packet.
     - !sample->eth_type : we don't know the L3 protocol. VLAN or MPLS accounting case.
  */
  if (sample->gotIPV4 || sample->gotIPV6 || !sample->eth_type) {
    flow_type = SF_evaluate_flow_type(pptrs);

    /* we need to understand the IP protocol version in order to build the fake packet */
    switch (flow_type) {
    case NF9_FTYPE_IPV4:
      if (req->bpf_filter) {
        reset_mac(pptrs);
        reset_ip4(pptrs);

        memcpy(pptrs->mac_ptr+ETH_ADDR_LEN, &sample->eth_src, ETH_ADDR_LEN); 
        memcpy(pptrs->mac_ptr, &sample->eth_dst, ETH_ADDR_LEN);
	((struct my_iphdr *)pptrs->iph_ptr)->ip_vhl = 0x45;
        memcpy(&((struct my_iphdr *)pptrs->iph_ptr)->ip_src, &sample->dcd_srcIP, 4);
        memcpy(&((struct my_iphdr *)pptrs->iph_ptr)->ip_dst, &sample->dcd_dstIP, 4);
        memcpy(&((struct my_iphdr *)pptrs->iph_ptr)->ip_p, &sample->dcd_ipProtocol, 1);
        memcpy(&((struct my_iphdr *)pptrs->iph_ptr)->ip_tos, &sample->dcd_ipTos, 1);
        memcpy(&((struct my_tlhdr *)pptrs->tlh_ptr)->src_port, &dcd_sport, 2);
        memcpy(&((struct my_tlhdr *)pptrs->tlh_ptr)->dst_port, &dcd_dport, 2);
      }
      pptrs->l4_proto = sample->dcd_ipProtocol;

      if (config.pre_tag_map) pptrs->tag = SF_find_id(pptrs);
      exec_plugins(pptrs);
      break;
#if defined ENABLE_IPV6
    case NF9_FTYPE_IPV6:
      if (req->bpf_filter) {
        reset_mac(&pptrsv->v6);
        reset_ip6(&pptrsv->v6);

	((struct ip6_hdr *)pptrsv->v6.iph_ptr)->ip6_ctlun.ip6_un2_vfc = 0x60;
        memcpy(pptrsv->v6.mac_ptr+ETH_ADDR_LEN, &sample->eth_src, ETH_ADDR_LEN); 
        memcpy(pptrsv->v6.mac_ptr, &sample->eth_dst, ETH_ADDR_LEN);
        memcpy(&((struct ip6_hdr *)pptrsv->v6.iph_ptr)->ip6_src, &sample->ipsrc.address.ip_v6, IP6AddrSz);
        memcpy(&((struct ip6_hdr *)pptrsv->v6.iph_ptr)->ip6_dst, &sample->ipdst.address.ip_v6, IP6AddrSz);
        memcpy(&((struct ip6_hdr *)pptrsv->v6.iph_ptr)->ip6_nxt, &sample->dcd_ipProtocol, 1);
        /* XXX: class ID ? */
        memcpy(&((struct my_tlhdr *)pptrsv->v6.tlh_ptr)->src_port, &dcd_sport, 2); 
        memcpy(&((struct my_tlhdr *)pptrsv->v6.tlh_ptr)->dst_port, &dcd_dport, 2);
      }
      pptrsv->v6.l4_proto = sample->dcd_ipProtocol;

      if (config.pre_tag_map) pptrsv->v6.tag = SF_find_id(&pptrsv->v6);
      exec_plugins(&pptrsv->v6);
      break;
#endif
    case NF9_FTYPE_VLAN_IPV4:
      if (req->bpf_filter) {
        reset_mac_vlan(&pptrsv->vlan4);
        reset_ip4(&pptrsv->vlan4);

        memcpy(pptrsv->vlan4.mac_ptr+ETH_ADDR_LEN, &sample->eth_src, ETH_ADDR_LEN); 
        memcpy(pptrsv->vlan4.mac_ptr, &sample->eth_dst, ETH_ADDR_LEN); 
        memcpy(pptrsv->vlan4.vlan_ptr, &in_vlan, 2); 
	((struct my_iphdr *)pptrsv->vlan4.iph_ptr)->ip_vhl = 0x45;
        memcpy(&((struct my_iphdr *)pptrsv->vlan4.iph_ptr)->ip_src, &sample->dcd_srcIP, 4);
        memcpy(&((struct my_iphdr *)pptrsv->vlan4.iph_ptr)->ip_dst, &sample->dcd_dstIP, 4);
        memcpy(&((struct my_iphdr *)pptrsv->vlan4.iph_ptr)->ip_p, &sample->dcd_ipProtocol, 1);
        memcpy(&((struct my_iphdr *)pptrsv->vlan4.iph_ptr)->ip_tos, &sample->dcd_ipTos, 1); 
        memcpy(&((struct my_tlhdr *)pptrsv->vlan4.tlh_ptr)->src_port, &dcd_sport, 2);
        memcpy(&((struct my_tlhdr *)pptrsv->vlan4.tlh_ptr)->dst_port, &dcd_dport, 2);
      }
      pptrsv->vlan4.l4_proto = sample->dcd_ipProtocol;

      if (config.pre_tag_map) pptrsv->vlan4.tag = SF_find_id(&pptrsv->vlan4);
      exec_plugins(&pptrsv->vlan4);
      break;
#if defined ENABLE_IPV6
    case NF9_FTYPE_VLAN_IPV6:
      if (req->bpf_filter) {
        reset_mac_vlan(&pptrsv->vlan6);
        reset_ip6(&pptrsv->vlan6);

        memcpy(pptrsv->vlan6.mac_ptr+ETH_ADDR_LEN, &sample->eth_src, ETH_ADDR_LEN);
        memcpy(pptrsv->vlan6.mac_ptr, &sample->eth_dst, ETH_ADDR_LEN); 
        memcpy(pptrsv->vlan6.vlan_ptr, &in_vlan, 2); 
	((struct ip6_hdr *)pptrsv->vlan6.iph_ptr)->ip6_ctlun.ip6_un2_vfc = 0x60;
        memcpy(&((struct ip6_hdr *)pptrsv->vlan6.iph_ptr)->ip6_src, &sample->ipsrc.address.ip_v6, IP6AddrSz); 
        memcpy(&((struct ip6_hdr *)pptrsv->vlan6.iph_ptr)->ip6_dst, &sample->ipdst.address.ip_v6, IP6AddrSz);
        memcpy(&((struct ip6_hdr *)pptrsv->vlan6.iph_ptr)->ip6_nxt, &sample->dcd_ipProtocol, 1); 
        /* XXX: class ID ? */
        memcpy(&((struct my_tlhdr *)pptrsv->vlan6.tlh_ptr)->src_port, &dcd_sport, 2); 
        memcpy(&((struct my_tlhdr *)pptrsv->vlan6.tlh_ptr)->dst_port, &dcd_dport, 2); 
      }
      pptrsv->vlan6.l4_proto = sample->dcd_ipProtocol;

      if (config.pre_tag_map) pptrsv->vlan6.tag = SF_find_id(&pptrsv->vlan6);
      exec_plugins(&pptrsv->vlan6);
      break;
#endif
    case NF9_FTYPE_MPLS_IPV4:
      if (req->bpf_filter) {
        u_char *ptr = pptrsv->mpls4.mpls_ptr;
        u_int32_t label, idx;

        /* XXX: fix caplen */
        reset_mac(&pptrsv->mpls4);

        memcpy(pptrsv->mpls4.mac_ptr+ETH_ADDR_LEN, &sample->eth_src, ETH_ADDR_LEN); 
        memcpy(pptrsv->mpls4.mac_ptr, &sample->eth_dst, ETH_ADDR_LEN); 

        for (idx = 0; idx <= sample->lstk.depth && idx < 10; idx++) { 
          label = htonl(sample->lstk.stack[idx]);
          memcpy(ptr, &label, 4);
          ptr += 4;
        }
	stick_bosbit(ptr-4);
        pptrsv->mpls4.iph_ptr = ptr;
        pptrsv->mpls4.tlh_ptr = ptr + IP4HdrSz;
        reset_ip4(&pptrsv->mpls4);
	
	((struct my_iphdr *)pptrsv->mpls4.iph_ptr)->ip_vhl = 0x45;
        memcpy(&((struct my_iphdr *)pptrsv->mpls4.iph_ptr)->ip_src, &sample->dcd_srcIP, 4);
        memcpy(&((struct my_iphdr *)pptrsv->mpls4.iph_ptr)->ip_dst, &sample->dcd_dstIP, 4); 
        memcpy(&((struct my_iphdr *)pptrsv->mpls4.iph_ptr)->ip_p, &sample->dcd_ipProtocol, 1); 
        memcpy(&((struct my_iphdr *)pptrsv->mpls4.iph_ptr)->ip_tos, &sample->dcd_ipTos, 1); 
        memcpy(&((struct my_tlhdr *)pptrsv->mpls4.tlh_ptr)->src_port, &dcd_sport, 2); 
        memcpy(&((struct my_tlhdr *)pptrsv->mpls4.tlh_ptr)->dst_port, &dcd_dport, 2); 
      }
      pptrsv->mpls4.l4_proto = sample->dcd_ipProtocol;

      if (config.pre_tag_map) pptrsv->mpls4.tag = SF_find_id(&pptrsv->mpls4);
      exec_plugins(&pptrsv->mpls4);
      break;
#if defined ENABLE_IPV6
    case NF9_FTYPE_MPLS_IPV6:
      if (req->bpf_filter) {
        u_char *ptr = pptrsv->mpls6.mpls_ptr;
        u_int32_t label, idx;

        /* XXX: fix caplen */
        reset_mac(&pptrsv->mpls6);
        memcpy(pptrsv->mpls6.mac_ptr+ETH_ADDR_LEN, &sample->eth_src, ETH_ADDR_LEN); 
        memcpy(pptrsv->mpls6.mac_ptr, &sample->eth_dst, ETH_ADDR_LEN); 

	for (idx = 0; idx <= sample->lstk.depth && idx < 10; idx++) {
	  label = htonl(sample->lstk.stack[idx]);
	  memcpy(ptr, &label, 4);
	  ptr += 4;
	}
	stick_bosbit(ptr-4);
        pptrsv->mpls6.iph_ptr = ptr;
        pptrsv->mpls6.tlh_ptr = ptr + IP6HdrSz;
        reset_ip6(&pptrsv->mpls6);

	((struct ip6_hdr *)pptrsv->mpls6.iph_ptr)->ip6_ctlun.ip6_un2_vfc = 0x60;
        memcpy(&((struct ip6_hdr *)pptrsv->mpls6.iph_ptr)->ip6_src, &sample->ipsrc.address.ip_v6, IP6AddrSz); 
        memcpy(&((struct ip6_hdr *)pptrsv->mpls6.iph_ptr)->ip6_dst, &sample->ipdst.address.ip_v6, IP6AddrSz); 
        memcpy(&((struct ip6_hdr *)pptrsv->mpls6.iph_ptr)->ip6_nxt, &sample->dcd_ipProtocol, 1); 
        /* XXX: class ID ? */
        memcpy(&((struct my_tlhdr *)pptrsv->mpls6.tlh_ptr)->src_port, &dcd_sport, 2); 
        memcpy(&((struct my_tlhdr *)pptrsv->mpls6.tlh_ptr)->dst_port, &dcd_dport, 2);
      }
      pptrsv->mpls6.l4_proto = sample->dcd_ipProtocol;

      if (config.pre_tag_map) pptrsv->mpls6.tag = SF_find_id(&pptrsv->mpls6);
      exec_plugins(&pptrsv->mpls6);
      break;
#endif
    case NF9_FTYPE_VLAN_MPLS_IPV4:
      if (req->bpf_filter) {
        u_char *ptr = pptrsv->vlanmpls4.mpls_ptr;
        u_int32_t label, idx;

        /* XXX: fix caplen */
        reset_mac_vlan(&pptrsv->vlanmpls4);
        memcpy(pptrsv->vlanmpls4.mac_ptr+ETH_ADDR_LEN, &sample->eth_src, ETH_ADDR_LEN); 
        memcpy(pptrsv->vlanmpls4.mac_ptr, &sample->eth_dst, ETH_ADDR_LEN); 
        memcpy(pptrsv->vlanmpls4.vlan_ptr, &in_vlan, 2); 

	for (idx = 0; idx <= sample->lstk.depth && idx < 10; idx++) {
	  label = htonl(sample->lstk.stack[idx]);
	  memcpy(ptr, &label, 4);
	  ptr += 4;
	}
	stick_bosbit(ptr-4);
        pptrsv->vlanmpls4.iph_ptr = ptr;
        pptrsv->vlanmpls4.tlh_ptr = ptr + IP4HdrSz;
        reset_ip4(&pptrsv->vlanmpls4);

	((struct my_iphdr *)pptrsv->vlanmpls4.iph_ptr)->ip_vhl = 0x45;
        memcpy(&((struct my_iphdr *)pptrsv->vlanmpls4.iph_ptr)->ip_src, &sample->dcd_srcIP, 4); 
        memcpy(&((struct my_iphdr *)pptrsv->vlanmpls4.iph_ptr)->ip_dst, &sample->dcd_dstIP, 4);
        memcpy(&((struct my_iphdr *)pptrsv->vlanmpls4.iph_ptr)->ip_p, &sample->dcd_ipProtocol, 1);
        memcpy(&((struct my_iphdr *)pptrsv->vlanmpls4.iph_ptr)->ip_tos, &sample->dcd_ipTos, 1);
        memcpy(&((struct my_tlhdr *)pptrsv->vlanmpls4.tlh_ptr)->src_port, &dcd_sport, 2);
        memcpy(&((struct my_tlhdr *)pptrsv->vlanmpls4.tlh_ptr)->dst_port, &dcd_dport, 2);
      }
      pptrsv->vlanmpls4.l4_proto = sample->dcd_ipProtocol;

      if (config.pre_tag_map) pptrsv->vlanmpls4.tag = SF_find_id(&pptrsv->vlanmpls4);
      exec_plugins(&pptrsv->vlanmpls4);
      break;
#if defined ENABLE_IPV6
    case NF9_FTYPE_VLAN_MPLS_IPV6:
      if (req->bpf_filter) {
        u_char *ptr = pptrsv->vlanmpls6.mpls_ptr;
        u_int32_t label, idx;

        /* XXX: fix caplen */
        reset_mac_vlan(&pptrsv->vlanmpls6);
        memcpy(pptrsv->vlanmpls6.mac_ptr+ETH_ADDR_LEN, &sample->eth_src, ETH_ADDR_LEN); 
        memcpy(pptrsv->vlanmpls6.mac_ptr, &sample->eth_dst, ETH_ADDR_LEN); 
        memcpy(pptrsv->vlanmpls6.vlan_ptr, &in_vlan, 2); 

	for (idx = 0; idx <= sample->lstk.depth && idx < 10; idx++) {
	  label = htonl(sample->lstk.stack[idx]);
	  memcpy(ptr, &label, 4);
	  ptr += 4;
	}
	stick_bosbit(ptr-4);
        pptrsv->vlanmpls6.iph_ptr = ptr;
        pptrsv->vlanmpls6.tlh_ptr = ptr + IP6HdrSz;
        reset_ip6(&pptrsv->vlanmpls6);

	((struct ip6_hdr *)pptrsv->vlanmpls6.iph_ptr)->ip6_ctlun.ip6_un2_vfc = 0x60;
        memcpy(&((struct ip6_hdr *)pptrsv->vlanmpls6.iph_ptr)->ip6_src, &sample->ipsrc.address.ip_v6, IP6AddrSz); 
        memcpy(&((struct ip6_hdr *)pptrsv->vlanmpls6.iph_ptr)->ip6_dst, &sample->ipdst.address.ip_v6, IP6AddrSz); 
        memcpy(&((struct ip6_hdr *)pptrsv->vlanmpls6.iph_ptr)->ip6_nxt, &sample->dcd_ipProtocol, 1); 
        /* XXX: class ID ? */
        memcpy(&((struct my_tlhdr *)pptrsv->vlanmpls6.tlh_ptr)->src_port, &dcd_sport, 2); 
        memcpy(&((struct my_tlhdr *)pptrsv->vlanmpls6.tlh_ptr)->dst_port, &dcd_dport, 2);
      }
      pptrsv->vlanmpls6.l4_proto = sample->dcd_ipProtocol;

      if (config.pre_tag_map) pptrsv->vlanmpls6.tag = SF_find_id(&pptrsv->vlanmpls6);
      exec_plugins(&pptrsv->vlanmpls6);
      break;
#endif
    default:
      break;
    }
  }
}

int SF_find_id(struct packet_ptrs *pptrs)
{
  struct id_table *t = (struct id_table *)pptrs->idtable;
  SFSample *sample = (SFSample *)pptrs->f_data; 
  int x, j, id, stop;

  /* The id_table is shared between by IPv4 and IPv6 sFlow collectors.
     IPv4 ones are in the lower part (0..x), IPv6 ones are in the upper
     part (x+1..end)
  */

  id = 0;
  if (sample->agent_addr.type == SFLADDRESSTYPE_IP_V4) {
    for (x = 0; x < t->ipv4_num; x++) {
      if (t->e[x].agent_ip.address.ipv4.s_addr == sample->agent_addr.address.ip_v4.s_addr) {
        for (j = 0, stop = 0; !stop; j++) stop = (*t->e[x].func[j])(pptrs, &id, &t->e[x]);
        if (id) break;
      }
      else if (t->e[x].agent_ip.address.ipv4.s_addr > sample->agent_addr.address.ip_v4.s_addr) break;
    }
  }
#if defined ENABLE_IPV6
  else if (sample->agent_addr.type == SFLADDRESSTYPE_IP_V6) { 
    for (x = (t->num-t->ipv6_num); x < t->num; x++) {
      if (!ip6_addr_cmp(&t->e[x].agent_ip.address.ipv6, &sample->agent_addr.address.ip_v6)) {
        for (j = 0, stop = 0; !stop; j++) stop = (*t->e[x].func[j])(pptrs, &id, &t->e[x]);
        if (id) break;
      }
      else if (ip6_addr_cmp(&t->e[x].agent_ip.address.ipv6, &sample->agent_addr.address.ip_v6) > 0)
        break;
    }
  }
#endif
  return id;
}

u_int16_t SF_evaluate_flow_type(struct packet_ptrs *pptrs)
{
  SFSample *sample = (SFSample *)pptrs->f_data;
  u_int8_t ret = 0;

  if (sample->in_vlan) ret += NF9_FTYPE_VLAN;
  if (sample->lstk.depth > 0) ret += NF9_FTYPE_MPLS;
  if (sample->gotIPV4); 
  else if (sample->gotIPV6) ret += NF9_FTYPE_IPV6;

  return ret;
}

void reset_mac(struct packet_ptrs *pptrs)
{
  memset(pptrs->mac_ptr, 0, 2*ETH_ADDR_LEN);
}

void reset_mac_vlan(struct packet_ptrs *pptrs)
{
  memset(pptrs->mac_ptr, 0, 2*ETH_ADDR_LEN);
  memset(pptrs->vlan_ptr, 0, 2);
}

void reset_ip4(struct packet_ptrs *pptrs)
{
  memset(pptrs->iph_ptr, 0, IP4TlSz);
  Assign8(((struct my_iphdr *)pptrs->iph_ptr)->ip_vhl, 5);
}

#if defined ENABLE_IPV6
void reset_ip6(struct packet_ptrs *pptrs)
{
  memset(pptrs->iph_ptr, 0, IP6TlSz);
  Assign16(((struct ip6_hdr *)pptrs->iph_ptr)->ip6_plen, htons(100));
  Assign16(((struct ip6_hdr *)pptrs->iph_ptr)->ip6_hlim, htons(64));
}
#endif

/* dummy functions; their use is limited to solve a trivial dependency */ 
int ip_handler(register struct packet_ptrs *pptrs)
{
}

int ip6_handler(register struct packet_ptrs *pptrs)
{
}

/*
 * $Id: nemesis-rip.c,v 1.2 2004/03/06 22:13:30 jnathan Exp $
 *
 * THE NEMESIS PROJECT
 * Copyright (C) 1999, 2000 Mark Grimes <mark@stateful.net>
 * Copyright (C) 2001 - 2003 Jeff Nathan <jeff@snort.org>
 *
 * nemesis-rip.c (RIP Packet Injector)
 *
 */

#include "nemesis.h"

static ETHERhdr etherhdr;
static IPhdr iphdr;
static UDPhdr udphdr;
static RIPhdr riphdr;
static FileData pd, ipod;
static int got_payload, got_domain;
static char *payloadfile = NULL;   /* payload file name */
static char *ipoptionsfile = NULL; /* IP options file name */
static char *device = NULL;        /* Ethernet device */

static void rip_cmdline(int, char **);
static int rip_exit(int);
static void rip_initdata(void);
static void rip_usage(char *);
static void rip_validatedata(void);
static void rip_verbose(void);

int buildrip(ETHERhdr *eth, IPhdr *ip, UDPhdr *udp, RIPhdr *rip, FileData *pd,
             FileData *ipod, char *device) {
    int n;
    u_int32_t rip_packetlen = 0, rip_meta_packetlen = 0;
    static u_int8_t *pkt;
    static int sockfd = -1;
    struct libnet_link_int *l2 = NULL;
    u_int8_t link_offset = 0;
    int sockbuff = IP_MAXPACKET;

    if (pd->file_mem == NULL) pd->file_s = 0;
    if (ipod->file_mem == NULL) ipod->file_s = 0;

    if (got_link) /* data link layer transport */
    {
        if ((l2 = libnet_open_link_interface(device, errbuf)) == NULL) {
            nemesis_device_failure(INJECTION_LINK, (const char *)device);
            return -1;
        }
        link_offset = LIBNET_ETH_H;
    } else {
        if ((sockfd = libnet_open_raw_sock(IPPROTO_RAW)) < 0) {
            nemesis_device_failure(INJECTION_RAW, (const char *)NULL);
            return -1;
        }
        if ((setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, (const void *)&sockbuff,
                        sizeof(sockbuff))) < 0) {
            fprintf(stderr, "ERROR: setsockopt() failed.\n");
            return -1;
        }
    }

    rip_packetlen = link_offset + LIBNET_IP_H + LIBNET_UDP_H + LIBNET_RIP_H +
                    pd->file_s + ipod->file_s;

    rip_meta_packetlen = rip_packetlen - (link_offset + LIBNET_IP_H);

#ifdef DEBUG
    printf("DEBUG: RIP packet length %u.\n", rip_packetlen);
    printf("DEBUG:  IP options size  %u.\n", ipod->file_s);
    printf("DEBUG: RIP payload size  %u.\n", pd->file_s);
#endif

    if (libnet_init_packet(rip_packetlen, &pkt) == -1) {
        fprintf(stderr, "ERROR: Unable to allocate packet memory.\n");
        return -1;
    }

    if (got_link)
        libnet_build_ethernet(eth->ether_dhost, eth->ether_shost, ETHERTYPE_IP,
                              NULL, 0, pkt);

    libnet_build_ip(rip_meta_packetlen, ip->ip_tos, ip->ip_id, ip->ip_off,
                    ip->ip_ttl, ip->ip_p, ip->ip_src.s_addr, ip->ip_dst.s_addr,
                    NULL, 0, pkt + link_offset);

    libnet_build_udp(udp->uh_sport, udp->uh_dport, NULL, 0,
                     pkt + link_offset + LIBNET_IP_H);

    libnet_build_rip(rip->cmd, rip->ver, rip->rd, rip->af, rip->rt,
                     ntohl(rip->addr), ntohl(rip->mask), ntohl(rip->next_hop),
                     rip->metric, pd->file_mem, pd->file_s,
                     pkt + link_offset + LIBNET_IP_H + LIBNET_UDP_H);

    if (got_ipoptions) {
        if ((libnet_insert_ipo((struct ipoption *)ipod->file_mem, ipod->file_s,
                               pkt + link_offset)) == -1) {
            fprintf(stderr,
                    "ERROR: Unable to add IP options, discarding "
                    "them.\n");
        }
    }

    if (got_link)
        libnet_do_checksum(pkt + LIBNET_ETH_H, IPPROTO_IP,
                           LIBNET_IP_H + ipod->file_s);

    libnet_do_checksum(pkt + link_offset, IPPROTO_UDP,
                       LIBNET_UDP_H + pd->file_s + ipod->file_s);

    if (got_link)
        n = libnet_write_link_layer(l2, device, pkt, rip_packetlen);
    else
        n = libnet_write_ip(sockfd, pkt, rip_packetlen);

    if (verbose == 2) nemesis_hexdump(pkt, rip_packetlen, HEX_ASCII_DECODE);
    if (verbose == 3) nemesis_hexdump(pkt, rip_packetlen, HEX_RAW_DECODE);

    if (n != rip_packetlen) {
        fprintf(stderr,
                "ERROR: Incomplete packet injection.  Only wrote "
                "%d bytes.\n",
                n);
    } else {
        if (verbose) {
            if (got_link)
                printf("Wrote %d byte RIP packet through linktype %s.\n", n,
                       nemesis_lookup_linktype(l2->linktype));
            else
                printf("Wrote %d byte RIP packet.\n", n);
        }
    }
    libnet_destroy_packet(&pkt);
    if (got_link)
        libnet_close_link_interface(l2);
    else
        libnet_close_raw_sock(sockfd);
    return n;
}

void nemesis_rip(int argc, char **argv) {
    if (argc > 1 && !strncmp(argv[1], "help", 4)) rip_usage(argv[0]);

    if (nemesis_seedrand() < 0)
        fprintf(stderr, "ERROR: Unable to seed random number generator.\n");

    rip_initdata();
    rip_cmdline(argc, argv);
    rip_validatedata();
    rip_verbose();

    if (got_payload) {
        if (builddatafromfile(
                    ((got_link == 1) ? RIP_LINKBUFFSIZE : RIP_RAWBUFFSIZE), &pd,
                    (const char *)payloadfile, (const u_int32_t)PAYLOADMODE) < 0)
            rip_exit(1);
    }

    if (got_ipoptions) {
        if (builddatafromfile(OPTIONSBUFFSIZE, &ipod, (const char *)ipoptionsfile,
                              (const u_int32_t)OPTIONSMODE) < 0)
            rip_exit(1);
    }

    if (buildrip(&etherhdr, &iphdr, &udphdr, &riphdr, &pd, &ipod, device) < 0) {
        puts("\nRIP Injection Failure");
        rip_exit(1);
    } else {
        puts("\nRIP Packet Injected");
        rip_exit(0);
    }
}

static void rip_initdata(void) {
    /* defaults */
    etherhdr.ether_type = ETHERTYPE_IP;    /* Ethernet type IP */
    memset(etherhdr.ether_shost, 0, 6);    /* Ethernet source address */
    memset(etherhdr.ether_dhost, 0xff, 6); /* Ethernet destination address */
    memset(&iphdr.ip_src.s_addr, 0, 4);    /* IP source address */
    memset(&iphdr.ip_dst.s_addr, 0, 4);    /* IP destination address */
    iphdr.ip_tos = IPTOS_RELIABILITY;      /* IP type of service */
    iphdr.ip_id = (u_int16_t)libnet_get_prand(PRu16); /* IP ID */
    iphdr.ip_p = IPPROTO_UDP;                         /* IP protocol UDP */
    iphdr.ip_off = 0;            /* IP fragmentation offset */
    iphdr.ip_ttl = 255;          /* IP TTL */
    udphdr.uh_sport = 520;       /* UDP source port */
    udphdr.uh_dport = 520;       /* UDP destination port */
    riphdr.cmd = RIPCMD_REQUEST; /* RIP command */
    riphdr.ver = 2;              /* RIP version */
    riphdr.rd = 0;               /* RIP routing domain */
    riphdr.af = 2;               /* RIP address family */
    riphdr.rt = (u_int16_t)libnet_get_prand(PRu16);
    /* RIP route tag */
    riphdr.addr = 0;     /* RIP address */
    riphdr.mask = 0;     /* RIP subnet mask */
    riphdr.next_hop = 0; /* RIP next-hop IP address */
    riphdr.metric = 1;   /* RIP metric */
    pd.file_mem = NULL;
    pd.file_s = 0;
    ipod.file_mem = NULL;
    ipod.file_s = 0;
    return;
}

static void rip_validatedata(void) {
    struct sockaddr_in sin;
    u_int32_t tmp;

    /* validation tests */
    if (riphdr.ver == 2) {
        /* allow routing domain 0 in RIP2 if specified by the user */
        if (riphdr.rd == 0 && got_domain == 0)
            riphdr.rd = (u_int16_t)libnet_get_prand(PRu16);
        if (riphdr.mask == 0)
            nemesis_name_resolve("255.255.255.0", (u_int32_t *)&riphdr.mask);
    }

    if (iphdr.ip_src.s_addr == 0)
        iphdr.ip_src.s_addr = (u_int32_t)libnet_get_prand(PRu32);
    if (iphdr.ip_dst.s_addr == 0) {
        switch (riphdr.ver) {
        case 1:
            tmp = (u_int32_t)libnet_get_prand(PRu32);
            iphdr.ip_dst.s_addr = (htonl(tmp) | 0xFF000000);
            break;
        case 2:
            /* The multicast address for RIP2 is RIP2-ROUTERS.MCAST.NET */
            nemesis_name_resolve("224.0.0.9", (u_int32_t *)&iphdr.ip_dst.s_addr);
            break;
        default:
            iphdr.ip_dst.s_addr = (u_int32_t)libnet_get_prand(PRu32);
            break;
        }
    }

    if (riphdr.addr == 0) riphdr.addr = (u_int32_t)libnet_get_prand(PRu32);

    /* if the user has supplied a source hardware addess but not a device
     * try to select a device automatically
     */
    if (memcmp(etherhdr.ether_shost, zero, 6) && !got_link && !device) {
        if (libnet_select_device(&sin, &device, (char *)&errbuf) < 0) {
            fprintf(stderr,
                    "ERROR: Device not specified and unable to "
                    "automatically select a device.\n");
            rip_exit(1);
        } else {
#ifdef DEBUG
            printf("DEBUG: automatically selected device: "
                   "       %s\n",
                   device);
#endif
            got_link = 1;
        }
    }

    /* if a device was specified and the user has not specified a source
     * hardware address, try to determine the source address automatically
     */
    if (got_link) {
        if ((nemesis_check_link(&etherhdr, device)) < 0) {
            fprintf(stderr, "ERROR: cannot retrieve hardware address of %s.\n",
                    device);
            rip_exit(1);
        }
    }

    /* Attempt to send valid packets if the user hasn't decided to craft an
     * anomolous packet
     */
    return;
}

static void rip_usage(char *arg) {
    printf("RIP usage:\n  %s [-v (verbose)] [options]\n\n", arg);
    printf("RIP options: \n"
           "  -c <RIP command>\n"
           "  -V <RIP version>\n"
           "  -r <RIP routing domain>\n"
           "  -a <RIP address family>\n"
           "  -R <RIP route tag>\n"
           "  -i <RIP route address>\n"
           "  -k <RIP network address mask>\n"
           "  -h <RIP next hop address>\n"
           "  -m <RIP metric>\n"
           "  -P <Payload file>\n\n");
    printf("UDP options:\n"
           "  -x <Source port>\n"
           "  -y <Destination port>\n\n");
    printf("IP options: \n"
           "  -S <Source IP address>\n"
           "  -D <Destination IP address>\n"
           "  -I <IP ID>\n"
           "  -T <IP TTL>\n"
           "  -t <IP TOS>\n"
           "  -F <IP fragmentation options>\n"
           "     -F[D],[M],[R],[offset]\n"
           "  -O <IP options file>\n\n");
    printf("Data Link Options: \n"
           "  -d <Ethernet device name>\n"
           "  -H <Source MAC address>\n"
           "  -M <Destination MAC address>\n");
    putchar('\n');
    rip_exit(1);
}

static void rip_cmdline(int argc, char **argv) {
    int opt, i;
    u_int32_t addr_tmp[6];
    char *rip_options;
    extern char *optarg;
    extern int optind;

#if defined(ENABLE_PCAPOUTPUT)
    rip_options = "a:c:d:D:F:h:H:i:I:k:m:M:O:P:r:R:S:t:T:V:x:y:vW?";
#else
    rip_options = "a:c:d:D:F:h:H:i:I:k:m:M:O:P:r:R:S:t:T:V:x:y:v?";
#endif

    while ((opt = getopt(argc, argv, rip_options)) != -1) {
        switch (opt) {
        case 'a': /* RIP address family */
            riphdr.af = xgetint16(optarg);
            break;
        case 'c': /* RIP command */
            riphdr.cmd = xgetint8(optarg);
            break;
        case 'd': /* Ethernet device */
            if (strlen(optarg) < 256) {
                device = strdup(optarg);
                got_link = 1;
            } else {
                fprintf(stderr, "ERROR: device %s > 256 characters.\n", optarg);
                rip_exit(1);
            }
            break;
        case 'D': /* destination IP address */
            if ((nemesis_name_resolve(optarg,
                                      (u_int32_t *)&iphdr.ip_dst.s_addr)) < 0) {
                fprintf(stderr,
                        "ERROR: Invalid destination IP address: "
                        "\"%s\".\n",
                        optarg);
                rip_exit(1);
            }
            break;
        case 'F': /* IP fragmentation options */
            if (parsefragoptions(&iphdr, optarg) < 0) rip_exit(1);
            break;
        case 'h': /* RIP next hop address */
            if ((nemesis_name_resolve(optarg, (u_int32_t *)&riphdr.next_hop)) <
                0) {
                fprintf(stderr,
                        "ERROR: Invalid next hop IP address: "
                        "\"%s\".\n",
                        optarg);
                rip_exit(1);
            }
            break;
        case 'H': /* Ethernet source address */
            memset(addr_tmp, 0, sizeof(addr_tmp));
            sscanf(optarg, "%02X:%02X:%02X:%02X:%02X:%02X", &addr_tmp[0],
                   &addr_tmp[1], &addr_tmp[2], &addr_tmp[3], &addr_tmp[4],
                   &addr_tmp[5]);
            for (i = 0; i < 6; i++)
                etherhdr.ether_shost[i] = (u_int8_t)addr_tmp[i];
            break;
        case 'i': /* RIP route address */
            if ((nemesis_name_resolve(optarg, (u_int32_t *)&riphdr.addr)) < 0) {
                fprintf(stderr,
                        "ERROR: Invalid destination IP address: "
                        "\"%s\".\n",
                        optarg);
                rip_exit(1);
            }
            break;
        case 'I': /* IP ID */
            iphdr.ip_id = xgetint16(optarg);
            break;
        case 'k': /* RIP netmask address */
            if ((nemesis_name_resolve(optarg, (u_int32_t *)&riphdr.mask)) < 0) {
                fprintf(stderr,
                        "ERROR: Invalid RIP mask IP address: "
                        "\"%s\".\n",
                        optarg);
                rip_exit(1);
            }
            break;
        case 'm': /* RIP metric */
            riphdr.metric = xgetint32(optarg);
            break;
        case 'M': /* Ethernet destination address */
            memset(addr_tmp, 0, sizeof(addr_tmp));
            sscanf(optarg, "%02X:%02X:%02X:%02X:%02X:%02X", &addr_tmp[0],
                   &addr_tmp[1], &addr_tmp[2], &addr_tmp[3], &addr_tmp[4],
                   &addr_tmp[5]);
            for (i = 0; i < 6; i++)
                etherhdr.ether_dhost[i] = (u_int8_t)addr_tmp[i];
            break;
        case 'O': /* IP options file */
            if (strlen(optarg) < 256) {
                ipoptionsfile = strdup(optarg);
                got_ipoptions = 1;
            } else {
                fprintf(stderr,
                        "ERROR: IP options file %s > 256 "
                        "characters.\n",
                        optarg);
                rip_exit(1);
            }
            break;
        case 'P': /* payload file */
            if (strlen(optarg) < 256) {
                payloadfile = strdup(optarg);
                got_payload = 1;
            } else {
                fprintf(stderr,
                        "ERROR: payload file %s > 256 "
                        "characters.\n",
                        optarg);
                rip_exit(1);
            }
            break;
        case 'r': /* RIP routing domain */
            riphdr.rd = xgetint16(optarg);
            got_domain = 1;
            break;
        case 'R': /* RIP route tag */
            riphdr.rt = xgetint16(optarg);
            break;
        case 'S': /* source IP address */
            if ((nemesis_name_resolve(optarg,
                                      (u_int32_t *)&iphdr.ip_src.s_addr)) < 0) {
                fprintf(stderr,
                        "ERROR: Invalid source IP address: \"%s\"."
                        "\n",
                        optarg);
                rip_exit(1);
            }
            break;
        case 't': /* IP type of service */
            iphdr.ip_tos = xgetint8(optarg);
            break;
        case 'T': /* IP time to live */
            iphdr.ip_ttl = xgetint8(optarg);
            break;
        case 'v':
            verbose++;
            break;
        case 'V': /* RIP version */
            riphdr.ver = xgetint8(optarg);
            break;
        case 'x': /* UDP source port */
            udphdr.uh_sport = xgetint16(optarg);
            break;
        case 'y': /* UDP destination port */
            udphdr.uh_dport = xgetint16(optarg);
            break;
        case '?': /* FALLTHROUGH */
        default:
            rip_usage(argv[0]);
            break;
        }
    }
    argc -= optind;
    argv += optind;
    return;
}

static int rip_exit(int code) {
    if (got_payload) free(pd.file_mem);

    if (got_ipoptions) free(ipod.file_mem);

    if (device != NULL) free(device);

    if (ipoptionsfile != NULL) free(ipoptionsfile);

    if (payloadfile != NULL) free(payloadfile);

    exit(code);
}

static void rip_verbose(void) {
    if (verbose) {
        if (got_link) nemesis_printeth(&etherhdr);

        nemesis_printip(&iphdr);
        nemesis_printudp(&udphdr);
        nemesis_printrip(&riphdr);
    }
    return;
}

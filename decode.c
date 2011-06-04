/* darkstat 3
 * copyright (c) 2001-2011 Emil Mikulic.
 *
 * decode.c: packet decoding.
 *
 * Given a captured packet, decode it and fill out a pktsummary struct which
 * will be sent to the accounting code in acct.c
 *
 * You may use, modify and redistribute this file under the terms of the
 * GNU General Public License version 2. (see COPYING.GPL)
 */

#include "cdefs.h"
#include "acct.h"
#include "cap.h"
#include "config.h"
#include "decode.h"
#include "err.h"
#include "opt.h"

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <assert.h>
#include <pcap.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h> /* inet_ntoa() */

/* need struct ether_header */
#if defined(__NetBSD__) || defined(__OpenBSD__)
# include <sys/queue.h>
# include <net/if.h>
# include <net/if_arp.h>
# include <netinet/if_ether.h>
#else
# ifdef __sun
#  include <sys/ethernet.h>
#  define ETHER_HDR_LEN 14
# else
#  ifdef _AIX
#   include <netinet/if_ether.h>
#   define ETHER_HDR_LEN 14
#  else
#   include <net/ethernet.h>
#  endif
# endif
#endif
#ifndef ETHERTYPE_PPPOE
#define ETHERTYPE_PPPOE 0x8864
#endif

#ifndef ETHERTYPE_IPV6
# ifdef HAVE_NET_IF_ETHER_H
#  include <net/if_ether.h>	/* ETH_P_IPV6 for GNU/kfreebsd */
# endif
# ifdef ETH_P_IPV6
#  define ETHERTYPE_IPV6 ETH_P_IPV6
# endif
#endif

#include <net/if.h> /* struct ifreq */
#include <netinet/in_systm.h> /* n_long */
#include <netinet/ip.h> /* struct ip */
#include <netinet/ip6.h> /* struct ip6_hdr */
#define __FAVOR_BSD
#include <netinet/tcp.h> /* struct tcphdr */
#include <netinet/udp.h> /* struct udphdr */

static void decode_ether(u_char *, const struct pcap_pkthdr *,
   const u_char *);
static void decode_loop(u_char *, const struct pcap_pkthdr *,
   const u_char *);
static void decode_ppp(u_char *, const struct pcap_pkthdr *,
   const u_char *);
static void decode_pppoe(u_char *, const struct pcap_pkthdr *,
   const u_char *);
static void decode_pppoe_real(const u_char *pdata, const uint32_t len,
   struct pktsummary *sm);
static void decode_linux_sll(u_char *, const struct pcap_pkthdr *,
   const u_char *);
static void decode_raw(u_char *, const struct pcap_pkthdr *,
   const u_char *);
static void decode_ip(const u_char *pdata, const uint32_t len,
   struct pktsummary *sm);
static void decode_ipv6(const u_char *pdata, const uint32_t len,
   struct pktsummary *sm);

/* Link-type header information */
static const struct linkhdr linkhdrs[] = {
  /* linktype       hdrlen         handler       */
   { DLT_EN10MB,    ETHER_HDR_LEN, decode_ether  },
   { DLT_LOOP,      NULL_HDR_LEN,  decode_loop  },
   { DLT_NULL,      NULL_HDR_LEN,  decode_loop  },
   { DLT_PPP,       PPP_HDR_LEN,   decode_ppp },
#if defined(__NetBSD__)
   { DLT_PPP_SERIAL, PPP_HDR_LEN,  decode_ppp },
#endif
   { DLT_FDDI,      FDDI_HDR_LEN,  NULL },
   { DLT_PPP_ETHER, PPPOE_HDR_LEN, decode_pppoe },
#ifdef DLT_LINUX_SLL
   { DLT_LINUX_SLL, SLL_HDR_LEN,   decode_linux_sll },
#endif
   { DLT_RAW,       RAW_HDR_LEN,   decode_raw },
   { -1, 0, NULL }
};

/*
 * Returns a pointer to the linkhdr record matching the given linktype, or
 * NULL if no matching entry found.
 */
const struct linkhdr *
getlinkhdr(const int linktype)
{
   size_t i;

   for (i=0; linkhdrs[i].linktype != -1; i++)
      if (linkhdrs[i].linktype == linktype)
         return (&(linkhdrs[i]));
   return (NULL);
}

/*
 * Returns the minimum snaplen needed to decode everything up to the TCP/UDP
 * packet headers.  The IPv6 header is normative.  The argument lh is not
 * allowed to be NULL.
 */
int
getsnaplen(const struct linkhdr *lh)
{
   return (int)(lh->hdrlen + IPV6_HDR_LEN + max(TCP_HDR_LEN, UDP_HDR_LEN));
}

/* Decoding functions. */
static void
decode_ether(u_char *user _unused_,
      const struct pcap_pkthdr *pheader,
      const u_char *pdata)
{
   u_short type;
   const struct ether_header *hdr = (const struct ether_header *)pdata;
   struct pktsummary sm;
   memset(&sm, 0, sizeof(sm));
   sm.time = pheader->ts.tv_sec;

   if (pheader->caplen < ETHER_HDR_LEN) {
      verbosef("ether: packet too short (%u bytes)", pheader->caplen);
      return;
   }

#ifdef __sun
   memcpy(sm.src_mac, hdr->ether_shost.ether_addr_octet, sizeof(sm.src_mac));
   memcpy(sm.dst_mac, hdr->ether_dhost.ether_addr_octet, sizeof(sm.dst_mac));
#else
   memcpy(sm.src_mac, hdr->ether_shost, sizeof(sm.src_mac));
   memcpy(sm.dst_mac, hdr->ether_dhost, sizeof(sm.dst_mac));
#endif

   type = ntohs( hdr->ether_type );
   switch (type) {
   case ETHERTYPE_IP:
   case ETHERTYPE_IPV6:
      if (!opt_want_pppoe) {
         decode_ip(pdata + ETHER_HDR_LEN,
                   pheader->caplen - ETHER_HDR_LEN, &sm);
         acct_for(&sm);
      } else
         verbosef("ether: discarded IP packet, expecting PPPoE instead");
      break;
   case ETHERTYPE_ARP:
      /* known protocol, don't complain about it. */
      break;
   case ETHERTYPE_PPPOE:
      if (opt_want_pppoe)
         decode_pppoe_real(pdata + ETHER_HDR_LEN,
                           pheader->caplen - ETHER_HDR_LEN, &sm);
      else
         verbosef("ether: got PPPoE frame: maybe you want --pppoe");
      break;
   default:
      verbosef("ether: unknown protocol (0x%04x)", type);
   }
}

static void
decode_loop(u_char *user _unused_,
      const struct pcap_pkthdr *pheader,
      const u_char *pdata)
{
   uint32_t family;
   struct pktsummary sm;
   memset(&sm, 0, sizeof(sm));

   if (pheader->caplen < NULL_HDR_LEN) {
      verbosef("loop: packet too short (%u bytes)", pheader->caplen);
      return;
   }
   family = *(const uint32_t *)pdata;
#ifdef __OpenBSD__
   family = ntohl(family);
#endif
   if (family == AF_INET) {
      /* OpenBSD tun or FreeBSD tun or FreeBSD lo */
      decode_ip(pdata + NULL_HDR_LEN, pheader->caplen - NULL_HDR_LEN, &sm);
      sm.time = pheader->ts.tv_sec;
      acct_for(&sm);
   }
   else if (family == AF_INET6) {
      /* XXX: Check this! */
      decode_ip(pdata + NULL_HDR_LEN, pheader->caplen - NULL_HDR_LEN, &sm);
      sm.time = pheader->ts.tv_sec;
      acct_for(&sm);
   }
   else
      verbosef("loop: unknown family (%x)", family);
}

static void
decode_ppp(u_char *user _unused_,
      const struct pcap_pkthdr *pheader,
      const u_char *pdata)
{
   struct pktsummary sm;
   memset(&sm, 0, sizeof(sm));

   if (pheader->caplen < PPPOE_HDR_LEN) {
      verbosef("ppp: packet too short (%u bytes)", pheader->caplen);
      return;
   }

   if (pdata[2] == 0x00 && pdata[3] == 0x21) {
         decode_ip(pdata + PPP_HDR_LEN, pheader->caplen - PPP_HDR_LEN, &sm);
         sm.time = pheader->ts.tv_sec;
         acct_for(&sm);
   } else
      verbosef("non-IP PPP packet; ignoring.");
}

static void
decode_pppoe(u_char *user _unused_,
      const struct pcap_pkthdr *pheader,
      const u_char *pdata)
{
   struct pktsummary sm;
   memset(&sm, 0, sizeof(sm));
   sm.time = pheader->ts.tv_sec;
   decode_pppoe_real(pdata, pheader->caplen, &sm);
}

static void
decode_pppoe_real(const u_char *pdata, const uint32_t len,
   struct pktsummary *sm)
{
   if (len < PPPOE_HDR_LEN) {
      verbosef("pppoe: packet too short (%u bytes)", len);
      return;
   }

   if (pdata[1] != 0x00) {
      verbosef("pppoe: code = 0x%02x, expecting 0; ignoring.", pdata[1]);
      return;
   }

   if ((pdata[6] == 0xc0) && (pdata[7] == 0x21)) return; /* LCP */
   if ((pdata[6] == 0xc0) && (pdata[7] == 0x25)) return; /* LQR */

   if ((pdata[6] == 0x00) && (pdata[7] == 0x21)) {
      decode_ip(pdata + PPPOE_HDR_LEN, len - PPPOE_HDR_LEN, sm);
      acct_for(sm);
   } else
      verbosef("pppoe: non-IP PPPoE packet (0x%02x%02x); ignoring.",
         pdata[6], pdata[7]);
}

/* very similar to decode_ether ... */
static void
decode_linux_sll(u_char *user _unused_,
      const struct pcap_pkthdr *pheader,
      const u_char *pdata)
{
   const struct sll_header {
      uint16_t packet_type;
      uint16_t device_type;
      uint16_t addr_length;
#define SLL_MAX_ADDRLEN 8
      uint8_t addr[SLL_MAX_ADDRLEN];
      uint16_t ether_type;
   } *hdr = (const struct sll_header *)pdata;
   u_short type;
   struct pktsummary sm;
   memset(&sm, 0, sizeof(sm));

   if (pheader->caplen < SLL_HDR_LEN) {
      verbosef("linux_sll: packet too short (%u bytes)", pheader->caplen);
      return;
   }

   type = ntohs( hdr->ether_type );
   switch (type) {
   case ETHERTYPE_IP:
   case ETHERTYPE_IPV6:
      decode_ip(pdata + SLL_HDR_LEN, pheader->caplen - SLL_HDR_LEN, &sm);
      sm.time = pheader->ts.tv_sec;
      acct_for(&sm);
      break;
   case ETHERTYPE_ARP:
      /* known protocol, don't complain about it. */
      break;
   default:
      verbosef("linux_sll: unknown protocol (%04x)", type);
   }
}

static void
decode_raw(u_char *user _unused_,
      const struct pcap_pkthdr *pheader,
      const u_char *pdata)
{
   struct pktsummary sm;
   memset(&sm, 0, sizeof(sm));

   decode_ip(pdata, pheader->caplen, &sm);
   sm.time = pheader->ts.tv_sec;
   acct_for(&sm);
}

static void decode_ip_payload(const u_char *pdata, const uint32_t len,
      struct pktsummary *sm);

static void
decode_ip(const u_char *pdata, const uint32_t len, struct pktsummary *sm)
{
   const struct ip *hdr = (const struct ip *)pdata;

   if (hdr->ip_v == 6) {
      /* Redirect parsing of IPv6 packets. */
      decode_ipv6(pdata, len, sm);
      return;
   }
   if (len < IP_HDR_LEN) {
      verbosef("ip: packet too short (%u bytes)", len);
      return;
   }
   if (hdr->ip_v != 4) {
      verbosef("ip: version %d (expecting 4 or 6)", hdr->ip_v);
      return;
   }

   sm->len = ntohs(hdr->ip_len);
   sm->proto = hdr->ip_p;

   sm->src.family = IPv4;
   sm->src.ip.v4 = hdr->ip_src.s_addr;

   sm->dst.family = IPv4;
   sm->dst.ip.v4 = hdr->ip_dst.s_addr;

   decode_ip_payload(pdata + IP_HDR_LEN, len - IP_HDR_LEN, sm);
}

static void
decode_ipv6(const u_char *pdata, const uint32_t len, struct pktsummary *sm)
{
   const struct ip6_hdr *hdr = (const struct ip6_hdr *)pdata;

   if (len < IPV6_HDR_LEN) {
      verbosef("ipv6: packet too short (%u bytes)", len);
      return;
   }

   sm->len = ntohs(hdr->ip6_plen) + IPV6_HDR_LEN;
   sm->proto = hdr->ip6_nxt;

   sm->src.family = IPv6;
   memcpy(&sm->src.ip.v6, &hdr->ip6_src, sizeof(sm->src.ip.v6));

   sm->dst.family = IPv6;
   memcpy(&sm->dst.ip.v6, &hdr->ip6_dst, sizeof(sm->dst.ip.v6));

   decode_ip_payload(pdata + IPV6_HDR_LEN, len - IPV6_HDR_LEN, sm);
}

static void
decode_ip_payload(const u_char *pdata, const uint32_t len,
      struct pktsummary *sm)
{
   switch (sm->proto) {
      case IPPROTO_TCP: {
         const struct tcphdr *thdr = (const struct tcphdr *)pdata;
         if (len < TCP_HDR_LEN) {
            verbosef("tcp: packet too short (%u bytes)", len);
            sm->proto = IPPROTO_INVALID; /* don't do accounting! */
            return;
         }
         sm->src_port = ntohs(thdr->th_sport);
         sm->dst_port = ntohs(thdr->th_dport);
         sm->tcp_flags = thdr->th_flags &
            (TH_FIN|TH_SYN|TH_RST|TH_PUSH|TH_ACK|TH_URG);
         break;
      }

      case IPPROTO_UDP: {
         const struct udphdr *uhdr = (const struct udphdr *)pdata;
         if (len < UDP_HDR_LEN) {
            verbosef("udp: packet too short (%u bytes)", len);
            sm->proto = IPPROTO_INVALID; /* don't do accounting! */
            return;
         }
         sm->src_port = ntohs(uhdr->uh_sport);
         sm->dst_port = ntohs(uhdr->uh_dport);
         break;
      }

      case IPPROTO_ICMP:
      case IPPROTO_ICMPV6:
      case IPPROTO_AH:
      case IPPROTO_ESP:
      case IPPROTO_OSPF:
         /* known protocol, don't complain about it */
         break;

      default:
         verbosef("ip: unknown protocol %d", sm->proto);
   }
}

/* vim:set ts=3 sw=3 tw=78 expandtab: */

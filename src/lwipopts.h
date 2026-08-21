#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#define NO_SYS                        1
#define SYS_LIGHTWEIGHT_PROT          0
#define MEM_ALIGNMENT                 4
#define MEM_SIZE                      (16 * 1024)

#define LWIP_RAW                      1
#define LWIP_NETCONN                  0
#define LWIP_SOCKET                   0
#define LWIP_DHCP                     0
#define LWIP_DNS                      0
#define LWIP_ICMP                     1
#define LWIP_UDP                      1
#define LWIP_TCP                      1
#define LWIP_IPV4                     1
#define LWIP_IPV6                     0
#define LWIP_IGMP                     0
#define LWIP_AUTOIP                   0
#define LWIP_SNMP                     0
#define LWIP_STATS                    0
#define LWIP_NETIF_STATUS_CALLBACK    0
#define LWIP_NETIF_LINK_CALLBACK      0

#define ETH_PAD_SIZE                  0
#define LWIP_IP_ACCEPT_UDP_PORT(port) ((port) == PP_NTOHS(67))
#define ETHARP_SUPPORT_STATIC_ENTRIES 1
#define LWIP_SINGLE_NETIF             1

#define TCP_MSS                       1460
#define TCP_SND_BUF                   (4 * TCP_MSS)
#define TCP_WND                       (4 * TCP_MSS)
#define TCP_SND_QUEUELEN              16
#define MEMP_NUM_TCP_PCB              5
#define MEMP_NUM_TCP_PCB_LISTEN       1
#define MEMP_NUM_TCP_SEG              24
#define MEMP_NUM_UDP_PCB              3

#define PBUF_POOL_SIZE                6
#define PBUF_POOL_BUFSIZE             1536

#define LWIP_MULTICAST_PING           1
#define LWIP_BROADCAST_PING           1

#endif

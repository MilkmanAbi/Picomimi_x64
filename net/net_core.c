/**
 * Picomimi-x64 Network Core
 * net/net_core.c
 *
 * Implements:
 *   sk_buff lifecycle
 *   Network device registry
 *   ARP table (16-entry cache)
 *   IP send/receive dispatcher
 *   ICMP echo reply
 *   UDP send/receive
 *   Loopback interface (127.0.0.1)
 */

#include <kernel/types.h>
#include <net/socket.h>
#include <mm/slab.h>
#include <lib/printk.h>
#include <lib/string.h>


/* =========================================================
 * sk_buff
 * ========================================================= */

sk_buff_t *skb_alloc(u32 size) {
    sk_buff_t *skb = kzalloc(sizeof(sk_buff_t), GFP_KERNEL);
    if (!skb) return NULL;

    if (size <= SKB_MAX_DATA) {
        skb->head = skb->_buf;
    } else {
        skb->head = kmalloc(size, GFP_KERNEL);
        if (!skb->head) { kfree(skb); return NULL; }
    }

    skb->data      = skb->head;
    skb->tail      = skb->head;
    skb->end       = skb->head + size;
    skb->truesize  = size;
    skb->len       = 0;
    return skb;
}

void skb_free(sk_buff_t *skb) {
    if (!skb) return;
    if (skb->head != skb->_buf) kfree(skb->head);
    kfree(skb);
}

u8 *skb_put(sk_buff_t *skb, u32 len) {
    u8 *old = skb->tail;
    if (skb->tail + len > skb->end) return NULL;
    skb->tail += len;
    skb->len  += len;
    return old;
}

u8 *skb_push(sk_buff_t *skb, u32 len) {
    if (skb->data < skb->head + len) return NULL;
    skb->data -= len;
    skb->len  += len;
    return skb->data;
}

u8 *skb_pull(sk_buff_t *skb, u32 len) {
    if (len > skb->len) return NULL;
    skb->data += len;
    skb->len  -= len;
    return skb->data;
}

/* =========================================================
 * Network device registry
 * ========================================================= */

static net_device_t *netdev_list = NULL;
static spinlock_t    netdev_lock = {.raw_lock={0}};

void netdev_register(net_device_t *dev) {
    if (!dev) return;
    spin_lock(&netdev_lock);
    dev->next    = netdev_list;
    netdev_list  = dev;
    spin_unlock(&netdev_lock);
    printk(KERN_INFO "[net] registered device '%s' ip=%u.%u.%u.%u\n",
           dev->name,
           (dev->ip4 >> 0)  & 0xFF, (dev->ip4 >> 8)  & 0xFF,
           (dev->ip4 >> 16) & 0xFF, (dev->ip4 >> 24) & 0xFF);
}

net_device_t *netdev_get_by_name(const char *name) {
    for (net_device_t *d = netdev_list; d; d = d->next)
        if (strcmp(d->name, name) == 0) return d;
    return NULL;
}

net_device_t *netdev_get_default(void) {
    /* Return first non-loopback device, or loopback if nothing else */
    net_device_t *lo = NULL;
    for (net_device_t *d = netdev_list; d; d = d->next) {
        if (strcmp(d->name, "lo") == 0) { lo = d; continue; }
        if (d->up) return d;
    }
    return lo;
}

/* =========================================================
 * Checksum (ones-complement sum)
 * ========================================================= */

u16 ip_checksum(const void *data, u32 len) {
    const u16 *words = (const u16 *)data;
    u32 sum = 0;
    while (len > 1) { sum += *words++; len -= 2; }
    if (len) sum += *(const u8 *)words;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)(~sum);
}

static u16 tcp_udp_checksum(u32 src_ip, u32 dst_ip, u8 proto,
                              const void *payload, u16 plen) {
    struct pseudo {
        u32 src; u32 dst; u8 zero; u8 proto; u16 len;
    } hdr = { src_ip, dst_ip, 0, proto, htons(plen) };

    u32 sum = 0;
    /* Use memcpy to avoid unaligned access warning */
    u8 *ph = (u8 *)&hdr;
    for (u32 i = 0; i < sizeof(hdr); i += 2) {
        u16 v; __builtin_memcpy(&v, ph + i, 2); sum += v;
    }

    const u16 *p = (const u16 *)payload;
    u32 rem = plen;
    while (rem > 1) { sum += *p++; rem -= 2; }
    if (rem) sum += *(const u8 *)p;

    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)(~sum);
}

/* =========================================================
 * ARP table
 * ========================================================= */

#define ARP_CACHE_SIZE  16

typedef struct {
    u32 ip;
    u8  mac[6];
    bool valid;
} arp_entry_t;

static arp_entry_t arp_cache[ARP_CACHE_SIZE];
static spinlock_t  arp_lock = {.raw_lock={0}};

static void arp_cache_insert(u32 ip, const u8 *mac) {
    spin_lock(&arp_lock);
    /* Look for existing entry */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].ip == ip || !arp_cache[i].valid) {
            arp_cache[i].ip    = ip;
            arp_cache[i].valid = true;
            memcpy(arp_cache[i].mac, mac, 6);
            spin_unlock(&arp_lock);
            return;
        }
    }
    /* Evict slot 0 */
    arp_cache[0].ip    = ip;
    arp_cache[0].valid = true;
    memcpy(arp_cache[0].mac, mac, 6);
    spin_unlock(&arp_lock);
}

int arp_resolve(net_device_t *dev, u32 ip, u8 *mac_out) {
    /* Check loopback */
    if ((ip & 0xFF) == 0x7F) {
        memset(mac_out, 0, 6);
        return 0;
    }

    spin_lock(&arp_lock);
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            memcpy(mac_out, arp_cache[i].mac, 6);
            spin_unlock(&arp_lock);
            return 0;
        }
    }
    spin_unlock(&arp_lock);

    /* Send ARP request */
    arp_send_request(dev, ip);
    /* Spin-wait up to 100ms for ARP reply — naive but functional */
    extern volatile u64 jiffies;
    u64 deadline = jiffies + 10;
    while (jiffies < deadline) {
        spin_lock(&arp_lock);
        for (int i = 0; i < ARP_CACHE_SIZE; i++) {
            if (arp_cache[i].valid && arp_cache[i].ip == ip) {
                memcpy(mac_out, arp_cache[i].mac, 6);
                spin_unlock(&arp_lock);
                return 0;
            }
        }
        spin_unlock(&arp_lock);
        __asm__ volatile("pause");
    }
    return -EHOSTUNREACH;
}

int arp_send_request(net_device_t *dev, u32 target_ip) {
    if (!dev || !dev->transmit) return -ENODEV;

    sk_buff_t *skb = skb_alloc(sizeof(eth_header_t) + sizeof(arp_packet_t));
    if (!skb) return -ENOMEM;

    eth_header_t *eth = (eth_header_t *)skb_put(skb, sizeof(eth_header_t));
    memset(eth->dst_mac, 0xFF, 6);               /* broadcast */
    memcpy(eth->src_mac, dev->mac, 6);
    eth->ethertype = htons(ETH_P_ARP);

    arp_packet_t *arp = (arp_packet_t *)skb_put(skb, sizeof(arp_packet_t));
    arp->htype      = htons(1);                  /* Ethernet */
    arp->ptype      = htons(ETH_P_IP);
    arp->hlen       = 6;
    arp->plen       = 4;
    arp->operation  = htons(1);                  /* Request */
    memcpy(arp->sender_mac, dev->mac, 6);
    arp->sender_ip  = dev->ip4;
    memset(arp->target_mac, 0, 6);
    arp->target_ip  = target_ip;

    dev->transmit(dev, skb);
    skb_free(skb);
    return 0;
}

void arp_receive(net_device_t *dev, sk_buff_t *skb) {
    if (skb->len < sizeof(arp_packet_t)) return;
    arp_packet_t *arp = (arp_packet_t *)skb->data;

    arp_cache_insert(arp->sender_ip, arp->sender_mac);

    u16 op = ntohs(arp->operation);
    if (op == 1 && arp->target_ip == dev->ip4) {
        /* ARP request for us — send reply */
        sk_buff_t *reply = skb_alloc(sizeof(eth_header_t) + sizeof(arp_packet_t));
        if (!reply) return;

        eth_header_t *eth = (eth_header_t *)skb_put(reply, sizeof(eth_header_t));
        memcpy(eth->dst_mac, arp->sender_mac, 6);
        memcpy(eth->src_mac, dev->mac, 6);
        eth->ethertype = htons(ETH_P_ARP);

        arp_packet_t *rep = (arp_packet_t *)skb_put(reply, sizeof(arp_packet_t));
        rep->htype      = htons(1);
        rep->ptype      = htons(ETH_P_IP);
        rep->hlen       = 6; rep->plen = 4;
        rep->operation  = htons(2);
        memcpy(rep->sender_mac, dev->mac, 6);
        rep->sender_ip  = dev->ip4;
        memcpy(rep->target_mac, arp->sender_mac, 6);
        rep->target_ip  = arp->sender_ip;

        dev->transmit(dev, reply);
        skb_free(reply);
    }
}

/* =========================================================
 * IP send
 * ========================================================= */

static u16 ip_id_counter = 0x1234;

int ip_send(net_device_t *dev, u32 dst_ip, u8 proto, sk_buff_t *skb) {
    if (!dev) {
        dev = netdev_get_default();
        if (!dev) return -ENETDOWN;
    }

    /* Prepend IP header */
    ip_header_t *iph = (ip_header_t *)skb_push(skb, sizeof(ip_header_t));
    if (!iph) return -ENOMEM;

    u16 tot_len = (u16)(skb->len);
    iph->version_ihl = 0x45;           /* IPv4, 20-byte header */
    iph->tos         = 0;
    iph->tot_len     = htons(tot_len);
    iph->id          = htons(ip_id_counter++);
    iph->frag_off    = 0;
    iph->ttl         = 64;
    iph->protocol    = proto;
    iph->checksum    = 0;
    iph->src_ip      = dev->ip4;
    iph->dst_ip      = dst_ip;
    iph->checksum    = ip_checksum(iph, sizeof(ip_header_t));

    /* Prepend Ethernet header */
    eth_header_t *eth = (eth_header_t *)skb_push(skb, sizeof(eth_header_t));
    if (!eth) return -ENOMEM;

    u8 dst_mac[6];
    u32 nexthop = dst_ip;
    /* Check if dst is on same subnet */
    if ((dst_ip & dev->netmask) != (dev->ip4 & dev->netmask))
        nexthop = dev->gateway;

    int r = arp_resolve(dev, nexthop, dst_mac);
    if (r < 0) {
        memset(dst_mac, 0xFF, 6);  /* Broadcast fallback */
    }

    memcpy(eth->dst_mac, dst_mac, 6);
    memcpy(eth->src_mac, dev->mac, 6);
    eth->ethertype = htons(ETH_P_IP);

    return dev->transmit ? dev->transmit(dev, skb) : -ENODEV;
}

/* =========================================================
 * IP receive dispatcher
 * ========================================================= */

void ip_receive(net_device_t *dev, sk_buff_t *skb) {
    if (skb->len < sizeof(ip_header_t)) return;
    ip_header_t *iph = (ip_header_t *)skb->data;

    u8 ihl = (iph->version_ihl & 0x0F) * 4;
    if (ihl < 20 || ihl > skb->len) return;

    /* Verify checksum */
    u16 saved = iph->checksum;
    iph->checksum = 0;
    if (ip_checksum(iph, ihl) != saved) {
        printk(KERN_DEBUG "[ip] bad checksum, dropping\n");
        return;
    }
    iph->checksum = saved;

    skb->src_ip  = ntohl(iph->src_ip);
    skb->dst_ip  = ntohl(iph->dst_ip);
    skb->protocol = iph->protocol;

    skb_pull(skb, ihl);

    switch (iph->protocol) {
    case IPPROTO_ICMP:
        icmp_receive(dev, skb);
        break;
    case IPPROTO_UDP:
        udp_receive(dev, skb);
        break;
    case IPPROTO_TCP:
        extern void tcp_receive(net_device_t *dev, sk_buff_t *skb);
        tcp_receive(dev, skb);
        break;
    default:
        break;
    }
}

/* =========================================================
 * Ethernet receive dispatcher
 * ========================================================= */

void eth_receive(net_device_t *dev, sk_buff_t *skb) {
    if (skb->len < sizeof(eth_header_t)) return;
    eth_header_t *eth = (eth_header_t *)skb->data;
    u16 et = ntohs(eth->ethertype);
    skb_pull(skb, sizeof(eth_header_t));

    /* Cache sender MAC */
    arp_cache_insert(0, eth->src_mac);  /* IP unknown at this point */

    switch (et) {
    case ETH_P_ARP: arp_receive(dev, skb); break;
    case ETH_P_IP:  ip_receive(dev, skb);  break;
    default: break;
    }
}

/* =========================================================
 * ICMP
 * ========================================================= */

void icmp_receive(net_device_t *dev, sk_buff_t *skb) {
    if (skb->len < sizeof(icmp_header_t)) return;
    icmp_header_t *icmp = (icmp_header_t *)skb->data;

    if (icmp->type == ICMP_ECHO_REQUEST) {
        /* Send echo reply */
        sk_buff_t *reply = skb_alloc(skb->len + sizeof(ip_header_t) + sizeof(eth_header_t) + 64);
        if (!reply) return;

        /* Copy payload after ICMP header */
        u32 data_len = skb->len;
        icmp_header_t *rep = (icmp_header_t *)skb_put(reply, sizeof(icmp_header_t));
        rep->type     = ICMP_ECHO_REPLY;
        rep->code     = 0;
        rep->checksum = 0;
        rep->rest     = icmp->rest;  /* carry id/seq */

        u8 *payload = skb_put(reply, data_len - sizeof(icmp_header_t));
        if (payload && data_len > sizeof(icmp_header_t))
            memcpy(payload, skb->data + sizeof(icmp_header_t),
                   data_len - sizeof(icmp_header_t));

        rep->checksum = ip_checksum(rep, (u32)(reply->tail - (u8*)rep));

        ip_send(dev, htonl(skb->src_ip), IPPROTO_ICMP, reply);
        skb_free(reply);
    }
}

int icmp_send_echo(net_device_t *dev, u32 dst_ip, u16 id, u16 seq) {
    sk_buff_t *skb = skb_alloc(sizeof(icmp_header_t) + 64 +
                                sizeof(ip_header_t) + sizeof(eth_header_t));
    if (!skb) return -ENOMEM;

    icmp_header_t *hdr = (icmp_header_t *)skb_put(skb, sizeof(icmp_header_t));
    hdr->type     = ICMP_ECHO_REQUEST;
    hdr->code     = 0;
    hdr->checksum = 0;
    hdr->rest     = (u32)((id << 16) | seq);

    /* 56-byte payload */
    u8 *data = skb_put(skb, 56);
    if (data) for (int i = 0; i < 56; i++) data[i] = (u8)i;

    hdr->checksum = ip_checksum(hdr, (u32)(skb->tail - (u8*)hdr));
    return ip_send(dev, dst_ip, IPPROTO_ICMP, skb);
}

/* =========================================================
 * UDP send/receive
 * ========================================================= */

void udp_receive(net_device_t *dev, sk_buff_t *skb) {
    (void)dev;
    if (skb->len < sizeof(udp_header_t)) return;
    udp_header_t *udp = (udp_header_t *)skb->data;

    skb->src_port  = ntohs(udp->src_port);
    skb->dst_port  = ntohs(udp->dst_port);
    skb_pull(skb, sizeof(udp_header_t));

    /* Deliver to the matching UDP socket */
    sock_t *sk = inet_lookup(IPPROTO_UDP, skb->src_ip, skb->src_port,
                              skb->dst_ip, skb->dst_port);
    if (!sk) {
        /* Send ICMP port unreachable */
        return;
    }

    sock_ringbuf_write(&sk->sk_rcvbuf, skb->data, skb->len);
}

int udp_send(sock_t *sk, const void *data, u32 len, u32 dst_ip, u16 dst_port) {
    net_device_t *dev = netdev_get_default();
    if (!dev) return -ENETDOWN;

    sk_buff_t *skb = skb_alloc(len + sizeof(udp_header_t) +
                                sizeof(ip_header_t) + sizeof(eth_header_t) + 32);
    if (!skb) return -ENOMEM;

    udp_header_t *udp = (udp_header_t *)skb_put(skb, sizeof(udp_header_t));
    udp->src_port  = htons(sk->sk_local_port);
    udp->dst_port  = htons(dst_port);
    udp->length    = htons((u16)(sizeof(udp_header_t) + len));
    udp->checksum  = 0;

    u8 *payload = skb_put(skb, len);
    if (!payload) { skb_free(skb); return -ENOMEM; }
    memcpy(payload, data, len);

    /* Compute UDP checksum */
    udp->checksum = tcp_udp_checksum(dev->ip4, htonl(dst_ip),
                                      IPPROTO_UDP,
                                      udp, (u16)(sizeof(udp_header_t) + len));

    int r = ip_send(dev, htonl(dst_ip), IPPROTO_UDP, skb);
    skb_free(skb);
    return r < 0 ? r : (int)len;
}

/* =========================================================
 * Socket ring buffer
 * ========================================================= */

int sock_ringbuf_init(sock_ringbuf_t *rb, u32 size) {
    rb->buf = kmalloc(size, GFP_KERNEL);
    if (!rb->buf) return -ENOMEM;
    rb->size = size;
    rb->head = rb->tail = rb->len = 0;
    spin_lock_init(&rb->lock);
    return 0;
}

void sock_ringbuf_free(sock_ringbuf_t *rb) {
    if (rb->buf) { kfree(rb->buf); rb->buf = NULL; }
}

s64 sock_ringbuf_write(sock_ringbuf_t *rb, const void *data, u32 len) {
    spin_lock(&rb->lock);
    u32 space = rb->size - rb->len;
    if (len > space) len = space;
    for (u32 i = 0; i < len; i++) {
        rb->buf[rb->tail] = ((const u8 *)data)[i];
        rb->tail = (rb->tail + 1) % rb->size;
    }
    rb->len += len;
    spin_unlock(&rb->lock);
    return (s64)len;
}

s64 sock_ringbuf_read(sock_ringbuf_t *rb, void *data, u32 len, bool peek) {
    spin_lock(&rb->lock);
    if (rb->len == 0) { spin_unlock(&rb->lock); return 0; }
    if (len > rb->len) len = rb->len;
    u32 h = rb->head;
    for (u32 i = 0; i < len; i++) {
        ((u8 *)data)[i] = rb->buf[(h + i) % rb->size];
    }
    if (!peek) {
        rb->head = (rb->head + len) % rb->size;
        rb->len -= len;
    }
    spin_unlock(&rb->lock);
    return (s64)len;
}

/* =========================================================
 * sk / socket allocators
 * ========================================================= */

sock_t *sk_alloc(sa_family_t family, int type, int protocol) {
    sock_t *sk = kzalloc(sizeof(sock_t), GFP_KERNEL);
    if (!sk) return NULL;
    sk->sk_family   = family;
    sk->sk_type     = type;
    sk->sk_protocol = protocol;
    sk->sk_state    = TCP_CLOSED;
    sk->sk_ttl      = 64;
    sk->sk_refcount = 1;
    sk->sk_backlog_max = SOCK_BACKLOG_MAX;
    spin_lock_init(&sk->sk_lock);
    spin_lock_init(&sk->sk_accept_lock);
    sock_ringbuf_init(&sk->sk_rcvbuf, SOCK_RCVBUF_SIZE);
    sock_ringbuf_init(&sk->sk_sndbuf, SOCK_SNDBUF_SIZE);
    return sk;
}

void sk_free(sock_t *sk) {
    if (!sk) return;
    sock_ringbuf_free(&sk->sk_rcvbuf);
    sock_ringbuf_free(&sk->sk_sndbuf);
    kfree(sk);
}

socket_t *sock_alloc(void) {
    socket_t *sock = kzalloc(sizeof(socket_t), GFP_KERNEL);
    if (!sock) return NULL;
    sock->state = SS_UNCONNECTED;
    return sock;
}

/* =========================================================
 * Port allocation
 * ========================================================= */

#define EPHEMERAL_PORT_LOW  32768
#define EPHEMERAL_PORT_HIGH 60999

static u16 next_eph_port = EPHEMERAL_PORT_LOW;
static spinlock_t port_lock = {.raw_lock={0}};
static u64 udp_port_bitmap[1024];   /* 65536 bits */
static u64 tcp_port_bitmap[1024];

u16 inet_alloc_port(void) {
    spin_lock(&port_lock);
    for (int i = 0; i < (EPHEMERAL_PORT_HIGH - EPHEMERAL_PORT_LOW); i++) {
        u16 port = next_eph_port++;
        if (next_eph_port > EPHEMERAL_PORT_HIGH) next_eph_port = EPHEMERAL_PORT_LOW;
        int idx = port / 64, bit = port % 64;
        if (!(tcp_port_bitmap[idx] & (1ULL << bit))) {
            tcp_port_bitmap[idx] |= (1ULL << bit);
            udp_port_bitmap[idx] |= (1ULL << bit);
            spin_unlock(&port_lock);
            return port;
        }
    }
    spin_unlock(&port_lock);
    return 0;
}

void inet_free_port(u16 port) {
    if (!port) return;
    spin_lock(&port_lock);
    tcp_port_bitmap[port/64] &= ~(1ULL << (port%64));
    udp_port_bitmap[port/64] &= ~(1ULL << (port%64));
    spin_unlock(&port_lock);
}

/* =========================================================
 * Socket lookup table
 * ========================================================= */

#define SOCK_TABLE_SIZE 256
static sock_t    *sock_table[SOCK_TABLE_SIZE];
static spinlock_t sock_table_lock = {.raw_lock={0}};

static void sock_table_insert(sock_t *sk) {
    spin_lock(&sock_table_lock);
    for (int i = 0; i < SOCK_TABLE_SIZE; i++) {
        if (!sock_table[i]) { sock_table[i] = sk; break; }
    }
    spin_unlock(&sock_table_lock);
}

static void sock_table_remove(sock_t *sk) {
    spin_lock(&sock_table_lock);
    for (int i = 0; i < SOCK_TABLE_SIZE; i++) {
        if (sock_table[i] == sk) { sock_table[i] = NULL; break; }
    }
    spin_unlock(&sock_table_lock);
}

sock_t *inet_lookup(u8 proto, u32 src_ip, u16 src_port,
                     u32 dst_ip, u16 dst_port) {
    (void)src_ip; (void)src_port;
    spin_lock(&sock_table_lock);
    sock_t *best = NULL;
    for (int i = 0; i < SOCK_TABLE_SIZE; i++) {
        sock_t *sk = sock_table[i];
        if (!sk) continue;
        if ((u8)sk->sk_protocol != proto) continue;
        if (sk->sk_local_port != dst_port) continue;
        /* Check IP: 0 = INADDR_ANY */
        if (sk->sk_local_ip != 0 && sk->sk_local_ip != dst_ip) continue;
        best = sk;
        break;
    }
    spin_unlock(&sock_table_lock);
    return best;
}

/* =========================================================
 * Loopback interface
 * ========================================================= */

static int lo_transmit(net_device_t *dev, sk_buff_t *skb) {
    /* Loop packet back into the receive path */
    sk_buff_t *lb = skb_alloc(skb->len + 64);
    if (!lb) return -ENOMEM;
    u8 *d = skb_put(lb, skb->len);
    if (d) memcpy(d, skb->data, skb->len);
    eth_receive(dev, lb);
    skb_free(lb);
    return 0;
}

void loopback_init(void) {
    static net_device_t lo_dev;
    memset(&lo_dev, 0, sizeof(lo_dev));
    memcpy(lo_dev.name, "lo", 3);
    lo_dev.ip4      = htonl(INADDR_LOOPBACK);   /* 127.0.0.1 */
    lo_dev.netmask  = htonl(0xFF000000U);        /* /8 */
    lo_dev.gateway  = htonl(INADDR_LOOPBACK);
    lo_dev.mtu      = 65536;
    lo_dev.up       = true;
    lo_dev.transmit = lo_transmit;
    memset(lo_dev.mac, 0, 6);
    netdev_register(&lo_dev);
}

/* =========================================================
 * Network subsystem init
 * ========================================================= */

void net_init(void) {
    memset(arp_cache, 0, sizeof(arp_cache));
    memset(sock_table, 0, sizeof(sock_table));
    memset(tcp_port_bitmap, 0, sizeof(tcp_port_bitmap));
    memset(udp_port_bitmap, 0, sizeof(udp_port_bitmap));
    spin_lock_init(&arp_lock);
    spin_lock_init(&sock_table_lock);
    spin_lock_init(&port_lock);
    spin_lock_init(&netdev_lock);

    loopback_init();
    printk(KERN_INFO "[net] network subsystem initialized\n");
}

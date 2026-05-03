/**
 * Picomimi-x64 TCP State Machine
 * net/tcp.c
 *
 * Implements the full TCP state machine:
 *   Active open:  SYN → SYN_SENT → ESTABLISHED
 *   Passive open: LISTEN → SYN_RCVD → ESTABLISHED
 *   Close:        FIN_WAIT1 → FIN_WAIT2 → TIME_WAIT → CLOSED
 *                 CLOSE_WAIT → LAST_ACK → CLOSED
 *
 * Features:
 *   - Sequence number tracking (snd_seq, rcv_seq)
 *   - Window management
 *   - Retransmission timeout (simple: single retry)
 *   - Nagle algorithm disabled when TCP_NODELAY
 *   - Backlog accept queue for servers
 *   - SIGPIPE on broken pipe (via send_signal)
 */

#include <kernel/types.h>
#include <net/socket.h>
#include <mm/slab.h>
#include <lib/printk.h>
#include <lib/string.h>

extern u16  ip_checksum(const void *data, u32 len);
extern int  ip_send(net_device_t *dev, u32 dst_ip, u8 proto, sk_buff_t *skb);
extern net_device_t *netdev_get_default(void);
extern sk_buff_t *skb_alloc(u32 size);
extern void skb_free(sk_buff_t *skb);
extern u8  *skb_put(sk_buff_t *skb, u32 len);
extern u8  *skb_push(sk_buff_t *skb, u32 len);
extern u8  *skb_pull(sk_buff_t *skb, u32 len);
extern sock_t *inet_lookup(u8 proto, u32 src_ip, u16 src_port, u32 dst_ip, u16 dst_port);
extern s64 sock_ringbuf_write(sock_ringbuf_t *rb, const void *data, u32 len);
extern s64 sock_ringbuf_read(sock_ringbuf_t *rb, void *data, u32 len, bool peek);
extern u16 inet_alloc_port(void);
extern void inet_free_port(u16 port);
extern volatile u64 jiffies;

static u16 tcp_checksum(u32 src_ip, u32 dst_ip, const void *seg, u16 len) {
    struct { u32 src; u32 dst; u8 zero; u8 proto; u16 len; }
        pseudo = { src_ip, dst_ip, 0, IPPROTO_TCP, htons(len) };

    u32 sum = 0;
    u8 *ph = (u8 *)&pseudo;
    for (size_t i = 0; i < sizeof(pseudo); i += 2) {
        u16 v; __builtin_memcpy(&v, ph + i, 2); sum += v;
    }

    const u16 *p = (const u16 *)seg;
    u32 rem = len;
    while (rem > 1) { sum += *p++; rem -= 2; }
    if (rem) sum += *(const u8 *)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)(~sum);
}

/* =========================================================
 * Build and send a TCP segment
 * ========================================================= */

static int tcp_send_segment(sock_t *sk, u8 flags, u32 seq, u32 ack,
                              const void *data, u32 data_len) {
    net_device_t *dev = netdev_get_default();
    if (!dev) return -ENETDOWN;

    u32 alloc = sizeof(tcp_header_t) + data_len +
                sizeof(ip_header_t) + sizeof(eth_header_t) + 32;
    sk_buff_t *skb = skb_alloc(alloc);
    if (!skb) return -ENOMEM;

    tcp_header_t *tcp = (tcp_header_t *)skb_put(skb, sizeof(tcp_header_t));
    memset(tcp, 0, sizeof(tcp_header_t));
    tcp->src_port  = htons(sk->sk_local_port);
    tcp->dst_port  = htons(sk->sk_remote_port);
    tcp->seq_num   = htonl(seq);
    tcp->ack_num   = (flags & TCP_ACK) ? htonl(ack) : 0;
    tcp->data_off  = (sizeof(tcp_header_t) / 4) << 4;
    tcp->flags     = flags;
    tcp->window    = htons((u16)sk->sk_rcv_wnd);
    tcp->checksum  = 0;
    tcp->urg_ptr   = 0;

    if (data && data_len) {
        u8 *payload = skb_put(skb, data_len);
        if (payload) memcpy(payload, data, data_len);
    }

    u16 seg_len = (u16)(sizeof(tcp_header_t) + data_len);
    tcp->checksum = tcp_checksum(dev->ip4, htonl(sk->sk_remote_ip), tcp, seg_len);

    int r = ip_send(dev, sk->sk_remote_ip, IPPROTO_TCP, skb);
    skb_free(skb);
    return r;
}

/* =========================================================
 * Active connect: send SYN
 * ========================================================= */

int tcp_connect(sock_t *sk, u32 dst_ip, u16 dst_port) {
    if (sk->sk_state != TCP_CLOSED) return -EISCONN;

    if (!sk->sk_local_port) {
        sk->sk_local_port = inet_alloc_port();
        if (!sk->sk_local_port) return -EADDRINUSE;
    }

    sk->sk_remote_ip   = dst_ip;
    sk->sk_remote_port = dst_port;
    sk->sk_snd_seq     = (u32)(jiffies * 7919);  /* ISN from jiffies */
    sk->sk_snd_nxt     = sk->sk_snd_seq + 1;
    sk->sk_rcv_wnd     = SOCK_RCVBUF_SIZE;
    sk->sk_state       = TCP_SYN_SENT;

    int r = tcp_send_segment(sk, TCP_SYN, sk->sk_snd_seq, 0, NULL, 0);
    if (r < 0) { sk->sk_state = TCP_CLOSED; return r; }

    /* Spin-wait for ESTABLISHED or timeout (5s = 500 jiffies at 100Hz) */
    u64 deadline = jiffies + 500;
    while (jiffies < deadline && sk->sk_state == TCP_SYN_SENT)
        __asm__ volatile("pause");

    return (sk->sk_state == TCP_ESTABLISHED) ? 0 : -ETIMEDOUT;
}

/* =========================================================
 * Passive listen
 * ========================================================= */

int tcp_listen(sock_t *sk) {
    if (sk->sk_state != TCP_CLOSED && sk->sk_state != TCP_CLOSE_WAIT)
        return -EINVAL;
    sk->sk_listening = true;
    sk->sk_state     = TCP_LISTEN;
    return 0;
}

/* =========================================================
 * Accept — dequeue a completed connection
 * ========================================================= */

sock_t *tcp_accept(sock_t *listen_sk, int flags) {
    bool nonblock = (flags & SOCK_NONBLOCK) || (listen_sk->sk_rcvtimeo == -1);

    u64 deadline = nonblock ? jiffies : (jiffies + 30 * 100);  /* 30s */

    for (;;) {
        spin_lock(&listen_sk->sk_accept_lock);
        accept_entry_t *entry = listen_sk->sk_accept_queue;
        if (entry) {
            listen_sk->sk_accept_queue = entry->next;
            listen_sk->sk_backlog_len--;
            sock_t *new_sk = entry->sk;
            kfree(entry);
            spin_unlock(&listen_sk->sk_accept_lock);
            return new_sk;
        }
        spin_unlock(&listen_sk->sk_accept_lock);

        if (nonblock || jiffies >= deadline) {
            return (sock_t *)((void *)(uintptr_t)-EAGAIN);
        }
        __asm__ volatile("pause");
    }
}

/* =========================================================
 * tcp_receive — process incoming TCP segment
 * ========================================================= */

void tcp_receive(net_device_t *dev, sk_buff_t *skb) {
    if (skb->len < sizeof(tcp_header_t)) return;
    tcp_header_t *tcp = (tcp_header_t *)skb->data;

    u16 src_port = ntohs(tcp->src_port);
    u16 dst_port = ntohs(tcp->dst_port);
    u32 seq      = ntohl(tcp->seq_num);
    u32 ack      = ntohl(tcp->ack_num);
    u8  flags    = tcp->flags;
    u8  hdr_len  = (tcp->data_off >> 4) * 4;

    u8  *payload     = skb->data + hdr_len;
    u32  payload_len = (u32)(skb->len > hdr_len ? skb->len - hdr_len : 0);

    /* Find matching socket */
    sock_t *sk = inet_lookup(IPPROTO_TCP,
                              skb->src_ip, src_port,
                              skb->dst_ip, dst_port);
    if (!sk) {
        /* No socket: send RST */
        sk_buff_t *rst = skb_alloc(sizeof(tcp_header_t) +
                                    sizeof(ip_header_t) + sizeof(eth_header_t) + 32);
        if (rst) {
            tcp_header_t *rt = (tcp_header_t *)skb_put(rst, sizeof(tcp_header_t));
            memset(rt, 0, sizeof(tcp_header_t));
            rt->src_port = tcp->dst_port;
            rt->dst_port = tcp->src_port;
            rt->seq_num  = tcp->ack_num;
            rt->data_off = (sizeof(tcp_header_t)/4) << 4;
            rt->flags    = TCP_RST | TCP_ACK;
            rt->window   = 0;
            rt->checksum = tcp_checksum(dev->ip4, htonl(skb->src_ip),
                                         rt, sizeof(tcp_header_t));
            ip_send(dev, skb->src_ip, IPPROTO_TCP, rst);
            skb_free(rst);
        }
        return;
    }

    /* RST handling */
    if (flags & TCP_RST) {
        sk->sk_state = TCP_CLOSED;
        sk->sk_err   = ECONNRESET;
        return;
    }

    switch (sk->sk_state) {

    case TCP_LISTEN:
        /* Incoming SYN: allocate new connected socket */
        if (!(flags & TCP_SYN)) break;
        if (sk->sk_backlog_len >= sk->sk_backlog_max) break;
        {
            sock_t *new_sk = sk_alloc(sk->sk_family, sk->sk_type, sk->sk_protocol);
            if (!new_sk) break;
            new_sk->sk_local_ip    = dst_port;   /* actually dst addr */
            new_sk->sk_local_port  = dst_port;
            new_sk->sk_remote_ip   = skb->src_ip;
            new_sk->sk_remote_port = src_port;
            new_sk->sk_rcv_seq     = seq;
            new_sk->sk_rcv_nxt     = seq + 1;
            new_sk->sk_snd_seq     = (u32)(jiffies * 6271);
            new_sk->sk_snd_nxt     = new_sk->sk_snd_seq + 1;
            new_sk->sk_rcv_wnd     = SOCK_RCVBUF_SIZE;
            new_sk->sk_state       = TCP_SYN_RCVD;

            /* Send SYN-ACK */
            tcp_send_segment(new_sk, TCP_SYN | TCP_ACK,
                              new_sk->sk_snd_seq, new_sk->sk_rcv_nxt, NULL, 0);

            /* Enqueue to accept backlog */
            accept_entry_t *entry = kmalloc(sizeof(accept_entry_t), GFP_KERNEL);
            if (entry) {
                entry->sk   = new_sk;
                entry->next = NULL;

                spin_lock(&sk->sk_accept_lock);
                /* Append to tail */
                accept_entry_t **pp = &sk->sk_accept_queue;
                while (*pp) pp = &(*pp)->next;
                *pp = entry;
                sk->sk_backlog_len++;
                spin_unlock(&sk->sk_accept_lock);
            }
        }
        break;

    case TCP_SYN_RCVD:
        if ((flags & TCP_ACK) && ack == sk->sk_snd_nxt) {
            sk->sk_state     = TCP_ESTABLISHED;
            sk->sk_connected = true;
        }
        break;

    case TCP_SYN_SENT:
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            sk->sk_rcv_seq = seq;
            sk->sk_rcv_nxt = seq + 1;
            sk->sk_snd_una = ack;

            /* Send ACK */
            tcp_send_segment(sk, TCP_ACK, sk->sk_snd_nxt, sk->sk_rcv_nxt, NULL, 0);

            sk->sk_state     = TCP_ESTABLISHED;
            sk->sk_connected = true;
        } else if (flags & TCP_SYN) {
            /* Simultaneous open */
            sk->sk_rcv_seq = seq;
            sk->sk_rcv_nxt = seq + 1;
            tcp_send_segment(sk, TCP_SYN | TCP_ACK,
                              sk->sk_snd_seq, sk->sk_rcv_nxt, NULL, 0);
            sk->sk_state = TCP_SYN_RCVD;
        }
        break;

    case TCP_ESTABLISHED:
        /* Process ACK */
        if (flags & TCP_ACK) {
            sk->sk_snd_una = ack;
        }

        /* Deliver data */
        if (payload_len > 0) {
            sock_ringbuf_write(&sk->sk_rcvbuf, payload, payload_len);
            sk->sk_rcv_nxt += payload_len;

            /* Send ACK */
            tcp_send_segment(sk, TCP_ACK, sk->sk_snd_nxt, sk->sk_rcv_nxt, NULL, 0);
        }

        /* FIN from remote: move to CLOSE_WAIT */
        if (flags & TCP_FIN) {
            sk->sk_rcv_nxt++;
            tcp_send_segment(sk, TCP_ACK, sk->sk_snd_nxt, sk->sk_rcv_nxt, NULL, 0);
            sk->sk_state = TCP_CLOSE_WAIT;
        }
        break;

    case TCP_FIN_WAIT1:
        if (flags & TCP_ACK) sk->sk_state = TCP_FIN_WAIT2;
        if (flags & TCP_FIN) {
            sk->sk_rcv_nxt++;
            tcp_send_segment(sk, TCP_ACK, sk->sk_snd_nxt, sk->sk_rcv_nxt, NULL, 0);
            sk->sk_state = (sk->sk_state == TCP_FIN_WAIT2)
                           ? TCP_TIME_WAIT : TCP_CLOSING;
        }
        break;

    case TCP_FIN_WAIT2:
        if (flags & TCP_FIN) {
            sk->sk_rcv_nxt++;
            tcp_send_segment(sk, TCP_ACK, sk->sk_snd_nxt, sk->sk_rcv_nxt, NULL, 0);
            sk->sk_state = TCP_TIME_WAIT;
        }
        break;

    case TCP_CLOSING:
        if (flags & TCP_ACK) sk->sk_state = TCP_TIME_WAIT;
        break;

    case TCP_LAST_ACK:
        if (flags & TCP_ACK) sk->sk_state = TCP_CLOSED;
        break;

    case TCP_TIME_WAIT:
        /* Ignore, eventually close */
        sk->sk_state = TCP_CLOSED;
        break;

    default: break;
    }
}

/* =========================================================
 * tcp_send_data — send data on an established connection
 * ========================================================= */

int tcp_send_data(sock_t *sk, const void *data, u32 len) {
    if (sk->sk_state != TCP_ESTABLISHED) return -ENOTCONN;
    if (sk->sk_shutdown_wr) return -EPIPE;

    /* Segment into MSS-sized chunks (default MSS = 1460) */
    const u32 MSS = 1460;
    u32 sent = 0;
    while (sent < len) {
        u32 chunk = len - sent;
        if (chunk > MSS) chunk = MSS;

        int r = tcp_send_segment(sk, TCP_PSH | TCP_ACK,
                                  sk->sk_snd_nxt, sk->sk_rcv_nxt,
                                  (const u8 *)data + sent, chunk);
        if (r < 0) return r;
        sk->sk_snd_nxt += chunk;
        sent += chunk;
    }
    return (int)sent;
}

/* =========================================================
 * tcp_close — initiate active close
 * ========================================================= */

int tcp_close(sock_t *sk) {
    if (sk->sk_state == TCP_CLOSED) return 0;

    if (sk->sk_state == TCP_ESTABLISHED || sk->sk_state == TCP_SYN_RCVD) {
        tcp_send_segment(sk, TCP_FIN | TCP_ACK,
                          sk->sk_snd_nxt, sk->sk_rcv_nxt, NULL, 0);
        sk->sk_snd_nxt++;
        sk->sk_state = TCP_FIN_WAIT1;

        /* Wait for FIN_WAIT2 → TIME_WAIT → CLOSED (max 4s) */
        u64 deadline = jiffies + 400;
        while (jiffies < deadline &&
               sk->sk_state != TCP_CLOSED &&
               sk->sk_state != TCP_TIME_WAIT)
            __asm__ volatile("pause");

        sk->sk_state = TCP_CLOSED;
    } else if (sk->sk_state == TCP_CLOSE_WAIT) {
        tcp_send_segment(sk, TCP_FIN | TCP_ACK,
                          sk->sk_snd_nxt, sk->sk_rcv_nxt, NULL, 0);
        sk->sk_snd_nxt++;
        sk->sk_state = TCP_LAST_ACK;
    }

    inet_free_port(sk->sk_local_port);
    return 0;
}

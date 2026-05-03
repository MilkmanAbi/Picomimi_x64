/**
 * Picomimi-x64 Network Socket Interface
 * include/net/socket.h
 */

#ifndef _NET_SOCKET_H
#define _NET_SOCKET_H

#include <kernel/types.h>

/* =========================================================
 * Address families
 * ========================================================= */
#define AF_UNSPEC       0
#define AF_UNIX         1
#define AF_LOCAL        1
#define AF_INET         2
#define AF_INET6        10
#define AF_NETLINK      16
#define AF_PACKET       17
#define PF_UNIX         AF_UNIX
#define PF_INET         AF_INET
#define PF_INET6        AF_INET6

/* Socket types */
#define SOCK_STREAM     1
#define SOCK_DGRAM      2
#define SOCK_RAW        3
#define SOCK_SEQPACKET  5
#define SOCK_DCCP       6
#define SOCK_NONBLOCK   0x800
#define SOCK_CLOEXEC    0x80000

/* Protocol numbers */
#define IPPROTO_IP      0
#define IPPROTO_ICMP    1
#define IPPROTO_TCP     6
#define IPPROTO_UDP     17
#define IPPROTO_RAW     255

/* Socket levels */
#define SOL_SOCKET      1
#define SOL_TCP         6
#define SOL_UDP         17
#define SOL_IP          0

/* SOL_SOCKET options */
#define SO_DEBUG        1
#define SO_REUSEADDR    2
#define SO_TYPE         3
#define SO_ERROR        4
#define SO_DONTROUTE    5
#define SO_BROADCAST    6
#define SO_SNDBUF       7
#define SO_RCVBUF       8
#define SO_KEEPALIVE    9
#define SO_OOBINLINE    10
#define SO_LINGER       13
#define SO_RCVTIMEO     20
#define SO_SNDTIMEO     21
#define SO_ACCEPTCONN   30
#define SO_PEERCRED     17
#define SO_RCVBUFFORCE  33
#define SO_SNDBUFFORCE  32
#define SO_REUSEPORT    15

/* TCP options */
#define TCP_NODELAY     1
#define TCP_MAXSEG      2
#define TCP_CORK        3
#define TCP_KEEPIDLE    4
#define TCP_KEEPINTVL   5
#define TCP_KEEPCNT     6
#define TCP_SYNCOUNT    7
#define TCP_LINGER2     8
#define TCP_DEFER_ACCEPT 9
#define TCP_WINDOW_CLAMP 10
#define TCP_INFO        11
#define TCP_QUICKACK    12
#define TCP_CONGESTION  13

/* IP options */
#define IP_TOS          1
#define IP_TTL          2
#define IP_HDRINCL      3
#define IP_OPTIONS      4
#define IP_RECVOPTS     6
#define IP_RETOPTS      7
#define IP_MULTICAST_IF 32
#define IP_MULTICAST_TTL 33
#define IP_ADD_MEMBERSHIP 35

/* SHUT_ flags */
#define SHUT_RD         0
#define SHUT_WR         1
#define SHUT_RDWR       2

/* MSG_ flags */
#define MSG_OOB         0x01
#define MSG_PEEK        0x02
#define MSG_DONTROUTE   0x04
#define MSG_TRYHARD     MSG_DONTROUTE
#define MSG_CTRUNC      0x08
#define MSG_PROBE       0x10
#define MSG_TRUNC       0x20
#define MSG_DONTWAIT    0x40
#define MSG_EOR         0x80
#define MSG_WAITALL     0x100
#define MSG_NOSIGNAL    0x4000
#define MSG_MORE        0x8000

/* =========================================================
 * Socket address structures
 * ========================================================= */

typedef u16 sa_family_t;
typedef u32 in_addr_t;
typedef u16 in_port_t;

struct sockaddr {
    sa_family_t sa_family;
    char        sa_data[14];
};

struct in_addr {
    in_addr_t   s_addr;         /* IPv4 address in network byte order */
};

struct sockaddr_in {
    sa_family_t     sin_family; /* AF_INET */
    in_port_t       sin_port;   /* port in network byte order */
    struct in_addr  sin_addr;
    u8              sin_zero[8];
};

struct sockaddr_in6 {
    sa_family_t     sin6_family;    /* AF_INET6 */
    in_port_t       sin6_port;
    u32             sin6_flowinfo;
    u8              sin6_addr[16];
    u32             sin6_scope_id;
};

struct sockaddr_un {
    sa_family_t     sun_family;     /* AF_UNIX */
    char            sun_path[108];
};

struct sockaddr_storage {
    sa_family_t     ss_family;
    u8              _padding[126];
};

/* iovec + msghdr */
struct msghdr {
    void        *msg_name;          /* optional address */
    int          msg_namelen;
    struct iovec *msg_iov;
    size_t       msg_iovlen;
    void        *msg_control;
    size_t       msg_controllen;
    int          msg_flags;
};

struct cmsghdr {
    size_t  cmsg_len;
    int     cmsg_level;
    int     cmsg_type;
};

/* linger */
struct linger {
    int l_onoff;
    int l_linger;
};

/* =========================================================
 * sk_buff — socket buffer (network packet)
 * ========================================================= */

#define SKB_MAX_DATA    2048

typedef struct sk_buff {
    struct sk_buff  *next;
    struct sk_buff  *prev;

    /* Data pointers */
    u8              *head;       /* start of allocated buffer */
    u8              *data;       /* start of payload */
    u8              *tail;       /* end of payload */
    u8              *end;        /* end of allocated buffer */

    u32             len;         /* total length of data */
    u32             truesize;    /* total allocation size */

    /* Protocol layer offsets */
    u16             network_header;
    u16             transport_header;
    u16             mac_header;

    /* Source/dest info */
    u32             src_ip;
    u32             dst_ip;
    u16             src_port;
    u16             dst_port;
    u8              protocol;

    /* Inline buffer */
    u8              _buf[SKB_MAX_DATA];
} sk_buff_t;

/* =========================================================
 * TCP connection state
 * ========================================================= */

typedef enum tcp_state {
    TCP_CLOSED      = 0,
    TCP_LISTEN      = 1,
    TCP_SYN_SENT    = 2,
    TCP_SYN_RCVD    = 3,
    TCP_ESTABLISHED = 4,
    TCP_FIN_WAIT1   = 5,
    TCP_FIN_WAIT2   = 6,
    TCP_CLOSE_WAIT  = 7,
    TCP_CLOSING     = 8,
    TCP_LAST_ACK    = 9,
    TCP_TIME_WAIT   = 10,
} tcp_state_t;

/* =========================================================
 * struct sock — protocol-independent socket core
 * ========================================================= */

#define SOCK_RCVBUF_SIZE    (64 * 1024)
#define SOCK_SNDBUF_SIZE    (64 * 1024)
#define SOCK_BACKLOG_MAX    128

/* Ring buffer for socket data */
typedef struct sock_ringbuf {
    u8      *buf;
    u32      size;
    u32      head;
    u32      tail;
    u32      len;
    spinlock_t lock;
} sock_ringbuf_t;

/* Accepted connection queue entry */
typedef struct accept_entry {
    struct accept_entry *next;
    struct sock         *sk;
} accept_entry_t;

typedef struct sock {
    sa_family_t     sk_family;
    int             sk_type;
    int             sk_protocol;

    /* State */
    tcp_state_t     sk_state;
    int             sk_err;
    bool            sk_connected;
    bool            sk_listening;
    bool            sk_shutdown_rd;
    bool            sk_shutdown_wr;

    /* Addressing */
    u32             sk_local_ip;
    u16             sk_local_port;
    u32             sk_remote_ip;
    u16             sk_remote_port;

    /* Buffers */
    sock_ringbuf_t  sk_rcvbuf;
    sock_ringbuf_t  sk_sndbuf;

    /* Options */
    bool            sk_reuseaddr;
    bool            sk_reuseport;
    bool            sk_broadcast;
    bool            sk_keepalive;
    bool            sk_nodelay;     /* TCP_NODELAY */
    int             sk_rcvtimeo;    /* ms, 0 = infinite */
    int             sk_sndtimeo;
    int             sk_ttl;
    int             sk_tos;

    /* Backlog (listen) */
    int             sk_backlog_max;
    int             sk_backlog_len;
    accept_entry_t  *sk_accept_queue;
    spinlock_t       sk_accept_lock;

    /* TCP sequence tracking */
    u32             sk_snd_seq;     /* send sequence */
    u32             sk_snd_una;     /* send unacknowledged */
    u32             sk_snd_nxt;     /* send next */
    u32             sk_rcv_seq;     /* receive sequence */
    u32             sk_rcv_nxt;     /* receive next */
    u32             sk_snd_wnd;     /* send window */
    u32             sk_rcv_wnd;     /* receive window */

    /* AF_UNIX peer */
    struct sock     *sk_peer;
    char            sk_unix_path[108];

    /* Network interface binding */
    struct net_device *sk_dev;

    /* Protocol ops */
    const struct proto_ops *sk_ops;

    /* Refcount */
    int             sk_refcount;

    spinlock_t      sk_lock;
} sock_t;

/* =========================================================
 * struct socket — the file-visible socket (sock wrapper)
 * ========================================================= */

typedef enum {
    SS_FREE         = 0,
    SS_UNCONNECTED  = 1,
    SS_CONNECTING   = 2,
    SS_CONNECTED    = 3,
    SS_DISCONNECTING = 4,
} socket_state_t;

typedef struct socket {
    socket_state_t  state;
    short           type;
    int             flags;

    sock_t          *sk;

    /* Back-reference to file */
    struct file     *file;

    /* Wait queue for blocking ops */
    wait_queue_head_t wait;

    const struct socket_ops *ops;
} socket_t;

/* =========================================================
 * Protocol ops vtable
 * ========================================================= */

struct proto_ops {
    int     family;
    int     (*bind)     (sock_t *sk, struct sockaddr *addr, int addrlen);
    int     (*connect)  (sock_t *sk, struct sockaddr *addr, int addrlen);
    int     (*listen)   (sock_t *sk, int backlog);
    sock_t *(*accept)   (sock_t *sk, int flags);
    s64     (*sendmsg)  (sock_t *sk, struct msghdr *msg, size_t len);
    s64     (*recvmsg)  (sock_t *sk, struct msghdr *msg, size_t len, int flags);
    int     (*shutdown) (sock_t *sk, int how);
    int     (*setsockopt)(sock_t *sk, int level, int optname,
                          const void *optval, u32 optlen);
    int     (*getsockopt)(sock_t *sk, int level, int optname,
                          void *optval, u32 *optlen);
    void    (*close)    (sock_t *sk);
    int     (*getname)  (sock_t *sk, struct sockaddr *addr, int peer);
    int     (*poll)     (sock_t *sk);
    int     (*ioctl)    (sock_t *sk, int cmd, u64 arg);
};

/* =========================================================
 * Network device interface
 * ========================================================= */

#define IFNAMSIZ    16

typedef struct net_device {
    char            name[IFNAMSIZ];
    u8              mac[6];
    u32             ip4;            /* IPv4 in network order */
    u32             netmask;
    u32             gateway;
    u32             mtu;
    bool            up;

    /* Transmit a packet */
    int (*transmit)(struct net_device *dev, sk_buff_t *skb);

    void            *priv;          /* driver private data */
    struct net_device *next;
} net_device_t;

/* =========================================================
 * IP / TCP / UDP header structures
 * ========================================================= */

typedef struct __packed ip_header {
    u8      version_ihl;    /* version (4) << 4 | ihl */
    u8      tos;
    u16     tot_len;
    u16     id;
    u16     frag_off;
    u8      ttl;
    u8      protocol;
    u16     checksum;
    u32     src_ip;
    u32     dst_ip;
} ip_header_t;

typedef struct __packed tcp_header {
    u16     src_port;
    u16     dst_port;
    u32     seq_num;
    u32     ack_num;
    u8      data_off;  /* (header length / 4) << 4 */
    u8      flags;
    u16     window;
    u16     checksum;
    u16     urg_ptr;
} tcp_header_t;

/* TCP flag bits */
#define TCP_FIN     0x01
#define TCP_SYN     0x02
#define TCP_RST     0x04
#define TCP_PSH     0x08
#define TCP_ACK     0x10
#define TCP_URG     0x20
#define TCP_ECE     0x40
#define TCP_CWR     0x80

typedef struct __packed udp_header {
    u16     src_port;
    u16     dst_port;
    u16     length;
    u16     checksum;
} udp_header_t;

typedef struct __packed icmp_header {
    u8      type;
    u8      code;
    u16     checksum;
    u32     rest;
} icmp_header_t;

/* ICMP types */
#define ICMP_ECHO_REPLY     0
#define ICMP_ECHO_REQUEST   8

/* Ethernet header */
typedef struct __packed eth_header {
    u8  dst_mac[6];
    u8  src_mac[6];
    u16 ethertype;
} eth_header_t;

#define ETH_P_IP    0x0800
#define ETH_P_ARP   0x0806
#define ETH_P_IPV6  0x86DD

/* ARP */
typedef struct __packed arp_packet {
    u16 htype;
    u16 ptype;
    u8  hlen;
    u8  plen;
    u16 operation;
    u8  sender_mac[6];
    u32 sender_ip;
    u8  target_mac[6];
    u32 target_ip;
} arp_packet_t;

/* =========================================================
 * Exported functions
 * ========================================================= */

/* Socket creation */
socket_t *sock_alloc(void);
sock_t   *sk_alloc(sa_family_t family, int type, int protocol);
void      sk_free(sock_t *sk);

/* Ringbuf */
int  sock_ringbuf_init(sock_ringbuf_t *rb, u32 size);
void sock_ringbuf_free(sock_ringbuf_t *rb);
s64  sock_ringbuf_write(sock_ringbuf_t *rb, const void *data, u32 len);
s64  sock_ringbuf_read(sock_ringbuf_t *rb, void *data, u32 len, bool peek);

/* sk_buff */
sk_buff_t *skb_alloc(u32 size);
void       skb_free(sk_buff_t *skb);
u8        *skb_push(sk_buff_t *skb, u32 len);
u8        *skb_put(sk_buff_t *skb, u32 len);
u8        *skb_pull(sk_buff_t *skb, u32 len);

/* Network device */
void        netdev_register(net_device_t *dev);
net_device_t *netdev_get_by_name(const char *name);
net_device_t *netdev_get_default(void);

/* IP */
u16  ip_checksum(const void *data, u32 len);
int  ip_send(net_device_t *dev, u32 dst_ip, u8 proto, sk_buff_t *skb);
void ip_receive(net_device_t *dev, sk_buff_t *skb);

/* TCP */
int  tcp_connect(sock_t *sk, u32 dst_ip, u16 dst_port);
int  tcp_listen(sock_t *sk);
void tcp_receive(net_device_t *dev, sk_buff_t *skb);
int  tcp_send_data(sock_t *sk, const void *data, u32 len);
int  tcp_close(sock_t *sk);

/* UDP */
int  udp_send(sock_t *sk, const void *data, u32 len, u32 dst_ip, u16 dst_port);
void udp_receive(net_device_t *dev, sk_buff_t *skb);

/* ICMP */
void icmp_receive(net_device_t *dev, sk_buff_t *skb);
int  icmp_send_echo(net_device_t *dev, u32 dst_ip, u16 id, u16 seq);

/* ARP */
int  arp_send_request(net_device_t *dev, u32 target_ip);
void arp_receive(net_device_t *dev, sk_buff_t *skb);
int  arp_resolve(net_device_t *dev, u32 ip, u8 *mac_out);

/* Loopback */
void loopback_init(void);

/* Port allocation */
u16  inet_alloc_port(void);
void inet_free_port(u16 port);
sock_t *inet_lookup(u8 proto, u32 src_ip, u16 src_port, u32 dst_ip, u16 dst_port);

/* Syscall layer */
s64 sys_socket(int domain, int type, int protocol);
s64 sys_bind(int sockfd, const struct sockaddr *addr, u32 addrlen);
s64 sys_connect(int sockfd, const struct sockaddr *addr, u32 addrlen);
s64 sys_listen(int sockfd, int backlog);
s64 sys_accept(int sockfd, struct sockaddr *addr, u32 *addrlen);
s64 sys_accept4(int sockfd, struct sockaddr *addr, u32 *addrlen, int flags);
s64 sys_send(int sockfd, const void *buf, size_t len, int flags);
s64 sys_recv(int sockfd, void *buf, size_t len, int flags);
s64 sys_sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest, u32 addrlen);
s64 sys_recvfrom(int sockfd, void *buf, size_t len, int flags,
                  struct sockaddr *src_addr, u32 *addrlen);
s64 sys_sendmsg(int sockfd, const struct msghdr *msg, int flags);
s64 sys_recvmsg(int sockfd, struct msghdr *msg, int flags);
s64 sys_shutdown(int sockfd, int how);
s64 sys_setsockopt(int sockfd, int level, int optname,
                    const void *optval, u32 optlen);
s64 sys_getsockopt(int sockfd, int level, int optname,
                    void *optval, u32 *optlen);
s64 sys_getsockname(int sockfd, struct sockaddr *addr, u32 *addrlen);
s64 sys_getpeername(int sockfd, struct sockaddr *addr, u32 *addrlen);
s64 sys_socketpair(int domain, int type, int protocol, int sv[2]);

/* Byte order */
static inline u16 htons(u16 v) {
    return (u16)((v >> 8) | (v << 8));
}
static inline u16 ntohs(u16 v) { return htons(v); }
static inline u32 htonl(u32 v) {
    return ((v >> 24) & 0xFF)
         | (((v >> 16) & 0xFF) << 8)
         | (((v >>  8) & 0xFF) << 16)
         | ((v & 0xFF) << 24);
}
static inline u32 ntohl(u32 v) { return htonl(v); }

/* INADDR */
#define INADDR_ANY       0x00000000U
#define INADDR_BROADCAST 0xFFFFFFFFU
#define INADDR_LOOPBACK  0x7F000001U    /* 127.0.0.1 */
#define INADDR_NONE      0xFFFFFFFFU

#endif /* _NET_SOCKET_H */

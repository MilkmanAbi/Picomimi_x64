/**
 * Picomimi-x64 Socket Syscall Layer
 * net/socket_syscalls.c
 *
 * Translates Linux socket syscalls to the internal sock_t / socket_t layer.
 * Supports AF_INET (TCP/UDP) and AF_UNIX (SOCK_STREAM pairs).
 *
 * Each socket is backed by a file descriptor in the process fd_array.
 * The socket_t is stored in file->private_data; f_op = &socket_fops.
 */

#include <kernel/types.h>
#include <kernel/process.h>
#include <net/socket.h>
#include <fs/vfs.h>
#include <mm/slab.h>
#include <lib/printk.h>
#include <lib/string.h>

extern int  get_unused_fd(void);
extern void fd_install(int fd, file_t *file);
extern file_t *fget(unsigned int fd);
extern void fput(file_t *file);
extern sock_t  *sk_alloc(sa_family_t family, int type, int protocol);
extern socket_t *sock_alloc(void);
extern void sk_free(sock_t *sk);
extern int  tcp_connect(sock_t *sk, u32 dst_ip, u16 dst_port);
extern int  tcp_listen(sock_t *sk);
extern sock_t *tcp_accept(sock_t *sk, int flags);
extern int  tcp_send_data(sock_t *sk, const void *data, u32 len);
extern int  tcp_close(sock_t *sk);
extern int  udp_send(sock_t *sk, const void *data, u32 len, u32 dst_ip, u16 dst_port);
extern u16  inet_alloc_port(void);
extern void inet_free_port(u16 port);
extern s64  sock_ringbuf_read(sock_ringbuf_t *rb, void *data, u32 len, bool peek);

/* =========================================================
 * socket_fops — file_operations for socket file descriptors
 * ========================================================= */

static s64 socket_file_read(file_t *file, char *buf, size_t count, u64 *pos) {
    (void)pos;
    socket_t *sock = (socket_t *)file->private_data;
    if (!sock || !sock->sk) return -EBADF;
    sock_t *sk = sock->sk;

    /* Blocking read: spin until data or error */
    bool nonblock = !!(file->f_flags & O_NONBLOCK);
    u64 deadline  = jiffies + (u64)(sk->sk_rcvtimeo ? sk->sk_rcvtimeo / 10 : 300*100);

    for (;;) {
        s64 n = sock_ringbuf_read(&sk->sk_rcvbuf, buf, (u32)count, false);
        if (n > 0) return n;
        if (sk->sk_state == TCP_CLOSED || sk->sk_state == TCP_CLOSE_WAIT) return 0;
        if (nonblock) return -EAGAIN;
        if (jiffies >= deadline) return -ETIMEDOUT;
        __asm__ volatile("pause");
    }
}

static s64 socket_file_write(file_t *file, const char *buf, size_t count, u64 *pos) {
    (void)pos;
    socket_t *sock = (socket_t *)file->private_data;
    if (!sock || !sock->sk) return -EBADF;
    sock_t *sk = sock->sk;

    if (sk->sk_type == SOCK_STREAM) {
        return (s64)tcp_send_data(sk, buf, (u32)count);
    }
    return -EOPNOTSUPP;
}

static int socket_file_release(inode_t *inode, file_t *file) {
    (void)inode;
    socket_t *sock = (socket_t *)file->private_data;
    if (!sock) return 0;
    if (sock->sk) {
        if (sock->sk->sk_type == SOCK_STREAM)
            tcp_close(sock->sk);
        sk_free(sock->sk);
    }
    kfree(sock);
    file->private_data = NULL;
    return 0;
}

static const file_operations_t socket_fops = {
    .read    = socket_file_read,
    .write   = socket_file_write,
    .release = socket_file_release,
};

/* =========================================================
 * Allocate a socket file descriptor
 * ========================================================= */

static int socket_alloc_fd(socket_t *sock, int flags) {
    int fd = get_unused_fd();
    if (fd < 0) return -EMFILE;

    file_t *file = kzalloc(sizeof(file_t), GFP_KERNEL);
    if (!file) return -ENOMEM;

    atomic_set(&file->f_count, 1);
    file->f_op           = &socket_fops;
    file->private_data   = sock;
    file->f_flags        = (u32)flags;
    sock->file           = file;

    fd_install(fd, file);
    return fd;
}

static socket_t *fd_to_socket(int sockfd) {
    file_t *f = fget((unsigned int)sockfd);
    if (!f) return NULL;
    if (f->f_op != &socket_fops) { fput(f); return NULL; }
    socket_t *sock = (socket_t *)f->private_data;
    fput(f);
    return sock;
}

/* =========================================================
 * sys_socket
 * ========================================================= */

s64 sys_socket(int domain, int type, int protocol) {
    int sock_type = type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
    int flags     = type & (SOCK_NONBLOCK | SOCK_CLOEXEC);

    if (domain != AF_INET && domain != AF_UNIX && domain != AF_INET6)
        return -EAFNOSUPPORT;
    if (sock_type != SOCK_STREAM && sock_type != SOCK_DGRAM &&
        sock_type != SOCK_RAW)
        return -EINVAL;

    /* Auto-select protocol */
    if (!protocol) {
        protocol = (sock_type == SOCK_STREAM) ? IPPROTO_TCP : IPPROTO_UDP;
    }

    socket_t *sock = sock_alloc();
    if (!sock) return -ENOMEM;

    sock->type = (short)sock_type;
    sock->sk   = sk_alloc((sa_family_t)domain, sock_type, protocol);
    if (!sock->sk) { kfree(sock); return -ENOMEM; }

    int fd = socket_alloc_fd(sock, O_RDWR | (flags & SOCK_NONBLOCK ? O_NONBLOCK : 0));
    if (fd < 0) { sk_free(sock->sk); kfree(sock); }
    return (s64)fd;
}

/* =========================================================
 * sys_bind
 * ========================================================= */

s64 sys_bind(int sockfd, const struct sockaddr *addr, u32 addrlen) {
    if (!addr) return -EFAULT;
    socket_t *sock = fd_to_socket(sockfd);
    if (!sock) return -EBADF;
    sock_t *sk = sock->sk;

    if (addr->sa_family == AF_INET) {
        if (addrlen < sizeof(struct sockaddr_in)) return -EINVAL;
        const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
        sk->sk_local_ip   = ntohl(sin->sin_addr.s_addr);
        sk->sk_local_port = ntohs(sin->sin_port);
        if (!sk->sk_local_port) {
            sk->sk_local_port = inet_alloc_port();
            if (!sk->sk_local_port) return -EADDRINUSE;
        }
    } else if (addr->sa_family == AF_UNIX) {
        if (addrlen < sizeof(struct sockaddr_un)) return -EINVAL;
        const struct sockaddr_un *sun = (const struct sockaddr_un *)addr;
        memcpy(sk->sk_unix_path, sun->sun_path,
               sizeof(sk->sk_unix_path) - 1);
    } else {
        return -EAFNOSUPPORT;
    }

    sock->state = SS_UNCONNECTED;
    return 0;
}

/* =========================================================
 * sys_connect
 * ========================================================= */

s64 sys_connect(int sockfd, const struct sockaddr *addr, u32 addrlen) {
    if (!addr) return -EFAULT;
    socket_t *sock = fd_to_socket(sockfd);
    if (!sock) return -EBADF;
    sock_t *sk = sock->sk;

    if (addr->sa_family != (sa_family_t)sk->sk_family) return -EAFNOSUPPORT;

    if (addr->sa_family == AF_INET) {
        if (addrlen < sizeof(struct sockaddr_in)) return -EINVAL;
        const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
        u32 dst_ip   = ntohl(sin->sin_addr.s_addr);
        u16 dst_port = ntohs(sin->sin_port);

        if (!sk->sk_local_port) {
            sk->sk_local_port = inet_alloc_port();
            if (!sk->sk_local_port) return -EADDRINUSE;
        }

        sock->state = SS_CONNECTING;
        int r;
        if (sk->sk_type == SOCK_STREAM) {
            r = tcp_connect(sk, dst_ip, dst_port);
        } else {
            /* UDP: just record destination */
            sk->sk_remote_ip   = dst_ip;
            sk->sk_remote_port = dst_port;
            sk->sk_connected   = true;
            r = 0;
        }

        if (r == 0) {
            sock->state = SS_CONNECTED;
            sk->sk_connected = true;
        } else {
            sock->state = SS_UNCONNECTED;
        }
        return (s64)r;
    }

    if (addr->sa_family == AF_UNIX) {
        /* Unix socket connect: find bound peer and create pair */
        const struct sockaddr_un *sun = (const struct sockaddr_un *)addr;
        (void)sun;
        /* Simplified: UNIX sockets not fully implemented yet */
        return -ENOENT;
    }

    return -EAFNOSUPPORT;
}

/* =========================================================
 * sys_listen
 * ========================================================= */

s64 sys_listen(int sockfd, int backlog) {
    socket_t *sock = fd_to_socket(sockfd);
    if (!sock) return -EBADF;
    sock_t *sk = sock->sk;

    if (sk->sk_type != SOCK_STREAM) return -EOPNOTSUPP;
    if (!sk->sk_local_port) return -EDESTADDRREQ;

    if (backlog > SOCK_BACKLOG_MAX) backlog = SOCK_BACKLOG_MAX;
    sk->sk_backlog_max = backlog;

    int r = tcp_listen(sk);
    if (r == 0) sock->state = SS_CONNECTED; /* listening */
    return (s64)r;
}

/* =========================================================
 * sys_accept4
 * ========================================================= */

s64 sys_accept4(int sockfd, struct sockaddr *addr, u32 *addrlen, int flags) {
    socket_t *sock = fd_to_socket(sockfd);
    if (!sock) return -EBADF;
    sock_t *sk = sock->sk;

    if (sk->sk_type != SOCK_STREAM) return -EOPNOTSUPP;
    if (sk->sk_state != TCP_LISTEN) return -EINVAL;

    sock_t *new_sk = tcp_accept(sk, flags);
    if (IS_ERR(new_sk)) return PTR_ERR(new_sk);
    if (!new_sk) return -EAGAIN;

    /* Fill peer address */
    if (addr && addrlen && *addrlen >= sizeof(struct sockaddr_in)) {
        struct sockaddr_in *sin = (struct sockaddr_in *)addr;
        sin->sin_family      = AF_INET;
        sin->sin_port        = htons(new_sk->sk_remote_port);
        sin->sin_addr.s_addr = htonl(new_sk->sk_remote_ip);
        *addrlen = sizeof(struct sockaddr_in);
    }

    socket_t *new_sock = sock_alloc();
    if (!new_sock) { sk_free(new_sk); return -ENOMEM; }
    new_sock->sk    = new_sk;
    new_sock->state = SS_CONNECTED;
    new_sock->type  = sock->type;

    int new_fd = socket_alloc_fd(new_sock,
                                  O_RDWR | (flags & SOCK_NONBLOCK ? O_NONBLOCK : 0));
    if (new_fd < 0) { sk_free(new_sk); kfree(new_sock); return (s64)new_fd; }
    return (s64)new_fd;
}

s64 sys_accept(int sockfd, struct sockaddr *addr, u32 *addrlen) {
    return sys_accept4(sockfd, addr, addrlen, 0);
}

/* =========================================================
 * sys_sendto
 * ========================================================= */

s64 sys_sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest, u32 addrlen) {
    if (!buf) return -EFAULT;
    (void)flags;

    socket_t *sock = fd_to_socket(sockfd);
    if (!sock) return -EBADF;
    sock_t *sk = sock->sk;

    if (sk->sk_type == SOCK_STREAM) {
        return (s64)tcp_send_data(sk, buf, (u32)len);
    }

    if (sk->sk_type == SOCK_DGRAM) {
        u32 dst_ip;
        u16 dst_port;

        if (dest && addrlen >= sizeof(struct sockaddr_in)) {
            const struct sockaddr_in *sin = (const struct sockaddr_in *)dest;
            dst_ip   = ntohl(sin->sin_addr.s_addr);
            dst_port = ntohs(sin->sin_port);
        } else if (sk->sk_connected) {
            dst_ip   = sk->sk_remote_ip;
            dst_port = sk->sk_remote_port;
        } else {
            return -EDESTADDRREQ;
        }

        if (!sk->sk_local_port) {
            sk->sk_local_port = inet_alloc_port();
            if (!sk->sk_local_port) return -EADDRINUSE;
        }

        return (s64)udp_send(sk, buf, (u32)len, dst_ip, dst_port);
    }

    return -EOPNOTSUPP;
}

s64 sys_send(int sockfd, const void *buf, size_t len, int flags) {
    return sys_sendto(sockfd, buf, len, flags, NULL, 0);
}

/* =========================================================
 * sys_recvfrom
 * ========================================================= */

s64 sys_recvfrom(int sockfd, void *buf, size_t len, int flags,
                  struct sockaddr *src_addr, u32 *addrlen) {
    if (!buf) return -EFAULT;
    socket_t *sock = fd_to_socket(sockfd);
    if (!sock) return -EBADF;
    sock_t *sk = sock->sk;

    file_t *f = fget((unsigned int)sockfd);
    bool nonblock = f ? !!(f->f_flags & O_NONBLOCK) : false;
    if (f) fput(f);

    bool peek = !!(flags & MSG_PEEK);
    u64 deadline = jiffies + (nonblock ? 0 : (sk->sk_rcvtimeo ? sk->sk_rcvtimeo/10 : 300*100));

    for (;;) {
        s64 n = sock_ringbuf_read(&sk->sk_rcvbuf, buf, (u32)len, peek);
        if (n > 0) {
            if (src_addr && addrlen && *addrlen >= sizeof(struct sockaddr_in) &&
                sk->sk_family == AF_INET) {
                struct sockaddr_in *sin = (struct sockaddr_in *)src_addr;
                sin->sin_family      = AF_INET;
                sin->sin_port        = htons(sk->sk_remote_port);
                sin->sin_addr.s_addr = htonl(sk->sk_remote_ip);
                *addrlen = sizeof(struct sockaddr_in);
            }
            return n;
        }
        if (sk->sk_state == TCP_CLOSED) return 0;
        if (nonblock || jiffies >= deadline) return -EAGAIN;
        __asm__ volatile("pause");
    }
}

s64 sys_recv(int sockfd, void *buf, size_t len, int flags) {
    return sys_recvfrom(sockfd, buf, len, flags, NULL, NULL);
}

/* =========================================================
 * sys_sendmsg / sys_recvmsg
 * ========================================================= */

s64 sys_sendmsg(int sockfd, const struct msghdr *msg, int flags) {
    if (!msg) return -EFAULT;
    s64 total = 0;
    for (size_t i = 0; i < msg->msg_iovlen; i++) {
        s64 r = sys_sendto(sockfd, msg->msg_iov[i].iov_base,
                            msg->msg_iov[i].iov_len, flags,
                            (const struct sockaddr *)msg->msg_name,
                            (u32)msg->msg_namelen);
        if (r < 0) return total ? total : r;
        total += r;
    }
    return total;
}

s64 sys_recvmsg(int sockfd, struct msghdr *msg, int flags) {
    if (!msg) return -EFAULT;
    s64 total = 0;
    for (size_t i = 0; i < msg->msg_iovlen; i++) {
        s64 r = sys_recvfrom(sockfd, msg->msg_iov[i].iov_base,
                              msg->msg_iov[i].iov_len, flags,
                              (struct sockaddr *)msg->msg_name,
                              msg->msg_name ? (u32 *)&msg->msg_namelen : NULL);
        if (r < 0) return total ? total : r;
        total += r;
        if ((size_t)r < msg->msg_iov[i].iov_len) break;
    }
    return total;
}

/* =========================================================
 * sys_shutdown
 * ========================================================= */

s64 sys_shutdown(int sockfd, int how) {
    socket_t *sock = fd_to_socket(sockfd);
    if (!sock) return -EBADF;
    sock_t *sk = sock->sk;

    if (how == SHUT_RD || how == SHUT_RDWR)  sk->sk_shutdown_rd = true;
    if (how == SHUT_WR || how == SHUT_RDWR)  sk->sk_shutdown_wr = true;

    if ((how == SHUT_WR || how == SHUT_RDWR) && sk->sk_type == SOCK_STREAM)
        tcp_close(sk);

    return 0;
}

/* =========================================================
 * sys_setsockopt / getsockopt
 * ========================================================= */

s64 sys_setsockopt(int sockfd, int level, int optname,
                    const void *optval, u32 optlen) {
    if (!optval) return -EFAULT;
    socket_t *sock = fd_to_socket(sockfd);
    if (!sock) return -EBADF;
    sock_t *sk = sock->sk;

    int ival = (optlen >= 4) ? *(const int *)optval : 0;

    if (level == SOL_SOCKET) {
        switch (optname) {
        case SO_REUSEADDR:  sk->sk_reuseaddr  = !!ival; return 0;
        case SO_REUSEPORT:  sk->sk_reuseport  = !!ival; return 0;
        case SO_BROADCAST:  sk->sk_broadcast  = !!ival; return 0;
        case SO_KEEPALIVE:  sk->sk_keepalive  = !!ival; return 0;
        case SO_SNDBUF:     /* ignored */               return 0;
        case SO_RCVBUF:     /* ignored */               return 0;
        case SO_RCVTIMEO:   {
            if (optlen >= sizeof(struct timeval)) {
                const struct timeval *tv = (const struct timeval *)optval;
                sk->sk_rcvtimeo = (int)(tv->tv_sec * 1000 + tv->tv_usec / 1000);
            }
            return 0;
        }
        case SO_SNDTIMEO: {
            if (optlen >= sizeof(struct timeval)) {
                const struct timeval *tv = (const struct timeval *)optval;
                sk->sk_sndtimeo = (int)(tv->tv_sec * 1000 + tv->tv_usec / 1000);
            }
            return 0;
        }
        case SO_LINGER:     return 0;  /* No linger */
        default:            return -ENOPROTOOPT;
        }
    }

    if (level == SOL_TCP || level == IPPROTO_TCP) {
        switch (optname) {
        case TCP_NODELAY:   sk->sk_nodelay = !!ival; return 0;
        case TCP_KEEPIDLE:
        case TCP_KEEPINTVL:
        case TCP_KEEPCNT:   return 0;  /* Accepted but ignored */
        case TCP_CORK:      return 0;
        default:            return -ENOPROTOOPT;
        }
    }

    if (level == SOL_IP || level == IPPROTO_IP) {
        switch (optname) {
        case IP_TTL:        sk->sk_ttl = ival; return 0;
        case IP_TOS:        sk->sk_tos = ival; return 0;
        default:            return -ENOPROTOOPT;
        }
    }

    return -ENOPROTOOPT;
}

s64 sys_getsockopt(int sockfd, int level, int optname,
                    void *optval, u32 *optlen) {
    if (!optval || !optlen) return -EFAULT;
    socket_t *sock = fd_to_socket(sockfd);
    if (!sock) return -EBADF;
    sock_t *sk = sock->sk;

    auto void store_int(int v) {
        u32 sz = *optlen < 4 ? *optlen : 4;
        memcpy(optval, &v, sz);
        *optlen = sz;
    }

    if (level == SOL_SOCKET) {
        switch (optname) {
        case SO_TYPE:       store_int(sk->sk_type);     return 0;
        case SO_ERROR:      store_int(sk->sk_err);
                            sk->sk_err = 0;             return 0;
        case SO_REUSEADDR:  store_int(sk->sk_reuseaddr); return 0;
        case SO_KEEPALIVE:  store_int(sk->sk_keepalive); return 0;
        case SO_ACCEPTCONN: store_int(sk->sk_listening); return 0;
        case SO_SNDBUF:     store_int(SOCK_SNDBUF_SIZE); return 0;
        case SO_RCVBUF:     store_int(SOCK_RCVBUF_SIZE); return 0;
        default:            return -ENOPROTOOPT;
        }
    }

    if (level == SOL_TCP || level == IPPROTO_TCP) {
        switch (optname) {
        case TCP_NODELAY:   store_int(sk->sk_nodelay);  return 0;
        default:            return -ENOPROTOOPT;
        }
    }

    return -ENOPROTOOPT;
}

/* =========================================================
 * sys_getsockname / getpeername
 * ========================================================= */

s64 sys_getsockname(int sockfd, struct sockaddr *addr, u32 *addrlen) {
    if (!addr || !addrlen) return -EFAULT;
    socket_t *sock = fd_to_socket(sockfd);
    if (!sock) return -EBADF;
    sock_t *sk = sock->sk;

    if (sk->sk_family == AF_INET) {
        if (*addrlen < sizeof(struct sockaddr_in)) return -EINVAL;
        struct sockaddr_in *sin = (struct sockaddr_in *)addr;
        sin->sin_family      = AF_INET;
        sin->sin_port        = htons(sk->sk_local_port);
        sin->sin_addr.s_addr = htonl(sk->sk_local_ip);
        *addrlen = sizeof(struct sockaddr_in);
        return 0;
    }
    return -EAFNOSUPPORT;
}

s64 sys_getpeername(int sockfd, struct sockaddr *addr, u32 *addrlen) {
    if (!addr || !addrlen) return -EFAULT;
    socket_t *sock = fd_to_socket(sockfd);
    if (!sock) return -EBADF;
    sock_t *sk = sock->sk;

    if (!sk->sk_connected) return -ENOTCONN;

    if (sk->sk_family == AF_INET) {
        if (*addrlen < sizeof(struct sockaddr_in)) return -EINVAL;
        struct sockaddr_in *sin = (struct sockaddr_in *)addr;
        sin->sin_family      = AF_INET;
        sin->sin_port        = htons(sk->sk_remote_port);
        sin->sin_addr.s_addr = htonl(sk->sk_remote_ip);
        *addrlen = sizeof(struct sockaddr_in);
        return 0;
    }
    return -EAFNOSUPPORT;
}

/* =========================================================
 * sys_socketpair — create a connected pair of sockets
 * ========================================================= */

s64 sys_socketpair(int domain, int type, int protocol, int sv[2]) {
    if (!sv) return -EFAULT;

    /* Only AF_UNIX / SOCK_STREAM supported for now */
    if (domain != AF_UNIX && domain != AF_INET) return -EAFNOSUPPORT;

    /* Allocate two connected sockets */
    sock_t *sk0 = sk_alloc((sa_family_t)domain, type & ~(SOCK_NONBLOCK|SOCK_CLOEXEC),
                             protocol);
    sock_t *sk1 = sk_alloc((sa_family_t)domain, type & ~(SOCK_NONBLOCK|SOCK_CLOEXEC),
                             protocol);
    if (!sk0 || !sk1) {
        if (sk0) sk_free(sk0);
        if (sk1) sk_free(sk1);
        return -ENOMEM;
    }

    /* Cross-connect the pair via peer pointer */
    sk0->sk_peer      = sk1;
    sk1->sk_peer      = sk0;
    sk0->sk_state     = TCP_ESTABLISHED;
    sk1->sk_state     = TCP_ESTABLISHED;
    sk0->sk_connected = true;
    sk1->sk_connected = true;

    socket_t *sock0 = sock_alloc();
    socket_t *sock1 = sock_alloc();
    if (!sock0 || !sock1) {
        if (sock0) kfree(sock0);
        if (sock1) kfree(sock1);
        sk_free(sk0); sk_free(sk1);
        return -ENOMEM;
    }

    sock0->sk = sk0; sock0->state = SS_CONNECTED;
    sock1->sk = sk1; sock1->state = SS_CONNECTED;

    sv[0] = socket_alloc_fd(sock0, O_RDWR);
    if (sv[0] < 0) { kfree(sock0); kfree(sock1); sk_free(sk0); sk_free(sk1); return sv[0]; }

    sv[1] = socket_alloc_fd(sock1, O_RDWR);
    if (sv[1] < 0) {
        /* Close sv[0] */
        extern int do_close(int fd);
        do_close(sv[0]);
        kfree(sock1); sk_free(sk1);
        return sv[1];
    }

    return 0;
}

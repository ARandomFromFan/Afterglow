/* nn::socket: loopback-only BSD socket layer. */
#include "hos.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

uint64_t hos_sys_alloc(uint64_t size, uint64_t align);

static int force_online(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("HOS_FORCE_ONLINE"); v = e && *e == '1'; }
    return v;
}

static __thread int last_errno = 0;
static __thread int last_host_errno = 0;

/* HOS_NET_TRACE=1: one line per socket call, args preformatted by the caller, no payload bytes. */
static int net_trace_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("HOS_NET_TRACE"); v = e && *e == '1'; }
    return v;
}

static void net_tracef(const char *call, const char *args, long long ret) {
    if (net_trace_on()) hos_log("net: trace %s(%s) -> %lld errno %d", call, args, ret, last_errno);
}

static void net_tracep(const char *call, unsigned long long arg, long long ret) {
    if (net_trace_on()) hos_log("net: trace %s(%#llx) -> %lld errno %d", call, arg, ret, last_errno);
}

static void set_err(int err) {
    /* newlib errno numbers are the Linux ones, so both fields hold the host value */
    last_host_errno = err;
    last_errno = err;
}

static int map_msg_flags(int gflags) {
    int flags = 0;
    if (gflags & 0x02) flags |= MSG_PEEK;
    if (gflags & 0x80) flags |= MSG_DONTWAIT;
    return flags;
}

static void write_guest_sockaddr(uint64_t gaddr, uint64_t glen_addr, const struct sockaddr_in *sin) {
    if (gaddr) {
        uint8_t *p = (uint8_t *)GP(gaddr);
        p[0] = 16;
        p[1] = 2;
        memcpy(p + 2, &sin->sin_port, 2);
        memcpy(p + 4, &sin->sin_addr.s_addr, 4);
        memset(p + 8, 0, 8);
    }
    if (glen_addr) {
        G32(glen_addr) = 16;
    }
}

static int map_sockopt(int glevel, int gopt, int *hlevel, int *hopt) {
    if (glevel == 0xffff) {
        *hlevel = SOL_SOCKET;
        switch (gopt) {
        case 0x0004: *hopt = SO_REUSEADDR; return 0;
        case 0x0008: *hopt = SO_KEEPALIVE; return 0;
        case 0x0020: *hopt = SO_BROADCAST; return 0;
        case 0x0080: *hopt = SO_LINGER; return 0;
        case 0x1001: *hopt = SO_SNDBUF; return 0;
        case 0x1002: *hopt = SO_RCVBUF; return 0;
        case 0x1005: *hopt = SO_SNDTIMEO; return 0;
        case 0x1006: *hopt = SO_RCVTIMEO; return 0;
        case 0x1007: *hopt = SO_ERROR; return 0;
        case 0x1008: *hopt = SO_TYPE; return 0;
        default: return -1;
        }
    } else if (glevel == 6) {
        *hlevel = IPPROTO_TCP;
        if (gopt == 1) { *hopt = TCP_NODELAY; return 0; }
        return -1;
    }
    return -1;
}

static void hle_socket_impl(Ctx *c) {
    int family = (int)ARG(0);
    int raw_type = (int)ARG(1);
    int proto = (int)ARG(2);
    long long r;
    if (!force_online()) {
        last_errno = 100; /* ENETDOWN, the Linux value the guest libc uses */
        last_host_errno = 100;
        r = -1;
    } else {
        int type = raw_type & 0xf;
        if (raw_type & ~0xf) hos_log("net: socket type high flags %#x", raw_type & ~0xf);
        if (family != 2 || (type != 1 && type != 2)) {
            last_errno = 22;
            last_host_errno = 22;
            r = -1;
        } else {
            int fd = socket(AF_INET, (type == 1) ? SOCK_STREAM : SOCK_DGRAM, proto);
            if (fd < 0) { set_err(errno); r = -1; }
            else r = fd;
        }
    }
    char ta[128];
    HosThread *self = hos_self();
    snprintf(ta, sizeof ta, "family=%d type=%d proto=%d lr=%#llx thread=%s", family, raw_type, proto,
             (unsigned long long)c->x[30], self ? hos_thread_name(self) : "?");
    net_tracef("socket", ta, r);
    HLE_RETURN((uint64_t)r);
}

HLE(_ZN2nn6socket6SocketENS0_6FamilyENS0_4TypeENS0_8ProtocolE) { hle_socket_impl(c); }
HLE(_ZN2nn6socket6SocketEiii) { hle_socket_impl(c); }

static void hle_connect_impl(Ctx *c) {
    int fd = (int)ARG(0);
    uint64_t gaddr = ARG(1);
    uint32_t glen = (uint32_t)ARG(2);
    char ta[96];
    long long r;
    if (!gaddr || glen < 8) {
        last_errno = 22;
        last_host_errno = 22;
        r = -1;
        snprintf(ta, sizeof ta, "fd=%d addr=?", fd);
    } else {
        uint8_t *p = (uint8_t *)GP(gaddr);
        uint16_t orig_port_be;
        memcpy(&orig_port_be, p + 2, 2);
        uint16_t orig_port = ntohs(orig_port_be);
        hos_log("net: connect fd=%d to %u.%u.%u.%u:%u", fd, p[4], p[5], p[6], p[7], orig_port);
        struct sockaddr_in dst;
        memset(&dst, 0, sizeof(dst));
        dst.sin_family = AF_INET;
        dst.sin_port = (orig_port == 443) ? htons(18443) : orig_port_be;
        dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int ret = connect(fd, (struct sockaddr *)&dst, sizeof(dst));
        if (ret < 0) { set_err(errno); r = -1; }
        else r = 0;
        snprintf(ta, sizeof ta, "fd=%d addr=%u.%u.%u.%u:%u", fd, p[4], p[5], p[6], p[7], orig_port);
    }
    net_tracef("connect", ta, r);
    HLE_RETURN((uint64_t)r);
}

HLE(_ZN2nn6socket7ConnectEiPK8sockaddrj) { hle_connect_impl(c); }
HLE(_ZN2nn6socket7ConnectEiPKNS0_8SockAddrEj) { hle_connect_impl(c); }

static void hle_bind_impl(Ctx *c) {
    int fd = (int)ARG(0);
    uint64_t gaddr = ARG(1);
    uint32_t glen = (uint32_t)ARG(2);
    uint16_t port_be = 0;
    if (gaddr && glen >= 4) memcpy(&port_be, (uint8_t *)GP(gaddr) + 2, 2);
    hos_log("net: bind fd=%d to 127.0.0.1:%u", fd, ntohs(port_be));
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = port_be;
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int ret = bind(fd, (struct sockaddr *)&dst, sizeof(dst));
    long long r;
    if (ret < 0) { set_err(errno); r = -1; } else r = 0;
    char ta[64];
    snprintf(ta, sizeof ta, "fd=%d port=%u", fd, ntohs(port_be));
    net_tracef("bind", ta, r);
    HLE_RETURN((uint64_t)r);
}

HLE(_ZN2nn6socket4BindEiPK8sockaddrj) { hle_bind_impl(c); }
HLE(_ZN2nn6socket4BindEiPKNS0_8SockAddrEj) { hle_bind_impl(c); }

static void hle_sendto_impl(Ctx *c) {
    int fd = (int)ARG(0);
    void *buf = GP(ARG(1));
    size_t len = (size_t)ARG(2);
    int flags = map_msg_flags((int)ARG(3)) | MSG_NOSIGNAL;
    uint64_t gaddr = ARG(4);
    uint32_t glen = (uint32_t)ARG(5);
    ssize_t ret;
    char ta[96];
    if (gaddr && glen >= 8) {
        uint8_t *p = (uint8_t *)GP(gaddr);
        uint16_t orig_port_be;
        memcpy(&orig_port_be, p + 2, 2);
        uint16_t orig_port = ntohs(orig_port_be);
        hos_log("net: sendto fd=%d to %u.%u.%u.%u:%u len=%zu", fd, p[4], p[5], p[6], p[7], orig_port, len);
        struct sockaddr_in dst;
        memset(&dst, 0, sizeof(dst));
        dst.sin_family = AF_INET;
        dst.sin_port = (orig_port == 443) ? htons(18443) : orig_port_be;
        dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ret = sendto(fd, buf, len, flags, (struct sockaddr *)&dst, sizeof(dst));
        snprintf(ta, sizeof ta, "fd=%d len=%zu flags=%d addr=%u.%u.%u.%u:%u", fd, len, flags, p[4], p[5], p[6], p[7], orig_port);
    } else {
        ret = send(fd, buf, len, flags);
        snprintf(ta, sizeof ta, "fd=%d len=%zu flags=%d", fd, len, flags);
    }
    long long r;
    if (ret < 0) { set_err(errno); r = -1; } else r = (long long)ret;
    net_tracef("sendto", ta, r);
    HLE_RETURN((uint64_t)r);
}

HLE(_ZN2nn6socket6SendToEiPKvmNS0_7MsgFlagEPKNS0_8SockAddrEj) { hle_sendto_impl(c); }
HLE(_ZN2nn6socket6SendToEiPKvmiPK8sockaddrj) { hle_sendto_impl(c); }

static void hle_send_impl(Ctx *c) {
    int fd = (int)ARG(0);
    void *buf = GP(ARG(1));
    size_t len = (size_t)ARG(2);
    int flags = map_msg_flags((int)ARG(3)) | MSG_NOSIGNAL;
    ssize_t ret = send(fd, buf, len, flags);
    long long r;
    if (ret < 0) { set_err(errno); r = -1; } else r = (long long)ret;
    char ta[64];
    snprintf(ta, sizeof ta, "fd=%d len=%zu flags=%d", fd, len, flags);
    net_tracef("send", ta, r);
    HLE_RETURN((uint64_t)r);
}

HLE(_ZN2nn6socket4SendEiPKvmNS0_7MsgFlagE) { hle_send_impl(c); }
HLE(_ZN2nn6socket4SendEiPKvmi) { hle_send_impl(c); }

static void hle_write_impl(Ctx *c) {
    int fd = (int)ARG(0);
    void *buf = GP(ARG(1));
    size_t len = (size_t)ARG(2);
    ssize_t ret = write(fd, buf, len);
    long long r;
    if (ret < 0) { set_err(errno); r = -1; } else r = (long long)ret;
    char ta[64];
    snprintf(ta, sizeof ta, "fd=%d len=%zu", fd, len);
    net_tracef("write", ta, r);
    HLE_RETURN((uint64_t)r);
}

HLE(_ZN2nn6socket5WriteEiPKvm) { hle_write_impl(c); }

static void hle_recv_impl(Ctx *c) {
    int fd = (int)ARG(0);
    void *buf = GP(ARG(1));
    size_t len = (size_t)ARG(2);
    int flags = map_msg_flags((int)ARG(3));
    ssize_t ret = recv(fd, buf, len, flags);
    long long r;
    if (ret < 0) { set_err(errno); r = -1; } else r = (long long)ret;
    char ta[64];
    snprintf(ta, sizeof ta, "fd=%d len=%zu flags=%d", fd, len, flags);
    net_tracef("recv", ta, r);
    HLE_RETURN((uint64_t)r);
}

HLE(_ZN2nn6socket4RecvEiPvmNS0_7MsgFlagE) { hle_recv_impl(c); }
HLE(_ZN2nn6socket4RecvEiPvmi) { hle_recv_impl(c); }

static void hle_read_impl(Ctx *c) {
    int fd = (int)ARG(0);
    void *buf = GP(ARG(1));
    size_t len = (size_t)ARG(2);
    ssize_t ret = read(fd, buf, len);
    long long r;
    if (ret < 0) { set_err(errno); r = -1; } else r = (long long)ret;
    char ta[64];
    snprintf(ta, sizeof ta, "fd=%d len=%zu", fd, len);
    net_tracef("read", ta, r);
    HLE_RETURN((uint64_t)r);
}

HLE(_ZN2nn6socket4ReadEiPvm) { hle_read_impl(c); }

static void hle_recvfrom_impl(Ctx *c) {
    int fd = (int)ARG(0);
    void *buf = GP(ARG(1));
    size_t len = (size_t)ARG(2);
    int flags = map_msg_flags((int)ARG(3));
    uint64_t gaddr = ARG(4);
    uint64_t glen = ARG(5);
    struct sockaddr_in src;
    socklen_t slen = sizeof(src);
    ssize_t ret;
    if (gaddr) {
        ret = recvfrom(fd, buf, len, flags, (struct sockaddr *)&src, &slen);
        if (ret >= 0) write_guest_sockaddr(gaddr, glen, &src);
    } else {
        ret = recv(fd, buf, len, flags);
    }
    long long r;
    if (ret < 0) { set_err(errno); r = -1; } else r = (long long)ret;
    char ta[64];
    snprintf(ta, sizeof ta, "fd=%d len=%zu flags=%d", fd, len, flags);
    net_tracef("recvfrom", ta, r);
    HLE_RETURN((uint64_t)r);
}

HLE(_ZN2nn6socket8RecvFromEiPvmNS0_7MsgFlagEPNS0_8SockAddrEPj) { hle_recvfrom_impl(c); }
HLE(_ZN2nn6socket8RecvFromEiPvmiP8sockaddrPj) { hle_recvfrom_impl(c); }

static __thread struct iovec h_iovecs[1024];

static void hle_sendmsg_impl(Ctx *c) {
    int fd = (int)ARG(0);
    uint64_t gmsg = ARG(1);
    int raw_flags = (int)ARG(2);
    long long r;
    if (!gmsg) {
        set_err(EINVAL);
        r = -1;
        net_tracef("sendmsg", "msg=NULL", r);
        HLE_RETURN((uint64_t)r);
    }
    uint64_t gname = G64(gmsg + 0);
    uint32_t gnamelen = G32(gmsg + 8);
    uint64_t giov = G64(gmsg + 16);
    uint32_t giovlen = G32(gmsg + 24);
    if (giovlen > 1024) {
        set_err(EMSGSIZE);
        r = -1;
        char ta[64];
        snprintf(ta, sizeof ta, "fd=%d iov=%u > 1024", fd, giovlen);
        net_tracef("sendmsg", ta, r);
        HLE_RETURN((uint64_t)r);
    }
    size_t total_len = 0;
    for (uint32_t i = 0; i < giovlen; i++) {
        uint64_t iov_base = G64(giov + i * 16 + 0);
        uint64_t iov_len = G64(giov + i * 16 + 8);
        h_iovecs[i].iov_base = iov_base ? GP(iov_base) : NULL;
        h_iovecs[i].iov_len = (size_t)iov_len;
        total_len += (size_t)iov_len;
    }
    struct msghdr hmsg;
    memset(&hmsg, 0, sizeof(hmsg));
    hmsg.msg_iov = h_iovecs;
    hmsg.msg_iovlen = giovlen;
    struct sockaddr_in dst;
    if (gname && gnamelen >= 8) {
        uint8_t *p = (uint8_t *)GP(gname);
        uint16_t orig_port_be;
        memcpy(&orig_port_be, p + 2, 2);
        uint16_t orig_port = ntohs(orig_port_be);
        memset(&dst, 0, sizeof(dst));
        dst.sin_family = AF_INET;
        dst.sin_port = (orig_port == 443) ? htons(18443) : orig_port_be;
        dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        hmsg.msg_name = &dst;
        hmsg.msg_namelen = sizeof(dst);
    }
    int flags = map_msg_flags(raw_flags) | MSG_NOSIGNAL;
    ssize_t ret = sendmsg(fd, &hmsg, flags);
    if (ret < 0) { set_err(errno); r = -1; } else r = (long long)ret;
    char ta[80];
    snprintf(ta, sizeof ta, "fd=%d iov=%u len=%zu flags=%#x", fd, giovlen, total_len, raw_flags);
    net_tracef("sendmsg", ta, r);
    HLE_RETURN((uint64_t)r);
}

static void hle_recvmsg_impl(Ctx *c) {
    int fd = (int)ARG(0);
    uint64_t gmsg = ARG(1);
    int raw_flags = (int)ARG(2);
    long long r;
    if (!gmsg) {
        set_err(EINVAL);
        r = -1;
        net_tracef("recvmsg", "msg=NULL", r);
        HLE_RETURN((uint64_t)r);
    }
    uint64_t gname = G64(gmsg + 0);
    uint32_t gnamelen = G32(gmsg + 8);
    uint64_t giov = G64(gmsg + 16);
    uint32_t giovlen = G32(gmsg + 24);
    if (giovlen > 1024) {
        set_err(EMSGSIZE);
        r = -1;
        char ta[64];
        snprintf(ta, sizeof ta, "fd=%d iov=%u > 1024", fd, giovlen);
        net_tracef("recvmsg", ta, r);
        HLE_RETURN((uint64_t)r);
    }
    size_t total_len = 0;
    for (uint32_t i = 0; i < giovlen; i++) {
        uint64_t iov_base = G64(giov + i * 16 + 0);
        uint64_t iov_len = G64(giov + i * 16 + 8);
        h_iovecs[i].iov_base = iov_base ? GP(iov_base) : NULL;
        h_iovecs[i].iov_len = (size_t)iov_len;
        total_len += (size_t)iov_len;
    }
    struct msghdr hmsg;
    memset(&hmsg, 0, sizeof(hmsg));
    hmsg.msg_iov = h_iovecs;
    hmsg.msg_iovlen = giovlen;
    struct sockaddr_in src;
    memset(&src, 0, sizeof(src));
    if (gname) {
        hmsg.msg_name = &src;
        hmsg.msg_namelen = sizeof(src);
    }
    int flags = map_msg_flags(raw_flags);
    ssize_t ret = recvmsg(fd, &hmsg, flags);
    if (ret < 0) {
        set_err(errno);
        r = -1;
    } else {
        r = (long long)ret;
        G32(gmsg + 40) = 0;
        if (gname) write_guest_sockaddr(gname, gmsg + 8, &src);
        else G32(gmsg + 8) = 0;
        int out_flags = 0;
        if (hmsg.msg_flags & MSG_TRUNC) out_flags |= 0x20;
        if (hmsg.msg_flags & MSG_CTRUNC) out_flags |= 0x40;
        G32(gmsg + 44) = (uint32_t)out_flags;
    }
    char ta[80];
    snprintf(ta, sizeof ta, "fd=%d iov=%u len=%zu flags=%#x", fd, giovlen, total_len, raw_flags);
    net_tracef("recvmsg", ta, r);
    HLE_RETURN((uint64_t)r);
}

static void hle_listen_impl(Ctx *c) {
    int fd = (int)ARG(0);
    int backlog = (int)ARG(1);
    int ret = listen(fd, backlog);
    long long r;
    if (ret < 0) { set_err(errno); r = -1; } else r = 0;
    char ta[64];
    snprintf(ta, sizeof ta, "fd=%d backlog=%d", fd, backlog);
    net_tracef("listen", ta, r);
    HLE_RETURN((uint64_t)r);
}

HLE(_ZN2nn6socket6ListenEii) { hle_listen_impl(c); }

static void hle_accept_impl(Ctx *c) {
    int fd = (int)ARG(0);
    uint64_t gaddr = ARG(1);
    uint64_t glen = ARG(2);
    struct sockaddr_in cli;
    socklen_t clen = sizeof(cli);
    int cfd = accept(fd, gaddr ? (struct sockaddr *)&cli : NULL, gaddr ? &clen : NULL);
    long long r;
    if (cfd < 0) { set_err(errno); r = -1; }
    else { if (gaddr) write_guest_sockaddr(gaddr, glen, &cli); r = cfd; }
    char ta[32];
    snprintf(ta, sizeof ta, "fd=%d", fd);
    net_tracef("accept", ta, r);
    HLE_RETURN((uint64_t)r);
}

HLE(_ZN2nn6socket6AcceptEiP8sockaddrPj) { hle_accept_impl(c); }
HLE(_ZN2nn6socket6AcceptEiPNS0_8SockAddrEPj) { hle_accept_impl(c); }

static void hle_shutdown_impl(Ctx *c) {
    int fd = (int)ARG(0);
    int how = (int)ARG(1);
    int ret = shutdown(fd, how);
    long long r;
    if (ret < 0) { set_err(errno); r = -1; } else r = 0;
    char ta[64];
    snprintf(ta, sizeof ta, "fd=%d how=%d", fd, how);
    net_tracef("shutdown", ta, r);
    HLE_RETURN((uint64_t)r);
}

HLE(_ZN2nn6socket8ShutdownEiNS0_14ShutdownMethodE) { hle_shutdown_impl(c); }
HLE(_ZN2nn6socket8ShutdownEii) { hle_shutdown_impl(c); }

static void hle_close_impl(Ctx *c) {
    int fd = (int)ARG(0);
    int ret = close(fd);
    long long r;
    if (ret < 0) { set_err(errno); r = -1; } else r = 0;
    char ta[32];
    snprintf(ta, sizeof ta, "fd=%d", fd);
    net_tracef("close", ta, r);
    HLE_RETURN((uint64_t)r);
}

HLE(_ZN2nn6socket5CloseEi) { hle_close_impl(c); }

/* host/nn_ssl.c: bytes of a TLS connection already decrypted and buffered in the host, so a poll/select on
   its socket reports the connection readable even though the kernel socket has nothing left */
int nn_ssl_pending(int fd);

static void hle_poll_impl(Ctx *c) {
    uint64_t gfds = ARG(0);
    nfds_t nfds = (nfds_t)ARG(1);
    int timeout = (int)ARG(2);
    if (nfds > 256) nfds = 256;
    struct pollfd pfds[256];
    if (gfds && nfds > 0) memcpy(pfds, GP(gfds), nfds * sizeof(struct pollfd));
    int ret = poll(pfds, nfds, timeout);
    long long r;
    if (ret >= 0 && gfds && nfds > 0) {
        /* a TLS connection whose plaintext the host already holds is readable for the guest */
        for (nfds_t i = 0; i < nfds; i++)
            if ((pfds[i].events & POLLIN) && !(pfds[i].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) &&
                nn_ssl_pending(pfds[i].fd) > 0) {
                pfds[i].revents |= POLLIN;
                ret++;
            }
    }
    if (ret < 0) { set_err(errno); r = -1; } else r = ret;
    if (net_trace_on()) {
        static uint32_t poll_calls = 0;
        uint32_t n = __atomic_add_fetch(&poll_calls, 1, __ATOMIC_RELAXED);
        if (n <= 40 || r != 0) {
            char ta[64];
            snprintf(ta, sizeof ta, "nfds=%zu timeout=%d", (size_t)nfds, timeout);
            net_tracef("poll", ta, r);
        }
    }
    if (ret < 0) HLE_RETURN((uint64_t)-1);
    if (gfds && nfds > 0) {
        struct pollfd *gp = (struct pollfd *)GP(gfds);
        for (nfds_t i = 0; i < nfds; i++) gp[i].revents = pfds[i].revents;
    }
    HLE_RETURN((uint64_t)r);
}

HLE(_ZN2nn6socket4PollEP6pollfdmi) { hle_poll_impl(c); }
HLE(_ZN2nn6socket4PollEPNS0_6PollFdEmi) { hle_poll_impl(c); }

static void hle_select_impl(Ctx *c) {
    int nfds = (int)ARG(0);
    uint64_t r_addr = ARG(1);
    uint64_t w_addr = ARG(2);
    uint64_t e_addr = ARG(3);
    uint64_t tv_addr = ARG(4);
    fd_set rfds, wfds, efds;
    if (r_addr) memcpy(&rfds, GP(r_addr), 128);
    if (w_addr) memcpy(&wfds, GP(w_addr), 128);
    if (e_addr) memcpy(&efds, GP(e_addr), 128);
    struct timeval tv, *ptv = NULL;
    if (tv_addr) {
        tv.tv_sec = (time_t)G64(tv_addr);
        tv.tv_usec = (suseconds_t)G64(tv_addr + 8);
        ptv = &tv;
    }
    int ret = select(nfds, r_addr ? &rfds : NULL, w_addr ? &wfds : NULL, e_addr ? &efds : NULL, ptv);
    long long r;
    if (ret >= 0 && r_addr) {
        /* same as poll: plaintext buffered by the host makes the fd readable */
        const int lim = nfds < 1024 ? nfds : 1024;
        for (int i = 0; i < lim; i++)
            if (!FD_ISSET(i, &rfds) && nn_ssl_pending(i) > 0) {
                FD_SET(i, &rfds);
                ret++;
            }
    }
    if (ret < 0) { set_err(errno); r = -1; }
    else {
        if (r_addr) memcpy(GP(r_addr), &rfds, 128);
        if (w_addr) memcpy(GP(w_addr), &wfds, 128);
        if (e_addr) memcpy(GP(e_addr), &efds, 128);
        r = ret;
    }
    char ta[64];
    snprintf(ta, sizeof ta, "nfds=%d timeout=%s", nfds, tv_addr ? "set" : "null");
    net_tracef("select", ta, r);
    HLE_RETURN((uint64_t)r);
}

HLE(_ZN2nn6socket6SelectEiPNS0_5FdSetES2_S2_PNS0_7TimeValE) { hle_select_impl(c); }

static void hle_getsockopt_impl(Ctx *c) {
    int fd = (int)ARG(0);
    int level = (int)ARG(1);
    int opt = (int)ARG(2);
    uint64_t val = ARG(3);
    uint64_t optlen = ARG(4);
    long long r;
    if (level == 0xffff && opt == 0x1007) {
        int err = 0;
        socklen_t slen = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &slen) < 0) {
            set_err(errno);
            r = -1;
        } else {
            if (val && optlen && G32(optlen) >= sizeof(int)) {
                G32(val) = err;
                G32(optlen) = sizeof(int);
            }
            r = 0;
        }
    } else {
        int hlevel, hopt;
        if (map_sockopt(level, opt, &hlevel, &hopt) < 0) {
            hos_log("net: unknown getsockopt level=%#x opt=%#x", level, opt);
            r = 0;
        } else {
            socklen_t slen = optlen ? G32(optlen) : 0;
            int ret = getsockopt(fd, hlevel, hopt, GP(val), &slen);
            if (ret < 0) { set_err(errno); r = -1; }
            else { if (optlen) G32(optlen) = slen; r = 0; }
        }
    }
    char ta[64];
    snprintf(ta, sizeof ta, "fd=%d level=%d opt=%d", fd, level, opt);
    net_tracef("getsockopt", ta, r);
    HLE_RETURN((uint64_t)r);
}

HLE(_ZN2nn6socket10GetSockOptEiiiPvPj) { hle_getsockopt_impl(c); }

static void hle_setsockopt_impl(Ctx *c) {
    int fd = (int)ARG(0);
    int level = (int)ARG(1);
    int opt = (int)ARG(2);
    uint64_t val = ARG(3);
    uint32_t optlen = (uint32_t)ARG(4);
    int hlevel, hopt;
    long long r;
    if (map_sockopt(level, opt, &hlevel, &hopt) < 0) {
        hos_log("net: unknown setsockopt level=%#x opt=%#x", level, opt);
        r = 0;
    } else {
        int ret = setsockopt(fd, hlevel, hopt, GP(val), (socklen_t)optlen);
        if (ret < 0) { set_err(errno); r = -1; } else r = 0;
    }
    char ta[80];
    snprintf(ta, sizeof ta, "fd=%d level=%d opt=%d len=%u", fd, level, opt, optlen);
    net_tracef("setsockopt", ta, r);
    HLE_RETURN((uint64_t)r);
}

HLE(_ZN2nn6socket10SetSockOptEiNS0_5LevelENS0_6OptionEPKvj) { hle_setsockopt_impl(c); }
HLE(_ZN2nn6socket10SetSockOptEiiiPKvj) { hle_setsockopt_impl(c); }

static void hle_getsockname_impl(Ctx *c) {
    int fd = (int)ARG(0);
    uint64_t gaddr = ARG(1);
    uint64_t glen = ARG(2);
    struct sockaddr_in sin;
    socklen_t slen = sizeof(sin);
    int ret = getsockname(fd, (struct sockaddr *)&sin, &slen);
    long long r;
    if (ret < 0) { set_err(errno); r = -1; }
    else { write_guest_sockaddr(gaddr, glen, &sin); r = 0; }
    char ta[32];
    snprintf(ta, sizeof ta, "fd=%d", fd);
    net_tracef("getsockname", ta, r);
    HLE_RETURN((uint64_t)r);
}

HLE(_ZN2nn6socket11GetSockNameEiP8sockaddrPj) { hle_getsockname_impl(c); }
HLE(_ZN2nn6socket11GetSockNameEiPNS0_8SockAddrEPj) { hle_getsockname_impl(c); }

static void hle_getpeername_impl(Ctx *c) {
    int fd = (int)ARG(0);
    uint64_t gaddr = ARG(1);
    uint64_t glen = ARG(2);
    struct sockaddr_in sin;
    socklen_t slen = sizeof(sin);
    int ret = getpeername(fd, (struct sockaddr *)&sin, &slen);
    long long r;
    if (ret < 0) { set_err(errno); r = -1; }
    else { write_guest_sockaddr(gaddr, glen, &sin); r = 0; }
    char ta[32];
    snprintf(ta, sizeof ta, "fd=%d", fd);
    net_tracef("getpeername", ta, r);
    HLE_RETURN((uint64_t)r);
}

HLE(_ZN2nn6socket11GetPeerNameEiP8sockaddrPj) { hle_getpeername_impl(c); }

static void hle_fcntl_impl(Ctx *c) {
    int fd = (int)ARG(0);
    int cmd = (int)ARG(1);
    long long r;
    if (cmd == 3) {
        int flags = fcntl(fd, F_GETFL);
        if (flags < 0) { set_err(errno); r = -1; }
        else {
            int gflags = 0;
            if (flags & O_NONBLOCK) gflags |= 0x800;
            r = gflags;
        }
    } else if (cmd == 4) {
        int gflags = (int)ARG(2);
        int flags = 0;
        /* Guest libc uses 0x800 for O_NONBLOCK; nn::socket enum uses 0x4. */
        if ((gflags & 0x4) || (gflags & 0x800)) flags |= O_NONBLOCK;
        int ret = fcntl(fd, F_SETFL, flags);
        if (ret < 0) { set_err(errno); r = -1; } else r = 0;
    } else {
        hos_log("net: fcntl unknown cmd %d", cmd);
        r = 0;
    }
    char ta[80];
    snprintf(ta, sizeof ta, "fd=%d cmd=%d arg=%lld", fd, cmd, (long long)ARG(2));
    net_tracef("fcntl", ta, r);
    HLE_RETURN((uint64_t)r);
}

HLE(_ZN2nn6socket5FcntlEiNS0_12FcntlCommandEz) { hle_fcntl_impl(c); }

static void hle_getaddrinfo_impl(Ctx *c, const char *tag) {
    uint64_t node_ptr = ARG(0);
    uint64_t serv_ptr = ARG(1);
    uint64_t hints_ptr = ARG(2);
    uint64_t res_ptr = ARG(3);
    const char *node = node_ptr ? (const char *)GP(node_ptr) : NULL;
    const char *serv = serv_ptr ? (const char *)GP(serv_ptr) : NULL;
    hos_log("net: %s \"%s\" port \"%s\"", tag, node ? node : "(null)", serv ? serv : "(null)");
    int socktype = 1;
    if (hints_ptr) {
        int st = (int)G32(hints_ptr + 8);
        if (st) socktype = st;
    }
    uint16_t port = 0;
    if (serv) {
        int p = atoi(serv);
        if (p > 0) port = (uint16_t)p;
        else if (!strcmp(serv, "https")) port = 443;
        else if (!strcmp(serv, "http")) port = 80;
    }
    uint64_t ai = hos_sys_alloc(64, 16);
    uint64_t sa = ai + 48;
    G32(ai + 0) = 0;
    G32(ai + 4) = 2;
    G32(ai + 8) = socktype;
    G32(ai + 12) = 0;
    G32(ai + 16) = 16;
    G64(ai + 24) = 0;
    G64(ai + 32) = sa;
    G64(ai + 40) = 0;
    uint8_t *p = (uint8_t *)GP(sa);
    p[0] = 16;
    p[1] = 2;
    uint16_t port_be = htons(port);
    memcpy(p + 2, &port_be, 2);
    uint32_t addr_be = htonl(INADDR_LOOPBACK);
    memcpy(p + 4, &addr_be, 4);
    memset(p + 8, 0, 8);
    if (res_ptr) G64(res_ptr) = ai;
    char ta[256];
    snprintf(ta, sizeof ta, "node=%s serv=%s", node ? node : "(null)", serv ? serv : "(null)");
    net_tracef(tag, ta, 0);
    HLE_RETURN(0);
}

HLE(_ZN2nn6socket11GetAddrInfoEPKcS2_PK8addrinfoPPS3_) { hle_getaddrinfo_impl(c, "resolve"); }
HLE(_ZN2nn6socket11GetAddrInfoEPKcS2_PKNS0_8AddrInfoEPPS3_) { hle_getaddrinfo_impl(c, "resolve"); }

HLE(_ZN2nn6socket12FreeAddrInfoEP8addrinfo) { net_tracep("FreeAddrInfo", (unsigned long long)ARG(0), 0); HLE_RETURN(0); }
HLE(_ZN2nn6socket12FreeAddrInfoEPNS0_8AddrInfoE) { net_tracep("FreeAddrInfo", (unsigned long long)ARG(0), 0); HLE_RETURN(0); }
/* C-ABI wrappers the game also imports; same addrinfo layout as the C++ overloads above */
HLE(nnsocketGetAddrInfo) { hle_getaddrinfo_impl(c, "resolve"); }
/* (afterglow step 60) the Options variant the resolver worker uses: same (node, serv, hints, res) in x0-x3, options/count in x4/x5 ignored */
HLE(nnsocketGetAddrInfoWithOptions) { hle_getaddrinfo_impl(c, "resolve(opts)"); }
HLE(nnsocketFreeAddrInfo) { net_tracep("nnsocketFreeAddrInfo", (unsigned long long)ARG(0), 0); HLE_RETURN(0); }

/* (afterglow diag, step 59) the errno word the guest reads back, and from which thread */
static void log_lasterrno(const char *who, int v) {
    static int n;
    if (n++ < 40) hos_log("diag: %s -> %d thread %s", who, v, hos_thread_name(hos_self()));
}

HLE(_ZN2nn6socket12GetLastErrnoEv) { log_lasterrno("GetLastErrno", last_errno); HLE_RETURN((uint64_t)last_errno); }
HLE(_ZN2nn6socket12GetLastErrorEv) { log_lasterrno("GetLastError", last_errno); HLE_RETURN((uint64_t)last_errno); }

HLE(_ZN2nn6socket9InetHtonlEj) { HLE_RETURN((uint64_t)htonl((uint32_t)ARG(0))); }
HLE(_ZN2nn6socket9InetHtonsEt) { HLE_RETURN((uint64_t)htons((uint16_t)ARG(0))); }
HLE(_ZN2nn6socket9InetNtohlEj) { HLE_RETURN((uint64_t)ntohl((uint32_t)ARG(0))); }
HLE(_ZN2nn6socket9InetNtohsEt) { HLE_RETURN((uint64_t)ntohs((uint16_t)ARG(0))); }

HLE(_ZN2nn6socket8InetAtonEPKcPNS0_6InAddrE) {
    const char *cp = (const char *)GP(ARG(0));
    uint64_t pin = ARG(1);
    struct in_addr addr;
    int ret = inet_aton(cp, &addr);
    if (ret && pin) memcpy(GP(pin), &addr.s_addr, 4);
    HLE_RETURN((uint64_t)ret);
}

HLE(_ZN2nn6socket8InetNtoaENS0_6InAddrE) {
    static uint64_t sbuf = 0;
    if (!sbuf) sbuf = hos_sys_alloc(32, 8);
    struct in_addr addr;
    addr.s_addr = (uint32_t)ARG(0);
    const char *str = inet_ntoa(addr);
    strncpy((char *)GP(sbuf), str ? str : "", 32);
    HLE_RETURN(sbuf);
}

static void hle_inetntop_impl(Ctx *c) {
    int af = (int)ARG(0);
    uint64_t src = ARG(1);
    uint64_t dst = ARG(2);
    socklen_t size = (socklen_t)ARG(3);
    if (af == 2) {
        const char *ret = inet_ntop(AF_INET, GP(src), (char *)GP(dst), size);
        HLE_RETURN(ret ? dst : 0);
    }
    HLE_RETURN(0);
}

HLE(_ZN2nn6socket8InetNtopENS0_6FamilyEPKvPcj) { hle_inetntop_impl(c); }
HLE(_ZN2nn6socket8InetNtopEiPKvPcj) { hle_inetntop_impl(c); }

HLE(_ZN2nn6socket8InetPtonEiPKcPv) {
    int af = (int)ARG(0);
    const char *src = (const char *)GP(ARG(1));
    uint64_t dst = ARG(2);
    if (af == 2) {
        int ret = inet_pton(AF_INET, src, GP(dst));
        HLE_RETURN((uint64_t)ret);
    }
    HLE_RETURN(0);
}

static void hle_eventfd_impl(Ctx *c) {
    unsigned int initval = (unsigned int)ARG(0);
    int gflags = (int)ARG(1);
    int flags = 0;
    if (gflags & 0x01) flags |= EFD_SEMAPHORE;
    if (gflags & 0x04) flags |= EFD_NONBLOCK;
    int fd = eventfd(initval, flags);
    long long r;
    if (fd < 0) { set_err(errno); r = -1; } else r = fd;
    char ta[64];
    snprintf(ta, sizeof ta, "initval=%u flags=%d", initval, gflags);
    net_tracef("EventFd", ta, r);
    HLE_RETURN((uint64_t)r);
}

HLE(_ZN2nn6socket7EventFdEmNS0_12EventFdFlagsE) { hle_eventfd_impl(c); }

static void hle_eventfdread_impl(Ctx *c) {
    int fd = (int)ARG(0);
    uint64_t pval = ARG(1);
    uint64_t val = 0;
    ssize_t ret = eventfd_read(fd, &val);
    long long r;
    if (ret < 0) { set_err(errno); r = -1; }
    else { if (pval) G64(pval) = val; r = 0; }
    char ta[32];
    snprintf(ta, sizeof ta, "fd=%d", fd);
    net_tracef("EventFdRead", ta, r);
    HLE_RETURN((uint64_t)r);
}

HLE(_ZN2nn6socket11EventFdReadEiPm) { hle_eventfdread_impl(c); }

static void hle_eventfdwrite_impl(Ctx *c) {
    int fd = (int)ARG(0);
    uint64_t val = ARG(1);
    int ret = eventfd_write(fd, val);
    long long r;
    if (ret < 0) { set_err(errno); r = -1; } else r = 0;
    char ta[64];
    snprintf(ta, sizeof ta, "fd=%d val=%llu", fd, (unsigned long long)val);
    net_tracef("EventFdWrite", ta, r);
    HLE_RETURN((uint64_t)r);
}

HLE(_ZN2nn6socket12EventFdWriteEim) { hle_eventfdwrite_impl(c); }

/* Guest __errno_location at 0x8a79f220 from image.json / PLT import. */
static void set_guest_errno(Ctx *c, int err) {
    A64Fn fn = a64_lookup(0x8a79f220);
    if (!fn) return;
    uint64_t saved_x[32];
    memcpy(saved_x, c->x, sizeof(saved_x));
    uint8_t n = c->n, z = c->z, cv = c->c, v = c->v;
    fn(c);
    uint64_t ptr = c->x[0];
    if (ptr) G32(ptr) = (uint32_t)err;
    memcpy(c->x, saved_x, sizeof(saved_x));
    c->n = n; c->z = z; c->c = cv; c->v = v;
}

static void check_guest_errno(Ctx *c) {
    if ((int64_t)c->x[0] == -1 || (int32_t)c->x[0] == -1) {
        /* the guest libc is newlib: its errno numbers are the Linux ones, so the host value goes out as is */
        set_guest_errno(c, last_host_errno);
        if (net_trace_on()) hos_log("net: trace gerrno=%d", last_host_errno);
    }
}

/* C-ABI wrappers: the nnsocket* imports take the same arguments as the int overloads above */
HLE(nnsocketSocket) { hle_socket_impl(c); check_guest_errno(c); }
HLE(nnsocketConnect) { hle_connect_impl(c); check_guest_errno(c); }
HLE(nnsocketBind) { hle_bind_impl(c); check_guest_errno(c); }
HLE(nnsocketListen) { hle_listen_impl(c); check_guest_errno(c); }
HLE(nnsocketAccept) { hle_accept_impl(c); check_guest_errno(c); }
HLE(nnsocketSend) { hle_send_impl(c); check_guest_errno(c); }
HLE(nnsocketSendTo) { hle_sendto_impl(c); check_guest_errno(c); }
HLE(nnsocketSendMsg) { hle_sendmsg_impl(c); check_guest_errno(c); }
HLE(nnsocketRecv) { hle_recv_impl(c); check_guest_errno(c); }
HLE(nnsocketRecvFrom) { hle_recvfrom_impl(c); check_guest_errno(c); }
HLE(nnsocketRecvMsg) { hle_recvmsg_impl(c); check_guest_errno(c); }
HLE(nnsocketRead) { hle_read_impl(c); check_guest_errno(c); }
HLE(nnsocketWrite) { hle_write_impl(c); check_guest_errno(c); }
HLE(nnsocketClose) { hle_close_impl(c); check_guest_errno(c); }
HLE(nnsocketShutdown) { hle_shutdown_impl(c); check_guest_errno(c); }
HLE(nnsocketPoll) { hle_poll_impl(c); check_guest_errno(c); }
HLE(nnsocketSelect) { hle_select_impl(c); check_guest_errno(c); }
HLE(nnsocketGetSockOpt) { hle_getsockopt_impl(c); check_guest_errno(c); }
HLE(nnsocketSetSockOpt) { hle_setsockopt_impl(c); check_guest_errno(c); }
HLE(nnsocketGetSockName) { hle_getsockname_impl(c); check_guest_errno(c); }
HLE(nnsocketGetPeerName) { hle_getpeername_impl(c); check_guest_errno(c); }
HLE(nnsocketFcntl) { hle_fcntl_impl(c); check_guest_errno(c); }
HLE(nnsocketEventFd) { hle_eventfd_impl(c); check_guest_errno(c); }
HLE(nnsocketEventFdRead) { hle_eventfdread_impl(c); check_guest_errno(c); }
HLE(nnsocketEventFdWrite) { hle_eventfdwrite_impl(c); check_guest_errno(c); }
HLE(nnsocketGetLastErrno) { log_lasterrno("nnsocketGetLastErrno", last_errno); HLE_RETURN((uint64_t)last_errno); }
HLE(nnsocketSetLastErrno) { last_errno = (int)ARG(0); HLE_RETURN(0); }
/* nn::socket::Fcntl(int, int, long long): the same command set as the enum overload */
HLE(_ZN2nn6socket5FcntlEiiz) { hle_fcntl_impl(c); }

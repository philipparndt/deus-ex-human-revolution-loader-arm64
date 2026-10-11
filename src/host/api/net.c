// BSD sockets, resolver and interface enumeration.
//
// struct sockaddr*, fd_set, struct linger and socklen_t are layout-identical
// in i386 and x86_64. Structures with pointers or timevals are converted:
//   struct hostent (i386, 20 bytes): h_name@0 h_aliases@4 h_addrtype@8 h_length@12 h_addr_list@16
//   struct ifaddrs (i386, 28 bytes): next@0 name@4 flags@8 addr@12 netmask@16 dstaddr@20 data@24
#include "common.h"
#include "libc.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

THUNK(socket) { RET_ERRNO(socket(ARGI(0), ARGI(1), ARGI(2))); }
THUNK(socketpair) { RET_ERRNO(socketpair(ARGI(0), ARGI(1), ARGI(2), ARGP(3))); }
THUNK_ALIAS(socketpair_unix2003, "_socketpair$UNIX2003", socketpair)
THUNK(bind) { RET_ERRNO(bind(ARGI(0), ARGP(1), ARG(2))); }
THUNK_ALIAS(bind_unix2003, "_bind$UNIX2003", bind)
THUNK(connect) { RET_ERRNO(connect(ARGI(0), ARGP(1), ARG(2))); }
THUNK_ALIAS(connect_unix2003, "_connect$UNIX2003", connect)
THUNK_ALIAS(connect_nocancel, "_connect$NOCANCEL$UNIX2003", connect)
THUNK(listen) { RET_ERRNO(listen(ARGI(0), ARGI(1))); }
THUNK_ALIAS(listen_unix2003, "_listen$UNIX2003", listen)
THUNK(accept) { RET_ERRNO(accept(ARGI(0), ARGP(1), ARGP(2))); }
THUNK_ALIAS(accept_unix2003, "_accept$UNIX2003", accept)
THUNK_ALIAS(accept_nocancel, "_accept$NOCANCEL$UNIX2003", accept)
THUNK(shutdown) { RET_ERRNO(shutdown(ARGI(0), ARGI(1))); }
THUNK(getsockname) { RET_ERRNO(getsockname(ARGI(0), ARGP(1), ARGP(2))); }
THUNK_ALIAS(getsockname_unix2003, "_getsockname$UNIX2003", getsockname)
THUNK(getpeername) { RET_ERRNO(getpeername(ARGI(0), ARGP(1), ARGP(2))); }
THUNK_ALIAS(getpeername_unix2003, "_getpeername$UNIX2003", getpeername)

THUNK(send) { RET_ERRNO(send(ARGI(0), ARGP(1), ARG(2), ARGI(3))); }
THUNK_ALIAS(send_unix2003, "_send$UNIX2003", send)
THUNK_ALIAS(send_nocancel, "_send$NOCANCEL$UNIX2003", send)
THUNK(recv) { RET_ERRNO(recv(ARGI(0), ARGP(1), ARG(2), ARGI(3))); }
THUNK_ALIAS(recv_unix2003, "_recv$UNIX2003", recv)
THUNK_ALIAS(recv_nocancel, "_recv$NOCANCEL$UNIX2003", recv)
THUNK(sendto) { RET_ERRNO(sendto(ARGI(0), ARGP(1), ARG(2), ARGI(3), ARGP(4), ARG(5))); }
THUNK_ALIAS(sendto_unix2003, "_sendto$UNIX2003", sendto)
THUNK_ALIAS(sendto_nocancel, "_sendto$NOCANCEL$UNIX2003", sendto)
THUNK(recvfrom) { RET_ERRNO(recvfrom(ARGI(0), ARGP(1), ARG(2), ARGI(3), ARGP(4), ARGP(5))); }
THUNK_ALIAS(recvfrom_unix2003, "_recvfrom$UNIX2003", recvfrom)
THUNK_ALIAS(recvfrom_nocancel, "_recvfrom$NOCANCEL$UNIX2003", recvfrom)

// select(nfds, fd_set *r, fd_set *w, fd_set *e, struct timeval *timeout)
THUNK(select) {
    struct timeval tv;
    if (ARG(4)) get_gtimeval(ARG(4), &tv);
    int rc = select(ARGI(0), ARGP(1), ARGP(2), ARGP(3), ARG(4) ? &tv : NULL);
    if (rc == -1) set_gerrno(errno);
    RET(rc);
}
THUNK_ALIAS(select_unix2003, "_select$UNIX2003", select)
THUNK_ALIAS(select_nocancel, "_select$NOCANCEL$UNIX2003", select)
THUNK_ALIAS(select_extsn, "_select$DARWIN_EXTSN", select)
THUNK_ALIAS(select_extsn_nocancel, "_select$DARWIN_EXTSN$NOCANCEL", select)

static bool is_timeval_opt(int level, int opt) {
    return level == SOL_SOCKET && (opt == SO_RCVTIMEO || opt == SO_SNDTIMEO);
}
// setsockopt(fd, level, opt, const void *val, socklen_t len)
THUNK(setsockopt) {
    int level = ARGI(1), opt = ARGI(2);
    int rc;
    if (is_timeval_opt(level, opt) && ARG(4) >= 8) {
        struct timeval tv;
        get_gtimeval(ARG(3), &tv);
        rc = setsockopt(ARGI(0), level, opt, &tv, sizeof tv);
    } else {
        rc = setsockopt(ARGI(0), level, opt, ARGP(3), ARG(4));
    }
    if (rc == -1) set_gerrno(errno);
    RET(rc);
}
// getsockopt(fd, level, opt, void *val, socklen_t *len)
THUNK(getsockopt) {
    int level = ARGI(1), opt = ARGI(2);
    int rc;
    if (is_timeval_opt(level, opt)) {
        struct timeval tv;
        socklen_t l = sizeof tv;
        rc = getsockopt(ARGI(0), level, opt, &tv, &l);
        if (rc == 0) {
            if (gread32(ARG(4)) < 8) {
                set_gerrno(EINVAL);
                RET(-1);
                return;
            }
            put_gtimeval(ARG(3), &tv);
            gwrite32(ARG(4), 8);
        }
    } else {
        rc = getsockopt(ARGI(0), level, opt, ARGP(3), ARGP(4));
    }
    if (rc == -1) set_gerrno(errno);
    RET(rc);
}

THUNK(gethostname) { RET_ERRNO(gethostname(ARGP(0), ARG(1))); }

// --- resolver ---------------------------------------------------------------------------------
static _Thread_local gaddr_t tls_hostent;

static uint32_t count_list(char **l) {
    uint32_t n = 0;
    while (l && l[n]) n++;
    return n;
}

// Build a guest hostent (and everything it points to) in one per-thread block.
static gaddr_t put_hostent(const struct hostent *h) {
    if (!h) return 0;
    uint32_t nal = count_list(h->h_aliases), nad = count_list(h->h_addr_list);
    uint32_t alen = (uint32_t)h->h_length;
    uint32_t size = 20 + 4 * (nal + 1) + 4 * (nad + 1) + alen * nad + (uint32_t)strlen(h->h_name) + 1;
    for (uint32_t i = 0; i < nal; i++) size += (uint32_t)strlen(h->h_aliases[i]) + 1;
    gheap_free(tls_hostent);
    gaddr_t g = tls_hostent = gheap_calloc(1, size);
    gaddr_t aliases = g + 20, addrs = aliases + 4 * (nal + 1), data = addrs + 4 * (nad + 1);
    for (uint32_t i = 0; i < nad; i++, data += alen) {
        memcpy(g2h(data), h->h_addr_list[i], alen);
        gwrite32(addrs + 4 * i, data);
    }
    gwrite32(g, data);
    strcpy(g2h(data), h->h_name);
    data += (uint32_t)strlen(h->h_name) + 1;
    for (uint32_t i = 0; i < nal; i++) {
        strcpy(g2h(data), h->h_aliases[i]);
        gwrite32(aliases + 4 * i, data);
        data += (uint32_t)strlen(h->h_aliases[i]) + 1;
    }
    gwrite32(g + 4, aliases);
    gwrite32(g + 8, (uint32_t)h->h_addrtype);
    gwrite32(g + 12, alen);
    gwrite32(g + 16, addrs);
    return g;
}

THUNK(gethostbyname) { RET(put_hostent(gethostbyname(ARGS(0)))); }
THUNK(gethostbyname2) { RET(put_hostent(gethostbyname2(ARGS(0), ARGI(1)))); }
THUNK(gethostbyaddr) { RET(put_hostent(gethostbyaddr(ARGP(0), ARG(1), ARGI(2)))); }

THUNK(inet_addr) { RET(inet_addr(ARGS(0))); }
THUNK(inet_aton) { RET(inet_aton(ARGS(0), ARGP(1))); }
// inet_ntoa(struct in_addr) - the 4-byte struct is passed by value.
THUNK(inet_ntoa) {
    struct in_addr a = {ARG(0)};
    RET(guest_static_string(4, inet_ntoa(a)));
}
THUNK(inet_ntop) {
    const char *s = inet_ntop(ARGI(0), ARGP(1), ARGP(2), ARG(3));
    if (!s) set_gerrno(errno);
    RET(s ? ARG(2) : 0);
}
THUNK(inet_pton) { RET_ERRNO(inet_pton(ARGI(0), ARGS(1), ARGP(2))); }
// getnameinfo(sa, salen, host, hostlen, serv, servlen, flags): all sizes are socklen_t.
THUNK(getnameinfo) { RET(getnameinfo(ARGP(0), ARG(1), ARGP(2), ARG(3), ARGP(4), ARG(5), ARGI(6))); }
THUNK(gai_strerror) { RET(gintern(gai_strerror(ARGI(0)))); }
THUNK(htonl) { RET(htonl(ARG(0))); }
THUNK(htons) { RET(htons((uint16_t)ARG(0))); }
THUNK(ntohl) { RET(ntohl(ARG(0))); }
THUNK(ntohs) { RET(ntohs((uint16_t)ARG(0))); }

// --- getifaddrs: the whole list lives in one guest block (freed via the head) -------------------
static uint32_t sa_size(const struct sockaddr *sa) {
    if (!sa) return 0;
    uint32_t n = sa->sa_len ? sa->sa_len : sizeof(struct sockaddr);
    return (n + 3) & ~3u;
}

THUNK(getifaddrs) {
    struct ifaddrs *list;
    if (getifaddrs(&list)) {
        set_gerrno(errno);
        RET(-1);
        return;
    }
    uint32_t size = 0;
    for (struct ifaddrs *i = list; i; i = i->ifa_next) {
        size += 28 + (((uint32_t)strlen(i->ifa_name) + 4) & ~3u);
        size += sa_size(i->ifa_addr) + sa_size(i->ifa_netmask) + sa_size(i->ifa_dstaddr);
        if (i->ifa_data && i->ifa_addr && i->ifa_addr->sa_family == AF_LINK) size += sizeof(struct if_data);
    }
    gaddr_t block = gheap_calloc(1, size ? size : 4), p = block, prev = 0;
    if (!block) {
        freeifaddrs(list);
        set_gerrno(ENOMEM);
        RET(-1);
        return;
    }
    for (struct ifaddrs *i = list; i; i = i->ifa_next) {
        gaddr_t node = p;
        p += 28;
        if (prev) gwrite32(prev, node);
        prev = node;
        gwrite32(node + 4, p);
        strcpy(g2h(p), i->ifa_name);
        p += ((uint32_t)strlen(i->ifa_name) + 4) & ~3u;
        gwrite32(node + 8, i->ifa_flags);
        const struct sockaddr *sas[3] = {i->ifa_addr, i->ifa_netmask, i->ifa_dstaddr};
        for (int k = 0; k < 3; k++) {
            if (!sas[k]) continue;
            uint32_t n = sas[k]->sa_len ? sas[k]->sa_len : sizeof(struct sockaddr);
            memcpy(g2h(p), sas[k], n);
            gwrite32(node + 12 + 4 * (uint32_t)k, p);
            p += sa_size(sas[k]);
        }
        // AF_LINK entries carry a struct if_data (identical layout on both sides).
        if (i->ifa_data && i->ifa_addr && i->ifa_addr->sa_family == AF_LINK) {
            memcpy(g2h(p), i->ifa_data, sizeof(struct if_data));
            gwrite32(node + 24, p);
            p += sizeof(struct if_data);
        }
    }
    freeifaddrs(list);
    gwrite32(ARG(0), list ? block : 0);
    if (!list) gheap_free(block);
    RET(0);
}
THUNK(freeifaddrs) { gheap_free(ARG(0)); }
THUNK(if_nametoindex) { RET(if_nametoindex(ARGS(0))); }

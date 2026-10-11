// sockets over loopback, select with timeout, socket options, resolver, getifaddrs.
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stddef.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include "libc_check.h"

_Static_assert(sizeof(struct hostent) == 20 && sizeof(struct ifaddrs) == 28, "layouts");

int main(void) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(s >= 0, "socket");
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_len = sizeof sa;
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(s, (struct sockaddr *)&sa, sizeof sa) == 0, "bind");
    socklen_t sl = sizeof sa;
    CHECK(getsockname(s, (struct sockaddr *)&sa, &sl) == 0 && sa.sin_port != 0 && sl == sizeof sa, "getsockname");

    struct timeval to = {1, 250000}, got = {0, 0};
    CHECK(setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof to) == 0, "setsockopt timeval");
    socklen_t gl = sizeof got;
    CHECK(getsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &got, &gl) == 0 && gl == 8 && got.tv_sec == 1 && got.tv_usec == 250000,
          "getsockopt timeval %ld.%d", (long)got.tv_sec, got.tv_usec);
    int one = 1, v = 0;
    socklen_t il = sizeof v;
    CHECK(setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) == 0 &&
              getsockopt(s, SOL_SOCKET, SO_REUSEADDR, &v, &il) == 0 && v != 0,
          "int sockopt");

    // select timing out
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(s, &rf);
    struct timeval st = {0, 10000};
    CHECK(select(s + 1, &rf, NULL, NULL, &st) == 0, "select timeout");

    int c = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(sendto(c, "ping", 4, 0, (struct sockaddr *)&sa, sizeof sa) == 4, "sendto");
    FD_ZERO(&rf);
    FD_SET(s, &rf);
    st.tv_sec = 2;
    st.tv_usec = 0;
    CHECK(select(s + 1, &rf, NULL, NULL, &st) == 1 && FD_ISSET(s, &rf), "select ready");
    char buf[16] = {0};
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    CHECK(recvfrom(s, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl) == 4 && memcmp(buf, "ping", 4) == 0 &&
              from.sin_addr.s_addr == htonl(INADDR_LOOPBACK),
          "recvfrom");
    CHECK(connect(c, (struct sockaddr *)&sa, sizeof sa) == 0 && send(c, "pong", 4, 0) == 4 &&
              recv(s, buf, sizeof buf, 0) == 4 && memcmp(buf, "pong", 4) == 0,
          "connect/send/recv");
    close(c);
    close(s);

    // TCP listen/accept
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    sa.sin_port = 0;
    CHECK(bind(ls, (struct sockaddr *)&sa, sizeof sa) == 0 && listen(ls, 1) == 0, "listen");
    sl = sizeof sa;
    getsockname(ls, (struct sockaddr *)&sa, &sl);
    int cs = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(connect(cs, (struct sockaddr *)&sa, sizeof sa) == 0, "tcp connect");
    struct sockaddr_in peer;
    socklen_t pl = sizeof peer;
    int as = accept(ls, (struct sockaddr *)&peer, &pl);
    CHECK(as >= 0 && pl == sizeof peer && peer.sin_family == AF_INET, "accept");
    CHECK(send(cs, "tcp!", 4, 0) == 4 && recv(as, buf, 4, MSG_WAITALL) == 4 && memcmp(buf, "tcp!", 4) == 0, "tcp data");
    close(as);
    close(cs);
    close(ls);

    char host[256] = {0};
    CHECK(gethostname(host, sizeof host) == 0 && host[0], "gethostname");
    CHECK(inet_addr("10.1.2.3") == htonl(0x0a010203), "inet_addr");
    struct in_addr ia = {htonl(0xc0a80001)};
    CHECK(strcmp(inet_ntoa(ia), "192.168.0.1") == 0, "inet_ntoa");
    struct hostent *he = gethostbyname("localhost");
    CHECK(he && he->h_addrtype == AF_INET && he->h_length == 4 && he->h_addr_list[0] &&
              *(uint32_t *)he->h_addr_list[0] == htonl(INADDR_LOOPBACK) && he->h_name && he->h_aliases,
          "gethostbyname");
    char hb[NI_MAXHOST], sb[NI_MAXSERV];
    sa.sin_port = htons(80);
    CHECK(getnameinfo((struct sockaddr *)&sa, sizeof sa, hb, sizeof hb, sb, sizeof sb, NI_NUMERICHOST | NI_NUMERICSERV) == 0 &&
              strcmp(hb, "127.0.0.1") == 0 && strcmp(sb, "80") == 0,
          "getnameinfo %s %s", hb, sb);

    struct ifaddrs *ifa = NULL;
    CHECK(getifaddrs(&ifa) == 0 && ifa, "getifaddrs");
    int lo = 0, n = 0;
    for (struct ifaddrs *i = ifa; i; i = i->ifa_next, n++) {
        if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && strncmp(i->ifa_name, "lo", 2) == 0) {
            lo |= ((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr == htonl(INADDR_LOOPBACK) &&
                 (i->ifa_flags & IFF_LOOPBACK) && i->ifa_netmask;
        }
    }
    CHECK(lo && n > 1, "getifaddrs loopback (%d entries)", n);
    freeifaddrs(ifa);
    DONE();
}

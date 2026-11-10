/* rdr_net.c - UDP sockets for online play (Winsock or BSD). */
#include "rdr_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef int socklen_t;
static void net_init(void)
{
    static int done;
    if (!done) {
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
        done = 1;
    }
}
#define CLOSESOCK closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
static void net_init(void) {}
#define CLOSESOCK close
#endif

int rdr_sock_open(uint16_t port)
{
    struct sockaddr_in a;
    int s, big = 4 << 20;
    net_init();
    s = (int)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0)
        return -1;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) {
        CLOSESOCK(s);
        return -1;
    }
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&big, sizeof big);
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char *)&big, sizeof big);
#if defined(_WIN32)
    {
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
    }
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
    return s;
}

static void to_sa(const RdrAddr *a, struct sockaddr_in *sa)
{
    memset(sa, 0, sizeof *sa);
    sa->sin_family = AF_INET;
    sa->sin_port = htons(a->port);
    memcpy(&sa->sin_addr, a->ip, 4);
}

int rdr_sock_send(int s, const RdrAddr *to, const void *p, int n)
{
    struct sockaddr_in sa;
    to_sa(to, &sa);
    return (int)sendto(s, (const char *)p, n, 0, (struct sockaddr *)&sa, sizeof sa);
}

int rdr_sock_recv(int s, RdrAddr *from, void *p, int max)
{
    struct sockaddr_in sa;
    socklen_t l = sizeof sa;
    int n = (int)recvfrom(s, (char *)p, max, 0, (struct sockaddr *)&sa, &l);
    if (n > 0 && from) {
        memcpy(from->ip, &sa.sin_addr, 4);
        from->port = ntohs(sa.sin_port);
    }
    return n;
}

void rdr_sock_close(int s)
{
    if (s >= 0)
        CLOSESOCK(s);
}

int rdr_addr_parse(const char *text, RdrAddr *out)
{
    char host[256];
    const char *colon = strrchr(text, ':');
    struct addrinfo hints, *res = NULL;
    size_t n = colon ? (size_t)(colon - text) : strlen(text);
    net_init();
    if (n >= sizeof host)
        return -1;
    memcpy(host, text, n);
    host[n] = 0;
    out->port = colon ? (uint16_t)atoi(colon + 1) : RDR_NET_PORT;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res)
        return -1;
    memcpy(out->ip, &((struct sockaddr_in *)res->ai_addr)->sin_addr, 4);
    freeaddrinfo(res);
    return 0;
}

int rdr_addr_same(const RdrAddr *a, const RdrAddr *b)
{
    return !memcmp(a->ip, b->ip, 4) && a->port == b->port;
}

void rdr_net_sleep_ms(int ms)
{
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

uint64_t rdr_net_now_ms(void)
{
#if defined(_WIN32)
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

/* This machine's IPv4 addresses (not loopback): the one the default route
 * leaves from first (a UDP socket "connected" to a public address sends
 * nothing, but learns its own address), then the rest of the host name's
 * (LAN, VPN such as Tailscale's 100.x). */
int rdr_net_local_ips(uint8_t out[][4], int max)
{
    int n = 0, s;
    struct sockaddr_in sa;
    socklen_t l = sizeof sa;
    char host[256];
    struct addrinfo hints, *res = NULL, *r;
    net_init();
    s = (int)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s >= 0) {
        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_port = htons(53);
        sa.sin_addr.s_addr = htonl(0x08080808u);
        if (connect(s, (struct sockaddr *)&sa, sizeof sa) == 0
            && getsockname(s, (struct sockaddr *)&sa, &l) == 0 && sa.sin_addr.s_addr != 0
            && ((const uint8_t *)&sa.sin_addr)[0] != 127 && n < max)
            memcpy(out[n++], &sa.sin_addr, 4);
        CLOSESOCK(s);
    }
    if (gethostname(host, sizeof host) == 0) {
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;
        if (getaddrinfo(host, NULL, &hints, &res) == 0) {
            for (r = res; r && n < max; r = r->ai_next) {
                const uint8_t *ip = (const uint8_t *)&((struct sockaddr_in *)r->ai_addr)->sin_addr;
                int k, dup = 0;
                if (ip[0] == 127 || ip[0] == 0 || (ip[0] == 169 && ip[1] == 254))
                    continue;
                for (k = 0; k < n; k++)
                    dup |= !memcmp(out[k], ip, 4);
                if (!dup)
                    memcpy(out[n++], ip, 4);
            }
            freeaddrinfo(res);
        }
    }
    return n;
}

/* The address the internet sees this socket at (a STUN binding request,
 * RFC 5389, to a public server); 0 when found. The answer is read from the
 * socket within timeout_ms: call it before anything else uses the socket. */
int rdr_net_stun(int sock, const char *server, RdrAddr *out, int timeout_ms)
{
    RdrAddr srv;
    uint8_t req[20], buf[512];
    uint64_t t0;
    int k;
    if (rdr_addr_parse(server, &srv) != 0)
        return -1;
    memset(req, 0, sizeof req);
    req[1] = 0x01;                              /* Binding Request */
    req[4] = 0x21; req[5] = 0x12; req[6] = 0xA4; req[7] = 0x42;   /* magic cookie */
    for (k = 8; k < 20; k++)
        req[k] = (uint8_t)(rand() & 0xFF);
    t0 = rdr_net_now_ms();
    rdr_sock_send(sock, &srv, req, 20);
    while (rdr_net_now_ms() - t0 < (uint64_t)timeout_ms) {
        RdrAddr from;
        int n = rdr_sock_recv(sock, &from, buf, sizeof buf), p;
        if (n <= 0) {
            rdr_net_sleep_ms(10);
            if (rdr_net_now_ms() - t0 > (uint64_t)timeout_ms / 2)
                rdr_sock_send(sock, &srv, req, 20);   /* once more */
            continue;
        }
        if (n < 20 || buf[0] != 0x01 || buf[1] != 0x01 || memcmp(buf + 8, req + 8, 12))
            continue;
        for (p = 20; p + 4 <= n;) {
            int type = buf[p] << 8 | buf[p + 1], len = buf[p + 2] << 8 | buf[p + 3];
            const uint8_t *v = buf + p + 4;
            if (p + 4 + len > n)
                break;
            if ((type == 0x0020 || type == 0x0001) && len >= 8 && v[1] == 0x01) {
                uint16_t port = (uint16_t)(v[2] << 8 | v[3]);
                uint8_t ip[4];
                memcpy(ip, v + 4, 4);
                if (type == 0x0020) {           /* XOR-MAPPED-ADDRESS */
                    port ^= 0x2112;
                    ip[0] ^= 0x21; ip[1] ^= 0x12; ip[2] ^= 0xA4; ip[3] ^= 0x42;
                }
                memcpy(out->ip, ip, 4);
                out->port = port;
                return 0;
            }
            p += 4 + ((len + 3) & ~3);
        }
    }
    return -1;
}

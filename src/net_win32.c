/* net_win32.c - Winsock2 implementation of the net_sys_* primitives (net.h).
 *
 * Compiled ONLY on Windows (the XP / Pentium-M ship target); the BSD-socket twin
 * is net_linux.c. Exactly one of the pair compiles per target. XP-SAFE: uses
 * inet_addr (XP's Winsock lacks inet_pton, which arrived with Vista) and the
 * legacy WSAStartup(2,2) path. Non-blocking via ioctlsocket(FIONBIO). Return
 * codes follow net.h's normalised convention so net.c carries no #ifdefs.
 *
 * Links against ws2_32 (see the Makefile WIN_LIBS).
 */
#ifdef _WIN32

#include "net.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

static void set_nonblock(SOCKET s)
{
    u_long mode = 1;
    (void)ioctlsocket(s, FIONBIO, &mode);
}

static void set_nodelay(SOCKET s)
{
    BOOL on = TRUE;
    (void)setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof on);
}

int net_sys_init(void)
{
    WSADATA wsa;
    return (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) ? 0 : -1;
}

void net_sys_cleanup(void) { WSACleanup(); }

net_sock net_sys_listen(unsigned short port)
{
    SOCKET s;
    BOOL on = TRUE;
    struct sockaddr_in addr;

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return NET_SOCK_INVALID;
    (void)setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof on);

    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);

    if (bind(s, (struct sockaddr *)&addr, sizeof addr) == SOCKET_ERROR) { closesocket(s); return NET_SOCK_INVALID; }
    if (listen(s, 8) == SOCKET_ERROR)                                   { closesocket(s); return NET_SOCK_INVALID; }
    set_nonblock(s);
    return (net_sock)s;
}

net_sock net_sys_accept(net_sock listener)
{
    SOCKET s = accept((SOCKET)listener, NULL, NULL);
    if (s == INVALID_SOCKET) return NET_SOCK_INVALID;   /* WSAEWOULDBLOCK -> none */
    set_nonblock(s);
    set_nodelay(s);
    return (net_sock)s;
}

net_sock net_sys_connect(const char *ip, unsigned short port)
{
    SOCKET s;
    int rc;
    struct sockaddr_in addr;

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return NET_SOCK_INVALID;
    set_nonblock(s);
    set_nodelay(s);

    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip);      /* IPv4 dotted-quad (XP-safe) */
    if (addr.sin_addr.s_addr == INADDR_NONE) { closesocket(s); return NET_SOCK_INVALID; }

    rc = connect(s, (struct sockaddr *)&addr, sizeof addr);
    if (rc == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) { closesocket(s); return NET_SOCK_INVALID; }
    return (net_sock)s;
}

int net_sys_connect_done(net_sock sk)
{
    SOCKET s = (SOCKET)sk;
    fd_set wf, ef;
    struct timeval tv;
    int rc;

    FD_ZERO(&wf); FD_SET(s, &wf);
    FD_ZERO(&ef); FD_SET(s, &ef);
    tv.tv_sec = 0; tv.tv_usec = 0;             /* poll, never block */
    rc = select(0, NULL, &wf, &ef, &tv);       /* nfds ignored on Winsock */
    if (rc == SOCKET_ERROR) return -1;
    if (FD_ISSET(s, &ef)) return -1;           /* connection failed */
    if (FD_ISSET(s, &wf)) return 1;            /* writable -> connected */
    return 0;                                  /* still pending */
}

long net_sys_send(net_sock s, const void *buf, long len)
{
    int n = send((SOCKET)s, (const char *)buf, (int)len, 0);
    if (n != SOCKET_ERROR) return (long)n;
    return (WSAGetLastError() == WSAEWOULDBLOCK) ? 0 : -1;
}

long net_sys_recv(net_sock s, void *buf, long len)
{
    int n = recv((SOCKET)s, (char *)buf, (int)len, 0);
    if (n > 0)  return (long)n;
    if (n == 0) return -1;                     /* orderly close -> drop */
    return (WSAGetLastError() == WSAEWOULDBLOCK) ? 0 : -1;
}

void net_sys_close(net_sock s)
{
    if (s != NET_SOCK_INVALID) closesocket((SOCKET)s);
}

void net_sys_sleep_ms(int ms)
{
    if (ms < 0) ms = 0;
    Sleep((DWORD)ms);
}

double net_sys_now_ms(void)
{
    /* GetTickCount: ms since boot, 32-bit (wraps ~49.7 days). Fine for the small
     * RELATIVE idle windows we measure; XP-safe (GetTickCount64 is Vista+). */
    return (double)GetTickCount();
}

#endif /* _WIN32 */

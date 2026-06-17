/* net_linux.c - BSD-socket implementation of the net_sys_* primitives (net.h).
 *
 * Compiled ONLY on non-Windows (the Linux dev host). The Win32/Winsock twin is
 * net_win32.c; exactly one of the pair compiles per target, the same split as
 * platform_linux.c / platform_win32.c. All sockets are non-blocking; the return
 * codes follow net.h's normalised convention so net.c carries no #ifdefs.
 */
#define _POSIX_C_SOURCE 200112L   /* inet_addr, nanosleep, TCP_NODELAY visibility */

#ifndef _WIN32

#include "net.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>   /* TCP_NODELAY */
#include <arpa/inet.h>     /* inet_addr, htons */
#include <sys/select.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <time.h>

/* Flip a fd to non-blocking; returns 0 on success. */
static int set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* Small-message latency: disable Nagle so a 16-byte edit is not coalesced. */
static void set_nodelay(int fd)
{
    int on = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
}

int net_sys_init(void)
{
    /* send() to a peer that has closed would raise SIGPIPE and kill the process;
     * ignore it process-wide (we also pass MSG_NOSIGNAL below as a belt). */
    signal(SIGPIPE, SIG_IGN);
    return 0;
}

void net_sys_cleanup(void) { /* nothing to undo on POSIX */ }

net_sock net_sys_listen(unsigned short port)
{
    int fd, on = 1;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return NET_SOCK_INVALID;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);

    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) { close(fd); return NET_SOCK_INVALID; }
    if (listen(fd, 8) < 0)                                   { close(fd); return NET_SOCK_INVALID; }
    if (set_nonblock(fd) < 0)                                { close(fd); return NET_SOCK_INVALID; }
    return (net_sock)fd;
}

net_sock net_sys_accept(net_sock listener)
{
    int fd = accept((int)listener, NULL, NULL);
    if (fd < 0) return NET_SOCK_INVALID;    /* EAGAIN/EWOULDBLOCK -> none pending */
    if (set_nonblock(fd) < 0) { close(fd); return NET_SOCK_INVALID; }
    set_nodelay(fd);
    return (net_sock)fd;
}

net_sock net_sys_connect(const char *ip, unsigned short port)
{
    int fd, rc;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return NET_SOCK_INVALID;
    if (set_nonblock(fd) < 0) { close(fd); return NET_SOCK_INVALID; }
    set_nodelay(fd);

    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip);     /* IPv4 dotted-quad (XP-safe path)  */
    if (addr.sin_addr.s_addr == INADDR_NONE) { close(fd); return NET_SOCK_INVALID; }

    rc = connect(fd, (struct sockaddr *)&addr, sizeof addr);
    if (rc < 0 && errno != EINPROGRESS && errno != EINTR) { close(fd); return NET_SOCK_INVALID; }
    return (net_sock)fd;   /* connection completes asynchronously (net_sys_connect_done) */
}

int net_sys_connect_done(net_sock s)
{
    int fd = (int)s, err = 0;
    socklen_t elen = sizeof err;
    fd_set wf;
    struct timeval tv;

    FD_ZERO(&wf);
    FD_SET(fd, &wf);
    tv.tv_sec = 0; tv.tv_usec = 0;          /* poll, never block */
    if (select(fd + 1, NULL, &wf, NULL, &tv) <= 0) return 0;   /* still pending */
    if (!FD_ISSET(fd, &wf)) return 0;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0) return -1;
    return (err == 0) ? 1 : -1;
}

long net_sys_send(net_sock s, const void *buf, long len)
{
    ssize_t n = send((int)s, buf, (size_t)len, MSG_NOSIGNAL);
    if (n >= 0) return (long)n;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    return -1;
}

long net_sys_recv(net_sock s, void *buf, long len)
{
    ssize_t n = recv((int)s, buf, (size_t)len, 0);
    if (n > 0)  return (long)n;
    if (n == 0) return -1;                  /* orderly close -> drop */
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    return -1;
}

void net_sys_close(net_sock s)
{
    if (s != NET_SOCK_INVALID) close((int)s);
}

void net_sys_sleep_ms(int ms)
{
    struct timespec ts;
    if (ms < 0) ms = 0;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    (void)nanosleep(&ts, NULL);
}

double net_sys_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

#endif /* !_WIN32 */

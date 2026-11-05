/*
 * The parts of cpmemu's platform layer that gdbstub.cc uses: the TCP socket
 * calls, and the console and signal calls that the stub makes while it waits.
 * POSIX only; romwbw_emu.cc is a POSIX program.
 *
 * cpmemu's os/linux/platform.cc is not compiled here.  It owns the terminal
 * and the signal handlers of cpmemu, and this program has its own in
 * emu_io_cli.cc and romwbw_emu.cc.  The socket functions below are the same
 * code as cpmemu's, with the same return values (os/platform.h says which).
 */

#include "os/platform.h"
#include "romwbw_gdb.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

static volatile sig_atomic_t g_signal = 0;

void romwbw_gdb_note_signal(int sig) { g_signal = sig; }

namespace platform {

bool is_terminal() { return isatty(STDIN_FILENO) != 0; }

// The stub reads the console only while it waits for a connection, and only
// when the console is a terminal.  The terminal is in raw mode by then.
int console_getchar() {
  for (;;) {
    if (g_signal) return -1;
    unsigned char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n == 1) return c;
    if (n == 0) return -1;
    if (errno == EINTR) continue;
    return -1;
  }
}

int termination_requested() { return g_signal; }

socket_t net_listen(uint32_t addr, int port, int* bound_port) {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) {
    fprintf(stderr, "gdb stub: socket: %s\n", strerror(errno));
    return kNoSocket;
  }
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(addr);
  a.sin_port = htons((unsigned short)port);
  if (bind(s, (struct sockaddr*)&a, sizeof a) != 0 || listen(s, 1) != 0) {
    fprintf(stderr, "gdb stub: cannot listen on %u.%u.%u.%u:%d: %s\n",
            (unsigned)(addr >> 24), (unsigned)(addr >> 16) & 255u,
            (unsigned)(addr >> 8) & 255u, (unsigned)addr & 255u, port, strerror(errno));
    close(s);
    return kNoSocket;
  }
  socklen_t len = sizeof a;
  if (getsockname(s, (struct sockaddr*)&a, &len) != 0) {
    fprintf(stderr, "gdb stub: getsockname: %s\n", strerror(errno));
    close(s);
    return kNoSocket;
  }
  if (bound_port) *bound_port = ntohs(a.sin_port);
  return (socket_t)s;
}

socket_t net_listen_local(int port, int* bound_port) {
  return net_listen(INADDR_LOOPBACK, port, bound_port);
}

socket_t net_accept(socket_t listener) {
  for (;;) {
    int c = accept((int)listener, NULL, NULL);
    if (c >= 0) {
      int one = 1;
      setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
      setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
      return (socket_t)c;
    }
    if (errno == EINTR) {
      if (termination_requested()) return kNoSocket;
      continue;
    }
    fprintf(stderr, "gdb stub: accept: %s\n", strerror(errno));
    return kNoSocket;
  }
}

int net_recv(socket_t s, void* buf, int len, int timeout_ms) {
  if (timeout_ms >= 0) {
    struct pollfd p;
    p.fd = (int)s;
    p.events = POLLIN;
    p.revents = 0;
    int n = poll(&p, 1, timeout_ms);
    if (n < 0) return errno == EINTR ? -3 : -1;
    if (n == 0) return -2;
  }
  ssize_t n = recv((int)s, buf, (size_t)len, 0);
  if (n > 0) return (int)n;
  if (n == 0) return 0;
  return errno == EINTR ? -3 : -1;
}

bool net_send_all(socket_t s, const void* buf, int len) {
  const char* p = (const char*)buf;
#ifdef MSG_NOSIGNAL
  const int flags = MSG_NOSIGNAL;
#else
  const int flags = 0;
#endif
  while (len > 0) {
    ssize_t n = send((int)s, p, (size_t)len, flags);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    p += n;
    len -= (int)n;
  }
  return true;
}

void net_close(socket_t s) {
  if (s != kNoSocket) close((int)s);
}

int wait_console_or_socket(socket_t s, int timeout_ms) {
  struct pollfd p[2];
  p[0].fd = STDIN_FILENO;
  p[0].events = POLLIN;
  p[0].revents = 0;
  nfds_t count = 1;
  if (s != kNoSocket) {
    p[1].fd = (int)s;
    p[1].events = POLLIN;
    p[1].revents = 0;
    count = 2;
  }
  int n = poll(p, count, timeout_ms);
  if (n < 0) return -1;
  int mask = 0;
  if (p[0].revents) mask |= 1;
  if (count == 2 && p[1].revents) mask |= 2;
  return mask;
}

}  // namespace platform

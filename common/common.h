/* common.h - shared by uxxt-logic, uxxt-x11, uxxt-buffer.
 * unix socket connect + hex coding. nothing else. */
#ifndef UXXT_COMMON_H
#define UXXT_COMMON_H

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static inline int uxxt_connect(const char *path) {
  struct sockaddr_un a;
  int fd, e;

  if (strlen(path) >= sizeof a.sun_path) {
    errno = ENAMETOOLONG;
    return -1;
  }
  fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;
  memset(&a, 0, sizeof a);
  a.sun_family = AF_UNIX;
  strcpy(a.sun_path, path);
  if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
    e = errno;
    close(fd);
    errno = e;
    return -1;
  }
  return fd;
}

/* out needs 2*n+1 bytes */
static inline size_t uxxt_hexenc(const unsigned char *in, size_t n, char *out) {
  static const char h[] = "0123456789abcdef";
  size_t i;

  for (i = 0; i < n; ++i) {
    out[2 * i] = h[in[i] >> 4];
    out[2 * i + 1] = h[in[i] & 15];
  }
  out[2 * n] = 0;
  return 2 * n;
}

static inline int uxxt_hexval(int c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

/* returns decoded byte count, or -1 on bad input */
static inline long uxxt_hexdec(const char *in, unsigned char *out, size_t max) {
  size_t n = 0;
  int hi, lo;

  while (in[0] && in[1]) {
    hi = uxxt_hexval(in[0]);
    lo = uxxt_hexval(in[1]);
    if (hi < 0 || lo < 0 || n >= max)
      return -1;
    out[n++] = (unsigned char)(hi << 4 | lo);
    in += 2;
  }
  return in[0] ? -1 : (long)n;
}

#endif

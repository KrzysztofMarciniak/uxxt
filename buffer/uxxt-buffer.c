/* uxxt-buffer - connect to uxxt-logic, write the terminal text to stdout.
 * usage: uxxt-buffer [-s] [socket]     -s: visible screen only */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "../common/common.h"

static int wall(int fd, const void *d, size_t n) {
  const char *p = d;
  while (n) {
    ssize_t w = write(fd, p, n);
    if (w < 0)
      return -1;
    p += w;
    n -= (size_t)w;
  }
  return 0;
}

int main(int argc, char **argv) {
  const char *scope = BUFFER_SCOPE, *path;
  char line[64], buf[65536];
  size_t used = 0, left = 0;
  int fd, i = 1;
  ssize_t n;

  if (i < argc && !strcmp(argv[i], "-s")) {
    scope = "screen";
    ++i;
  }
  path = i < argc ? argv[i] : getenv(SOCKET_ENV);
  if (!path || !*path) {
    fprintf(stderr, "usage: uxxt-buffer [-s] [socket]   (or set $%s)\n", SOCKET_ENV);
    return 2;
  }
  fd = uxxt_connect(path);
  if (fd < 0) {
    perror(path);
    return 1;
  }
  snprintf(line, sizeof line, "buffer %s\n", scope);
  if (wall(fd, line, strlen(line)) < 0)
    return 1;

  /* header line: "text N\n" */
  for (;;) {
    char *nl;
    n = read(fd, buf + used, sizeof buf - used);
    if (n <= 0)
      return 1;
    used += (size_t)n;
    if ((nl = memchr(buf, '\n', used))) {
      size_t hl = (size_t)(nl - buf) + 1;
      *nl = 0;
      if (sscanf(buf, "text %zu", &left) != 1)
        return 1;
      used -= hl;
      memmove(buf, buf + hl, used);
      break;
    }
    if (used == sizeof buf)
      return 1;
  }
  for (;;) {
    size_t k = used < left ? used : left;
    if (k && wall(1, buf, k) < 0)
      return 1;
    left -= k;
    used = 0;
    if (!left)
      return 0;
    n = read(fd, buf, sizeof buf);
    if (n <= 0)
      return 1;
    used = (size_t)n;
  }
}

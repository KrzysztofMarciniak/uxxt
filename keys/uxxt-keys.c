/* uxxt-keys - send keys or text to a running uxxt-logic. like tmux send-keys.
 *
 * usage: uxxt-keys [-s socket] [-l|-x] [key|text ...]
 *   enter  tab  esc  up  f5  C-c  A-x  C-A-left  space   named keys, modifiers
 *   anything else                                         typed as text
 *   -l       every argument is literal text, joined by spaces
 *   -x       every argument is a raw protocol line, e.g. -x quit
 *   no args  stdin is sent as text
 *
 *   uxxt-keys 'ls -la' enter
 *   uxxt-keys C-c
 *   echo 'make' | uxxt-keys
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "config.h"
#include "../common/common.h"

struct buf {
  char *p;
  size_t n, cap;
};

static void bput(struct buf *b, const void *d, size_t n) {
  if (b->n + n + 1 > b->cap) {
    size_t c = b->cap ? b->cap : 4096;
    while (c < b->n + n + 1)
      c *= 2;
    b->p = realloc(b->p, c);
    if (!b->p) {
      perror("realloc");
      exit(1);
    }
    b->cap = c;
  }
  memcpy(b->p + b->n, d, n);
  b->n += n;
  b->p[b->n] = 0;
}

static void bstr(struct buf *b, const char *s) { bput(b, s, strlen(s)); }

/* text as hex: "in HEX\n" */
static void send_text(struct buf *b, const unsigned char *d, size_t n) {
  char *h;

  if (!n)
    return;
  h = malloc(2 * n + 1);
  if (!h)
    exit(1);
  uxxt_hexenc(d, n, h);
  bstr(b, "in ");
  bstr(b, h);
  bstr(b, "\n");
  free(h);
}

/* decode the first utf-8 char of s; returns its byte length, 0 if invalid */
static int u8first(const char *str, uint32_t *cp) {
  const unsigned char *s = (const unsigned char *)str;
  int n, i;
  uint32_t c;

  if (s[0] < 0x80) {
    *cp = s[0];
    return s[0] ? 1 : 0;
  }
  if ((s[0] & 0xe0) == 0xc0) {
    c = s[0] & 0x1f;
    n = 2;
  } else if ((s[0] & 0xf0) == 0xe0) {
    c = s[0] & 0x0f;
    n = 3;
  } else if ((s[0] & 0xf8) == 0xf0) {
    c = s[0] & 0x07;
    n = 4;
  } else
    return 0;
  for (i = 1; i < n; ++i) {
    if ((s[i] & 0xc0) != 0x80)
      return 0;
    c = c << 6 | (s[i] & 0x3f);
  }
  *cp = c;
  return n;
}

static int is_key_name(const char *t) {
  size_t i;
  int f;
  char x;

  for (i = 0; i < sizeof key_names / sizeof *key_names; ++i)
    if (!strcasecmp(t, key_names[i]))
      return 1;
  return (t[0] == 'f' || t[0] == 'F') && sscanf(t + 1, "%d%c", &f, &x) == 1 &&
         f >= 1 && f <= 35;
}

/* one argument -> protocol line(s). returns 0 on success */
static int send_arg(struct buf *b, const char *arg) {
  const char *t = arg;
  const char *key = NULL;
  uint32_t cp = 0;
  char line[96], name[32];
  int mod = 0, n;
  size_t i;

  while (t[0] && t[1] == '-' && t[2]) {
    if (t[0] == PREFIX_CTRL)
      mod |= 4;
    else if (t[0] == PREFIX_ALT || t[0] == PREFIX_ALT2)
      mod |= 2;
    else if (t[0] == PREFIX_SHIFT)
      mod |= 1;
    else
      break;
    t += 2;
  }

  for (i = 0; i < sizeof aliases / sizeof *aliases; ++i)
    if (!strcasecmp(t, aliases[i].alias)) {
      key = aliases[i].key;
      cp = aliases[i].cp;
      break;
    }

  if (!key && !cp && is_key_name(t))
    key = t;

  if (key) {
    snprintf(name, sizeof name, "%s", key);
    for (i = 0; name[i]; ++i)
      if (name[i] >= 'A' && name[i] <= 'Z')
        name[i] = (char)(name[i] + 32);
    snprintf(line, sizeof line, "key %s %d\n", name, mod);
    bstr(b, line);
    return 0;
  }

  if (!cp) {
    n = u8first(t, &cp);
    if (!n || t[n]) /* not exactly one character */
      cp = 0;
  }
  if (cp) {
    snprintf(line, sizeof line, "chr %lu %d\n", (unsigned long)cp, mod & ~1);
    bstr(b, line);
    return 0;
  }

  if (mod) {
    fprintf(stderr, "uxxt-keys: bad key: %s\n", arg);
    return -1;
  }
  send_text(b, (const unsigned char *)t, strlen(t));
  return 0;
}

int main(int argc, char **argv) {
  const char *path = NULL;
  struct buf b = {0};
  int i = 1, literal = 0, raw = 0, fd, bad = 0;
  size_t off = 0;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; ++i) {
    if (!strcmp(argv[i], "--")) {
      ++i;
      break;
    }
    if (!strcmp(argv[i], "-l"))
      literal = 1;
    else if (!strcmp(argv[i], "-x"))
      raw = 1;
    else if (!strcmp(argv[i], "-s") && i + 1 < argc)
      path = argv[++i];
    else
      break;
  }
  if (!path)
    path = getenv(SOCKET_ENV);
  if (!path || !*path) {
    fprintf(stderr, "usage: uxxt-keys [-s socket] [-l|-x] [key|text ...]   (or set $%s)\n", SOCKET_ENV);
    return 2;
  }

  if (i >= argc) {
    unsigned char tmp[8192];
    struct buf in = {0};
    ssize_t n;

    while ((n = read(0, tmp, sizeof tmp)) > 0)
      bput(&in, tmp, (size_t)n);
    send_text(&b, (const unsigned char *)(in.p ? in.p : ""), in.n);
    free(in.p);
  } else if (raw) {
    for (; i < argc; ++i) {
      bstr(&b, argv[i]);
      bstr(&b, "\n");
    }
  } else if (literal) {
    struct buf lit = {0};
    int k;

    for (k = i; k < argc; ++k) {
      if (k > i)
        bput(&lit, " ", 1);
      bstr(&lit, argv[k]);
    }
    send_text(&b, (const unsigned char *)lit.p, lit.n);
    free(lit.p);
  } else {
    for (; i < argc; ++i)
      if (send_arg(&b, argv[i]) < 0)
        bad = 1;
  }

  if (!b.n)
    return bad ? 1 : 0;
  fd = uxxt_connect(path);
  if (fd < 0) {
    perror(path);
    return 1;
  }
  while (off < b.n) {
    ssize_t w = write(fd, b.p + off, b.n - off);
    if (w < 0) {
      perror("write");
      return 1;
    }
    off += (size_t)w;
  }
  close(fd);
  return bad ? 1 : 0;
}

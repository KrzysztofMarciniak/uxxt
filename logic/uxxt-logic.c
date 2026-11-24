/* uxxt-logic - owns the pty, the child, libvterm and the scrollback.
 * talks to frontends over a unix socket in plain text lines. no X11.
 *
 * usage: uxxt-logic [-s sockpath] [-c cols] [-r rows] [-p] [-- cmd args...]
 *   -p  print the socket path on stdout
 *
 * client -> logic (one command per line)
 *   attach C R        subscribe to screen updates, set size
 *   resize C R
 *   key NAME MOD      enter tab backspace escape up down left right ins del
 *                     home end pageup pagedown f1..f35     MOD: shift=1 alt=2 ctrl=4
 *   chr CODEPOINT MOD
 *   in HEX            raw bytes to the child
 *   scroll N          N>0 back into history, N<0 forward
 *   buffer [all|screen] reply: "text N\n" + N bytes of plain text
 *   quit              hang up the child and exit
 *
 * logic -> attached clients
 *   g C R             grid size (a full redraw follows)
 *   r Y               row Y is cleared, spans for it follow
 *   s Y X FG BG ATTR TEXT   FG/BG: "-" default, "iNN" palette, "#rrggbb"
 *                     ATTR: bold=1 underline=2 italic=4 blink=8 reverse=16
 *                     strike=32 conceal=64 wide=128 (TEXT is one 2-cell char)
 *   c X Y VIS BLINK SHAPE   cursor (shape 1 block 2 underline 3 bar)
 *   o OFFSET TOTAL    scrollback offset / lines in scrollback
 *   t TITLE
 *   b                 bell
 *   f                 end of frame
 *   x STATUS          child exited, logic is exiting
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <vterm.h>

#include "config.h"
#include "../common/common.h"

/* ---------- small helpers ---------- */

struct buf {
  char *p;
  size_t n, cap;
};

static void die(const char *m) {
  perror(m);
  exit(1);
}

static void bput(struct buf *b, const void *d, size_t n) {
  if (b->n + n + 1 > b->cap) {
    size_t c = b->cap ? b->cap : 4096;
    while (c < b->n + n + 1)
      c *= 2;
    b->p = realloc(b->p, c);
    if (!b->p)
      die("realloc");
    b->cap = c;
  }
  memcpy(b->p + b->n, d, n);
  b->n += n;
  b->p[b->n] = 0;
}

static void bfmt(struct buf *b, const char *fmt, ...) {
  char t[256];
  va_list ap;
  int n;

  va_start(ap, fmt);
  n = vsnprintf(t, sizeof t, fmt, ap);
  va_end(ap);
  if (n > 0)
    bput(b, t, (size_t)n < sizeof t ? (size_t)n : sizeof t - 1);
}

static int u8enc(uint32_t c, char *o) {
  if (c < 0x80) {
    o[0] = (char)c;
    return 1;
  }
  if (c < 0x800) {
    o[0] = (char)(0xc0 | c >> 6);
    o[1] = (char)(0x80 | (c & 63));
    return 2;
  }
  if (c < 0x10000) {
    o[0] = (char)(0xe0 | c >> 12);
    o[1] = (char)(0x80 | ((c >> 6) & 63));
    o[2] = (char)(0x80 | (c & 63));
    return 3;
  }
  o[0] = (char)(0xf0 | c >> 18);
  o[1] = (char)(0x80 | ((c >> 12) & 63));
  o[2] = (char)(0x80 | ((c >> 6) & 63));
  o[3] = (char)(0x80 | (c & 63));
  return 4;
}

/* ---------- state ---------- */

static VTerm *vt;
static VTermScreen *vts;
static VTermScreenCallbacks cbs;

static int cols, rows, master = -1, listener = -1;
static pid_t child;
static const char *sockpath;
static volatile sig_atomic_t quit_flag;

static int cur_x, cur_y, cur_vis = 1, cur_blink = 1, cur_shape = 1;
static char title[1024];
static int title_len, title_dirty;

static int view_off; /* lines scrolled back from the bottom */
static int dirty, need_full;
static size_t pending_bytes;

struct line {
  int n;
  VTermScreenCell *cells;
};
static struct line *hist;
static int hstart, hcount;

static char **shadow; /* last text sent per row */

static struct buf ptyq; /* bytes waiting to go to the child */
static size_t ptypos;

struct client {
  int fd, attached;
  char *in;
  size_t inlen;
  char *out;
  size_t outlen, outpos, outcap;
};
static struct client cl[MAX_CLIENTS];

/* ---------- clients ---------- */

static void client_drop(struct client *c) {
  if (c->fd >= 0)
    close(c->fd);
  free(c->in);
  free(c->out);
  memset(c, 0, sizeof *c);
  c->fd = -1;
}

static void client_flush(struct client *c) {
  while (c->fd >= 0 && c->outpos < c->outlen) {
    ssize_t n = write(c->fd, c->out + c->outpos, c->outlen - c->outpos);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      if (errno != EAGAIN)
        client_drop(c);
      return;
    }
    c->outpos += (size_t)n;
  }
  if (c->fd >= 0 && c->outpos == c->outlen)
    c->outpos = c->outlen = 0;
}

static void client_send(struct client *c, const char *s, size_t n) {
  if (c->fd < 0)
    return;
  if (c->outlen - c->outpos + n > CLIENT_QUEUE_MAX) {
    client_drop(c);
    return;
  }
  if (c->outlen + n > c->outcap) {
    size_t cap = c->outcap ? c->outcap : 8192;
    while (cap < c->outlen + n)
      cap *= 2;
    c->out = realloc(c->out, cap);
    if (!c->out)
      die("realloc");
    c->outcap = cap;
  }
  memcpy(c->out + c->outlen, s, n);
  c->outlen += n;
  client_flush(c);
}

static void broadcast(const struct buf *b, int attached_only) {
  int i;

  for (i = 0; i < MAX_CLIENTS; ++i)
    if (cl[i].fd >= 0 && (cl[i].attached || !attached_only))
      client_send(&cl[i], b->p, b->n);
}

static int nattached(void) {
  int i, n = 0;

  for (i = 0; i < MAX_CLIENTS; ++i)
    if (cl[i].fd >= 0 && cl[i].attached)
      ++n;
  return n;
}

/* ---------- child pty ---------- */

static void pty_flush(void) {
  while (ptypos < ptyq.n) {
    ssize_t n = write(master, ptyq.p + ptypos, ptyq.n - ptypos);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return; /* EAGAIN: poll will tell us */
    }
    ptypos += (size_t)n;
  }
  ptyq.n = ptypos = 0;
}

static void to_child(const void *d, size_t n) {
  bput(&ptyq, d, n);
  pty_flush();
}

static void vt_output(const char *s, size_t len, void *user) {
  (void)user;
  to_child(s, len);
}

static void set_winsize(void) {
  struct winsize ws;

  memset(&ws, 0, sizeof ws);
  ws.ws_row = (unsigned short)rows;
  ws.ws_col = (unsigned short)cols;
  ioctl(master, TIOCSWINSZ, &ws);
}

static void spawn(char **argv) {
  char *name, *dflt[2];
  int sl;

  master = posix_openpt(O_RDWR | O_NOCTTY);
  if (master < 0 || grantpt(master) < 0 || unlockpt(master) < 0)
    die("pty");
  name = strdup(ptsname(master));
  if (!name)
    die("strdup");
  set_winsize();

  child = fork();
  if (child < 0)
    die("fork");
  if (child == 0) {
    signal(SIGPIPE, SIG_DFL);
    setsid();
    sl = open(name, O_RDWR);
    if (sl < 0)
      _exit(127);
    ioctl(sl, TIOCSCTTY, 0);
    dup2(sl, 0);
    dup2(sl, 1);
    dup2(sl, 2);
    if (sl > 2)
      close(sl);
    close(master);
    setenv("TERM", TERM_NAME, 1);
    setenv("COLORTERM", COLORTERM, 1);
    if (!argv[0]) {
      dflt[0] = getenv("SHELL") ? getenv("SHELL") : DEFAULT_SHELL;
      dflt[1] = NULL;
      argv = dflt;
    }
    execvp(argv[0], argv);
    _exit(127);
  }
  free(name);
  fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
  fcntl(master, F_SETFD, FD_CLOEXEC);
}

/* ---------- scrollback ---------- */

static void blank_cell(VTermScreenCell *c) {
  memset(c, 0, sizeof *c);
  c->chars[0] = ' ';
  c->width = 1;
  c->fg.type = VTERM_COLOR_DEFAULT_FG;
  c->bg.type = VTERM_COLOR_DEFAULT_BG;
}

static int cell_is_blank(const VTermScreenCell *c) {
  return (c->chars[0] == 0 || c->chars[0] == ' ') && !c->attrs.reverse &&
         VTERM_COLOR_IS_DEFAULT_BG(&c->bg);
}

static int cb_pushline(int ncols, const VTermScreenCell *line, void *user) {
  struct line *l;
  int n, idx;

  (void)user;
  for (n = ncols; n > 0 && cell_is_blank(&line[n - 1]); --n)
    ;

  if (hcount == SCROLLBACK_LINES) {
    idx = hstart;
    hstart = (hstart + 1) % SCROLLBACK_LINES;
    free(hist[idx].cells);
  } else {
    idx = (hstart + hcount++) % SCROLLBACK_LINES;
  }
  l = &hist[idx];
  l->n = 0;
  l->cells = malloc(sizeof *l->cells * (size_t)(n ? n : 1));
  if (!l->cells)
    return 1;
  memcpy(l->cells, line, sizeof *l->cells * (size_t)n);
  l->n = n;

  if (view_off > 0)
    ++view_off;
  if (view_off > hcount)
    view_off = hcount;
  return 1;
}

/* the cell at column x of visible row y, taking the scroll offset into account */
static void view_cell(int x, int y, VTermScreenCell *c) {
  int idx = hcount - view_off + y;
  VTermPos p;

  if (idx < 0) {
    blank_cell(c);
    return;
  }
  if (idx < hcount) {
    struct line *l = &hist[(hstart + idx) % SCROLLBACK_LINES];
    if (x < l->n)
      *c = l->cells[x];
    else
      blank_cell(c);
    return;
  }
  p.row = idx - hcount;
  p.col = x;
  vterm_screen_get_cell(vts, p, c);
}

/* ---------- vterm callbacks ---------- */

static int cb_movecursor(VTermPos pos, VTermPos old, int visible, void *user) {
  (void)old;
  (void)user;
  cur_x = pos.col;
  cur_y = pos.row;
  cur_vis = visible;
  dirty = 1;
  return 1;
}

static int cb_settermprop(VTermProp prop, VTermValue *val, void *user) {
  (void)user;
  switch (prop) {
  case VTERM_PROP_CURSORVISIBLE:
    cur_vis = val->boolean;
    break;
  case VTERM_PROP_CURSORBLINK:
    cur_blink = val->boolean;
    break;
  case VTERM_PROP_CURSORSHAPE:
    cur_shape = val->number;
    break;
  case VTERM_PROP_TITLE: {
    VTermStringFragment f = val->string;
    size_t i;

    if (f.initial)
      title_len = 0;
    for (i = 0; i < f.len && title_len < (int)sizeof title - 1; ++i)
      if ((unsigned char)f.str[i] >= 0x20 && f.str[i] != 0x7f)
        title[title_len++] = f.str[i];
    title[title_len] = 0;
    if (f.final)
      title_dirty = 1;
    break;
  }
  default:
    break;
  }
  dirty = 1;
  return 1;
}

static int cb_bell(void *user) {
  struct buf b = {0};

  (void)user;
  bput(&b, "b\n", 2);
  broadcast(&b, 1);
  free(b.p);
  return 1;
}

/* ---------- frame building ---------- */

static void col_tok(const VTermColor *c, int is_bg, char *o) {
  if (is_bg ? VTERM_COLOR_IS_DEFAULT_BG(c) : VTERM_COLOR_IS_DEFAULT_FG(c))
    strcpy(o, "-");
  else if (VTERM_COLOR_IS_INDEXED(c))
    sprintf(o, "i%d", c->indexed.idx);
  else
    sprintf(o, "#%02x%02x%02x", c->rgb.red, c->rgb.green, c->rgb.blue);
}

struct span {
  int open, x, n, attr;
  char fg[16], bg[16];
  struct buf t;
};

static void span_flush(struct buf *b, struct span *s, int y) {
  if (!s->open)
    return;
  bfmt(b, "s %d %d %s %s %d ", y, s->x, s->fg, s->bg, s->attr);
  bput(b, s->t.p ? s->t.p : "", s->t.n);
  bput(b, "\n", 1);
  s->open = 0;
  s->t.n = 0;
}

static void emit_row(struct buf *b, int y) {
  static struct span s;
  int x;

  s.open = 0;
  s.t.n = 0;
  bfmt(b, "r %d\n", y);
  for (x = 0; x < cols; ++x) {
    VTermScreenCell c;
    uint32_t ch;
    int attr, wide;
    char fg[16], bg[16], u[4];

    view_cell(x, y, &c);
    if (c.chars[0] == (uint32_t)-1) /* right half of a wide char */
      continue;
    ch = c.chars[0] ? c.chars[0] : ' ';
    if (ch < 0x20 || (ch >= 0x7f && ch < 0xa0))
      ch = ' ';
    wide = c.width == 2;
    attr = (c.attrs.bold ? 1 : 0) | (c.attrs.underline ? 2 : 0) |
           (c.attrs.italic ? 4 : 0) | (c.attrs.blink ? 8 : 0) |
           (c.attrs.reverse ? 16 : 0) | (c.attrs.strike ? 32 : 0) |
           (c.attrs.conceal ? 64 : 0) | (wide ? 128 : 0);
    col_tok(&c.fg, 0, fg);
    col_tok(&c.bg, 1, bg);

    if (ch == ' ' && !attr && !strcmp(fg, "-") && !strcmp(bg, "-")) {
      span_flush(b, &s, y);
      continue;
    }
    if (s.open && !wide && attr == s.attr && !strcmp(fg, s.fg) &&
        !strcmp(bg, s.bg) && x == s.x + s.n) {
      ++s.n;
    } else {
      span_flush(b, &s, y);
      s.open = 1;
      s.x = x;
      s.n = 1;
      s.attr = attr;
      strcpy(s.fg, fg);
      strcpy(s.bg, bg);
    }
    bput(&s.t, u, (size_t)u8enc(ch, u));
    if (wide)
      span_flush(b, &s, y);
  }
  span_flush(b, &s, y);
}

static void emit_frame(void) {
  static struct buf fb, rb;
  int y;

  dirty = 0;
  pending_bytes = 0;
  if (!nattached())
    return;

  fb.n = 0;
  if (need_full) {
    for (y = 0; y < rows; ++y) {
      free(shadow[y]);
      shadow[y] = NULL;
    }
    bfmt(&fb, "g %d %d\n", cols, rows);
  }
  for (y = 0; y < rows; ++y) {
    rb.n = 0;
    emit_row(&rb, y);
    if (shadow[y] && !strcmp(shadow[y], rb.p ? rb.p : ""))
      continue;
    free(shadow[y]);
    shadow[y] = strdup(rb.p ? rb.p : "");
    bput(&fb, rb.p, rb.n);
  }
  bfmt(&fb, "c %d %d %d %d %d\n", cur_x, cur_y, cur_vis, cur_blink, cur_shape);
  bfmt(&fb, "o %d %d\n", view_off, hcount);
  if (title_dirty || need_full) {
    bput(&fb, "t ", 2);
    bput(&fb, title, (size_t)title_len);
    bput(&fb, "\n", 1);
    title_dirty = 0;
  }
  bput(&fb, "f\n", 2);
  need_full = 0;
  broadcast(&fb, 1);
}

/* ---------- buffer ---------- */

static void cells_text(struct buf *b, const VTermScreenCell *c, int n) {
  struct buf t = {0};
  char u[4];
  int x;

  for (x = 0; x < n; ++x) {
    if (c[x].chars[0] == (uint32_t)-1)
      continue;
    bput(&t, u, (size_t)u8enc(c[x].chars[0] ? c[x].chars[0] : ' ', u));
  }
  if (BUFFER_TRIM)
    while (t.n && t.p[t.n - 1] == ' ')
      --t.n;
  bput(b, t.p ? t.p : "", t.n);
  bput(b, "\n", 1);
  free(t.p);
}

static void buffer_text(struct buf *b, int with_history) {
  VTermScreenCell *row;
  int i, x;
  VTermPos p;

  row = malloc(sizeof *row * (size_t)cols);
  if (!row)
    return;
  if (with_history)
    for (i = 0; i < hcount; ++i) {
      struct line *l = &hist[(hstart + i) % SCROLLBACK_LINES];
      cells_text(b, l->cells, l->n);
    }
  for (i = 0; i < rows; ++i) {
    p.row = i;
    for (x = 0; x < cols; ++x) {
      p.col = x;
      vterm_screen_get_cell(vts, p, &row[x]);
    }
    cells_text(b, row, cols);
  }
  free(row);
  if (BUFFER_TRIM)
    while (b->n >= 2 && b->p[b->n - 1] == '\n' && b->p[b->n - 2] == '\n')
      --b->n;
}

/* ---------- commands ---------- */

static void do_resize(int c, int r) {
  int y;

  if (c < 1)
    c = 1;
  if (r < 1)
    r = 1;
  if (c > 1000)
    c = 1000;
  if (r > 1000)
    r = 1000;
  if (c == cols && r == rows)
    return;
  for (y = 0; y < rows; ++y)
    free(shadow[y]);
  free(shadow);
  cols = c;
  rows = r;
  shadow = calloc((size_t)rows, sizeof *shadow);
  if (!shadow)
    die("calloc");
  vterm_set_size(vt, rows, cols);
  set_winsize();
  if (view_off > hcount)
    view_off = hcount;
}

static const struct {
  const char *name;
  VTermKey key;
} keys[] = {
    {"enter", VTERM_KEY_ENTER}, {"tab", VTERM_KEY_TAB},
    {"backspace", VTERM_KEY_BACKSPACE}, {"escape", VTERM_KEY_ESCAPE},
    {"up", VTERM_KEY_UP}, {"down", VTERM_KEY_DOWN},
    {"left", VTERM_KEY_LEFT}, {"right", VTERM_KEY_RIGHT},
    {"ins", VTERM_KEY_INS}, {"del", VTERM_KEY_DEL},
    {"home", VTERM_KEY_HOME}, {"end", VTERM_KEY_END},
    {"pageup", VTERM_KEY_PAGEUP}, {"pagedown", VTERM_KEY_PAGEDOWN},
};

static VTermKey find_key(const char *n) {
  size_t i;
  int f;

  for (i = 0; i < sizeof keys / sizeof *keys; ++i)
    if (!strcmp(keys[i].name, n))
      return keys[i].key;
  if (n[0] == 'f' && sscanf(n + 1, "%d", &f) == 1 && f >= 1 && f <= 35)
    return (VTermKey)VTERM_KEY_FUNCTION(f);
  return VTERM_KEY_NONE;
}

static void snap(void) {
  if (SNAP_ON_INPUT && view_off) {
    view_off = 0;
    dirty = 1;
  }
}

static void on_line(struct client *c, char *l) {
  char name[32], kn[16];
  char *arg;
  int a, b, mod;
  unsigned long cp;

  if (sscanf(l, "%31s", name) != 1)
    return;
  arg = l + strlen(name);
  while (*arg == ' ')
    ++arg;

  if (!strcmp(name, "attach") || !strcmp(name, "resize")) {
    if (sscanf(arg, "%d %d", &a, &b) == 2) {
      if (name[0] == 'a')
        c->attached = 1;
      do_resize(a, b);
      need_full = dirty = 1;
    }
  } else if (!strcmp(name, "key")) {
    if (sscanf(arg, "%15s %d", kn, &mod) == 2 && find_key(kn) != VTERM_KEY_NONE) {
      snap();
      vterm_keyboard_key(vt, find_key(kn), (VTermModifier)mod);
      pty_flush();
    }
  } else if (!strcmp(name, "chr")) {
    if (sscanf(arg, "%lu %d", &cp, &mod) == 2) {
      snap();
      vterm_keyboard_unichar(vt, (uint32_t)cp, (VTermModifier)mod);
      pty_flush();
    }
  } else if (!strcmp(name, "in")) {
    size_t max = strlen(arg) / 2 + 1;
    unsigned char *d = malloc(max);
    long n = d ? uxxt_hexdec(arg, d, max) : -1;
    if (n > 0) {
      snap();
      to_child(d, (size_t)n);
    }
    free(d);
  } else if (!strcmp(name, "scroll")) {
    if (sscanf(arg, "%d", &a) == 1) {
      long v = (long)view_off + a;
      view_off = v < 0 ? 0 : v > hcount ? hcount : (int)v;
      dirty = 1;
    }
  } else if (!strcmp(name, "buffer")) {
    struct buf out = {0}, hdr = {0};
    buffer_text(&out, strcmp(arg, "screen") != 0);
    bfmt(&hdr, "text %zu\n", out.n);
    client_send(c, hdr.p, hdr.n);
    if (out.n)
      client_send(c, out.p, out.n);
    free(out.p);
    free(hdr.p);
  } else if (!strcmp(name, "quit")) {
    quit_flag = 1;
  }
}

static void on_client_data(struct client *c) {
  char tmp[65536];
  ssize_t n = read(c->fd, tmp, sizeof tmp);
  char *nl;
  size_t used;

  if (n < 0 && (errno == EAGAIN || errno == EINTR))
    return;
  if (n <= 0) {
    client_drop(c);
    return;
  }
  c->in = realloc(c->in, c->inlen + (size_t)n + 1);
  if (!c->in)
    die("realloc");
  memcpy(c->in + c->inlen, tmp, (size_t)n);
  c->inlen += (size_t)n;
  c->in[c->inlen] = 0;

  used = 0;
  while (c->fd >= 0 && (nl = memchr(c->in + used, '\n', c->inlen - used))) {
    *nl = 0;
    on_line(c, c->in + used);
    used = (size_t)(nl - c->in) + 1;
  }
  if (c->fd < 0)
    return;
  memmove(c->in, c->in + used, c->inlen - used);
  c->inlen -= used;
  if (c->inlen > LINE_MAX_BYTES)
    client_drop(c);
}

/* ---------- setup ---------- */

static void on_signal(int s) {
  (void)s;
  quit_flag = 1;
}

static void cleanup(void) {
  if (sockpath)
    unlink(sockpath);
}

static void make_listener(void) {
  struct sockaddr_un a;
  mode_t old;

  if (strlen(sockpath) >= sizeof a.sun_path) {
    fprintf(stderr, "uxxt-logic: socket path too long\n");
    exit(1);
  }
  listener = socket(AF_UNIX, SOCK_STREAM, 0);
  if (listener < 0)
    die("socket");
  memset(&a, 0, sizeof a);
  a.sun_family = AF_UNIX;
  strcpy(a.sun_path, sockpath);
  unlink(sockpath);
  old = umask(077);
  if (bind(listener, (struct sockaddr *)&a, sizeof a) < 0)
    die("bind");
  umask(old);
  if (listen(listener, 8) < 0)
    die("listen");
  fcntl(listener, F_SETFL, fcntl(listener, F_GETFL) | O_NONBLOCK);
  fcntl(listener, F_SETFD, FD_CLOEXEC);
}

static void init_vterm(void) {
  vt = vterm_new(rows, cols);
  if (!vt)
    die("vterm_new");
  vterm_set_utf8(vt, 1);
  vterm_output_set_callback(vt, vt_output, NULL);
  vts = vterm_obtain_screen(vt);
  vterm_screen_enable_altscreen(vts, 1);
  memset(&cbs, 0, sizeof cbs);
  cbs.movecursor = cb_movecursor;
  cbs.settermprop = cb_settermprop;
  cbs.bell = cb_bell;
  cbs.sb_pushline = cb_pushline;
  vterm_screen_set_callbacks(vts, &cbs, NULL);
  vterm_screen_reset(vts, 1);
}

int main(int argc, char **argv) {
  static char rbuf[READ_SIZE];
  static char path[256];
  struct sigaction sa;
  int opt, print = 0, exited = 0, status = 0, i;

  cols = DEFAULT_COLS;
  rows = DEFAULT_ROWS;

  while ((opt = getopt(argc, argv, "+s:c:r:p")) != -1) {
    if (opt == 's')
      sockpath = optarg;
    else if (opt == 'c')
      cols = atoi(optarg);
    else if (opt == 'r')
      rows = atoi(optarg);
    else if (opt == 'p')
      print = 1;
    else {
      fprintf(stderr, "usage: uxxt-logic [-s sock] [-c cols] [-r rows] [-p] [-- cmd...]\n");
      return 2;
    }
  }
  if (cols < 1)
    cols = DEFAULT_COLS;
  if (rows < 1)
    rows = DEFAULT_ROWS;
  if (!sockpath) {
    const char *d = getenv(SOCKET_DIR_ENV);
    snprintf(path, sizeof path, "%s/%s%d.sock", d && *d ? d : SOCKET_FALLBACK,
             SOCKET_PREFIX, (int)getpid());
    sockpath = path;
  }

  for (i = 0; i < MAX_CLIENTS; ++i)
    cl[i].fd = -1;
  hist = calloc(SCROLLBACK_LINES, sizeof *hist);
  shadow = calloc((size_t)rows, sizeof *shadow);
  if (!hist || !shadow)
    die("calloc");

  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_signal;
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGHUP, &sa, NULL);
  signal(SIGPIPE, SIG_IGN);
  atexit(cleanup);

  make_listener();
  if (print) {
    printf("%s\n", sockpath);
    fflush(stdout);
  }
  init_vterm();
  spawn(argv + optind);

  while (!quit_flag && !exited) {
    struct pollfd p[2 + MAX_CLIENTS];
    int map[MAX_CLIENTS], np = 0, r, timeout;

    p[np].fd = listener;
    p[np++].events = POLLIN;
    p[np].fd = master;
    p[np++].events = (short)(POLLIN | (ptyq.n > ptypos ? POLLOUT : 0));
    for (i = 0; i < MAX_CLIENTS; ++i) {
      map[i] = -1;
      if (cl[i].fd < 0)
        continue;
      map[i] = np;
      p[np].fd = cl[i].fd;
      p[np++].events = (short)(POLLIN | (cl[i].outlen > cl[i].outpos ? POLLOUT : 0));
    }

    timeout = dirty ? FRAME_MS : -1;
    r = poll(p, (nfds_t)np, timeout);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      die("poll");
    }
    if (r == 0) {
      if (dirty)
        emit_frame();
      continue;
    }

    if (p[0].revents & POLLIN) {
      int fd = accept(listener, NULL, NULL);
      if (fd >= 0) {
        for (i = 0; i < MAX_CLIENTS && cl[i].fd >= 0; ++i)
          ;
        if (i == MAX_CLIENTS)
          close(fd);
        else {
          fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
          fcntl(fd, F_SETFD, FD_CLOEXEC);
          cl[i].fd = fd;
        }
      }
    }

    if (p[1].revents & POLLOUT)
      pty_flush();
    if (p[1].revents & (POLLIN | POLLHUP | POLLERR)) {
      ssize_t n = read(master, rbuf, sizeof rbuf);
      if (n > 0) {
        vterm_input_write(vt, rbuf, (size_t)n);
        pty_flush();
        dirty = 1;
        pending_bytes += (size_t)n;
        if (SNAP_ON_OUTPUT)
          view_off = 0;
        if (pending_bytes >= FLUSH_BYTES)
          emit_frame();
      } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
        exited = 1;
      }
    }

    for (i = 0; i < MAX_CLIENTS; ++i) {
      if (map[i] < 0 || cl[i].fd < 0)
        continue;
      if (p[map[i]].revents & POLLOUT)
        client_flush(&cl[i]);
      if (cl[i].fd >= 0 && (p[map[i]].revents & (POLLIN | POLLHUP | POLLERR)))
        on_client_data(&cl[i]);
    }
  }

  if (quit_flag && child > 0)
    kill(child, KILL_SIGNAL);

  if (exited) {
    struct buf b = {0};
    int t;

    for (t = 0; t < 50; ++t)
      if (waitpid(child, &status, WNOHANG) != 0)
        break;
      else
        usleep(10000);
    if (dirty)
      emit_frame();
    bfmt(&b, "x %d\n", WIFEXITED(status) ? WEXITSTATUS(status) : 128);
    broadcast(&b, 0);
    free(b.p);
    /* give lagging clients a moment to drain */
    for (i = 0; i < 100; ++i) {
      int left = 0, k;
      for (k = 0; k < MAX_CLIENTS; ++k) {
        client_flush(&cl[k]);
        if (cl[k].fd >= 0 && cl[k].outlen > cl[k].outpos)
          left = 1;
      }
      if (!left)
        break;
      usleep(10000);
    }
  }
  return 0;
}

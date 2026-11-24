/* uxxt-x11 - X11 frontend for uxxt-logic.
 * usage: uxxt-x11 <socket path>      (or $UXXT_SOCK)
 * protocol: see logic/uxxt-logic.c. clipboard, search, url opening etc. are
 * external tools configured in config.h. */
#define _GNU_SOURCE
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "../common/common.h"

#define DEF 0x01000000u /* "default color" marker in a cell */
#define A_BOLD 1
#define A_UL 2
#define A_REV 16
#define A_STRIKE 32
#define A_CONCEAL 64
#define A_WIDE 128
#define A_CONT 256 /* right half of a wide char */
#define MOD_USED (ShiftMask | ControlMask | Mod1Mask | Mod4Mask)

struct cell {
  uint32_t ch, fg, bg;
  unsigned short attr;
};

static Display *dpy;
static int scr;
static Window win;
static Pixmap pm;
static GC gc;
static Colormap cmap;
static XFontStruct *font;
static XIM xim;
static XIC xic;
static Atom wm_delete, net_wm_name, utf8_string;

static int sock = -1;
static int fw, fh, fa;       /* cell width, height, ascent */
static int W, H;             /* window pixels */
static int cols, rows;       /* grid we are drawing */
static struct cell *grid;
static char *rowdirty;
static uint32_t *ef, *eb;    /* per-row effective colors */
static uint32_t pal[256], FGRGB, BGRGB, CURRGB, SELFGRGB, SELBGRGB;

static int cur_x, cur_y, cur_vis = 1, cur_blink, cur_shape = 1;
static int view_off, focused = 1, blink_on = 1;
static long next_blink;

static int sel_on, sel0, sel1, sel_a0, sel_a1, sel_mode, dragging;

static char *inbuf;
static size_t inlen;

/* ---------- util ---------- */

static long now_ms(void) {
  struct timespec t;

  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static void die(const char *m) {
  perror(m);
  exit(1);
}

static void tx(const char *fmt, ...) {
  char t[256];
  va_list ap;
  int n;
  size_t off = 0;

  va_start(ap, fmt);
  n = vsnprintf(t, sizeof t - 1, fmt, ap);
  va_end(ap);
  t[n++] = '\n';
  while (off < (size_t)n) {
    ssize_t w = write(sock, t + off, (size_t)n - off);
    if (w < 0 && errno == EINTR)
      continue;
    if (w < 0)
      exit(0);
    off += (size_t)w;
  }
}

static void tx_in(const unsigned char *d, size_t n) {
  size_t len = 3 + 2 * n + 1, off = 0;
  char *s = malloc(len + 1);

  if (!s)
    return;
  memcpy(s, "in ", 3);
  uxxt_hexenc(d, n, s + 3);
  s[len - 1] = '\n';
  while (off < len) {
    ssize_t w = write(sock, s + off, len - off);
    if (w < 0 && errno == EINTR)
      continue;
    if (w < 0)
      exit(0);
    off += (size_t)w;
  }
  free(s);
}

static uint32_t u8dec(const char **p) {
  const unsigned char *s = (const unsigned char *)*p;
  uint32_t c;
  int n, i;

  if (s[0] < 0x80) {
    c = s[0];
    n = 1;
  } else if ((s[0] & 0xe0) == 0xc0) {
    c = s[0] & 0x1f;
    n = 2;
  } else if ((s[0] & 0xf0) == 0xe0) {
    c = s[0] & 0x0f;
    n = 3;
  } else if ((s[0] & 0xf8) == 0xf0) {
    c = s[0] & 0x07;
    n = 4;
  } else {
    *p += 1;
    return '?';
  }
  for (i = 1; i < n; ++i) {
    if ((s[i] & 0xc0) != 0x80) {
      *p += i;
      return '?';
    }
    c = c << 6 | (s[i] & 0x3f);
  }
  *p += n;
  return c;
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

/* ---------- colors ---------- */

static uint32_t parse_rgb(const char *s) {
  XColor c;

  if (!XParseColor(dpy, cmap, s, &c))
    return 0xff00ff;
  return (uint32_t)(((c.red >> 8) << 16) | ((c.green >> 8) << 8) | (c.blue >> 8));
}

static unsigned long pix(uint32_t rgb) {
  static struct {
    uint32_t key;
    unsigned long p;
  } cache[4096];
  uint32_t k = rgb | 0x80000000u, h = (rgb * 2654435761u) >> 20;
  XColor c;

  if (cache[h].key == k)
    return cache[h].p;
  memset(&c, 0, sizeof c);
  c.red = (unsigned short)(((rgb >> 16) & 255) * 257);
  c.green = (unsigned short)(((rgb >> 8) & 255) * 257);
  c.blue = (unsigned short)((rgb & 255) * 257);
  c.flags = DoRed | DoGreen | DoBlue;
  XAllocColor(dpy, cmap, &c);
  cache[h].key = k;
  cache[h].p = c.pixel;
  return c.pixel;
}

static void init_colors(void) {
  static const char *base[16] = PALETTE;
  static const int lv[6] = {0, 95, 135, 175, 215, 255};
  int i;

  for (i = 0; i < 16; ++i)
    pal[i] = parse_rgb(base[i]);
  for (i = 16; i < 232; ++i)
    pal[i] = (uint32_t)(lv[(i - 16) / 36] << 16 | lv[((i - 16) / 6) % 6] << 8 |
                        lv[(i - 16) % 6]);
  for (i = 232; i < 256; ++i) {
    int g = 8 + 10 * (i - 232);
    pal[i] = (uint32_t)(g << 16 | g << 8 | g);
  }
  FGRGB = parse_rgb(FG_COLOR);
  BGRGB = parse_rgb(BG_COLOR);
  CURRGB = parse_rgb(CURSOR_COLOR);
  SELFGRGB = parse_rgb(SEL_FG_COLOR);
  SELBGRGB = parse_rgb(SEL_BG_COLOR);
}

static uint32_t tok_color(const char *t) {
  if (t[0] == '-')
    return DEF;
  if (t[0] == 'i')
    return pal[atoi(t + 1) & 255];
  if (t[0] == '#')
    return (uint32_t)strtoul(t + 1, NULL, 16);
  return DEF;
}

/* ---------- grid + selection ---------- */

static void mark_all(void) {
  if (rowdirty)
    memset(rowdirty, 1, (size_t)rows);
}

static void clear_sel(void) {
  if (sel_on) {
    sel_on = 0;
    mark_all();
  }
}

static void alloc_grid(int c, int r) {
  int i;

  free(grid);
  free(rowdirty);
  free(ef);
  free(eb);
  cols = c;
  rows = r;
  grid = malloc(sizeof *grid * (size_t)c * (size_t)r);
  rowdirty = malloc((size_t)r);
  ef = malloc(sizeof *ef * (size_t)c);
  eb = malloc(sizeof *eb * (size_t)c);
  if (!grid || !rowdirty || !ef || !eb)
    die("malloc");
  for (i = 0; i < c * r; ++i) {
    grid[i].ch = ' ';
    grid[i].fg = grid[i].bg = DEF;
    grid[i].attr = 0;
  }
  memset(rowdirty, 1, (size_t)r);
  sel_on = 0;
  dragging = 0;
}

static int in_sel(int idx) { return sel_on && idx >= sel0 && idx <= sel1; }

static int char_class(uint32_t ch) {
  if (ch == ' ' || ch == 0)
    return ch == 0 ? 2 : 0;
  if (ch < 128 && strchr(WORD_DELIMS, (int)ch))
    return 1;
  return 2;
}

static void range_at(int mode, int x, int y, int *r0, int *r1) {
  int a = x, b = x;

  if (mode == 1) {
    *r0 = *r1 = y * cols + x;
    return;
  }
  if (mode == 3) {
    *r0 = y * cols;
    *r1 = y * cols + cols - 1;
    return;
  }
  {
    int cls = char_class(grid[y * cols + x].ch);
    while (a > 0 && char_class(grid[y * cols + a - 1].ch) == cls)
      --a;
    while (b < cols - 1 && char_class(grid[y * cols + b + 1].ch) == cls)
      ++b;
  }
  *r0 = y * cols + a;
  *r1 = y * cols + b;
}

static char *sel_text(size_t *outn) {
  struct { char *p; size_t n, cap; } b = {0, 0, 0};
  int y, y0 = sel0 / cols, y1 = sel1 / cols;

  b.cap = (size_t)(sel1 - sel0 + 2) * 4 + (size_t)(y1 - y0 + 2);
  b.p = malloc(b.cap);
  if (!b.p)
    return NULL;
  for (y = y0; y <= y1; ++y) {
    int xa = y == y0 ? sel0 % cols : 0, xb = y == y1 ? sel1 % cols : cols - 1, x;
    size_t start = b.n, lastnb = b.n;

    for (x = xa; x <= xb; ++x) {
      struct cell *c = &grid[y * cols + x];
      uint32_t ch = c->ch;
      if (c->attr & A_CONT)
        continue;
      b.n += (size_t)u8enc(ch ? ch : ' ', b.p + b.n);
      if (ch != ' ' && ch != 0)
        lastnb = b.n;
    }
    b.n = lastnb > start ? lastnb : start; /* trim trailing blanks */
    if (y < y1)
      b.p[b.n++] = '\n';
  }
  b.p[b.n] = 0;
  *outn = b.n;
  return b.p;
}

/* ---------- external tools ---------- */

static void on_sigchld(int s) {
  int e = errno;
  (void)s;
  while (waitpid(-1, NULL, WNOHANG) > 0)
    ;
  errno = e;
}

static void spawn_cmd(const char *cmd) {
  pid_t p = fork();

  if (p == 0) {
    setsid();
    execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
    _exit(127);
  }
}

static void pipe_to(const char *tool, const char *text, size_t n) {
  FILE *f = popen(tool, "w");

  if (!f)
    return;
  fwrite(text, 1, n, f);
  pclose(f);
}

static void copy_selection(const char *tool) {
  size_t n;
  char *t;

  if (!sel_on || !(t = sel_text(&n)))
    return;
  if (n)
    pipe_to(tool, t, n);
  free(t);
}

static void paste_from(const char *tool) {
  FILE *f = popen(tool, "r");
  unsigned char b[2048];
  size_t n, i;

  if (!f)
    return;
  while ((n = fread(b, 1, sizeof b, f)) > 0) {
    if (PASTE_NL_TO_CR)
      for (i = 0; i < n; ++i)
        if (b[i] == '\n')
          b[i] = '\r';
    tx_in(b, n);
  }
  pclose(f);
}

/* ---------- drawing ---------- */

static int cursor_shown(void) {
  return cur_vis && view_off == 0 && cur_y >= 0 && cur_y < rows && cur_x >= 0 &&
         cur_x < cols && (blink_on || !cur_blink || !CURSOR_BLINK_MS);
}

static void draw_row(int y) {
  int x, py = PAD_Y + y * fh, show = cursor_shown() && cur_y == y;
  int block = show && focused && cur_shape == 1;

  XSetForeground(dpy, gc, pix(BGRGB));
  XFillRectangle(dpy, pm, gc, 0, py, (unsigned)W, (unsigned)fh);

  for (x = 0; x < cols; ++x) {
    struct cell *c = &grid[y * cols + x];
    uint32_t f = c->fg == DEF ? FGRGB : c->fg, b = c->bg == DEF ? BGRGB : c->bg, t;

    if (c->attr & A_REV) {
      t = f;
      f = b;
      b = t;
    }
    if (in_sel(y * cols + x)) {
      f = SELFGRGB;
      b = SELBGRGB;
    }
    if (block && x == cur_x) {
      f = BGRGB;
      b = CURRGB;
    }
    ef[x] = f;
    eb[x] = b;
  }

  for (x = 0; x < cols;) {
    int j = x;
    while (j < cols && eb[j] == eb[x])
      ++j;
    if (eb[x] != BGRGB) {
      XSetForeground(dpy, gc, pix(eb[x]));
      XFillRectangle(dpy, pm, gc, PAD_X + x * fw, py, (unsigned)((j - x) * fw), (unsigned)fh);
    }
    x = j;
  }

  XSetFont(dpy, gc, font->fid);
  for (x = 0; x < cols; ++x) {
    struct cell *c = &grid[y * cols + x];
    int px = PAD_X + x * fw, cw = (c->attr & A_WIDE) ? 2 * fw : fw;

    if (c->attr & A_CONT)
      continue;
    if (c->ch > ' ' && !(c->attr & A_CONCEAL)) {
      XChar2b g;
      g.byte1 = (unsigned char)(c->ch > 0xffff ? 0 : c->ch >> 8);
      g.byte2 = (unsigned char)(c->ch > 0xffff ? '?' : c->ch & 255);
      XSetForeground(dpy, gc, pix(ef[x]));
      XDrawString16(dpy, pm, gc, px, py + fa, &g, 1);
      if ((c->attr & A_BOLD) && BOLD_OVERSTRIKE)
        XDrawString16(dpy, pm, gc, px + 1, py + fa, &g, 1);
    }
    if (c->attr & (A_UL | A_STRIKE)) {
      XSetForeground(dpy, gc, pix(ef[x]));
      if (c->attr & A_UL)
        XFillRectangle(dpy, pm, gc, px, py + fh - 1, (unsigned)cw, 1);
      if (c->attr & A_STRIKE)
        XFillRectangle(dpy, pm, gc, px, py + fh / 2, (unsigned)cw, 1);
    }
  }

  if (show) {
    int px = PAD_X + cur_x * fw, cw = (grid[y * cols + cur_x].attr & A_WIDE) ? 2 * fw : fw;
    XSetForeground(dpy, gc, pix(CURRGB));
    if (!focused) {
      if (CURSOR_UNFOCUSED_BOX)
        XDrawRectangle(dpy, pm, gc, px, py, (unsigned)(cw - 1), (unsigned)(fh - 1));
    } else if (cur_shape == 2) {
      XFillRectangle(dpy, pm, gc, px, py + fh - 2, (unsigned)cw, 2);
    } else if (cur_shape == 3) {
      XFillRectangle(dpy, pm, gc, px, py, 2, (unsigned)fh);
    }
  }
}

static void flush_draw(void) {
  int y, lo = -1, hi = -1;

  for (y = 0; y < rows; ++y)
    if (rowdirty[y]) {
      draw_row(y);
      rowdirty[y] = 0;
      if (lo < 0)
        lo = y;
      hi = y;
    }
  if (lo >= 0)
    XCopyArea(dpy, pm, win, gc, 0, PAD_Y + lo * fh, (unsigned)W,
              (unsigned)((hi - lo + 1) * fh), 0, PAD_Y + lo * fh);
  XFlush(dpy);
}

static void new_pixmap(void) {
  if (pm)
    XFreePixmap(dpy, pm);
  pm = XCreatePixmap(dpy, win, (unsigned)W, (unsigned)H, (unsigned)DefaultDepth(dpy, scr));
  XSetForeground(dpy, gc, pix(BGRGB));
  XFillRectangle(dpy, pm, gc, 0, 0, (unsigned)W, (unsigned)H);
}

static void set_title(const char *t) {
  XStoreName(dpy, win, t);
  XChangeProperty(dpy, win, net_wm_name, utf8_string, 8, PropModeReplace,
                  (const unsigned char *)t, (int)strlen(t));
}

/* ---------- protocol from logic ---------- */

static void put_span(int y, int x, uint32_t fg, uint32_t bg, int attr, const char *t) {
  while (*t && x < cols) {
    uint32_t ch = u8dec(&t);
    struct cell *c = &grid[y * cols + x];

    c->ch = ch;
    c->fg = fg;
    c->bg = bg;
    c->attr = (unsigned short)attr;
    if ((attr & A_WIDE) && x + 1 < cols) {
      struct cell *d = &grid[y * cols + x + 1];
      d->ch = 0;
      d->fg = fg;
      d->bg = bg;
      d->attr = (unsigned short)(attr | A_CONT);
      ++x;
    }
    ++x;
  }
}

static void on_logic_line(char *l) {
  int a, b, c, d, e, n;
  char fg[24], bg[24];

  switch (l[0]) {
  case 'g':
    if (sscanf(l, "g %d %d", &a, &b) == 2 && a > 0 && b > 0)
      alloc_grid(a, b);
    break;
  case 'r':
    if (sscanf(l, "r %d", &a) == 1 && a >= 0 && a < rows) {
      for (b = 0; b < cols; ++b) {
        grid[a * cols + b].ch = ' ';
        grid[a * cols + b].fg = grid[a * cols + b].bg = DEF;
        grid[a * cols + b].attr = 0;
      }
      rowdirty[a] = 1;
      if (SEL_CLEAR_ON_OUTPUT && sel_on && a >= sel0 / cols && a <= sel1 / cols)
        clear_sel();
    }
    break;
  case 's':
    n = 0;
    if (sscanf(l, "s %d %d %23s %23s %d%n", &a, &b, fg, bg, &c, &n) == 5 && n &&
        a >= 0 && a < rows && b >= 0 && b < cols)
      put_span(a, b, tok_color(fg), tok_color(bg), c, l + n + 1);
    break;
  case 'c':
    if (sscanf(l, "c %d %d %d %d %d", &a, &b, &c, &d, &e) == 5) {
      if (cur_y >= 0 && cur_y < rows)
        rowdirty[cur_y] = 1;
      cur_x = a;
      cur_y = b;
      cur_vis = c;
      cur_blink = d;
      cur_shape = e;
      if (cur_y >= 0 && cur_y < rows)
        rowdirty[cur_y] = 1;
      blink_on = 1;
      next_blink = now_ms() + CURSOR_BLINK_MS;
    }
    break;
  case 'o':
    if (sscanf(l, "o %d %d", &a, &b) == 2) {
      view_off = a;
      if (cur_y >= 0 && cur_y < rows)
        rowdirty[cur_y] = 1;
    }
    break;
  case 't':
    set_title(l[1] == ' ' ? l + 2 : l + 1);
    break;
  case 'b':
    if (BELL)
      XBell(dpy, 0);
    break;
  case 'f':
    flush_draw();
    break;
  case 'x':
    exit(0);
  default:
    break;
  }
}

static void on_sock(void) {
  char tmp[65536], *nl;
  ssize_t n = read(sock, tmp, sizeof tmp);
  size_t used = 0;

  if (n < 0 && (errno == EAGAIN || errno == EINTR))
    return;
  if (n <= 0)
    exit(0);
  inbuf = realloc(inbuf, inlen + (size_t)n + 1);
  if (!inbuf)
    die("realloc");
  memcpy(inbuf + inlen, tmp, (size_t)n);
  inlen += (size_t)n;
  inbuf[inlen] = 0;
  while ((nl = memchr(inbuf + used, '\n', inlen - used))) {
    *nl = 0;
    on_logic_line(inbuf + used);
    used = (size_t)(nl - inbuf) + 1;
  }
  memmove(inbuf, inbuf + used, inlen - used);
  inlen -= used;
}

/* ---------- X events ---------- */

static int mods_of(unsigned int s) {
  return ((s & ShiftMask) ? 1 : 0) | ((s & Mod1Mask) ? 2 : 0) | ((s & ControlMask) ? 4 : 0);
}

static const char *special_name(KeySym ks, char *buf) {
  switch (ks) {
  case XK_Return: case XK_KP_Enter: return "enter";
  case XK_Tab: case XK_ISO_Left_Tab: return "tab";
  case XK_BackSpace: return "backspace";
  case XK_Escape: return "escape";
  case XK_Up: return "up";
  case XK_Down: return "down";
  case XK_Left: return "left";
  case XK_Right: return "right";
  case XK_Insert: return "ins";
  case XK_Delete: return "del";
  case XK_Home: return "home";
  case XK_End: return "end";
  case XK_Prior: return "pageup";
  case XK_Next: return "pagedown";
  default:
    if (ks >= XK_F1 && ks <= XK_F35) {
      sprintf(buf, "f%d", (int)(ks - XK_F1 + 1));
      return buf;
    }
    return NULL;
  }
}

static int run_binding(const struct binding *b) {
  switch (b->act) {
  case ACT_COPY: copy_selection(COPY_TOOL); break;
  case ACT_PASTE: paste_from(PASTE_TOOL); break;
  case ACT_PASTE_PRIMARY: paste_from(PRIMARY_PASTE_TOOL); break;
  case ACT_SCROLL_LINES: tx("scroll %d", b->arg); break;
  case ACT_SCROLL_HALF: tx("scroll %d", b->arg * (rows / 2 ? rows / 2 : 1)); break;
  case ACT_SCROLL_TOP: tx("scroll 1000000000"); break;
  case ACT_SCROLL_BOTTOM: tx("scroll -1000000000"); break;
  case ACT_SPAWN: spawn_cmd(b->cmd); break;
  default: return 0;
  }
  return 1;
}

static void on_key(XKeyEvent *e) {
  char buf[64], nb[16];
  KeySym ks = NoSymbol, base;
  Status st = XLookupNone;
  const char *name;
  int n, mod = mods_of(e->state);
  size_t i;

  base = XLookupKeysym(e, 0);
  for (i = 0; i < sizeof bindings / sizeof *bindings; ++i)
    if (bindings[i].key == base && (e->state & MOD_USED) == bindings[i].mod) {
      run_binding(&bindings[i]);
      return;
    }

  if (xic) {
    n = Xutf8LookupString(xic, e, buf, sizeof buf - 1, &ks, &st);
    if (st != XLookupKeySym && st != XLookupBoth)
      ks = NoSymbol;
    if (st != XLookupChars && st != XLookupBoth)
      n = 0;
  } else {
    n = XLookupString(e, buf, sizeof buf - 1, &ks, NULL);
  }
  if (n < 0)
    n = 0;
  buf[n] = 0;

  blink_on = 1;
  next_blink = now_ms() + CURSOR_BLINK_MS;

  if (ks != NoSymbol && (name = special_name(ks, nb))) {
    tx("key %s %d", name, mod);
    return;
  }
  if ((mod & 4) && ks >= 0x20 && ks <= 0x7e) {
    tx("chr %lu %d", (unsigned long)ks, mod & ~1);
    return;
  }
  {
    const char *p = buf;
    while (*p) {
      uint32_t cp = u8dec(&p);
      if (cp >= 0x20 && cp != 0x7f)
        tx("chr %lu %d", (unsigned long)cp, mod & ~1);
    }
  }
}

static void cell_at(int ex, int ey, int *x, int *y) {
  *x = (ex - PAD_X) / fw;
  *y = (ey - PAD_Y) / fh;
  if (*x < 0) *x = 0;
  if (*y < 0) *y = 0;
  if (*x >= cols) *x = cols - 1;
  if (*y >= rows) *y = rows - 1;
}

static void on_button(XButtonEvent *e) {
  static long last;
  static int clicks, lx, ly;
  int x, y;
  long t = now_ms();

  if (e->button == Button4 || e->button == Button5) {
    if (e->type == ButtonPress)
      tx("scroll %d", e->button == Button4 ? WHEEL_LINES : -WHEEL_LINES);
    return;
  }
  if (e->button == Button2) {
    if (e->type == ButtonPress)
      paste_from(PRIMARY_PASTE_TOOL);
    return;
  }
  if (e->button != Button1)
    return;
  cell_at(e->x, e->y, &x, &y);

  if (e->type == ButtonPress) {
    clicks = (t - last < MULTI_CLICK_MS && x == lx && y == ly) ? clicks % 3 + 1 : 1;
    last = t;
    lx = x;
    ly = y;
    sel_mode = clicks;
    range_at(sel_mode, x, y, &sel_a0, &sel_a1);
    dragging = 1;
    sel_on = sel_mode > 1;
    sel0 = sel_a0;
    sel1 = sel_a1;
    mark_all();
    flush_draw();
  } else if (dragging) {
    dragging = 0;
    if (sel_on && COPY_ON_SELECT)
      copy_selection(PRIMARY_COPY_TOOL);
  }
}

static void on_motion(XMotionEvent *e) {
  int x, y, c0, c1, s0, s1;

  if (!dragging)
    return;
  cell_at(e->x, e->y, &x, &y);
  range_at(sel_mode, x, y, &c0, &c1);
  s0 = c0 < sel_a0 ? c0 : sel_a0;
  s1 = c1 > sel_a1 ? c1 : sel_a1;
  if (sel_mode == 1 && !sel_on && s0 == s1)
    return;
  if (!sel_on || s0 != sel0 || s1 != sel1) {
    sel_on = 1;
    sel0 = s0;
    sel1 = s1;
    mark_all();
    flush_draw();
  }
}

static void on_configure(XConfigureEvent *e) {
  int c, r;

  if (e->width == W && e->height == H)
    return;
  W = e->width;
  H = e->height;
  new_pixmap();
  mark_all();
  flush_draw();
  XCopyArea(dpy, pm, win, gc, 0, 0, (unsigned)W, (unsigned)H, 0, 0);
  c = (W - 2 * PAD_X) / fw;
  r = (H - 2 * PAD_Y) / fh;
  if (c < 1) c = 1;
  if (r < 1) r = 1;
  if (c != cols || r != rows)
    tx("resize %d %d", c, r);
}

static int handle_x_events(void) {
  XEvent ev;

  while (XPending(dpy)) {
    XNextEvent(dpy, &ev);
    if (XFilterEvent(&ev, None))
      continue;
    switch (ev.type) {
    case KeyPress: on_key(&ev.xkey); break;
    case ButtonPress:
    case ButtonRelease: on_button(&ev.xbutton); break;
    case MotionNotify: on_motion(&ev.xmotion); break;
    case ConfigureNotify: on_configure(&ev.xconfigure); break;
    case Expose:
      XCopyArea(dpy, pm, win, gc, ev.xexpose.x, ev.xexpose.y, (unsigned)ev.xexpose.width,
                (unsigned)ev.xexpose.height, ev.xexpose.x, ev.xexpose.y);
      break;
    case FocusIn:
      focused = 1;
      if (xic) XSetICFocus(xic);
      if (cur_y >= 0 && cur_y < rows) rowdirty[cur_y] = 1;
      flush_draw();
      break;
    case FocusOut:
      focused = 0;
      if (xic) XUnsetICFocus(xic);
      if (cur_y >= 0 && cur_y < rows) rowdirty[cur_y] = 1;
      flush_draw();
      break;
    case ClientMessage:
      if ((Atom)ev.xclient.data.l[0] == wm_delete) {
        if (KILL_ON_CLOSE)
          tx("quit");
        return 0;
      }
      break;
    default: break;
    }
  }
  return 1;
}

/* ---------- main ---------- */

static void open_window(void) {
  XSetWindowAttributes a;
  XSizeHints *sh;
  XClassHint ch;
  long mask;

  cmap = DefaultColormap(dpy, scr);
  init_colors();

  font = XLoadQueryFont(dpy, FONT);
  if (!font)
    font = XLoadQueryFont(dpy, FONT_FALLBACK);
  if (!font) {
    fprintf(stderr, "uxxt-x11: no font\n");
    exit(1);
  }
  fw = font->max_bounds.width;
  fh = font->ascent + font->descent;
  fa = font->ascent;
  if (fw < 1) fw = 1;

  W = WIN_COLS * fw + 2 * PAD_X;
  H = WIN_ROWS * fh + 2 * PAD_Y;
  a.background_pixel = pix(BGRGB);
  a.event_mask = KeyPressMask | ButtonPressMask | ButtonReleaseMask | ButtonMotionMask |
                 ExposureMask | StructureNotifyMask | FocusChangeMask;
  win = XCreateWindow(dpy, RootWindow(dpy, scr), 0, 0, (unsigned)W, (unsigned)H, 0,
                      CopyFromParent, InputOutput, CopyFromParent, CWBackPixel | CWEventMask, &a);
  gc = XCreateGC(dpy, win, 0, NULL);

  XSetLocaleModifiers("");
  xim = XOpenIM(dpy, NULL, NULL, NULL);
  if (xim) {
    xic = XCreateIC(xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                    XNClientWindow, win, XNFocusWindow, win, NULL);
    if (xic) {
      XGetICValues(xic, XNFilterEvents, &mask, NULL);
      XSelectInput(dpy, win, a.event_mask | mask);
      XSetICFocus(xic);
    }
  }

  sh = XAllocSizeHints();
  sh->flags = PResizeInc | PBaseSize | PMinSize;
  sh->width_inc = fw;
  sh->height_inc = fh;
  sh->base_width = 2 * PAD_X;
  sh->base_height = 2 * PAD_Y;
  sh->min_width = sh->base_width + fw;
  sh->min_height = sh->base_height + fh;
  ch.res_name = WIN_CLASS_NAME;
  ch.res_class = WIN_CLASS;
  XSetWMProperties(dpy, win, NULL, NULL, NULL, 0, sh, NULL, &ch);
  XFree(sh);

  net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);
  utf8_string = XInternAtom(dpy, "UTF8_STRING", False);
  wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
  XSetWMProtocols(dpy, win, &wm_delete, 1);
  set_title(WIN_TITLE);

  new_pixmap();
  alloc_grid(WIN_COLS, WIN_ROWS);
  XMapWindow(dpy, win);
  fcntl(ConnectionNumber(dpy), F_SETFD, FD_CLOEXEC);
}

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : getenv(SOCKET_ENV);
  struct sigaction sa;
  int i;

  if (!path || !*path) {
    fprintf(stderr, "usage: uxxt-x11 <socket>   (or set $%s)\n", SOCKET_ENV);
    return 2;
  }
  setlocale(LC_CTYPE, "");
  signal(SIGPIPE, SIG_IGN);
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_sigchld;
  sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
  sigaction(SIGCHLD, &sa, NULL);

  dpy = XOpenDisplay(NULL);
  if (!dpy) {
    fprintf(stderr, "uxxt-x11: cannot open display\n");
    return 1;
  }
  scr = DefaultScreen(dpy);
  open_window();

  for (i = 0; i < CONNECT_RETRIES; ++i) {
    sock = uxxt_connect(path);
    if (sock >= 0)
      break;
    usleep(CONNECT_RETRY_MS * 1000);
  }
  if (sock < 0) {
    fprintf(stderr, "uxxt-x11: cannot connect to %s\n", path);
    return 1;
  }
  fcntl(sock, F_SETFD, FD_CLOEXEC);
  setenv(SOCKET_ENV, path, 1);
  tx("attach %d %d", cols, rows);

  next_blink = now_ms() + CURSOR_BLINK_MS;
  for (;;) {
    struct pollfd p[2];
    int timeout = -1;

    if (!handle_x_events())
      return 0;
    p[0].fd = ConnectionNumber(dpy);
    p[0].events = POLLIN;
    p[1].fd = sock;
    p[1].events = POLLIN;
    if (CURSOR_BLINK_MS > 0 && cur_blink && cur_vis && view_off == 0) {
      long d = next_blink - now_ms();
      timeout = d < 0 ? 0 : (int)d;
    }
    if (poll(p, 2, timeout) < 0 && errno != EINTR)
      die("poll");
    if (CURSOR_BLINK_MS > 0 && cur_blink && now_ms() >= next_blink) {
      blink_on = !blink_on;
      next_blink = now_ms() + CURSOR_BLINK_MS;
      if (cur_y >= 0 && cur_y < rows)
        rowdirty[cur_y] = 1;
      flush_draw();
    }
    if (p[1].revents & (POLLIN | POLLHUP))
      on_sock();
  }
}

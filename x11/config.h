/* x11/config.h - uxxt-x11: X11 only. draws what uxxt-logic sends, sends
 * keys and mouse back. no pty, no escape sequences, no clipboard code:
 * everything else is an external tool below.
 * included after <X11/keysym.h>, so keysyms and masks work here. */

/* appearance */
#define FONT             "-misc-fixed-medium-r-normal--15-*-*-*-c-*-iso10646-1"
#define FONT_FALLBACK    "fixed"
#define FG_COLOR         "#d8d8d8"
#define BG_COLOR         "#101010"
#define CURSOR_COLOR     "#ffffff"
#define SEL_FG_COLOR     "#000000"
#define SEL_BG_COLOR     "#80a0ff"
#define PALETTE { \
  "#000000", "#cd0000", "#00cd00", "#cdcd00", "#0000ee", "#cd00cd", "#00cdcd", "#e5e5e5", \
  "#7f7f7f", "#ff0000", "#00ff00", "#ffff00", "#5c5cff", "#ff00ff", "#00ffff", "#ffffff" }
#define BOLD_OVERSTRIKE  1       /* bold = draw the glyph twice, 1px apart */
#define PAD_X            2
#define PAD_Y            2

/* window */
#define WIN_TITLE        "uxxt"
#define WIN_CLASS_NAME   "uxxt"
#define WIN_CLASS        "Uxxt"
#define WIN_COLS         80
#define WIN_ROWS         24

/* cursor */
#define CURSOR_BLINK_MS  600     /* 0 = never blink; blinks only if the program asks */
#define CURSOR_UNFOCUSED_BOX 1   /* hollow box when the window has no focus */

/* misc behaviour */
#define BELL             1       /* XBell */
#define WHEEL_LINES      3
#define MULTI_CLICK_MS   350     /* double click = word, triple click = line */
#define WORD_DELIMS      " \t\"'`()[]{}<>|;,=&"
#define COPY_ON_SELECT   1       /* run PRIMARY_COPY_TOOL when a selection ends */
#define SEL_CLEAR_ON_OUTPUT 1    /* new output on a selected row drops the selection */
#define PASTE_NL_TO_CR   1       /* send \n as \r when pasting, like xterm */
#define KILL_ON_CLOSE    0       /* 0 = closing the window detaches (uxxt brings it back); 1 = ends the shell */

/* connection */
#define SOCKET_ENV       "UXXT_SOCK"   /* exported to every tool started from here */
#define CONNECT_RETRIES  50
#define CONNECT_RETRY_MS 40

/* tools: each one is a shell command. swap them for whatever you like. */
#define COPY_TOOL          "xclip -selection clipboard -in"
#define PASTE_TOOL         "xclip -selection clipboard -out"
#define PRIMARY_COPY_TOOL  "xclip -selection primary -in"
#define PRIMARY_PASTE_TOOL "xclip -selection primary -out"

#define BUFFER_TOOL        "uxxt-buffer"
#define PROMPT_TOOL      "dmenu -p search:"
#define SEARCH_TOOL      "grep"
#define SEARCH_ARGS      "-a -i -F"
#define PICK_TOOL        "dmenu -i -l 20"
#define URL_TOOL         "grep -a -o -E 'https?://[^ \"<>)]+'"
#define OPEN_TOOL        "xdg-open"

/* prompt for a pattern, grep the whole history, pick a line, copy it */
#define SEARCH_CMD \
  "pat=$(" PROMPT_TOOL " </dev/null) && [ -n \"$pat\" ] && " \
  BUFFER_TOOL " | " SEARCH_TOOL " " SEARCH_ARGS " -e \"$pat\" | " PICK_TOOL " | " COPY_TOOL

/* list every url in the history, pick one, open it */
#define URL_CMD \
  BUFFER_TOOL " | " URL_TOOL " | " PICK_TOOL " | xargs -r " OPEN_TOOL

/* key bindings. key is the unshifted keysym, mod is an exact modifier match
 * (ShiftMask ControlMask Mod1Mask Mod4Mask). ACT_SPAWN runs cmd with sh -c. */
enum { ACT_COPY, ACT_PASTE, ACT_PASTE_PRIMARY, ACT_SCROLL_LINES,
       ACT_SCROLL_HALF, ACT_SCROLL_TOP, ACT_SCROLL_BOTTOM, ACT_SPAWN };

struct binding {
  KeySym key;
  unsigned int mod;
  int act;
  int arg;
  const char *cmd;
};

static const struct binding bindings[] = {
  { XK_c,      ControlMask | ShiftMask, ACT_COPY,          0, NULL },
  { XK_v,      ControlMask | ShiftMask, ACT_PASTE,         0, NULL },
  { XK_Insert, ShiftMask,               ACT_PASTE_PRIMARY, 0, NULL },
  { XK_Prior,  ShiftMask,               ACT_SCROLL_HALF,   1, NULL },
  { XK_Next,   ShiftMask,               ACT_SCROLL_HALF,  -1, NULL },
  { XK_Up,     ShiftMask,               ACT_SCROLL_LINES,  1, NULL },
  { XK_Down,   ShiftMask,               ACT_SCROLL_LINES, -1, NULL },
  { XK_Home,   ShiftMask,               ACT_SCROLL_TOP,    0, NULL },
  { XK_End,    ShiftMask,               ACT_SCROLL_BOTTOM, 0, NULL },
  { XK_f,      ControlMask | ShiftMask, ACT_SPAWN,         0, SEARCH_CMD },
  { XK_u,      ControlMask | ShiftMask, ACT_SPAWN,         0, URL_CMD },
};

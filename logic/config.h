/* logic/config.h - uxxt-logic: pty + libvterm + scrollback + socket.
 * knows nothing about X11, fonts or colors. */

/* child */
#define DEFAULT_SHELL     "/bin/sh"   /* used when $SHELL is unset */
#define TERM_NAME         "xterm-256color"
#define COLORTERM         "truecolor"
#define KILL_SIGNAL       SIGHUP      /* sent to the child on "quit" */

/* grid used until a client attaches */
#define DEFAULT_COLS      80
#define DEFAULT_ROWS      24

/* scrollback */
#define SCROLLBACK_LINES  10000
#define SNAP_ON_INPUT     1           /* keypress jumps back to the bottom */
#define SNAP_ON_OUTPUT    0           /* new output jumps back to the bottom */

/* socket: $SOCKET_DIR_ENV/SOCKET_PREFIX<pid>.sock, or -s <path> */
#define SOCKET_DIR_ENV    "XDG_RUNTIME_DIR"
#define SOCKET_FALLBACK   "/tmp"
#define SOCKET_PREFIX     "uxxt-"
#define MAX_CLIENTS       8
#define CLIENT_QUEUE_MAX  (16 << 20)  /* drop a client that lags this far */
#define LINE_MAX_BYTES    (1 << 18)   /* longest accepted command line */

/* io and frame pacing */
#define READ_SIZE         65536
#define FRAME_MS          8           /* wait this long for more output before drawing */
#define FLUSH_BYTES       262144      /* draw anyway after this much output */

/* "buffer" */
#define BUFFER_TRIM         1           /* strip trailing blanks and trailing empty lines */

/* keys/config.h - uxxt-keys: send keys or text to a running uxxt-logic. */

#define SOCKET_ENV    "UXXT_SOCK"   /* socket path: -s <path>, or this variable */

/* modifier prefixes, combinable: C-c  A-x  M-x  C-A-Left */
#define PREFIX_CTRL   'C'
#define PREFIX_ALT    'A'
#define PREFIX_ALT2   'M'
#define PREFIX_SHIFT  'S'

/* key names uxxt-logic understands (f1..f35 are handled separately) */
static const char *const key_names[] = {
  "enter", "tab", "backspace", "escape", "up", "down", "left", "right",
  "ins", "del", "home", "end", "pageup", "pagedown",
};

/* aliases: either a key name, or (key == NULL) a single character */
static const struct {
  const char *alias;
  const char *key;
  unsigned int cp;
} aliases[] = {
  { "return", "enter",     0 },
  { "cr",     "enter",     0 },
  { "esc",    "escape",    0 },
  { "bs",     "backspace", 0 },
  { "insert", "ins",       0 },
  { "delete", "del",       0 },
  { "pgup",   "pageup",    0 },
  { "pgdn",   "pagedown",  0 },
  { "space",  NULL,      ' ' },
};

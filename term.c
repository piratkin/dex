#include "term.h"
#include "common.h"
#include "editor.h"
#include "options.h"

#undef CTRL

#include <sys/ioctl.h>
#include <termios.h>

struct keymap {
	enum term_key_type type;
	unsigned int key;
	const char *code;
	unsigned int terms;
};

enum {
	T_RXVT = 1,
	T_SCREEN = 2,
	T_ST = 4,
	T_XTERM = 8,
	T_ALL = 16 - 1,
};

// prefixes, st-256color matches st
const char *terms[] = {
	"rxvt",
	"screen",
	"st",
	"xterm",
};

static const struct keymap builtin_keys[] = {
	// ansi
	{ KEY_SPECIAL, SKEY_LEFT,	"\033[D", T_ALL },
	{ KEY_SPECIAL, SKEY_RIGHT,	"\033[C", T_ALL },
	{ KEY_SPECIAL, SKEY_UP,		"\033[A", T_ALL },
	{ KEY_SPECIAL, SKEY_DOWN,	"\033[B", T_ALL },

	// ???
	{ KEY_SPECIAL, SKEY_HOME,	"\033[1~", T_ALL },
	{ KEY_SPECIAL, SKEY_END,	"\033[4~", T_ALL },

	// fix keypad when numlock is off
	{ KEY_NORMAL, '/',		"\033Oo", T_ALL },
	{ KEY_NORMAL, '*',		"\033Oj", T_ALL },
	{ KEY_NORMAL, '-',		"\033Om", T_ALL },
	{ KEY_NORMAL, '+',		"\033Ok", T_ALL },
	{ KEY_NORMAL, '\r',		"\033OM", T_ALL },

	{ KEY_SPECIAL, SKEY_SHIFT_LEFT,	"\033[d", T_RXVT },
	{ KEY_SPECIAL, SKEY_SHIFT_RIGHT,"\033[c", T_RXVT },
	{ KEY_SPECIAL, SKEY_SHIFT_UP,	"\033[a", T_RXVT },
	{ KEY_SPECIAL, SKEY_SHIFT_DOWN,	"\033[b", T_RXVT },
	{ KEY_SPECIAL, SKEY_SHIFT_UP,	"\033[1;2A", T_SCREEN | T_ST | T_XTERM },
	{ KEY_SPECIAL, SKEY_SHIFT_DOWN,	"\033[1;2B", T_SCREEN | T_ST | T_XTERM },
	{ KEY_SPECIAL, SKEY_SHIFT_LEFT,	"\033[1;2D", T_SCREEN | T_ST | T_XTERM },
	{ KEY_SPECIAL, SKEY_SHIFT_RIGHT,"\033[1;2C", T_SCREEN | T_ST | T_XTERM },
};

struct term_cap term_cap;

static struct termios termios_save;
static char buffer[64];
static int buffer_pos;
static unsigned int term_flags; // T_*

static void buffer_num(unsigned int num)
{
	char stack[32];
	int pos = 0;

	do {
		stack[pos++] = (num % 10) + '0';
		num /= 10;
	} while (num);
	do {
		buffer[buffer_pos++] = stack[--pos];
	} while (pos);
}

static char *escape_key(const char *key, int len)
{
	static char buf[1024];
	int i, j = 0;

	for (i = 0; i < len && i < sizeof(buf) - 1; i++) {
		unsigned char ch = key[i];

		if (ch < 0x20) {
			buf[j++] = '^';
			ch |= 0x40;
		} else if (ch == 0x7f) {
			buf[j++] = '^';
			ch = '?';
		}
		buf[j++] = ch;
	}
	buf[j] = 0;
	return buf;
}

static int load_terminfo_caps(const char *path, const char *term)
{
	char filename[512];

	snprintf(filename, sizeof(filename), "%s/%c/%s", path, term[0], term);
	return terminfo_get_caps(filename);
}

int read_terminfo(const char *term)
{
	static const char *paths[] = {
		NULL, // $HOME/.terminfo
		"/etc/terminfo",
		"/lib/terminfo",
		"/usr/share/terminfo",
		"/usr/share/lib/terminfo",
		"/usr/lib/terminfo",
		"/usr/local/share/terminfo",
		"/usr/local/lib/terminfo",
	};
	const char *path = getenv("TERMINFO");
	char buf[1024];
	int i, rc = 0;

	if (path && *path)
		return load_terminfo_caps(path, term);

	snprintf(buf, sizeof(buf), "%s/.terminfo", home_dir);
	paths[0] = buf;
	for (i = 0; i < ARRAY_COUNT(paths); i++) {
		rc = load_terminfo_caps(paths[i], term);
		if (!rc)
			return 0;
	}
	return rc;
}

void term_setup_extra_keys(const char *term)
{
	int i;

	for (i = 0; i < ARRAY_COUNT(terms); i++) {
		if (str_has_prefix(term, terms[i])) {
			term_flags = 1 << i;
			break;
		}
	}
}

void term_raw(void)
{
	/* see termios(3) */
	struct termios termios;

	tcgetattr(0, &termios);
	termios_save = termios;

	/* disable buffering
	 * disable echo
	 * disable generation of signals (free some control keys)
	 */
	termios.c_lflag &= ~(ICANON | ECHO | ISIG);

	/* disable CR to NL conversion (differentiate ^J from enter)
	 * disable flow control (free ^Q and ^S)
	 */
	termios.c_iflag &= ~(ICRNL | IXON | IXOFF);

	/* read at least 1 char on each read() */
	termios.c_cc[VMIN] = 1;

	/* read blocks until there are MIN(VMIN, requested) bytes available */
	termios.c_cc[VTIME] = 0;

	tcsetattr(0, 0, &termios);
}

void term_cooked(void)
{
	tcsetattr(0, 0, &termios_save);
}

static char input_buf[256];
static int input_buf_fill;
static bool input_can_be_truncated;

static void consume_input(int len)
{
	input_buf_fill -= len;
	if (input_buf_fill) {
		memmove(input_buf, input_buf + len, input_buf_fill);

		/* keys are sent faster than we can read */
		input_can_be_truncated = true;
	}
}

static bool fill_buffer(void)
{
	int rc;

	if (input_buf_fill == sizeof(input_buf))
		return false;

	if (!input_buf_fill)
		input_can_be_truncated = false;

	rc = read(0, input_buf + input_buf_fill, sizeof(input_buf) - input_buf_fill);
	if (rc <= 0)
		return false;
	input_buf_fill += rc;
	return true;
}

static bool fill_buffer_timeout(void)
{
	struct timeval tv = {
		.tv_sec = options.esc_timeout / 1000,
		.tv_usec = (options.esc_timeout % 1000) * 1000
	};
	fd_set set;
	int rc;

	FD_ZERO(&set);
	FD_SET(0, &set);
	rc = select(1, &set, NULL, NULL, &tv);
	if (rc > 0 && fill_buffer())
		return true;
	return false;
}

static bool input_get_byte(unsigned char *ch)
{
	if (!input_buf_fill && !fill_buffer())
		return false;
	*ch = input_buf[0];
	consume_input(1);
	return true;
}

static const struct keymap *find_key(bool *possibly_truncated)
{
	int i;

	for (i = 0; i < ARRAY_COUNT(builtin_keys); i++) {
		const struct keymap *entry = &builtin_keys[i];
		int len;

		if ((entry->terms & term_flags) == 0) {
			continue;
		}
		len = strlen(entry->code);
		if (len > input_buf_fill) {
			if (!memcmp(entry->code, input_buf, input_buf_fill)) {
				*possibly_truncated = true;
			}
		} else if (strncmp(entry->code, input_buf, len) == 0) {
			return entry;
		}
	}
	return NULL;
}

static bool read_special(unsigned int *key, enum term_key_type *type)
{
	const struct keymap *entry;
	bool possibly_truncated = false;
	int i;

	if (DEBUG > 2) {
		d_print("key: '%s'\n", escape_key(input_buf, input_buf_fill));
	}
	for (i = 0; i < NR_KEY_CAPS; i++) { // NOTE: not NR_SKEYS
		const char *keycode = term_cap.keys[i];
		int len;

		if (!keycode)
			continue;

		len = strlen(keycode);
		if (len > input_buf_fill) {
			/* this might be a truncated escape sequence */
			if (!memcmp(keycode, input_buf, input_buf_fill))
				possibly_truncated = true;
			continue;
		}
		if (strncmp(keycode, input_buf, len))
			continue;
		*key = i;
		*type = KEY_SPECIAL;
		consume_input(len);
		return true;
	}
	entry = find_key(&possibly_truncated);
	if (entry != NULL) {
		*key = entry->key;
		*type = entry->type;
		consume_input(strlen(entry->code));
		return true;
	}

	if (possibly_truncated && input_can_be_truncated && fill_buffer())
		return read_special(key, type);
	return false;
}

static bool read_simple(unsigned int *key, enum term_key_type *type)
{
	unsigned char ch = 0;

	/* > 0 bytes in buf */
	input_get_byte(&ch);

	/* normal key */
	if (term_utf8 && ch > 0x7f) {
		/*
		 * 10xx xxxx invalid
		 * 110x xxxx valid
		 * 1110 xxxx valid
		 * 1111 0xxx valid
		 * 1111 1xxx invalid
		 */
		unsigned int u, bit = 1 << 6;
		int count = 0;

		while (ch & bit) {
			bit >>= 1;
			count++;
		}
		if (count == 0 || count > 3) {
			/* invalid first byte */
			return false;
		}
		u = ch & (bit - 1);
		do {
			if (!input_get_byte(&ch))
				return false;
			if (ch >> 6 != 2)
				return false;
			u = (u << 6) | (ch & 0x3f);
		} while (--count);
		*key = u;
	} else {
		*key = ch;
	}
	*type = KEY_NORMAL;
	return true;
}

static bool is_text(const char *str, int len)
{
	int i;

	for (i = 0; i < len; i++) {
		// NOTE: must be unsigned!
		unsigned char ch = str[i];

		switch (ch) {
		case '\t':
		case '\n':
		case '\r':
			break;
		default:
			if (ch < 0x20 || ch == 0x7f)
				return false;
		}
	}
	return true;
}

bool term_read_key(unsigned int *key, enum term_key_type *type)
{
	if (!input_buf_fill && !fill_buffer())
		return false;

	if (input_buf_fill > 4 && is_text(input_buf, input_buf_fill)) {
		*key = 0;
		*type = KEY_PASTE;
		return true;
	}

	if (input_buf[0] == '\033') {
		if (input_buf_fill > 1 || input_can_be_truncated) {
			if (read_special(key, type))
				return true;
		}
		if (input_buf_fill == 1) {
			/* sometimes alt-key gets split into two reads */
			fill_buffer_timeout();

			if (input_buf_fill > 1 && input_buf[1] == '\033') {
				/*
				 * Double-esc (+ maybe some other characters)
				 *
				 * Treat the first esc as a single key to make
				 * things like arrow keys work immediately after
				 * leaving (esc) the command line.
				 *
				 * Special key can't start with double-esc so this
				 * should be safe.
				 *
				 * This breaks the esc-key == alt-key rule for the
				 * esc-esc case but it shouldn't matter.
				 */
				return read_simple(key, type);
			}
		}
		if (input_buf_fill > 1) {
			if (input_buf_fill == 2 || input_buf[2] == '\033') {
				/* 'esc key' or 'alt-key' */
				*key = input_buf[1];
				*type = KEY_META;
				consume_input(2);
				return true;
			}
			/* unknown escape sequence, avoid inserting it */
			input_buf_fill = 0;
			return false;
		}
	}
	return read_simple(key, type);
}

char *term_read_paste(long *size)
{
	long alloc = ROUND_UP(input_buf_fill + 1, 1024);
	long count = 0;
	long i;
	char *buf = xmalloc(alloc);

	if (input_buf_fill) {
		memcpy(buf, input_buf, input_buf_fill);
		count = input_buf_fill;
		input_buf_fill = 0;
	}
	while (1) {
		struct timeval tv = {
			.tv_sec = 0,
			.tv_usec = 0
		};
		fd_set set;
		int rc;

		FD_ZERO(&set);
		FD_SET(0, &set);
		rc = select(1, &set, NULL, NULL, &tv);
		if (rc < 0 && errno == EINTR)
			continue;
		if (rc <= 0)
			break;

		if (alloc - count < 256) {
			alloc *= 2;
			xrenew(buf, alloc);
		}
		do {
			rc = read(0, buf + count, alloc - count);
		} while (rc < 0 && errno == EINTR);
		if (rc <= 0)
			break;
		count += rc;
	}
	for (i = 0; i < count; i++) {
		if (buf[i] == '\r')
			buf[i] = '\n';
	}
	*size = count;
	return buf;
}

void term_discard_paste(void)
{
	long size;
	free(term_read_paste(&size));
}

int term_get_size(int *w, int *h)
{
	struct winsize ws;

	if (ioctl(0, TIOCGWINSZ, &ws) != -1) {
		*w = ws.ws_col;
		*h = ws.ws_row;
		return 0;
	}
	return -1;
}

static void buffer_color(char x, unsigned char color)
{
	buffer[buffer_pos++] = ';';
	buffer[buffer_pos++] = x;
	if (color < 8) {
		buffer[buffer_pos++] = '0' + color;
	} else {
		buffer[buffer_pos++] = '8';
		buffer[buffer_pos++] = ';';
		buffer[buffer_pos++] = '5';
		buffer[buffer_pos++] = ';';
		buffer_num(color);
	}
}

const char *term_set_color(const struct term_color *color)
{
	struct term_color c = *color;

	// TERM=xterm: 8 colors
	// TERM=linux: 8 colors. colors > 7 corrupt screen
	if (term_cap.colors < 16 && c.fg >= 8 && c.fg <= 15) {
		c.attr |= ATTR_BOLD;
		c.fg &= 7;
	}

	// max 35 bytes (3 + 6 * 2 + 2 * 9 + 2)
	buffer_pos = 0;
	buffer[buffer_pos++] = '\033';
	buffer[buffer_pos++] = '[';
	buffer[buffer_pos++] = '0';

	if (c.attr & ATTR_BOLD) {
		buffer[buffer_pos++] = ';';
		buffer[buffer_pos++] = '1';
	}
	if (c.attr & ATTR_LOW_INTENSITY) {
		buffer[buffer_pos++] = ';';
		buffer[buffer_pos++] = '2';
	}
	if (c.attr & ATTR_UNDERLINE) {
		buffer[buffer_pos++] = ';';
		buffer[buffer_pos++] = '4';
	}
	if (c.attr & ATTR_BLINKING) {
		buffer[buffer_pos++] = ';';
		buffer[buffer_pos++] = '5';
	}
	if (c.attr & ATTR_REVERSE_VIDEO) {
		buffer[buffer_pos++] = ';';
		buffer[buffer_pos++] = '7';
	}
	if (c.attr & ATTR_INVISIBLE_TEXT) {
		buffer[buffer_pos++] = ';';
		buffer[buffer_pos++] = '8';
	}
	if (c.fg >= 0)
		buffer_color('3', c.fg);
	if (c.bg >= 0)
		buffer_color('4', c.bg);
	buffer[buffer_pos++] = 'm';
	buffer[buffer_pos++] = 0;
	return buffer;
}

const char *term_move_cursor(int x, int y)
{
	if (x < 0 || x >= 999 || y < 0 || y >= 999)
		return "";

	x++;
	y++;
	// max 11 bytes
	buffer_pos = 0;
	buffer[buffer_pos++] = '\033';
	buffer[buffer_pos++] = '[';
	buffer_num(y);
	buffer[buffer_pos++] = ';';
	buffer_num(x);
	buffer[buffer_pos++] = 'H';
	buffer[buffer_pos++] = 0;
	return buffer;
}

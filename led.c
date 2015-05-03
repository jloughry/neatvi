#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "vi.h"
#include "kmap.h"

static char **led_kmap = kmap_def;

static char *keymap(char **kmap, int c)
{
	static char cs[4];
	cs[0] = c;
	return kmap[c] ? kmap[c] : cs;
}

char *led_keymap(int c)
{
	return c >= 0 ? keymap(led_kmap, c) : NULL;
}

void led_print(char *s, int row)
{
	char *r = ren_all(s, -1);
	term_pos(row, 0);
	term_kill();
	term_str(r);
	free(r);
}

static int led_lastchar(char *s)
{
	char *r = *s ? strchr(s, '\0') : s;
	if (r != s)
		r = uc_beg(s, r - 1);
	return r - s;
}

static void led_printparts(char *pref, char *main, char *post)
{
	struct sbuf *ln;
	int col;
	int cur;
	int dir;
	ln = sbuf_make();
	sbuf_str(ln, pref);
	sbuf_str(ln, main);
	cur = uc_slen(sbuf_buf(ln)) - 1;
	dir = uc_dir(sbuf_buf(ln) + led_lastchar(sbuf_buf(ln)));
	sbuf_str(ln, post);
	led_print(sbuf_buf(ln), -1);
	col = ren_cursor(sbuf_buf(ln), ren_pos(sbuf_buf(ln), MAX(cur, 0)));
	if (cur >= 0) {
		if (dir < 0 || (!dir && ren_dir(sbuf_buf(ln)) < 0))
			col = MAX(col - 1, 0);
		else
			col += 1;
	}
	term_pos(-1, col);
	sbuf_free(ln);
}

static char *led_line(char *pref, char *post, int *key, char ***kmap)
{
	struct sbuf *sb;
	int c;
	sb = sbuf_make();
	if (!pref)
		pref = "";
	if (!post)
		post = "";
	while (1) {
		led_printparts(pref, sbuf_buf(sb), post);
		c = term_read(-1);
		switch (c) {
		case TERMCTRL('f'):
			*kmap = *kmap == kmap_def ? kmap_farsi : kmap_def;
			continue;
		case TERMCTRL('h'):
		case 127:
			if (sbuf_len(sb))
				sbuf_cut(sb, led_lastchar(sbuf_buf(sb)));
			break;
		case TERMCTRL('u'):
			sbuf_cut(sb, 0);
			break;
		case TERMCTRL('v'):
			sbuf_chr(sb, term_read(-1));
			break;
		default:
			if (c == '\n' || c == TERMESC || c < 0)
				break;
			sbuf_str(sb, keymap(*kmap, c));
		}
		if (c == '\n' || c == TERMESC || c < 0)
			break;
	}
	*key = c;
	return sbuf_done(sb);
}

/* read an ex command */
char *led_prompt(char *pref, char *post)
{
	char **kmap = kmap_def;
	char *s;
	int key;
	s = led_line(pref, post, &key, &kmap);
	if (key == '\n')
		return s;
	free(s);
	return NULL;
}

/* read visual command input */
char *led_input(char *pref, char *post, int *row, int *col)
{
	struct sbuf *sb = sbuf_make();
	int key;
	*row = 0;
	while (1) {
		char *ln = led_line(pref, post, &key, &led_kmap);
		if (pref)
			sbuf_str(sb, pref);
		sbuf_str(sb, ln);
		if (key == '\n')
			sbuf_chr(sb, '\n');
		*col = ren_last(pref ? sbuf_buf(sb) : ln);
		led_printparts(pref ? pref : "", ln, key == '\n' ? "" : post);
		if (key == '\n')
			term_chr('\n');
		pref = NULL;
		term_kill();
		free(ln);
		if (key != '\n')
			break;
		(*row)++;
	}
	sbuf_str(sb, post);
	if (key == TERMESC)
		return sbuf_done(sb);
	sbuf_free(sb);
	return NULL;
}
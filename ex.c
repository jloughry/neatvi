#include <ctype.h>
#include <fcntl.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "vi.h"

#define EXLEN		512

/* read an input line; ex's input function */
static char *ex_read(char *msg)
{
	struct sbuf *sb;
	char c;
	if (xled) {
		char *s = led_prompt(msg, "");
		printf("\n");
		return s;
	}
	sb = sbuf_make();
	while ((c = getchar()) != EOF) {
		if (c == '\n')
			break;
		sbuf_chr(sb, c);
	}
	if (c == EOF) {
		sbuf_free(sb);
		return NULL;
	}
	return sbuf_done(sb);
}

/* print an output line; ex's output function */
static void ex_show(char *msg)
{
	if (xled)
		led_print(msg, -1);
	else
		printf("%s", msg);
}

/* read ex command location */
static char *ex_loc(char *s, char *loc)
{
	while (*s == ':' || isspace((unsigned char) *s))
		s++;
	while (*s && !isalpha((unsigned char) *s) && *s != '=') {
		if (*s == '\'')
			*loc++ = *s++;
		if (*s == '/' || *s == '?') {
			int d = *s;
			*loc++ = *s++;
			while (*s && *s != d) {
				if (*s == '\\' && s[1])
					*loc++ = *s++;
				*loc++ = *s++;
			}
		}
		*loc++ = *s++;
	}
	*loc = '\0';
	return s;
}

/* read ex command name */
static char *ex_cmd(char *s, char *cmd)
{
	char *cmd0 = cmd;
	s = ex_loc(s, cmd);
	while (isspace((unsigned char) *s))
		s++;
	while (isalpha((unsigned char) *s) || *s == '=' || *s == '!')
		if ((*cmd++ = *s++) == 'k' && cmd == cmd0 + 1)
			break;
	*cmd = '\0';
	return s;
}

/* read ex command argument */
static char *ex_arg(char *s, char *arg)
{
	s = ex_cmd(s, arg);
	while (isspace((unsigned char) *s))
		s++;
	while (*s && !isspace((unsigned char) *s))
		*arg++ = *s++;
	*arg = '\0';
	return s;
}

static int ex_search(char *pat)
{
	struct sbuf *kwd;
	int dir = *pat == '/' ? 1 : -1;
	char *b = pat;
	char *e = b;
	int i = xrow;
	regex_t re;
	kwd = sbuf_make();
	while (*++e) {
		if (*e == *pat)
			break;
		sbuf_chr(kwd, (unsigned char) *e);
		if (*e == '\\' && e[1])
			e++;
	}
	regcomp(&re, sbuf_buf(kwd), 0);
	while (i >= 0 && i < lbuf_len(xb)) {
		if (!regexec(&re, lbuf_get(xb, i), 0, NULL, 0))
			break;
		i += dir;
	}
	regfree(&re);
	sbuf_free(kwd);
	return i;
}

static int ex_lineno(char *num)
{
	int n = xrow;
	if (!num[0] || num[0] == '.')
		n = xrow;
	if (isdigit(num[0]))
		n = atoi(num) - 1;
	if (num[0] == '$')
		n = lbuf_len(xb) - 1;
	if (num[0] == '-')
		n = xrow - (num[1] ? ex_lineno(num + 1) : 1);
	if (num[0] == '+')
		n = xrow + (num[1] ? ex_lineno(num + 1) : 1);
	if (num[0] == '\'')
		n = lbuf_markpos(xb, num[1]);
	if (num[0] == '/' && num[1])
		n = ex_search(num);
	if (num[0] == '?' && num[1])
		n = ex_search(num);
	return n;
}

/* parse ex command location */
static int ex_region(char *loc, int *beg, int *end)
{
	int naddr = 0;
	if (!strcmp("%", loc)) {
		*beg = 0;
		*end = MAX(0, lbuf_len(xb));
		return 0;
	}
	if (!*loc) {
		*beg = xrow;
		*end = xrow == lbuf_len(xb) ? xrow : xrow + 1;
		return 0;
	}
	while (*loc) {
		char *r = loc;
		while (*loc && *loc != ';' && *loc != ',')
			loc++;
		*beg = *end;
		*end = ex_lineno(r) + 1;
		if (!naddr++)
			*beg = *end - 1;
		if (!*loc)
			break;
		if (*loc == ';')
			xrow = *end - 1;
		loc++;
	}
	if (*beg < 0 || *beg >= lbuf_len(xb))
		return 1;
	if (*end < *beg || *end > lbuf_len(xb))
		return 1;
	return 0;
}

static void ec_edit(char *ec)
{
	char arg[EXLEN];
	int fd;
	ex_arg(ec, arg);
	fd = open(arg, O_RDONLY);
	if (fd >= 0) {
		lbuf_rm(xb, 0, lbuf_len(xb));
		lbuf_rd(xb, fd, 0);
		lbuf_undofree(xb);
		close(fd);
		xrow = MAX(0, lbuf_len(xb) - 1);
	}
	snprintf(xpath, PATHLEN, "%s", arg);
}

static void ec_read(char *ec)
{
	char arg[EXLEN], loc[EXLEN];
	int fd;
	int beg, end;
	int n = lbuf_len(xb);
	ex_arg(ec, arg);
	ex_loc(ec, loc);
	fd = open(arg[0] ? arg : xpath, O_RDONLY);
	if (fd >= 0 && !ex_region(loc, &beg, &end)) {
		lbuf_rd(xb, fd, lbuf_len(xb) ? end : 0);
		close(fd);
		xrow = end + lbuf_len(xb) - n;
	}
}

static void ec_write(char *ec)
{
	char arg[EXLEN], loc[EXLEN];
	char *path;
	int beg, end;
	int fd;
	ex_arg(ec, arg);
	ex_loc(ec, loc);
	path = arg[0] ? arg : xpath;
	if (ex_region(loc, &beg, &end))
		return;
	if (!loc[0]) {
		beg = 0;
		end = lbuf_len(xb);
	}
	fd = open(path, O_WRONLY | O_CREAT, 0600);
	if (fd >= 0) {
		lbuf_wr(xb, fd, beg, end);
		close(fd);
	}
}

static void ec_insert(char *ec)
{
	char arg[EXLEN], cmd[EXLEN], loc[EXLEN];
	struct sbuf *sb;
	char *s;
	int beg, end;
	int n;
	ex_arg(ec, arg);
	ex_cmd(ec, cmd);
	ex_loc(ec, loc);
	if (ex_region(loc, &beg, &end) && (beg != 0 || end != 0))
		return;
	if (cmd[0] == 'c') {
		if (lbuf_len(xb))
			lbuf_rm(xb, beg, end);
		end = beg + 1;
	}
	sb = sbuf_make();
	while ((s = ex_read(""))) {
		if (!strcmp(".", s)) {
			free(s);
			break;
		}
		sbuf_str(sb, s);
		sbuf_chr(sb, '\n');
		free(s);
	}
	if (cmd[0] == 'a')
		if (end > lbuf_len(xb))
			end = lbuf_len(xb);
	n = lbuf_len(xb);
	lbuf_put(xb, end, sbuf_buf(sb));
	xrow = MIN(lbuf_len(xb) - 1, end + lbuf_len(xb) - n - 1);
	sbuf_free(sb);
}

static void ec_print(char *ec)
{
	char cmd[EXLEN], loc[EXLEN];
	int beg, end;
	int i;
	ex_cmd(ec, cmd);
	ex_loc(ec, loc);
	if (!cmd[0] && !loc[0]) {
		if (xrow >= lbuf_len(xb) - 1)
			return;
		xrow = xrow + 1;
	}
	if (!ex_region(loc, &beg, &end)) {
		for (i = beg; i < end; i++)
			ex_show(lbuf_get(xb, i));
		xrow = end;
	}
}

static void ec_delete(char *ec)
{
	char loc[EXLEN];
	int beg, end;
	ex_loc(ec, loc);
	if (!ex_region(loc, &beg, &end) && lbuf_len(xb)) {
		lbuf_rm(xb, beg, end);
		xrow = beg;
	}
}

static void ec_lnum(char *ec)
{
	char loc[EXLEN];
	char msg[128];
	int beg, end;
	ex_loc(ec, loc);
	if (ex_region(loc, &beg, &end))
		return;
	sprintf(msg, "%d\n", end);
	ex_show(msg);
}

static void ec_undo(char *ec)
{
	lbuf_undo(xb);
}

static void ec_redo(char *ec)
{
	lbuf_redo(xb);
}

static void ec_mark(char *ec)
{
	char loc[EXLEN], arg[EXLEN];
	int beg, end;
	ex_arg(ec, arg);
	ex_loc(ec, loc);
	if (ex_region(loc, &beg, &end))
		return;
	lbuf_mark(xb, arg[0], end - 1);
}

static char *readuntil(char **src, int delim)
{
	struct sbuf *sbuf = sbuf_make();
	char *s = *src;
	/* reading the pattern */
	while (*s && *s != delim) {
		if (s[0] == '\\' && s[1])
			sbuf_chr(sbuf, (unsigned char) *s++);
		sbuf_chr(sbuf, (unsigned char) *s++);
	}
	if (*s)			/* skipping the delimiter */
		s++;
	*src = s;
	return sbuf_done(sbuf);
}

static void ec_substitute(char *ec)
{
	char loc[EXLEN], arg[EXLEN];
	regmatch_t subs[16];
	regex_t re;
	int beg, end;
	char *pat, *rep;
	char *s = arg;
	int delim;
	int i;
	ex_arg(ec, arg);
	ex_loc(ec, loc);
	if (ex_region(loc, &beg, &end))
		return;
	delim = (unsigned char) *s++;
	pat = readuntil(&s, delim);
	rep = readuntil(&s, delim);
	regcomp(&re, pat, 0);
	for (i = beg; i < end; i++) {
		char *ln = lbuf_get(xb, i);
		if (!regexec(&re, ln, LEN(subs), subs, 0)) {
			struct sbuf *r = sbuf_make();
			sbuf_mem(r, ln, subs[0].rm_so);
			sbuf_str(r, rep);
			sbuf_str(r, ln + subs[0].rm_eo);
			lbuf_put(xb, i, sbuf_buf(r));
			lbuf_rm(xb, i + 1, i + 2);
			sbuf_free(r);
		}
	}
	regfree(&re);
	free(pat);
	free(rep);
}

static void ec_quit(char *ec)
{
	xquit = 1;
}

static struct excmd {
	char *abbr;
	char *name;
	void (*ec)(char *s);
} excmds[] = {
	{"p", "print", ec_print},
	{"a", "append", ec_insert},
	{"i", "insert", ec_insert},
	{"d", "delete", ec_delete},
	{"c", "change", ec_insert},
	{"e", "edit", ec_edit},
	{"=", "=", ec_lnum},
	{"k", "mark", ec_mark},
	{"q", "quit", ec_quit},
	{"r", "read", ec_read},
	{"w", "write", ec_write},
	{"u", "undo", ec_undo},
	{"r", "redo", ec_redo},
	{"s", "substitute", ec_substitute},
	{"", "", ec_print},
};

/* execute a single ex command */
void ex_command(char *ln0)
{
	char cmd[EXLEN];
	char *ln = ln0 ? ln0 : ex_read(":");
	int i;
	if (!ln)
		return;
	ex_cmd(ln, cmd);
	for (i = 0; i < LEN(excmds); i++) {
		if (!strcmp(excmds[i].abbr, cmd) || !strcmp(excmds[i].name, cmd)) {
			excmds[i].ec(ln);
			break;
		}
	}
	if (!ln0)
		free(ln);
	lbuf_undomark(xb);
}

/* ex main loop */
void ex(void)
{
	if (xled)
		term_init();
	while (!xquit)
		ex_command(NULL);
	if (xled)
		term_done();
}
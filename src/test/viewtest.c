#include "compat.h"
#include "coil.h"
#include "script.h"
#include "cells.h"

/* viewtest: headless layout driver. One Buf and one View, no Ed. */

static Buf *buf;
static View view;
static int cellmode;

static char*
skip(char *a)
{
	while(*a == ' ')
		a++;
	return a;
}

static int
nextint(char **pp, int *v)
{
	char *p;
	int n, nd;

	p = skip(*pp);
	n = 0;
	nd = 0;
	while(*p >= '0' && *p <= '9'){
		n = n*10 + (*p - '0');
		nd++;
		p++;
	}
	if(nd == 0 || (*p != ' ' && *p != '\0'))
		return -1;
	*v = n;
	*pp = p;
	return 0;
}

static int
hv(int c)
{
	if(c >= '0' && c <= '9')
		return c - '0';
	if(c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if(c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* quoted string with escapes \n \r \t \\ \" \0 \xHH; result in malloc'd *out, length *n */
static int
nextquoted(char **pp, char **out, int *n)
{
	char *p, *o;
	int len, h1, h2;

	p = skip(*pp);
	if(*p != '"')
		return -1;
	p++;
	o = malloc(strlen(p) + 1);
	len = 0;
	while(*p != '"'){
		if(*p == '\0'){
			free(o);
			return -1;
		}
		if(*p == '\\'){
			p++;
			switch(*p){
			case 'n': o[len++] = '\n'; break;
			case 'r': o[len++] = '\r'; break;
			case 't': o[len++] = '\t'; break;
			case '\\': o[len++] = '\\'; break;
			case '"': o[len++] = '"'; break;
			case '0': o[len++] = '\0'; break;
			case 'x':
				h1 = hv(p[1]);
				h2 = h1 < 0 ? -1 : hv(p[2]);
				if(h1 < 0 || h2 < 0){
					free(o);
					return -1;
				}
				o[len++] = (char)(h1*16 + h2);
				p += 2;
				break;
			default:
				free(o);
				return -1;
			}
			p++;
		}else
			o[len++] = *p++;
	}
	p++;
	*pp = p;
	*out = o;
	*n = len;
	return 0;
}

static void
badargs(void)
{
	lprint("error bad arguments\n");
}

static void
load(char *a)
{
	char *s;
	int n;
	char err[64];

	if(nextquoted(&a, &s, &n) < 0){
		badargs();
		return;
	}
	buf_free(buf);
	buf = buf_new();
	if(buf_load(buf, s, n, err, sizeof err) < 0)
		lprint("error %s\n", err);
}

/* load the line "x" repeated n times, then a suffix: loadrep N "suffix" */
static void
loadrep(char *a)
{
	char *s, *t, err[64];
	int n, m;

	if(nextint(&a, &n) < 0 || nextquoted(&a, &s, &m) < 0){
		badargs();
		return;
	}
	t = malloc(n + m + 1);
	memset(t, 'x', n);
	memmove(t + n, s, m);
	free(s);
	buf_free(buf);
	buf = buf_new();
	if(buf_load(buf, t, n + m, err, sizeof err) < 0)
		lprint("error %s\n", err);
}

static int
topline(void)
{
	return buf_pos_to_line(buf, view.top);
}

static void
row(int r)
{
	Vcell *vc;
	int i;
	char sl;

	if(view.cols <= 0){
		badargs();
		return;
	}
	vc = malloc(view.cols * sizeof *vc);
	view_fix(&view, buf);
	view_layout(&view, buf, buf_line_start(buf, topline() + r), vc, view.cols);
	lprint("row %d", r);
	for(i = 0; i < view.cols; i++){
		if(vc[i].r == Ccont){
			lprint(" cont");
			continue;
		}
		sl = vc[i].slot == Sctrl ? '^' : vc[i].slot == Sinvalid ? '?' : '.';
		lprint(" %04x%c", (int)vc[i].r, sl);
	}
	lprint("\n");
	free(vc);
}

static void
cmd(char *l)
{
	char w[32], *a;
	int k, x, y, rr, cc;
	Pos p;

	a = l;
	for(k = 0; *a != ' ' && *a != '\0' && k < 31; k++)
		w[k] = *a++;
	w[k] = '\0';
	if(strcmp(w, "load") == 0)
		load(a);
	else if(strcmp(w, "loadrep") == 0)
		loadrep(a);
	else if(strcmp(w, "size") == 0){
		if(nextint(&a, &x) < 0 || nextint(&a, &y) < 0){
			badargs();
			return;
		}
		view.rows = x;
		view.cols = y;
	}else if(strcmp(w, "width") == 0){
		a = skip(a);
		cellmode = strcmp(a, "eaw") == 0 ? Weaw : Wascii;
	}else if(strcmp(w, "tab") == 0){
		if(nextint(&a, &x) < 0)
			badargs();
		else
			view.tabwidth = x;
	}else if(strcmp(w, "top") == 0){
		if(nextint(&a, &x) < 0)
			badargs();
		else
			view.top = buf_line_start(buf, x);
	}else if(strcmp(w, "hscroll") == 0){
		if(nextint(&a, &x) < 0)
			badargs();
		else
			view.hscroll = x;
	}else if(strcmp(w, "row") == 0){
		if(nextint(&a, &x) < 0)
			badargs();
		else
			row(x);
	}else if(strcmp(w, "pos2cell") == 0){
		if(nextint(&a, &x) < 0){
			badargs();
			return;
		}
		if(view_pos_to_cell(&view, buf, x, &rr, &cc) < 0)
			lprint("pos2cell %d off\n", x);
		else
			lprint("pos2cell %d %d %d\n", x, rr, cc);
	}else if(strcmp(w, "cell2pos") == 0){
		if(nextint(&a, &x) < 0 || nextint(&a, &y) < 0){
			badargs();
			return;
		}
		lprint("cell2pos %d %d %d\n", x, y, view_cell_to_pos(&view, buf, x, y));
	}else if(strcmp(w, "colof") == 0){
		if(nextint(&a, &x) < 0)
			badargs();
		else
			lprint("colof %d %d\n", x, view_col_of(&view, buf, x));
	}else if(strcmp(w, "reveal") == 0){
		if(nextint(&a, &x) < 0){
			badargs();
			return;
		}
		view_reveal(&view, buf, x);
		lprint("reveal %d top %d hscroll %d\n", x, topline(), view.hscroll);
	}else if(strcmp(w, "roundtrip") == 0){
		/* every rune start must come back from its own cell (needs the position on screen) */
		int bad, seen;

		bad = seen = 0;
		for(p = 0; p < buf_len(buf); p = buf_next(buf, p)){
			if(view_pos_to_cell(&view, buf, p, &rr, &cc) < 0)
				continue;
			seen++;
			if(view_cell_to_pos(&view, buf, rr, cc) != p){
				lprint("mismatch %d -> %d %d -> %d\n", p, rr, cc, view_cell_to_pos(&view, buf, rr, cc));
				bad++;
			}
		}
		lprint("roundtrip seen %d bad %d\n", seen, bad);
	}else
		lprint("error unknown command\n");
}

static int
run(void)
{
	int rc;

	view_init(&view);
	view.rows = 10;
	view.cols = 20;
	view.runecells = tcells;
	view.rcarg = &cellmode;
	buf = buf_new();
	rc = script(LINEMAX, cmd);
	buf_free(buf);
	return rc;
}

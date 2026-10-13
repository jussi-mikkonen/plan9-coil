#include "compat.h"
#include "coil.h"

/*
 * cmd.c: parser for the sam-subset command language.
 *
 *   line  := [ '%' | ',' ] chain
 *   chain := ('x'|'y') ['/re/'] chain | ('g'|'v') '/re/' chain | term | empty
 *   term  := 'd' | ('c'|'a'|'i') delim text [delim] | 's' delim re delim text [delim] ['g']
 *
 * Hand-written, iterative, no recursion: the chain is a flat array bounded by Maxdepth.
 */

static int
isblank_(int c)
{
	return c == ' ' || c == '\t';
}

static int
isalnum_(int c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* sam okdelim: not alphanumeric, backslash, blank or newline */
static int
okdelim(int c)
{
	return c != '\0' && c != '\\' && c != '\n' && !isblank_(c) && !isalnum_(c);
}

static void
seterr(char *err, int nerr, char *fmt, char *a)
{
	if(err != nil && nerr > 0)
		lsnprint(err, nerr, fmt, a);
}

/* one rune of s as a NUL-terminated string in out (at least 5 bytes) */
static void
oneruneq(char *s, char *out)
{
	Rune r;
	int n;

	n = utf_dec(s, (int)strlen(s), &r);
	memmove(out, s, n);
	out[n] = '\0';
}

void
cmd_free(Cmd *c)
{
	int i;

	for(i = 0; i < c->nstep; i++)
		free(c->step[i].re);
	free(c->text);
	free(c->re);
	memset(c, 0, sizeof *c);
}

/*
 * Read delimited text at *pp (just after the opening delimiter of dn bytes d).
 * kind 0 = pattern: "\delim" -> delim, other backslash pairs kept.
 * kind 1 = text: also \n \t \\ ; other "\x" kept as two bytes.
 * The closing delimiter is optional at the end of the line. Returns malloc'd
 * string (length in *n) or nil when out of memory; *pp is left after the closing delimiter.
 */
static int rdclosed;	/* readdelim met the closing delimiter */

static char*
readdelim(char **pp, char *d, int dn, int kind, Pos *n)
{
	char *p, *o;
	int len;

	p = *pp;
	o = malloc(strlen(p) + 1);
	if(o == nil)
		return nil;
	len = 0;
	rdclosed = 0;
	while(*p != '\0'){
		if(strncmp(p, d, dn) == 0){
			p += dn;
			rdclosed = 1;
			break;
		}
		if(*p == '\\' && p[1] != '\0'){
			if(strncmp(p + 1, d, dn) == 0){
				memmove(o + len, d, dn);
				len += dn;
				p += 1 + dn;
				continue;
			}
			if(kind == 1 && p[1] == 'n'){
				o[len++] = '\n';
				p += 2;
				continue;
			}
			if(kind == 1 && p[1] == 't'){
				o[len++] = '\t';
				p += 2;
				continue;
			}
			if(kind == 1 && p[1] == '\\'){
				o[len++] = '\\';
				p += 2;
				continue;
			}
			o[len++] = p[0];
			o[len++] = p[1];
			p += 2;
			continue;
		}
		o[len++] = *p++;
	}
	o[len] = '\0';
	*pp = p;
	*n = len;
	return o;
}

static char*
skipb(char *p)
{
	while(isblank_(*p))
		p++;
	return p;
}

int
cmd_parse(char *line, Cmd *c, char *err, int nerr)
{
	char *p, d[8], q[8];
	int ch, dn, op;
	Rune r;
	Pos n;

	memset(c, 0, sizeof *c);
	p = skipb(line);
	if(*p == '%' || *p == ','){
		c->whole = 1;
		p++;
	}
	for(;;){
		p = skipb(p);
		ch = (uchar)*p;
		if(ch == '\0')
			return 0;
		if(ch == 'x' || ch == 'y' || ch == 'g' || ch == 'v'){
			op = ch;
			p++;
			if(c->nstep >= Maxdepth){
				seterr(err, nerr, "%s", "command nested too deeply");
				goto fail;
			}
			p = skipb(p);
			if(!okdelim((uchar)*p)){
				if(*p == '\\'){
					seterr(err, nerr, "bad delimiter '%s'", "\\");
					goto fail;
				}
				if(op == 'g' || op == 'v'){
					seterr(err, nerr, "missing pattern for %s", op == 'g' ? "g" : "v");
					goto fail;
				}
				/* bare x/y: iterate lines */
				c->step[c->nstep].op = op;
				c->nstep++;
				continue;
			}
			dn = utf_dec(p, (int)strlen(p), &r);
			memmove(d, p, dn);
			d[dn] = '\0';
			p += dn;
			c->step[c->nstep].re = readdelim(&p, d, dn, 0, &n);
			if(c->step[c->nstep].re == nil){
				seterr(err, nerr, "%s", "out of memory");
				goto fail;
			}
			c->step[c->nstep].op = op;
			c->step[c->nstep].nre = (int)n;
			c->nstep++;
			continue;
		}
		if(ch == 'd'){
			c->term = 'd';
			p = skipb(p + 1);
			if(*p != '\0'){
				seterr(err, nerr, "%s", "unexpected text after d");
				goto fail;
			}
			return 0;
		}
		if(ch == 's'){
			p++;
			if(*p >= '0' && *p <= '9'){
				seterr(err, nerr, "%s", "s: counts are not supported");
				goto fail;
			}
			p = skipb(p);
			if(*p == '\0'){
				seterr(err, nerr, "missing pattern for %s", "s");
				goto fail;
			}
			dn = utf_dec(p, (int)strlen(p), &r);
			if(dn == 1 && !okdelim((uchar)*p)){
				oneruneq(p, q);
				seterr(err, nerr, "bad delimiter '%s'", q);
				goto fail;
			}
			memmove(d, p, dn);
			d[dn] = '\0';
			p += dn;
			c->re = readdelim(&p, d, dn, 0, &n);
			if(c->re == nil){
				seterr(err, nerr, "%s", "out of memory");
				goto fail;
			}
			c->nre = (int)n;
			if(!rdclosed){
				seterr(err, nerr, "missing replacement for %s", "s");
				goto fail;
			}
			c->text = readdelim(&p, d, dn, 0, &c->ntext);
			if(c->text == nil){
				seterr(err, nerr, "%s", "out of memory");
				goto fail;
			}
			c->term = 's';
			if(rdclosed){
				p = skipb(p);
				if(*p == 'g'){
					c->global = 1;
					p = skipb(p + 1);
				}
			}
			if(*p != '\0'){
				seterr(err, nerr, "unexpected text after %s", "s");
				goto fail;
			}
			return 0;
		}
		if(ch == 'c' || ch == 'i' || ch == 'a'){
			op = ch;
			p = skipb(p + 1);
			if(*p == '\0'){
				seterr(err, nerr, "missing text for %s", op == 'c' ? "c" : op == 'i' ? "i" : "a");
				goto fail;
			}
			dn = utf_dec(p, (int)strlen(p), &r);
			memmove(d, p, dn);
			d[dn] = '\0';
			if(dn == 1 && !okdelim((uchar)*p)){
				oneruneq(p, q);
				seterr(err, nerr, "bad delimiter '%s'", q);
				goto fail;
			}
			p += dn;
			c->text = readdelim(&p, d, dn, 1, &c->ntext);
			if(c->text == nil){
				seterr(err, nerr, "%s", "out of memory");
				goto fail;
			}
			c->term = op;
			p = skipb(p);
			if(*p != '\0'){
				seterr(err, nerr, "unexpected text after %s", op == 'c' ? "c" : op == 'i' ? "i" : "a");
				goto fail;
			}
			return 0;
		}
		if((ch >= '0' && ch <= '9') || ch == '.' || ch == '$' || ch == '+' || ch == '-' ||
		   ch == '/' || ch == '?' || ch == '#' || ch == '\'' || ch == ';'){
			seterr(err, nerr, "%s", "addresses are not supported; use % or a selection");
			goto fail;
		}
		oneruneq(p, q);
		seterr(err, nerr, "unknown command '%s'", q);
		goto fail;
	}
fail:
	cmd_free(c);
	return -1;
}

/* append s to out at *k, within n bytes */
static void
put(char *out, int n, int *k, char *s, int len)
{
	int i;

	for(i = 0; i < len && *k < n - 1; i++)
		out[(*k)++] = s[i];
	out[*k] = '\0';
}

static void
putq(char *out, int n, int *k, char *s, Pos len)
{
	static char hex[] = "0123456789abcdef";
	Pos i;
	int c;
	char t[4];

	put(out, n, k, "\"", 1);
	for(i = 0; i < len; i++){
		c = (uchar)s[i];
		if(c == '\n')
			put(out, n, k, "\\n", 2);
		else if(c == '\t')
			put(out, n, k, "\\t", 2);
		else if(c == '\\')
			put(out, n, k, "\\\\", 2);
		else if(c == '"')
			put(out, n, k, "\\\"", 2);
		else if(c < 0x20){
			t[0] = '\\';
			t[1] = 'x';
			t[2] = hex[c >> 4];
			t[3] = hex[c & 15];
			put(out, n, k, t, 4);
		}else
			put(out, n, k, s + i, 1);
	}
	put(out, n, k, "\"", 1);
}

void
cmd_dump(Cmd *c, char *out, int n)
{
	int i, k, sp;
	char t[2];

	k = 0;
	sp = 0;
	if(n <= 0)
		return;
	out[0] = '\0';
	if(c->whole){
		put(out, n, &k, "whole", 5);
		sp = 1;
	}
	for(i = 0; i < c->nstep; i++){
		if(sp)
			put(out, n, &k, " ", 1);
		sp = 1;
		t[0] = (char)c->step[i].op;
		put(out, n, &k, t, 1);
		if(c->step[i].re != nil){
			put(out, n, &k, " /", 2);
			put(out, n, &k, c->step[i].re, c->step[i].nre);
			put(out, n, &k, "/", 1);
		}
	}
	if(c->term != 0){
		if(sp)
			put(out, n, &k, " ", 1);
		t[0] = (char)c->term;
		put(out, n, &k, t, 1);
		if(c->term == 's'){
			put(out, n, &k, " /", 2);
			put(out, n, &k, c->re, c->nre);
			put(out, n, &k, "/ ", 2);
			putq(out, n, &k, c->text, c->ntext);
			if(c->global)
				put(out, n, &k, " g", 2);
		}else if(c->term != 'd'){
			put(out, n, &k, " ", 1);
			putq(out, n, &k, c->text, c->ntext);
		}
	}
}

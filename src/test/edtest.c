#include "compat.h"
#include "coil.h"
#include "host.h"
#include "script.h"
#include "cells.h"

/* edtest: headless key-script driver. A fake host holds path -> bytes. */

enum { Nfile = 16, Namelen = 64 };

typedef struct File File;
struct File {
	char	name[Namelen];
	char	*data;
	int	n;
	int	big;
	int	used;
	int	perm;
	u32int	vers;
	u32int	mtime;
	int	kind;
	int	append;
	int	failwrite;
};

static u32int fclock = 1000;

static File files[Nfile];
static Host host;
static Ed ed;
static Canvas cv;
static int cellmode;
static int autowork = 1;
static char err[128];

static char*
skip(char *a)
{
	while(*a == ' ')
		a++;
	return a;
}

/* integer token, optionally negative; -1 on failure */
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

static File*
find(char *name)
{
	int i;

	for(i = 0; i < Nfile; i++)
		if(files[i].used && strcmp(files[i].name, name) == 0)
			return &files[i];
	return nil;
}

static File*
slot(char *name)
{
	int i;
	File *f;

	f = find(name);
	if(f != nil)
		return f;
	for(i = 0; i < Nfile; i++)
		if(!files[i].used){
			f = &files[i];
			f->used = 1;
			lsnprint(f->name, Namelen, "%s", name);
			f->data = nil;
			f->n = 0;
			f->big = 0;
			f->perm = 0644;
			f->vers = 1;
			f->mtime = fclock;
			f->kind = Sfile;
			f->append = 0;
			f->failwrite = 0;
			return f;
		}
	return nil;
}

static int
h_exists(Host *h, char *path)
{
	(void)h;
	return find(path) != nil;
}

static void
fstamp(File *f, Stamp *st)
{
	if(f == nil){
		stamp_clear(st);
		return;
	}
	st->kind = f->kind;
	st->append = f->append;
	st->type = 0;
	st->dev = 0;
	st->pathhi = 0;
	st->pathlo = (u32int)(f - files) + 1;
	st->vers = f->vers;
	st->mtime = f->mtime;
	st->length = f->n;
}

static int
h_stat(Host *h, char *path, Stamp *st, char *e, int ne)
{
	(void)h;
	(void)e;
	(void)ne;
	fstamp(find(path), st);
	return 0;
}

static int
h_read(Host *h, char *path, char **data, Pos *n, int *isnew, Stamp *st, char *e, int ne)
{
	File *f;

	(void)h;
	f = find(path);
	fstamp(f, st);
	if(f == nil){
		*data = malloc(1);
		*n = 0;
		*isnew = 1;
		return 0;
	}
	if(f->kind == Sdir){
		lsnprint(e, ne, "is a directory");
		return -1;
	}
	if(f->big){
		lsnprint(e, ne, "file too large");
		return -1;
	}
	*data = malloc(f->n + 1);
	memmove(*data, f->data, f->n);
	*n = f->n;
	*isnew = 0;
	return 0;
}

typedef struct Coll Coll;
struct Coll {
	char	*p;
	int	n;
};

static int
collect(void *arg, char *p, int n)
{
	Coll *c;

	c = arg;
	memmove(c->p + c->n, p, n);
	c->n += n;
	return n;
}

static int
h_write(Host *h, char *path, Buf *b, Stamp *expect, Stamp *out, char *e, int ne)
{
	File *f;
	Coll c;
	Stamp now;

	(void)h;
	f = find(path);
	fstamp(f, &now);
	if(stamp_refuse(&now, e, ne) < 0)
		return -1;
	if(expect != nil && stamp_changed(expect, &now))
		return Wchanged;
	if(f != nil && (f->perm & 0200) == 0){
		lsnprint(e, ne, "permission denied");
		return -1;
	}
	if(f != nil && f->failwrite){
		lsnprint(e, ne, "write: disk full");
		return -1;
	}
	f = slot(path);
	if(f == nil){
		lsnprint(e, ne, "too many files");
		return -1;
	}
	c.p = malloc(buf_len(b) + 1);
	c.n = 0;
	if(buf_write(b, collect, &c) < 0){
		free(c.p);
		lsnprint(e, ne, "write error");
		return -1;
	}
	free(f->data);
	f->data = c.p;
	f->n = c.n;
	f->big = 0;
	f->vers++;
	fclock += 2;
	f->mtime = fclock;
	fstamp(f, out);
	return 0;
}

typedef struct Snarf Snarf;
struct Snarf {
	int	present;
	char	*data;
	int	n;
	int	cap;	/* puts larger than cap fail; -1 = no cap */
	int	nput;
	int	nget;
	int	rio;	/* convert on put like rio's cvttorunes: drop NULs, invalid bytes -> U+FFFD */
};

static Snarf snarf = {1, nil, 0, -1, 0, 0, 0};

static int
h_snarfget(Host *h, char **data, Pos *n, char *e, int ne)
{
	(void)h;
	snarf.nget++;
	if(!snarf.present){
		lsnprint(e, ne, "'/dev/snarf' file does not exist");
		return -1;
	}
	*data = malloc(snarf.n + 1);
	if(snarf.n > 0)
		memmove(*data, snarf.data, snarf.n);
	*n = snarf.n;
	return 0;
}

static int
h_snarfput(Host *h, char *p, Pos n, char *e, int ne)
{
	(void)h;
	snarf.nput++;
	if(!snarf.present){
		lsnprint(e, ne, "'/dev/snarf' file does not exist");
		return -1;
	}
	if(snarf.cap >= 0 && n > snarf.cap){
		lsnprint(e, ne, "read or write too large");
		return -1;
	}
	free(snarf.data);
	if(snarf.rio){
		Pos i, o;
		int w;
		Rune r;

		snarf.data = malloc(3 * n + 1);	/* each byte at most U+FFFD (3 bytes) */
		o = 0;
		for(i = 0; i < n; i += w){
			w = utf_dec(p + i, n - i, &r);
			if(r != 0)
				o += utf_enc(snarf.data + o, r);
		}
		snarf.n = o;
		return 0;
	}
	snarf.data = malloc(n + 1);
	if(n > 0)
		memmove(snarf.data, p, n);
	snarf.n = n;
	return 0;
}

static void
setsnarf(char *s, int n)
{
	free(snarf.data);
	snarf.data = s;
	snarf.n = n;
}

/* print n bytes as an escaped, quoted string */
static void
quoted(char *label, char *s, int n)
{
	char *o;
	int k, c, m;
	static char hex[] = "0123456789abcdef";

	o = malloc(n * 4 + 1);
	m = 0;
	for(k = 0; k < n; k++){
		c = (uchar)s[k];
		if(c == '\n'){ o[m++] = '\\'; o[m++] = 'n'; }
		else if(c == '\r'){ o[m++] = '\\'; o[m++] = 'r'; }
		else if(c == '\t'){ o[m++] = '\\'; o[m++] = 't'; }
		else if(c < 0x20 || c == 0x22 || c == 0x5c || c >= 0x7f){
			o[m++] = '\\';
			o[m++] = 'x';
			o[m++] = hex[c >> 4];
			o[m++] = hex[c & 15];
		}else
			o[m++] = (char)c;
	}
	o[m] = '\0';
	lprint("%s \"%s\"\n", label, o);
	free(o);
}

/* key names: <esc> <ret> <cr> <bs> <tab> <del> <c-a>..<c-z> <up> <down> <left> <right> <home> <end> <pgup> <pgdn> <lt> */
static int
keyname(char *name, Rune *r)
{
	static struct { char *n; Rune r; } t[] = {
		{"esc", Ck_esc}, {"ret", Ck_nl}, {"cr", Ck_cr}, {"bs", Ck_bs}, {"tab", Ck_tab},
		{"del", Ck_del}, {"up", Ck_up}, {"down", Ck_down}, {"left", Ck_left},
		{"right", Ck_right}, {"home", Ck_home}, {"end", Ck_end}, {"pgup", Ck_pgup},
		{"pgdn", Ck_pgdn}, {"lt", '<'}
	};
	int i;

	for(i = 0; i < (int)(sizeof t / sizeof t[0]); i++)
		if(strcmp(t[i].n, name) == 0){
			*r = t[i].r;
			return 0;
		}
	if(name[0] == 'c' && name[1] == '-' && name[2] >= 'a' && name[2] <= 'z' && name[3] == '\0'){
		*r = name[2] - 'a' + 1;
		return 0;
	}
	return -1;
}

static void
keys(char *s, int n)
{
	char name[16];
	int i, k, used;
	Rune r;
	Ev ev;

	i = 0;
	while(i < n){
		if(s[i] == '<'){
			k = 0;
			i++;
			while(i < n && s[i] != '>' && k < 15)
				name[k++] = s[i++];
			name[k] = '\0';
			if(i >= n || s[i] != '>' || keyname(name, &r) < 0){
				lprint("error bad key name\n");
				return;
			}
			i++;
		}else{
			used = utf_dec(s + i, n - i, &r);
			i += used;
		}
		ev.kind = Ekey;
		ev.r = r;
		ev.row = ev.col = ev.n = 0;
		ed_event(&ed, &ev);
	}
}

static char*
modename(void)
{
	return ed.mode == Minsert ? "INS" : ed.mode == Mselect ? "SEL" : "NOR";
}

static void
dumptext(void)
{
	char *s;
	int n;

	n = buf_len(ed.doc.b);
	s = malloc(n + 1);
	buf_read(ed.doc.b, 0, s, n);
	quoted("text", s, n);
	free(s);
}

static void
dumpsel(void)
{
	SelSet *ss;
	int i;

	ss = &ed.doc.sel;
	lprint("sel %s", modename());
	for(i = 0; i < ss->n; i++)
		lprint(" [%d,%d)%s", ss->s[i].anchor, ss->s[i].head, i == ss->primary ? "*" : "");
	lprint("\n");
}

/* row of the canvas as UTF-8 without continuation cells and trailing spaces */
static int
rowtext(int row, char *out)
{
	int c, n;
	Rune r;

	n = 0;
	for(c = 0; c < cv.cols; c++){
		r = cv.r[row * cv.cols + c];
		if(r == Ccont)
			continue;
		n += utf_enc(out + n, r);
	}
	while(n > 0 && out[n-1] == ' ')
		n--;
	out[n] = '\0';
	return n;
}

static void
dumpscreen(void)
{
	int row, c, st;
	char *out, *sty;
	Rune r;

	if(cv.rows <= 0 || cv.cols <= 0){
		lprint("screen empty\n");
		return;
	}
	render(&ed, &cv);
	out = malloc(cv.cols * 4 + 1);
	sty = malloc(cv.cols + 1);
	for(row = 0; row < cv.rows; row++){
		rowtext(row, out);
		lprint("r %02d %s\n", row, out);
		for(c = 0; c < cv.cols; c++){
			st = cv.style[row * cv.cols + c];
			r = cv.r[row * cv.cols + c];
			if(st & Ocur)
				sty[c] = '@';
			else if(st & Osel)
				sty[c] = '#';
			else if(r == Ccont)
				sty[c] = '>';
			else switch(st & 0x3f){
			case Sstatus: sty[c] = '='; break;
			case Smessage: sty[c] = '!'; break;
			case Sctrl: sty[c] = '^'; break;
			case Sinvalid: sty[c] = '?'; break;
			case Skeyword: sty[c] = 'k'; break;
			case Stype: sty[c] = 't'; break;
			case Scomment: sty[c] = 'c'; break;
			case Sstring: sty[c] = 's'; break;
			case Snumber: sty[c] = 'n'; break;
			case Spreproc: sty[c] = 'p'; break;
			case Svariable: sty[c] = 'v'; break;
			case Starget: sty[c] = 'T'; break;
			case Srecipe: sty[c] = 'r'; break;
			case Sheading: sty[c] = 'H'; break;
			case Semph: sty[c] = 'e'; break;
			case Sstrong: sty[c] = 'S'; break;
			case Scode: sty[c] = 'C'; break;
			case Slink: sty[c] = 'l'; break;
			case Slistmark: sty[c] = 'm'; break;
			case Sbracket: sty[c] = 'M'; break;
			case Strailing: sty[c] = '~'; break;
			case Slinenr: sty[c] = '0'; break;
			default: sty[c] = '.'; break;
			}
		}
		sty[cv.cols] = '\0';
		lprint("s %02d %s\n", row, sty);
	}
	free(out);
	free(sty);
}

static void
dumpstatus(void)
{
	char *out;

	if(cv.rows <= 0 || cv.cols <= 0){
		lprint("status empty\n");
		return;
	}
	render(&ed, &cv);
	out = malloc(cv.cols * 4 + 1);
	rowtext(cv.rows - 1, out);
	lprint("status %s\n", out);
	free(out);
}

static void
dumpprompt(void)
{
	char k[8];
	int n;

	if(!ed.prompt.on){
		lprint("prompt off\n");
		return;
	}
	n = utf_enc(k, ed.prompt.kind);
	k[n] = '\0';
	{
		char lab[16];
		lsnprint(lab, sizeof lab, "prompt %s", k);
		quoted(lab, ed.prompt.text, ed.prompt.n);
	}
}

static void
dumpstamp(char *label, Stamp *st, int perm)
{
	static char *kn[] = {"none", "file", "dir", "unknown"};

	/* %d with casts: the u verb is a flag in Plan 9 print and is not portable */
	lprint("%s %s app %d vers %d mtime %d len %d", label, kn[st->kind], st->append,
		(int)st->vers, (int)st->mtime, st->length);
	if(perm >= 0)
		lprint(" perm %03o", perm);
	lprint("\n");
}

static void
dumpreg(void)
{
	int i;

	lprint("reg %d\n", ed.reg.n);
	for(i = 0; i < ed.reg.n; i++)
		quoted("r", ed.reg.s[i], ed.reg.len[i]);
}

/* setsel A H [A H ...]: install the selection list directly, primary 0 */
static void
setsel(char *a)
{
	int x, y;
	SelSet ss;

	sel_init(&ss);
	while(nextint(&a, &x) == 0){
		if(nextint(&a, &y) < 0){
			badargs();
			sel_free(&ss);
			return;
		}
		sel_add(&ss, x, y);
	}
	if(ss.n == 0){
		badargs();
		sel_free(&ss);
		return;
	}
	ss.primary = 0;
	sel_normalize(&ss, ed.doc.b);
	sel_copy(&ed.doc.sel, &ss);
	sel_free(&ss);
}

static void
dump(char *a)
{
	char w[16], name[Namelen];
	int k;
	File *f;

	a = skip(a);
	for(k = 0; *a != ' ' && *a != '\0' && k < 15; k++)
		w[k] = *a++;
	w[k] = '\0';
	if(strcmp(w, "text") == 0)
		dumptext();
	else if(strcmp(w, "sel") == 0)
		dumpsel();
	else if(strcmp(w, "reg") == 0)
		dumpreg();
	else if(strcmp(w, "prompt") == 0)
		dumpprompt();
	else if(strcmp(w, "search") == 0){
		if(ed.srch.havepat)
			lprint("search \"%s\" dir %d\n", ed.srch.pat.src, ed.srch.dir);
		else
			lprint("search none\n");
	}
	else if(strcmp(w, "job") == 0){
		if(ed.job.on)
			lprint("job %s %d/%d\n", ed.job.what, ed.job.done, ed.job.total);
		else
			lprint("job none\n");
	}else if(strcmp(w, "stamp") == 0)
		dumpstamp("stamp", &ed.stamp, -1);
	else if(strcmp(w, "hoststamp") == 0){
		Stamp st;

		a = skip(a);
		f = find(a);
		if(f == nil)
			lprint("error no such file\n");
		else{
			fstamp(f, &st);
			dumpstamp("hoststamp", &st, f->perm);
		}
	}else if(strcmp(w, "snarf") == 0){
		if(snarf.present){
			quoted("snarf", snarf.data == nil ? "" : snarf.data, snarf.n);
			lprint("puts %d gets %d\n", snarf.nput, snarf.nget);
		}else
			lprint("snarf absent puts %d gets %d\n", snarf.nput, snarf.nget);
	}else if(strcmp(w, "mode") == 0)
		lprint("mode %s\n", modename());
	else if(strcmp(w, "status") == 0)
		dumpstatus();
	else if(strcmp(w, "msg") == 0)
		lprint("msg \"%s\"\n", ed.msg);
	else if(strcmp(w, "quit") == 0)
		lprint("quit %d\n", ed.quit);
	else if(strcmp(w, "screen") == 0)
		dumpscreen();
	else if(strcmp(w, "host") == 0){
		a = skip(a);
		for(k = 0; *a != ' ' && *a != '\0' && k < Namelen - 1; k++)
			name[k] = *a++;
		name[k] = '\0';
		f = find(name);
		if(f == nil || f->data == nil)
			lprint("host %s missing\n", name);
		else{
			lsnprint(w, sizeof w, "host");
			{
				char lab[Namelen + 8];
				lsnprint(lab, sizeof lab, "host %s", name);
				quoted(lab, f->data, f->n);
			}
		}
	}else
		badargs();
}

/* signed integer token */
static int
nextsint(char **pp, int *v)
{
	char *p;
	int neg, rc;

	p = skip(*pp);
	neg = 0;
	if(*p == '-'){
		neg = 1;
		p++;
	}
	rc = nextint(&p, v);
	if(rc == 0 && neg)
		*v = -*v;
	if(rc == 0)
		*pp = p;
	return rc;
}

/* click R C, drag R C, double R C, release, wheel N: mouse events on text cells */
static void
mouse(int kind, char *a)
{
	Ev ev;
	int r, c;

	memset(&ev, 0, sizeof ev);
	ev.kind = kind;
	if(kind == Ewheel){
		if(nextsint(&a, &ev.n) < 0){
			badargs();
			return;
		}
	}else if(kind != Erelease){
		if(nextint(&a, &r) < 0 || nextint(&a, &c) < 0){
			badargs();
			return;
		}
		ev.row = r;
		ev.col = c;
	}
	ed_event(&ed, &ev);
}

static void
cmd1(char *l)
{
	char w[32], *s, name[Namelen], *a;
	int k, n, r, c;
	File *f;

	a = l;
	for(k = 0; *a != ' ' && *a != '\0' && k < 31; k++)
		w[k] = *a++;
	w[k] = '\0';
	if(strcmp(w, "click") == 0)
		mouse(Eclick, a);
	else if(strcmp(w, "drag") == 0)
		mouse(Edrag, a);
	else if(strcmp(w, "double") == 0)
		mouse(Edouble, a);
	else if(strcmp(w, "release") == 0)
		mouse(Erelease, a);
	else if(strcmp(w, "wheel") == 0)
		mouse(Ewheel, a);
	else if(strcmp(w, "size") == 0 || strcmp(w, "resize") == 0){
		if(nextint(&a, &r) < 0 || nextint(&a, &c) < 0){
			badargs();
			return;
		}
		canvas_resize(&cv, r, c);
		ed_resize(&ed, r, c);
	}else if(strcmp(w, "gutter") == 0){
		if(nextint(&a, &n) < 0){
			badargs();
			return;
		}
		ed.gutter = n;
		ed_layout(&ed);
		ed_reveal(&ed);
	}else if(strcmp(w, "width") == 0){
		a = skip(a);
		cellmode = strcmp(a, "eaw") == 0 ? Weaw : Wascii;
	}else if(strcmp(w, "file") == 0){
		a = skip(a);
		for(k = 0; *a != ' ' && *a != '\0' && k < Namelen - 1; k++)
			name[k] = *a++;
		name[k] = '\0';
		if(nextquoted(&a, &s, &n) < 0){
			badargs();
			return;
		}
		f = slot(name);
		if(f == nil){
			free(s);
			lprint("error too many files\n");
			return;
		}
		free(f->data);
		f->data = s;
		f->n = n;
		f->big = 0;
	}else if(strcmp(w, "filerep") == 0){
		char *chunk, *suf;
		int cn, reps, i, sn;

		a = skip(a);
		for(k = 0; *a != ' ' && *a != '\0' && k < Namelen - 1; k++)
			name[k] = *a++;
		name[k] = '\0';
		if(nextint(&a, &reps) < 0 || nextquoted(&a, &chunk, &cn) < 0){
			badargs();
			return;
		}
		f = slot(name);
		if(f == nil){
			free(chunk);
			lprint("error too many files\n");
			return;
		}
		free(f->data);
		suf = nil;
		sn = 0;
		if(nextquoted(&a, &suf, &sn) < 0)
			sn = 0;
		f->data = malloc(cn * reps + sn + 1);
		for(i = 0; i < reps; i++)
			memmove(f->data + i * cn, chunk, cn);
		if(sn > 0)
			memmove(f->data + cn * reps, suf, sn);
		free(suf);
		f->n = cn * reps + sn;
		f->big = 0;
		free(chunk);
	}else if(strcmp(w, "snarf") == 0){
		a = skip(a);
		if(strncmp(a, "set ", 4) == 0){
			a += 4;
			if(nextquoted(&a, &s, &n) < 0){
				badargs();
				return;
			}
			setsnarf(s, n);
		}else if(strcmp(a, "absent") == 0)
			snarf.present = 0;
		else if(strcmp(a, "present") == 0)
			snarf.present = 1;
		else if(strcmp(a, "rio") == 0)
			snarf.rio = 1;
		else if(strcmp(a, "exact") == 0)
			snarf.rio = 0;
		else if(strncmp(a, "cap ", 4) == 0){
			a += 4;
			if(nextint(&a, &n) < 0){
				badargs();
				return;
			}
			snarf.cap = n;
		}else
			badargs();
	}else if(strcmp(w, "extmod") == 0 || strcmp(w, "extvers") == 0 || strcmp(w, "extmtime") == 0
	|| strcmp(w, "extdel") == 0 || strcmp(w, "fmode") == 0 || strcmp(w, "perm") == 0
	|| strcmp(w, "failwrite") == 0){
		a = skip(a);
		for(k = 0; *a != ' ' && *a != '\0' && k < Namelen - 1; k++)
			name[k] = *a++;
		name[k] = '\0';
		f = find(name);
		if(f == nil && strcmp(w, "extmod") == 0)
			f = slot(name);	/* another program creates the file */
		if(f == nil){
			lprint("error no such file\n");
			return;
		}
		a = skip(a);
		if(strcmp(w, "extmod") == 0){
			if(nextquoted(&a, &s, &n) < 0){
				badargs();
				return;
			}
			free(f->data);
			f->data = s;
			f->n = n;
			f->vers++;
			fclock += 2;
			f->mtime = fclock;
		}else if(strcmp(w, "extvers") == 0)
			f->vers++;
		else if(strcmp(w, "extmtime") == 0){
			if(nextint(&a, &n) < 0){
				badargs();
				return;
			}
			f->mtime += n;
		}else if(strcmp(w, "extdel") == 0){
			free(f->data);
			f->data = nil;
			f->n = 0;
			f->used = 0;
		}else if(strcmp(w, "fmode") == 0){
			if(strcmp(a, "plain") == 0){
				f->kind = Sfile;
				f->append = 0;
			}else if(strcmp(a, "append") == 0){
				f->kind = Sfile;
				f->append = 1;
			}else if(strcmp(a, "dir") == 0){
				f->kind = Sdir;
				f->append = 0;
			}else
				badargs();
		}else if(strcmp(w, "perm") == 0){
			n = 0;
			while(*a >= '0' && *a <= '7')
				n = n*8 + (*a++ - '0');
			f->perm = n;
		}else
			f->failwrite = strcmp(a, "on") == 0;
	}else if(strcmp(w, "bigfile") == 0){
		a = skip(a);
		f = slot(a);
		if(f != nil)
			f->big = 1;
	}else if(strcmp(w, "open") == 0){
		a = skip(a);
		if(ed_open(&ed, a) < 0)
			lprint("open -1 %s\n", ed.msg);
		else
			lprint("open 0\n");
	}else if(strcmp(w, "setsel") == 0){
		setsel(a);
	}else if(strcmp(w, "slice") == 0){
		if(nextint(&a, &n) < 0 || n <= 0){
			badargs();
			return;
		}
		ed.slice = n;
		rx_winsize = n;
	}else if(strcmp(w, "rxfail") == 0){
		/* rxfail N: N scan window allocations succeed, then they fail; rxfail off */
		if(strcmp(skip(a), "off") == 0)
			rx_failwin = -1;
		else if(nextint(&a, &n) < 0){
			badargs();
			return;
		}else
			rx_failwin = n;
	}else if(strcmp(w, "maxedits") == 0){
		if(nextint(&a, &n) < 0 || n <= 0){
			badargs();
			return;
		}
		exec_maxedits = n;
	}else if(strcmp(w, "bulkmin") == 0){
		if(nextint(&a, &n) < 0 || n < 0){
			badargs();
			return;
		}
		doc_bulkmin = n;
	}else if(strcmp(w, "bufmax") == 0){
		if(nextint(&a, &n) < 0 || n < 0){
			badargs();
			return;
		}
		ed.doc.b->max = n;
	}else if(strcmp(w, "autowork") == 0){
		a = skip(a);
		autowork = strcmp(a, "off") != 0;
	}else if(strcmp(w, "work") == 0){
		if(nextint(&a, &n) < 0)
			n = -1;
		while(n != 0 && ed_work(&ed) != Jdone)
			n--;
	}else if(strcmp(w, "keys") == 0){
		if(nextquoted(&a, &s, &n) < 0){
			badargs();
			return;
		}
		keys(s, n);
		free(s);
	}else if(strcmp(w, "dump") == 0)
		dump(a);
	else
		lprint("error unknown command\n");
	(void)err;
}

/* after input commands, run an unfinished job to its end unless autowork is off */
static void
cmd(char *l)
{
	cmd1(l);
	if(autowork)
		while(ed_work(&ed) != Jdone)
			;
}

static int
run(void)
{
	int rc, i;

	host.exists = h_exists;
	host.readfile = h_read;
	host.writefile = h_write;
	host.stat = h_stat;
	host.snarfget = h_snarfget;
	host.snarfput = h_snarfput;
	host.aux = nil;
	ed_init(&ed, &host, tcells, &cellmode);
	ed.gutter = 0;
	canvas_resize(&cv, 24, 80);
	ed_resize(&ed, 24, 80);
	rc = script(LINEMAX, cmd);
	for(i = 0; i < Nfile; i++)
		free(files[i].data);
	free(snarf.data);
	canvas_free(&cv);
	ed_free(&ed);
	return rc;
}

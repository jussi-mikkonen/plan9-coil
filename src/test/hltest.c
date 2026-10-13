#include "compat.h"
#include "coil.h"
#include "script.h"

/* hltest: highlighter driver. Lexers, language detection and the checkpoint cache, edits and the differential check. */

static Lang *lang;
static Doc d;
static Hl h;

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


/* the legend of one slot, shared with the edtest screen dump */
static int
letter(int slot)
{
	switch(slot){
	case Sdefault: return '.';
	case Sstatus: return '=';
	case Smessage: return '!';
	case Sctrl: return '^';
	case Sinvalid: return '?';
	case Skeyword: return 'k';
	case Stype: return 't';
	case Scomment: return 'c';
	case Sstring: return 's';
	case Snumber: return 'n';
	case Spreproc: return 'p';
	case Svariable: return 'v';
	case Starget: return 'T';
	case Srecipe: return 'r';
	case Sheading: return 'H';
	case Semph: return 'e';
	case Sstrong: return 'S';
	case Scode: return 'C';
	case Slink: return 'l';
	case Slistmark: return 'm';
	case Sbracket: return 'M';
	case Strailing: return '~';
	case Slinenr: return '0';
	}
	return '*';
}

static void
showtext(uchar *s, int n)
{
	int i;

	for(i = 0; i < n; i++){
		if(s[i] == '\\')
			lprint("\\\\");
		else if(s[i] >= 0x20 && s[i] < 0x7f)
			lprint("%c", s[i]);
		else
			lprint("\\x%02x", s[i]);
	}
}

static void
doLang(char *a)
{
	Lang *l;
	char *name;

	name = skip(a);
	l = lang_by_name(name);
	if(l == nil){
		lprint("lang %s nil\n", name);
		return;
	}
	lang = l;
	hl_reset(&h, lang);
	lprint("lang %s tab %d %s\n", name, l->tabwidth, l->usetabs ? "tabs" : "spaces");
}

static void
doDetect(char *a)
{
	char *name, *text;
	int nn, nt;
	Buf *b;
	Lang *l;
	char err[64];

	if(nextquoted(&a, &name, &nn) < 0 || nextquoted(&a, &text, &nt) < 0){
		badargs();
		return;
	}
	name[nn] = '\0';
	b = buf_new();
	if(buf_load(b, text, nt, err, sizeof err) < 0){
		lprint("error %s\n", err);
		free(name);
		buf_free(b);
		return;
	}
	l = lang_detect(nn == 0 ? nil : name, b);
	lprint("detect %s %s\n", name, l->name);
	free(name);
	buf_free(b);
}

static void
doLex(char *a)
{
	char *s;
	int n, i;
	uchar *out;
	Hstate st;

	if(nextquoted(&a, &s, &n) < 0){
		badargs();
		return;
	}
	out = malloc(n + 1);
	st = 0;
	for(i = 0; i < n; i++)
		out[i] = Sdefault;
	if(lang != nil && lang->lex != nil)
		lang->lex(&st, (uchar*)s, n, 1, out);
	lprint("l ");
	showtext((uchar*)s, n);
	lprint("\ns ");
	for(i = 0; i < n; i++)
		lprint("%c", letter(out[i]));
	lprint("\nst %x\n", (int)st);
	free(out);
	free(s);
}

static void
newdoc(char *data, int n)
{
	char err[64];

	if(doc_load(&d, data, n, err, sizeof err) < 0)
		lprint("error %s\n", err);
	hl_reset(&h, lang);
}

/* last real line: an empty line after a final newline does not count */
static int
lastreal(void)
{
	int last;
	Buf *b;

	b = d.b;
	last = buf_nlines(b) - 1;
	if(buf_len(b) > 0 && buf_byte(b, buf_len(b) - 1) == '\n')
		last--;
	return last;
}

static void
doLoad(char *a)
{
	char *s;
	int n;

	if(nextquoted(&a, &s, &n) < 0){
		badargs();
		return;
	}
	newdoc(s, n);
}

/* genlines N "TEXT" ["HEAD"]: HEAD, then N lines of TEXT */
static void
doGenlines(char *a)
{
	char *t, *hd, *s;
	int n, nt, nh, i;

	hd = nil;
	nh = 0;
	if(nextint(&a, &n) < 0 || nextquoted(&a, &t, &nt) < 0){
		badargs();
		return;
	}
	if(*skip(a) != '\0' && nextquoted(&a, &hd, &nh) < 0){
		badargs();
		return;
	}
	s = malloc(nh + n * (nt + 1) + 1);
	if(nh > 0)
		memmove(s, hd, nh);
	for(i = 0; i < n; i++){
		memmove(s + nh + i * (nt + 1), t, nt);
		s[nh + i * (nt + 1) + nt] = '\n';
	}
	newdoc(s, nh + n * (nt + 1));
	free(t);
	free(hd);
}

/* print lines from..to like render does: state of the first line, then hl_line per line */
static void
showlines(int from, int to)
{
	Hstate st;
	Pos ls;
	int l, n, i, m;
	uchar slots[Hlcap];
	char text[8192];

	hl_sync(&h, &d);
	if(to > lastreal())
		to = lastreal();
	st = hl_state(&h, d.b, from);
	ls = buf_line_start(d.b, from);
	for(l = from; l <= to; l++){
		n = hl_line(&h, d.b, ls, &st, slots);
		m = h.next - ls;
		if(m > 0 && buf_byte(d.b, h.next - 1) == '\n')
			m--;
		if(m > (int)sizeof text)
			m = sizeof text;
		buf_read(d.b, ls, text, m);
		lprint("l %02d ", l);
		showtext((uchar*)text, m);
		lprint("\ns %02d ", l);
		for(i = 0; i < m; i++)
			lprint("%c", i < n ? letter(slots[i]) : '.');
		lprint("\n");
		ls = h.next;
	}
}

static void
doHl(char *a)
{
	int l;

	a = skip(a);
	if(strcmp(a, "all") == 0)
		showlines(0, lastreal());
	else if(nextint(&a, &l) == 0)
		showlines(l, l);
	else
		badargs();
}

static void
doEdit(char *a)
{
	int pos, ndel, n;
	char *s;
	Edit e;

	if(nextint(&a, &pos) < 0 || nextint(&a, &ndel) < 0 || nextquoted(&a, &s, &n) < 0){
		badargs();
		return;
	}
	e.pos = pos;
	e.ndel = ndel;
	e.ins = s;
	e.nins = n;
	if(doc_apply(&d, &e, 1, nil) < 0)
		lprint("edit -1 %s\n", d.err);
	free(s);
}

/* the cached path against a full lex from line 0 with an unbounded look-back: -1 when equal, else the first differing line */
static int
checkall(void)
{
	Hl r;
	Hstate sr, srin, sc, sc2;
	Pos ls;
	int l, last, nr, nc, i, save, bad;
	uchar a[Hlcap], c[Hlcap];

	save = h.nlexed;
	hl_init(&r);
	r.look = 1 << 30;
	r.cap = h.cap;
	hl_reset(&r, h.lang);
	hl_sync(&h, &d);
	last = lastreal();
	sr = 0;
	ls = 0;
	bad = -1;
	for(l = 0; l <= last && bad < 0; l++){
		sc = hl_state(&h, d.b, l);
		sc2 = sc;
		nc = hl_line(&h, d.b, ls, &sc2, c);
		srin = sr;
		nr = hl_line(&r, d.b, ls, &sr, a);
		if(sc != srin || nc != nr || sc2 != sr)
			bad = l;
		else
			for(i = 0; i < nr; i++)
				if(a[i] != c[i] || a[i] >= Nslot)
					bad = l;
		ls = r.next;
	}
	hl_free(&r);
	h.nlexed = save;
	return bad;
}

static void
doCheck(char *a)
{
	int l;

	(void)a;
	l = checkall();
	if(l < 0)
		lprint("check ok\n");
	else
		lprint("check line %d differs\n", l);
}

static void
doState(char *a)
{
	int l;

	if(nextint(&a, &l) < 0){
		badargs();
		return;
	}
	hl_sync(&h, &d);
	lprint("state %d %x\n", l, (int)hl_state(&h, d.b, l));
}

/* bracket POS: the partner of the bracket at POS */
static void
doBracket(char *a)
{
	int v;

	if(nextint(&a, &v) < 0){
		badargs();
		return;
	}
	lprint("bracket %d -> %d\n", v, (int)hl_match(&h, d.b, v));
}

static void
doLexed(char *a)
{
	(void)a;
	lprint("lexed %d\n", h.nlexed);
	h.nlexed = 0;
}

static void
doLook(char *a)
{
	int v;

	if(nextint(&a, &v) < 0){
		badargs();
		return;
	}
	h.look = v;
}

static void
doCap(char *a)
{
	int v;

	if(nextint(&a, &v) < 0){
		badargs();
		return;
	}
	if(v > Hlcap)
		v = Hlcap;
	h.cap = v;
	hl_reset(&h, lang);
}

static void
doUndo(char *a)
{
	(void)a;
	if(doc_undo(&d) < 0)
		lprint("error %s\n", d.err);
}

static void
doRedo(char *a)
{
	(void)a;
	if(doc_redo(&d) < 0)
		lprint("error %s\n", d.err);
}

/* ---- fuzz: random edits and undo/redo, cached highlight against a full lex ---- */

typedef struct Tok Tok;
struct Tok {
	char	*s;
	int	n;
};

static Tok toks[] = {
	{"/*", 2}, {"*/", 2}, {"\"", 1}, {"'", 1}, {"\\", 1}, {"\n", 1}, {"#", 1}, {"`", 1},
	{"~~~", 3}, {"$", 1}, {"*", 1}, {"_", 1}, {"[", 1}, {"]", 1}, {"(", 1}, {")", 1},
	{"{", 1}, {"}", 1}, {"x", 1}, {" ", 1}, {"\t", 1}, {"\xff", 1}, {"\0", 1}, {"//", 2},
	{"\r\n", 2}, {"int ", 4}, {"\\\n", 2}
};

static u32int rstate;

static int
rnd(int n)
{
	rstate = rstate * 1103515245 + 12345;
	return (int)((rstate >> 16) & 0x7fff) % n;
}

static void
fuzz(int seed, int ops)
{
	static char unit[] = "int a;\n/* c */\n#define X \\\n  1\nstatic char *s = \"x\";\n// z\n";
	char *data;
	int nu, i, k, len, ndel, r, l, bad, nl;
	Edit e;
	Hstate st;
	Tok *t;

	rstate = seed;
	nu = strlen(unit);
	data = malloc(nu * 100 + 1);
	for(i = 0; i < 100; i++)
		memmove(data + i * nu, unit, nu);
	newdoc(data, nu * 100);
	for(k = 0; k < ops; k++){
		r = rnd(8);
		if(rnd(2) == 0)
			doc_seal(&d);
		len = buf_len(d.b);
		if(r == 0){
			if(rnd(2) == 0)
				doc_undo(&d);
			else
				doc_redo(&d);
		}else{
			e.pos = rnd(len + 1);
			if(r <= 3 && len > 0){
				ndel = 1 + rnd(5);
				if(e.pos + ndel > len)
					e.pos = len - ndel < 0 ? 0 : len - ndel;
				if(ndel > len)
					ndel = len;
				e.ndel = ndel;
				e.ins = nil;
				e.nins = 0;
			}else{
				t = &toks[rnd(sizeof toks / sizeof toks[0])];
				e.ndel = 0;
				e.ins = t->s;
				e.nins = t->n;
			}
			doc_apply(&d, &e, 1, nil);
		}
		hl_sync(&h, &d);
		nl = lastreal();
		for(i = 0; i < 3; i++){
			l = rnd(nl + 1);
			st = hl_state(&h, d.b, l);
			(void)st;
		}
		if((k + 1) % 50 == 0 || k == ops - 1){
			bad = checkall();
			if(bad >= 0){
				lprint("fuzz %d op %d line %d differs\n", seed, k, bad);
				return;
			}
		}
	}
	lprint("fuzz %d %d ok\n", seed, ops);
}

static void
doFuzz(char *a)
{
	int seed, n;

	if(nextint(&a, &seed) < 0 || nextint(&a, &n) < 0){
		badargs();
		return;
	}
	fuzz(seed, n);
}

static void
cmd(char *a)
{
	char *w;

	a = skip(a);
	w = a;
	while(*a != '\0' && *a != ' ')
		a++;
	if(*a == ' '){
		*a = '\0';
		a++;
	}
	if(strcmp(w, "lang") == 0)
		doLang(a);
	else if(strcmp(w, "detect") == 0)
		doDetect(a);
	else if(strcmp(w, "lex") == 0)
		doLex(a);
	else if(strcmp(w, "load") == 0)
		doLoad(a);
	else if(strcmp(w, "genlines") == 0)
		doGenlines(a);
	else if(strcmp(w, "hl") == 0)
		doHl(a);
	else if(strcmp(w, "edit") == 0)
		doEdit(a);
	else if(strcmp(w, "seal") == 0)
		doc_seal(&d);
	else if(strcmp(w, "undo") == 0)
		doUndo(a);
	else if(strcmp(w, "redo") == 0)
		doRedo(a);
	else if(strcmp(w, "check") == 0)
		doCheck(a);
	else if(strcmp(w, "state") == 0)
		doState(a);
	else if(strcmp(w, "bracket") == 0)
		doBracket(a);
	else if(strcmp(w, "lexed") == 0)
		doLexed(a);
	else if(strcmp(w, "look") == 0)
		doLook(a);
	else if(strcmp(w, "cap") == 0)
		doCap(a);
	else if(strcmp(w, "fuzz") == 0)
		doFuzz(a);
	else
		lprint("error unknown command\n");
}

static int
run(void)
{
	int rc;

	lang = lang_by_name("plain");
	doc_init(&d);
	hl_init(&h);
	hl_reset(&h, lang);
	rc = script(LINEMAX, cmd);
	hl_free(&h);
	doc_free(&d);
	return rc;
}

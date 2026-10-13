#include "compat.h"
#include "coil.h"
#include "script.h"

/* doctest: command driver for Doc history (undo/redo/dirty) plus the seeded fuzz oracle.
 * Output only via lprint. The fuzz uses its own LCG (never rand) so Linux and 9front agree. */

static Doc d;
/* not #define b (d.b): 6c's preprocessor re-expands a macro named in its own body */
static Buf*
dbuf(void)
{
	return d.b;
}
#define b dbuf()
#define ss (d.sel)
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

static void
dump(void)
{
	int i, n;
	char *s;

	n = buf_len(b);
	s = malloc(n + 1);
	buf_read(b, 0, s, n);
	s[n] = '\0';
	for(i = 0; i < n; i++)
		if(s[i] == '\n')
			s[i] = '|';
	lprint("text %s\n", s);
	free(s);
	lprint("sel");
	for(i = 0; i < ss.n; i++)
		lprint(" [%d,%d)%s", ss.s[i].anchor, ss.s[i].head, i == ss.primary ? "*" : "");
	lprint("\n");
	lprint("modified %d\n", doc_modified(&d));
	lprint("hist cur %d n %d saved %d\n", d.cur, d.nh, d.saved);
}

/* ---- fuzz: naive model with a delta log per transaction ---- */

enum { Mcap = 4 << 20, Bigins = 70000 };

typedef struct MRec MRec;
typedef struct MTxn MTxn;

struct MRec {
	int	pos;
	int	ndel;
	int	nins;
	char	*del;
	char	*ins;
};

struct MTxn {
	MRec	*r;
	int	nr;
	int	cap;
	SelSet	before;
	SelSet	after;
};

static unsigned int lcg;
static char *mt;
static int mn;
static MTxn *mh;
static int nmh, mhcap, mcur, msaved, mopen;

static int
rnd(int n)
{
	unsigned int a, c;

	lcg = lcg * 1103515245u + 12345u;
	a = (lcg >> 16) & 0x7fff;
	lcg = lcg * 1103515245u + 12345u;
	c = (lcg >> 16) & 0x7fff;
	return (int)(((a << 15) | c) % (unsigned int)n);
}

static void
mtxnfree(MTxn *t)
{
	int i;

	for(i = 0; i < t->nr; i++){
		free(t->r[i].del);
		free(t->r[i].ins);
	}
	free(t->r);
	sel_free(&t->before);
	sel_free(&t->after);
}

static void
mdroptail(void)
{
	while(nmh > mcur)
		mtxnfree(&mh[--nmh]);
	if(msaved > mcur)
		msaved = -1;
}

/* replace [pos,pos+ndel) of the model text by s[0..n) */
static void
mreplace(int pos, int ndel, char *s, int n)
{
	memmove(mt + pos + n, mt + pos + ndel, mn - pos - ndel);
	if(n > 0)
		memmove(mt + pos, s, n);
	mn += n - ndel;
}

static int
iscont(int c)
{
	return (c & 0xC0) == 0x80;
}

/* random rune-aligned position */
static int
rpos(void)
{
	int p;

	p = rnd(mn + 1);
	while(p > 0 && p < mn && iscont((uchar)mt[p]))
		p--;
	return p;
}

/* snap q (pos <= q <= lim, lim rune aligned) back to a rune start */
static int
qsnap(int pos, int q)
{
	while(q > pos && q < mn && iscont((uchar)mt[q]))
		q--;
	return q;
}

static int
genins(char **out)
{
	static char *alpha[] = {"a", "b", "x", " ", "\n", "\xc3\xa9", "\xe2\x82\xac", "\t", "\xf0\x9f\x98\x80"};
	char *s;
	int n, k, i, l;

	if(rnd(200) == 0){
		s = malloc(Bigins);
		for(i = 0; i < Bigins; i++)
			s[i] = i % 61 == 60 ? '\n' : 'k';
		*out = s;
		return Bigins;
	}
	k = 1 + rnd(8);
	s = malloc(k * 4 + 1);
	n = 0;
	for(i = 0; i < k; i++){
		l = rnd((int)(sizeof alpha / sizeof alpha[0]));
		memmove(s + n, alpha[l], strlen(alpha[l]));
		n += strlen(alpha[l]);
	}
	*out = s;
	return n;
}

static int
selsame(SelSet *a, SelSet *c)
{
	int i;

	if(a->n != c->n || a->primary != c->primary)
		return 0;
	for(i = 0; i < a->n; i++)
		if(a->s[i].anchor != c->s[i].anchor || a->s[i].head != c->s[i].head)
			return 0;
	return 1;
}

static void
setsel(void)
{
	int i, k, a, h;

	k = 1 + rnd(4);
	ss.n = 0;
	for(i = 0; i < k; i++){
		a = rpos();
		h = rnd(3) == 0 ? a : rpos();
		sel_add(&ss, a, h);
	}
	ss.primary = rnd(k);
	sel_normalize(&ss, b);
}

/* compare doc and model; full also compares every byte and runs buf_check */
static int
compare(int full, char *why, int nwhy)
{
	char *s;
	int r;

	if(doc_modified(&d) != (mcur != msaved)){
		lsnprint(why, nwhy, "modified %d, model %d", doc_modified(&d), mcur != msaved);
		return -1;
	}
	if(d.cur != mcur || d.nh != nmh || d.saved != msaved || d.open != mopen){
		lsnprint(why, nwhy, "history cur %d n %d saved %d open %d, model %d %d %d %d",
			d.cur, d.nh, d.saved, d.open, mcur, nmh, msaved, mopen);
		return -1;
	}
	if(buf_len(b) != mn){
		lsnprint(why, nwhy, "length %d, model %d", buf_len(b), mn);
		return -1;
	}
	if(sel_check(&ss, b, why, nwhy) < 0)
		return -1;
	if(full){
		s = malloc(mn + 1);
		buf_read(b, 0, s, mn);
		r = memcmp(s, mt, mn);
		free(s);
		if(r != 0){
			lsnprint(why, nwhy, "bytes differ");
			return -1;
		}
		if(buf_check(b, why, nwhy) < 0)
			return -1;
	}
	return 0;
}

static MTxn*
mpush(void)
{
	MTxn *t;

	mdroptail();
	if(nmh == mhcap){
		mhcap = mhcap == 0 ? 64 : mhcap * 2;
		mh = realloc(mh, mhcap * sizeof(MTxn));
	}
	t = &mh[nmh++];
	memset(t, 0, sizeof *t);
	sel_init(&t->before);
	sel_init(&t->after);
	sel_copy(&t->before, &ss);
	mopen = 1;
	mcur = nmh;
	return t;
}

/* apply a batch to doc and model; 1 = skipped (model cap), 0 ok, -1 failure */
static int
doedit(Edit *e, int n, char *why, int nwhy)
{
	SelSet before;
	MTxn *t;
	MRec *r;
	int i, total;

	total = mn;
	for(i = 0; i < n; i++)
		total += e[i].nins - e[i].ndel;
	if(total > Mcap)
		return 1;
	sel_init(&before);
	sel_copy(&before, &ss);
	if(doc_apply(&d, e, n, nil) < 0){
		lsnprint(why, nwhy, "doc_apply: %s", d.err);
		sel_free(&before);
		return -1;
	}
	if(!mopen){
		t = mpush();
		sel_copy(&t->before, &before);
	}else
		t = &mh[nmh - 1];
	sel_free(&before);
	if(t->nr + n > t->cap){
		t->cap = (t->nr + n) * 2;
		t->r = realloc(t->r, t->cap * sizeof(MRec));
	}
	for(i = n - 1; i >= 0; i--){
		r = &t->r[t->nr++];
		r->pos = e[i].pos;
		r->ndel = e[i].ndel;
		r->nins = e[i].nins;
		r->del = malloc(e[i].ndel + 1);
		memmove(r->del, mt + e[i].pos, e[i].ndel);
		r->ins = malloc(e[i].nins + 1);
		if(e[i].nins > 0)
			memmove(r->ins, e[i].ins, e[i].nins);
		mreplace(e[i].pos, e[i].ndel, e[i].ins, e[i].nins);
	}
	sel_copy(&t->after, &ss);
	return 0;
}

/* k sorted, non-overlapping edits (k <= 64) */
static int
genbatch(Edit *e, int k)
{
	int i, p[64], t, lim;

	for(i = 0; i < k; i++)
		p[i] = rpos();
	for(i = 1; i < k; i++)
		for(t = i; t > 0 && p[t-1] > p[t]; t--){
			lim = p[t]; p[t] = p[t-1]; p[t-1] = lim;
		}
	for(i = 0; i < k; i++){
		lim = i + 1 < k ? p[i+1] : mn;
		e[i].pos = p[i];
		t = p[i] + rnd(6);
		if(t > lim)
			t = lim;
		t = qsnap(p[i], t);
		e[i].ndel = t - p[i];
		e[i].ins = nil;
		e[i].nins = 0;
		if(rnd(3) != 0 || e[i].ndel == 0)
			e[i].nins = genins(&e[i].ins);
	}
	return k;
}

/* generate edits into e (max 3); returns count (0 = nothing to do); *name is the op name */
static int
genedit(Edit *e, char **name)
{
	int roll, t;

	roll = rnd(70);
	if(roll < 45){
		*name = "ins";
		e[0].pos = rpos();
		e[0].ndel = 0;
		e[0].nins = genins(&e[0].ins);
		return 1;
	}
	if(roll < 60){
		*name = "del";
		e[0].pos = rpos();
		t = e[0].pos + (rnd(50) == 0 ? rnd(100000) : rnd(40));
		if(t > mn)
			t = mn;
		t = qsnap(e[0].pos, t);
		if(t == e[0].pos)
			return 0;
		e[0].ndel = t - e[0].pos;
		e[0].ins = nil;
		e[0].nins = 0;
		return 1;
	}
	*name = "batch";
	return genbatch(e, 2 + rnd(2));
}

static void
freeedit(Edit *e, int n)
{
	int i;

	for(i = 0; i < n; i++)
		if(e[i].nins > 0)
			free(e[i].ins);
}

static void
fuzz(int seed, int ops, int multi, int big)
{
	static char initial[] = "hello\nw\xc3\xb6rld\n\xe2\x82\xac end\n";
	char why[160], *name;
	Edit e[64];
	int op, n, r, i, full, last;
	MTxn *t;
	MRec *m;

	lcg = (unsigned int)seed;
	mt = malloc(Mcap + 64);
	mn = sizeof initial - 1;
	memmove(mt, initial, mn);
	mh = nil;
	nmh = mhcap = mcur = mopen = 0;
	msaved = 0;
	{
		char *copy;

		copy = malloc(mn + 1);
		memmove(copy, initial, mn);
		if(doc_load(&d, copy, mn, err, sizeof err) < 0){
			lprint("fuzz seed %d op 0 load: %s\n", seed, err);
			free(mt);
			return;
		}
	}
	why[0] = '\0';
	name = "start";
	for(op = 1; op <= ops; op++){
		if(multi && rnd(3) == 0)
			setsel();
		r = rnd(100);
		n = 0;
		if(r < 70){
			if(big && rnd(2) == 0){
				name = "bigbatch";
				n = genbatch(e, 17 + rnd(48));
			}else
				n = genedit(e, &name);
			if(n > 0){
				i = doedit(e, n, why, sizeof why);
				freeedit(e, n);
				if(i < 0)
					goto bad;
			}
		}else if(r < 75){
			name = "seal";
			doc_seal(&d);
			mopen = 0;
		}else if(r < 87){
			name = "undo";
			mopen = 0;
			i = doc_undo(&d);
			if(mcur == 0){
				if(i == 0){
					lsnprint(why, sizeof why, "undo succeeded on empty history");
					goto bad;
				}
			}else{
				if(i < 0){
					lsnprint(why, sizeof why, "undo failed: %s", d.err);
					goto bad;
				}
				t = &mh[mcur - 1];
				for(i = t->nr - 1; i >= 0; i--){
					m = &t->r[i];
					mreplace(m->pos, m->nins, m->del, m->ndel);
				}
				mcur--;
				if(!selsame(&ss, &t->before)){
					lsnprint(why, sizeof why, "selection after undo differs from the step's before");
					goto bad;
				}
			}
		}else if(r < 94){
			name = "redo";
			mopen = 0;
			i = doc_redo(&d);
			if(mcur == nmh){
				if(i == 0){
					lsnprint(why, sizeof why, "redo succeeded with nothing to redo");
					goto bad;
				}
			}else{
				if(i < 0){
					lsnprint(why, sizeof why, "redo failed: %s", d.err);
					goto bad;
				}
				t = &mh[mcur];
				for(i = 0; i < t->nr; i++){
					m = &t->r[i];
					mreplace(m->pos, m->ndel, m->ins, m->nins);
				}
				mcur++;
				if(!selsame(&ss, &t->after)){
					lsnprint(why, sizeof why, "selection after redo differs from the step's after");
					goto bad;
				}
			}
		}else{
			name = "save";
			doc_mark_saved(&d);
			mopen = 0;
			msaved = mcur;
		}
		last = op == ops;
		full = mn < 65536 || op % 32 == 0 || last;
		if(compare(full, why, sizeof why) < 0)
			goto bad;
	}
	lprint("fuzz seed %d ops %d ok\n", seed, ops);
	goto done;
bad:
	lprint("fuzz seed %d op %d %s: %s\n", seed, op, name, why);
done:
	for(i = 0; i < nmh; i++)
		mtxnfree(&mh[i]);
	free(mh);
	mh = nil;
	free(mt);
}

static void
cmd(char *l)
{
	char w[32], *s, *a;
	int k, n, v1, v2;
	Edit e[16];

	a = l;
	for(k = 0; *a != ' ' && *a != '\0' && k < 31; k++)
		w[k] = *a++;
	w[k] = '\0';
	if(strcmp(w, "load") == 0){
		if(nextquoted(&a, &s, &n) < 0){
			badargs();
			return;
		}
		if(doc_load(&d, s, n, err, sizeof err) < 0)
			lprint("error %s\n", err);
	}else if(strcmp(w, "sel") == 0){
		ss.n = 0;
		while(nextint(&a, &v1) == 0){
			if(nextint(&a, &v2) < 0){
				badargs();
				return;
			}
			sel_add(&ss, v1, v2);
		}
		ss.primary = 0;
		if(ss.n == 0)
			badargs();
		else
			sel_normalize(&ss, b);
	}else if(strcmp(w, "ins") == 0){
		if(nextint(&a, &v1) < 0 || nextquoted(&a, &s, &n) < 0){
			badargs();
			return;
		}
		e[0].pos = v1;
		e[0].ndel = 0;
		e[0].ins = s;
		e[0].nins = n;
		if(doc_apply(&d, e, 1, nil) < 0)
			lprint("error %s\n", d.err);
		free(s);
	}else if(strcmp(w, "del") == 0){
		if(nextint(&a, &v1) < 0 || nextint(&a, &v2) < 0){
			badargs();
			return;
		}
		e[0].pos = v1;
		e[0].ndel = v2;
		e[0].ins = nil;
		e[0].nins = 0;
		if(doc_apply(&d, e, 1, nil) < 0)
			lprint("error %s\n", d.err);
	}else if(strcmp(w, "batch") == 0){
		n = 0;
		while(n < 16 && nextint(&a, &v1) == 0){
			if(nextint(&a, &v2) < 0 || nextquoted(&a, &s, &k) < 0){
				badargs();
				return;
			}
			e[n].pos = v1;
			e[n].ndel = v2;
			e[n].ins = s;
			e[n].nins = k;
			n++;
		}
		if(doc_apply(&d, e, n, nil) < 0)
			lprint("error %s\n", d.err);
		while(n > 0)
			free(e[--n].ins);
	}else if(strcmp(w, "bulkmin") == 0){
		if(nextint(&a, &v1) < 0 || v1 < 0){
			badargs();
			return;
		}
		doc_bulkmin = v1;
	}else if(strcmp(w, "seal") == 0)
		doc_seal(&d);
	else if(strcmp(w, "undo") == 0){
		if(doc_undo(&d) < 0)
			lprint("error %s\n", d.err);
	}else if(strcmp(w, "redo") == 0){
		if(doc_redo(&d) < 0)
			lprint("error %s\n", d.err);
	}else if(strcmp(w, "save") == 0)
		doc_mark_saved(&d);
	else if(strcmp(w, "text") == 0 || strcmp(w, "dump") == 0)
		dump();
	else if(strcmp(w, "check") == 0){
		if(sel_check(&ss, b, err, sizeof err) < 0)
			lprint("check bad %s\n", err);
		else if(buf_check(b, err, sizeof err) < 0)
			lprint("check bad %s\n", err);
		else
			lprint("check ok\n");
	}else if(strcmp(w, "fuzz") == 0){
		if(nextint(&a, &v1) < 0 || nextint(&a, &v2) < 0){
			badargs();
			return;
		}
		a = skip(a);
		fuzz(v1, v2, strcmp(a, "multi") == 0 || strcmp(a, "big") == 0, strcmp(a, "big") == 0);
	}else
		lprint("error unknown command\n");
}

static int
run(void)
{
	int rc;

	doc_init(&d);
	rc = script(LINEMAX, cmd);
	doc_free(&d);
	return rc;
}

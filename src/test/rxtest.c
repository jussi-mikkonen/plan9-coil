#include "compat.h"
#include "coil.h"
#include "script.h"

/* rxtest: regexp seam driver: translate, compile, and (with the scan commands) windowed matching */

static char err[128];
static Buf *tb;
static int budget0;

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

static void
docomp(char *a, int lit)
{
	char *s;
	int n;
	Pat p;

	if(nextquoted(&a, &s, &n) < 0){
		badargs();
		return;
	}
	s[n] = '\0';
	if(pat_comp(&p, s, lit, err, sizeof err) < 0)
		lprint("error %s\n", err);
	else{
		lprint("ok multiline %d\n", p.multiline);
		pat_free(&p);
	}
	free(s);
}

/* print all matches of src over [lo, hi) of the loaded text */
static void
dofind(char *src, int lo, int hi)
{
	Pat p;
	Scan sc;
	Match m;
	int k, r, bud, resumes;

	if(tb == nil){
		lprint("error no text\n");
		return;
	}
	if(pat_comp(&p, src, 0, err, sizeof err) < 0){
		lprint("error %s\n", err);
		return;
	}
	if(hi < 0)
		hi = buf_len(tb);
	scan_init(&sc, &p, tb, lo, hi);
	resumes = 0;
	for(;;){
		bud = budget0 > 0 ? budget0 : 1 << 30;
		r = scan_next(&sc, &bud, &m);
		if(r == Smore){
			resumes++;
			continue;
		}
		if(r == Sdone)
			break;
		lprint("m %d %d", m.sp[0], m.ep[0]);
		for(k = 1; k < Nsub; k++)
			if(m.sp[k] >= 0)
				lprint(" g%d %d %d", k, m.sp[k], m.ep[k]);
		lprint("\n");
	}
	lprint("done\n");
	if(resumes > 0)
		lprint("resumed %d\n", resumes);
	scan_free(&sc);
	pat_free(&p);
}

/* the oracle: whole buffer in one NUL-terminated string, same empty-match rule; fills out[] with start,end pairs */
static int
oracle(Buf *b, Pat *p, Pos *out, int max)
{
	char *t;
	Resub sub[Nsub];
	Pos n, pos, prevend, st, en;
	int k, cnt;

	n = buf_len(b);
	t = malloc(n + 2);
	buf_read(b, 0, t, n);
	for(k = 0; k < n; k++)
		if(t[k] == '\0')
			t[k] = 1;
	t[n] = '\0';
	pos = 0;
	prevend = -1;
	cnt = 0;
	while(pos <= n){
		memset(sub, 0, sizeof sub);
		sub[0].sp = t + pos;
		sub[0].ep = nil;
		if(regexec(p->rx, t, sub, Nsub) == 0)
			break;
		st = (Pos)(sub[0].sp - t);
		en = (Pos)(sub[0].ep - t);
		if(en == st && st == prevend){
			pos = st < n ? buf_next(b, st) : n + 1;
			continue;
		}
		if(cnt + 2 > max)
			break;
		out[cnt++] = st;
		out[cnt++] = en;
		prevend = en;
		if(en == st)
			pos = st < n ? buf_next(b, st) : n + 1;
		else
			pos = en;
	}
	free(t);
	return cnt;
}

static u32int seed;

static u32int
rnd(void)
{
	seed = seed * 1664525u + 1013904223u;
	return seed >> 8;
}

static char *pats[] = {"a", "ab", "^a", "a$", "^$", "a*", "(a|b)+", "a\nb", "[^a]+", ".", "b$", "^b*$", nil};
static int wins[] = {8, 16, 64};

static void
dofuzz(int sd, int ncase)
{
	char txt[160], *s;
	int len, line, c, i, j, w, k, cnt, rc, bud;
	Pos want[200];
	Pat p;
	Scan sc;
	Match m;
	Buf *b;
	Pos got[200];

	seed = (u32int)sd;
	for(i = 0; i < ncase; i++){
		len = 0;
		line = 0;
		k = rnd() % 100 + 10;
		while(len < k){
			c = rnd() % 9;
			if(line >= 5)
				c = 7;
			if(c == 7){
				txt[len++] = '\n';
				line = 0;
			}else if(c == 6){
				txt[len++] = '\0';
				line++;
			}else if(c == 5){
				txt[len++] = '\t';
				line++;
			}else if(c == 8){
				txt[len++] = (char)0xC3;
				txt[len++] = (char)0xA9;
				line += 2;
			}else{
				txt[len++] = c < 3 ? 'a' : 'b';
				line++;
			}
		}
		for(j = 0; pats[j] != nil; j++){
			if(pat_comp(&p, pats[j], 0, err, sizeof err) < 0){
				lprint("fuzz error %s\n", err);
				return;
			}
			b = buf_new();
			s = malloc(len + 1);
			memmove(s, txt, len);
			buf_load(b, s, len, err, sizeof err);
			cnt = oracle(b, &p, want, 200);
			for(w = 0; w < 3; w++){
				rx_winsize = wins[w];
				scan_init(&sc, &p, b, 0, buf_len(b));
				rc = 0;
				for(;;){
					bud = 1 << 30;
					k = scan_next(&sc, &bud, &m);
					if(k == Sdone)
						break;
					if(k == Smatch && rc + 2 <= 200){
						got[rc++] = m.sp[0];
						got[rc++] = m.ep[0];
					}
				}
				scan_free(&sc);
				if(rc != cnt || memcmp(got, want, cnt * sizeof(Pos)) != 0){
					lprint("fuzz DIFFER seed %d case %d pat %d win %d (got %d want %d)\n", sd, i, j, wins[w], rc / 2, cnt / 2);
					rx_winsize = Scanwin;
					pat_free(&p);
					buf_free(b);
					return;
				}
			}
			rx_winsize = Scanwin;
			pat_free(&p);
			buf_free(b);
		}
	}
	lprint("fuzz same %d\n", ncase);
}

/* rfind: the match with the greatest start before limit */
static void
dorfind(char *src, int limit, int lo, int hi)
{
	Pat p;
	Scan sc;
	Match m;
	int k, r, bud, resumes;

	if(tb == nil){
		lprint("error no text\n");
		return;
	}
	if(pat_comp(&p, src, 0, err, sizeof err) < 0){
		lprint("error %s\n", err);
		return;
	}
	if(hi < 0)
		hi = buf_len(tb);
	scan_init(&sc, &p, tb, lo, hi);
	resumes = 0;
	for(;;){
		bud = budget0 > 0 ? budget0 : 1 << 30;
		r = scan_prev(&sc, limit, &bud, &m);
		if(r == Smore){
			resumes++;
			continue;
		}
		break;
	}
	if(r == Smatch){
		lprint("m %d %d", m.sp[0], m.ep[0]);
		for(k = 1; k < Nsub; k++)
			if(m.sp[k] >= 0)
				lprint(" g%d %d %d", k, m.sp[k], m.ep[k]);
		lprint("\n");
	}else
		lprint("none\n");
	if(resumes > 0)
		lprint("resumed %d\n", resumes);
	scan_free(&sc);
	pat_free(&p);
}

/* oracle for scan_prev: every start from limit-1 down, anchored regexec on the whole text */
static int
roracle(Buf *b, Pat *p, Pos limit, Pos *st, Pos *en)
{
	char *t;
	Resub sub[Nsub];
	Pos n, q;
	int k, found;

	n = buf_len(b);
	t = malloc(n + 2);
	buf_read(b, 0, t, n);
	for(k = 0; k < n; k++)
		if(t[k] == '\0')
			t[k] = 1;
	t[n] = '\0';
	found = 0;
	for(q = limit - 1; q >= 0 && !found; q--){
		if(q < n && ((uchar)t[q] & 0xC0) == 0x80)
			continue;
		memset(sub, 0, sizeof sub);
		sub[0].sp = t + q;
		sub[0].ep = nil;
		if(regexec(p->rx, t, sub, Nsub) == 0)
			continue;
		if((Pos)(sub[0].sp - t) != q)
			continue;
		*st = q;
		*en = (Pos)(sub[0].ep - t);
		found = 1;
	}
	free(t);
	return found;
}

static void
dorfuzz(int sd, int ncase)
{
	char txt[160], *s;
	int len, line, c, i, j, w, k, bud, r, want;
	Pos limit, ws, we, gs, ge;
	Pat p;
	Scan sc;
	Match m;
	Buf *b;

	seed = (u32int)sd;
	for(i = 0; i < ncase; i++){
		len = 0;
		line = 0;
		k = rnd() % 100 + 10;
		while(len < k){
			c = rnd() % 9;
			if(line >= 5)
				c = 7;
			if(c == 7){
				txt[len++] = '\n';
				line = 0;
			}else if(c == 6){
				txt[len++] = '\0';
				line++;
			}else if(c == 5){
				txt[len++] = '\t';
				line++;
			}else if(c == 8){
				txt[len++] = (char)0xC3;
				txt[len++] = (char)0xA9;
				line += 2;
			}else{
				txt[len++] = c < 3 ? 'a' : 'b';
				line++;
			}
		}
		for(j = 0; pats[j] != nil; j++){
			if(pat_comp(&p, pats[j], 0, err, sizeof err) < 0){
				lprint("fuzz error %s\n", err);
				return;
			}
			b = buf_new();
			s = malloc(len + 1);
			memmove(s, txt, len);
			buf_load(b, s, len, err, sizeof err);
			limit = rnd() % (len + 1);
			if(limit < len && ((uchar)txt[limit] & 0xC0) == 0x80)
				limit--;
			want = roracle(b, &p, limit, &ws, &we);
			for(w = 0; w < 3; w++){
				rx_winsize = wins[w];
				scan_init(&sc, &p, b, 0, buf_len(b));
				do{
					bud = 1 << 30;
					r = scan_prev(&sc, limit, &bud, &m);
				}while(r == Smore);
				gs = ge = -1;
				if(r == Smatch){
					gs = m.sp[0];
					ge = m.ep[0];
				}
				scan_free(&sc);
				if((r == Smatch) != want || (want && (gs != ws || ge != we))){
					lprint("rfuzz DIFFER seed %d case %d pat %d win %d limit %d (got %d %d want %d %d)\n", sd, i, j, wins[w], limit, gs, ge, want ? ws : -1, want ? we : -1);
					rx_winsize = Scanwin;
					pat_free(&p);
					buf_free(b);
					return;
				}
			}
			rx_winsize = Scanwin;
			pat_free(&p);
			buf_free(b);
		}
	}
	lprint("rfuzz same %d\n", ncase);
}

static void
cmd(char *l)
{
	char w[32], *s, *a, out[Maxpat + 2];
	int k, n, v1, v2;

	a = l;
	for(k = 0; *a != ' ' && *a != '\0' && k < 31; k++)
		w[k] = *a++;
	w[k] = '\0';
	if(strcmp(w, "translate") == 0){
		if(nextquoted(&a, &s, &n) < 0){
			badargs();
			return;
		}
		s[n] = '\0';
		n = rx_translate(s, out, sizeof out, err, sizeof err);
		if(n < 0)
			lprint("error %s\n", err);
		else
			quoted("t", out, n);
		free(s);
	}else if(strcmp(w, "comp") == 0)
		docomp(a, 0);
	else if(strcmp(w, "lit") == 0)
		docomp(a, 1);
	else if(strcmp(w, "text") == 0){
		if(nextquoted(&a, &s, &n) < 0){
			badargs();
			return;
		}
		if(tb != nil)
			buf_free(tb);
		tb = buf_new();
		if(buf_load(tb, s, n, err, sizeof err) < 0)
			lprint("error %s\n", err);
	}else if(strcmp(w, "budget") == 0){
		if(nextint(&a, &n) < 0){
			badargs();
			return;
		}
		budget0 = n;
	}else if(strcmp(w, "find") == 0){
		if(nextquoted(&a, &s, &n) < 0){
			badargs();
			return;
		}
		s[n] = '\0';
		v1 = 0;
		v2 = -1;
		if(nextint(&a, &v1) == 0 && nextint(&a, &v2) < 0)
			badargs();
		else
			dofind(s, v1, v2);
		free(s);
	}else if(strcmp(w, "rfind") == 0){
		if(nextquoted(&a, &s, &n) < 0 || nextint(&a, &v2) < 0){
			badargs();
			return;
		}
		s[n] = '\0';
		v1 = 0;
		n = -1;
		if(nextint(&a, &v1) == 0 && nextint(&a, &n) < 0)
			badargs();
		else
			dorfind(s, v2, v1, n);
		free(s);
	}else if(strcmp(w, "rfuzz") == 0){
		if(nextint(&a, &v1) < 0 || nextint(&a, &v2) < 0){
			badargs();
			return;
		}
		dorfuzz(v1, v2);
	}else if(strcmp(w, "fuzz") == 0){
		if(nextint(&a, &v1) < 0 || nextint(&a, &v2) < 0){
			badargs();
			return;
		}
		dofuzz(v1, v2);
	}else if(strcmp(w, "win") == 0){
		if(nextint(&a, &n) < 0 || n < 2){
			badargs();
			return;
		}
		rx_winsize = n;
	}else
		lprint("error unknown command\n");
}

static int
run(void)
{
	int rc;

	rc = script(LINEMAX, cmd);
	if(tb != nil)
		buf_free(tb);
	return rc;
}

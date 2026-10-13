#include "compat.h"
#include "coil.h"
#include "script.h"

/* buftest: command driver for the piece-table buffer; output only via lprint */

static Buf *b;
static char *ref;	/* bytes given to the last load */
static int nref;
static PieceRun saved;
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

/* load a malloc'd byte string (ownership passes to the buffer) */
static void
doload(char *data, int n)
{
	free(ref);
	ref = malloc(n + 1);
	memmove(ref, data, n);
	nref = n;
	if(buf_load(b, data, n, err, sizeof err) < 0)
		lprint("error %s\n", err);
}

static int
collect(void *arg, char *p, int n)
{
	char **w;

	if(n > Chunk)
		return -1;
	w = arg;
	memmove(*w, p, n);
	*w += n;
	return n;
}

static void
roundtrip(void)
{
	char *out, *w;
	int k, n;

	n = buf_len(b);
	out = malloc(n + 1);
	w = out;
	if(buf_write(b, collect, &w) < 0){
		lprint("error write failed\n");
		free(out);
		return;
	}
	n = w - out;
	for(k = 0; k < n && k < nref; k++)
		if(out[k] != ref[k])
			break;
	if(k == n && n == nref)
		lprint("roundtrip SAME %d\n", n);
	else
		lprint("roundtrip DIFF at %d\n", k);
	free(out);
}

static void
text(void)
{
	char *buf, *o;
	int n, k, c;
	static char hex[] = "0123456789abcdef";

	n = buf_len(b);
	buf = malloc(n + 1);
	buf_read(b, 0, buf, n);
	o = malloc(n*4 + 1);
	k = 0;
	for(c = 0; c < n; c++){
		int ch = (uchar)buf[c];
		if(ch == '\n'){ o[k++] = '\\'; o[k++] = 'n'; }
		else if(ch == '\r'){ o[k++] = '\\'; o[k++] = 'r'; }
		else if(ch == '\t'){ o[k++] = '\\'; o[k++] = 't'; }
		else if(ch < 0x20 || ch == 0x22 || ch == 0x5c || ch >= 0x7f){
			o[k++] = '\\';
			o[k++] = 'x';
			o[k++] = hex[ch >> 4];
			o[k++] = hex[ch & 15];
		}else
			o[k++] = (char)ch;
	}
	o[k] = '\0';
	lprint("text \"%s\"\n", o);
	free(o);
	free(buf);
}

static void
gen(char *a)
{
	char kind[16], *data;
	int n, w, i, j, k, c;
	static char u[] = "a\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80";

	a = skip(a);
	for(k = 0; *a != ' ' && *a != '\0' && k < 15; k++)
		kind[k] = *a++;
	kind[k] = '\0';
	if(strcmp(kind, "lines") == 0){
		if(nextint(&a, &n) < 0 || nextint(&a, &w) < 0 || w < 1){
			badargs();
			return;
		}
		data = malloc(n*w + 1);
		k = 0;
		for(i = 0; i < n; i++){
			for(j = 0; j < w - 1; j++)
				data[k++] = (char)('a' + j % 26);
			data[k++] = '\n';
		}
		doload(data, k);
	}else if(strcmp(kind, "utf") == 0){
		if(nextint(&a, &n) < 0){
			badargs();
			return;
		}
		data = malloc(n*10 + 1);
		k = 0;
		for(i = 0; i < n; i++)
			for(c = 0; c < 10; c++)
				data[k++] = u[c];
		doload(data, k);
	}else
		badargs();
}

static void
cmd(char *l)
{
	char w[32], *s, *a;
	int k, p, n, q;
	Rune r;
	Pos x;

	a = l;
	for(k = 0; *a != ' ' && *a != '\0' && k < 31; k++)
		w[k] = *a++;
	w[k] = '\0';
	if(strcmp(w, "load") == 0){
		if(nextquoted(&a, &s, &n) < 0){
			badargs();
			return;
		}
		doload(s, n);
	}else if(strcmp(w, "allbytes") == 0){
		s = malloc(257);
		for(k = 0; k < 256; k++)
			s[k] = (char)k;
		doload(s, 256);
	}else if(strcmp(w, "gen") == 0)
		gen(a);
	else if(strcmp(w, "max") == 0){
		if(nextint(&a, &n) < 0){
			badargs();
			return;
		}
		b->max = n;
	}else if(strcmp(w, "len") == 0)
		lprint("len %d\n", buf_len(b));
	else if(strcmp(w, "lines") == 0)
		lprint("lines %d\n", buf_nlines(b));
	else if(strcmp(w, "pieces") == 0){
		n = 0;
		for(k = 0; k < b->np; k++)
			if(b->p[k].len > n)
				n = b->p[k].len;
		lprint("pieces %d maxlen %d\n", b->np, n);
	}else if(strcmp(w, "cuts") == 0){
		x = 0;
		for(k = 0; k < b->np; k++){
			if(x > 0 && !utf_isstart(buf_byte(b, x))){
				lprint("cuts bad at %d\n", x);
				return;
			}
			x += b->p[k].len;
		}
		lprint("cuts ok\n");
	}else if(strcmp(w, "roundtrip") == 0)
		roundtrip();
	else if(strcmp(w, "linestart") == 0){
		if(nextint(&a, &n) < 0){
			badargs();
			return;
		}
		lprint("linestart %d %d\n", n, buf_line_start(b, n));
	}else if(strcmp(w, "line") == 0){
		if(nextint(&a, &p) < 0){
			badargs();
			return;
		}
		lprint("line %d %d\n", p, buf_pos_to_line(b, p));
	}else if(strcmp(w, "rune") == 0){
		if(nextint(&a, &p) < 0){
			badargs();
			return;
		}
		n = buf_rune(b, p, &r);
		lprint("rune %d %04x len %d\n", p, (unsigned)r, n);
	}else if(strcmp(w, "next") == 0 || strcmp(w, "prev") == 0 || strcmp(w, "snap") == 0){
		if(nextint(&a, &p) < 0){
			badargs();
			return;
		}
		if(w[0] == 'n')
			q = buf_next(b, p);
		else if(w[0] == 'p')
			q = buf_prev(b, p);
		else
			q = buf_snap(b, p);
		lprint("%s %d %d\n", w, p, q);
	}else if(strcmp(w, "ins") == 0){
		if(nextint(&a, &p) < 0 || nextquoted(&a, &s, &n) < 0){
			badargs();
			return;
		}
		if(buf_insert(b, p, s, n, err, sizeof err) < 0)
			lprint("error %s\n", err);
		free(s);
	}else if(strcmp(w, "del") == 0){
		if(nextint(&a, &p) < 0 || nextint(&a, &n) < 0){
			badargs();
			return;
		}
		if(buf_delete(b, p, n, nil) < 0)
			lprint("error bad range\n");
	}else if(strcmp(w, "delsave") == 0){
		if(nextint(&a, &p) < 0 || nextint(&a, &n) < 0){
			badargs();
			return;
		}
		buf_runfree(&saved);
		if(buf_delete(b, p, n, &saved) < 0)
			lprint("error bad range\n");
	}else if(strcmp(w, "restore") == 0){
		if(nextint(&a, &p) < 0){
			badargs();
			return;
		}
		if(buf_insert_pieces(b, p, &saved) < 0)
			lprint("error bad range\n");
		buf_runfree(&saved);
	}else if(strcmp(w, "text") == 0)
		text();
	else if(strcmp(w, "check") == 0){
		if(buf_check(b, err, sizeof err) < 0)
			lprint("check bad %s\n", err);
		else
			lprint("check ok\n");
	}else
		lprint("error unknown command\n");
}

static int
run(void)
{
	int rc;

	b = buf_new();
	rc = script(LINEMAX, cmd);
	buf_runfree(&saved);
	buf_free(b);
	free(ref);
	return rc;
}

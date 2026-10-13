#include "compat.h"
#include "coil.h"
#include "script.h"

/* seltest: command driver for selection sets; output only via lprint */

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
	int i;

	lprint("sel");
	for(i = 0; i < ss.n; i++)
		lprint(" [%d,%d)%s", ss.s[i].anchor, ss.s[i].head, i == ss.primary ? "*" : "");
	lprint("\n");
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
		if(buf_load(b, s, n, err, sizeof err) < 0)
			lprint("error %s\n", err);
		sel_set1(&ss, 0, 0);
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
	}else if(strcmp(w, "primary") == 0){
		if(nextint(&a, &n) < 0 || n >= ss.n){
			badargs();
			return;
		}
		ss.primary = n;
	}else if(strcmp(w, "norm") == 0)
		sel_normalize(&ss, b);
	else if(strcmp(w, "map") == 0){
		n = 0;
		while(n < 16 && nextint(&a, &v1) == 0){
			if(nextint(&a, &v2) < 0 || nextint(&a, &k) < 0){
				badargs();
				return;
			}
			e[n].pos = v1;
			e[n].ndel = v2;
			e[n].ins = nil;
			e[n].nins = k;
			n++;
		}
		sel_map(&ss, e, n);
	}else if(strcmp(w, "max") == 0){
		if(nextint(&a, &n) < 0){
			badargs();
			return;
		}
		b->max = n;
	}else if(strcmp(w, "apply") == 0){
		/* apply P NDEL "text" ...: one doc_apply batch, selections mapped */
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
		else
			lprint("apply ok modified %d\n", doc_modified(&d));
		while(n > 0)
			free(e[--n].ins);
	}else if(strcmp(w, "text") == 0){
		n = buf_len(b);
		s = malloc(n + 1);
		buf_read(b, 0, s, n);
		s[n] = '\0';
		for(k = 0; k < n; k++)
			if(s[k] == '\n')
				s[k] = '|';
		lprint("text %s\n", s);
		free(s);
	}else if(strcmp(w, "dump") == 0)
		dump();
	else if(strcmp(w, "check") == 0){
		if(sel_check(&ss, b, err, sizeof err) < 0)
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

	doc_init(&d);
	rc = script(LINEMAX, cmd);
	doc_free(&d);
	return rc;
}

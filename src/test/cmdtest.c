#include "compat.h"
#include "coil.h"
#include "script.h"

/* cmdtest: parser driver. parse "LINE" prints "ok CANONICAL" or "error MSG". */

static char*
skip(char *a)
{
	while(*a == ' ')
		a++;
	return a;
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
cmd(char *l)
{
	char *a, *s, w[16], out[2048], err[128];
	int k, n;
	Cmd c;

	a = skip(l);
	for(k = 0; *a != ' ' && *a != '\0' && k < 15; k++)
		w[k] = *a++;
	w[k] = '\0';
	if(strcmp(w, "parse") != 0){
		lprint("error unknown command\n");
		return;
	}
	if(nextquoted(&a, &s, &n) < 0){
		badargs();
		return;
	}
	s[n] = '\0';
	if(cmd_parse(s, &c, err, sizeof err) < 0)
		lprint("error %s\n", err);
	else{
		cmd_dump(&c, out, sizeof out);
		if(out[0] == '\0')
			lprint("ok\n");
		else
			lprint("ok %s\n", out);
		cmd_free(&c);
	}
	free(s);
}

static int
run(void)
{
	return script(LINEMAX, cmd);
}

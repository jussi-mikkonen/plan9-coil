/*
 * script.h: stdin script reader and main, shared by the test drivers. Each driver
 * includes it once, so it carries its own static copy; no extra object.
 */
#define INMAX 65536
#define LINEMAX 1200

/* each driver defines run(); main below calls it */
static int run(void);

static char inbuf[INMAX + 1];
static char line[LINEMAX + 1];

/* Read all of stdin into inbuf, NUL-terminated. Returns 1 if the input is too large, else 0. */
static int
readall(void)
{
	long n, tot;

	tot = 0;
	for(;;){
		n = read(0, inbuf + tot, INMAX - tot);
		if(n <= 0)
			break;
		tot += n;
		if(tot >= INMAX){
			/* full: any further byte is too much */
			if(read(0, line, 1) > 0){
				lprint("error input too large\n");
				return 1;
			}
			break;
		}
	}
	inbuf[tot] = '\0';
	return 0;
}

#ifndef NOSCRIPT
/*
 * Read all of stdin, then call fn on each line that is neither empty nor
 * a comment. A line longer than max is reported and skipped. Returns 1
 * if the input is too large (nothing is run), else 0.
 */
static int
script(int max, void (*fn)(char*))
{
	char *p, *nl;
	int len;

	if(readall())
		return 1;
	p = inbuf;
	while(*p != '\0'){
		nl = p;
		while(*nl != '\0' && *nl != '\n')
			nl++;
		len = nl - p;
		if(*nl == '\n')
			nl++;
		if(len > max){
			lprint("error line too long\n");
			p = nl;
			continue;
		}
		memmove(line, p, len);
		line[len] = '\0';
		p = nl;
		if(len == 0 || line[0] == '#')
			continue;
		fn(line);
	}
	return 0;
}
#endif

#ifdef HEXHELP
static int
hexval(int c)
{
	if(c >= '0' && c <= '9')
		return c - '0';
	if(c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	if(c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

/* parse hex token at *pp, advance past it and spaces; -1 if none/bad */
static long
nexthex(char **pp)
{
	char *p;
	long v;
	int h, nd;

	p = *pp;
	while(*p == ' ')
		p++;
	v = 0;
	nd = 0;
	while((h = hexval(*p)) >= 0){
		v = v*16 + h;
		if(v > 0x7FFFFFF)
			return -1;
		nd++;
		p++;
	}
	if(nd == 0 || (*p != ' ' && *p != '\0'))
		return -1;
	*pp = p;
	return v;
}
#endif



/* Plan 9 main must exit with exits(); returning gives status "main". */
#ifdef PLAN9
void
main(void)
{
	exits(run() ? "fail" : nil);
}
#else
int
main(void)
{
	return run();
}
#endif

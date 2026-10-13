#include "compat.h"
#include "coil.h"

/* lang.c: per-language editing settings. Tab width 4; real tabs except Markdown. */

static Lang langs[] = {
	{"c", 4, 1, lex_c},
	{"rc", 4, 1, lex_rc},
	{"mkfile", 4, 1, lex_mk},
	{"markdown", 4, 0, lex_md},
	{"plain", 4, 1, nil}
};

enum { Lc, Lrc, Lmkfile, Lmarkdown, Lplain };

static int
suffix(char *s, char *suf)
{
	int n, m;

	n = strlen(s);
	m = strlen(suf);
	return n >= m && strcmp(s + n - m, suf) == 0;
}

static int
prefix(char *s, char *pre)
{
	return strncmp(s, pre, strlen(pre)) == 0;
}

Lang*
lang_for_name(char *path)
{
	char *base, *p;

	if(path == nil)
		return &langs[Lplain];
	base = path;
	for(p = path; *p != '\0'; p++)
		if(*p == '/')
			base = p + 1;
	if(strcmp(base, "mkfile") == 0 || prefix(base, "mkfile.") || suffix(base, ".mk") ||
	   strcmp(base, "mkone") == 0 || strcmp(base, "mkmany") == 0 ||
	   strcmp(base, "mklib") == 0 || strcmp(base, "mksyslib") == 0)
		return &langs[Lmkfile];
	if(suffix(base, ".c") || suffix(base, ".h") || suffix(base, ".y") || suffix(base, ".l"))
		return &langs[Lc];
	if(suffix(base, ".rc"))
		return &langs[Lrc];
	if(suffix(base, ".md"))
		return &langs[Lmarkdown];
	return &langs[Lplain];
}

Lang*
lang_detect(char *path, Buf *b)
{
	Lang *l;
	char t[65];
	int n, i, j;

	l = lang_for_name(path);
	if(l != &langs[Lplain] || b == nil)
		return l;
	n = buf_read(b, 0, t, 64);
	t[n] = '\0';
	if(n < 2 || t[0] != '#' || t[1] != '!')
		return l;
	for(i = 0; i < n && t[i] != '\n'; i++)
		;
	t[i] = '\0';
	n = i;
	i = 2;
	while(t[i] == ' ' || t[i] == '\t')
		i++;
	for(j = i; j < n && t[j] != ' ' && t[j] != '\t'; j++)
		;
	t[j] = '\0';
	if(strcmp(t + i, "/bin/rc") == 0 || strcmp(t + i, "/rc/bin/rc") == 0)
		return &langs[Lrc];
	return l;
}

Lang*
lang_by_name(char *name)
{
	int i;

	if(name == nil)
		return nil;
	for(i = 0; i < (int)(sizeof langs / sizeof langs[0]); i++)
		if(strcmp(name, langs[i].name) == 0)
			return &langs[i];
	return nil;
}

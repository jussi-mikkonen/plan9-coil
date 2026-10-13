/* tools/rx9/libc.h: Linux shim of the few <libc.h> names the vendored libregexp uses. */
#ifndef RX9_LIBC_H
#define RX9_LIBC_H
#include <stdlib.h>
#include <string.h>
/* Plan 9 qsort takes int(*)(void*, void*) */
#define qsort(a, n, sz, f) qsort((a), (n), (sz), (int(*)(const void*, const void*))(f))
enum { UTFmax = 4, Runesync = 0x80, Runeself = 0x80, Runeerror = 0xFFFD, Runemax = 0x10FFFF };
int	chartorune(Rune *r, char *s);
int	runetochar(char *s, Rune *r);
int	utflen(char *s);
int	snprint(char *buf, int n, char *fmt, ...);	/* %s %C only */
int	print(char *fmt, ...);
#endif

/* tools/rx9/rx9.c: the libc names the vendored libregexp needs, for Linux only.
 * UTF-8 rules as utf.c: an invalid or truncated byte is Runeerror consuming 1. */
#include <stdarg.h>
#include <stdio.h>
#include "u.h"
#include "libc.h"

int
chartorune(Rune *r, char *s)
{
	uchar *p;
	int c, n, i;
	Rune v;

	p = (uchar*)s;
	c = p[0];
	if(c < 0x80){
		*r = c;
		return 1;
	}
	if(c >= 0xC2 && c <= 0xDF){
		n = 2;
		v = c & 0x1F;
	}else if(c >= 0xE0 && c <= 0xEF){
		n = 3;
		v = c & 0x0F;
	}else if(c >= 0xF0 && c <= 0xF4){
		n = 4;
		v = c & 0x07;
	}else{
		*r = Runeerror;
		return 1;
	}
	for(i = 1; i < n; i++){
		if((p[i] & 0xC0) != 0x80){
			*r = Runeerror;
			return 1;
		}
		v = (v << 6) | (p[i] & 0x3F);
	}
	if((n == 3 && v < 0x800) || (n == 4 && (v < 0x10000 || v > Runemax)) || (v >= 0xD800 && v <= 0xDFFF)){
		*r = Runeerror;
		return 1;
	}
	*r = v;
	return n;
}

int
runetochar(char *s, Rune *rp)
{
	Rune r;

	r = *rp;
	if(r < 0x80){
		s[0] = r;
		return 1;
	}
	if(r < 0x800){
		s[0] = 0xC0 | (r >> 6);
		s[1] = 0x80 | (r & 0x3F);
		return 2;
	}
	if(r < 0x10000){
		s[0] = 0xE0 | (r >> 12);
		s[1] = 0x80 | ((r >> 6) & 0x3F);
		s[2] = 0x80 | (r & 0x3F);
		return 3;
	}
	s[0] = 0xF0 | (r >> 18);
	s[1] = 0x80 | ((r >> 12) & 0x3F);
	s[2] = 0x80 | ((r >> 6) & 0x3F);
	s[3] = 0x80 | (r & 0x3F);
	return 4;
}

int
utflen(char *s)
{
	int n;
	Rune r;

	n = 0;
	while(*s != '\0'){
		s += chartorune(&r, s);
		n++;
	}
	return n;
}

/* %s and %C only: all regcomp.c asks for */
int
snprint(char *buf, int n, char *fmt, ...)
{
	va_list ap;
	int len;
	char *s, tmp[8];
	Rune r;
	int k;

	va_start(ap, fmt);
	len = 0;
	for(; *fmt != '\0'; fmt++){
		if(*fmt == '%' && (fmt[1] == 's' || fmt[1] == 'C')){
			if(fmt[1] == 's'){
				s = va_arg(ap, char*);
				k = strlen(s);
			}else{
				r = va_arg(ap, Rune);
				k = runetochar(tmp, &r);
				s = tmp;
			}
			fmt++;
		}else{
			tmp[0] = *fmt;
			s = tmp;
			k = 1;
		}
		if(len + k < n)
			memmove(buf + len, s, k);
		len += k;
	}
	va_end(ap);
	if(n > 0)
		buf[len < n ? len : n - 1] = '\0';
	return len;
}

int
print(char *fmt, ...)
{
	(void)fmt;
	return 0;
}

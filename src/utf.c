#include "compat.h"
#include "coil.h"

/* continuation byte at s[i], i < n */
static int
cont(char *s, int n, int i)
{
	return i < n && ((uchar)s[i] & 0xC0) == 0x80;
}

int
utf_dec(char *s, int n, Rune *r)
{
	int c, c1;

	if(n <= 0){
		*r = Runeerr;
		return 0;
	}
	c = (uchar)s[0];
	if(c < 0x80){
		*r = (Rune)c;
		return 1;
	}
	if(c >= 0xC2 && c <= 0xDF){
		if(cont(s, n, 1)){
			*r = (Rune)(((c & 0x1F) << 6) | ((uchar)s[1] & 0x3F));
			return 2;
		}
	}else if(c >= 0xE0 && c <= 0xEF){
		if(cont(s, n, 1)){
			c1 = (uchar)s[1];
			if((c != 0xE0 || c1 >= 0xA0) && (c != 0xED || c1 <= 0x9F) && cont(s, n, 2)){
				*r = (Rune)(((c & 0x0F) << 12) | ((c1 & 0x3F) << 6) | ((uchar)s[2] & 0x3F));
				return 3;
			}
		}
	}else if(c >= 0xF0 && c <= 0xF4){
		if(cont(s, n, 1)){
			c1 = (uchar)s[1];
			if((c != 0xF0 || c1 >= 0x90) && (c != 0xF4 || c1 <= 0x8F)
			&& cont(s, n, 2) && cont(s, n, 3)){
				*r = (Rune)(((c & 0x07) << 18) | ((c1 & 0x3F) << 12)
					| (((uchar)s[2] & 0x3F) << 6) | ((uchar)s[3] & 0x3F));
				return 4;
			}
		}
	}
	*r = Runeerr;
	return 1;
}

int
utf_enc(char *s, Rune r)
{
	if(r > 0x10FFFF || (r >= 0xD800 && r <= 0xDFFF))
		r = Runeerr;
	if(r < 0x80){
		s[0] = (char)r;
		return 1;
	}
	if(r < 0x800){
		s[0] = (char)(0xC0 | (r >> 6));
		s[1] = (char)(0x80 | (r & 0x3F));
		return 2;
	}
	if(r < 0x10000){
		s[0] = (char)(0xE0 | (r >> 12));
		s[1] = (char)(0x80 | ((r >> 6) & 0x3F));
		s[2] = (char)(0x80 | (r & 0x3F));
		return 3;
	}
	s[0] = (char)(0xF0 | (r >> 18));
	s[1] = (char)(0x80 | ((r >> 12) & 0x3F));
	s[2] = (char)(0x80 | ((r >> 6) & 0x3F));
	s[3] = (char)(0x80 | (r & 0x3F));
	return 4;
}

int
utf_isstart(int c)
{
	return (c & 0xC0) != 0x80;
}

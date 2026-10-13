#include "compat.h"
#include "coil.h"

/* obj.c: text objects (mi / ma). */

enum { Objscan = 1 << 20 };	/* bound on bytes scanned in each direction */

static int
cls_at(Buf *b, Pos p, int big)
{
	Rune r;
	int c;

	if(buf_rune(b, p, &r) <= 0)
		return -1;
	c = rune_class(r);
	if(big && c == Cpunct)
		c = Cword;
	return c;
}

static int
cls_before(Buf *b, Pos p, int big)
{
	if(p <= 0)
		return -1;
	return cls_at(b, buf_prev(b, p), big);
}

static Pos
line_after(Buf *b, int line)
{
	if(line + 1 < buf_nlines(b))
		return buf_line_start(b, line + 1);
	return buf_len(b);
}

static int
word(Buf *b, Pos p, int inner, int big, Pos *lo, Pos *hi)
{
	Pos a, e, len;
	int c;

	len = buf_len(b);
	c = cls_at(b, p, big);
	if(c < 0)
		return 0;
	a = p;
	e = buf_next(b, p);
	if(c != Ceol){
		while(a > 0 && cls_before(b, a, big) == c)
			a = buf_prev(b, a);
		while(e < len && cls_at(b, e, big) == c)
			e = buf_next(b, e);
	}
	if(!inner && c != Cspace && c != Ceol){
		if(e < len && cls_at(b, e, big) == Cspace){
			while(e < len && cls_at(b, e, big) == Cspace)
				e = buf_next(b, e);
		}else
			while(a > 0 && cls_before(b, a, big) == Cspace)
				a = buf_prev(b, a);
	}
	*lo = a;
	*hi = e;
	return 1;
}

/* bracket pair around p; brackets are ASCII so a byte scan is exact */
static int
pair(Buf *b, Pos p, int inner, int open, int close, Pos *lo, Pos *hi)
{
	Pos o, c, q, len;
	int d, ch, n;

	len = buf_len(b);
	o = -1;
	if(buf_byte(b, p) == open)
		o = p;
	else{
		d = 0;
		n = 0;
		for(q = p - 1; q >= 0 && n < Objscan; q--, n++){
			ch = buf_byte(b, q);
			if(ch == close)
				d++;
			else if(ch == open){
				if(d == 0){
					o = q;
					break;
				}
				d--;
			}
		}
	}
	if(o < 0)
		return 0;
	c = -1;
	d = 0;
	n = 0;
	for(q = o + 1; q < len && n < Objscan; q++, n++){
		ch = buf_byte(b, q);
		if(ch == open)
			d++;
		else if(ch == close){
			if(d == 0){
				c = q;
				break;
			}
			d--;
		}
	}
	if(c < 0)
		return 0;
	if(inner){
		*lo = o + 1;
		*hi = c;
		return *lo < *hi;
	}
	*lo = o;
	*hi = c + 1;
	return 1;
}

/* quote pair on the line of p; quotes pair left to right, a backslash escapes the next byte */
static int
quote(Buf *b, Pos p, int inner, int q, Pos *lo, Pos *hi)
{
	Pos ls, x, open, len;
	int ch;

	len = buf_len(b);
	ls = buf_line_start(b, buf_pos_to_line(b, p));
	open = -1;
	for(x = ls; x < len && x - ls < Objscan; x++){
		ch = buf_byte(b, x);
		if(ch == '\n')
			break;
		if(ch == '\\'){
			if(buf_byte(b, x + 1) == '\n')
				break;
			x++;
			continue;
		}
		if(ch != q)
			continue;
		if(open < 0){
			open = x;
			continue;
		}
		if(p >= open && p <= x){
			if(inner){
				*lo = open + 1;
				*hi = x;
				return *lo < *hi;
			}
			*lo = open;
			*hi = x + 1;
			return 1;
		}
		if(x >= p)
			break;
		open = -1;
	}
	return 0;
}

/* a line with nothing but spaces, tabs and \r */
static int
blank(Buf *b, int line)
{
	Pos p, len;
	Rune r;

	len = buf_len(b);
	p = buf_line_start(b, line);
	while(p < len && buf_rune(b, p, &r) > 0 && rune_class(r) == Cspace)
		p = buf_next(b, p);
	return p >= len || (buf_rune(b, p, &r) > 0 && rune_class(r) == Ceol);
}

static int
para(Ed *ed, Pos p, int inner, Pos *lo, Pos *hi)
{
	Buf *b;
	int line, last, f, l, bl, n;

	b = ed->doc.b;
	last = ed_lastline(ed);
	line = buf_pos_to_line(b, p);
	if(line > last)
		line = last;
	bl = blank(b, line);
	f = l = line;
	while(f > 0 && blank(b, f - 1) == bl)
		f--;
	while(l < last && blank(b, l + 1) == bl)
		l++;
	*lo = buf_line_start(b, f);
	*hi = line_after(b, l);
	if(!bl && !inner){
		n = l + 1;
		while(n <= last && blank(b, n))
			n++;
		if(n > l + 1)
			*hi = n <= last ? buf_line_start(b, n) : buf_len(b);
	}
	return *lo < *hi;
}

static int
find(Ed *ed, Sel *s, int inner, Rune what, Pos *lo, Pos *hi)
{
	Buf *b;
	Pos p;

	b = ed->doc.b;
	p = sel_min(s);
	switch(what){
	case 'w':
		return word(b, p, inner, 0, lo, hi);
	case 'W':
		return word(b, p, inner, 1, lo, hi);
	case 'p':
		return para(ed, p, inner, lo, hi);
	case '(':
	case ')':
	case 'b':
		return pair(b, p, inner, '(', ')', lo, hi);
	case '{':
	case '}':
	case 'B':
		return pair(b, p, inner, '{', '}', lo, hi);
	case '[':
	case ']':
		return pair(b, p, inner, '[', ']', lo, hi);
	case '"':
	case '\'':
	case '`':
		return quote(b, p, inner, what, lo, hi);
	}
	return 0;
}

int
obj_select(Ed *ed, int inner, Rune what)
{
	SelSet *ss;
	Sel *s;
	Pos lo, hi;
	int k, prim;

	ss = &ed->doc.sel;
	prim = 0;
	for(k = 0; k < ss->n; k++){
		s = &ss->s[k];
		if(find(ed, s, inner, what, &lo, &hi)){
			s->anchor = lo;
			s->head = hi;
			s->goal = -1;
			if(k == ss->primary)
				prim = 1;
		}
	}
	sel_normalize(ss, ed->doc.b);
	if(!prim){
		lsnprint(ed->msg, sizeof ed->msg, "%s", "no object");
		return -1;
	}
	return 0;
}

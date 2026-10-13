#include "compat.h"
#include "coil.h"

/* motion.c: motions and selection operators. */

int
rune_class(Rune r)
{
	if(r == ' ' || r == '\t' || r == '\r')
		return Cspace;
	if(r == '\n')
		return Ceol;
	if(r >= 0x80 || r == '_' || (r >= '0' && r <= '9') || (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z'))
		return Cword;
	return Cpunct;
}

/* class of the rune at p, -1 at EOF; big folds punctuation into words (W B E) */
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

/* make s the one-rune cursor at t (empty at EOF) */
static void
setcur(Buf *b, Sel *s, Pos t)
{
	s->anchor = t;
	s->head = t < buf_len(b) ? buf_cnext(b, t) : t;
}

/* start of the line after `line`, or the buffer length for the last line */
static Pos
line_after(Buf *b, int line)
{
	if(line + 1 < buf_nlines(b))
		return buf_line_start(b, line + 1);
	return buf_len(b);
}

/* ---- word steps: one step from p, result is the end position (or p if nothing moves) ---- */

static Pos
wnext(Buf *b, Pos p, int big)
{
	Pos len;
	int c;

	len = buf_len(b);
	if(p >= len)
		return p;
	c = cls_at(b, p, big);
	if(c == Ceol)
		p = buf_next(b, p);
	else
		while(p < len && cls_at(b, p, big) == c)
			p = buf_next(b, p);
	while(p < len && cls_at(b, p, big) == Cspace)
		p = buf_next(b, p);
	return p;
}

static Pos
wend(Buf *b, Pos p, int big)
{
	Pos len, q;
	int c;

	len = buf_len(b);
	q = p;
	while(q < len && ((c = cls_at(b, q, big)) == Cspace || c == Ceol))
		q = buf_next(b, q);
	if(q >= len)
		return q;
	c = cls_at(b, q, big);
	while(q < len && cls_at(b, q, big) == c)
		q = buf_next(b, q);
	return q;
}

static Pos
wprev(Buf *b, Pos p, int big)
{
	Pos q;
	int c;

	q = p;
	while(q > 0 && ((c = cls_before(b, q, big)) == Cspace || c == Ceol))
		q = buf_prev(b, q);
	c = cls_before(b, q, big);
	while(q > 0 && cls_before(b, q, big) == c)
		q = buf_prev(b, q);
	return q;
}

/* count steps from st; 1 and *out set if the position moved */
static int
wrun(Buf *b, int kind, Pos st, int count, Pos *out)
{
	Pos p, q;
	int i, big, what;

	big = kind >= Mv_bigwordnext;
	what = big ? kind - Mv_bigwordnext : kind - Mv_wordnext;	/* 0 next, 1 prev, 2 end */
	p = st;
	for(i = 0; i < count; i++){
		if(what == 0)
			q = wnext(b, p, big);
		else if(what == 1)
			q = wprev(b, p, big);
		else
			q = wend(b, p, big);
		if(q == p)
			break;
		p = q;
	}
	*out = p;
	return p != st;
}

static void
wordmotion(Ed *ed, Sel *s, int kind, int count)
{
	Buf *b;
	Pos st, p, a, h;

	b = ed->doc.b;
	if(s->anchor < s->head && buf_next(b, s->anchor) < s->head)
		st = s->head;
	else
		st = ed_cursor(ed, s);
	if(!wrun(b, kind, st, count, &p))
		return;
	if(ed->mode == Mselect){
		s->head = p;
		if(s->head == s->anchor && s->anchor < buf_len(b))
			s->head = buf_next(b, s->anchor);
		return;
	}
	a = st;
	h = p;
	if(a == s->anchor && h == s->head){
		/* the one-rune word the selection already is: go on from its end */
		st = h;
		if(!wrun(b, kind, st, count, &p))
			return;
		a = st;
		h = p;
	}
	s->anchor = a;
	s->head = h;
}

/* ---- character-like motions ---- */

/* the cursor position moved n lines in dir ('j' or 'k') keeping the goal column */
static Pos
vertical(Ed *ed, Sel *s, Pos c, int dir, int n, int *goal)
{
	Buf *b;
	int line, target, last;

	b = ed->doc.b;
	last = ed_lastline(ed);
	*goal = s->goal >= 0 ? s->goal : view_col_of(&ed->view, b, c);
	line = buf_pos_to_line(b, c);
	if(dir == 'j'){
		target = n > last - line ? last : line + n;
		if(target < line)
			target = line;
	}else
		target = n > line ? 0 : line - n;
	if(target == line)
		return c;
	return view_pos_at_col(&ed->view, b, target, *goal);
}

static Pos
firstnonblank(Buf *b, Pos ls)
{
	Pos p, len;

	len = buf_len(b);
	p = ls;
	while(p < len && cls_at(b, p, 0) == Cspace)
		p = buf_next(b, p);
	return p;
}

/* last rune of the line before its ending ("\r\n" counts as the ending); the newline itself on an empty line */
static Pos
lineend(Buf *b, Pos c)
{
	Pos ls, end, len;
	int line;

	len = buf_len(b);
	line = buf_pos_to_line(b, c);
	ls = buf_line_start(b, line);
	if(line + 1 < buf_nlines(b))
		end = buf_line_start(b, line + 1) - 1;
	else
		end = len;
	if(end > ls && end < len && buf_byte(b, end) == '\n' && buf_byte(b, end - 1) == '\r')
		end--;
	if(end <= ls)
		return ls;
	return buf_prev(b, end);
}

/* place the result of a character-like motion: a cursor in Normal, an extension in Select */
static void
put(Ed *ed, Sel *s, Pos t)
{
	Buf *b;

	b = ed->doc.b;
	if(ed->mode == Mselect){
		if(t >= s->anchor)
			s->head = t < buf_len(b) ? buf_cnext(b, t) : t;
		else
			s->head = t;
	}else
		setcur(b, s, t);
}

void
mv_motion(Ed *ed, int kind, int count)
{
	Buf *b;
	Sel *s;
	Pos c, t, ls, nx;
	int k, i, goal, n, line, rows;

	b = ed->doc.b;
	if(count < 1)
		count = 1;
	rows = ed->view.rows > 0 ? ed->view.rows : 1;
	for(k = 0; k < ed->doc.sel.n; k++){
		s = &ed->doc.sel.s[k];
		if(kind >= Mv_wordnext && kind <= Mv_bigwordend){
			wordmotion(ed, s, kind, count);
			s->goal = -1;
			continue;
		}
		c = ed_cursor(ed, s);
		t = c;
		goal = -1;
		switch(kind){
		case Mv_left:
			for(i = 0; i < count && t > 0; i++)
				t = buf_cprev(b, t);
			break;
		case Mv_right:
			for(i = 0; i < count; i++){
				nx = buf_cnext(b, t);
				if(nx >= buf_len(b))
					break;
				t = nx;
			}
			break;
		case Mv_down:
			t = vertical(ed, s, c, 'j', count, &goal);
			break;
		case Mv_up:
			t = vertical(ed, s, c, 'k', count, &goal);
			break;
		case Mv_halfdown:
		case Mv_halfup:
		case Mv_pagedown:
		case Mv_pageup:
			n = (kind == Mv_halfdown || kind == Mv_halfup) ? rows / 2 : rows;
			if(n < 1)
				n = 1;
			t = vertical(ed, s, c, (kind == Mv_halfdown || kind == Mv_pagedown) ? 'j' : 'k', n * count, &goal);
			break;
		case Mv_linestart:
			ls = buf_line_start(b, buf_pos_to_line(b, c));
			t = firstnonblank(b, ls);
			if(t == c)
				t = ls;
			break;
		case Mv_lineend:
			t = lineend(b, c);
			break;
		case Mv_filestart:
			t = 0;
			break;
		case Mv_fileend:
			t = buf_len(b) > 0 ? buf_prev(b, buf_len(b)) : 0;
			break;
		case Mv_goline:
			line = count - 1;
			if(line > ed_lastline(ed))
				line = ed_lastline(ed);
			t = buf_line_start(b, line);
			break;
		}
		put(ed, s, t);
		s->goal = goal;
	}
	sel_normalize(&ed->doc.sel, b);
	view_reveal(&ed->view, b, ed_cursor(ed, sel_primary(&ed->doc.sel)));
}

/* ---- selection operators ---- */

static void
op_line(Ed *ed, Sel *s)
{
	Buf *b;
	Pos lo, hi, nlo, nhi;
	int l0, l1;

	b = ed->doc.b;
	lo = sel_min(s);
	hi = sel_max(s);
	l0 = buf_pos_to_line(b, lo);
	l1 = buf_pos_to_line(b, hi > lo ? hi - 1 : lo);
	nlo = buf_line_start(b, l0);
	nhi = line_after(b, l1);
	if(hi > lo && nlo == lo && nhi == hi && hi < buf_len(b))
		nhi = line_after(b, buf_pos_to_line(b, hi));
	s->anchor = nlo;
	s->head = nhi;
}

void
sel_op(Ed *ed, int op)
{
	Buf *b;
	Sel *s;
	Pos t;
	int k;

	b = ed->doc.b;
	if(op == '%'){
		sel_set1(&ed->doc.sel, 0, buf_len(b));
	}else{
		for(k = 0; k < ed->doc.sel.n; k++){
			s = &ed->doc.sel.s[k];
			s->goal = -1;
			switch(op){
			case 'x':
				op_line(ed, s);
				break;
			case ';':
				t = ed_cursor(ed, s);
				setcur(b, s, t);
				break;
			case ',':
				t = s->anchor;
				s->anchor = s->head;
				s->head = t;
				break;
			}
		}
	}
	sel_normalize(&ed->doc.sel, b);
	view_reveal(&ed->view, b, ed_cursor(ed, sel_primary(&ed->doc.sel)));
}

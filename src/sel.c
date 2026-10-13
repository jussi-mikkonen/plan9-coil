#include "compat.h"
#include "coil.h"

/* sel.c: selection sets. */

void
sel_init(SelSet *ss)
{
	ss->s = nil;
	ss->n = 0;
	ss->cap = 0;
	ss->primary = 0;
}

void
sel_free(SelSet *ss)
{
	free(ss->s);
	sel_init(ss);
}

static int
grow(SelSet *ss, int need)
{
	Sel *p;
	int c;

	if(need <= ss->cap)
		return 0;
	c = ss->cap < 4 ? 4 : ss->cap;
	while(c < need)
		c *= 2;
	p = realloc(ss->s, c * sizeof(Sel));
	if(p == nil)
		return -1;
	ss->s = p;
	ss->cap = c;
	return 0;
}

void
sel_copy(SelSet *dst, SelSet *src)
{
	if(dst == src)
		return;
	if(grow(dst, src->n) < 0){
		/* out of memory: keep dst unchanged but valid */
		return;
	}
	if(src->n > 0)
		memmove(dst->s, src->s, src->n * sizeof(Sel));
	dst->n = src->n;
	dst->primary = src->primary;
}

void
sel_set1(SelSet *ss, Pos anchor, Pos head)
{
	if(grow(ss, 1) < 0)
		return;
	ss->s[0].anchor = anchor;
	ss->s[0].head = head;
	ss->s[0].goal = -1;
	ss->n = 1;
	ss->primary = 0;
}

int
sel_add(SelSet *ss, Pos anchor, Pos head)
{
	if(grow(ss, ss->n + 1) < 0)
		return -1;
	ss->s[ss->n].anchor = anchor;
	ss->s[ss->n].head = head;
	ss->s[ss->n].goal = -1;
	return ss->n++;
}

Pos
sel_min(Sel *s)
{
	return s->anchor < s->head ? s->anchor : s->head;
}

Pos
sel_max(Sel *s)
{
	return s->anchor < s->head ? s->head : s->anchor;
}

Sel*
sel_primary(SelSet *ss)
{
	return &ss->s[ss->primary];
}

/* is p between the CR and the LF of a "\r\n" pair? */
static int
midcrlf(Buf *b, Pos p)
{
	return p > 0 && p < buf_len(b) && buf_byte(b, p - 1) == '\r' && buf_byte(b, p) == '\n';
}

static Pos
fixpos(Buf *b, Pos p)
{
	if(p < 0)
		p = 0;
	if(p > buf_len(b))
		p = buf_len(b);
	return buf_snap(b, p);
}

void
sel_normalize(SelSet *ss, Buf *b)
{
	Sel t, *s;
	int i, j, o, oldpri, newpri, pri;
	Pos mn, mx, pmn, pmx;
	Sel *src;

	if(ss->n == 0){
		sel_set1(ss, 0, 0);
		if(ss->n == 0)
			return;
	}
	s = ss->s;
	if(ss->primary < 0 || ss->primary >= ss->n)
		ss->primary = 0;
	for(i = 0; i < ss->n; i++){
		s[i].anchor = fixpos(b, s[i].anchor);
		s[i].head = fixpos(b, s[i].head);
		/* never end a selection inside a CRLF pair: carets move down, ranges widen */
		if(midcrlf(b, s[i].anchor) || midcrlf(b, s[i].head)){
			if(s[i].anchor == s[i].head)
				s[i].anchor = s[i].head = s[i].head - 1;
			else if(s[i].anchor < s[i].head){
				if(midcrlf(b, s[i].anchor))
					s[i].anchor--;
				if(midcrlf(b, s[i].head))
					s[i].head++;
			}else{
				if(midcrlf(b, s[i].head))
					s[i].head--;
				if(midcrlf(b, s[i].anchor))
					s[i].anchor++;
			}
		}
	}
	/* insertion sort by min, tracking the primary */
	for(i = 1; i < ss->n; i++){
		t = s[i];
		pri = ss->primary == i;
		j = i - 1;
		while(j >= 0 && sel_min(&s[j]) > sel_min(&t)){
			s[j+1] = s[j];
			if(ss->primary == j)
				ss->primary = j + 1;
			j--;
		}
		s[j+1] = t;
		if(pri)
			ss->primary = j + 1;
	}
	/* merge overlapping, and touching when one side is empty */
	oldpri = ss->primary;
	newpri = 0;
	o = 0;
	for(i = 0; i < ss->n; i++){
		if(o > 0){
			pmn = sel_min(&s[o-1]);
			pmx = sel_max(&s[o-1]);
			mn = sel_min(&s[i]);
			mx = sel_max(&s[i]);
			if(mn < pmx || (mn == pmx && (pmn == pmx || mn == mx))){
				src = i == oldpri ? &s[i] : &s[o-1];
				if(mn > pmn)
					mn = pmn;
				if(mx < pmx)
					mx = pmx;
				if(src->anchor > src->head){
					t.anchor = mx;
					t.head = mn;
				}else{
					t.anchor = mn;
					t.head = mx;
				}
				t.goal = src->goal;
				s[o-1] = t;
				if(i == oldpri)
					newpri = o - 1;
				continue;
			}
		}
		s[o] = s[i];
		if(i == oldpri)
			newpri = o;
		o++;
	}
	ss->n = o;
	ss->primary = newpri;
}

/*
 * Position mapping through a batch in one pass. ei and delta carry the
 * edits already passed; role 0 = min end or empty range, 1 = max end.
 */
static Pos
mappos(Pos p, int role, Edit *e, int n, int *ei, Pos *delta)
{
	while(*ei < n && e[*ei].pos <= p){
		Edit *x = &e[*ei];

		if(x->pos + x->ndel <= p){
			if(x->ndel == 0 && x->pos == p && role == 1)
				break;
			*delta += x->nins - x->ndel;
			(*ei)++;
			continue;
		}
		return x->pos + *delta;	/* p is inside the deleted span, or at its start */
	}
	return p + *delta;
}

void
sel_map(SelSet *ss, Edit *e, int n)
{
	int i, ei;
	Pos delta, last, omin, omax, a, h;
	Sel *s;

	ei = 0;
	delta = 0;
	last = 0;
	for(i = 0; i < ss->n; i++){
		s = &ss->s[i];
		omin = sel_min(s);
		omax = sel_max(s);
		if(omin < last){
			/* not sorted: restart the pass */
			ei = 0;
			delta = 0;
		}
		if(s->anchor < s->head){
			a = mappos(s->anchor, 0, e, n, &ei, &delta);
			h = mappos(s->head, 1, e, n, &ei, &delta);
		}else if(s->anchor > s->head){
			h = mappos(s->head, 0, e, n, &ei, &delta);
			a = mappos(s->anchor, 1, e, n, &ei, &delta);
		}else{
			a = mappos(s->anchor, 0, e, n, &ei, &delta);
			h = a;
		}
		s->anchor = a;
		s->head = h;
		last = omax;
	}
}

int
sel_check(SelSet *ss, Buf *b, char *why, int nwhy)
{
	int i;
	Pos mn, mx, pmn, pmx;
	char *m;

	m = nil;
	if(ss->n < 1)
		m = "empty set";
	else if(ss->primary < 0 || ss->primary >= ss->n)
		m = "bad primary";
	for(i = 0; m == nil && i < ss->n; i++){
		mn = sel_min(&ss->s[i]);
		mx = sel_max(&ss->s[i]);
		if(mn < 0 || mx > buf_len(b))
			m = "out of range";
		else if(buf_snap(b, ss->s[i].anchor) != ss->s[i].anchor ||
		   buf_snap(b, ss->s[i].head) != ss->s[i].head)
			m = "not a rune start";
		else if(i > 0){
			pmn = sel_min(&ss->s[i-1]);
			pmx = sel_max(&ss->s[i-1]);
			if(mn < pmn)
				m = "unsorted";
			else if(mn < pmx || (mn == pmx && (pmn == pmx || mn == mx)))
				m = "overlap";
		}
	}
	if(m == nil)
		return 0;
	if(nwhy > 0){
		lsnprint(why, nwhy, "%s", m);
	}
	return -1;
}

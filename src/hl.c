#include "compat.h"
#include "coil.h"

/* hl.c: highlight state cache. The state at the start of every Hlckint-th line is
 * remembered; an edit drops the checkpoints from its line on (Doc.dirty), so the
 * next frame lexes forward from the nearest older checkpoint. A cold jump farther
 * than h->look lines past every checkpoint starts from "normal" at the look-back
 * start: bounded work, accepted inexactness. */

void
hl_init(Hl *h)
{
	memset(h, 0, sizeof *h);
	h->look = Hllook;
	h->cap = Hlcap;
}

void
hl_free(Hl *h)
{
	free(h->ck);
	h->ck = nil;
	h->nck = 0;
	h->ckcap = 0;
}

/* index of the greatest checkpoint with ck.line <= line (ck[0] is line 0) */
static int
ckfind(Hl *h, int line)
{
	int lo, hi, mid;

	lo = 0;
	hi = h->nck - 1;
	while(lo < hi){
		mid = (lo + hi + 1) / 2;
		if(h->ck[mid].line <= line)
			lo = mid;
		else
			hi = mid - 1;
	}
	return lo;
}

/* remember the state at the start of line; silently skipped when out of memory */
static void
ckput(Hl *h, int line, Hstate st)
{
	int i;
	Hlck *n;

	if(h->nck > 0){
		i = ckfind(h, line);
		if(h->ck[i].line == line){
			h->ck[i].st = st;
			return;
		}
		i++;
	}else
		i = 0;
	if(h->nck >= Hlmaxck){
		/* keep line 0 and the newest entry */
		h->nck = 1;
		i = 1;
	}
	if(h->nck == h->ckcap){
		n = czalloc((h->ckcap == 0 ? 64 : h->ckcap * 2) * sizeof *n);
		if(n == nil)
			return;
		if(h->nck > 0)
			memmove(n, h->ck, h->nck * sizeof *n);
		free(h->ck);
		h->ck = n;
		h->ckcap = h->ckcap == 0 ? 64 : h->ckcap * 2;
	}
	memmove(&h->ck[i + 1], &h->ck[i], (h->nck - i) * sizeof *h->ck);
	h->ck[i].line = line;
	h->ck[i].st = st;
	h->nck++;
}

void
hl_reset(Hl *h, Lang *lang)
{
	h->lang = lang;
	h->nck = 0;
	h->nlexed = 0;
	h->next = 0;
	ckput(h, 0, 0);
}

void
hl_sync(Hl *h, Doc *d)
{
	int dl, i;

	if(d->dirty < 0)
		return;
	dl = buf_pos_to_line(d->b, d->dirty);
	d->dirty = -1;
	if(h->nck == 0)
		return;
	i = ckfind(h, dl);
	if(h->ck[i].line > dl)
		i = 0;
	h->nck = i + 1;		/* keep checkpoints with line <= dl */
}

int
hl_line(Hl *h, Buf *b, Pos ls, Hstate *st, uchar *slots)
{
	int cap, got, n, full, i;
	Pos len;

	cap = h->cap;
	if(cap < 1)
		cap = 1;
	if(cap > Hlcap)
		cap = Hlcap;
	len = buf_len(b);
	got = buf_read(b, ls, h->text, cap + 1);
	for(n = 0; n < got && h->text[n] != '\n'; n++)
		;
	if(n < got){		/* newline at n <= cap */
		full = 1;
		h->next = ls + n + 1;
		if(n > 0 && h->text[n-1] == '\r')
			n--;
	}else if(got <= cap){	/* the buffer ends the line */
		full = 1;
		h->next = len;
	}else{			/* longer than the cap: lex the first cap bytes, find the next line the cheap way */
		full = 0;
		n = cap;
		h->next = buf_line_start(b, buf_pos_to_line(b, ls) + 1);
		if(h->next <= ls)
			h->next = len;
	}
	h->nlexed++;
	if(h->lang == nil || h->lang->lex == nil){
		for(i = 0; i < n; i++)
			slots[i] = Sdefault;
		return n;
	}
	h->lang->lex(st, (uchar*)h->text, n, full, slots);
	return n;
}

Hstate
hl_state(Hl *h, Buf *b, int line)
{
	int i, l, start, last;
	Hstate st;
	Pos ls;

	if(h->lang == nil || h->lang->lex == nil)
		return 0;
	if(h->nck == 0){
		ckput(h, 0, 0);
		if(h->nck == 0)
			return 0;	/* out of memory: no highlight state */
	}
	last = buf_nlines(b) - 1;
	if(line > last)
		line = last;
	if(line <= 0)
		return 0;
	i = ckfind(h, line);
	start = h->ck[i].line;
	st = h->ck[i].st;
	if(start == line)
		return st;
	if(line - start > h->look){
		l = (line - h->look) / Hlckint * Hlckint;
		if(l > start){
			start = l;
			st = 0;
			ckput(h, start, 0);
		}
	}
	ls = buf_line_start(b, start);
	for(l = start; l < line; l++){
		hl_line(h, b, ls, &st, h->slots);
		ls = h->next;
		if((l + 1) % Hlckint == 0)
			ckput(h, l + 1, st);
	}
	return st;
}

/* the bracket family of c: 1 opener, -1 closer, 0 none; *mate gets the partner byte */
static int
brfam(int c, int *mate)
{
	switch(c){
	case '(': *mate = ')'; return 1;
	case '[': *mate = ']'; return 1;
	case '{': *mate = '}'; return 1;
	case ')': *mate = '('; return -1;
	case ']': *mate = '['; return -1;
	case '}': *mate = '{'; return -1;
	}
	return 0;
}

/* Partner of the bracket at p. Only brackets in the same slot as the one at p count (code brackets
 * skip comments and strings). Openers scan forward with a depth count; closers lex forward from a
 * bounded start with a stack of eligible openers (the lexer never runs backwards). Any bound or an
 * overflow gives -1. */
Pos
hl_match(Hl *h, Buf *b, Pos p)
{
	int c, mate, fam, line, l, s, i, n, depth, slot, top, cl, lines;
	Pos ls, len, from, bytes, r, *stack;
	Hstate st;

	len = buf_len(b);
	if(p < 0 || p >= len)
		return -1;
	c = buf_byte(b, p);
	fam = brfam(c, &mate);
	if(fam == 0)
		return -1;
	line = buf_pos_to_line(b, p);
	ls = buf_line_start(b, line);
	st = hl_state(h, b, line);
	n = hl_line(h, b, ls, &st, h->slots);
	slot = p - ls < n ? h->slots[p - ls] : Sdefault;
	r = -1;
	if(fam > 0){
		/* forward: the rest of this line, then following lines */
		st = hl_state(h, b, line);
		depth = 0;
		bytes = 0;
		for(l = 0; l < Brlines && ls < len; l++){
			n = hl_line(h, b, ls, &st, h->slots);
			for(i = l == 0 ? p - ls + 1 : 0; i < n; i++){
				if(h->slots[i] != slot)
					continue;
				if(h->text[i] == c)
					depth++;
				else if(h->text[i] == mate){
					if(depth == 0)
						return ls + i;
					depth--;
				}
			}
			bytes += h->next - ls;
			if(bytes > Brbytes || h->next <= ls)
				break;
			ls = h->next;
		}
		return -1;
	}
	/* backward: lex forward from a bounded start, keep a stack of eligible openers */
	s = line - Brlines;
	if(s < 0)
		s = 0;
	from = p > Brbytes ? p - Brbytes : 0;
	lines = buf_pos_to_line(b, from);
	if(lines > s)
		s = lines;
	stack = malloc(Brstack * sizeof *stack);
	if(stack == nil)
		return -1;
	top = 0;
	st = hl_state(h, b, s);
	ls = buf_line_start(b, s);
	for(l = s; l <= line; l++){
		n = hl_line(h, b, ls, &st, h->slots);
		cl = l == line ? p - ls : n;
		if(cl > n)
			cl = n;
		for(i = 0; i < cl; i++){
			if(h->slots[i] != slot)
				continue;
			if(h->text[i] == mate){
				if(top >= Brstack){
					free(stack);
					return -1;
				}
				stack[top++] = ls + i;
			}else if(h->text[i] == c && top > 0)
				top--;
		}
		if(l == line)
			break;
		if(h->next <= ls)
			break;
		ls = h->next;
	}
	if(l == line && top > 0)
		r = stack[top - 1];
	free(stack);
	return r;
}

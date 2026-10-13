#include "compat.h"
#include "coil.h"

/*
 * rx.c: the regexp seam. libregexp compiles and runs the patterns; this file adds
 * what it lacks: a regerror that returns instead of exiting, the \n and \t escapes
 * libregexp does not know, and (below) a windowed forward scan over the piece table.
 */

static char rxerr[128];

int rx_winsize = Scanwin;
int rx_failwin = -1;

/* libregexp calls this on a syntax error; the default one prints and exits the process. */
void
regerror(char *s)
{
	lsnprint(rxerr, sizeof rxerr, "%s", s);
}

/* \n -> newline, \t -> tab; every other backslash pair is kept as two bytes (libregexp
 * reads the second byte literally). A trailing lone backslash is kept. */
int
rx_translate(char *in, char *out, int nout, char *err, int nerr)
{
	int n;

	n = 0;
	while(*in != '\0'){
		if(n + 2 >= nout || n >= Maxpat)
			goto toolong;
		if(in[0] == '\\' && in[1] == 'n'){
			out[n++] = '\n';
			in += 2;
		}else if(in[0] == '\\' && in[1] == 't'){
			out[n++] = '\t';
			in += 2;
		}else if(in[0] == '\\' && in[1] != '\0'){
			out[n++] = in[0];
			out[n++] = in[1];
			in += 2;
		}else
			out[n++] = *in++;
	}
	if(n > Maxpat)
		goto toolong;
	out[n] = '\0';
	return n;
toolong:
	if(nout > 0)
		out[0] = '\0';
	lsnprint(err, nerr, "regexp too long");
	return -1;
}

int
pat_comp(Pat *p, char *src, int literal, char *err, int nerr)
{
	char t[Maxpat + 2];
	int n;

	p->rx = nil;
	p->multiline = 0;
	p->literal = literal;
	p->src[0] = '\0';
	if(strlen(src) > Maxpat){
		lsnprint(err, nerr, "regexp too long");
		return -1;
	}
	if(literal){
		n = strlen(src);
		memmove(t, src, n + 1);
	}else if((n = rx_translate(src, t, sizeof t, err, nerr)) < 0)
		return -1;
	rxerr[0] = '\0';
	p->rx = literal ? regcomplit(t) : regcomp(t);
	if(p->rx == nil){
		lsnprint(err, nerr, "bad regexp: %s", rxerr[0] != '\0' ? rxerr : "cannot compile");
		return -1;
	}
	/*
	 * regcomp mallocs the program without clearing Reinst.gen, and regexec clears
	 * the gens only when startinst->gen is non-zero: with stale memory the first
	 * real regexec can lose a match at its first byte. One throwaway call leaves
	 * startinst->gen set, so every later call starts from cleared gens.
	 */
	regexec(p->rx, "", nil, 0);
	p->multiline = memchr(t, '\n', n) != nil;
	memmove(p->src, src, strlen(src) + 1);
	return 0;
}

void
pat_free(Pat *p)
{
	free(p->rx);
	p->rx = nil;
}

/*
 * Windowed forward scan. libregexp wants one contiguous NUL-terminated string, the
 * buffer is a piece table: the scan copies a window of at most winsize bytes (plus a
 * lookahead for patterns containing a newline) into scratch and matches there.
 *
 *  - win[0] is the byte before the window ('\n' at buffer start) so "^" sees the
 *    true left context; text starts at win[1].
 *  - a window ends after the last '\n' of its block when there is one, so single
 *    line patterns never see a line cut (a line longer than winsize is cut at a rune
 *    start; a match can then be truncated there: documented limit).
 *  - "$" matches only at a true end (buffer end, hi followed by '\n', or before a
 *    '\n'). Where the window ends mid-text the next real byte is copied and
 *    Resub.ep points at it: libregexp then stops the string there and "$" cannot
 *    match at ep.
 *  - NUL bytes become 0x01 so they cannot end the string (positions stay exact).
 *  - an empty match right at the end of the previous match is skipped (sam's rule),
 *    so "a*" always makes progress.
 */

enum { Unlimited = 1 << 30 };

void
scan_init(Scan *s, Pat *p, Buf *b, Pos lo, Pos hi)
{
	Pos len;

	len = buf_len(b);
	if(hi > len)
		hi = len;
	if(lo > hi)
		lo = hi;
	memset(s, 0, sizeof *s);
	s->pat = p;
	s->b = b;
	s->lo = lo;
	s->hi = hi;
	s->pos = lo;
	s->prevend = -1;
	s->wbase = -1;
	s->wend = -1;
	s->blockend = -1;
	s->winsize = rx_winsize;
}

void
scan_free(Scan *s)
{
	free(s->win);
	s->win = nil;
	s->wcap = 0;
	s->wbase = s->wend = s->blockend = -1;
}

/* finish a window holding [p, e): NULs, context byte, terminator, ep rule; blockend = bend */
static int
seal(Scan *s, Pos p, Pos e, Pos bend)
{
	Pos len;
	char *t;
	int i;

	len = buf_len(s->b);
	t = s->win;
	for(i = 1; i <= e - p; i++)
		if(t[i] == '\0')
			t[i] = 1;
	t[0] = p == 0 ? '\n' : (char)buf_byte(s->b, p - 1);
	if(t[0] == '\0')
		t[0] = 1;
	if(e >= len || buf_byte(s->b, e) == '\n'){
		s->atend = 1;
		t[1 + e - p] = '\0';
	}else{
		s->atend = 0;
		t[1 + e - p] = (char)buf_byte(s->b, e);
		if(t[1 + e - p] == '\0')
			t[1 + e - p] = 1;
		t[2 + e - p] = '\0';
	}
	s->wbase = p;
	s->wend = e;
	s->blockend = bend;
	return 1 + (int)(e - p);
}

/* grow the window to cap bytes; 0, or -1 when out of memory */
static int
wingrow(Scan *s, int cap)
{
	char *t;

	if(s->wcap >= cap)
		return 0;
	if(rx_failwin == 0)
		return -1;
	if(rx_failwin > 0)
		rx_failwin--;
	t = realloc(s->win, cap);
	if(t == nil)
		return -1;
	s->win = t;
	s->wcap = cap;
	return 0;
}

/* build the window for a search starting at p; returns the bytes copied, or -1 when out of memory */
static int
fill(Scan *s, Pos p)
{
	Pos rend, bend, e;
	int n, i, extra;
	char *t;

	extra = s->pat->multiline ? Scanlook : 0;
	if(wingrow(s, 1 + s->winsize + extra + 2) < 0)
		return -1;
	t = s->win;
	rend = p + s->winsize + extra;
	if(rend > s->hi || rend < p)
		rend = s->hi;
	n = buf_read(s->b, p, t + 1, rend - p);
	rend = p + n;
	/* block: [p, bend) */
	bend = p + s->winsize;
	if(bend >= s->hi || bend < p)
		bend = s->hi;
	if(bend > rend)
		bend = rend;
	if(bend < s->hi){
		for(i = bend - p; i > 0; i--)
			if(t[i] == '\n')	/* t[i] = byte at p + i - 1 */
				break;
		if(i > 0)
			bend = p + i;
		else{
			while(bend > p + 1 && (buf_byte(s->b, bend) & 0xC0) == 0x80)
				bend--;
		}
	}
	/* text: [p, e) */
	e = bend + extra;
	if(e > rend)
		e = rend;
	if(e < s->hi)
		while(e > bend && e > p + 1 && (buf_byte(s->b, e) & 0xC0) == 0x80)
			e--;
	return seal(s, p, e, bend);
}

int
scan_next(Scan *s, int *budget, Match *m)
{
	Resub sub[Nsub];
	Pos st, en, from;
	int k, used, big;
	char *tp;

	big = Unlimited;
	if(budget == nil)
		budget = &big;
	for(;;){
		if(s->pos > s->hi)
			return Sdone;
		if(s->wbase < 0 || s->pos < s->wbase || s->pos >= s->blockend){
			if(*budget <= 0)
				return Smore;
			used = fill(s, s->pos);
			if(used < 0)
				return Serror;	/* not "exhausted": callers must not treat it as no match */
			*budget -= used;
		}
		tp = s->win + 1;	/* text pointer of absolute position q is tp + (q - wbase) */
		memset(sub, 0, sizeof sub);
		sub[0].sp = tp + (s->pos - s->wbase);
		sub[0].ep = s->atend ? nil : tp + (s->wend - s->wbase);
		from = s->pos;
		if(regexec(s->pat->rx, s->win, sub, Nsub) == 0 || sub[0].sp == nil){
			*budget -= (int)(s->blockend - from);
			if(s->blockend >= s->hi || s->blockend <= from)
				s->pos = s->hi + 1;
			else
				s->pos = s->blockend;
			continue;
		}
		st = (Pos)(sub[0].sp - tp) + s->wbase;
		en = (Pos)(sub[0].ep - tp) + s->wbase;
		if(st >= s->blockend && s->blockend < s->hi){
			*budget -= (int)(s->blockend - from);
			s->pos = s->blockend;
			continue;
		}
		*budget -= (int)(en - from);
		if(en == st && st == s->prevend){
			s->pos = st < s->hi ? buf_next(s->b, st) : s->hi + 1;
			continue;
		}
		for(k = 0; k < Nsub; k++){
			if(sub[k].sp == nil || sub[k].ep == nil){
				m->sp[k] = -1;
				m->ep[k] = -1;
			}else{
				m->sp[k] = (Pos)(sub[k].sp - tp) + s->wbase;
				m->ep[k] = (Pos)(sub[k].ep - tp) + s->wbase;
			}
		}
		m->sp[0] = st;
		m->ep[0] = en;
		s->prevend = en;
		if(en == st)
			s->pos = st < s->hi ? buf_next(s->b, st) : s->hi + 1;
		else
			s->pos = en;
		return Smatch;
	}
}

/*
 * Backward scan: the match with the greatest start in [lo, limit). Blocks of at most
 * winsize bytes are walked from limit down; a block starts at a line start when one
 * lies inside it. The window of a block extends to the end of the line holding the
 * block end (plus the multiline lookahead) so a match starting before the block end
 * may run past it. Inside a block every matching start is tried (retry from the next
 * rune after each start, so overlapping candidates count) and the last one wins.
 * The budget is charged per window and per retry; Smore is returned between blocks.
 */
int
scan_prev(Scan *s, Pos limit, int *budget, Match *m)
{
	Resub sub[Nsub];
	Pos bs, be, pos, st, from, q, e, rend;
	int k, n, cap, extra, big, found, used;
	char *t, *tp;

	big = Unlimited;
	if(budget == nil)
		budget = &big;
	if(!s->pstarted){
		s->pstarted = 1;
		s->pbe = limit < s->hi ? limit : s->hi;
	}
	extra = s->pat->multiline ? Scanlook : 0;
	for(;;){
		be = s->pbe;
		if(be <= s->lo)
			return Sdone;
		if(*budget <= 0)
			return Smore;
		bs = be - s->winsize;
		if(bs < s->lo || bs > be)
			bs = s->lo;
		if(bs > s->lo){
			for(q = bs; q < be; q++)
				if(buf_byte(s->b, q) == '\n')
					break;
			if(q + 1 < be)
				bs = q + 1;
			else{
				while(bs > s->lo + 1 && (buf_byte(s->b, bs) & 0xC0) == 0x80)
					bs--;
			}
		}
		cap = 1 + (int)(be - bs) + s->winsize + extra + 2;
		if(wingrow(s, cap) < 0)
			return Serror;
		t = s->win;
		rend = be + s->winsize + extra;
		if(rend > s->hi || rend < be)
			rend = s->hi;
		n = buf_read(s->b, bs, t + 1, rend - bs);
		rend = bs + n;
		for(q = be; q < rend; q++)
			if(t[1 + q - bs] == '\n')
				break;
		e = q < rend ? q + extra : rend;
		if(e > rend)
			e = rend;
		if(e < s->hi)
			while(e > be && e > bs + 1 && (buf_byte(s->b, e) & 0xC0) == 0x80)
				e--;
		used = seal(s, bs, e, be);
		*budget -= used;
		tp = s->win + 1;
		found = 0;
		pos = bs;
		while(pos < be){
			memset(sub, 0, sizeof sub);
			sub[0].sp = tp + (pos - bs);
			sub[0].ep = s->atend ? nil : tp + (e - bs);
			from = pos;
			if(regexec(s->pat->rx, s->win, sub, Nsub) == 0 || sub[0].sp == nil)
				break;
			st = (Pos)(sub[0].sp - tp) + bs;
			if(st >= be)
				break;
			for(k = 0; k < Nsub; k++){
				if(sub[k].sp == nil || sub[k].ep == nil){
					m->sp[k] = -1;
					m->ep[k] = -1;
				}else{
					m->sp[k] = (Pos)(sub[k].sp - tp) + bs;
					m->ep[k] = (Pos)(sub[k].ep - tp) + bs;
				}
			}
			m->sp[0] = st;
			m->ep[0] = (Pos)(sub[0].ep - tp) + bs;
			found = 1;
			*budget -= (int)(st - from) + 1;
			pos = buf_next(s->b, st);
			if(pos <= st)
				break;
		}
		if(found){
			s->pbe = bs;
			return Smatch;
		}
		s->pbe = bs;
	}
}

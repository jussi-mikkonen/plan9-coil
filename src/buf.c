#include "compat.h"
#include "coil.h"

/*
 * Piece table over raw bytes. orig is the loaded file, never modified;
 * add is an append-only store of inserted bytes. Deleted bytes stay in
 * their store, so undo (buf_insert_pieces) never copies text.
 * start[] and nlstart[] are prefix sums over pieces, rebuilt lazily from
 * the clean watermark: no per-file line table.
 */

static void
seterr(char *err, int nerr, char *msg)
{
	if(err != nil && nerr > 0)
		lsnprint(err, nerr, "%s", msg);
}

static char*
store(Buf *b, Piece *p)
{
	return (p->src == 0 ? b->orig : b->add) + p->off;
}

static int
countnl(char *s, Pos n)
{
	char *e;
	int c;

	c = 0;
	e = s + n;
	while(s < e && (s = memchr(s, '\n', e - s)) != nil){
		c++;
		s++;
	}
	return c;
}

Buf*
buf_new(void)
{
	Buf *b;

	b = czalloc(sizeof(Buf));
	if(b == nil)
		return nil;
	b->max = Maxfile;
	return b;
}

void
buf_free(Buf *b)
{
	if(b == nil)
		return;
	free(b->orig);
	free(b->add);
	free(b->p);
	free(b->start);
	free(b->nlstart);
	free(b);
}

/* make room for k more pieces (and prefix slots) */
static int
growp(Buf *b, int k)
{
	int ncap;
	Piece *np;
	Pos *ns;
	int *nn;

	if(b->np + k <= b->pcap)
		return 0;
	ncap = b->pcap * 2;
	if(ncap < b->np + k)
		ncap = b->np + k;
	if(ncap < 16)
		ncap = 16;
	np = realloc(b->p, ncap * sizeof(Piece));
	if(np == nil)
		return -1;
	b->p = np;
	ns = realloc(b->start, ncap * sizeof(Pos));
	if(ns == nil)
		return -1;
	b->start = ns;
	nn = realloc(b->nlstart, ncap * sizeof(int));
	if(nn == nil)
		return -1;
	b->nlstart = nn;
	b->pcap = ncap;
	return 0;
}

/* rebuild prefix sums for pieces [clean, np) */
static void
fix(Buf *b)
{
	int i;

	ASSERT(b->clean <= b->np || b->np == 0);
	if(b->clean > b->np)
		b->clean = b->np;
	for(i = b->clean; i < b->np; i++){
		if(i == 0){
			b->start[0] = 0;
			b->nlstart[0] = 0;
		}else{
			b->start[i] = b->start[i-1] + b->p[i-1].len;
			b->nlstart[i] = b->nlstart[i-1] + b->p[i-1].nl;
		}
	}
	b->clean = b->np;
}

/* piece containing pos, with offset into it; np (off 0) when pos >= len */
static int
find(Buf *b, Pos pos, Pos *off)
{
	int lo, hi, mid, i;

	fix(b);
	if(pos >= b->len || b->np == 0){
		*off = 0;
		return b->np;
	}
	i = b->lastpi;
	if(i < b->np && b->start[i] <= pos && pos < b->start[i] + b->p[i].len){
		*off = pos - b->start[i];
		return i;
	}
	if(i + 1 < b->np && b->start[i+1] <= pos && pos < b->start[i+1] + b->p[i+1].len){
		b->lastpi = i + 1;
		*off = pos - b->start[i+1];
		return i + 1;
	}
	lo = 0;
	hi = b->np - 1;
	while(lo < hi){
		mid = (lo + hi + 1) / 2;
		if(b->start[mid] <= pos)
			lo = mid;
		else
			hi = mid - 1;
	}
	ASSERT(b->start[lo] <= pos && pos < b->start[lo] + b->p[lo].len);
	b->lastpi = lo;
	*off = pos - b->start[lo];
	return lo;
}

/* ensure a piece boundary at pos; returns the index of the piece starting there (np at end), -1 if out of memory */
static int
split(Buf *b, Pos pos)
{
	int i, lnl;
	Pos off;
	Piece *p;

	i = find(b, pos, &off);
	if(off == 0)
		return i;
	if(growp(b, 1) < 0)
		return -1;
	p = &b->p[i];
	memmove(&b->p[i+2], &b->p[i+1], (b->np - i - 1) * sizeof(Piece));
	lnl = countnl(store(b, p), off);
	b->p[i+1].src = p->src;
	b->p[i+1].off = p->off + off;
	b->p[i+1].len = p->len - off;
	b->p[i+1].nl = p->nl - lnl;
	p->len = off;
	p->nl = lnl;
	b->np++;
	if(b->clean > i + 1)
		b->clean = i + 1;
	return i + 1;
}

int
buf_load(Buf *b, char *data, Pos n, char *err, int nerr)
{
	Pos off, end, k;
	int back, nl;
	Piece *p;

	if(n < 0 || n > b->max){
		free(data);
		seterr(err, nerr, "file too large");
		return -1;
	}
	free(b->orig);
	free(b->add);
	b->orig = data;
	b->norig = n;
	b->add = nil;
	b->nadd = 0;
	b->addcap = 0;
	b->np = 0;
	b->clean = 0;
	b->lastpi = 0;
	b->len = n;
	b->nl = 0;
	off = 0;
	while(off < n){
		end = n;
		if(n - off > Chunk){
			end = off + Chunk;
			/* prefer a cut just after the last newline in the window */
			for(k = end - 1; k >= off; k--)
				if(data[k] == '\n')
					break;
			if(k >= off)
				end = k + 1;
			else{
				/* else a rune start: byte at the cut must not be a continuation */
				k = end;
				back = 0;
				while(back < 4 && k > off && !utf_isstart((uchar)data[k])){
					k--;
					back++;
				}
				if(k > off && utf_isstart((uchar)data[k]))
					end = k;
			}
		}
		if(growp(b, 1) < 0){
			seterr(err, nerr, "out of memory");
			b->np = 0;
			b->len = 0;
			b->nl = 0;
			return -1;
		}
		p = &b->p[b->np++];
		p->src = 0;
		p->off = off;
		p->len = end - off;
		nl = countnl(data + off, end - off);
		p->nl = nl;
		b->nl += nl;
		off = end;
	}
	return 0;
}

Pos
buf_len(Buf *b)
{
	return b->len;
}

int
buf_nlines(Buf *b)
{
	return b->nl + 1;
}

int
buf_byte(Buf *b, Pos pos)
{
	Pos off;
	int i;

	if(pos < 0 || pos >= b->len)
		return -1;
	i = find(b, pos, &off);
	ASSERT(i < b->np);
	return (uchar)store(b, &b->p[i])[off];
}

int
buf_read(Buf *b, Pos pos, char *out, int n)
{
	Pos off, m;
	int i, got;
	Piece *p;

	if(pos < 0 || pos >= b->len || n <= 0)
		return 0;
	i = find(b, pos, &off);
	got = 0;
	while(got < n && i < b->np){
		p = &b->p[i];
		m = p->len - off;
		if(m > n - got)
			m = n - got;
		memmove(out + got, store(b, p) + off, m);
		got += m;
		off = 0;
		i++;
	}
	return got;
}

int
buf_rune(Buf *b, Pos pos, Rune *r)
{
	char tmp[4];
	int n;

	n = buf_read(b, pos, tmp, 4);
	if(n == 0){
		*r = Runeerr;
		return 0;
	}
	return utf_dec(tmp, n, r);
}

Pos
buf_next(Buf *b, Pos pos)
{
	Rune r;
	int n;

	if(pos < 0)
		return 0;
	if(pos >= b->len)
		return b->len;
	n = buf_rune(b, pos, &r);
	return pos + n;
}

Pos
buf_prev(Buf *b, Pos pos)
{
	Pos q;
	Rune r;
	int n;

	if(pos <= 0)
		return 0;
	if(pos > b->len)
		pos = b->len;
	q = pos - 1;
	while(q > 0 && pos - q < 4 && !utf_isstart(buf_byte(b, q)))
		q--;
	n = buf_rune(b, q, &r);
	if(q + n == pos)
		return q;
	return pos - 1;
}

Pos
buf_cnext(Buf *b, Pos pos)
{
	Pos q;

	q = buf_next(b, pos);
	if(q < b->len && buf_byte(b, pos) == '\r' && buf_byte(b, q) == '\n')
		q = buf_next(b, q);
	return q;
}

Pos
buf_cprev(Buf *b, Pos pos)
{
	Pos q;

	q = buf_prev(b, pos);
	if(q > 0 && buf_byte(b, q) == '\n' && buf_byte(b, q - 1) == '\r')
		q--;
	return q;
}

Pos
buf_snap(Buf *b, Pos pos)
{
	Pos q;
	Rune r;
	int n;

	if(pos <= 0)
		return 0;
	if(pos >= b->len)
		return b->len;
	q = pos;
	while(q > 0 && pos - q < 3 && !utf_isstart(buf_byte(b, q)))
		q--;
	n = buf_rune(b, q, &r);
	if(q < pos && q + n > pos)
		return q;
	return pos;
}

Pos
buf_line_start(Buf *b, int line)
{
	int lo, hi, mid, k;
	char *s, *e, *base;

	if(line <= 0)
		return 0;
	if(line > b->nl)
		line = b->nl;
	if(line == 0)
		return 0;
	fix(b);
	ASSERT(b->np > 0);
	/* smallest piece whose newlines reach the line-th one */
	lo = 0;
	hi = b->np - 1;
	while(lo < hi){
		mid = (lo + hi) / 2;
		if(b->nlstart[mid] + b->p[mid].nl >= line)
			hi = mid;
		else
			lo = mid + 1;
	}
	ASSERT(b->nlstart[lo] < line && line <= b->nlstart[lo] + b->p[lo].nl);
	k = line - b->nlstart[lo];
	base = store(b, &b->p[lo]);
	s = base;
	e = base + b->p[lo].len;
	while(k > 0){
		s = memchr(s, '\n', e - s);
		ASSERT(s != nil);
		if(s == nil)
			return b->len;
		s++;
		k--;
	}
	return b->start[lo] + (Pos)(s - base);
}

int
buf_pos_to_line(Buf *b, Pos pos)
{
	Pos off;
	int i;

	if(pos <= 0)
		return 0;
	if(pos >= b->len)
		return b->nl;
	i = find(b, pos, &off);
	ASSERT(i < b->np);
	return b->nlstart[i] + countnl(store(b, &b->p[i]), off);
}

int
buf_write(Buf *b, int (*fn)(void *arg, char *p, int n), void *arg)
{
	int i;
	Pos off, m;
	Piece *p;
	char *s;

	for(i = 0; i < b->np; i++){
		p = &b->p[i];
		s = store(b, p);
		for(off = 0; off < p->len; off += m){
			m = p->len - off;
			if(m > Chunk)
				m = Chunk;
			if(fn(arg, s + off, (int)m) != (int)m)
				return -1;
		}
	}
	return 0;
}

/* append n bytes to the add store; returns the offset they landed at, -1 if out of memory */
static Pos
addbytes(Buf *b, char *s, Pos n)
{
	Pos ncap, at;
	char *na;

	if(b->nadd + n > b->addcap){
		ncap = b->addcap > b->max / 2 ? b->max : b->addcap * 2;
		if(ncap < b->nadd + n)
			ncap = b->nadd + n;
		if(ncap < 4096)
			ncap = 4096;
		na = realloc(b->add, ncap);
		if(na == nil)
			return -1;
		b->add = na;
		b->addcap = ncap;
	}
	at = b->nadd;
	memmove(b->add + at, s, n);
	b->nadd += n;
	return at;
}

int
buf_insert(Buf *b, Pos pos, char *s, Pos n, char *err, int nerr)
{
	Pos off, at, m, done;
	int i, q, k, nl;
	Piece *p;

	if(pos < 0 || pos > b->len || n < 0){
		seterr(err, nerr, "bad position");
		return -1;
	}
	if(n == 0)
		return 0;
	if(n > b->max - b->len || n > b->max - b->nadd){
		seterr(err, nerr, "buffer full");
		return -1;
	}
	/* typing coalescing: extend the add piece that ends exactly at pos */
	i = find(b, pos, &off);
	if(off == 0 && i > 0){
		q = i - 1;
		p = &b->p[q];
		if(p->src == 1 && p->off + p->len == b->nadd && p->len + n <= Chunk){
			if(addbytes(b, s, n) < 0){
				seterr(err, nerr, "out of memory");
				return -1;
			}
			nl = countnl(s, n);
			p->len += n;
			p->nl += nl;
			b->len += n;
			b->nl += nl;
			if(b->clean > q + 1)
				b->clean = q + 1;
			return 0;
		}
	}
	at = addbytes(b, s, n);
	if(at < 0){
		seterr(err, nerr, "out of memory");
		return -1;
	}
	i = split(b, pos);
	if(i < 0){
		seterr(err, nerr, "out of memory");
		return -1;
	}
	k = (n + Chunk - 1) / Chunk;
	if(growp(b, k) < 0){
		seterr(err, nerr, "out of memory");
		return -1;
	}
	memmove(&b->p[i+k], &b->p[i], (b->np - i) * sizeof(Piece));
	for(q = 0, done = 0; q < k; q++, done += m){
		m = n - done;
		if(m > Chunk)
			m = Chunk;
		p = &b->p[i+q];
		p->src = 1;
		p->off = at + done;
		p->len = m;
		p->nl = countnl(s + done, m);
		b->nl += p->nl;
	}
	b->np += k;
	b->len += n;
	if(b->clean > i)
		b->clean = i;
	return 0;
}

int
buf_delete(Buf *b, Pos pos, Pos n, PieceRun *rm)
{
	int i, j, k, nl;

	if(rm != nil){
		rm->p = nil;
		rm->n = 0;
	}
	if(pos < 0 || n < 0 || pos > b->len || n > b->len - pos)
		return -1;
	if(n == 0)
		return 0;
	i = split(b, pos);
	if(i < 0)
		return -1;
	j = split(b, pos + n);
	if(j < 0)
		return -1;
	ASSERT(j > i);
	if(rm != nil){
		rm->p = malloc((j - i) * sizeof(Piece));
		if(rm->p == nil)
			return -1;
		memmove(rm->p, &b->p[i], (j - i) * sizeof(Piece));
		rm->n = j - i;
	}
	nl = 0;
	for(k = i; k < j; k++)
		nl += b->p[k].nl;
	memmove(&b->p[i], &b->p[j], (b->np - j) * sizeof(Piece));
	b->np -= j - i;
	b->len -= n;
	b->nl -= nl;
	if(b->clean > i)
		b->clean = i;
	return 0;
}

int
buf_insert_pieces(Buf *b, Pos pos, PieceRun *run)
{
	Pos total;
	int i, k, nl;

	if(run == nil || run->n <= 0)
		return 0;
	if(pos < 0 || pos > b->len)
		return -1;
	total = 0;
	nl = 0;
	for(k = 0; k < run->n; k++){
		if(run->p[k].len > b->max - total)
			return -1;
		total += run->p[k].len;
		nl += run->p[k].nl;
	}
	if(total > b->max - b->len)
		return -1;
	i = split(b, pos);
	if(i < 0)
		return -1;
	if(growp(b, run->n) < 0)
		return -1;
	memmove(&b->p[i + run->n], &b->p[i], (b->np - i) * sizeof(Piece));
	memmove(&b->p[i], run->p, run->n * sizeof(Piece));
	b->np += run->n;
	b->len += total;
	b->nl += nl;
	if(b->clean > i)
		b->clean = i;
	return 0;
}

int
buf_addpieces(Buf *b, char *s, Pos n, PieceRun *out)
{
	Pos at, m, done;
	int k, q;
	Piece *p;

	out->p = nil;
	out->n = 0;
	if(n <= 0)
		return n == 0 ? 0 : -1;
	if(n > b->max - b->nadd)
		return -1;
	k = (n + Chunk - 1) / Chunk;
	p = malloc(k * sizeof(Piece));
	if(p == nil)
		return -1;
	at = addbytes(b, s, n);
	if(at < 0){
		free(p);
		return -1;
	}
	for(q = 0, done = 0; q < k; q++, done += m){
		m = n - done;
		if(m > Chunk)
			m = Chunk;
		p[q].src = 1;
		p[q].off = at + done;
		p[q].len = m;
		p[q].nl = countnl(s + done, m);
	}
	out->p = p;
	out->n = k;
	return 0;
}

/* cursor into the old piece array: the unconsumed text starts at old piece ci, byte coff, and cnl is the newline count of [0, coff) */
typedef struct Cur Cur;
struct Cur {
	int	ci;
	Pos	coff;
	int	cnl;
};

/* append the old text between the cursor and (i, off) to dst as pieces (dst nil: discard) and move the cursor there */
static void
consume(Buf *b, Cur *c, int i, Pos off, Piece *dst, int *nd)
{
	Piece *p, *q;
	int nl;

	while(c->ci != i || c->coff != off){
		p = &b->p[c->ci];
		if(c->ci == i){
			nl = c->cnl + countnl(store(b, p) + c->coff, off - c->coff);
			if(dst != nil){
				q = &dst[(*nd)++];
				q->src = p->src;
				q->off = p->off + c->coff;
				q->len = off - c->coff;
				q->nl = nl - c->cnl;
			}
			c->coff = off;
			c->cnl = nl;
			return;
		}
		if(dst != nil){
			q = &dst[(*nd)++];
			q->src = p->src;
			q->off = p->off + c->coff;
			q->len = p->len - c->coff;
			q->nl = p->nl - c->cnl;
		}
		c->ci++;
		c->coff = 0;
		c->cnl = 0;
	}
}

int
buf_splice(Buf *b, Splice *s, int n)
{
	Piece *out;
	Pos *ns, end, len, pos, off, off2;
	int *nn;
	int k, m, i, j, nout, cap, first, nl;
	Cur c;

	m = 0;

	if(n <= 0)
		return 0;
	end = 0;
	cap = b->np + 1;
	for(k = 0; k < n; k++){
		if(s[k].pos < end || s[k].ndel < 0 || s[k].ndel > b->len - s[k].pos)
			return -1;
		end = s[k].pos + s[k].ndel;
		cap += 2;
		if(s[k].ins != nil)
			cap += s[k].ins->n;
		if(s[k].rm != nil){
			s[k].rm->p = nil;
			s[k].rm->n = 0;
		}
	}
	out = malloc(cap * sizeof(Piece));
	if(out == nil)
		return -1;
	fix(b);
	nout = 0;
	c.ci = 0;
	c.coff = 0;
	c.cnl = 0;
	first = -1;
	for(k = 0; k < n; k++){
		pos = s[k].pos;
		i = find(b, pos, &off);
		if(first < 0)
			first = i;
		consume(b, &c, i, off, out, &nout);
		if(s[k].ndel > 0){
			j = find(b, pos + s[k].ndel, &off2);
			if(s[k].rm != nil){
				s[k].rm->p = malloc((j - i + 1) * sizeof(Piece));
				if(s[k].rm->p == nil)
					goto fail;
				m = 0;
				consume(b, &c, j, off2, s[k].rm->p, &m);
				s[k].rm->n = m;
			}else
				consume(b, &c, j, off2, nil, &m);
		}
		if(s[k].ins != nil)
			for(m = 0; m < s[k].ins->n; m++)
				out[nout++] = s[k].ins->p[m];
	}
	consume(b, &c, b->np, 0, out, &nout);
	len = 0;
	nl = 0;
	for(i = 0; i < nout; i++){
		len += out[i].len;
		nl += out[i].nl;
	}
	if(len > b->max)
		goto fail;
	if(cap > b->pcap){
		ns = realloc(b->start, cap * sizeof(Pos));
		if(ns == nil)
			goto fail;
		b->start = ns;
		nn = realloc(b->nlstart, cap * sizeof(int));
		if(nn == nil)
			goto fail;
		b->nlstart = nn;
		b->pcap = cap;
	}
	free(b->p);
	b->p = out;
	b->np = nout;
	b->pcap = cap;	/* out has cap slots; start/nlstart have at least that many */
	b->len = len;
	b->nl = nl;
	if(b->clean > first)
		b->clean = first;
	b->lastpi = 0;
	return 0;
fail:
	free(out);
	for(k = 0; k < n; k++)
		if(s[k].rm != nil)
			buf_runfree(s[k].rm);
	return -1;
}

void
buf_runfree(PieceRun *run)
{
	free(run->p);
	run->p = nil;
	run->n = 0;
}

static int
bad(char *why, int nwhy, char *msg)
{
	seterr(why, nwhy, msg);
	return -1;
}

int
buf_check(Buf *b, char *why, int nwhy)
{
	Pos len, lim, run;
	int nl, i, c, rn;
	Piece *p;

	len = 0;
	nl = 0;
	for(i = 0; i < b->np; i++){
		p = &b->p[i];
		if(p->len <= 0)
			return bad(why, nwhy, "empty piece");
		if(p->src > 1)
			return bad(why, nwhy, "bad source");
		lim = p->src == 0 ? b->norig : b->nadd;
		if(p->off < 0 || p->len > lim - p->off)
			return bad(why, nwhy, "piece outside store");
		c = countnl(store(b, p), p->len);
		if(c != p->nl)
			return bad(why, nwhy, "newline count");
		len += p->len;
		nl += c;
	}
	if(len != b->len)
		return bad(why, nwhy, "length");
	if(nl != b->nl)
		return bad(why, nwhy, "newline total");
	if(b->clean > b->np)
		return bad(why, nwhy, "clean past end");
	run = 0;
	rn = 0;
	for(i = 0; i < b->clean; i++){
		if(b->start[i] != run)
			return bad(why, nwhy, "start prefix");
		if(b->nlstart[i] != rn)
			return bad(why, nwhy, "nlstart prefix");
		run += b->p[i].len;
		rn += b->p[i].nl;
	}
	return 0;
}

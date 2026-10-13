#include "compat.h"
#include "coil.h"

/* render.c: Ed -> Canvas of (rune, style) cells. No libcell, no libdraw. */

int
canvas_resize(Canvas *cv, int rows, int cols)
{
	Rune *r;
	uchar *s;
	int n;

	if(rows < 0)
		rows = 0;
	if(cols < 0)
		cols = 0;
	n = rows * cols;
	free(cv->r);
	free(cv->style);
	cv->r = nil;
	cv->style = nil;
	cv->rows = 0;
	cv->cols = 0;
	if(n == 0){
		cv->rows = rows;
		cv->cols = cols;
		return 0;
	}
	r = malloc(n * sizeof(Rune));
	s = malloc(n);
	if(r == nil || s == nil){
		free(r);
		free(s);
		return -1;
	}
	cv->r = r;
	cv->style = s;
	cv->rows = rows;
	cv->cols = cols;
	return 0;
}

void
canvas_free(Canvas *cv)
{
	free(cv->r);
	free(cv->style);
	cv->r = nil;
	cv->style = nil;
	cv->rows = 0;
	cv->cols = 0;
}

static void
put(Canvas *cv, int row, int col, Rune r, int style)
{
	if(row < 0 || row >= cv->rows || col < 0 || col >= cv->cols)
		return;
	cv->r[row * cv->cols + col] = r;
	cv->style[row * cv->cols + col] = (uchar)style;
}

/* first selection whose max is >= p (binary search; the set is sorted and disjoint) */
static int
firstsel(SelSet *ss, Pos p)
{
	int lo, hi, mid;

	lo = 0;
	hi = ss->n;
	while(lo < hi){
		mid = (lo + hi) / 2;
		if(sel_max(&ss->s[mid]) < p)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

static void
textrow(Ed *ed, Canvas *cv, int row, int gw, int tc, Pos ls, Pos le, Pos match, Vcell *vc, uchar *syn, int nsyn)
{
	SelSet *ss;
	Buf *b;
	int i, endi, si, ci, ov, byte, slot, j;
	Pos p, e, tfrom, ex, c;

	ss = &ed->doc.sel;
	b = ed->doc.b;
	si = ci = firstsel(ss, ls);
	/* trailing run [tfrom, e): spaces and tabs before the line end, a final \r of \r\n excluded */
	e = le;
	if(e > ls && buf_byte(b, e - 1) == '\n'){
		e--;
		if(e > ls && buf_byte(b, e - 1) == '\r')
			e--;
	}
	tfrom = e;
	while(tfrom > ls && e - tfrom < Hlcap && (buf_byte(b, tfrom - 1) == ' ' || buf_byte(b, tfrom - 1) == '\t'))
		tfrom--;
	/* insert mode: whitespace before a caret on this line is not flagged */
	ex = ls;
	if(ed->mode == Minsert && tfrom < e){
		for(j = ci; j < ss->n && sel_min(&ss->s[j]) <= le; j++){
			c = ed_cursor(ed, &ss->s[j]);
			if(c >= ls && c <= le && c > ex)
				ex = c;
		}
	}
	endi = view_layout(&ed->view, ed->doc.b, ls, vc, tc);
	for(i = 0; i < tc; i++){
		if(endi >= 0 && i > endi)
			break;
		p = vc[i].pos;
		while(si < ss->n && sel_max(&ss->s[si]) <= p)
			si++;
		while(ci < ss->n && ed_cursor(ed, &ss->s[ci]) < p)
			ci++;
		ov = 0;
		if(si < ss->n && sel_min(&ss->s[si]) <= p && p < sel_max(&ss->s[si]))
			ov |= Osel;
		if(ci < ss->n && ed_cursor(ed, &ss->s[ci]) == p)
			ov |= Ocur;
		if(i == endi){
			byte = buf_byte(ed->doc.b, p);
			if(byte != '\n' && byte != '\r')
				ov &= ~Osel;
			if(ov != 0)
				put(cv, row, gw + i, ' ', Sdefault | ov);
			break;
		}
		/* Sctrl and Sinvalid (from the layout) win over the syntax slot of the source byte */
		slot = vc[i].slot;
		if(slot == Sdefault){
			if(p == match)
				slot = Sbracket;
			else if(p >= tfrom && p < e && p >= ex)
				slot = Strailing;
			else if(p >= ls && p - ls < nsyn)
				slot = syn[p - ls];
		}
		put(cv, row, gw + i, vc[i].r, slot | ov);
	}
}

/* gutter cells of one row: right-aligned line number (line < 0: blank), a separator space, all Slinenr */
static void
gutterrow(Canvas *cv, int row, int gw, int line)
{
	int c, n;

	for(c = 0; c < gw; c++)
		put(cv, row, c, ' ', Slinenr);
	if(line < 0)
		return;
	n = line + 1;
	for(c = gw - 2; c >= 0 && n > 0; c--){
		put(cv, row, c, '0' + n % 10, Slinenr);
		n /= 10;
	}
}

/* the prompt line: kind rune, then the text (its tail when it does not fit), a cursor cell after it */
static void
promptrow(Ed *ed, Canvas *cv)
{
	Prompt *p;
	Rune *rs;
	int i, n, nr, first, col, row;

	p = &ed->prompt;
	row = cv->rows - 1;
	rs = malloc((p->n + 1) * sizeof *rs);
	if(rs == nil)
		return;
	nr = 0;
	for(i = 0; i < p->n; nr++)
		i += utf_dec(p->text + i, p->n - i, &rs[nr]);
	n = cv->cols - 2;
	first = nr > n && n > 0 ? nr - n : 0;
	if(n < 0)
		first = nr;
	col = 0;
	put(cv, row, col++, p->kind, Sstatus);
	for(i = first; i < nr && col < cv->cols; i++)
		put(cv, row, col++, rs[i], Sstatus);
	if(col < cv->cols)
		put(cv, row, col++, ' ', Sstatus | Ocur);
	while(col < cv->cols)
		put(cv, row, col++, ' ', Sstatus);
	free(rs);
}

static void
statusrow(Ed *ed, Canvas *cv)
{
	char buf[512];
	int nmsg, i, n, col, st, used;
	Rune r;

	if(ed->prompt.on){
		promptrow(ed, cv);
		return;
	}
	ed_status(ed, buf, sizeof buf, &nmsg);
	n = strlen(buf);
	col = 0;
	i = 0;
	while(i < n && col < cv->cols){
		used = utf_dec(buf + i, n - i, &r);
		if(used <= 0)
			break;
		st = nmsg >= 0 && i >= nmsg ? Smessage : Sstatus;
		put(cv, cv->rows - 1, col, r, st);
		col++;
		i += used;
	}
	while(col < cv->cols){
		put(cv, cv->rows - 1, col, ' ', Sstatus);
		col++;
	}
}

void
render(Ed *ed, Canvas *cv)
{
	Buf *b;
	Vcell *vc;
	int i, n, line0, nl, textrows, row, hlon, nsyn, gw, tc, last, c;
	Hstate st;
	Pos ls, le, match;

	if(cv->rows <= 0 || cv->cols <= 0 || cv->r == nil)
		return;
	n = cv->rows * cv->cols;
	for(i = 0; i < n; i++){
		cv->r[i] = ' ';
		cv->style[i] = Sdefault;
	}
	ed_layout(ed);
	gw = ed_gutter(ed);
	tc = ed->view.cols;
	if(gw + tc > cv->cols)
		tc = cv->cols - gw;
	if(tc < 1)
		tc = 1;
	vc = malloc(tc * sizeof *vc);
	if(vc == nil)
		return;
	b = ed->doc.b;
	last = ed_lastline(ed);
	view_fix(&ed->view, b);
	textrows = cv->rows - 1;
	line0 = buf_pos_to_line(b, ed->view.top);
	nl = buf_nlines(b);
	hl_sync(&ed->hl, &ed->doc);
	match = -1;
	c = ed_cursor(ed, sel_primary(&ed->doc.sel));
	if(c < buf_len(b)){
		c = buf_byte(b, c);
		if(c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}')
			match = hl_match(&ed->hl, b, ed_cursor(ed, sel_primary(&ed->doc.sel)));
	}
	hlon = ed->hl.lang != nil && ed->hl.lang->lex != nil;
	st = 0;
	ls = buf_line_start(b, line0);
	if(hlon)
		st = hl_state(&ed->hl, b, line0);
	for(row = 0; row < textrows; row++){
		if(line0 + row >= nl)
			break;
		if(gw > 0)
			gutterrow(cv, row, gw, line0 + row <= last ? line0 + row : -1);
		nsyn = 0;
		if(hlon){
			nsyn = hl_line(&ed->hl, b, ls, &st, ed->hl.slots);
			textrow(ed, cv, row, gw, tc, ls, ed->hl.next, match, vc, ed->hl.slots, nsyn);
			ls = ed->hl.next;
		}else{
			le = line0 + row + 1 <= b->nl ? buf_line_start(b, line0 + row + 1) : buf_len(b);
			textrow(ed, cv, row, gw, tc, ls, le, match, vc, nil, 0);
			ls = le;
		}
	}
	if(gw > 0)
		for(; row < textrows; row++)
			gutterrow(cv, row, gw, -1);
	free(vc);
	statusrow(ed, cv);
}

#include "compat.h"
#include "coil.h"

/* view.c: the one layout walker. Render, pos->cell and cell->pos all use walk_next. */

void
view_init(View *v)
{
	memset(v, 0, sizeof *v);
	v->tabwidth = 4;
}

void
walk_init(Walk *w, View *v, Buf *b, Pos linestart)
{
	w->v = v;
	w->b = b;
	w->start = linestart;
	w->pos = linestart;
	w->col = 0;
	w->show2 = 0;
}

/*
 * Classify the rune at w->pos. Newline, EOF, a "\r" directly before "\n" and
 * (kind Wtrunc) a line longer than Maxline bytes end the line: no advance.
 * Otherwise advance past the rune. Wctrl shows '^' then w->show2 (two cells).
 */
int
walk_next(Walk *w, Rune *show, int *cells)
{
	Rune r;
	int n, c, tw;

	*show = ' ';
	*cells = 0;
	if(w->pos >= buf_len(w->b))
		return Weof;
	n = buf_rune(w->b, w->pos, &r);
	if(n <= 0)
		return Weof;
	if(r == '\n' && n == 1)
		return Wnl;
	if(r == '\r' && n == 1 && buf_byte(w->b, w->pos + 1) == '\n')
		return Wnl;
	if(w->pos - w->start >= Maxline){
		*show = 0x2026;
		*cells = 1;
		return Wtrunc;
	}
	if(r == '\t' && n == 1){
		tw = w->v->tabwidth > 0 ? w->v->tabwidth : 4;
		c = tw - w->col % tw;
		*cells = c;
		w->pos += n;
		w->col += c;
		return Wtab;
	}
	if((r < 0x20 || r == 0x7f) && n == 1){
		*show = '^';
		w->show2 = r == 0x7f ? '?' : r + 0x40;
		*cells = 2;
		w->pos += n;
		w->col += 2;
		return Wctrl;
	}
	if((r == Runeerr && n == 1) || (r >= 0x80 && r <= 0x9f)){
		*show = Runeerr;
		*cells = 1;
		w->pos += n;
		w->col += 1;
		return Winvalid;
	}
	c = 1;
	if(w->v->runecells != nil)
		c = w->v->runecells(w->v->rcarg, r);
	if(c < 1)
		c = 1;
	if(c > 2)
		c = 2;
	*show = r;
	*cells = c;
	w->pos += n;
	w->col += c;
	return Wtext;
}

int
view_col_of(View *v, Buf *b, Pos pos)
{
	Walk w;
	Rune r;
	int cells, k;
	Pos ls;

	ls = buf_line_start(b, buf_pos_to_line(b, pos));
	walk_init(&w, v, b, ls);
	while(w.pos < pos){
		k = walk_next(&w, &r, &cells);
		if(k == Wnl || k == Weof || k == Wtrunc)
			break;
	}
	return w.col;
}

Pos
view_pos_at_col(View *v, Buf *b, int line, int col)
{
	Walk w;
	Rune r;
	int cells, k;
	Pos p;

	walk_init(&w, v, b, buf_line_start(b, line));
	for(;;){
		p = w.pos;
		k = walk_next(&w, &r, &cells);
		if(k == Wnl || k == Weof || k == Wtrunc)
			return p;
		if(col < w.col)
			return p;
	}
}

int
view_pos_to_cell(View *v, Buf *b, Pos pos, int *row, int *col)
{
	int line, topline, c;

	line = buf_pos_to_line(b, pos);
	topline = buf_pos_to_line(b, v->top);
	if(line < topline || line - topline >= v->rows)
		return -1;
	c = view_col_of(v, b, pos) - v->hscroll;
	if(c < 0 || c >= v->cols)
		return -1;
	*row = line - topline;
	*col = c;
	return 0;
}

void
view_fix(View *v, Buf *b)
{
	if(v->top < 0)
		v->top = 0;
	if(v->top > buf_len(b))
		v->top = buf_len(b);
	v->top = buf_line_start(b, buf_pos_to_line(b, v->top));
}

/*
 * Fill n screen cells of the row whose line starts at ls, from column hscroll.
 * A wide rune cut by either edge shows a space in its visible half. Cells past the
 * text keep a space and the position of the line end. Returns the index of the
 * line-end cell (where a newline or the cursor at the end is drawn), or -1.
 */
int
view_layout(View *v, Buf *b, Pos ls, Vcell *out, int n)
{
	Walk w;
	Rune r;
	int k, cells, c0, c, i, j, slot, cut;
	Pos p;

	for(i = 0; i < n; i++){
		out[i].r = ' ';
		out[i].slot = Sdefault;
		out[i].pos = ls;
	}
	walk_init(&w, v, b, ls);
	for(;;){
		p = w.pos;
		c0 = w.col;
		if(c0 >= v->hscroll + n)
			return -1;
		k = walk_next(&w, &r, &cells);
		if(k == Wnl || k == Weof || k == Wtrunc){
			i = c0 - v->hscroll;
			if(k == Wtrunc && i >= 0 && i < n){
				out[i].r = 0x2026;
				out[i].slot = Sctrl;
				out[i].pos = p;
			}
			for(j = i < 0 ? 0 : i; j < n; j++)
				out[j].pos = p;
			if(k == Wtrunc || i < 0 || i >= n)
				return -1;
			return i;
		}
		slot = k == Wctrl ? Sctrl : k == Winvalid ? Sinvalid : Sdefault;
		cut = cells == 2 && k == Wtext && (c0 < v->hscroll || c0 + 2 > v->hscroll + n);
		for(j = 0; j < cells; j++){
			c = c0 + j - v->hscroll;
			if(c < 0 || c >= n)
				continue;
			out[c].slot = slot;
			out[c].pos = p;
			if(k == Wtab || cut)
				out[c].r = ' ';
			else if(k == Wctrl)
				out[c].r = j == 0 ? '^' : w.show2;
			else if(cells == 2 && j == 1)
				out[c].r = Ccont;
			else
				out[c].r = r;
		}
	}
}

Pos
view_cell_to_pos(View *v, Buf *b, int row, int col)
{
	Vcell *vc;
	int line, nl;
	Pos ls, p;

	view_fix(v, b);
	nl = buf_nlines(b);
	line = buf_pos_to_line(b, v->top) + row;
	if(line < 0)
		line = 0;
	if(line > nl - 1)
		line = nl - 1;
	ls = buf_line_start(b, line);
	if(v->cols <= 0)
		return ls;
	vc = malloc(v->cols * sizeof *vc);
	if(vc == nil)
		return ls;
	view_layout(v, b, ls, vc, v->cols);
	if(col < 0)
		col = 0;
	if(col > v->cols - 1)
		col = v->cols - 1;
	p = vc[col].pos;
	free(vc);
	return p;
}

static int
lastline(Buf *b)
{
	int n;

	n = buf_nlines(b);
	if(n > 1 && buf_len(b) > 0 && buf_byte(b, buf_len(b) - 1) == '\n')
		return n - 2;
	return n - 1;
}

void
view_scroll(View *v, Buf *b, int nlines)
{
	int line, to;

	view_fix(v, b);
	line = buf_pos_to_line(b, v->top);
	to = line + nlines;
	if(to > lastline(b))
		to = lastline(b);
	if(to < 0)
		to = 0;
	v->top = buf_line_start(b, to);
	v->scrolled += to - line;
}

void
view_reveal(View *v, Buf *b, Pos pos)
{
	int line, topline, old, d, col, col2, m;

	if(v->rows <= 0)
		return;
	view_fix(v, b);
	line = buf_pos_to_line(b, pos);
	topline = old = buf_pos_to_line(b, v->top);
	if(line < topline)
		topline = line;
	else if(line > topline + v->rows - 1)
		topline = line - v->rows + 1;
	if(topline != old){
		v->top = buf_line_start(b, topline);
		d = topline - old;
		if(d > -v->rows && d < v->rows)
			v->scrolled += d;
		else
			v->scrolled = 0;
	}
	if(v->cols <= 0)
		return;
	col = view_col_of(v, b, pos);
	col2 = col;
	if(pos < buf_len(b) && buf_byte(b, pos) != '\n' && buf_byte(b, pos) != '\r')
		col2 = view_col_of(v, b, buf_next(b, pos)) - 1;
	if(col2 < col)
		col2 = col;
	m = (v->cols - 1) / 2;
	if(m > 4)
		m = 4;
	if(col - v->hscroll < m)
		v->hscroll = col - m > 0 ? col - m : 0;
	else if(col2 - v->hscroll >= v->cols - m)
		v->hscroll = col2 - v->cols + m + 1;
}

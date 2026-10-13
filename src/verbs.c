#include "compat.h"
#include "coil.h"

/*
 * verbs.c: acting on the selection set. Every verb builds one Edit per selection
 * (ascending, pre-batch coordinates) and applies the batch with one doc_apply call,
 * so a verb is one transaction. The register is coil's own copy of the
 * yanked text; snarf.c mirrors it to /dev/snarf and adopts foreign snarf content.
 */

static void
msg(Ed *ed, char *s)
{
	lsnprint(ed->msg, sizeof ed->msg, "%s", s);
}

void
reg_free(Reg *r)
{
	int i;

	for(i = 0; i < r->n; i++)
		free(r->s[i]);
	free(r->s);
	free(r->len);
	memset(r, 0, sizeof *r);
}

/* make s a one-rune cursor at its head when it is empty and not at EOF */
void
vb_cursors(Ed *ed)
{
	Buf *b;
	Sel *s;
	int k;

	b = ed->doc.b;
	for(k = 0; k < ed->doc.sel.n; k++){
		s = &ed->doc.sel.s[k];
		if(s->anchor == s->head && s->head < buf_len(b)){
			s->anchor = s->head;
			s->head = buf_next(b, s->head);
		}
		s->goal = -1;
	}
	sel_normalize(&ed->doc.sel, b);
}

/* apply a batch; on failure leave the buffer alone and report it */
static int
run(Ed *ed, Edit *e, int n, SelSet *after)
{
	int r;

	after->primary = ed->doc.sel.primary;
	r = doc_apply(&ed->doc, e, n, after);
	if(r < 0)
		msg(ed, ed->doc.err);
	return r;
}

/* copy the selections into the register: 0 ok, 1 nothing selected (register untouched), -1 out of memory */
static int
yank_sel(Ed *ed)
{
	Reg nr;
	Buf *b;
	Sel *s;
	Pos len, total;
	int k, n;

	b = ed->doc.b;
	n = ed->doc.sel.n;
	total = 0;
	for(k = 0; k < n; k++)
		total += sel_max(&ed->doc.sel.s[k]) - sel_min(&ed->doc.sel.s[k]);
	if(total == 0)
		return 1;
	memset(&nr, 0, sizeof nr);
	nr.s = czalloc(n * sizeof(char*));
	nr.len = czalloc(n * sizeof(Pos));
	if(nr.s == nil || nr.len == nil){
		free(nr.s);
		free(nr.len);
		msg(ed, "out of memory");
		return -1;
	}
	nr.n = n;
	for(k = 0; k < n; k++){
		s = &ed->doc.sel.s[k];
		len = sel_max(s) - sel_min(s);
		nr.s[k] = malloc(len + 1);
		if(nr.s[k] == nil){
			reg_free(&nr);
			msg(ed, "out of memory");
			return -1;
		}
		buf_read(b, sel_min(s), nr.s[k], len);
		nr.s[k][len] = '\0';
		nr.len[k] = len;
	}
	nr.primary = ed->doc.sel.primary;
	nr.mark = ed->reg.mark;
	nr.markvalid = ed->reg.markvalid;
	nr.warned = ed->reg.warned;
	reg_free(&ed->reg);
	ed->reg = nr;
	snarf_publish(ed);
	return 0;
}

void
vb_yank(Ed *ed)
{
	yank_sel(ed);
	ed->mode = Mnormal;
}

/* edits removing every selection; after = carets (empty) where the text was */
static int
delete_sel(Ed *ed)
{
	Edit *e;
	SelSet after;
	Pos delta, p;
	Sel *s;
	int k, n, r;

	n = ed->doc.sel.n;
	e = malloc(n * sizeof(Edit));
	if(e == nil){
		msg(ed, "out of memory");
		return -1;
	}
	sel_init(&after);
	delta = 0;
	for(k = 0; k < n; k++){
		s = &ed->doc.sel.s[k];
		e[k].pos = sel_min(s);
		e[k].ndel = sel_max(s) - sel_min(s);
		e[k].ins = nil;
		e[k].nins = 0;
		p = e[k].pos + delta;
		sel_add(&after, p, p);
		delta -= e[k].ndel;
	}
	r = run(ed, e, n, &after);
	sel_free(&after);
	free(e);
	return r;
}

void
vb_delete(Ed *ed)
{
	if(yank_sel(ed) == 0 && delete_sel(ed) == 0)
		vb_cursors(ed);
	ed->mode = Mnormal;
}

void
vb_change(Ed *ed)
{
	int r;

	r = yank_sel(ed);
	if(r < 0){
		ed->mode = Mnormal;
		return;
	}
	if(r > 0){
		ed_to_insert(ed);	/* nothing to delete: just collapse to carets */
		return;
	}
	if(delete_sel(ed) < 0){
		ed->mode = Mnormal;
		return;
	}
	/* the transaction stays open: the insert session joins it, Esc seals */
	ed->mode = Minsert;
}

/* the register string for selection k of n */
static char*
regstr(Ed *ed, int k, int n, Pos *len)
{
	int i;

	i = ed->reg.n == n ? k : ed->reg.primary;
	if(i < 0 || i >= ed->reg.n)
		i = 0;
	*len = ed->reg.len[i];
	return ed->reg.s[i];
}

/* paste (replace == 0, at max or min) or replace each selection with its register string */
static void
put(Ed *ed, int after, int replace)
{
	Edit *e;
	SelSet set;
	Sel *s;
	Pos delta, p, total;
	int k, n;

	ed->mode = Mnormal;
	snarf_refresh(ed);
	if(ed->reg.n == 0){
		msg(ed, "register empty");
		return;
	}
	n = ed->doc.sel.n;
	e = malloc(n * sizeof(Edit));
	if(e == nil){
		msg(ed, "out of memory");
		return;
	}
	sel_init(&set);
	delta = 0;
	total = 0;
	for(k = 0; k < n; k++){
		s = &ed->doc.sel.s[k];
		e[k].ins = regstr(ed, k, n, &e[k].nins);
		if(replace){
			e[k].pos = sel_min(s);
			e[k].ndel = sel_max(s) - sel_min(s);
		}else{
			e[k].pos = after ? sel_max(s) : sel_min(s);
			e[k].ndel = 0;
		}
		total += e[k].nins + e[k].ndel;
		p = e[k].pos + delta;
		sel_add(&set, p, p + e[k].nins);
		delta += e[k].nins - e[k].ndel;
	}
	if(total > 0 && run(ed, e, n, &set) == 0)
		vb_cursors(ed);
	sel_free(&set);
	free(e);
}

void
vb_paste(Ed *ed, int after)
{
	put(ed, after, 0);
}

void
vb_replace(Ed *ed)
{
	put(ed, 0, 1);
}

static int
okrune(Rune r)
{
	if(r == '\t')
		return 1;
	if(r < 0x20 || r == Ck_del || r > 0x10FFFF)
		return 0;
	if(r >= 0xF000 && r <= 0xF8FF)
		return 0;
	return 1;
}

/* the replacement for selection s: line breaks stay, every other rune becomes r. *out is malloc'd; -1 on failure. */
static int
rchar_build(Ed *ed, Sel *s, Rune r, char **out, Pos *outn)
{
	Buf *b;
	Rune c;
	Pos p, hi, size;
	char *o, enc[4];
	int pass, n, el, keep;

	b = ed->doc.b;
	hi = sel_max(s);
	el = utf_enc(enc, r);
	o = nil;
	size = 0;
	for(pass = 0; pass < 2; pass++){
		size = 0;
		for(p = sel_min(s); p < hi; p += n){
			n = buf_rune(ed->doc.b, p, &c);
			if(n <= 0)
				break;
			keep = n == 1 && (c == '\n' || (c == '\r' && buf_byte(b, p + 1) == '\n'));
			if(size > Maxfile - 4)
				goto bad;
			if(pass == 1){
				if(keep)
					o[size] = (char)c;
				else
					memmove(o + size, enc, el);
			}
			size += keep ? 1 : el;
		}
		if(pass == 0){
			o = malloc(size + 1);
			if(o == nil)
				return -1;
		}
	}
	*out = o;
	*outn = size;
	return 0;
bad:
	free(o);
	return -1;
}

void
vb_rchar(Ed *ed, Rune r)
{
	Edit *e;
	SelSet set;
	char **own;
	Sel *s;
	Pos delta, p, total;
	int k, n, ok;

	ed->mode = Mnormal;
	if(!okrune(r))
		return;
	n = ed->doc.sel.n;
	e = malloc(n * sizeof(Edit));
	own = czalloc(n * sizeof(char*));
	if(e == nil || own == nil){
		free(e);
		free(own);
		msg(ed, "out of memory");
		return;
	}
	sel_init(&set);
	ok = 1;
	delta = 0;
	total = 0;
	for(k = 0; k < n && ok; k++){
		s = &ed->doc.sel.s[k];
		if(rchar_build(ed, s, r, &own[k], &e[k].nins) < 0){
			msg(ed, "out of memory");
			ok = 0;
			break;
		}
		e[k].pos = sel_min(s);
		e[k].ndel = sel_max(s) - sel_min(s);
		e[k].ins = own[k];
		total += e[k].ndel;
		p = e[k].pos + delta;
		sel_add(&set, p, p + e[k].nins);
		delta += e[k].nins - e[k].ndel;
	}
	if(ok && total > 0 && run(ed, e, n, &set) == 0)
		vb_cursors(ed);
	for(k = 0; k < n; k++)
		free(own[k]);
	free(own);
	sel_free(&set);
	free(e);
}

/*
 * Insert commands, typing and erase keys.
 */

static Pos
line_start(Buf *b, Pos p)
{
	return buf_line_start(b, buf_pos_to_line(b, p));
}

/* line end before "\n" or "\r\n", for the line starting at ls */
static Pos
line_end(Buf *b, Pos ls)
{
	Pos q, len;

	len = buf_len(b);
	for(q = ls; q < len && buf_byte(b, q) != '\n'; q++)
		;
	if(q < len && q > ls && buf_byte(b, q - 1) == '\r')
		q--;
	return q;
}

/* end of the run of spaces and tabs that starts at ls, not beyond limit */
static Pos
indent_end(Buf *b, Pos ls, Pos limit)
{
	Pos q;
	int c;

	for(q = ls; q < limit; q++){
		c = buf_byte(b, q);
		if(c != ' ' && c != '\t')
			break;
	}
	return q;
}

/* the position the last-line commands (o, A) work from: the last selected rune */
static Pos
last_pos(Buf *b, Sel *s)
{
	return sel_max(s) > sel_min(s) ? buf_prev(b, sel_max(s)) : sel_min(s);
}

/*
 * Apply one edit per entry of e (ne of them), then leave one empty caret per selection:
 * caret of edit j is pos + co[j] + the size change of the edits before it.
 * idx maps selection k to its edit (nil: identity, nsel == ne).
 */
static int
apply_carets(Ed *ed, Edit *e, int *co, int ne, int *idx, int nsel)
{
	SelSet after;
	Pos *cp, delta;
	int k, r;

	cp = malloc(ne * sizeof(Pos));
	if(cp == nil){
		msg(ed, "out of memory");
		return -1;
	}
	delta = 0;
	for(k = 0; k < ne; k++){
		cp[k] = e[k].pos + co[k] + delta;
		delta += e[k].nins - e[k].ndel;
	}
	sel_init(&after);
	for(k = 0; k < nsel; k++)
		sel_add(&after, cp[idx != nil ? idx[k] : k], cp[idx != nil ? idx[k] : k]);
	free(cp);
	r = run(ed, e, ne, &after);
	sel_free(&after);
	return r;
}

/* Insert mode with one caret per selection at the given positions; the session starts a new transaction */
static void
carets_at(Ed *ed, Pos *pos)
{
	Sel *s;
	int k;

	doc_seal(&ed->doc);
	doc_set_pre(&ed->doc, &ed->doc.sel);	/* undo of the session restores this selection */
	for(k = 0; k < ed->doc.sel.n; k++){
		s = &ed->doc.sel.s[k];
		s->anchor = s->head = pos[k];
		s->goal = -1;
	}
	sel_normalize(&ed->doc.sel, ed->doc.b);
	ed->mode = Minsert;
}

/* o and O: open a line below or above each selection (once per line) */
static void
open_lines(Ed *ed, int below)
{
	Edit *e;
	char **own;
	int *co, *idx;
	Buf *b;
	Sel *s;
	Pos p, ls, le, ie;
	int k, n, ne, ilen, elen, ok;
	char *t;

	b = ed->doc.b;
	n = ed->doc.sel.n;
	e = malloc(n * sizeof(Edit));
	own = czalloc(n * sizeof(char*));
	co = malloc(n * sizeof(int));
	idx = malloc(n * sizeof(int));
	if(e == nil || own == nil || co == nil || idx == nil){
		msg(ed, "out of memory");
		goto done;
	}
	elen = ed->crlf ? 2 : 1;
	ne = 0;
	ok = 1;
	for(k = 0; k < n && ok; k++){
		s = &ed->doc.sel.s[k];
		p = below ? last_pos(b, s) : sel_min(s);
		ls = line_start(b, p);
		le = line_end(b, ls);
		if(ne > 0 && e[ne - 1].pos == (below ? le : ls)){
			idx[k] = ne - 1;	/* two selections on one line open it once */
			continue;
		}
		ie = indent_end(b, ls, le);
		ilen = ie - ls;
		t = malloc(ilen + elen);
		if(t == nil){
			msg(ed, "out of memory");
			ok = 0;
			break;
		}
		if(below){
			if(ed->crlf)
				memmove(t, "\r\n", 2);
			else
				t[0] = '\n';
			buf_read(b, ls, t + elen, ilen);
			e[ne].pos = le;
			co[ne] = ilen + elen;
		}else{
			buf_read(b, ls, t, ilen);
			if(ed->crlf)
				memmove(t + ilen, "\r\n", 2);
			else
				t[ilen] = '\n';
			e[ne].pos = ls;
			co[ne] = ilen;
		}
		own[ne] = t;
		e[ne].ndel = 0;
		e[ne].ins = t;
		e[ne].nins = ilen + elen;
		idx[k] = ne;
		ne++;
	}
	if(ok){
		doc_seal(&ed->doc);
		doc_set_pre(&ed->doc, &ed->doc.sel);
		if(apply_carets(ed, e, co, ne, idx, n) == 0)
			ed->mode = Minsert;
		else
			doc_seal(&ed->doc);
	}
done:
	if(own != nil)
		for(k = 0; k < n; k++)
			free(own[k]);
	free(own);
	free(e);
	free(co);
	free(idx);
}

void
vb_insert(Ed *ed, int where)
{
	Pos *pos;
	Buf *b;
	Sel *s;
	int k, n;

	b = ed->doc.b;
	switch(where){
	case 'i':
		ed_to_insert(ed);
		return;
	case 'o':
		open_lines(ed, 1);
		return;
	case 'O':
		open_lines(ed, 0);
		return;
	}
	n = ed->doc.sel.n;
	pos = malloc(n * sizeof(Pos));
	if(pos == nil){
		msg(ed, "out of memory");
		return;
	}
	for(k = 0; k < n; k++){
		s = &ed->doc.sel.s[k];
		switch(where){
		case 'a':
			pos[k] = sel_max(s);
			break;
		case 'I':
			pos[k] = indent_end(b, line_start(b, sel_min(s)), buf_len(b));
			break;
		default:	/* 'A' */
			pos[k] = line_end(b, line_start(b, last_pos(b, s)));
			break;
		}
	}
	carets_at(ed, pos);
	free(pos);
}

void
vb_type(Ed *ed, Rune r)
{
	Edit *e;
	char **own;
	int *co;
	Buf *b;
	Pos head, ls, ie;
	int k, n, len, ilen, tw, col, ok;
	char *t;

	b = ed->doc.b;
	n = ed->doc.sel.n;
	e = malloc(n * sizeof(Edit));
	own = czalloc(n * sizeof(char*));
	co = malloc(n * sizeof(int));
	if(e == nil || own == nil || co == nil){
		msg(ed, "out of memory");
		goto done;
	}
	ok = 1;
	for(k = 0; k < n && ok; k++){
		head = ed->doc.sel.s[k].head;
		if(r == Ck_nl || r == Ck_cr){
			/* line ending plus the leading whitespace of the line up to the caret */
			ls = line_start(b, head);
			ie = indent_end(b, ls, head);
			ilen = ie - ls;
			len = (ed->crlf ? 2 : 1) + ilen;
			t = malloc(len);
			if(t != nil){
				if(ed->crlf)
					memmove(t, "\r\n", 2);
				else
					t[0] = '\n';
				buf_read(b, ls, t + len - ilen, ilen);
			}
		}else if(r == Ck_tab){
			tw = ed->view.tabwidth > 0 ? ed->view.tabwidth : 4;
			if(ed->lang->usetabs){
				len = 1;
				t = malloc(len);
				if(t != nil)
					t[0] = '\t';
			}else{
				col = view_col_of(&ed->view, b, head);
				len = tw - col % tw;
				t = malloc(len);
				if(t != nil)
					memset(t, ' ', len);
			}
		}else{
			len = 4;
			t = malloc(len);
			if(t != nil)
				len = utf_enc(t, r);
		}
		if(t == nil){
			msg(ed, "out of memory");
			ok = 0;
			break;
		}
		own[k] = t;
		e[k].pos = head;
		e[k].ndel = 0;
		e[k].ins = t;
		e[k].nins = len;
		co[k] = len;
	}
	if(ok)
		apply_carets(ed, e, co, n, nil, n);
done:
	if(own != nil)
		for(k = 0; k < n; k++)
			free(own[k]);
	free(own);
	free(e);
	free(co);
}

/* start of what Backspace removes before c: one rune, or a whole line ending; never before prevend */
static Pos
back_one(Buf *b, Pos c, Pos prevend)
{
	Pos q;

	q = c > 0 ? buf_prev(b, c) : c;
	if(q > 0 && q - 1 >= prevend && buf_byte(b, q) == '\n' && buf_byte(b, q - 1) == '\r')
		q--;
	if(q < prevend)
		q = c;
	return q;
}

static int
class_before(Buf *b, Pos p)
{
	Rune r;

	buf_rune(b, buf_prev(b, p), &r);
	return rune_class(r);
}

void
vb_erase(Ed *ed, int what)
{
	Edit *e;
	int *co;
	Buf *b;
	Pos c, q, ls, fnb, prevend, floor;
	int k, n, any, cl;

	b = ed->doc.b;
	n = ed->doc.sel.n;
	e = malloc(n * sizeof(Edit));
	co = czalloc(n * sizeof(int));
	if(e == nil || co == nil){
		msg(ed, "out of memory");
		free(e);
		free(co);
		return;
	}
	any = 0;
	prevend = 0;
	for(k = 0; k < n; k++){
		c = ed->doc.sel.s[k].head;
		ls = line_start(b, c);
		floor = ls > prevend ? ls : prevend;	/* word and line erase stay on the line and clear of the previous caret */
		q = c;
		if(what == 'h' || c <= floor){
			q = back_one(b, c, prevend);
		}else if(what == 'w'){
			while(q > floor && class_before(b, q) == Cspace)
				q = buf_prev(b, q);
			if(q > floor){
				cl = class_before(b, q);
				while(q > floor && class_before(b, q) == cl)
					q = buf_prev(b, q);
			}
		}else{	/* 'u' */
			fnb = indent_end(b, ls, buf_len(b));
			q = c > fnb ? fnb : ls;
			if(q < floor)
				q = floor;
		}
		e[k].pos = q;
		e[k].ndel = c - q;
		e[k].ins = nil;
		e[k].nins = 0;
		if(c > q)
			any = 1;
		prevend = c;
	}
	if(any)
		apply_carets(ed, e, co, n, nil, n);
	free(e);
	free(co);
}

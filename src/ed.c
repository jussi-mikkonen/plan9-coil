#include "compat.h"
#include "coil.h"
#include "host.h"

/* ed.c: editor state and the key-driven skeleton. All mutation goes through doc_apply. */

enum { Maxline9 = 1000000000 };

int
ed_init(Ed *ed, Host *h, int (*runecells)(void*, Rune), void *rcarg)
{
	memset(ed, 0, sizeof *ed);
	if(doc_init(&ed->doc) < 0)
		return -1;
	view_init(&ed->view);
	ed->view.runecells = runecells;
	ed->view.rcarg = rcarg;
	ed->lang = lang_detect(nil, nil);
	hl_init(&ed->hl);
	hl_reset(&ed->hl, ed->lang);
	ed->host = h;
	ed->mode = Mnormal;
	ed->slice = Slice;
	ed->gutter = 1;
	return 0;
}

void
ed_free(Ed *ed)
{
	hl_free(&ed->hl);
	free(ed->path);
	ed->path = nil;
	reg_free(&ed->reg);
	prompt_free(&ed->prompt);
	if(ed->srch.havepat)
		pat_free(&ed->srch.pat);
	ed->srch.havepat = 0;
	scan_free(&ed->srch.sc);
	exec_free(ed);
	doc_free(&ed->doc);
}

static void
setmsg(Ed *ed, char *fmt, char *a, char *b)
{
	if(b == nil)
		lsnprint(ed->msg, sizeof ed->msg, fmt, a);
	else
		lsnprint(ed->msg, sizeof ed->msg, fmt, a, b);
}

Pos
ed_cursor(Ed *ed, Sel *s)
{
	if(s->anchor < s->head)
		return buf_cprev(ed->doc.b, s->head);
	return s->head;
}

int
ed_lastline(Ed *ed)
{
	Buf *b;
	int n;

	b = ed->doc.b;
	n = buf_nlines(b);
	if(n > 1 && buf_len(b) > 0 && buf_byte(b, buf_len(b) - 1) == '\n')
		return n - 2;
	return n - 1;
}

/* make s a one-rune cursor at t (empty at EOF) */
static void
setcursor(Ed *ed, Sel *s, Pos t)
{
	Buf *b;

	b = ed->doc.b;
	s->anchor = t;
	s->head = t < buf_len(b) ? buf_cnext(b, t) : t;
}

/* gutter width: digits of the last line number plus one space; 0 when off or the window is too narrow */
int
ed_gutter(Ed *ed)
{
	int n, w;

	if(!ed->gutter)
		return 0;
	n = ed_lastline(ed) + 1;
	w = 1;
	while(n >= 10){
		n /= 10;
		w++;
	}
	w++;
	if(ed->wincols < w + 2)
		return 0;
	return w;
}

void
ed_layout(Ed *ed)
{
	int c;

	c = ed->wincols - ed_gutter(ed);
	if(c < 1 && ed->wincols >= 1)
		c = 1;
	ed->view.cols = c > 0 ? c : 0;
}

static void
reveal(Ed *ed)
{
	ed_layout(ed);
	view_reveal(&ed->view, ed->doc.b, ed_cursor(ed, sel_primary(&ed->doc.sel)));
}

void
ed_reveal(Ed *ed)
{
	reveal(ed);
}

void
ed_status(Ed *ed, char *out, int n, int *nmsg)
{
	static char *modes[] = {"NOR", "INS", "SEL"};
	Pos cur;
	int len;

	cur = ed_cursor(ed, sel_primary(&ed->doc.sel));
	lsnprint(out, n, "%s %s%s%s %d:%d%s",
		modes[ed->mode >= 0 && ed->mode <= 2 ? ed->mode : 0],
		ed->path != nil ? ed->path : "[no name]",
		ed->isnew ? " [new]" : "",
		doc_modified(&ed->doc) ? " [+]" : "",
		buf_pos_to_line(ed->doc.b, cur) + 1,
		view_col_of(&ed->view, ed->doc.b, cur) + 1,
		ed->crlf ? " crlf" : "");
	*nmsg = -1;
	if(ed->msg[0] != '\0'){
		len = strlen(out);
		if(len + 2 < n){
			lsnprint(out + len, n - len, "  %s", ed->msg);
			*nmsg = len + 2;
		}
	}
}

/* file:N rule: split at the last ':' if the suffix is non-empty digits and the whole argument is not a file */
int
ed_target(Host *h, char *arg, char **path, int *line)
{
	char *colon, *p;
	int n, len, digits;

	*line = 1;
	len = strlen(arg);
	colon = nil;
	for(p = arg; *p != '\0'; p++)
		if(*p == ':')
			colon = p;
	digits = 0;
	if(colon != nil && colon[1] != '\0'){
		digits = 1;
		for(p = colon + 1; *p != '\0'; p++)
			if(*p < '0' || *p > '9')
				digits = 0;
	}
	if(digits && !h->exists(h, arg)){
		n = 0;
		for(p = colon + 1; *p != '\0'; p++){
			if(n > Maxline9 / 10)
				n = Maxline9;	/* saturate: no overflow */
			else
				n = n * 10 + (*p - '0');
			if(n > Maxline9)
				n = Maxline9;
		}
		*line = n < 1 ? 1 : n;
		len = colon - arg;
	}
	*path = malloc(len + 1);
	if(*path == nil)
		return -1;
	memmove(*path, arg, len);
	(*path)[len] = '\0';
	return 0;
}

int
ed_load(Ed *ed, char *arg, int line)
{
	char *path, *data, err[100];
	int isnew, last;
	Pos n, pos, ls1;
	Buf *b;
	Doc nd;
	Stamp st;

	path = malloc(strlen(arg) + 1);
	if(path == nil){
		setmsg(ed, "%s: out of memory", arg, nil);
		return -1;
	}
	memmove(path, arg, strlen(arg) + 1);
	data = nil;
	n = 0;
	isnew = 0;
	if(ed->host->readfile(ed->host, path, &data, &n, &isnew, &st, err, sizeof err) < 0){
		setmsg(ed, "%s: %s", path, err);
		free(path);
		return -1;
	}
	/* load into a fresh Doc and swap it in only on success: a failure leaves the document as it was */
	if(doc_init(&nd) < 0){
		free(data);
		setmsg(ed, "%s: out of memory", path, nil);
		free(path);
		return -1;
	}
	nd.b->max = ed->doc.b->max;
	if(doc_load(&nd, data, n, err, sizeof err) < 0){
		setmsg(ed, "%s: %s", path, err);
		doc_free(&nd);
		free(path);
		return -1;
	}
	doc_free(&ed->doc);
	ed->doc = nd;
	b = ed->doc.b;
	free(ed->path);
	ed->path = path;
	ed->stamp = st;
	ed->isnew = isnew;
	ed->lang = lang_detect(path, b);
	ed->view.tabwidth = ed->lang->tabwidth;
	hl_reset(&ed->hl, ed->lang);
	ed->crlf = 0;
	if(buf_nlines(b) > 1){
		ls1 = buf_line_start(b, 1);
		ed->crlf = ls1 >= 2 && buf_byte(b, ls1 - 1) == '\n' && buf_byte(b, ls1 - 2) == '\r';
	}
	ed->mode = Mnormal;
	ed->quit = 0;
	ed->qarmed = 0;
	ed->count = 0;
	ed->npend = 0;
	ed->msg[0] = '\0';
	if(isnew)
		setmsg(ed, "new file", nil, nil);
	last = ed_lastline(ed);
	if(line - 1 > last)
		line = last + 1;
	if(line < 1)
		line = 1;
	pos = buf_line_start(b, line - 1);
	sel_set1(&ed->doc.sel, pos, pos);
	setcursor(ed, &ed->doc.sel.s[0], pos);
	ed->view.top = 0;
	ed->view.hscroll = 0;
	reveal(ed);
	return 0;
}

int
ed_open(Ed *ed, char *arg)
{
	char *path;
	int line, rc;

	if(ed_target(ed->host, arg, &path, &line) < 0){
		setmsg(ed, "%s: out of memory", arg, nil);
		return -1;
	}
	rc = ed_load(ed, path, line);
	free(path);
	return rc;
}

/*
 * write the document (path nil or its own name) or a copy under another name.
 * An unnamed buffer writes PATH under the copy rules and takes the name only
 * when the write succeeds.
 */
int
ed_write(Ed *ed, char *path, int force)
{
	char err[100];
	char *name;
	int own, rc;
	Stamp out;

	if(path == nil && ed->path == nil){
		setmsg(ed, "no file name", nil, nil);
		return -1;
	}
	name = nil;
	own = path == nil || (ed->path != nil && strcmp(path, ed->path) == 0);
	if(own){
		path = ed->path;
	}else if(!force && ed->host->exists(ed->host, path)){
		setmsg(ed, "%s exists; :w! %s overwrites", path, path);
		return -1;
	}
	if(ed->path == nil){
		/* allocated before the write, so a success never leaves the name untaken */
		name = malloc(strlen(path) + 1);
		if(name == nil){
			setmsg(ed, "%s: out of memory", path, nil);
			return -1;
		}
		memmove(name, path, strlen(path) + 1);
	}
	rc = ed->host->writefile(ed->host, path, ed->doc.b, own && !force ? &ed->stamp : nil, &out, err, sizeof err);
	if(rc == Wchanged){
		free(name);
		doc_seal(&ed->doc);
		setmsg(ed, "%s changed on disk; :w! overwrites", path, nil);
		return -1;
	}
	if(rc < 0){
		free(name);
		doc_seal(&ed->doc);
		setmsg(ed, "%s: %s", path, err);
		return -1;
	}
	if(name != nil){
		ed->path = name;
		ed->lang = lang_detect(ed->path, ed->doc.b);
		ed->view.tabwidth = ed->lang->tabwidth;
		hl_reset(&ed->hl, ed->lang);
		own = 1;
	}
	if(own){
		ed->stamp = out;
		doc_mark_saved(&ed->doc);
		ed->isnew = 0;
	}
	lsnprint(ed->msg, sizeof ed->msg, "wrote %s %d", path, buf_len(ed->doc.b));
	return 0;
}

int
ed_save(Ed *ed)
{
	return ed_write(ed, nil, 0);
}

void
ed_resize(Ed *ed, int rows, int cols)
{
	ed->view.rows = rows > 1 ? rows - 1 : 0;
	ed->wincols = cols > 0 ? cols : 0;
	ed_layout(ed);
	ed->view.scrolled = 0;
	reveal(ed);
}

void
ed_to_insert(Ed *ed)
{
	int k;
	Sel *s;

	doc_seal(&ed->doc);
	doc_set_pre(&ed->doc, &ed->doc.sel);	/* undo of the session restores this selection */
	for(k = 0; k < ed->doc.sel.n; k++){
		s = &ed->doc.sel.s[k];
		s->anchor = s->head = sel_min(s);
		s->goal = -1;
	}
	sel_normalize(&ed->doc.sel, ed->doc.b);
	ed->mode = Minsert;
}

void
ed_to_normal(Ed *ed)
{
	int k;
	Sel *s;

	doc_seal(&ed->doc);
	if(ed->mode == Minsert){
		for(k = 0; k < ed->doc.sel.n; k++){
			s = &ed->doc.sel.s[k];
			setcursor(ed, s, s->head);
		}
		sel_normalize(&ed->doc.sel, ed->doc.b);
	}
	ed->mode = Mnormal;
}

/* caret movement in insert mode: seals the session, then moves every caret */
void
ed_imove(Ed *ed, int kind)
{
	Buf *b;
	Sel *s;
	Pos p, t;
	int k, mode;

	b = ed->doc.b;
	doc_seal(&ed->doc);
	if(kind == Mv_left || kind == Mv_right){
		for(k = 0; k < ed->doc.sel.n; k++){
			s = &ed->doc.sel.s[k];
			p = s->head;
			if(kind == Mv_left)
				p = p > 0 ? buf_cprev(b, p) : p;
			else
				p = p < buf_len(b) ? buf_cnext(b, p) : p;
			s->anchor = s->head = p;
			s->goal = -1;
		}
		sel_normalize(&ed->doc.sel, b);
		return;
	}
	/* other motions: borrow the normal-mode motion on one-rune cursors, then collapse to a caret */
	for(k = 0; k < ed->doc.sel.n; k++)
		setcursor(ed, &ed->doc.sel.s[k], ed->doc.sel.s[k].head);
	mode = ed->mode;
	ed->mode = Mnormal;
	mv_motion(ed, kind, 1);
	ed->mode = mode;
	for(k = 0; k < ed->doc.sel.n; k++){
		s = &ed->doc.sel.s[k];
		t = sel_min(s);
		p = t;
		if(kind == Mv_lineend && t < buf_len(b) && buf_byte(b, t) != '\n' && buf_byte(b, t) != '\r')
			p = sel_max(s);	/* after the last rune of the line */
		s->anchor = s->head = p;
	}
	sel_normalize(&ed->doc.sel, b);
}

/* u / U: step the history count times; selections come back as one-rune cursors in normal mode */
void
ed_undo(Ed *ed, int redo, int count)
{
	Sel *s;
	int i, k, ok;

	ok = 0;
	for(i = 0; i < count; i++){
		if((redo ? doc_redo(&ed->doc) : doc_undo(&ed->doc)) < 0){
			setmsg(ed, "%s", ed->doc.err, nil);
			break;
		}
		ok++;
	}
	if(ok == 0)
		return;
	for(k = 0; k < ed->doc.sel.n; k++){
		s = &ed->doc.sel.s[k];
		if(s->anchor == s->head && s->head < buf_len(ed->doc.b))
			setcursor(ed, s, s->head);
		s->goal = -1;
	}
	sel_normalize(&ed->doc.sel, ed->doc.b);
	ed->mode = Mnormal;
}

void
ed_move(Ed *ed, int dir, int count)
{
	switch(dir){
	case 'h':
		mv_motion(ed, Mv_left, count);
		break;
	case 'j':
		mv_motion(ed, Mv_down, count);
		break;
	case 'k':
		mv_motion(ed, Mv_up, count);
		break;
	case 'l':
		mv_motion(ed, Mv_right, count);
		break;
	}
}

static int
printable(Rune r)
{
	if(r < 0x20 || r == Ck_del || r > 0x10FFFF)
		return 0;
	if(r >= 0xF000 && r <= 0xF8FF)
		return 0;
	return 1;
}

void
ed_key(Ed *ed, Rune r)
{
	if(ed->prompt.on){
		prompt_key(ed, r);
		reveal(ed);
		return;
	}
	if(ed->job.on){
		if(r == Ck_esc || r == Ck_del)
			job_cancel(ed);
		else
			setmsg(ed, "busy: Esc interrupts", nil, nil);
		return;
	}
	if(r == CTL('q')){
		ed->npend = 0;
		ed->count = 0;
		if(!doc_modified(&ed->doc) || ed->qarmed)
			ed->quit = 1;
		else{
			ed->qarmed = 1;
			setmsg(ed, "unsaved changes; Ctrl-Q again quits", nil, nil);
		}
		return;
	}
	ed->qarmed = 0;
	ed->msg[0] = '\0';
	if(ed->mode != Minsert)
		doc_seal(&ed->doc);
	if(!keymap_key(ed, r) && ed->mode == Minsert && printable(r))
		vb_type(ed, r);
	if(ed->mode != Minsert)
		doc_seal(&ed->doc);
	reveal(ed);
}

/* source position of the cell a click or drag points at; outside insert mode the line end is the last rune, as for $ */
static Pos
pointpos(Ed *ed, Ev *ev)
{
	Buf *b;
	Pos p, ls;
	int row, col;

	ed_layout(ed);
	b = ed->doc.b;
	row = ev->row;
	if(row >= ed->view.rows)
		row = ed->view.rows - 1;
	col = ev->col - ed_gutter(ed);
	if(col < 0)
		col = 0;
	p = view_cell_to_pos(&ed->view, b, row, col);
	if(ed->mode == Minsert)
		return p;
	if(p >= buf_len(b))
		p = buf_len(b) > 0 ? buf_prev(b, buf_len(b)) : 0;
	ls = buf_line_start(b, buf_pos_to_line(b, p));
	if(p > ls && (buf_byte(b, p) == '\n' || (buf_byte(b, p) == '\r' && buf_byte(b, p + 1) == '\n')))
		p = buf_prev(b, p);
	return p;
}

static void
mouse_click(Ed *ed, Ev *ev)
{
	Buf *b;
	Sel *s;
	Pos pos, a, h;

	if(ev->row < 0 || ev->row >= ed->view.rows)
		return;
	b = ed->doc.b;
	doc_seal(&ed->doc);
	ed->npend = 0;
	ed->count = 0;
	ed->qarmed = 0;
	ed->msg[0] = '\0';
	pos = pointpos(ed, ev);
	switch(ed->mode){
	case Minsert:
		sel_set1(&ed->doc.sel, pos, pos);
		break;
	case Mselect:
		s = sel_primary(&ed->doc.sel);
		a = s->anchor;
		h = pos >= a ? (pos < buf_len(b) ? buf_next(b, pos) : pos) : pos;
		sel_set1(&ed->doc.sel, a, h);
		break;
	default:
		sel_set1(&ed->doc.sel, pos, pos < buf_len(b) ? buf_next(b, pos) : pos);
		break;
	}
	ed->doc.sel.s[0].goal = -1;
	sel_normalize(&ed->doc.sel, b);
	ed->dragging = 1;
}

static void
mouse_drag(Ed *ed, Ev *ev)
{
	Buf *b;
	Sel *s;
	Pos pos, o, a, h;

	if(!ed->dragging)
		return;
	b = ed->doc.b;
	pos = pointpos(ed, ev);
	s = sel_primary(&ed->doc.sel);
	if(ed->mode == Minsert){
		sel_set1(&ed->doc.sel, pos, pos);
	}else{
		/* the dragged-from rune stays inside the range in both directions */
		o = s->anchor > s->head ? buf_prev(b, s->anchor) : s->anchor;
		if(pos >= o){
			a = o;
			h = pos < buf_len(b) ? buf_next(b, pos) : pos;
		}else{
			a = buf_next(b, o);
			h = pos;
		}
		sel_set1(&ed->doc.sel, a, h);
	}
	ed->doc.sel.s[0].goal = -1;
	sel_normalize(&ed->doc.sel, b);
}

static void
mouse_double(Ed *ed, Ev *ev)
{
	Pos pos;
	Buf *b;

	if(ev->row < 0 || ev->row >= ed->view.rows)
		return;
	b = ed->doc.b;
	if(ed->mode == Minsert)
		ed_to_normal(ed);
	doc_seal(&ed->doc);
	pos = pointpos(ed, ev);
	sel_set1(&ed->doc.sel, pos, pos < buf_len(b) ? buf_next(b, pos) : pos);
	ed->doc.sel.s[0].goal = -1;
	sel_normalize(&ed->doc.sel, b);
	obj_select(ed, 1, 'w');
	ed->dragging = 0;
}

void
ed_event(Ed *ed, Ev *ev)
{
	switch(ev->kind){
	case Ekey:
		ed_key(ed, ev->r);
		break;
	case Eclick:
		if(ed->job.on)
			break;
		mouse_click(ed, ev);
		break;
	case Edrag:
		if(ed->job.on)
			break;
		mouse_drag(ed, ev);
		break;
	case Edouble:
		if(ed->job.on)
			break;
		mouse_double(ed, ev);
		break;
	case Erelease:
		if(ed->job.on)
			break;
		ed->dragging = 0;
		break;
	case Ewheel:
		view_scroll(&ed->view, ed->doc.b, ev->n * 3);
		break;
	}
}

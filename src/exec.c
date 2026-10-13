#include "compat.h"
#include "coil.h"

/*
 * exec.c: structural commands (x y g v with d c i a) as an interruptible job.
 *
 * The chain is a flat array of steps; frame i runs step i over its dot [lo, hi) and the
 * frames form an explicit stack (no recursion). Starting dots are the selections, or the
 * whole buffer. Every nested step sees the unmodified buffer; terminals only collect Edits
 * (ascending, pre-batch coordinates) and the result selections. exec_finish applies them
 * with one doc_apply: one undo step, and cancel before that touches nothing.
 */

int exec_maxedits = Maxedits;

static void
msg(Ed *ed, char *s)
{
	lsnprint(ed->msg, sizeof ed->msg, "%s", s);
}

void
exec_free(Ed *ed)
{
	Exec *ex;
	Ablock *ab;
	int i;

	ex = ed->ex;
	if(ex == nil)
		return;
	for(i = 0; i < ex->nf; i++)
		if(ex->f[i].hasscan)
			scan_free(&ex->f[i].sc);
	for(i = 0; i < Maxdepth; i++)
		pat_free(&ex->pat[i]);
	cmd_free(&ex->cmd);
	sel_free(&ex->ranges);
	sel_free(&ex->after);
	pat_free(&ex->spat);
	while(ex->ab != nil){
		ab = ex->ab;
		ex->ab = ab->next;
		free(ab->d);
		free(ab);
	}
	free(ex->e);
	free(ex);
	ed->ex = nil;
}

static void
exec_cancel(Ed *ed)
{
	exec_free(ed);
}

static int
fail(Ed *ed, Exec *ex, char *m)
{
	msg(ed, m);
	ex->failed = 1;
	return -1;
}

/* collect one edit (ascending, pre-batch coordinates) and its result selection. 0, or -1 with ex->failed */
static int
emit(Ed *ed, Exec *ex, Pos pos, Pos ndel, char *ins, Pos nins)
{
	Edit *ne;
	Buf *b;
	Pos len;
	int c;

	if(pos < ex->lastend)
		return fail(ed, ex, "overlapping changes");
	if(ex->ne >= exec_maxedits){
		lsnprint(ed->msg, sizeof ed->msg, "too many changes (more than %d); narrow the range", exec_maxedits);
		ex->failed = 1;
		return -1;
	}
	b = ed->doc.b;
	len = buf_len(b);
	if(nins > ndel && ex->delta + nins - ndel > b->max - len)
		return fail(ed, ex, "buffer full");
	if(ex->ne == ex->ecap){
		c = ex->ecap == 0 ? 64 : ex->ecap * 2;
		ne = realloc(ex->e, c * sizeof(Edit));
		if(ne == nil)
			return fail(ed, ex, "out of memory");
		ex->e = ne;
		ex->ecap = c;
	}
	if(sel_add(&ex->after, pos + ex->delta, pos + ex->delta + nins) < 0)
		return fail(ed, ex, "out of memory");
	ex->e[ex->ne].pos = pos;
	ex->e[ex->ne].ndel = ndel;
	ex->e[ex->ne].ins = nins > 0 ? ins : nil;
	ex->e[ex->ne].nins = nins;
	ex->ne++;
	ex->delta += nins - ndel;
	ex->lastend = pos + ndel;
	return 0;
}

/* the chain is exhausted for the dot [lo, hi): emit its edit or record it. 0, or -1 with ex->failed */
static int
terminal(Ed *ed, Exec *ex, Pos lo, Pos hi)
{
	Pos pos, ndel, nins;
	int c;

	ex->nmatch++;
	c = ex->cmd.term;
	if(c == 0){
		if(ex->rec && sel_add(&ex->after, lo, hi) < 0)
			return fail(ed, ex, "out of memory");
		return 0;
	}
	ndel = 0;
	nins = 0;
	pos = lo;
	if(c == 'd' || c == 'c')
		ndel = hi - lo;
	if(c == 'c' || c == 'i' || c == 'a')
		nins = ex->cmd.ntext;
	if(c == 'a')
		pos = hi;
	if(ndel == 0 && nins == 0)
		return 0;
	return emit(ed, ex, pos, ndel, ex->cmd.text, nins);
}

/*
 * The replacement of s for match m: "&" and "\0".."\9" are groups (unset: empty),
 * "\n" newline, "\t" tab, "\&" "&", "\\" backslash, any other "\x" is x.
 * Writes to out unless it is nil; returns the length.
 */
static Pos
expand(Buf *b, char *t, Pos nt, Match *m, char *out)
{
	Pos i, k, g, n, c;
	int ch, grp;

	k = 0;
	for(i = 0; i < nt; i++){
		ch = (uchar)t[i];
		grp = -1;
		if(ch == '&')
			grp = 0;
		else if(ch == '\\' && i + 1 < nt){
			i++;
			ch = (uchar)t[i];
			if(ch >= '0' && ch <= '9')
				grp = ch - '0';
			else if(ch == 'n')
				ch = '\n';
			else if(ch == 't')
				ch = '\t';
		}
		if(grp < 0){
			if(out != nil)
				out[k] = (char)ch;
			k++;
			continue;
		}
		if(m->sp[grp] < 0 || m->ep[grp] < m->sp[grp])
			continue;
		n = m->ep[grp] - m->sp[grp];
		if(out != nil)
			for(g = 0; g < n; g += c){
				c = n - g > 4096 ? 4096 : n - g;
				c = buf_read(b, m->sp[grp] + g, out + k + g, (int)c);
				if(c <= 0)
					break;
			}
		k += n;
	}
	return k;
}

static char*
arena(Exec *ex, Pos n)
{
	Ablock *ab;
	char *p;

	ab = ex->ab;
	if(ab == nil || ab->cap - ab->used < n){
		ab = malloc(sizeof *ab);
		if(ab == nil)
			return nil;
		ab->cap = n > Ablk ? n : Ablk;
		ab->used = 0;
		ab->d = malloc(ab->cap);
		if(ab->d == nil){
			free(ab);
			return nil;
		}
		ab->next = ex->ab;
		ex->ab = ab;
	}
	p = ab->d + ab->used;
	ab->used += n;
	return p;
}

/* s: replace the match m. 0, or -1 with ex->failed */
static int
subst(Ed *ed, Exec *ex, Match *m)
{
	Buf *b;
	Pos ndel, nins;
	char *p;

	b = ed->doc.b;
	ex->nmatch++;
	ndel = m->ep[0] - m->sp[0];
	nins = expand(b, ex->cmd.text, ex->cmd.ntext, m, nil);
	if(ndel == 0 && nins == 0)
		return 0;
	if(nins > b->max - ex->asize)
		return fail(ed, ex, "buffer full");
	p = nil;
	if(nins > 0){
		p = arena(ex, nins);
		if(p == nil)
			return fail(ed, ex, "out of memory");
		expand(b, ex->cmd.text, ex->cmd.ntext, m, p);
		ex->asize += nins;
	}
	return emit(ed, ex, m->sp[0], ndel, p, nins);
}

/* run level `level` over the dot [lo, hi): a terminal when the chain is exhausted, else push its frame */
static int
enter(Ed *ed, Exec *ex, int level, Pos lo, Pos hi)
{
	Frame *fr;
	Cstep *st;

	if(level >= ex->cmd.nstep && ex->cmd.term != 's')
		return terminal(ed, ex, lo, hi);
	fr = &ex->f[level];
	if(level >= ex->cmd.nstep){
		/* terminal s: a frame of its own, so a big dot is scanned in slices */
		memset(fr, 0, sizeof *fr);
		fr->op = 's';
		fr->lo = lo;
		fr->hi = hi;
		fr->pat = &ex->spat;
		scan_init(&fr->sc, fr->pat, ed->doc.b, lo, hi);
		fr->hasscan = 1;
		ex->nf = level + 1;
		return 0;
	}
	st = &ex->cmd.step[level];
	memset(fr, 0, sizeof *fr);
	fr->op = st->op;
	fr->lo = lo;
	fr->hi = hi;
	fr->gap = lo;
	fr->pos = lo;
	if(st->re != nil){
		fr->pat = &ex->pat[level];
		scan_init(&fr->sc, fr->pat, ed->doc.b, lo, hi);
		fr->hasscan = 1;
	}
	ex->nf = level + 1;
	return 0;
}

static void
pop(Exec *ex)
{
	Frame *fr;

	fr = &ex->f[ex->nf - 1];
	if(fr->hasscan){
		scan_free(&fr->sc);
		fr->hasscan = 0;
	}
	ex->nf--;
}

static int
step1(Ed *ed, int *budget)
{
	Exec *ex;
	Frame *fr;
	Sel *s;
	Match m;
	Buf *b;
	int level, r, pass;
	Pos lo, hi, e;

	ex = ed->ex;
	b = ed->doc.b;
	for(;;){
		if(ex->nf == 0){
			if(ex->ri >= ex->ranges.n)
				return Sdone;
			s = &ex->ranges.s[ex->ri++];
			if(enter(ed, ex, 0, sel_min(s), sel_max(s)) < 0)
				return Sdone;
			continue;
		}
		if(*budget <= 0)
			return Smore;
		level = ex->nf - 1;
		fr = &ex->f[level];
		switch(fr->op){
		case 's':
			if(fr->phase){
				pop(ex);
				continue;
			}
			r = scan_next(&fr->sc, budget, &m);
			if(r == Smore)
				return Smore;
			if(r == Serror){
				fail(ed, ex, "out of memory");
				return Sdone;
			}
			if(r == Sdone){
				pop(ex);
				continue;
			}
			if(subst(ed, ex, &m) < 0)
				return Sdone;
			if(!ex->cmd.global)
				fr->phase = 1;
			break;
		case 'g':
		case 'v':
			if(fr->phase){
				pop(ex);
				continue;
			}
			r = scan_next(&fr->sc, budget, &m);
			if(r == Smore)
				return Smore;
			if(r == Serror){
				fail(ed, ex, "out of memory");
				return Sdone;
			}
			pass = (fr->op == 'g') == (r == Smatch);
			scan_free(&fr->sc);
			fr->hasscan = 0;
			fr->phase = 1;
			lo = fr->lo;
			hi = fr->hi;
			if(pass && enter(ed, ex, level + 1, lo, hi) < 0)
				return Sdone;
			break;
		case 'x':
		case 'y':
			if(fr->pat == nil){
				/* bare: one line (with its newline) per iteration; the last may lack one */
				if(fr->gap >= fr->hi){
					pop(ex);
					continue;
				}
				e = 0;
				while(fr->pos < fr->hi && *budget > 0){
					(*budget)--;
					if(buf_byte(b, fr->pos++) == '\n'){
						e = 1;
						break;
					}
				}
				if(!e && fr->pos < fr->hi)
					return Smore;
				lo = fr->gap;
				fr->gap = fr->pos;
				if(enter(ed, ex, level + 1, lo, fr->pos) < 0)
					return Sdone;
				break;
			}
			if(fr->op == 'y' && fr->phase){
				pop(ex);
				continue;
			}
			r = scan_next(&fr->sc, budget, &m);
			if(r == Smore)
				return Smore;
			if(r == Serror){
				fail(ed, ex, "out of memory");
				return Sdone;
			}
			if(r == Sdone){
				if(fr->op == 'x'){
					pop(ex);
					continue;
				}
				fr->phase = 1;
				lo = fr->gap;
				hi = fr->hi;
				if(enter(ed, ex, level + 1, lo, hi) < 0)
					return Sdone;
				break;
			}
			if(fr->op == 'x'){
				if(enter(ed, ex, level + 1, m.sp[0], m.ep[0]) < 0)
					return Sdone;
			}else{
				lo = fr->gap;
				fr->gap = m.ep[0];
				if(enter(ed, ex, level + 1, lo, m.sp[0]) < 0)
					return Sdone;
			}
			break;
		default:
			pop(ex);
			break;
		}
	}
}

static int
exec_step(Ed *ed, int *budget)
{
	int b0, r;

	b0 = *budget;
	r = step1(ed, budget);
	if(r == Sdone && ed->ex != nil && !ed->ex->failed && !ed->ex->noted && ed->ex->ne > Applynote){
		/* doc_apply cannot be interrupted: say so, and let the window redraw first */
		ed->ex->noted = 1;
		lsnprint(ed->msg, sizeof ed->msg, "applying %d changes...", ed->ex->ne);
		ed->job.draw = 1;
		r = Smore;
	}
	if(ed->job.on && b0 > *budget){
		ed->job.done += b0 - *budget;
		if(ed->job.done > ed->job.total)
			ed->job.done = ed->job.total;
	}
	return r;
}

static void
exec_finish(Ed *ed)
{
	Exec *ex;
	Txn *t;
	int n;

	ex = ed->ex;
	if(ex == nil || ex->failed){
		exec_free(ed);
		return;
	}
	n = ex->ne;
	ed->msg[0] = '\0';	/* drop work()'s "running NN%" progress text on every path */
	if(n == 0){
		if(ex->nmatch == 0)
			msg(ed, "no match");
		else if(ex->cmd.term != 0)
			msg(ed, "no changes");
		else if(ex->rec && ex->after.n > 0){
			ex->after.primary = 0;
			sel_copy(&ed->doc.sel, &ex->after);
			sel_normalize(&ed->doc.sel, ed->doc.b);
			ed->mode = Mnormal;
		}
		exec_free(ed);
		return;
	}
	ex->after.primary = ex->after.n - 1;
	doc_seal(&ed->doc);
	if(doc_apply(&ed->doc, ex->e, n, &ex->after) < 0){
		msg(ed, ed->doc.err);
		exec_free(ed);
		return;
	}
	vb_cursors(ed);
	/* redo must restore the cursors the user saw, not the bare carets */
	t = &ed->doc.h[ed->doc.cur - 1];
	sel_copy(&t->after, &ed->doc.sel);
	doc_seal(&ed->doc);
	ed->mode = Mnormal;
	if(n == 1)
		msg(ed, "1 change");
	else
		lsnprint(ed->msg, sizeof ed->msg, "%d changes", n);
	exec_free(ed);
}

/* compile re, or the last search pattern when it is empty */
static int
compile(Ed *ed, Pat *p, char *re, int nre, char *err, int nerr)
{
	if(nre == 0){
		if(!ed->srch.havepat){
			lsnprint(err, nerr, "%s", "no previous pattern");
			return -1;
		}
		return pat_comp(p, ed->srch.pat.src, ed->srch.pat.literal, err, nerr);
	}
	return pat_comp(p, re, 0, err, nerr);
}

int
exec_start(Ed *ed, char *line)
{
	Exec *ex;
	Sel *s;
	Cstep *st;
	char err[128];
	Pos total, mult;
	int i;

	exec_free(ed);
	ex = czalloc(sizeof *ex);
	if(ex == nil){
		msg(ed, "out of memory");
		return -1;
	}
	ed->ex = ex;
	sel_init(&ex->ranges);
	sel_init(&ex->after);
	if(cmd_parse(line, &ex->cmd, err, sizeof err) < 0){
		msg(ed, err);
		exec_free(ed);
		return -1;
	}
	ex->rec = ex->cmd.nstep == 0;
	for(i = 0; i < ex->cmd.nstep; i++){
		st = &ex->cmd.step[i];
		if(st->op == 'x' || st->op == 'y')
			ex->rec = 1;
		if(st->re != nil && compile(ed, &ex->pat[i], st->re, st->nre, err, sizeof err) < 0){
			msg(ed, err);
			exec_free(ed);
			return -1;
		}
	}
	if(ex->cmd.term == 's' && compile(ed, &ex->spat, ex->cmd.re, ex->cmd.nre, err, sizeof err) < 0){
		msg(ed, err);
		exec_free(ed);
		return -1;
	}
	if(ex->cmd.whole)
		sel_add(&ex->ranges, 0, buf_len(ed->doc.b));
	else
		for(i = 0; i < ed->doc.sel.n; i++){
			s = &ed->doc.sel.s[i];
			sel_add(&ex->ranges, sel_min(s), sel_max(s));
		}
	total = 0;
	for(i = 0; i < ex->ranges.n; i++)
		total += sel_max(&ex->ranges.s[i]) - sel_min(&ex->ranges.s[i]);
	mult = ex->cmd.nstep > 1 ? ex->cmd.nstep : 1;
	if(total > Maxfile / mult)
		total = Maxfile;
	else
		total *= mult;
	doc_seal(&ed->doc);
	job_start(ed, "running", exec_step, exec_finish, exec_cancel, total);
	return 0;
}

#include "compat.h"
#include "coil.h"

/*
 * search.c: / and n. A search is a Scan over [from, end) followed, when nothing is
 * found, by a Scan over [0, end) that accepts only a match starting before `from`
 * (the wrap). srch_step does one unit of work so a later plan can run it as an
 * job (job.c).
 */

static void
msg2(Ed *ed, char *fmt, char *a)
{
	lsnprint(ed->msg, sizeof ed->msg, fmt, a);
}

/* the match becomes the one selection; an empty match is a one-rune cursor; Select mode keeps the anchor */
static void
select_match(Ed *ed, Pos st, Pos en, int dir)
{
	Buf *b;
	Sel *p;

	b = ed->doc.b;
	if(ed->mode == Mselect){
		p = sel_primary(&ed->doc.sel);
		sel_set1(&ed->doc.sel, p->anchor, dir < 0 ? st : en);
		return;
	}
	if(en == st)
		en = buf_next(b, st);
	sel_set1(&ed->doc.sel, st, en);
}

static void
finish(Ed *ed)
{
	scan_free(&ed->srch.sc);
}

int
srch_step(Ed *ed, int *budget)
{
	Srch *s;
	Match m;
	int r, b0;

	s = &ed->srch;
	b0 = *budget;
	if(s->dir < 0)
		r = scan_prev(&s->sc, s->limit, budget, &m);
	else
		r = scan_next(&s->sc, budget, &m);
	if(ed->job.on && b0 > *budget){
		ed->job.done += b0 - *budget;
		if(ed->job.done > ed->job.total)
			ed->job.done = ed->job.total;
	}
	if(r == Smore)
		return Smore;
	if(r == Serror){
		msg2(ed, "%s", "out of memory");
		finish(ed);
		return Sdone;
	}
	if(r == Smatch){
		ed->msg[0] = '\0';
		if(s->dir > 0 && s->phase == 1 && m.sp[0] >= s->from){
			msg2(ed, "no match: %s", s->pat.src);
		}else{
			select_match(ed, m.sp[0], m.ep[0], s->dir);
			if(s->phase == 1){
				s->wrapped = 1;
				msg2(ed, "%s", "search wrapped");
			}
		}
		finish(ed);
		return Sdone;
	}
	/* Sdone: this range is exhausted */
	scan_free(&s->sc);
	if(s->phase == 0){
		s->phase = 1;
		scan_init(&s->sc, &s->pat, ed->doc.b, 0, buf_len(ed->doc.b));
		s->limit = buf_len(ed->doc.b);
		return Smore;
	}
	msg2(ed, "no match: %s", s->pat.src);
	return Sdone;
}

static void
srch_cancel(Ed *ed)
{
	scan_free(&ed->srch.sc);
}

/* start the search as a job in direction dir (stored in srch.dir) from the primary selection, to completion */
static int
search_run(Ed *ed)
{
	Srch *s;
	Pos len;

	s = &ed->srch;
	len = buf_len(ed->doc.b);
	s->phase = 0;
	s->wrapped = 0;
	ed->msg[0] = '\0';
	if(s->dir < 0){
		s->from = sel_min(sel_primary(&ed->doc.sel));
		s->limit = s->from;
		scan_init(&s->sc, &s->pat, ed->doc.b, 0, len);
	}else{
		s->from = sel_max(sel_primary(&ed->doc.sel));
		scan_init(&s->sc, &s->pat, ed->doc.b, s->from, len);
	}
	job_start(ed, "searching", srch_step, nil, srch_cancel, len);
	if(ed->job.on)
		return 0;
	return ed->msg[0] == '\0' || s->wrapped ? 0 : -1;
}

static int
setpat(Ed *ed, char *src, int literal)
{
	Srch *s;
	Pat np;
	char err[128];

	s = &ed->srch;
	if(pat_comp(&np, src, literal, err, sizeof err) < 0){
		msg2(ed, "%s", err);
		return -1;
	}
	if(s->havepat)
		pat_free(&s->pat);
	s->pat = np;
	s->havepat = 1;
	return 0;
}

int
search_start(Ed *ed, char *src, int dir)
{
	Srch *s;

	s = &ed->srch;
	if(src[0] == '\0'){
		if(!s->havepat){
			msg2(ed, "%s", "no previous pattern");
			return -1;
		}
	}else if(setpat(ed, src, 0) < 0)
		return -1;
	s->dir = dir;
	return search_run(ed);
}

/* n (dir +1) repeats in the stored direction, N (dir -1) in the opposite one; the stored direction stays */
int
search_next(Ed *ed, int dir)
{
	Srch *s;
	int saved, r;

	s = &ed->srch;
	if(!s->havepat){
		msg2(ed, "%s", "no previous pattern");
		return -1;
	}
	saved = s->dir;
	s->dir = saved * dir;
	r = search_run(ed);
	s->dir = saved;
	return r;
}

/* *: search forward for the selected text, literally; a one-rune cursor first becomes the word under it */
int
search_word(Ed *ed)
{
	Sel *p;
	Pos st, en, n;
	char t[Maxpat + 2];

	p = sel_primary(&ed->doc.sel);
	st = sel_min(p);
	en = sel_max(p);
	if(en == st || en == buf_next(ed->doc.b, st)){
		if(obj_select(ed, 1, 'w') < 0)
			return -1;
		p = sel_primary(&ed->doc.sel);
		st = sel_min(p);
		en = sel_max(p);
	}
	n = en - st;
	if(n > Maxpat){
		msg2(ed, "%s", "selection too long to search");
		return -1;
	}
	buf_read(ed->doc.b, st, t, n);
	t[n] = '\0';
	if((Pos)strlen(t) != n){
		msg2(ed, "%s", "cannot search for a NUL");
		return -1;
	}
	if(n == 0){
		msg2(ed, "%s", "nothing to search for");
		return -1;
	}
	if(setpat(ed, t, 1) < 0)
		return -1;
	ed->srch.dir = 1;
	return search_run(ed);
}

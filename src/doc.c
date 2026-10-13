#include "compat.h"
#include "coil.h"

/* doc.c: a buffer plus its selections. doc_apply is the only mutation path. */

int doc_bulkmin = Bulkmin;

int
doc_init(Doc *d)
{
	memset(d, 0, sizeof *d);
	d->b = buf_new();
	if(d->b == nil)
		return -1;
	sel_init(&d->sel);
	sel_init(&d->pre);
	sel_set1(&d->sel, 0, 0);
	d->dirty = -1;
	return 0;
}

static void
txnfree(Txn *t)
{
	int i;

	for(i = 0; i < t->nr; i++){
		buf_runfree(&t->r[i].removed);
		buf_runfree(&t->r[i].inserted);
	}
	free(t->r);
	sel_free(&t->before);
	sel_free(&t->after);
}

/* drop transactions [from, nh) */
static void
droptail(Doc *d, int from)
{
	while(d->nh > from)
		txnfree(&d->h[--d->nh]);
}

static void
histreset(Doc *d)
{
	droptail(d, 0);
	free(d->h);
	d->h = nil;
	d->hcap = 0;
	d->cur = 0;
	d->saved = 0;
	d->open = 0;
	d->haspre = 0;
}

void
doc_free(Doc *d)
{
	histreset(d);
	buf_free(d->b);
	d->b = nil;
	sel_free(&d->sel);
	sel_free(&d->pre);
}

int
doc_load(Doc *d, char *data, Pos n, char *err, int nerr)
{
	if(buf_load(d->b, data, n, err, nerr) < 0){
		/* the old stores are gone if buf_load took the data: drop history that points into them */
		if(d->b->orig == data){
			histreset(d);
			sel_set1(&d->sel, 0, 0);
			d->dirty = 0;
		}
		return -1;
	}
	histreset(d);
	sel_set1(&d->sel, 0, buf_next(d->b, 0));
	d->dirty = 0;
	d->err[0] = '\0';
	return 0;
}

void
doc_seal(Doc *d)
{
	d->open = 0;
	d->haspre = 0;
}

int
doc_modified(Doc *d)
{
	return d->cur != d->saved;
}

void
doc_mark_saved(Doc *d)
{
	doc_seal(d);
	d->saved = d->cur;
}

void
doc_set_pre(Doc *d, SelSet *s)
{
	sel_copy(&d->pre, s);
	d->haspre = 1;
}

static int
fail(Doc *d, char *msg)
{
	lsnprint(d->err, sizeof d->err, "%s", msg);
	return -1;
}

/* undo the records t->r[start..nr) of a batch that failed half way, newest first */
static void
rollback(Doc *d, Txn *t, int start)
{
	Rec *r;

	while(t->nr > start){
		r = &t->r[--t->nr];
		if(r->nins > 0)
			buf_delete(d->b, r->pos, r->nins, &r->inserted);
		buf_insert_pieces(d->b, r->pos, &r->removed);
		buf_runfree(&r->removed);
		buf_runfree(&r->inserted);
	}
}

int
doc_apply(Doc *d, Edit *e, int n, SelSet *after)
{
	int i, start, opened;
	Pos len, total, end;
	char err[64];
	Txn *t;
	Rec *r;

	if(n <= 0)
		return 0;
	len = buf_len(d->b);
	/* validate: sorted, non-overlapping, in range */
	end = 0;
	for(i = 0; i < n; i++){
		if(e[i].pos < end || e[i].ndel < 0 || e[i].nins < 0 ||
		   e[i].ndel > len - e[i].pos || (e[i].nins > 0 && e[i].ins == nil))
			return fail(d, "bad edit batch");
		end = e[i].pos + e[i].ndel;
	}
	/* cap */
	total = len;
	for(i = 0; i < n; i++){
		total -= e[i].ndel;
		if(e[i].nins > d->b->max - total)
			return fail(d, "buffer full");
		total += e[i].nins;
	}
	/* open a transaction if none is open: drops the redo tail */
	opened = !d->open;
	if(!d->open){
		if(d->cur < d->nh){
			droptail(d, d->cur);
			if(d->saved > d->cur)
				d->saved = -1;
		}
		if(d->nh == d->hcap){
			Txn *nh;
			int c;

			c = d->hcap == 0 ? 16 : d->hcap * 2;
			nh = realloc(d->h, c * sizeof(Txn));
			if(nh == nil)
				return fail(d, "out of memory");
			d->h = nh;
			d->hcap = c;
		}
		t = &d->h[d->nh];
		memset(t, 0, sizeof *t);
		sel_init(&t->before);
		sel_init(&t->after);
		sel_copy(&t->before, d->haspre ? &d->pre : &d->sel);
		d->haspre = 0;
		d->nh++;
		d->cur = d->nh;
		d->open = 1;
	}
	t = &d->h[d->cur - 1];
	if(t->nr + n > t->rcap){
		Rec *nr;
		int c;

		c = (t->nr + n) * 2;
		nr = realloc(t->r, c * sizeof(Rec));
		if(nr == nil)
			return fail(d, "out of memory");
		t->r = nr;
		t->rcap = c;
	}
	/* apply back to front so earlier positions stay valid; every edit is recorded.
	 * On failure the batch is rolled back so the buffer, history and selection stay consistent. */
	start = t->nr;
	if(n > doc_bulkmin){
		/* bulk: one merge pass; records are stored ascending with bulk = n. All-or-nothing. */
		Splice *sp;
		PieceRun *ins;

		sp = malloc(n * sizeof *sp);
		ins = czalloc(n * sizeof *ins);
		if(sp == nil || ins == nil){
			free(sp);
			free(ins);
			lsnprint(err, sizeof err, "out of memory");
			goto failed;
		}
		for(i = 0; i < n; i++){
			if(e[i].nins > 0 && buf_addpieces(d->b, e[i].ins, e[i].nins, &ins[i]) < 0){
				lsnprint(err, sizeof err, e[i].nins > d->b->max - d->b->nadd ? "buffer full" : "out of memory");
				goto bulkfail;
			}
			r = &t->r[start + i];
			memset(r, 0, sizeof *r);
			r->pos = e[i].pos;
			r->ndel = e[i].ndel;
			r->nins = e[i].nins;
			r->bulk = n;
			sp[i].pos = e[i].pos;
			sp[i].ndel = e[i].ndel;
			sp[i].rm = &r->removed;
			sp[i].ins = e[i].nins > 0 ? &ins[i] : nil;
		}
		if(buf_splice(d->b, sp, n) < 0){
			lsnprint(err, sizeof err, "out of memory");
			goto bulkfail;
		}
		t->nr += n;
		for(i = 0; i < n; i++)
			buf_runfree(&ins[i]);
		free(ins);
		free(sp);
		goto applied;
	bulkfail:
		for(i = 0; i < n; i++)
			buf_runfree(&ins[i]);
		free(ins);
		free(sp);
		goto failed;
	}
	for(i = n - 1; i >= 0; i--){
		r = &t->r[t->nr++];
		memset(r, 0, sizeof *r);
		r->pos = e[i].pos;
		r->ndel = e[i].ndel;
		r->nins = e[i].nins;
		if(e[i].ndel > 0 && buf_delete(d->b, e[i].pos, e[i].ndel, &r->removed) < 0){
			t->nr--;	/* nothing was removed */
			rollback(d, t, start);
			lsnprint(err, sizeof err, "out of memory");
			goto failed;
		}
		if(e[i].nins > 0 && buf_insert(d->b, e[i].pos, e[i].ins, e[i].nins, err, sizeof err) < 0){
			r->nins = 0;
			rollback(d, t, start);
			goto failed;
		}
	}
applied:
	if(after == nil)
		sel_map(&d->sel, e, n);
	else
		sel_copy(&d->sel, after);
	sel_normalize(&d->sel, d->b);
	sel_copy(&t->after, &d->sel);
	if(d->dirty < 0 || e[0].pos < d->dirty)
		d->dirty = e[0].pos;	/* the batch is sorted: e[0] is the lowest */
	d->err[0] = '\0';
	return 0;

failed:
	if(opened && t->nr == 0){
		/* drop the empty transaction this call created */
		txnfree(t);
		d->nh--;
		d->cur = d->nh;
		d->open = 0;
	}
	return fail(d, err);
}

static void
lowerdirty(Doc *d, Pos p)
{
	if(d->dirty < 0 || p < d->dirty)
		d->dirty = p;
}

static int
oom(Doc *d)
{
	return fail(d, "out of memory");
}

/* restore a recorded selection set, normalized */
static void
restore(Doc *d, SelSet *s)
{
	sel_copy(&d->sel, s);
	sel_normalize(&d->sel, d->b);
}

/* undo a bulk group of k ascending records in one pass; positions are in post-batch coordinates */
static int
undogroup(Doc *d, Rec *r, int k)
{
	Splice *sp;
	Pos shift;
	int j;

	sp = malloc(k * sizeof *sp);
	if(sp == nil)
		return -1;
	shift = 0;
	for(j = 0; j < k; j++){
		sp[j].pos = r[j].pos + shift;
		sp[j].ndel = r[j].nins;
		sp[j].rm = &r[j].inserted;
		sp[j].ins = &r[j].removed;
		shift += r[j].nins - r[j].ndel;
	}
	lowerdirty(d, r[0].pos);
	j = buf_splice(d->b, sp, k);
	free(sp);
	if(j < 0)
		return -1;
	for(j = 0; j < k; j++)
		buf_runfree(&r[j].removed);
	return 0;
}

/* redo a bulk group: positions are pre-batch coordinates */
static int
redogroup(Doc *d, Rec *r, int k)
{
	Splice *sp;
	int j;

	sp = malloc(k * sizeof *sp);
	if(sp == nil)
		return -1;
	for(j = 0; j < k; j++){
		sp[j].pos = r[j].pos;
		sp[j].ndel = r[j].ndel;
		sp[j].rm = &r[j].removed;
		sp[j].ins = &r[j].inserted;
	}
	lowerdirty(d, r[0].pos);
	j = buf_splice(d->b, sp, k);
	free(sp);
	if(j < 0)
		return -1;
	for(j = 0; j < k; j++)
		buf_runfree(&r[j].inserted);
	return 0;
}

/* undo records hi down to lo of t; *at is the record that failed. A failed record leaves the
 * buffer as it found it (best effort: its own deletion is put back). */
static int
undorecs(Doc *d, Txn *t, int hi, int lo, int *at)
{
	Rec *r;
	int i;

	for(i = hi; i >= lo; i--){
		r = &t->r[i];
		*at = i;
		if(r->bulk > 0){
			if(undogroup(d, &t->r[i - r->bulk + 1], r->bulk) < 0)
				return -1;
			i -= r->bulk - 1;
			continue;
		}
		lowerdirty(d, r->pos);
		if(buf_delete(d->b, r->pos, r->nins, &r->inserted) < 0)
			return -1;
		if(buf_insert_pieces(d->b, r->pos, &r->removed) < 0){
			if(buf_insert_pieces(d->b, r->pos, &r->inserted) == 0)
				buf_runfree(&r->inserted);
			return -1;
		}
		buf_runfree(&r->removed);
	}
	return 0;
}

/* redo records lo up to hi of t; *at is the record that failed (the first of a failed group) */
static int
redorecs(Doc *d, Txn *t, int lo, int hi, int *at)
{
	Rec *r;
	int i;

	for(i = lo; i <= hi; i++){
		r = &t->r[i];
		*at = i;
		if(r->bulk > 0){
			if(redogroup(d, &t->r[i], r->bulk) < 0)
				return -1;
			i += r->bulk - 1;
			continue;
		}
		lowerdirty(d, r->pos);
		if(buf_delete(d->b, r->pos, r->ndel, &r->removed) < 0)
			return -1;
		if(buf_insert_pieces(d->b, r->pos, &r->inserted) < 0){
			if(buf_insert_pieces(d->b, r->pos, &r->removed) == 0)
				buf_runfree(&r->removed);
			return -1;
		}
		buf_runfree(&r->inserted);
	}
	return 0;
}

int
doc_undo(Doc *d)
{
	Txn *t;
	int at, bt;

	doc_seal(d);
	if(d->cur == 0)
		return fail(d, "nothing to undo");
	t = &d->h[d->cur - 1];
	if(undorecs(d, t, t->nr - 1, 0, &at) < 0){
		/* put back the records already undone (those after at) so a retry sees the old text */
		redorecs(d, t, at + 1, t->nr - 1, &bt);
		return oom(d);
	}
	restore(d, &t->before);
	d->cur--;
	d->err[0] = '\0';
	return 0;
}

int
doc_redo(Doc *d)
{
	Txn *t;
	int at, bt;

	doc_seal(d);
	if(d->cur >= d->nh)
		return fail(d, "nothing to redo");
	t = &d->h[d->cur];
	if(redorecs(d, t, 0, t->nr - 1, &at) < 0){
		/* put back the records already redone (those before at) */
		if(at > 0)
			undorecs(d, t, at - 1, 0, &bt);
		return oom(d);
	}
	restore(d, &t->after);
	d->cur++;
	d->err[0] = '\0';
	return 0;
}

#include "compat.h"
#include "coil.h"

/*
 * job.c: lengthy work as a step function called in byte-budgeted slices by the
 * event loop. The steps never mutate the buffer; only finish may, so cancel
 * needs no rollback. One owner thread, no locks.
 */

static int
work(Ed *ed)
{
	Job *j;
	int budget, r, pct;

	j = &ed->job;
	budget = ed->slice > 0 ? ed->slice : Slice;
	r = j->step(ed, &budget);
	if(r == Sdone){
		j->on = 0;
		if(j->finish != nil)
			j->finish(ed);
		ed_reveal(ed);
		return Jdone;
	}
	if(j->draw){
		/* the step set a message of its own to be seen before the slow non-interruptible step */
		j->draw = 0;
		return Jdraw;
	}
	/* done * 100 overflows an int past 21 MB: divide first on big totals */
	if(j->total >= 100)
		pct = j->done / (j->total / 100);
	else if(j->total > 0)
		pct = j->done * 100 / j->total;
	else
		pct = 0;
	if(pct < 0)
		pct = 0;
	if(pct > 99)
		pct = 99;
	lsnprint(ed->msg, sizeof ed->msg, "%s %d%% (Esc interrupts)", j->what, pct);
	return Jmore;
}

int
ed_work(Ed *ed)
{
	if(!ed->job.on)
		return Jdone;
	return work(ed);
}

void
job_start(Ed *ed, char *what, int (*step)(Ed*, int*), void (*finish)(Ed*), void (*cancel)(Ed*), Pos total)
{
	Job *j;

	j = &ed->job;
	memset(j, 0, sizeof *j);
	j->on = 1;
	lsnprint(j->what, sizeof j->what, "%s", what);
	j->step = step;
	j->finish = finish;
	j->cancel = cancel;
	j->total = total > 0 ? total : 1;
	work(ed);
}

void
job_cancel(Ed *ed)
{
	if(!ed->job.on)
		return;
	if(ed->job.cancel != nil)
		ed->job.cancel(ed);
	ed->job.on = 0;
	lsnprint(ed->msg, sizeof ed->msg, "%s", "interrupted");
}

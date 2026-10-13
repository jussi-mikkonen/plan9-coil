#include "compat.h"
#include "coil.h"

/* bench: non-gating timing of the buffer on big inputs (make bench). */

#ifdef PLAN9
static long
now_ms(void)
{
	return (long)(nsec() / 1000000);
}
#else
#include <time.h>
static long
now_ms(void)
{
	return (long)((double)clock() * 1000.0 / CLOCKS_PER_SEC);
}
#endif

static u32int seed = 12345;

/* own 32-bit LCG, fixed seed */
static u32int
rnd(void)
{
	seed = seed * 1664525u + 1013904223u;
	return seed >> 8;
}

static char*
mklines(Pos total, int w)
{
	char *d;
	Pos i;

	d = malloc(total + 1);
	for(i = 0; i < total; i++)
		d[i] = ((i + 1) % w == 0) ? '\n' : (char)('a' + (i % w) % 26);
	return d;
}

static void
report(char *name, long t0)
{
	lprint("bench %s %d\n", name, (int)(now_ms() - t0));
}

static void
suite(char *tag, char *data, Pos n)
{
	Buf *b;
	long t;
	int i, nl;
	char name[64], err[64];
	Pos pos, len;

	b = buf_new();
	t = now_ms();
	buf_load(b, data, n, err, sizeof err);
	nl = buf_nlines(b);
	lsnprint(name, sizeof name, "%s load+index", tag);
	report(name, t);

	t = now_ms();
	for(i = 0; i < 1000; i++)
		buf_line_start(b, rnd() % nl);
	lsnprint(name, sizeof name, "%s 1000 line_start", tag);
	report(name, t);

	t = now_ms();
	for(i = 0; i < 1000; i++){
		len = buf_len(b);
		buf_insert(b, rnd() % (len + 1), "x", 1, err, sizeof err);
	}
	lsnprint(name, sizeof name, "%s 1000 random inserts", tag);
	report(name, t);

	t = now_ms();
	len = buf_len(b);
	for(i = 0; i < 100000; i++){
		pos = len - (Pos)i * (len / 100001);
		buf_insert(b, pos, "y", 1, err, sizeof err);
	}
	lsnprint(name, sizeof name, "%s 100000 back-to-front inserts", tag);
	report(name, t);

	t = now_ms();
	for(i = 0; i < 1000; i++)
		buf_pos_to_line(b, rnd() % buf_len(b));
	lsnprint(name, sizeof name, "%s 1000 pos_to_line after edits", tag);
	report(name, t);
	if(buf_check(b, err, sizeof err) < 0)
		lprint("bench check bad %s\n", err);
	buf_free(b);
}

static void
applybench(int n)
{
	Doc d;
	Edit *e;
	Pos len;
	long t;
	int i;
	char *data, name[64], err[64];

	len = (Pos)n * 4;
	data = malloc(len + 1);
	memset(data, 'a', len);
	doc_init(&d);
	doc_load(&d, data, len, err, sizeof err);
	e = malloc(n * sizeof(Edit));
	for(i = 0; i < n; i++){
		e[i].pos = (Pos)i * 4;
		e[i].ndel = 1;
		e[i].ins = "b";
		e[i].nins = 1;
	}
	t = now_ms();
	doc_apply(&d, e, n, nil);
	lsnprint(name, sizeof name, "apply %d", n);
	report(name, t);
	free(e);
	doc_free(&d);
}

/* ratio gate: scaling check, set by applybench2 */
static int ratiobad;
static long t2[3][3];	/* [op][size] ms */

static void
applybench2(int n, int si)
{
	Doc d;
	Edit *e;
	Pos len;
	long t;
	int i;
	char *data, name[64], err[64];

	len = 50 * 1024 * 1024;
	data = malloc(len + 1);
	memset(data, 'a', len);
	doc_init(&d);
	doc_load(&d, data, len, err, sizeof err);
	e = malloc(n * sizeof(Edit));
	for(i = 0; i < n; i++){
		e[i].pos = (Pos)i * (len / n);
		e[i].ndel = 1;
		e[i].ins = "b";
		e[i].nins = 1;
	}
	t = now_ms();
	if(doc_apply(&d, e, n, nil) < 0)
		lprint("bench apply2 failed %s\n", d.err);
	t2[0][si] = now_ms() - t;
	lsnprint(name, sizeof name, "apply2 %d", n);
	report(name, t);
	t = now_ms();
	doc_undo(&d);
	t2[1][si] = now_ms() - t;
	lsnprint(name, sizeof name, "undo2 %d", n);
	report(name, t);
	t = now_ms();
	doc_redo(&d);
	t2[2][si] = now_ms() - t;
	lsnprint(name, sizeof name, "redo2 %d", n);
	report(name, t);
	free(e);
	doc_free(&d);
}

static void
ratiogate(void)
{
	static char *ops[] = {"apply", "undo", "redo"};
	int op;
	long a, b;

	for(op = 0; op < 3; op++){
		a = t2[op][1];
		b = t2[op][2];
		lprint("bench ratio %s %d\n", ops[op], a > 0 ? (int)(b * 100 / a) : -1);
		if(b > 6 * a && b > 200)
			ratiobad = 1;
	}
}

#ifdef PLAN9
void
main(void)
#else
int
main(void)
#endif
{
	applybench(10000);
	applybench(20000);
	applybench(50000);
	applybench2(50000, 0);
	applybench2(100000, 1);
	applybench2(200000, 2);
	ratiogate();
	suite("50MB-lines", mklines(50*1024*1024, 80), 50*1024*1024);
	suite("5MB-oneline", mklines(5*1024*1024, 5*1024*1024 + 1), 5*1024*1024);
#ifdef PLAN9
	exits(ratiobad ? "ratio" : nil);
#else
	return ratiobad;
#endif
}

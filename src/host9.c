/* host9.c: Host implementation on 9front: open/dirfstat/readn/create. Impure. */
#include "compat.h"
#include <draw.h>
#include <cell.h>
#include "coil.h"
#include "host.h"
#include "shell.h"

static int
h_exists(Host*, char *path)
{
	return access(path, AEXIST) == 0;
}

static void
seterr(char *err, int nerr, char *s)
{
	if(err != nil && nerr > 0)
		snprint(err, nerr, "%s", s);
}

static void
fill(Stamp *st, Dir *d)
{
	st->kind = (d->qid.type & QTDIR) ? Sdir : Sfile;
	st->append = (d->qid.type & QTAPPEND) != 0;
	st->type = d->type;
	st->dev = d->dev;
	st->pathhi = (u32int)(d->qid.path >> 32);
	st->pathlo = (u32int)d->qid.path;
	st->vers = d->qid.vers;
	st->mtime = d->mtime;
	st->length = d->length > Maxfile ? Maxfile : (Pos)d->length;
}

static int
h_stat(Host*, char *path, Stamp *st, char *err, int nerr)
{
	Dir *d;

	d = dirstat(path);
	if(d == nil){
		if(access(path, AEXIST) < 0){
			stamp_clear(st);
			return 0;
		}
		snprint(err, nerr, "%r");
		return -1;
	}
	fill(st, d);
	free(d);
	return 0;
}

static int
h_readfile(Host*, char *path, char **data, Pos *n, int *isnew, Stamp *st, char *err, int nerr)
{
	int fd;
	Dir *d;
	char *buf, *nb;
	vlong cap, tot, want;
	long k;

	*data = nil;
	*n = 0;
	*isnew = 0;
	stamp_clear(st);
	fd = open(path, OREAD);
	if(fd < 0){
		if(access(path, AEXIST) < 0){
			*isnew = 1;
			return 0;
		}
		snprint(err, nerr, "%r");
		return -1;
	}
	d = dirfstat(fd);
	if(d == nil){
		snprint(err, nerr, "%r");
		close(fd);
		return -1;
	}
	fill(st, d);
	if(d->qid.type & QTDIR){
		free(d);
		close(fd);
		seterr(err, nerr, "is a directory");
		return -1;
	}
	if(d->length > Maxfile){
		free(d);
		close(fd);
		seterr(err, nerr, "file too large");
		return -1;
	}
	cap = d->length + 1;
	if(cap < 8192)
		cap = 8192;
	free(d);
	buf = malloc(cap);
	if(buf == nil){
		close(fd);
		seterr(err, nerr, "out of memory");
		return -1;
	}
	tot = 0;
	for(;;){
		if(tot == cap){
			want = cap * 2;
			if(tot >= Maxfile || want > (vlong)Maxfile + 1)
				want = (vlong)Maxfile + 1;
			if(want <= cap){
				free(buf);
				close(fd);
				seterr(err, nerr, "file too large");
				return -1;
			}
			nb = realloc(buf, want);
			if(nb == nil){
				free(buf);
				close(fd);
				seterr(err, nerr, "out of memory");
				return -1;
			}
			buf = nb;
			cap = want;
		}
		k = readn(fd, buf + tot, cap - tot);
		if(k < 0){
			snprint(err, nerr, "%r");
			free(buf);
			close(fd);
			return -1;
		}
		if(k == 0)
			break;
		tot += k;
		if(tot > Maxfile){
			free(buf);
			close(fd);
			seterr(err, nerr, "file too large");
			return -1;
		}
	}
	close(fd);
	*data = buf;
	*n = (Pos)tot;
	return 0;
}

static int
wfn(void *arg, char *p, int n)
{
	return write(*(int*)arg, p, n);
}

static int
h_writefile(Host*, char *path, Buf *b, Stamp *expect, Stamp *out, char *err, int nerr)
{
	int fd;
	Dir *d;
	Stamp now;

	/* a stat error on an existing file refuses: treating it as "absent" would skip the check */
	if(h_stat(nil, path, &now, err, nerr) < 0)
		return -1;
	if(now.kind != Snone && stamp_refuse(&now, err, nerr) < 0)
		return -1;
	/* checked just before open: the window for another writer is microseconds */
	if(expect != nil && stamp_changed(expect, &now))
		return Wchanged;

	if(access(path, AEXIST) == 0)
		fd = open(path, OWRITE|OTRUNC);
	else
		fd = create(path, OWRITE, 0666);
	if(fd < 0){
		snprint(err, nerr, "%r");
		return -1;
	}
	if(buf_write(b, wfn, &fd) < 0){
		snprint(err, nerr, "write: %r");
		close(fd);
		return -1;
	}
	/*
	 * the identity after our own write, so the next :w does not refuse against
	 * it: from the written fd, not the path, so a writer that slips in after
	 * close is not absorbed into it (acme exec.c)
	 */
	d = dirfstat(fd);
	if(d != nil){
		fill(out, d);
		free(d);
	}else{
		stamp_clear(out);
		out->kind = Sunknown;
	}
	/* in-place write: the file system may report a failure only at close */
	if(close(fd) < 0){
		snprint(err, nerr, "close: %r");
		return -1;
	}
	return 0;
}

void
host9_init(Host *h)
{
	h->exists = h_exists;
	h->readfile = h_readfile;
	h->writefile = h_writefile;
	h->stat = h_stat;
	h->snarfget = s9_snarfget;
	h->snarfput = s9_snarfput;
	h->aux = nil;
}

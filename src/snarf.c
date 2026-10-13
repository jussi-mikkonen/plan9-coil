#include "compat.h"
#include "coil.h"

/*
 * snarf.c: register-versus-/dev/snarf policy. The internal register (verbs.c)
 * keeps one string per selection; /dev/snarf holds them joined by newlines.
 * reg.mark is the hash of what snarf held when coil last looked (read back
 * after every put, since the device may convert what it is given), so a later
 * paste can tell "still ours" (use the register, per-selection strings and
 * oversize yanks survive) from "another program changed it" (paste that).
 * The device I/O is behind Host.snarfget/snarfput; this file is pure.
 */

/* 32-bit FNV-1a */
u32int
snarf_hash(char *p, Pos n)
{
	u32int h;
	Pos i;

	h = 2166136261UL;
	for(i = 0; i < n; i++){
		h ^= (uchar)p[i];
		h *= 16777619UL;
	}
	return h;
}

/* take the mark from the current snarf content; returns 0 when it can be read */
static int
setmark(Ed *ed)
{
	char *d;
	Pos n;
	char e[64];

	ed->reg.markvalid = 0;
	if(ed->host == nil || ed->host->snarfget == nil)
		return -1;
	if(ed->host->snarfget(ed->host, &d, &n, e, sizeof e) < 0)
		return -1;
	ed->reg.mark = snarf_hash(d, n);
	ed->reg.markvalid = 1;
	free(d);
	return 0;
}

static void
setmsg(Ed *ed, char *fmt, char *a)
{
	lsnprint(ed->msg, sizeof ed->msg, fmt, a);
}

/* the register was just filled by a yank: publish it */
void
snarf_publish(Ed *ed)
{
	Reg *r;
	Host *h;
	char *j, *p;
	char e[64];
	Pos total;
	int k;

	r = &ed->reg;
	h = ed->host;
	if(h == nil || h->snarfput == nil || r->n == 0)
		return;
	total = r->n - 1;
	for(k = 0; k < r->n; k++)
		total += r->len[k];
	if(total > Snarfmax){
		lsnprint(ed->msg, sizeof ed->msg, "yank %d bytes: too big for snarf, kept in coil", total);
		setmark(ed);	/* never open /dev/snarf for write: rio commits partial data at close */
		return;
	}
	j = malloc(total + 1);
	if(j == nil){
		r->markvalid = 0;
		setmsg(ed, "%s", "out of memory");
		return;
	}
	p = j;
	for(k = 0; k < r->n; k++){
		if(k > 0)
			*p++ = '\n';
		memmove(p, r->s[k], r->len[k]);
		p += r->len[k];
	}
	if(h->snarfput(h, j, total, e, sizeof e) == 0){
		free(j);
		/*
		 * hash what /dev/snarf now returns, not the bytes sent: rio stores
		 * runes (drops NULs, invalid bytes become U+FFFD), so a hash of our
		 * own bytes would call the next paste "changed by another program"
		 */
		setmark(ed);
		return;
	}
	free(j);
	if(setmark(ed) < 0){
		/* no snarf device at all: say so once */
		if(!r->warned){
			r->warned = 1;
			setmsg(ed, "%s", "no /dev/snarf; using coil's register");
		}
		return;
	}
	lsnprint(ed->msg, sizeof ed->msg, "snarf: %s; kept in coil", e);
}

/* a paste is about to read the register: adopt the snarf when another program changed it */
void
snarf_refresh(Ed *ed)
{
	Reg nr, *r;
	Host *h;
	char *d, *s;
	Pos n;
	char e[64];
	u32int hash;

	r = &ed->reg;
	h = ed->host;
	if(h == nil || h->snarfget == nil)
		return;
	if(h->snarfget(h, &d, &n, e, sizeof e) < 0)
		return;
	if(n == 0){
		free(d);
		return;
	}
	hash = snarf_hash(d, n);
	if(r->markvalid && hash == r->mark){
		free(d);
		return;
	}
	memset(&nr, 0, sizeof nr);
	nr.s = czalloc(sizeof(char*));
	nr.len = czalloc(sizeof(Pos));
	s = malloc(n + 1);
	if(nr.s == nil || nr.len == nil || s == nil){
		free(nr.s);
		free(nr.len);
		free(s);
		free(d);
		return;
	}
	memmove(s, d, n);
	s[n] = '\0';
	free(d);
	nr.s[0] = s;
	nr.len[0] = n;
	nr.n = 1;
	nr.primary = 0;
	nr.mark = hash;
	nr.markvalid = 1;
	nr.warned = r->warned;
	reg_free(r);
	*r = nr;
}

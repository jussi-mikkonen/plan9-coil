/* file.c: pure policy on file identity, shared by the real and the fake host. */
#include "compat.h"
#include "coil.h"

void
stamp_clear(Stamp *s)
{
	memset(s, 0, sizeof *s);
	s->kind = Snone;
}

/* 1 when now is not the file old describes */
int
stamp_changed(Stamp *old, Stamp *now)
{
	if(old->kind == Sunknown || now->kind == Sunknown)
		return 0;
	if(old->kind == Snone)
		return now->kind != Snone;
	if(now->kind == Snone)
		return 0;	/* deleted since the read: a write recreates it */
	if(old->kind != now->kind)
		return 1;
	if(old->type != now->type || old->dev != now->dev)
		return 1;
	if(old->pathhi != now->pathhi || old->pathlo != now->pathlo)
		return 1;
	if(old->vers != now->vers)
		return 1;
	if(old->mtime + 1 < now->mtime)
		return 1;
	return 0;
}

int
stamp_refuse(Stamp *now, char *err, int nerr)
{
	if(now->kind == Sdir){
		lsnprint(err, nerr, "is a directory");
		return -1;
	}
	if(now->kind == Sfile && now->append && now->length > 0){
		lsnprint(err, nerr, "append-only file, not written");
		return -1;
	}
	return 0;
}

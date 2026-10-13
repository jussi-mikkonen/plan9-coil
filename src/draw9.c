/* draw9.c: Canvas -> libcell adapter and theme loading. Impure. */
#include "compat.h"
#include <draw.h>
#include <cell.h>
#include "coil.h"
#include "host.h"
#include "shell.h"

static char coil_default_theme[] =
	"status fg=#101010 bg=#b0b0b0\n"
	"message fg=#ffaf00\n"
	"ctrl fg=#5fafff\n"
	"invalid fg=#ff5f5f\n"
	"type fg=#af87af\n"
	"number fg=#d7875f\n"
	"preproc fg=#af875f\n"
	"variable fg=#5fafaf\n"
	"target fg=#d7af5f\n"
	"recipe fg=#b8b8b8\n"
	"heading fg=#d7af5f underline\n"
	"emph fg=#af87d7\n"
	"strong fg=#d7af5f underline\n"
	"code fg=#87afaf\n"
	"link fg=#5f87d7\n"
	"listmark fg=#d7875f\n"
	"match fg=#ffffff bg=#5f5f87\n"
	"trailing bg=#5f2f2f\n"
	"linenr fg=#6c6c6c\n";

static int sid[Nslot][4];

int
d9_init(Cell *cell, char *themepath, char *msg, int nmsg)
{
	static char *names[Nslot] = {
		"default", "status", "message", "ctrl", "invalid",
		"keyword", "type", "comment", "string", "number", "preproc", "variable", "target", "recipe",
		"heading", "emph", "strong", "code", "link", "listmark", "match", "trailing", "linenr"
	};
	int i, base, sel, cur, rc;

	rc = 0;
	if(cellloadthemestr(cell, coil_default_theme) < 0)
		sysfatal("embedded theme: %r");
	if(themepath != nil && access(themepath, AREAD) == 0){
		if(cellloadtheme(cell, themepath) < 0){
			snprint(msg, nmsg, "theme: %r");
			rc = -1;
		}
	}
	sel = cellstyle(cell, "selection");
	cur = cellstyle(cell, "cursor");
	for(i = 0; i < Nslot; i++){
		base = cellstyle(cell, names[i]);
		sid[i][0] = base;
		sid[i][1] = cellmix(cell, base, sel);
		sid[i][2] = cellmix(cell, base, cur);
		sid[i][3] = sid[i][2];
	}
	return rc;
}

void
d9_present(Cell *cell, Canvas *cv, int scrolled, int usescroll)
{
	int row, col, st, slot, ov, textrows;
	Rune r;

	textrows = cv->rows - 1;
	if(usescroll && scrolled != 0 && scrolled > -textrows && scrolled < textrows)
		cellscroll(cell, 0, textrows, scrolled);

	for(row = 0; row < cv->rows; row++){
		for(col = 0; col < cv->cols; col++){
			r = cv->r[row*cv->cols + col];
			if(r == Ccont)
				continue;
			st = cv->style[row*cv->cols + col];
			slot = st & 0x3f;
			if(slot >= Nslot)
				slot = Sdefault;
			ov = (st & Osel ? 1 : 0) | (st & Ocur ? 2 : 0);
			cellput(cell, row, col, r, sid[slot][ov]);
		}
	}
	if(cellflush(cell) < 0)
		cellinvalidate(cell);	/* retry on the next event; never fatal */
}

int
d9_cells(void *c, Rune r)
{
	return cellwidth((Cell*)c, r);
}

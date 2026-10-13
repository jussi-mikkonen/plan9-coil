/* main.c: threadmain, event loop over mouse, resize and keys. Impure. */
#include "compat.h"
#include <draw.h>
#include <thread.h>
#include <mouse.h>
#include <keyboard.h>
#include <cell.h>
#include "coil.h"
#include "host.h"
#include "shell.h"

int mainstacksize = 32768;

static Ed ed;
static Canvas cv;
static Host host;
static Cell *cell;
static Font *cfont;
static Mousectl *mc;
static Keyboardctl *kc;
static int debug;
static int scrollhint;
static vlong lastdraw;

Rune
keytrans(Rune r)
{
	switch(r){
	case Kup:	return Ck_up;
	case Kdown:	return Ck_down;
	case Kleft:	return Ck_left;
	case Kright:	return Ck_right;
	case Khome:	return Ck_home;
	case Kend:	return Ck_end;
	case Kpgup:	return Ck_pgup;
	case Kpgdown:	return Ck_pgdn;
	}
	return r;
}

static void
usage(void)
{
	fprint(2, "usage: coil [-D] [-s] [-f font] file[:N]\n");
	threadexitsall("usage");
}

static void
relayout(void)
{
	int rows, cols;

	cellsize(cell, &rows, &cols);
	ed_resize(&ed, rows, cols);
	if(canvas_resize(&cv, rows, cols) < 0)
		snprint(ed.msg, sizeof ed.msg, "out of memory");
	if(debug)
		fprint(2, "resize %d %d\n", rows, cols);
}

static void
key(Rune r)
{
	Ev e;

	memset(&e, 0, sizeof e);
	if(r == Kscrolloneup || r == Kscrollonedown){
		e.kind = Ewheel;
		e.n = r == Kscrolloneup ? -1 : 1;
		ed_event(&ed, &e);
		return;
	}
	e.kind = Ekey;
	e.r = keytrans(r);
	if(debug)
		fprint(2, "key %x\n", r);
	ed_event(&ed, &e);
}

/* Mouse -> Ev: wheel is m.buttons & 8 (up) / & 16 (down), button 1 click/double/drag/release through cellpoint */
static void
mousein(Mouse m)
{
	static int prev, lastmsec, lastrow = -1, lastcol = -1, held;
	static int havelast, currow = -1, curcol = -1;
	int row, col, b, was;
	Ev e;

	b = m.buttons;
	was = prev;
	prev = b;
	memset(&e, 0, sizeof e);
	if(debug)
		fprint(2, "mouse b=%d x=%d y=%d\n", b, m.xy.x, m.xy.y);
	if((b & 8) && !(was & 8)){
		e.kind = Ewheel;
		e.n = -1;
		ed_event(&ed, &e);
		return;
	}
	if((b & 16) && !(was & 16)){
		e.kind = Ewheel;
		e.n = 1;
		ed_event(&ed, &e);
		return;
	}
	cellpoint(cell, m.xy, &row, &col);	/* clamped to the nearest cell when outside */
	if(debug)
		fprint(2, "  -> row %d col %d\n", row, col);
	e.row = row;
	e.col = col;
	if((b & 1) && !(was & 1)){
		if(havelast && (uint)m.msec - (uint)lastmsec < 500 && row == lastrow && col == lastcol){
			e.kind = Edouble;
			havelast = 0;
		}else{
			e.kind = Eclick;
			havelast = 1;
			lastmsec = m.msec;
			lastrow = row;
			lastcol = col;
		}
		held = 1;
		currow = row;
		curcol = col;
		ed_event(&ed, &e);
	}else if((b & 1) && held){
		if(row != currow || col != curcol){
			currow = row;
			curcol = col;
			e.kind = Edrag;
			ed_event(&ed, &e);
		}
	}else if(!(b & 1) && (was & 1)){
		held = 0;
		e.kind = Erelease;
		ed_event(&ed, &e);
	}
}

static void
redraw(void)
{
	render(&ed, &cv);
	d9_present(cell, &cv, ed.view.scrolled, scrollhint);
	ed.view.scrolled = 0;
	lastdraw = nsec();
	if(debug)
		fprint(2, "draw\n");
}

void
threadmain(int argc, char **argv)
{
	char *fontname, *home, *themepath;
	Mouse m;
	Rune r;
	int w, wr;
	Alt a[4];

	fontname = "/lib/font/bit/vga/unicode.font";
	ARGBEGIN{
	case 'D':
		debug++;
		break;
	case 's':
		scrollhint = 1;
		break;
	case 'f':
		fontname = EARGF(usage());
		break;
	default:
		usage();
	}ARGEND
	if(argc != 1)
		usage();

	host9_init(&host);
	if(ed_init(&ed, &host, nil, nil) < 0)
		sysfatal("ed_init: out of memory");
	if(ed_open(&ed, argv[0]) < 0){
		fprint(2, "coil: %s\n", ed.msg);
		threadexitsall("open");
	}
	if(initdraw(nil, nil, "coil") < 0)
		sysfatal("initdraw: %r");
	mc = initmouse(nil, screen);	/* rio hides navigation keys from a raw client without the mouse */
	if(mc == nil)
		sysfatal("initmouse: %r");
	kc = initkeyboard(nil);
	if(kc == nil)
		sysfatal("initkeyboard: %r");
	cfont = openfont(display, fontname);
	if(cfont == nil)
		sysfatal("openfont %s: %r", fontname);
	cell = cellopen(cfont);
	if(cell == nil)
		sysfatal("cellopen: %r");

	themepath = nil;
	home = getenv("home");
	if(home != nil){
		themepath = smprint("%s/lib/coil/theme", home);
		free(home);
	}
	d9_init(cell, themepath, ed.msg, sizeof ed.msg);
	free(themepath);

	ed.view.runecells = d9_cells;
	ed.view.rcarg = cell;
	relayout();
	redraw();

	a[0].c = mc->c;		a[0].v = &m;		a[0].op = CHANRCV;
	a[1].c = mc->resizec;	a[1].v = &w;		a[1].op = CHANRCV;
	a[2].c = kc->c;		a[2].v = &r;		a[2].op = CHANRCV;
	a[3].op = CHANEND;

	for(;;){
		/* CHANNOBLK: alt returns 3 only when no input is ready, so keys and mouse always win over a job slice */
		a[3].op = ed.job.on ? CHANNOBLK : CHANEND;
		switch(alt(a)){
		case 0:
			mousein(m);
			while(nbrecv(mc->c, &m) > 0)
				mousein(m);
			break;
		case 1:
			if(getwindow(display, Refnone) < 0)
				sysfatal("getwindow: %r");
			if(cellresize(cell) < 0)
				snprint(ed.msg, sizeof ed.msg, "resize: %r");
			relayout();
			break;
		case 2:
			key(r);
			while(nbrecv(kc->c, &r) > 0)
				key(r);
			break;
		case 3:
			wr = ed_work(&ed);
			if(wr == Jmore && nsec() - lastdraw < 100000000LL)
				continue;
			break;
		}
		if(ed.quit){
			cellclose(cell);
			threadexitsall(nil);
		}
		redraw();
	}
}

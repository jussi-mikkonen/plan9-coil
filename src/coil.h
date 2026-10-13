/* coil.h: shared types and prototypes of the pure core.
 * Included once per file, after compat.h. Includes nothing itself. */

enum { Chunk = 65536, Runeerr = 0xFFFD };

/* utf.c: strict UTF-8 */
int	utf_dec(char *s, int n, Rune *r);	/* bytes consumed (1-4); invalid byte -> Runeerr, 1; never reads past n */
int	utf_enc(char *s, Rune r);		/* bytes written (1-4) */
int	utf_isstart(int c);			/* 1 if byte c is not a continuation byte */

typedef struct Piece Piece;
typedef struct PieceRun PieceRun;
typedef struct Buf Buf;

struct Piece {
	uchar	src;	/* 0 = orig, 1 = add */
	Pos	off;	/* offset into the source store */
	Pos	len;
	int	nl;	/* newlines inside the piece */
};

struct PieceRun {	/* removed pieces, kept for undo */
	Piece	*p;
	int	n;
};

struct Buf {
	char	*orig;
	Pos	norig;
	char	*add;
	Pos	nadd, addcap;
	Piece	*p;
	int	np, pcap;
	Pos	*start;		/* start[i] = byte offset of piece i (valid for i < clean) */
	int	*nlstart;	/* nlstart[i] = newlines before piece i (valid for i < clean) */
	int	clean;		/* prefix arrays valid for pieces [0, clean) */
	int	lastpi;		/* cache: piece of the last lookup */
	Pos	max;		/* size cap, default Maxfile; tests lower it */
	Pos	len;
	int	nl;
};

/* buf.c: piece table over raw bytes */
Buf*	buf_new(void);
void	buf_free(Buf*);
int	buf_load(Buf*, char *data, Pos n, char *err, int nerr);	/* takes ownership of data (malloc), also on failure; -1 "file too large" if n > b->max */
Pos	buf_len(Buf*);
int	buf_nlines(Buf*);			/* newline count + 1 */
int	buf_byte(Buf*, Pos);			/* 0-255, or -1 outside [0,len) */
int	buf_read(Buf*, Pos, char *out, int n);	/* bytes copied */
int	buf_rune(Buf*, Pos, Rune *r);		/* decode at pos across pieces; length, 0 at EOF */
Pos	buf_next(Buf*, Pos);			/* next rune start */
Pos	buf_prev(Buf*, Pos);			/* previous rune start */
Pos	buf_cnext(Buf*, Pos);			/* next step; "\r\n" is one unit */
Pos	buf_cprev(Buf*, Pos);			/* previous step; "\r\n" is one unit */
Pos	buf_snap(Buf*, Pos);			/* move back to a rune start */
Pos	buf_line_start(Buf*, int line);		/* 0-based; clamps to [0, nlines-1] */
int	buf_pos_to_line(Buf*, Pos);
int	buf_write(Buf*, int (*fn)(void *arg, char *p, int n), void *arg);	/* slices <= Chunk; 0, or -1 if fn returns != n */
int	buf_insert(Buf*, Pos, char *p, Pos n, char *err, int nerr);	/* -1 "buffer full" past the cap */
int	buf_delete(Buf*, Pos, Pos n, PieceRun *removed);	/* removed may be nil */
int	buf_insert_pieces(Buf*, Pos, PieceRun*);	/* undo: re-insert removed pieces, no byte copying */
void	buf_runfree(PieceRun*);

/* one forward merge pass for a sorted batch; used by doc.c only */
typedef struct Splice Splice;
struct Splice {
	Pos	pos;		/* ascending, non-overlapping, in the coordinates of the buffer before the call */
	Pos	ndel;
	PieceRun *rm;		/* out: the removed pieces (nil = not wanted) */
	PieceRun *ins;		/* in: pieces to insert (nil or n == 0 = nothing); copied */
};
int	buf_splice(Buf*, Splice *s, int n);	/* 0, or -1 with the buffer unchanged (out of memory, buffer full) */
int	buf_addpieces(Buf*, char *p, Pos n, PieceRun *out);	/* append bytes to the add store, return them as Chunk-sized pieces; -1 changes no text */
int	buf_check(Buf*, char *why, int nwhy);	/* recount; 0 ok, -1 + reason */

/* sel.c: selections. Sel covers [min,max) */
typedef struct Edit Edit;
typedef struct Sel Sel;
typedef struct SelSet SelSet;

struct Edit {		/* pre-batch coordinates; a batch is sorted ascending and non-overlapping */
	Pos	pos;
	Pos	ndel;
	char	*ins;
	Pos	nins;
};

struct Sel {
	Pos	anchor;
	Pos	head;
	int	goal;	/* display cell column remembered by j/k, or -1 */
};

struct SelSet {
	Sel	*s;
	int	n;
	int	cap;
	int	primary;
};

void	sel_init(SelSet*);
void	sel_free(SelSet*);
void	sel_copy(SelSet *dst, SelSet *src);
void	sel_set1(SelSet*, Pos anchor, Pos head);	/* replace the set with one range */
int	sel_add(SelSet*, Pos anchor, Pos head);		/* append; index, or -1 when out of memory */
Pos	sel_min(Sel*);
Pos	sel_max(Sel*);
Sel*	sel_primary(SelSet*);
void	sel_normalize(SelSet*, Buf*);	/* clamp, snap to rune starts, sort by min, merge, keep the primary */
void	sel_map(SelSet*, Edit*, int n);	/* map a normalized set through a batch; no snap or merge */
int	sel_check(SelSet*, Buf*, char *why, int nwhy);	/* 0 ok, -1 + first violated invariant */

/* doc.c: the single mutation path for a buffer */
typedef struct Doc Doc;

/* history: transactions hold piece runs, not text (orig/add are never rewritten) */
typedef struct Rec Rec;
typedef struct Txn Txn;

struct Rec {
	Pos	pos;
	Pos	ndel;		/* bytes removed at pos when applied */
	Pos	nins;		/* bytes inserted at pos when applied */
	PieceRun removed;	/* filled while the record is applied */
	PieceRun inserted;	/* filled while the record is undone (for redo) */
	int	bulk;		/* 0, or the size k of the bulk batch this record belongs to (k records, ascending positions) */
};

struct Txn {
	Rec	*r;
	int	nr;
	int	rcap;
	SelSet	before;
	SelSet	after;
};

struct Doc {
	Buf	*b;
	SelSet	sel;
	Txn	*h;
	int	nh;
	int	hcap;
	int	cur;		/* transactions h[0..cur) are applied */
	int	saved;		/* value of cur at the last save, -1 if that state is gone */
	int	open;		/* 1 while h[cur-1] accepts edits */
	SelSet	pre;		/* selection to record as "before" of the next transaction */
	int	haspre;
	char	err[128];
	Pos	dirty;		/* lowest position changed since the last hl_sync, -1 = none */
};

int	doc_init(Doc*);
void	doc_free(Doc*);
int	doc_load(Doc*, char *data, Pos n, char *err, int nerr);	/* takes ownership of data; selection on the first rune; clean, history empty */
int	doc_apply(Doc*, Edit *e, int n, SelSet *after);	/* 0, or -1 + d->err ("bad edit batch", "buffer full"); after == nil maps sel */
int	doc_undo(Doc*);		/* 0, or -1 with err "nothing to undo"; seals first; restores the selection from before the step */
int	doc_redo(Doc*);		/* 0, or -1 with err "nothing to redo"; restores the selection from after the step */
void	doc_seal(Doc*);		/* the next edit starts a new transaction */
int	doc_modified(Doc*);	/* text differs from the last saved revision */
void	doc_mark_saved(Doc*);	/* seal, then remember the current revision as saved */
void	doc_set_pre(Doc*, SelSet*);	/* selection to restore on undo of the next transaction (cleared by seal) */

/* lang.c, lex.c, hl.c: languages and the syntax highlighter */
typedef struct Lang Lang;
typedef u32int Hstate;	/* 0 = normal in every language; the bit layout is private to each lexer */

/* Line lexer. s[0..n) is the line without its newline and without a final '\r'; full = 0 when the cap cut the line.
 * out[i] = slot of byte i (continuation bytes take the slot of their lead byte). *st in: state at line start, out: state at the next line start. */
typedef void (*Lexfn)(Hstate *st, uchar *s, int n, int full, uchar *out);

struct Lang {
	char	*name;
	int	tabwidth;
	int	usetabs;
	Lexfn	lex;		/* nil = plain */
};

Lang*	lang_for_name(char *path);		/* by file name only */
Lang*	lang_detect(char *path, Buf *b);	/* name rules first, then a #! first line when the name gave plain; path and b may be nil */
Lang*	lang_by_name(char *name);		/* "c" "rc" "mkfile" "markdown" "plain", else nil */

void	lex_c(Hstate*, uchar*, int, int, uchar*);
void	lex_rc(Hstate*, uchar*, int, int, uchar*);
void	lex_mk(Hstate*, uchar*, int, int, uchar*);
void	lex_md(Hstate*, uchar*, int, int, uchar*);

/* Hl: sparse checkpoints (state at the start of every Hlckint-th line), invalidated from the edit line on.
 * Per screen at most look + visible lines are lexed from the nearest checkpoint; a line is lexed up to cap bytes. */
enum { Brlines = 500, Brbytes = 65536, Brstack = 4096 };
enum { Hlcap = 4096, Hlckint = 128, Hllook = 2000, Hlmaxck = 4096 };

typedef struct Hlck Hlck;
struct Hlck {
	int	line;
	Hstate	st;
};

typedef struct Hl Hl;
struct Hl {
	Lang	*lang;
	Hlck	*ck;		/* sorted by line; ck[0] is always {0, 0} after hl_reset */
	int	nck, ckcap;
	int	look;		/* look-back in lines for a cold jump */
	int	cap;		/* bytes lexed per line */
	int	nlexed;		/* lines lexed since the counter was cleared (tests) */
	Pos	next;		/* hl_line: start of the line after the one just lexed (buffer length at the last line) */
	uchar	slots[Hlcap];	/* scratch for one line's slots (hl_state uses it; render may pass it to hl_line) */
	char	text[Hlcap + 1];
};

void	hl_init(Hl*);
void	hl_free(Hl*);
void	hl_reset(Hl*, Lang*);		/* new language or new document: drop every checkpoint */
void	hl_sync(Hl*, Doc*);		/* apply Doc.dirty: drop checkpoints from the dirty line on */
Pos	hl_match(Hl*, Buf*, Pos p);	/* partner of the bracket at p, or -1 (bounded: Brlines/Brbytes/Brstack) */
Hstate	hl_state(Hl*, Buf*, int line);	/* state at the start of line, after hl_sync */
int	hl_line(Hl*, Buf*, Pos ls, Hstate *st, uchar *slots);	/* bytes classified (<= cap); advances *st; sets h->next */

/* view.c: layout walker shared by render and position<->cell mapping */
typedef struct View View;
typedef struct Walk Walk;

struct View {
	Pos	top;		/* line start of the first text row */
	int	rows;		/* text rows (Canvas rows - 1) */
	int	cols;
	int	hscroll;
	int	tabwidth;
	int	(*runecells)(void*, Rune);	/* nil: every rune is 1 cell */
	void	*rcarg;
	int	scrolled;	/* lines the view changes moved top by since the last present (cellscroll hint), 0 when unknown */
};

enum { Wtext, Wtab, Wctrl, Winvalid, Wnl, Weof, Wtrunc, Maxline = 65536 };

struct Walk {
	View	*v;
	Buf	*b;
	Pos	start;	/* line start */
	Pos	pos;
	int	col;
	Rune	show2;	/* Wctrl: the second shown rune ("^" is the first) */
};

typedef struct Vcell Vcell;
struct Vcell {		/* one screen cell of a laid-out row */
	Rune	r;	/* shown rune, or Ccont */
	uchar	slot;	/* Sdefault, Sctrl or Sinvalid */
	Pos	pos;	/* source position of the rune shown here (line end for cells past the text) */
};

void	view_init(View*);
void	walk_init(Walk*, View*, Buf*, Pos linestart);
int	walk_next(Walk*, Rune *show, int *cells);	/* kind of the rune at w->pos and its shown rune and width, then advances; Wnl/Weof do not advance */
int	view_col_of(View*, Buf*, Pos);
Pos	view_pos_at_col(View*, Buf*, int line, int col);
int	view_pos_to_cell(View*, Buf*, Pos, int *row, int *col);	/* 0 if on screen, -1 if not */
int	view_layout(View*, Buf*, Pos linestart, Vcell *out, int n);	/* fill n cells from column hscroll; index of the line-end cell, or -1 if not visible */
Pos	view_cell_to_pos(View*, Buf*, int row, int col);	/* inverse of view_pos_to_cell with the same walker */
void	view_scroll(View*, Buf*, int nlines);	/* move top by whole lines, clamped to the last line */
void	view_fix(View*, Buf*);		/* top back onto a line start inside the buffer */
void	view_reveal(View*, Buf*, Pos);

/* render.c */
typedef struct Canvas Canvas;

struct Canvas {
	int	rows;
	int	cols;
	Rune	*r;
	uchar	*style;
};

enum { Ccont = 0x7FFFFFFF };	/* second cell of a wide rune; never passed to libcell */
enum {	/* style slots; ids 0-4 never move. Sbracket is the theme style "match" (Smatch is an rx result) */
	Sdefault, Sstatus, Smessage, Sctrl, Sinvalid,
	Skeyword, Stype, Scomment, Sstring, Snumber, Spreproc, Svariable, Starget, Srecipe,
	Sheading, Semph, Sstrong, Scode, Slink, Slistmark, Sbracket, Strailing, Slinenr, Nslot
};
enum { Osel = 0x40, Ocur = 0x80 };	/* overlay bits OR'ed into Canvas.style */

int	canvas_resize(Canvas*, int rows, int cols);
void	canvas_free(Canvas*);

/* rx.c: regexp seam over libregexp (the system library on 9front, the vendored copy on Linux) */
enum { Maxpat = 1024, Scanwin = 262144, Scanlook = 65536, Nsub = 10, Sdone = 0, Smatch = 1, Smore = 2, Serror = 3 };

typedef struct Pat Pat;
struct Pat {
	Rx	*rx;
	int	multiline;	/* the translated pattern contains a newline: scan windows look ahead */
	int	literal;
	char	src[Maxpat + 1];	/* the text as typed (for messages and repeat) */
};

typedef struct Match Match;
struct Match {
	Pos	sp[Nsub];	/* -1 for an unset group */
	Pos	ep[Nsub];
};

typedef struct Scan Scan;
struct Scan {
	Pat	*pat;
	Buf	*b;
	Pos	lo, hi;		/* matches start >= lo and end <= hi */
	Pos	pos;		/* next search start; hi + 1 when the range is exhausted */
	Pos	prevend;	/* end of the previous match (empty-match rule), -1 */
	char	*win;		/* scratch: win[0] = context byte, win[1..] = text from wbase, NULs replaced by 0x01 */
	int	wcap;
	Pos	wbase, wend;	/* absolute range win[1..] holds; -1 when empty */
	Pos	blockend;	/* matches must start before blockend in this window */
	int	winsize;	/* bytes per window block (Scanwin; tests lower it) */
	Pos	pbe;		/* scan_prev: end of the next block to search */
	int	pstarted;
	int	atend;		/* the window's text ends at a true line/buffer end: "$" may match there */
};

extern int rx_winsize;	/* default Scanwin; rxtest "win N" changes it */
extern int rx_failwin;	/* tests: window allocations left before one fails (out-of-memory path); -1 = never */

int	rx_translate(char *in, char *out, int nout, char *err, int nerr);	/* \n -> 0x0A, \t -> 0x09, \\ and \x kept; length, or -1 "regexp too long" */
int	pat_comp(Pat*, char *src, int literal, char *err, int nerr);	/* 0, or -1 + "bad regexp: MSG" / "regexp too long" */
void	pat_free(Pat*);
void	scan_init(Scan*, Pat*, Buf*, Pos lo, Pos hi);
int	scan_next(Scan*, int *budget, Match*);	/* Smatch, Sdone (range exhausted), Smore (budget spent: call again), Serror (out of memory) */
int	scan_prev(Scan*, Pos limit, int *budget, Match*);	/* match with the greatest start in [lo, limit); Smatch, Sdone, Smore, Serror */
void	scan_free(Scan*);

/* cmd.c: sam-subset structural commands, parsed into a linear chain */
enum { Maxdepth = 8, Maxedits = 250000, Applynote = 50000 };	/* doc_apply is linear; see make bench */
enum { Bulkmin = 16 };	/* batches with more edits than doc_bulkmin take the buf_splice path */
extern int doc_bulkmin;

extern int exec_maxedits;	/* = Maxedits; edtest "maxedits N" lowers it */

typedef struct Cstep Cstep;
struct Cstep {
	int	op;	/* 'x' 'y' 'g' 'v' */
	char	*re;	/* malloc'd, NUL-terminated; nil = bare x/y (lines) */
	int	nre;
};

typedef struct Cmd Cmd;
struct Cmd {
	int	whole;	/* % or , prefix: the whole buffer instead of each selection */
	Cstep	step[Maxdepth];
	int	nstep;
	int	term;	/* 0 (none) 'd' 'c' 'i' 'a' 's' */
	char	*text;	/* malloc'd, NUL-terminated, escapes resolved (c i a); raw replacement for s */
	Pos	ntext;
	char	*re;	/* s: pattern, malloc'd (empty = the last pattern) */
	int	nre;
	int	global;	/* s: the g flag */
};

int	cmd_parse(char *line, Cmd*, char *err, int nerr);	/* 0, or -1 + message; strings malloc'd */
void	cmd_free(Cmd*);
void	cmd_dump(Cmd*, char *out, int n);	/* canonical text for goldens */

/* ed.c */
enum { Mnormal, Minsert, Mselect };
enum { Ekey, Eclick, Edrag, Edouble, Ewheel, Erelease };	/* mouse: row/col are window cells (gutter included); Ewheel n = +-1 steps of 3 lines (n > 0 scrolls down) */

enum {
	Ck_esc = 0x1b, Ck_bs = 0x08, Ck_tab = 0x09, Ck_nl = 0x0a, Ck_cr = 0x0d, Ck_del = 0x7f,
	Ck_up = 0xF00E, Ck_down = 0xF800, Ck_left = 0xF011, Ck_right = 0xF012,
	Ck_home = 0xF00D, Ck_end = 0xF018, Ck_pgup = 0xF00F, Ck_pgdn = 0xF013
};
#define CTL(c) ((c) & 0x1f)

typedef struct Ev Ev;
typedef struct Ed Ed;
typedef struct Reg Reg;

/* Host is defined here, not in host.h, so every file sees struct Ed complete:
 * 6l compares type signatures, and a struct that is complete in one file and
 * incomplete in another gives each Ed* function two signatures. */
typedef struct Host Host;

enum { Snone, Sfile, Sdir, Sunknown };
enum { Wchanged = -2 };
enum { Snarfmax = 100*1024, Snarfread = 16*1024*1024 };

typedef struct Stamp Stamp;
struct Stamp {
	int	kind;		/* Snone (no file), Sfile, Sdir, Sunknown (identity not known: no change check) */
	int	append;		/* qid.type & QTAPPEND */
	int	type;		/* Dir.type */
	int	dev;		/* Dir.dev */
	u32int	pathhi;		/* qid.path >> 32 */
	u32int	pathlo;
	u32int	vers;		/* qid.vers */
	u32int	mtime;
	Pos	length;		/* clamped to Maxfile */
};

void	stamp_clear(Stamp*);			/* kind Snone, all fields 0 */
int	stamp_changed(Stamp *old, Stamp *now);	/* 1 when now is not the file old describes */
int	stamp_refuse(Stamp *now, char *err, int nerr);	/* -1 + "is a directory" / "append-only file, not written"; 0 otherwise */

struct Host {
	int	(*exists)(Host*, char *path);
	int	(*readfile)(Host*, char *path, char **data, Pos *n, int *isnew, Stamp *st, char *err, int nerr);	/* st filled (Snone for a missing file); data malloc'd, ownership passes to the caller; a missing file gives 0, isnew = 1, n = 0 */
	int	(*writefile)(Host*, char *path, Buf *b, Stamp *expect, Stamp *out, char *err, int nerr);	/* writes the buf_write slices; 0; -1 + err; Wchanged when expect != nil and the file is not the one expected; stamp_refuse always applies */
	int	(*stat)(Host*, char *path, Stamp *st, char *err, int nerr);	/* 0 (Snone when missing), -1 + err */
	int	(*snarfget)(Host*, char **data, Pos *n, char *err, int nerr);	/* 0: data malloc'd (n may be 0); -1 + err: no snarf device or read error */
	int	(*snarfput)(Host*, char *p, Pos n, char *err, int nerr);	/* 0, or -1 + err; never called with n > Snarfmax */
	void	*aux;
};

struct Ev {
	int	kind;
	Rune	r;
	int	row;
	int	col;
	int	n;
};

struct Reg {		/* internal register: one byte string per yanked selection (NUL-terminated copies) */
	char	**s;
	Pos	*len;
	int	n;
	int	primary;	/* index of the yanked primary selection */
	u32int	mark;		/* hash of what coil last published to /dev/snarf (or found there) */
	int	markvalid;
	int	warned;		/* "no /dev/snarf" already reported */
};

enum { Maxprompt = 1024, Nhist = 32 };

typedef struct Srch Srch;
struct Srch {
	Pat	pat;		/* the last pattern (valid when havepat) */
	int	havepat;
	int	dir;		/* +1 forward, -1 backward */
	Scan	sc;
	Pos	limit;		/* backward: the match must start before this */
	Pos	from;		/* where phase 0 started: the end of the primary selection */
	int	wrapped;
	int	phase;		/* 0: from..end, 1: start..end after the wrap */
};

typedef struct Prompt Prompt;
struct Prompt {
	int	on;
	Rune	kind;		/* ':', '/' or '?' */
	char	text[Maxprompt + 1];	/* UTF-8, NUL-terminated */
	int	n;		/* bytes in text */
	char	*hist[2][Nhist];	/* [0] ':' commands, [1] searches; newest last; malloc'd */
	int	nh[2];
	int	hi;		/* index while browsing with Up/Down, -1 when not browsing */
};

enum { Slice = 262144, Jdone = 0, Jmore = 1, Jdraw = 2 };

typedef struct Job Job;
struct Job {
	int	on;
	char	what[24];		/* "searching", "running" */
	int	(*step)(Ed*, int *budget);	/* Sdone / Smore; steps set msg themselves */
	void	(*finish)(Ed*);		/* nil allowed; called once after step returns Sdone */
	void	(*cancel)(Ed*);		/* nil allowed; frees job state */
	Pos	done, total;		/* progress in bytes, total > 0 */
	int	draw;			/* step asks for an immediate redraw (Jdraw) */
};

/* exec.c: a structural command in flight. Frame i runs step i over its dot [lo, hi); the stack is explicit, no recursion. */
typedef struct Frame Frame;
struct Frame {
	int	op;
	Pat	*pat;		/* compiled step pattern, nil for bare x/y (lines) */
	Scan	sc;
	int	hasscan;
	Pos	lo, hi;
	Pos	gap;		/* y: start of the next gap; bare x/y: start of the line being scanned */
	Pos	pos;		/* bare x/y: how far the line scan has got */
	int	phase;		/* g/v: 1 once the child ran; y: 1 once the tail was emitted */
};

/* replacement texts of s live in chunks that never move, so Edit.ins pointers stay valid */
enum { Ablk = 65536 };

typedef struct Ablock Ablock;
struct Ablock {
	Ablock	*next;
	char	*d;
	int	used, cap;
};

typedef struct Exec Exec;
struct Exec {
	Cmd	cmd;
	Pat	pat[Maxdepth];
	Pat	spat;		/* the pattern of a terminal s */
	Frame	f[Maxdepth + 1];	/* + the frame of a terminal s */
	int	nf;		/* active frames, indexes 0..nf-1 */
	SelSet	ranges;		/* the starting dots: each selection, or the whole buffer */
	int	ri;
	Edit	*e;		/* emitted edits, ascending */
	int	ne, ecap;
	Pos	lastend;	/* end of the last emitted edit */
	Pos	delta;		/* net growth of the edits so far */
	SelSet	after;		/* result selections */
	int	rec;		/* the chain selects (it contains x or y, or is empty) */
	int	nmatch;		/* dots that reached the end of the chain */
	int	failed;
	int	noted;		/* "applying N changes..." has been shown */
	Ablock	*ab;	/* replacement texts of s (chunks, so Edit.ins pointers stay valid) */
	Pos	asize;		/* bytes expanded so far */
};

struct Ed {
	Doc	doc;
	View	view;
	Lang	*lang;
	Host	*host;
	char	*path;
	int	isnew;
	Stamp	stamp;		/* identity of path as last read or written */
	int	crlf;
	int	mode;
	int	quit;
	int	qarmed;
	int	count;
	Rune	pend[4];
	int	npend;
	Rune	key;		/* the last key given to keymap_key (the argument of a Rany binding) */
	Reg	reg;
	int	reveal;
	int	dragging;
	char	msg[128];
	Prompt	prompt;
	Srch	srch;
	Job	job;
	Exec	*ex;		/* malloc'd while a structural command runs */
	int	slice;		/* byte budget of one job slice (Slice; tests lower it) */
	Hl	hl;
	int	gutter;	/* line numbers shown (user toggle, on by default) */
	int	wincols;	/* window columns; view.cols is the text part */
};

int	ed_init(Ed*, struct Host*, int (*runecells)(void*, Rune), void *rcarg);
void	ed_free(Ed*);
int	ed_target(struct Host*, char *arg, char **path, int *line);	/* path is malloc'd; line 1-based, 1 when there is no :N */
int	ed_open(Ed*, char *arg);	/* 0, or -1 with ed->msg */
void	ed_event(Ed*, Ev*);
void	ed_key(Ed*, Rune);
void	ed_resize(Ed*, int rows, int cols);	/* rows includes the status row */
int	ed_save(Ed*);
int	ed_load(Ed*, char *path, int line);	/* path is caller-owned (copied); 0, or -1 with msg and the current document untouched */
int	ed_write(Ed*, char *path, int force);	/* path nil = the document's own file; 0, or -1 with msg */
void	render(Ed*, Canvas*);
Pos	ed_cursor(Ed*, Sel*);		/* the cursor position of a selection (cursor cell rule) */
int	ed_gutter(Ed*);		/* gutter width in cells, 0 when hidden */
void	ed_layout(Ed*);		/* view.cols = wincols - ed_gutter */
int	ed_lastline(Ed*);		/* last real line: the empty line after a final newline does not count */
void	ed_status(Ed*, char *out, int n, int *nmsg);	/* status text; *nmsg = offset where the message part starts, or -1 */

/* keymap.c */
int	keymap_key(Ed*, Rune);	/* 1 if the key was consumed (bound, prefix or count digit) */
void	ed_to_insert(Ed*);
void	ed_to_normal(Ed*);
void	ed_imove(Ed*, int kind);	/* caret movement in insert mode; kind is an Mv_ value */
void	ed_undo(Ed*, int redo, int count);
void	ed_move(Ed*, int dir, int count);	/* dir: 'h' 'j' 'k' 'l' */

/* motion.c: motions and selection operators; every function acts on each selection */
enum {
	Mv_left, Mv_right, Mv_down, Mv_up,
	Mv_wordnext, Mv_wordprev, Mv_wordend,
	Mv_bigwordnext, Mv_bigwordprev, Mv_bigwordend,
	Mv_linestart, Mv_lineend, Mv_filestart, Mv_fileend, Mv_goline,
	Mv_halfdown, Mv_halfup, Mv_pagedown, Mv_pageup
};
enum { Cspace, Cword, Cpunct, Ceol };	/* rune classes: ASCII exact, >= 0x80 is Cword; never isalpharune */

int	rune_class(Rune);
void	mv_motion(Ed*, int kind, int count);	/* Mnormal replaces, Mselect extends (anchor kept); vertical kinds keep Sel.goal, others reset it */
void	sel_op(Ed*, int op);			/* op: 'x' line, '%' file, ';' collapse, ',' flip */

/* obj.c: text objects */
int	obj_select(Ed*, int inner, Rune what);	/* 0 if found for the primary; -1 and msg "no object" otherwise, selection unchanged where not found */

/* verbs.c: acting on every selection; each verb is one transaction (c: the insert session joins it) */
void	vb_cursors(Ed*);		/* an empty selection that is not at EOF becomes a one-rune cursor; goals reset */
void	vb_delete(Ed*);
void	vb_change(Ed*);
void	vb_yank(Ed*);
void	vb_paste(Ed*, int after);	/* p = 1, P = 0; the selection becomes the pasted text */
void	vb_replace(Ed*);		/* R: selections replaced by the register */
void	vb_rchar(Ed*, Rune);		/* r<char>: every rune of each selection except line breaks becomes the rune */
void	vb_insert(Ed*, int where);	/* 'i' before, 'a' after, 'I' first non-blank, 'A' line end, 'o' open below, 'O' open above */
void	vb_type(Ed*, Rune);		/* insert-mode rune at every caret; Enter auto-indents, Tab per language */
void	vb_erase(Ed*, int what);	/* 'h' rune, 'w' word, 'u' line */
void	reg_free(Reg*);
u32int	snarf_hash(char *p, Pos n);	/* FNV-1a 32 */
void	snarf_publish(Ed*);		/* after a yank filled ed->reg */
void	snarf_refresh(Ed*);		/* before a paste reads ed->reg */

/* prompt.c: the status-row command line */
void	prompt_open(Ed*, Rune kind);
int	prompt_key(Ed*, Rune);		/* always 1: the prompt consumes every key */
void	prompt_free(Prompt*);

/* search.c: / ? n N * */
int	search_start(Ed*, char *src, int dir);	/* dir +1 or -1; src "" reuses the last pattern; 0 on a match, -1 otherwise (msg set) */
int	search_next(Ed*, int dir);		/* n */
int	search_word(Ed*);			/* * */
int	srch_step(Ed*, int *budget);		/* Sdone when finished (selection/msg set), Smore otherwise */

/* job.c: lengthy work in budgeted slices; single owner thread, steps never mutate the buffer */
void	job_start(Ed*, char *what, int (*step)(Ed*, int*), void (*finish)(Ed*), void (*cancel)(Ed*), Pos total);	/* runs the first slice now */
int	ed_work(Ed*);		/* one slice: Jdone, Jmore, Jdraw */
void	job_cancel(Ed*);	/* msg "interrupted"; nothing applied */
void	ed_reveal(Ed*);		/* ed.c: scroll the primary cursor into view */

/* excmd.c: ':' commands */
int	ex_run(Ed*, char *line);	/* 0, or -1 with ed->msg */

/* exec.c: structural commands */
int	exec_start(Ed*, char *line);	/* parse, compile, run as a job; -1 + msg on a parse or regexp error */
void	exec_free(Ed*);			/* drop a running command's state (cancel, ed_free) */

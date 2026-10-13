#include "compat.h"
#include "coil.h"

/* keymap.c: key table with multi-key prefixes and a count prefix. */

typedef struct Bind Bind;

struct Bind {
	int	modes;		/* bit set of (1 << mode) */
	Rune	seq[3];		/* zero-terminated */
	void	(*fn)(Ed*, int);
	int	arg;
};

enum { MN = 1 << Mnormal, MI = 1 << Minsert, MS = 1 << Mselect, Maxcount = 100000, Rany = 0x7FFFFFFF };

static int
count(Ed *ed)
{
	return ed->count > 0 ? ed->count : 1;
}

static void
act_move(Ed *ed, int arg)
{
	mv_motion(ed, arg, count(ed));
}

/* G: with a count go to that line, else to the end of the file */
static void
act_goline(Ed *ed, int arg)
{
	(void)arg;
	if(ed->count > 0)
		mv_motion(ed, Mv_goline, ed->count);
	else
		mv_motion(ed, Mv_fileend, 1);
}

static void
act_selop(Ed *ed, int arg)
{
	int i, n;

	n = arg == 'x' ? count(ed) : 1;
	for(i = 0; i < n; i++)
		sel_op(ed, arg);
}

static void
act_select(Ed *ed, int arg)
{
	(void)arg;
	ed->mode = ed->mode == Mselect ? Mnormal : Mselect;
}

static void
act_unselect(Ed *ed, int arg)
{
	(void)arg;
	ed->mode = Mnormal;
}

/* Esc in Normal mode: drop every selection except the primary */
static void
act_keepprimary(Ed *ed, int arg)
{
	Sel p;

	(void)arg;
	if(ed->doc.sel.n <= 1)
		return;
	p = *sel_primary(&ed->doc.sel);
	sel_set1(&ed->doc.sel, p.anchor, p.head);
	ed->doc.sel.s[0].goal = p.goal;
}

/* mi/ma: arg is the object rune, with 0x10000 set for "inner" */
static void
act_obj(Ed *ed, int arg)
{
	obj_select(ed, (arg & 0x10000) != 0, (Rune)(arg & 0xffff));
}

static void
act_verb(Ed *ed, int arg)
{
	switch(arg){
	case 'd':
		vb_delete(ed);
		break;
	case 'c':
		vb_change(ed);
		break;
	case 'y':
		vb_yank(ed);
		break;
	case 'p':
		vb_paste(ed, 1);
		break;
	case 'P':
		vb_paste(ed, 0);
		break;
	case 'R':
		vb_replace(ed);
		break;
	}
}

/* r<char>: the second key is the replacement; Esc cancels */
static void
act_rchar(Ed *ed, int arg)
{
	(void)arg;
	if(ed->key != Ck_esc)
		vb_rchar(ed, ed->key);
}

static void
act_save(Ed *ed, int arg)
{
	(void)arg;
	ed_save(ed);
}

static void
act_prompt(Ed *ed, int arg)
{
	prompt_open(ed, (Rune)arg);
}

/* '/' and '?' open the search prompt, 'n' and 'N' repeat, '*' searches for the selection */
static void
act_search(Ed *ed, int arg)
{
	if(arg == '/' || arg == '?')
		prompt_open(ed, (Rune)arg);
	else if(arg == '*')
		search_word(ed);
	else
		search_next(ed, arg == 'N' ? -1 : 1);
}

static void
act_undo(Ed *ed, int arg)
{
	ed_undo(ed, arg, count(ed));
}

static void
act_imove(Ed *ed, int arg)
{
	ed_imove(ed, arg);
}

static void
act_esc(Ed *ed, int arg)
{
	(void)arg;
	ed_to_normal(ed);
}

static void
act_erase(Ed *ed, int arg)
{
	vb_erase(ed, arg);
}

static void
act_vbinsert(Ed *ed, int arg)
{
	vb_insert(ed, arg);
}

/* Enter and Tab: the key itself is the argument of vb_type */
static void
act_type(Ed *ed, int arg)
{
	(void)arg;
	vb_type(ed, ed->key);
}

#define OBJ(c) \
	{MN | MS, {'m', 'i', c}, act_obj, 0x10000 | c}, \
	{MN | MS, {'m', 'a', c}, act_obj, c}

static Bind binds[] = {
	{MN | MS, {'i', 0, 0}, act_vbinsert, 'i'},
	{MN | MS, {'a', 0, 0}, act_vbinsert, 'a'},
	{MN | MS, {'I', 0, 0}, act_vbinsert, 'I'},
	{MN | MS, {'A', 0, 0}, act_vbinsert, 'A'},
	{MN | MS, {'o', 0, 0}, act_vbinsert, 'o'},
	{MN | MS, {'O', 0, 0}, act_vbinsert, 'O'},
	{MN | MS, {'h', 0, 0}, act_move, Mv_left},
	{MN | MS, {'j', 0, 0}, act_move, Mv_down},
	{MN | MS, {'k', 0, 0}, act_move, Mv_up},
	{MN | MS, {'l', 0, 0}, act_move, Mv_right},
	{MN | MS, {Ck_left, 0, 0}, act_move, Mv_left},
	{MN | MS, {Ck_down, 0, 0}, act_move, Mv_down},
	{MN | MS, {Ck_up, 0, 0}, act_move, Mv_up},
	{MN | MS, {Ck_right, 0, 0}, act_move, Mv_right},
	{MN | MS, {'w', 0, 0}, act_move, Mv_wordnext},
	{MN | MS, {'b', 0, 0}, act_move, Mv_wordprev},
	{MN | MS, {'e', 0, 0}, act_move, Mv_wordend},
	{MN | MS, {'W', 0, 0}, act_move, Mv_bigwordnext},
	{MN | MS, {'B', 0, 0}, act_move, Mv_bigwordprev},
	{MN | MS, {'E', 0, 0}, act_move, Mv_bigwordend},
	{MN | MS, {'H', 0, 0}, act_move, Mv_linestart},
	{MN | MS, {Ck_home, 0, 0}, act_move, Mv_linestart},
	{MN | MS, {'L', 0, 0}, act_move, Mv_lineend},
	{MN | MS, {Ck_end, 0, 0}, act_move, Mv_lineend},
	{MN | MS, {'g', 'g', 0}, act_move, Mv_filestart},
	{MN | MS, {'g', 'e', 0}, act_move, Mv_fileend},
	{MN | MS, {'G', 0, 0}, act_goline, 0},
	{MN | MS, {CTL('d'), 0, 0}, act_move, Mv_halfdown},
	{MN | MS, {CTL('u'), 0, 0}, act_move, Mv_halfup},
	{MN | MS, {CTL('f'), 0, 0}, act_move, Mv_pagedown},
	{MN | MS, {Ck_pgdn, 0, 0}, act_move, Mv_pagedown},
	{MN | MS, {CTL('b'), 0, 0}, act_move, Mv_pageup},
	{MN | MS, {Ck_pgup, 0, 0}, act_move, Mv_pageup},
	{MN | MS, {'v', 0, 0}, act_select, 0},
	{MS, {Ck_esc, 0, 0}, act_unselect, 0},
	{MN, {Ck_esc, 0, 0}, act_keepprimary, 0},
	{MN | MS, {'x', 0, 0}, act_selop, 'x'},
	{MN | MS, {'%', 0, 0}, act_selop, '%'},
	{MN | MS, {';', 0, 0}, act_selop, ';'},
	{MN | MS, {',', 0, 0}, act_selop, ','},
	OBJ('w'), OBJ('W'), OBJ('p'), OBJ('('), OBJ(')'), OBJ('b'), OBJ('{'), OBJ('}'), OBJ('B'),
	OBJ('['), OBJ(']'), OBJ('"'), OBJ('\''), OBJ('`'),
	{MN | MS, {'d', 0, 0}, act_verb, 'd'},
	{MN | MS, {'c', 0, 0}, act_verb, 'c'},
	{MN | MS, {'y', 0, 0}, act_verb, 'y'},
	{MN | MS, {'p', 0, 0}, act_verb, 'p'},
	{MN | MS, {'P', 0, 0}, act_verb, 'P'},
	{MN | MS, {'R', 0, 0}, act_verb, 'R'},
	{MN | MS, {'r', Rany, 0}, act_rchar, 0},
	{MN | MS, {'u', 0, 0}, act_undo, 0},
	{MN | MS, {'U', 0, 0}, act_undo, 1},
	{MI, {Ck_left, 0, 0}, act_imove, Mv_left},
	{MI, {Ck_right, 0, 0}, act_imove, Mv_right},
	{MI, {Ck_up, 0, 0}, act_imove, Mv_up},
	{MI, {Ck_down, 0, 0}, act_imove, Mv_down},
	{MI, {Ck_home, 0, 0}, act_imove, Mv_linestart},
	{MI, {Ck_end, 0, 0}, act_imove, Mv_lineend},
	{MI, {Ck_pgup, 0, 0}, act_imove, Mv_pageup},
	{MI, {Ck_pgdn, 0, 0}, act_imove, Mv_pagedown},
	{MN | MS, {':', 0, 0}, act_prompt, ':'},
	{MN | MS, {'/', 0, 0}, act_search, '/'},
	{MN | MS, {'n', 0, 0}, act_search, 'n'},
	{MN | MS, {'?', 0, 0}, act_search, '?'},
	{MN | MS, {'N', 0, 0}, act_search, 'N'},
	{MN | MS, {'*', 0, 0}, act_search, '*'},
	{MN | MI | MS, {CTL('s'), 0, 0}, act_save, 0},
	{MI, {Ck_esc, 0, 0}, act_esc, 0},
	{MI, {Ck_bs, 0, 0}, act_erase, 'h'},
	{MI, {CTL('w'), 0, 0}, act_erase, 'w'},
	{MI, {CTL('u'), 0, 0}, act_erase, 'u'},
	{MI, {Ck_nl, 0, 0}, act_type, 0},
	{MI, {Ck_cr, 0, 0}, act_type, 0},
	{MI, {Ck_tab, 0, 0}, act_type, 0}
};

int
keymap_key(Ed *ed, Rune r)
{
	Rune seq[4];
	int i, j, len, prefix, mode;
	Bind *bd;

	mode = ed->mode;
	ed->key = r;
	if(mode != Minsert && ed->npend == 0 &&
	   ((r >= '1' && r <= '9') || (r == '0' && ed->count > 0))){
		if(ed->count < Maxcount)
			ed->count = ed->count * 10 + (int)(r - '0');
		return 1;
	}
	len = ed->npend;
	for(i = 0; i < len; i++)
		seq[i] = ed->pend[i];
	seq[len++] = r;
	prefix = 0;
	for(i = 0; i < (int)(sizeof binds / sizeof binds[0]); i++){
		bd = &binds[i];
		if(!(bd->modes & (1 << mode)))
			continue;
		for(j = 0; j < len; j++)
			if(bd->seq[j] != Rany && bd->seq[j] != seq[j])
				break;
		if(j < len)
			continue;
		if(len >= 3 || bd->seq[len] == 0){
			ed->npend = 0;
			bd->fn(ed, bd->arg);
			ed->count = 0;
			return 1;
		}
		prefix = 1;
	}
	if(prefix && len < 3){
		ed->pend[ed->npend++] = r;
		return 1;
	}
	ed->npend = 0;
	ed->count = 0;
	return 0;
}

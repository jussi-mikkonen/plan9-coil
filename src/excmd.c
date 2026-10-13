#include "compat.h"
#include "coil.h"

/* excmd.c: ':' commands: w q wq e and a line number; any other line is a structural command (exec.c). */

enum { Maxword = 16, Maxlineno = 1 << 30 };

static void
msg1(Ed *ed, char *fmt, char *a)
{
	lsnprint(ed->msg, sizeof ed->msg, fmt, a);
}

static int
isblank_(int c)
{
	return c == ' ' || c == '\t';
}

/* all digits with blanks around: the line number (saturating), else -1 */
static int
lineno(char *s)
{
	int n, nd;

	n = 0;
	nd = 0;
	while(*s >= '0' && *s <= '9'){
		if(n > Maxlineno / 10)
			n = Maxlineno;
		else
			n = n * 10 + (*s - '0');
		if(n > Maxlineno)
			n = Maxlineno;
		nd++;
		s++;
	}
	while(isblank_(*s))
		s++;
	if(nd == 0 || *s != '\0')
		return -1;
	return n;
}

static int
ex_num(Ed *ed, char *arg)
{
	if(arg[0] == '\0')
		ed->gutter = !ed->gutter;
	else if(strcmp(arg, "on") == 0)
		ed->gutter = 1;
	else if(strcmp(arg, "off") == 0)
		ed->gutter = 0;
	else{
		msg1(ed, "usage: num [on|off]", nil);
		return -1;
	}
	msg1(ed, ed->gutter ? "line numbers on" : "line numbers off", nil);
	ed_layout(ed);
	ed_reveal(ed);
	return 0;
}

static int
ex_lang(Ed *ed, char *arg)
{
	Lang *l;

	if(arg[0] == '\0'){
		msg1(ed, "lang: %s", ed->lang->name);
		return 0;
	}
	l = lang_by_name(arg);
	if(l == nil){
		msg1(ed, "unknown language '%.40s'", arg);
		return -1;
	}
	ed->lang = l;
	ed->view.tabwidth = l->tabwidth;
	hl_reset(&ed->hl, l);
	msg1(ed, "lang: %s", l->name);
	return 0;
}

static int
ex_edit(Ed *ed, char *arg, int bang)
{
	char *path;
	int line, rc;

	if(!bang && doc_modified(&ed->doc)){
		msg1(ed, "unsaved changes; :e! discards them", nil);
		return -1;
	}
	if(arg[0] != '\0'){
		if(ed_target(ed->host, arg, &path, &line) < 0){
			msg1(ed, "%s: out of memory", arg);
			return -1;
		}
		rc = ed_load(ed, path, line);
		free(path);
		return rc;
	}
	if(ed->path == nil){
		msg1(ed, "no file name", nil);
		return -1;
	}
	line = buf_pos_to_line(ed->doc.b, ed_cursor(ed, sel_primary(&ed->doc.sel))) + 1;
	return ed_load(ed, ed->path, line);
}

int
ex_run(Ed *ed, char *line)
{
	char word[Maxword], *p, *arg, *e;
	int n, bang, w;
	Sel *s;

	p = line;
	while(isblank_(*p))
		p++;
	if(*p == '\0')
		return 0;
	if(*p >= '0' && *p <= '9'){
		n = lineno(p);
		if(n >= 0){
			if(n < 1)
				n = 1;
			s = sel_primary(&ed->doc.sel);
			sel_set1(&ed->doc.sel, s->anchor, s->head);
			mv_motion(ed, Mv_goline, n);
			return 0;
		}
	}
	w = 0;
	e = p;
	while(*e >= 'a' && *e <= 'z'){
		if(w < Maxword - 1)
			word[w] = *e;
		w++;
		e++;
	}
	if(w > Maxword - 1)
		w = Maxword - 1;
	word[w] = '\0';
	bang = *e == '!';
	if(bang)
		e++;
	/* only w, wq, q, e and lang (alone or followed by a blank) are ex commands; everything else is a structural command */
	if(!((strcmp(word, "w") == 0 || strcmp(word, "wq") == 0 || strcmp(word, "q") == 0 ||
	      strcmp(word, "e") == 0 || strcmp(word, "lang") == 0 || strcmp(word, "num") == 0) && (*e == '\0' || isblank_(*e))))
		return exec_start(ed, line);
	p = e;
	while(isblank_(*p))
		p++;
	arg = p;
	e = p + strlen(p);
	while(e > arg && isblank_(e[-1]))
		e--;
	*e = '\0';
	if(strcmp(word, "w") == 0)
		return ed_write(ed, arg[0] != '\0' ? arg : nil, bang);
	if(strcmp(word, "wq") == 0){
		if(ed_write(ed, arg[0] != '\0' ? arg : nil, bang) < 0)
			return -1;
		ed->quit = 1;
		return 0;
	}
	if(strcmp(word, "q") == 0){
		if(arg[0] != '\0'){
			msg1(ed, "q: too many arguments", nil);
			return -1;
		}
		if(bang || !doc_modified(&ed->doc)){
			ed->quit = 1;
			return 0;
		}
		msg1(ed, "unsaved changes; :q! quits", nil);
		return -1;
	}
	if(strcmp(word, "e") == 0)
		return ex_edit(ed, arg, bang);
	if(strcmp(word, "lang") == 0)
		return ex_lang(ed, arg);
	if(strcmp(word, "num") == 0)
		return ex_num(ed, arg);
	return exec_start(ed, line);
}

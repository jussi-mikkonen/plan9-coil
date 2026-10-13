#include "compat.h"
#include "coil.h"

/* prompt.c: the status-row command line. Edited only at its end; one prompt at a time. */

static int
printable(Rune r)
{
	if(r < 0x20 || r == Ck_del || r > 0x10FFFF)
		return 0;
	if(r >= 0xF000 && r <= 0xF8FF)
		return 0;
	return 1;
}

static int
slotof(Rune kind)
{
	return kind == ':' ? 0 : 1;
}

void
prompt_open(Ed *ed, Rune kind)
{
	Prompt *p;

	doc_seal(&ed->doc);
	ed->count = 0;
	ed->npend = 0;
	ed->msg[0] = '\0';
	p = &ed->prompt;
	p->on = 1;
	p->kind = kind;
	p->n = 0;
	p->text[0] = '\0';
	p->hi = -1;
}

void
prompt_free(Prompt *p)
{
	int k, i;

	for(k = 0; k < 2; k++){
		for(i = 0; i < p->nh[k]; i++)
			free(p->hist[k][i]);
		p->nh[k] = 0;
	}
}

static void
pushhist(Prompt *p, int k)
{
	char *s;
	int i;

	if(p->n == 0)
		return;
	if(p->nh[k] > 0 && strcmp(p->hist[k][p->nh[k] - 1], p->text) == 0)
		return;
	s = malloc(p->n + 1);
	if(s == nil)
		return;
	memmove(s, p->text, p->n + 1);
	if(p->nh[k] == Nhist){
		free(p->hist[k][0]);
		for(i = 1; i < Nhist; i++)
			p->hist[k][i - 1] = p->hist[k][i];
		p->nh[k]--;
	}
	p->hist[k][p->nh[k]++] = s;
}

/* start of the last rune of text[0..n) */
static int
laststart(char *s, int n)
{
	int i;

	i = n - 1;
	while(i > 0 && !utf_isstart((uchar)s[i]))
		i--;
	return i;
}

static void
submit(Ed *ed, Rune kind, char *line)
{
	if(kind == ':')
		ex_run(ed, line);
	else if(kind == '/')
		search_start(ed, line, 1);
	else if(kind == '?')
		search_start(ed, line, -1);
}

static void
setline(Prompt *p, char *s)
{
	p->n = strlen(s);
	memmove(p->text, s, p->n + 1);
}

static void
browse(Prompt *p, int up)
{
	int k;

	k = slotof(p->kind);
	if(up){
		if(p->nh[k] == 0)
			return;
		if(p->hi < 0)
			p->hi = p->nh[k] - 1;
		else if(p->hi > 0)
			p->hi--;
		setline(p, p->hist[k][p->hi]);
		return;
	}
	if(p->hi < 0)
		return;
	if(p->hi < p->nh[k] - 1){
		p->hi++;
		setline(p, p->hist[k][p->hi]);
	}else{
		p->hi = -1;
		p->n = 0;
		p->text[0] = '\0';
	}
}

int
prompt_key(Ed *ed, Rune r)
{
	Prompt *p;
	char enc[8], *line;
	int n, i, c, cls;
	Rune q;

	p = &ed->prompt;
	switch(r){
	case Ck_esc:
		p->on = 0;
		return 1;
	case Ck_nl:
	case Ck_cr:
		p->on = 0;
		pushhist(p, slotof(p->kind));
		line = malloc(p->n + 1);
		if(line == nil)
			return 1;
		memmove(line, p->text, p->n + 1);
		submit(ed, p->kind, line);
		free(line);
		return 1;
	case Ck_up:
		browse(p, 1);
		return 1;
	case Ck_down:
		browse(p, 0);
		return 1;
	case Ck_bs:
		if(p->n == 0){
			p->on = 0;
			return 1;
		}
		p->hi = -1;
		p->n = laststart(p->text, p->n);
		p->text[p->n] = '\0';
		return 1;
	case CTL('u'):
		p->hi = -1;
		p->n = 0;
		p->text[0] = '\0';
		return 1;
	case CTL('w'):
		p->hi = -1;
		while(p->n > 0 && (p->text[p->n - 1] == ' ' || p->text[p->n - 1] == '\t'))
			p->n--;
		if(p->n > 0){
			i = laststart(p->text, p->n);
			utf_dec(p->text + i, p->n - i, &q);
			cls = rune_class(q);
			p->n = i;
			while(p->n > 0){
				i = laststart(p->text, p->n);
				utf_dec(p->text + i, p->n - i, &q);
				if(rune_class(q) != cls || q == ' ' || q == '\t')
					break;
				p->n = i;
			}
		}
		p->text[p->n] = '\0';
		return 1;
	}
	if(r == Ck_tab || printable(r)){
		c = utf_enc(enc, r);
		if(p->n + c > Maxprompt){
			lsnprint(ed->msg, sizeof ed->msg, "prompt full");
			return 1;
		}
		p->hi = -1;
		n = p->n;
		memmove(p->text + n, enc, c);
		p->n += c;
		p->text[p->n] = '\0';
	}
	return 1;
}

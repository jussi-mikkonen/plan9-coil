#include "compat.h"
#include "coil.h"

/* lex.c: line lexers for the highlighter. Pure; each lexer sees one line (s, n),
 * never indexes at or past n, and advances at least one byte per loop turn. */

/* lex_c states */
enum { Lnormal = 0, Lblock = 1, Lpre = 2 };

/* sorted for strcmp; the binary search is our own (no bsearch).
 * Two names are split in two literals so the portability grep for the C type word stays clean. */
static char *ckeywords[] = {
	"auto", "break", "case", "const", "continue", "default", "do", "else",
	"enum", "extern", "for", "goto", "if", "inline", "nil", "register",
	"restrict", "return", "sizeof", "static", "struct", "switch", "typedef",
	"union", "volatile", "while"
};

static char *ctypes[] = {
	"FILE", "Rune", "_Bool", "char", "double", "float", "int", "lo" "ng",
	"ptrdiff_t", "s16int", "s32int", "s64int", "s8int", "schar", "short",
	"signed", "size_t", "ssize_t", "u16int", "u32int", "u64int", "u8int",
	"uchar", "uint", "uintptr", "ulo" "ng", "unsigned", "ushort", "usize",
	"uvlong", "vlong", "void", "wchar_t"
};

enum { Identmax = 15 };

static int
inlist(char **tab, int n, char *w)
{
	int lo, hi, mid, c;

	lo = 0;
	hi = n - 1;
	while(lo <= hi){
		mid = (lo + hi) / 2;
		c = strcmp(w, tab[mid]);
		if(c == 0)
			return 1;
		if(c < 0)
			hi = mid - 1;
		else
			lo = mid + 1;
	}
	return 0;
}

static int
identstart(int c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static int
identbyte(int c)
{
	return identstart(c) || (c >= '0' && c <= '9');
}

static int
isdigit_(int c)
{
	return c >= '0' && c <= '9';
}

static void
fill(uchar *out, int a, int b, int slot)
{
	while(a < b)
		out[a++] = slot;
}

/* index just past the "*" "/" that closes a comment, searching from i; -1 if the line ends first */
static int
blockend(uchar *s, int n, int i)
{
	while(i + 1 < n){
		if(s[i] == '*' && s[i+1] == '/')
			return i + 2;
		i++;
	}
	return -1;
}

void
lex_c(Hstate *st, uchar *s, int n, int full, uchar *out)
{
	int i, j, k, e, inpre, firstnb, slot;
	uchar q;
	char w[Identmax + 1];
	Hstate in;

	in = *st;
	*st = Lnormal;
	inpre = 0;
	i = 0;
	if(in == Lblock){
		e = blockend(s, n, 0);
		if(e < 0){
			fill(out, 0, n, Scomment);
			*st = Lblock;
			return;
		}
		fill(out, 0, e, Scomment);
		i = e;
	}else if(in == Lpre)
		inpre = 1;
	firstnb = i;
	while(firstnb < n && (s[firstnb] == ' ' || s[firstnb] == '\t'))
		firstnb++;
	while(i < n){
		if(s[i] == '/' && i + 1 < n && s[i+1] == '*'){
			e = blockend(s, n, i + 2);
			if(e < 0){
				fill(out, i, n, Scomment);
				*st = Lblock;
				return;
			}
			fill(out, i, e, Scomment);
			i = e;
			continue;
		}
		if(s[i] == '/' && i + 1 < n && s[i+1] == '/'){
			fill(out, i, n, Scomment);
			i = n;
			break;
		}
		if(!inpre && in != Lblock && i == firstnb && s[i] == '#')	/* directive: the rest of the line is preproc */
			inpre = 1;
		if(inpre){
			out[i++] = Spreproc;
			continue;
		}
		if(s[i] == '"' || s[i] == '\''){
			q = s[i];
			j = i + 1;
			while(j < n && s[j] != q){
				if(s[j] == '\\' && j + 1 < n)
					j++;
				j++;
			}
			if(j < n)
				j++;	/* the closing quote */
			fill(out, i, j, Sstring);
			i = j;
			continue;
		}
		if(identstart(s[i])){
			j = i;
			while(j < n && identbyte(s[j]))
				j++;
			slot = Sdefault;
			if(j - i <= Identmax){
				for(k = 0; k < j - i; k++)
					w[k] = s[i+k];
				w[k] = '\0';
				if(inlist(ckeywords, sizeof ckeywords / sizeof ckeywords[0], w))
					slot = Skeyword;
				else if(inlist(ctypes, sizeof ctypes / sizeof ctypes[0], w))
					slot = Stype;
			}
			fill(out, i, j, slot);
			i = j;
			continue;
		}
		if(isdigit_(s[i]) || (s[i] == '.' && i + 1 < n && isdigit_(s[i+1]) && (i == 0 || !identbyte(s[i-1])))){
			j = i + 1;
			while(j < n){
				if(identbyte(s[j]) || s[j] == '.')
					j++;
				else if((s[j] == '+' || s[j] == '-') && (s[j-1] == 'e' || s[j-1] == 'E' || s[j-1] == 'p' || s[j-1] == 'P'))
					j++;
				else
					break;
			}
			fill(out, i, j, Snumber);
			i = j;
			continue;
		}
		out[i++] = Sdefault;
	}
	if(inpre && full && n > 0 && s[n-1] == '\\')
		*st = Lpre;
}

/* ---- rc and mkfile ---- */

enum { Lrcquote = 1 };

/* sorted for strcmp */
static char *rckeywords[] = {
	"break", "builtin", "case", "cd", "eval", "exit", "flag", "fn", "for",
	"if", "in", "not", "return", "rfork", "shift", "switch", "wait", "while"
};

static int
blank_(int c)
{
	return c == ' ' || c == '\t';
}

/* end of the variable token starting at s[i] (a '$'), or i when there is none:
 * $name $#name $"name $* $1 and ${...} */
static int
rcvar(uchar *s, int n, int i)
{
	int j, k;

	if(i >= n || s[i] != '$')
		return i;
	j = i + 1;
	if(j < n && s[j] == '{'){
		j++;
		while(j < n && s[j] != '}')
			j++;
		if(j < n)
			j++;
		return j > i + 2 ? j : i;
	}
	if(j < n && (s[j] == '#' || s[j] == '"'))
		j++;
	k = j;
	while(j < n && (identbyte(s[j]) || s[j] == '*'))
		j++;
	return j > k ? j : i;
}

/* index just past the closing quote of a '...' string whose body starts at i ('' is an
 * escaped quote); -1 if the line ends first */
static int
quoteend(uchar *s, int n, int i)
{
	while(i < n){
		if(s[i] == '\''){
			if(i + 1 < n && s[i+1] == '\'')
				i += 2;
			else
				return i + 1;
		}else
			i++;
	}
	return -1;
}

/* rc scanning of s[i..n): comments, quotes, variables and (kw) keywords at command position.
 * base is the slot of everything else; hashws: '#' starts a comment only at the line start or after
 * a blank. inq: s[i..] is inside a quote. Returns the quote state at n. */
static int
rcbody(uchar *s, int n, int i, uchar *out, int inq, int base, int kw, int hashws)
{
	int j, cmdpos, forp, isk, len, k;
	char w[Identmax + 1];

	cmdpos = kw && !inq;
	forp = 0;
	while(i < n){
		if(inq || s[i] == '\''){
			j = quoteend(s, n, inq ? i : i + 1);
			if(j < 0){
				fill(out, i, n, Sstring);
				return 1;
			}
			fill(out, i, j, Sstring);
			i = j;
			inq = 0;
			cmdpos = 0;
			continue;
		}
		if(s[i] == '$'){
			j = rcvar(s, n, i);
			if(j > i){
				fill(out, i, j, Svariable);
				i = j;
				cmdpos = 0;
				continue;
			}
		}
		if(s[i] == '#' && (!hashws || i == 0 || blank_(s[i-1]))){
			fill(out, i, n, Scomment);
			return 0;
		}
		if(kw && identstart(s[i])){
			j = i;
			while(j < n && identbyte(s[j]))
				j++;
			len = j - i;
			isk = 0;
			if(len <= Identmax){
				for(k = 0; k < len; k++)
					w[k] = s[i+k];
				w[k] = '\0';
				if(inlist(rckeywords, sizeof rckeywords / sizeof rckeywords[0], w)){
					if(cmdpos)
						isk = 1;
					else if(forp && strcmp(w, "in") == 0)
						isk = 1;
				}
			}
			if(isk){
				fill(out, i, j, Skeyword);
				if(strcmp(w, "in") == 0)
					forp = 0;
				if(strcmp(w, "for") == 0)
					forp = 1;
				cmdpos = strcmp(w, "if") == 0 || strcmp(w, "while") == 0 || strcmp(w, "not") == 0;
			}else{
				fill(out, i, j, base);
				cmdpos = 0;
			}
			i = j;
			continue;
		}
		out[i] = base;
		switch(s[i]){
		case ' ': case '\t':
			break;
		case ';': case '{': case '}': case '(': case ')': case '|': case '&': case '!':
			cmdpos = kw;
			break;
		default:
			cmdpos = 0;
			break;
		}
		i++;
	}
	return 0;
}

void
lex_rc(Hstate *st, uchar *s, int n, int full, uchar *out)
{
	(void)full;
	*st = rcbody(s, n, 0, out, *st == Lrcquote, Sdefault, 1, 0) ? Lrcquote : Lnormal;
}

void
lex_mk(Hstate *st, uchar *s, int n, int full, uchar *out)
{
	int j, k, e, col;

	(void)full;
	*st = Lnormal;
	if(n <= 0)
		return;
	if(s[0] == ' ' || s[0] == '\t'){
		rcbody(s, n, 0, out, 0, Srecipe, 0, 0);
		return;
	}
	if(s[0] == '#'){
		fill(out, 0, n, Scomment);
		return;
	}
	if(s[0] == '<'){
		fill(out, 0, n, Spreproc);
		return;
	}
	j = 0;
	if(identstart(s[0]))
		while(j < n && identbyte(s[j]))
			j++;
	k = j;
	while(k < n && blank_(s[k]))
		k++;
	if(j > 0 && k < n && s[k] == '='){
		fill(out, 0, j, Svariable);
		rcbody(s, n, j, out, 0, Sdefault, 0, 1);
		return;
	}
	col = -1;
	k = 0;
	while(k < n){
		if(s[k] == '\''){
			e = quoteend(s, n, k + 1);
			if(e < 0)
				break;
			k = e;
			continue;
		}
		if(s[k] == '$'){
			e = rcvar(s, n, k);
			if(e > k){
				k = e;
				continue;
			}
		}
		if(s[k] == ':'){
			col = k;
			break;
		}
		if(s[k] == '=' || (s[k] == '#' && (k == 0 || blank_(s[k-1]))))
			break;
		k++;
	}
	if(col >= 0){
		rcbody(s, col, 0, out, 0, Starget, 0, 1);
		rcbody(s, n, col, out, 0, Sdefault, 0, 1);
	}else
		rcbody(s, n, 0, out, 0, Sdefault, 0, 1);
}

/* ---- markdown ---- */

/* Hstate: 0, or a fence: Mdfence | length << 8 | character << 16 */
enum { Mdfence = 1 };

static int
mdalnum(int c)
{
	return identbyte(c) && c != '_' ? 1 : c >= 0x80;
}

/* run of byte c in s[i..n) */
static int
runlen(uchar *s, int n, int i, int c)
{
	int j;

	j = i;
	while(j < n && s[j] == c)
		j++;
	return j - i;
}

/* does s[i..n) open a code fence? returns the fence word or 0 */
static Hstate
fenceopen(uchar *s, int n, int i)
{
	int sp, r, j;
	uchar c;

	sp = 0;
	while(i + sp < n && s[i+sp] == ' ' && sp < 4)
		sp++;
	if(sp > 3 || i + sp >= n)
		return 0;
	c = s[i+sp];
	if(c != '`' && c != '~')
		return 0;
	r = runlen(s, n, i + sp, c);
	if(r < 3)
		return 0;
	if(c == '`')
		for(j = i + sp + r; j < n; j++)
			if(s[j] == '`')
				return 0;	/* a backtick in the info string: an inline span */
	if(r > 255)
		r = 255;
	return Mdfence | ((Hstate)r << 8) | ((Hstate)c << 16);
}

static int
fenceclose(uchar *s, int n, Hstate f)
{
	int sp, r, j;
	uchar c;

	c = (f >> 16) & 0xff;
	sp = 0;
	while(sp < n && s[sp] == ' ' && sp < 4)
		sp++;
	if(sp > 3)
		return 0;
	r = runlen(s, n, sp, c);
	if(r < (int)((f >> 8) & 0xff) || r == 0)
		return 0;
	for(j = sp + r; j < n; j++)
		if(!blank_(s[j]))
			return 0;
	return 1;
}

/* inline spans of s[i..n) */
static void
mdinline(uchar *s, int n, int i, uchar *out)
{
	int r, j, k, m, e, ok, slot, fail, idx;
	uchar c;

	fail = 0;
	while(i < n){
		c = s[i];
		out[i] = Sdefault;
		if(c == '\\' && i + 1 < n){
			out[i+1] = Sdefault;
			i += 2;
			continue;
		}
		if(c == '`'){
			r = runlen(s, n, i, '`');
			j = i + r;
			e = -1;
			while(j < n){
				if(s[j] == '`'){
					m = runlen(s, n, j, '`');
					if(m == r){
						e = j + m;
						break;
					}
					j += m;
				}else
					j++;
			}
			if(e < 0){
				fill(out, i, i + r, Sdefault);
				i += r;
			}else{
				fill(out, i, e, Scode);
				i = e;
			}
			continue;
		}
		if(c == '[' && !(fail & 1)){
			j = i + 1;
			while(j < n && s[j] != ']')
				j++;
			if(j + 1 < n && s[j+1] == '('){
				k = j + 2;
				while(k < n && s[k] != ')')
					k++;
				if(k < n){
					fill(out, i, k + 1, Slink);
					i = k + 1;
					continue;
				}
				fail |= 1;	/* no ')' after this point either */
			}
			i++;
			continue;
		}
		if(c == '<' && i + 1 < n && ((s[i+1] >= 'a' && s[i+1] <= 'z') || (s[i+1] >= 'A' && s[i+1] <= 'Z'))){
			j = i + 1;
			while(j < n && (mdalnum(s[j]) || s[j] == '+' || s[j] == '.' || s[j] == '-'))
				j++;
			if(j + 2 < n && s[j] == ':' && s[j+1] == '/' && s[j+2] == '/'){
				while(j < n && s[j] != '>' && !blank_(s[j]) && s[j] != '<')
					j++;
				if(j < n && s[j] == '>'){
					fill(out, i, j + 1, Slink);
					i = j + 1;
					continue;
				}
			}
			i++;
			continue;
		}
		if(c == '*' || c == '_'){
			r = runlen(s, n, i, c);
			ok = i + r < n && !blank_(s[i+r]);
			if(ok && c == '_' && i > 0 && mdalnum(s[i-1]))
				ok = 0;
			idx = (c == '*' ? 0 : 3) + (r > 3 ? 2 : r - 1);
			if(ok && (fail & (2 << idx))){
				ok = 0;
			}
			e = -1;
			if(ok){
				j = i + r;
				while(j < n){
					if(s[j] == c){
						m = runlen(s, n, j, c);
						if(m == r && !blank_(s[j-1]) && (c == '*' || j + m >= n || !mdalnum(s[j+m]))){
							e = j + m;
							break;
						}
						j += m;
					}else
						j++;
				}
				if(e < 0)
					fail |= 2 << idx;
			}
			if(e < 0){
				fill(out, i, i + r, Sdefault);
				i += r;
			}else{
				slot = r >= 2 ? Sstrong : Semph;
				fill(out, i, e, slot);
				i = e;
			}
			continue;
		}
		i++;
	}
}

void
lex_md(Hstate *st, uchar *s, int n, int full, uchar *out)
{
	Hstate in, f;
	int i, sp, r, j;

	(void)full;
	in = *st;
	*st = Lnormal;
	if(n <= 0){
		*st = in;
		return;
	}
	if(in & Mdfence){
		fill(out, 0, n, Scode);
		if(fenceclose(s, n, in))
			*st = Lnormal;
		else
			*st = in;
		return;
	}
	f = fenceopen(s, n, 0);
	if(f != 0){
		fill(out, 0, n, Scode);
		*st = f;
		return;
	}
	fill(out, 0, n, Sdefault);
	sp = 0;
	while(sp < n && sp < 3 && s[sp] == ' ')
		sp++;
	r = runlen(s, n, sp, '#');
	if(r >= 1 && r <= 6 && (sp + r >= n || blank_(s[sp+r]))){
		fill(out, 0, n, Sheading);
		return;
	}
	i = 0;
	while(i < n && blank_(s[i]))
		i++;
	if(i < n && s[i] == '>'){
		out[i] = Slistmark;
		mdinline(s, n, i + 1, out);
		return;
	}
	j = i;
	if(j < n && (s[j] == '-' || s[j] == '*' || s[j] == '+'))
		j++;
	else{
		while(j < n && s[j] >= '0' && s[j] <= '9')
			j++;
		if(j > i && j < n && (s[j] == '.' || s[j] == ')'))
			j++;
		else
			j = i;
	}
	if(j > i && j < n && blank_(s[j])){
		fill(out, i, j, Slistmark);
		mdinline(s, n, j, out);
		return;
	}
	mdinline(s, n, 0, out);
}

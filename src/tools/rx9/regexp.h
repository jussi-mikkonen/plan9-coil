/* tools/rx9/regexp.h: the 9front <regexp.h> declarations for the Linux test build.
 * Same layout as the system header; the anonymous unions of Resub are reduced to
 * their char* member so the -std=c89 -pedantic syntax pass accepts it (rregexec,
 * the Rune* user, is not built). No pragmas, no Fmt. */
#ifndef RX9_REGEXP_H
#define RX9_REGEXP_H
enum
{
	OANY = 0,
	OBOL,
	OCLASS,
	OEOL,
	OJMP,
	ONOTNL,
	ORUNE,
	OSAVE,
	OSPLIT,
	OUNSAVE
};

typedef struct Resub Resub;
typedef struct Reinst Reinst;
typedef struct Reprog Reprog;
typedef struct Rethread Rethread;

struct Resub
{
	char *sp;
	char *ep;
};
struct Reprog
{
	Reinst *startinst;
	Rethread *threads;
	char *regstr;
	int len;
	int nthr;
};

Reprog*	regcomp(char*);
Reprog*	regcomplit(char*);
Reprog*	regcompnl(char*);
void	regerror(char*);
int	regexec(Reprog*, char*, Resub*, int);
void	regsub(char*, char*, int, Resub*, int);
#endif

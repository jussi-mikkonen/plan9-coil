/* shell.h: prototypes of the impure files (main.c, host9.c, draw9.c).
 * Includes nothing; needs u.h libc.h draw.h cell.h coil.h host.h before it. */

void	host9_init(Host*);
int	s9_snarfget(Host*, char**, Pos*, char*, int);	/* snarf9.c: 0 + malloc'd data; -1 + err */
int	s9_snarfput(Host*, char*, Pos, char*, int);	/* snarf9.c: 0; -1 + err */
int	d9_init(Cell*, char *themepath, char *msg, int nmsg);	/* embedded theme + optional theme file + style table; 0, or -1 with msg (theme error; coil continues) */
void	d9_present(Cell*, Canvas*, int scrolled, int usescroll);	/* usescroll: cellscroll the text rows by scrolled first (coil -s) */
int	d9_cells(void *cell, Rune r);		/* cellwidth((Cell*)cell, r) */
Rune	keytrans(Rune r);			/* libdraw key rune -> Ck_* (main.c) */

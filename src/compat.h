/* compat.h: the single portability seam between 9front (6c) and Linux (gcc).
 * The only file of the pure core that may include <...> headers. */
#ifdef PLAN9
#include <u.h>
#include <libc.h>
#include <regexp.h>
#define lprint print
#define lsnprint snprint
#define czalloc(n) mallocz((n), 1)
#else
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
typedef unsigned char uchar;
typedef unsigned short ushort;
typedef unsigned int u32int;
typedef unsigned int Rune;
#define nil ((void*)0)
#define lprint printf
#define lsnprint snprintf
#define czalloc(n) calloc(1, (n))
#include "regexp.h"
#endif

/* The regexp engine handle: libregexp's compiled program. */
typedef Reprog Rx;

/* Buffer positions are 32-bit ints in both worlds, capped at Maxfile. */
typedef int Pos;
enum { Maxfile = 1<<30 };

/* ASSERT: checked only in COILDEBUG builds (all Linux test builds). */
#ifdef COILDEBUG
#define ASSERT(x) do{ if(!(x)){ lprint("assert failed %s:%d\n", __FILE__, __LINE__); abort(); } }while(0)
#else
#define ASSERT(x) ((void)0)
#endif

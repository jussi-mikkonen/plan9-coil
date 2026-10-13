/* tools/rx9/u.h: Linux shim of the 9front <u.h> for the vendored libregexp only
 * (the Linux test build). Never used by a 9front build. */
#ifndef RX9_U_H
#define RX9_U_H
#include <stddef.h>
#include <setjmp.h>
#include <assert.h>
#ifndef nil
#define nil ((void*)0)
#endif
typedef unsigned char uchar;
typedef unsigned short ushort;
typedef unsigned int uint;
typedef unsigned int Rune;
#define nelem(x) ((int)(sizeof(x)/sizeof((x)[0])))
#endif

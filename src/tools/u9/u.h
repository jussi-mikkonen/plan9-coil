/* tools/u9/u.h: STUB of the 9front <u.h>, used ONLY by "make check9" so that gcc can
 * parse the Plan 9 headers under -DPLAN9. Never used by a real 9front build. */
#define nil ((void*)0)
typedef unsigned char uchar;
typedef unsigned short ushort;
typedef unsigned int uint;
typedef unsigned long ulong;
typedef signed char schar;
typedef long long vlong;
typedef unsigned long long uvlong;
typedef unsigned long uintptr;
typedef unsigned long usize;
typedef uint Rune;
typedef unsigned char u8int;
typedef unsigned short u16int;
typedef unsigned int u32int;
typedef unsigned long long u64int;
typedef int s32int;
typedef union FPdbleword FPdbleword;
union FPdbleword { double x; struct { unsigned int lo, hi; }; };
typedef long jmp_buf[2];
typedef uint mpdigit;
typedef char *va_list;
#define va_start(list, start) list = (char*)&(start)
#define va_end(list) list = 0
#define va_arg(list, mode) ((mode*)(list += sizeof(mode)))[-1]
/* 6c builtins used by libc.h macros */
#define SET(x)
#define USED(...)

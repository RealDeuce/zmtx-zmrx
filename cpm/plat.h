/* Z88DK compiler prelude for CP/M 2.2. */

#ifndef ZMODEM_CPM_PLAT_H_INCLUDED
#define ZMODEM_CPM_PLAT_H_INCLUDED

#include <stdint.h>

/* Always select the 32-bit span scanner on this memory-constrained target. */
#define ZMODEM_FORCE_32BIT_SPAN 1

/* The classic sccz80 compiler accepts restrict but not inline. */
#ifndef inline
#define inline
#endif

/* Older Z88DK releases miss these C99 declarations. */
#ifndef ERANGE
#ifdef ANGE
#define ERANGE ANGE
#else
#define ERANGE 13
#endif
#endif

#ifndef difftime
#define difftime(first,second) ((first) - (second))
#endif

#endif

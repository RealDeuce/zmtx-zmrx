/* Open Watcom Windows 95 compiler prelude. */

#ifndef ZMODEM_WIN95_PLAT_H_INCLUDED
#define ZMODEM_WIN95_PLAT_H_INCLUDED

#ifndef WINVER
#define WINVER 0x0400
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0400
#endif
#ifndef _WIN32_WINDOWS
#define _WIN32_WINDOWS 0x0400
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <stdint.h>

/* A 64-bit word scan is emulated on the 386-compatible target. */
#define ZMODEM_FORCE_32BIT_SPAN 1

#endif

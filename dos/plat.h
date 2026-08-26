/* Open Watcom 16-bit DOS compiler prelude. */

#ifndef ZMODEM_DOS_PLAT_H_INCLUDED
#define ZMODEM_DOS_PLAT_H_INCLUDED

#include <stdint.h>

/* 64-bit word scans are a loss on an 8086-class target. */
#define ZMODEM_FORCE_32BIT_SPAN 1

#endif

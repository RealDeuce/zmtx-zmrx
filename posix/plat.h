/*
 * POSIX compiler prelude.  Every translation unit includes this before any
 * system header so feature-test macros and compiler workarounds take effect.
 */

#ifndef ZMODEM_POSIX_PLAT_H_INCLUDED
#define ZMODEM_POSIX_PLAT_H_INCLUDED

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L /* NOLINT(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) */
#endif

#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64 /* NOLINT(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) */
#endif

#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 600 /* NOLINT(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) */
#endif

#endif

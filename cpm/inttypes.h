/* C99 inttypes subset required by the zmtx/zmrx frontends on Z88DK. */

#ifndef ZMODEM_CPM_INTTYPES_H_INCLUDED
#define ZMODEM_CPM_INTTYPES_H_INCLUDED

#include <stdint.h>
#include <stdlib.h>

#define PRId8 "d"
#define PRIi8 "i"
#define PRIo8 "o"
#define PRIu8 "u"
#define PRIx8 "x"
#define PRIX8 "X"

#define PRId16 "d"
#define PRIi16 "i"
#define PRIo16 "o"
#define PRIu16 "u"
#define PRIx16 "x"
#define PRIX16 "X"

#define PRId32 "ld"
#define PRIi32 "li"
#define PRIo32 "lo"
#define PRIu32 "lu"
#define PRIx32 "lx"
#define PRIX32 "lX"

#define PRIdMAX "ld"
#define PRIiMAX "li"
#define PRIoMAX "lo"
#define PRIuMAX "lu"
#define PRIxMAX "lx"
#define PRIXMAX "lX"

intmax_t zmodem_cpm_strtoimax(const char *,char **,int);
uintmax_t zmodem_cpm_strtoumax(const char *,char **,int);

#define strtoimax zmodem_cpm_strtoimax
#define strtoumax zmodem_cpm_strtoumax

#endif

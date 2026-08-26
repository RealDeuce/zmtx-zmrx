/* Compile-only implementation of the platform contract using C99 types. */

#ifndef ZMODEM_PLAT_H_INCLUDED
#define ZMODEM_PLAT_H_INCLUDED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "zmdm.h"

struct zmodem_c99_timespec {
	time_t tv_sec;
	long tv_nsec;
};

struct zmodem_c99_stat {
	long st_size;
	time_t st_mtime;
};

struct zmodem_c99_utimbuf {
	time_t actime;
	time_t modtime;
};

#define ZMODEM_PLAT_OFF_T long
#define ZMODEM_PLAT_SSIZE_T long
#define ZMODEM_PLAT_TIMESPEC struct zmodem_c99_timespec
#define ZMODEM_PLAT_STAT_T struct zmodem_c99_stat
#define ZMODEM_PLAT_UTIMBUF struct zmodem_c99_utimbuf
#define ZMODEM_PLAT_MODE_T int

#define ZMODEM_PLAT_STDIN 0
#define ZMODEM_PLAT_STDOUT 1
#define ZMODEM_PLAT_CLOCK_MONOTONIC 1
#define ZMODEM_PLAT_SEEK_CURRENT 1
#define ZMODEM_PLAT_SEEK_START 0
#define ZMODEM_PLAT_OPEN_READ_ONLY 1
#define ZMODEM_PLAT_OPEN_WRITE_ONLY 2
#define ZMODEM_PLAT_OPEN_CREATE 4
#define ZMODEM_PLAT_OPEN_EXCLUSIVE 8
#define ZMODEM_PLAT_ERROR_INTERRUPTED 1
#define ZMODEM_PLAT_ERROR_NOT_FOUND 2
#define ZMODEM_PLAT_ERROR_IO 3
#define ZMODEM_PLAT_DEFAULT_NONSTREAMING false
#define ZMODEM_PLAT_DEFAULT_JUNK_PATHNAMES false
#define ZMODEM_PLAT_REQUIRES_NONSTREAMING(io) ((void)(io),false)
#define ZMODEM_PLAT_RECEIVE_BUFFER_SIZE(io) ((void)(io),0U)
#define ZMODEM_PLAT_ESCAPE_IAC(io) ((void)(io),false)

int zmodem_c99_clock_gettime(int,struct zmodem_c99_timespec *);
int zmodem_c99_open(const char *,int,...);
int zmodem_c99_close(int);
long zmodem_c99_read(int,void *,size_t);
long zmodem_c99_lseek(int,long,int);
int zmodem_c99_fstat(int,struct zmodem_c99_stat *);
int zmodem_c99_stat_file(const char *,struct zmodem_c99_stat *);
FILE * zmodem_c99_fdopen(int,const char *);
long zmodem_c99_ftell(FILE *);
int zmodem_c99_utime(const char *,const struct zmodem_c99_utimbuf *);

#define ZMODEM_PLAT_CLOCK_GETTIME(clock_id,value) \
	zmodem_c99_clock_gettime((clock_id),(value))
#define ZMODEM_PLAT_OPEN(path,flags,mode) \
	zmodem_c99_open((path),(flags),(mode))
#define ZMODEM_PLAT_CLOSE(fd) zmodem_c99_close((fd))
#define ZMODEM_PLAT_READ(fd,buffer,length) \
	zmodem_c99_read((fd),(buffer),(length))
#define ZMODEM_PLAT_LSEEK(fd,offset,origin) \
	zmodem_c99_lseek((fd),(offset),(origin))
#define ZMODEM_PLAT_FSTAT(fd,status) zmodem_c99_fstat((fd),(status))
#define ZMODEM_PLAT_STAT_FILE(path,status) \
	zmodem_c99_stat_file((path),(status))
#define ZMODEM_PLAT_FDOPEN(fd,mode) zmodem_c99_fdopen((fd),(mode))
#define ZMODEM_PLAT_FTELLO(stream) zmodem_c99_ftell((stream))
#define ZMODEM_PLAT_FFLUSH(stream) fflush((stream))
#define ZMODEM_PLAT_UTIME(path,times) zmodem_c99_utime((path),(times))
#define ZMODEM_PLAT_STRERROR(error) ((void)(error),"platform error")

enum zmodem_plat_application {
	ZMODEM_PLAT_ZMTX,
	ZMODEM_PLAT_ZMRX
};

enum zmodem_plat_option_result {
	ZMODEM_PLAT_OPTION_INVALID = -1,
	ZMODEM_PLAT_OPTION_NOT_HANDLED = 0,
	ZMODEM_PLAT_OPTION_ACCEPTED = 1
};

struct zmodem_plat_io {
	int unused;
};

void zmodem_plat_io_init(struct zmodem_plat_io *,int,int);
int zmodem_plat_ignore_sigpipe(void);
int zmodem_plat_io_open(struct zmodem_plat_io *,const char *);
int zmodem_plat_io_make_raw(struct zmodem_plat_io *);
int zmodem_plat_io_restore(struct zmodem_plat_io *);
int zmodem_plat_io_close(struct zmodem_plat_io *);
void zmodem_plat_io_bind(struct zmodem_io *,struct zmodem_plat_io *);
enum zmodem_plat_option_result zmodem_plat_parse_option(
    struct zmodem_plat_io *,enum zmodem_plat_application,const char *,size_t *);
int zmodem_plat_post_parse(struct zmodem_plat_io *,
    enum zmodem_plat_application,int,char * const *,size_t);
void zmodem_plat_usage(enum zmodem_plat_application);

#endif

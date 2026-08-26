/* Z88DK/CP/M mappings for the ZMODEM platform contract. */

#ifndef ZMODEM_PLAT_H_INCLUDED
#define ZMODEM_PLAT_H_INCLUDED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#include "zmdm.h"

struct zmodem_cpm_timespec {
	time_t tv_sec;
	long tv_nsec;
};

struct zmodem_cpm_utimbuf {
	time_t actime;
	time_t modtime;
};

#define ZMODEM_PLAT_OFF_T long
#define ZMODEM_PLAT_SSIZE_T ssize_t
#define ZMODEM_PLAT_TIMESPEC struct zmodem_cpm_timespec
#define ZMODEM_PLAT_STAT_T struct stat
#define ZMODEM_PLAT_UTIMBUF struct zmodem_cpm_utimbuf
#define ZMODEM_PLAT_MODE_T mode_t

#define ZMODEM_PLAT_STDIN 0
#define ZMODEM_PLAT_STDOUT 1
#define ZMODEM_PLAT_CLOCK_MONOTONIC 0
#define ZMODEM_PLAT_SEEK_CURRENT SEEK_CUR
#define ZMODEM_PLAT_SEEK_START SEEK_SET
#define ZMODEM_PLAT_OPEN_READ_ONLY O_RDONLY
#define ZMODEM_PLAT_OPEN_WRITE_ONLY O_WRONLY
#define ZMODEM_PLAT_OPEN_CREATE O_CREAT
#define ZMODEM_PLAT_OPEN_EXCLUSIVE 0x4000
#define ZMODEM_PLAT_ERROR_INTERRUPTED (-32767)
#define ZMODEM_PLAT_ERROR_NOT_FOUND ESTAT
#define ZMODEM_PLAT_ERROR_IO EINVAL

#ifndef ZMODEM_CPM_STREAMING
#define ZMODEM_CPM_STREAMING 0
#endif
#if ZMODEM_CPM_STREAMING
#define ZMODEM_PLAT_DEFAULT_NONSTREAMING false
#else
#define ZMODEM_PLAT_DEFAULT_NONSTREAMING true
#endif
#define ZMODEM_PLAT_DEFAULT_JUNK_PATHNAMES true

int zmodem_cpm_clock_gettime(int,struct zmodem_cpm_timespec *);
int zmodem_cpm_open(const char *,int,mode_t);
int zmodem_cpm_close(int);
ssize_t zmodem_cpm_read(int,void *,size_t);
long zmodem_cpm_lseek(int,long,int);
int zmodem_cpm_fstat(int,struct stat *);
int zmodem_cpm_stat(const char *,struct stat *);
FILE * zmodem_cpm_fdopen(int,const char *);
long zmodem_cpm_ftell(FILE *);
int zmodem_cpm_utime(const char *,const struct zmodem_cpm_utimbuf *);
const char * zmodem_cpm_strerror(int);

#define ZMODEM_PLAT_CLOCK_GETTIME(clock_id,value) \
	zmodem_cpm_clock_gettime((clock_id),(value))
#define ZMODEM_PLAT_OPEN(path,flags,mode) \
	zmodem_cpm_open((path),(flags),(mode))
#define ZMODEM_PLAT_CLOSE(fd) zmodem_cpm_close((fd))
#define ZMODEM_PLAT_READ(fd,buffer,length) \
	zmodem_cpm_read((fd),(buffer),(length))
#define ZMODEM_PLAT_LSEEK(fd,offset,origin) \
	zmodem_cpm_lseek((fd),(offset),(origin))
#define ZMODEM_PLAT_FSTAT(fd,status) zmodem_cpm_fstat((fd),(status))
#define ZMODEM_PLAT_STAT_FILE(path,status) zmodem_cpm_stat((path),(status))
#define ZMODEM_PLAT_FDOPEN(fd,mode) zmodem_cpm_fdopen((fd),(mode))
#define ZMODEM_PLAT_FTELLO(stream) zmodem_cpm_ftell((stream))
#define ZMODEM_PLAT_FFLUSH(stream) ((void)(stream),0)
#define ZMODEM_PLAT_UTIME(path,times) zmodem_cpm_utime((path),(times))
#define ZMODEM_PLAT_STRERROR(error) zmodem_cpm_strerror((error))

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
	bool initialized;
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

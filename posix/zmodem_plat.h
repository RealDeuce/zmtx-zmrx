/* POSIX mappings for the ZMODEM platform contract. */

#ifndef ZMODEM_PLAT_H_INCLUDED
#define ZMODEM_PLAT_H_INCLUDED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <utime.h>
#include <unistd.h>

#include "zmdm.h"

/* POSIX types used by the platform-neutral application sources. */
#define ZMODEM_PLAT_OFF_T off_t
#define ZMODEM_PLAT_SSIZE_T ssize_t
#define ZMODEM_PLAT_TIMESPEC struct timespec
#define ZMODEM_PLAT_STAT_T struct stat
#define ZMODEM_PLAT_UTIMBUF struct utimbuf
#define ZMODEM_PLAT_MODE_T mode_t

/* POSIX constants used by the platform-neutral application sources. */
#define ZMODEM_PLAT_STDIN STDIN_FILENO
#define ZMODEM_PLAT_STDOUT STDOUT_FILENO
#define ZMODEM_PLAT_CLOCK_MONOTONIC CLOCK_MONOTONIC
#define ZMODEM_PLAT_SEEK_CURRENT SEEK_CUR
#define ZMODEM_PLAT_SEEK_START SEEK_SET
#define ZMODEM_PLAT_OPEN_READ_ONLY O_RDONLY
#define ZMODEM_PLAT_OPEN_WRITE_ONLY O_WRONLY
#define ZMODEM_PLAT_OPEN_CREATE O_CREAT
#define ZMODEM_PLAT_OPEN_EXCLUSIVE O_EXCL
#define ZMODEM_PLAT_ERROR_INTERRUPTED EINTR
#define ZMODEM_PLAT_ERROR_NOT_FOUND ENOENT
#define ZMODEM_PLAT_ERROR_IO EIO
#define ZMODEM_PLAT_DEFAULT_NONSTREAMING false
#define ZMODEM_PLAT_DEFAULT_JUNK_PATHNAMES false

/* Direct POSIX call mappings: these deliberately add no wrapper calls. */
#ifndef ZMODEM_PLAT_CLOCK_GETTIME
#define ZMODEM_PLAT_CLOCK_GETTIME(clock_id,value) \
	clock_gettime((clock_id),(value))
#endif
#ifndef ZMODEM_PLAT_OPEN
#define ZMODEM_PLAT_OPEN(path,flags,mode) open((path),(flags),(mode))
#endif
#ifndef ZMODEM_PLAT_CLOSE
#define ZMODEM_PLAT_CLOSE(fd) close((fd))
#endif
#ifndef ZMODEM_PLAT_READ
#define ZMODEM_PLAT_READ(fd,buffer,length) read((fd),(buffer),(length))
#endif
#ifndef ZMODEM_PLAT_LSEEK
#define ZMODEM_PLAT_LSEEK(fd,offset,origin) lseek((fd),(offset),(origin))
#endif
#ifndef ZMODEM_PLAT_FSTAT
#define ZMODEM_PLAT_FSTAT(fd,status) fstat((fd),(status))
#endif
#ifndef ZMODEM_PLAT_STAT_FILE
#define ZMODEM_PLAT_STAT_FILE(path,status) stat((path),(status))
#endif
#ifndef ZMODEM_PLAT_FDOPEN
#define ZMODEM_PLAT_FDOPEN(fd,mode) fdopen((fd),(mode))
#endif
#ifndef ZMODEM_PLAT_FTELLO
#define ZMODEM_PLAT_FTELLO(stream) ftello((stream))
#endif
#ifndef ZMODEM_PLAT_FFLUSH
#define ZMODEM_PLAT_FFLUSH(stream) fflush((stream))
#endif
#ifndef ZMODEM_PLAT_UTIME
#define ZMODEM_PLAT_UTIME(path,times) utime((path),(times))
#endif
#ifndef ZMODEM_PLAT_STRERROR
#define ZMODEM_PLAT_STRERROR(error) strerror((error))
#endif

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
	int input_fd;
	int output_fd;
	int owned_fd;
	const char * line;
	bool termios_saved;
	struct termios saved_termios;
	uint8_t output_buffer[ZMODEM_TX_BURST_CAPACITY];
	size_t output_count;
};

void zmodem_plat_io_init(struct zmodem_plat_io *, int, int);
int zmodem_plat_ignore_sigpipe(void);
int zmodem_plat_io_open(struct zmodem_plat_io *, const char *);
int zmodem_plat_io_make_raw(struct zmodem_plat_io *);
int zmodem_plat_io_restore(struct zmodem_plat_io *);
int zmodem_plat_io_close(struct zmodem_plat_io *);
void zmodem_plat_io_bind(struct zmodem_io *, struct zmodem_plat_io *);
enum zmodem_plat_option_result zmodem_plat_parse_option(
    struct zmodem_plat_io *,enum zmodem_plat_application,const char *,size_t *);
int zmodem_plat_post_parse(struct zmodem_plat_io *,
    enum zmodem_plat_application,int,char * const *,size_t);
void zmodem_plat_usage(enum zmodem_plat_application);

#endif

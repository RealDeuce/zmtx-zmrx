/* Open Watcom Windows 95 mappings for the platform contract. */

#ifndef ZMODEM_WIN95_ZMODEM_PLAT_H_INCLUDED
#define ZMODEM_WIN95_ZMODEM_PLAT_H_INCLUDED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <utime.h>
#include <windows.h>
#include <winsock.h>

#include "zmdm.h"

struct zmodem_win95_timespec {
	long tv_sec;
	long tv_nsec;
};

#define ZMODEM_PLAT_OFF_T off_t
#define ZMODEM_PLAT_SSIZE_T int
#define ZMODEM_PLAT_TIMESPEC struct zmodem_win95_timespec
#define ZMODEM_PLAT_STAT_T struct stat
#define ZMODEM_PLAT_UTIMBUF struct utimbuf
#define ZMODEM_PLAT_MODE_T mode_t

#define ZMODEM_PLAT_STDIN 0
#define ZMODEM_PLAT_STDOUT 1
#define ZMODEM_PLAT_CLOCK_MONOTONIC 0
#define ZMODEM_PLAT_SEEK_CURRENT SEEK_CUR
#define ZMODEM_PLAT_SEEK_START SEEK_SET
#define ZMODEM_PLAT_OPEN_READ_ONLY (O_RDONLY | O_BINARY)
#define ZMODEM_PLAT_OPEN_WRITE_ONLY (O_WRONLY | O_BINARY)
#define ZMODEM_PLAT_OPEN_CREATE O_CREAT
#define ZMODEM_PLAT_OPEN_EXCLUSIVE O_EXCL
#define ZMODEM_PLAT_ERROR_INTERRUPTED EINTR
#define ZMODEM_PLAT_ERROR_NOT_FOUND ENOENT
#define ZMODEM_PLAT_ERROR_IO EIO
#define ZMODEM_PLAT_DEFAULT_NONSTREAMING false
#define ZMODEM_PLAT_DEFAULT_JUNK_PATHNAMES true
#define ZMODEM_PLAT_DEFAULT_ESCAPE_8TH_BIT false
#define ZMODEM_PLAT_DEFAULT_PACK7 false

int zmodem_win95_clock_gettime(int,struct zmodem_win95_timespec *);

#define ZMODEM_PLAT_CLOCK_GETTIME(id,value) \
	zmodem_win95_clock_gettime((id),(value))
#define ZMODEM_PLAT_OPEN(path,flags,mode) open((path),(flags),(mode))
#define ZMODEM_PLAT_CLOSE(fd) close((fd))
#define ZMODEM_PLAT_READ(fd,buffer,length) read((fd),(buffer),(length))
#define ZMODEM_PLAT_LSEEK(fd,offset,origin) lseek((fd),(offset),(origin))
#define ZMODEM_PLAT_FSTAT(fd,status) fstat((fd),(status))
#define ZMODEM_PLAT_STAT_FILE(path,status) stat((path),(status))
#define ZMODEM_PLAT_FDOPEN(fd,mode) fdopen((fd),(mode))
#define ZMODEM_PLAT_FTELLO(stream) ftell((stream))
#define ZMODEM_PLAT_FFLUSH(stream) fflush((stream))
#define ZMODEM_PLAT_UTIME(path,times) utime((path),(times))
#define ZMODEM_PLAT_STRERROR(error) strerror((error))

enum zmodem_plat_application {
	ZMODEM_PLAT_ZMTX,
	ZMODEM_PLAT_ZMRX
};

enum zmodem_plat_option_result {
	ZMODEM_PLAT_OPTION_INVALID = -1,
	ZMODEM_PLAT_OPTION_NOT_HANDLED = 0,
	ZMODEM_PLAT_OPTION_ACCEPTED = 1
};

enum zmodem_win95_transport {
	ZMODEM_WIN95_NONE,
	ZMODEM_WIN95_COM,
	ZMODEM_WIN95_SOCKET
};

struct zmodem_plat_io {
	enum zmodem_win95_transport transport;
	HANDLE comm_handle;
	SOCKET socket_handle;
	bool escape_iac;
	bool winsock_started;
	bool timeouts_saved;
	COMMTIMEOUTS saved_timeouts;
	uint8_t output_buffer[ZMODEM_TX_BURST_CAPACITY];
	size_t output_count;
};

#define ZMODEM_PLAT_REQUIRES_NONSTREAMING(io) ((void)(io),false)
#define ZMODEM_PLAT_RECEIVE_BUFFER_SIZE(io) ((void)(io),0U)
#define ZMODEM_PLAT_ESCAPE_IAC(io) ((io)->escape_iac)

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

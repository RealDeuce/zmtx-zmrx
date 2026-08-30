/* CP/M 2.2 file, transport, and frontend adapter for Z88DK. */

#include "plat.h"
#include "zmodem_plat.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "zmdm.h"
#include "zmodem_cpm_driver.h"

/*
 * Z88DK's CP/M file descriptor is an FCB address stored in an int.  Once the
 * program grows past 32 KiB, a valid address can therefore compare negative
 * on a 16-bit implementation.  Expose a small POSIX-like descriptor to the
 * frontend and retain the native bit pattern only inside this adapter.
 */
#define ZMODEM_CPM_FILE_FD 3
static int tracked_native_fd = -1;
static const char * tracked_path;

static void
terminate_command_tail(void)
{
	volatile uint8_t * command_tail = (volatile uint8_t *)0x0080;
	uint8_t length = command_tail[0];

	/* The classic CP/M CRT does not terminate its final argv string. */
	if (length < UINT8_C(127)) {
		command_tail[(size_t)length + 1U] = 0U;
	}
}

intmax_t
zmodem_cpm_strtoimax(const char * text,char ** end,int base)
{
	return (intmax_t)strtol((char *)text,end,base);
}

uintmax_t
zmodem_cpm_strtoumax(const char * text,char ** end,int base)
{
	return (uintmax_t)strtoul((char *)text,end,base);
}

int
zmodem_cpm_clock_gettime(int clock_id,struct zmodem_cpm_timespec * value)
{
	(void)clock_id;
	(void)value;
	return -1;
}

int
zmodem_cpm_stat(const char * path,struct stat * status)
{
	int result = stat((char *)path,status);

	if (result != 0 && errno == 0) {
		errno = ZMODEM_PLAT_ERROR_NOT_FOUND;
	}
	return result;
}

int
zmodem_cpm_open(const char * path,int flags,mode_t mode)
{
	int native_flags = flags & ~ZMODEM_PLAT_OPEN_EXCLUSIVE;
	int fd;

	if ((flags & ZMODEM_PLAT_OPEN_EXCLUSIVE) != 0) {
		struct stat status;

		if (zmodem_cpm_stat(path,&status) == 0) {
			errno = EACCES;
			return -1;
		}
	}
	errno = 0;
	fd = open(path,native_flags,mode);
	if (fd != -1) {
		tracked_native_fd = fd;
		tracked_path = path;
		return ZMODEM_CPM_FILE_FD;
	}
	return -1;
}

int
zmodem_cpm_close(int fd)
{
	int result;

	if (fd != ZMODEM_CPM_FILE_FD || tracked_native_fd == -1) {
		errno = EBADF;
		return -1;
	}
	result = close(tracked_native_fd);
	tracked_native_fd = -1;
	tracked_path = NULL;
	return result;
}

ssize_t
zmodem_cpm_read(int fd,void * buffer,size_t length)
{
	if (fd != ZMODEM_CPM_FILE_FD || tracked_native_fd == -1) {
		errno = EBADF;
		return -1;
	}
	return read(tracked_native_fd,buffer,length);
}

long
zmodem_cpm_lseek(int fd,long offset,int origin)
{
	if (fd != ZMODEM_CPM_FILE_FD || tracked_native_fd == -1) {
		errno = EBADF;
		return -1L;
	}
	return lseek(tracked_native_fd,offset,origin);
}

int
zmodem_cpm_fstat(int fd,struct stat * status)
{
	if (fd != ZMODEM_CPM_FILE_FD || tracked_native_fd == -1 ||
	    tracked_path == NULL) {
		errno = EBADF;
		return -1;
	}
	return zmodem_cpm_stat(tracked_path,status);
}

FILE *
zmodem_cpm_fdopen(int fd,const char * mode)
{
	if (fd != ZMODEM_CPM_FILE_FD || tracked_native_fd == -1) {
		errno = EBADF;
		return NULL;
	}
	return fdopen(tracked_native_fd,mode);
}

long
zmodem_cpm_ftell(FILE * stream)
{
	return (long)ftell(stream);
}

int
zmodem_cpm_utime(const char * path,const struct zmodem_cpm_utimbuf * times)
{
	(void)path;
	(void)times;
	return 0;
}

const char *
zmodem_cpm_strerror(int error)
{
	switch (error) {
		case EACCES:
			return "access denied";
		case EBADF:
			return "bad file descriptor";
		case EFBIG:
			return "file too large";
		case EINVAL:
			return "invalid argument";
		case EMFILE:
		case ENFILE:
			return "too many open files";
		case ENOMEM:
			return "out of memory";
		case ESTAT:
			return "file not found";
		default:
			return "CP/M error";
	}
}

void
zmodem_plat_io_init(struct zmodem_plat_io * io,int input,int output)
{
	(void)input;
	(void)output;
	terminate_command_tail();
	io->initialized = false;
}

int
zmodem_plat_ignore_sigpipe(void)
{
	return 0;
}

int
zmodem_plat_io_open(struct zmodem_plat_io * io,const char * path)
{
	(void)io;
	(void)path;
	return -1;
}

int
zmodem_plat_io_make_raw(struct zmodem_plat_io * io)
{
	(void)io;
	return 0;
}

int
zmodem_plat_io_restore(struct zmodem_plat_io * io)
{
	(void)io;
	return 0;
}

int
zmodem_plat_io_close(struct zmodem_plat_io * io)
{
	int result = 0;

	if (io->initialized) {
		result = zmodem_cpm_driver_close();
		io->initialized = false;
	}
	return result;
}

void
zmodem_plat_io_bind(struct zmodem_io * interface,struct zmodem_plat_io * io)
{
	interface->context = io;
	interface->read = zmodem_cpm_driver_read;
	interface->write = zmodem_cpm_driver_write;
	interface->flush = zmodem_cpm_driver_flush;
	interface->poll = zmodem_cpm_driver_poll;
	interface->purge = zmodem_cpm_driver_purge;
}

enum zmodem_plat_option_result
zmodem_plat_parse_option(struct zmodem_plat_io * io,
    enum zmodem_plat_application application,const char * argument,
    size_t * option_index)
{
	(void)io;
	(void)application;
	(void)argument;
	(void)option_index;
	return ZMODEM_PLAT_OPTION_NOT_HANDLED;
}

int
zmodem_plat_post_parse(struct zmodem_plat_io * io,
    enum zmodem_plat_application application,int argc,char * const * argv,
    size_t first_operand)
{
	(void)application;
	(void)argc;
	(void)argv;
	(void)first_operand;
	if (zmodem_cpm_driver_init() != 0) {
		return 2;
	}
	io->initialized = true;
	return 0;
}

void
zmodem_plat_usage(enum zmodem_plat_application application)
{
	(void)application;
	(void)printf("\tCP/M I/O uses the RDR: and PUN: devices\n");
}

#define _XOPEN_SOURCE 600

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include "zmdm.h"
#include "zmdm_posix.h"

static int wrapped_tcsetattr(int, int, const struct termios *);
static ssize_t wrapped_write(int, const void *, size_t);
static int wrapped_close(int);

#define ZMODEM_POSIX_TCSETATTR wrapped_tcsetattr
#define ZMODEM_POSIX_WRITE wrapped_write
#define ZMODEM_POSIX_CLOSE wrapped_close
#include "../zmdm_posix.c"
#undef ZMODEM_POSIX_CLOSE
#undef ZMODEM_POSIX_WRITE
#undef ZMODEM_POSIX_TCSETATTR

static int tcsetattr_failures;
static int tcsetattr_calls;
static int failed_write_fd = -1;
static size_t partial_write_length;
static int write_calls;
static int failed_close_fd = -1;
static int close_calls;

static int
wrapped_tcsetattr(int fd,int action,const struct termios * attributes)
{
	tcsetattr_calls += 1;
	if (tcsetattr_failures > 0) {
		tcsetattr_failures -= 1;
		errno = EIO;
		return -1;
	}
	return tcsetattr(fd,action,attributes);
}

static ssize_t
wrapped_write(int fd,const void * buffer,size_t length)
{
	write_calls += 1;
	if (fd == failed_write_fd) {
		if (partial_write_length > 0U) {
			size_t result = (length < partial_write_length) ? length :
			    partial_write_length;

			partial_write_length = 0U;
			return (ssize_t)result;
		}
		errno = EIO;
		return -1;
	}
	return write(fd,buffer,length);
}

static int
wrapped_close(int fd)
{
	close_calls += 1;
	if (fd == failed_close_fd) {
		errno = EIO;
		return -1;
	}
	return close(fd);
}

static bool
expect_cleanup(bool condition,const char * description)
{
	if (!condition) {
		(void)fprintf(stderr,"test_posix_cleanup: %s\n",description);
	}
	return condition;
}

static bool
open_terminal(int * master_fd,int * slave_fd)
{
	char * slave_name;

	*master_fd = posix_openpt(O_RDWR | O_NOCTTY);
	if (*master_fd < 0) {
		return false;
	}
	if (grantpt(*master_fd) != 0 || unlockpt(*master_fd) != 0) {
		(void)close(*master_fd);
		return false;
	}
	slave_name = ptsname(*master_fd);
	if (slave_name == NULL) {
		(void)close(*master_fd);
		return false;
	}
	*slave_fd = open(slave_name,O_RDWR | O_NOCTTY);
	if (*slave_fd < 0) {
		(void)close(*master_fd);
		return false;
	}
	return true;
}

static bool
test_failed_raw_setup_is_restored(void)
{
	struct zmodem_posix_io io;
	int master_fd;
	int slave_fd;
	bool passed = true;

	if (!open_terminal(&master_fd,&slave_fd)) {
		return expect_cleanup(false,"open raw-setup terminal");
	}
	zmodem_posix_io_init(&io,slave_fd,slave_fd);
	tcsetattr_calls = 0;
	tcsetattr_failures = 1;
	passed = expect_cleanup(zmodem_posix_io_make_raw(&io) != 0,
	    "inject raw setup failure") && passed;
	passed = expect_cleanup(io.termios_saved,
	    "retain state after raw setup failure") && passed;
	passed = expect_cleanup(zmodem_posix_io_restore(&io) == 0,
	    "restore after raw setup failure") && passed;
	passed = expect_cleanup(tcsetattr_calls == 2,
	    "attempt raw setup and restoration") && passed;
	passed = expect_cleanup(!io.termios_saved,
	    "clear successfully restored state") && passed;
	passed = expect_cleanup(close(slave_fd) == 0,
	    "close raw-setup terminal slave") && passed;
	passed = expect_cleanup(close(master_fd) == 0,
	    "close raw-setup terminal master") && passed;
	return passed;
}

static bool
test_failed_restore_is_retryable(void)
{
	struct zmodem_posix_io io;
	int master_fd;
	int slave_fd;
	bool passed = true;

	if (!open_terminal(&master_fd,&slave_fd)) {
		return expect_cleanup(false,"open restore terminal");
	}
	zmodem_posix_io_init(&io,slave_fd,slave_fd);
	tcsetattr_failures = 0;
	passed = expect_cleanup(zmodem_posix_io_make_raw(&io) == 0,
	    "configure restore terminal") && passed;
	tcsetattr_failures = 1;
	passed = expect_cleanup(zmodem_posix_io_restore(&io) != 0,
	    "report restore failure") && passed;
	passed = expect_cleanup(io.termios_saved,
	    "retain state after restore failure") && passed;
	passed = expect_cleanup(zmodem_posix_io_restore(&io) == 0,
	    "retry terminal restoration") && passed;
	passed = expect_cleanup(!io.termios_saved,
	    "clear state after restore retry") && passed;
	passed = expect_cleanup(close(slave_fd) == 0,
	    "close restore terminal slave") && passed;
	passed = expect_cleanup(close(master_fd) == 0,
	    "close restore terminal master") && passed;
	return passed;
}

static bool
test_close_attempts_flush_and_close(void)
{
	struct zmodem_posix_io io;
	int owned_fd;
	bool passed = true;

	zmodem_posix_io_init(&io,-1,-1);
	if (zmodem_posix_io_open(&io,"/dev/null") != 0) {
		return expect_cleanup(false,"open cleanup descriptor");
	}
	owned_fd = io.owned_fd;
	io.output_buffer[0] = UINT8_C(0);
	io.output_buffer[1] = UINT8_C(1);
	io.output_count = 2U;
	failed_write_fd = owned_fd;
	partial_write_length = 1U;
	failed_close_fd = owned_fd;
	write_calls = 0;
	close_calls = 0;
	passed = expect_cleanup(zmodem_posix_io_close(&io) != 0,
	    "report flush and close failures") && passed;
	passed = expect_cleanup(write_calls == 2,"attempt complete pending flush") &&
	    passed;
	passed = expect_cleanup(close_calls == 1,"attempt owned close") && passed;
	passed = expect_cleanup(io.output_count == 1U,
	    "retain unflushed output") && passed;
	passed = expect_cleanup(io.owned_fd == -1,
	    "release failed close ownership") && passed;
	failed_write_fd = -1;
	partial_write_length = 0U;
	failed_close_fd = -1;
	passed = expect_cleanup(close(owned_fd) == 0,
	    "close injected-failure descriptor") && passed;
	return passed;
}

int
main(void)
{
	bool passed = true;

	passed = test_failed_raw_setup_is_restored() && passed;
	passed = test_failed_restore_is_retryable() && passed;
	passed = test_close_attempts_flush_and_close() && passed;
	return passed ? 0 : 1;
}

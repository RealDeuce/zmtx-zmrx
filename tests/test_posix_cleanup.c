#include "plat.h"

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
#include <time.h>
#include <unistd.h>

#include "zmdm.h"
#include "zmodem_plat.h"

static int wrapped_tcsetattr(int, int, const struct termios *);
static ssize_t wrapped_write(int, const void *, size_t);
static int wrapped_close(int);
static int wrapped_clock_gettime(clockid_t, struct timespec *);
static int wrapped_select(int, fd_set *, fd_set *, fd_set *, struct timeval *);

#undef ZMODEM_PLAT_TCSETATTR
#undef ZMODEM_PLAT_WRITE
#undef ZMODEM_PLAT_CLOSE
#undef ZMODEM_PLAT_CLOCK_GETTIME
#define ZMODEM_PLAT_TCSETATTR(fd,action,attributes) \
	wrapped_tcsetattr((fd),(action),(attributes))
#define ZMODEM_PLAT_WRITE(fd,buffer,length) \
	wrapped_write((fd),(buffer),(length))
#define ZMODEM_PLAT_CLOSE(fd) wrapped_close((fd))
#define ZMODEM_PLAT_CLOCK_GETTIME(clock_id,value) \
	wrapped_clock_gettime((clock_id),(value))
#define ZMODEM_PLAT_SELECT(nfds,readfds,writefds,errorfds,timeout) \
	wrapped_select((nfds),(readfds),(writefds),(errorfds),(timeout))
#include "../posix/zmodem_plat.c"
#undef ZMODEM_PLAT_SELECT
#undef ZMODEM_PLAT_CLOCK_GETTIME
#undef ZMODEM_PLAT_CLOSE
#undef ZMODEM_PLAT_WRITE
#undef ZMODEM_PLAT_TCSETATTR

static int tcsetattr_failures;
static int tcsetattr_calls;
static int failed_write_fd = -1;
static size_t partial_write_length;
static int write_calls;
static int failed_close_fd = -1;
static int close_calls;
static bool use_fake_clock;
static int clock_gettime_failures;
static struct timespec fake_clock_value;
static bool use_fake_select;
static int fake_select_interruptions;
static int fake_select_calls;
static struct timeval fake_select_timeouts[4];

static int
wrapped_clock_gettime(clockid_t clock_id,struct timespec * value)
{
	if (clock_gettime_failures > 0) {
		clock_gettime_failures -= 1;
		errno = EIO;
		return -1;
	}
	if (use_fake_clock) {
		*value = fake_clock_value;
		return 0;
	}
	return clock_gettime(clock_id,value);
}

static int
wrapped_select(int descriptor_count,fd_set * read_descriptors,
    fd_set * write_descriptors,fd_set * error_descriptors,
    struct timeval * timeout)
{
	if (use_fake_select) {
		if (fake_select_calls < 4) {
			fake_select_timeouts[fake_select_calls] = *timeout;
		}
		fake_select_calls += 1;
		fake_clock_value.tv_nsec += 50000000L;
		if (fake_clock_value.tv_nsec >= 1000000000L) {
			fake_clock_value.tv_sec += (time_t)1;
			fake_clock_value.tv_nsec -= 1000000000L;
		}
		if (fake_select_interruptions > 0) {
			fake_select_interruptions -= 1;
			errno = EINTR;
			return -1;
		}
		return 0;
	}
	return select(descriptor_count,read_descriptors,write_descriptors,
	    error_descriptors,timeout);
}

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
	struct zmodem_plat_io io;
	int master_fd;
	int slave_fd;
	bool passed = true;

	if (!open_terminal(&master_fd,&slave_fd)) {
		return expect_cleanup(false,"open raw-setup terminal");
	}
	zmodem_plat_io_init(&io,slave_fd,slave_fd);
	tcsetattr_calls = 0;
	tcsetattr_failures = 1;
	passed = expect_cleanup(zmodem_plat_io_make_raw(&io) != 0,
	    "inject raw setup failure") && passed;
	passed = expect_cleanup(io.termios_saved,
	    "retain state after raw setup failure") && passed;
	passed = expect_cleanup(zmodem_plat_io_restore(&io) == 0,
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
	struct zmodem_plat_io io;
	int master_fd;
	int slave_fd;
	bool passed = true;

	if (!open_terminal(&master_fd,&slave_fd)) {
		return expect_cleanup(false,"open restore terminal");
	}
	zmodem_plat_io_init(&io,slave_fd,slave_fd);
	tcsetattr_failures = 0;
	passed = expect_cleanup(zmodem_plat_io_make_raw(&io) == 0,
	    "configure restore terminal") && passed;
	tcsetattr_failures = 1;
	passed = expect_cleanup(zmodem_plat_io_restore(&io) != 0,
	    "report restore failure") && passed;
	passed = expect_cleanup(io.termios_saved,
	    "retain state after restore failure") && passed;
	passed = expect_cleanup(zmodem_plat_io_restore(&io) == 0,
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
	struct zmodem_plat_io io;
	int owned_fd;
	bool passed = true;

	zmodem_plat_io_init(&io,-1,-1);
	if (zmodem_plat_io_open(&io,"/dev/null") != 0) {
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
	passed = expect_cleanup(zmodem_plat_io_close(&io) != 0,
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

static bool
test_input_deadlines(void)
{
	struct timespec deadline;
	struct timeval remaining;
	bool passed = true;

	use_fake_clock = true;
	clock_gettime_failures = 0;
	fake_clock_value.tv_sec = (time_t)10;
	fake_clock_value.tv_nsec = 100000000L;
	passed = expect_cleanup(set_input_deadline(&deadline,200) == 0,
	    "set input deadline") && passed;
	passed = expect_cleanup(deadline.tv_sec == (time_t)10,
	    "deadline second without rollover") && passed;
	passed = expect_cleanup(deadline.tv_nsec == 300000000L,
	    "deadline nanoseconds without rollover") && passed;
	fake_clock_value.tv_nsec = 900000000L;
	passed = expect_cleanup(set_input_deadline(&deadline,200) == 0,
	    "set rollover input deadline") && passed;
	passed = expect_cleanup(deadline.tv_sec == (time_t)11,
	    "deadline second rollover") && passed;
	passed = expect_cleanup(deadline.tv_nsec == 100000000L,
	    "deadline nanoseconds rollover") && passed;

	fake_clock_value.tv_sec = (time_t)10;
	fake_clock_value.tv_nsec = 200000000L;
	passed = expect_cleanup(input_time_remaining(&deadline,&remaining) == 1,
	    "calculate borrowed remaining time") && passed;
	passed = expect_cleanup(remaining.tv_sec == (time_t)0,
	    "borrow remaining second") && passed;
	passed = expect_cleanup(remaining.tv_usec == (suseconds_t)900000,
	    "borrow remaining microseconds") && passed;
	fake_clock_value.tv_nsec = 50000000L;
	passed = expect_cleanup(input_time_remaining(&deadline,&remaining) == 1,
	    "calculate unborrowed remaining time") && passed;
	passed = expect_cleanup(remaining.tv_sec == (time_t)1,
	    "retain whole remaining second") && passed;
	passed = expect_cleanup(remaining.tv_usec == (suseconds_t)50000,
	    "retain remaining microseconds") && passed;
	fake_clock_value.tv_nsec = 100000500L;
	passed = expect_cleanup(input_time_remaining(&deadline,&remaining) == 1,
	    "round remaining time upward") && passed;
	passed = expect_cleanup(remaining.tv_sec == (time_t)1,
	    "normalize rounded second") && passed;
	passed = expect_cleanup(remaining.tv_usec == (suseconds_t)0,
	    "normalize rounded microseconds") && passed;

	fake_clock_value.tv_sec = deadline.tv_sec;
	fake_clock_value.tv_nsec = 50000000L;
	passed = expect_cleanup(input_time_remaining(&deadline,&remaining) == 1,
	    "retain time within deadline second") && passed;
	passed = expect_cleanup(remaining.tv_sec == (time_t)0,
	    "deadline-second remaining seconds") && passed;
	passed = expect_cleanup(remaining.tv_usec == (suseconds_t)50000,
	    "deadline-second remaining microseconds") && passed;
	fake_clock_value.tv_sec = deadline.tv_sec;
	fake_clock_value.tv_nsec = deadline.tv_nsec;
	passed = expect_cleanup(input_time_remaining(&deadline,&remaining) == 0,
	    "expire at input deadline") && passed;
	passed = expect_cleanup(remaining.tv_sec == (time_t)0,
	    "clear expired timeout seconds") && passed;
	passed = expect_cleanup(remaining.tv_usec == (suseconds_t)0,
	    "clear expired timeout microseconds") && passed;
	fake_clock_value.tv_sec += (time_t)1;
	passed = expect_cleanup(input_time_remaining(&deadline,&remaining) == 0,
	    "expire after input deadline") && passed;

	clock_gettime_failures = 1;
	passed = expect_cleanup(set_input_deadline(&deadline,200) < 0,
	    "report deadline clock failure") && passed;
	clock_gettime_failures = 1;
	passed = expect_cleanup(input_time_remaining(&deadline,&remaining) < 0,
	    "report remaining-time clock failure") && passed;
	clock_gettime_failures = 1;
	passed = expect_cleanup(wait_for_input(STDIN_FILENO,200) == ZMODEM_IO_ERROR,
	    "propagate wait clock failure") && passed;
	fake_clock_value.tv_sec = (time_t)10;
	fake_clock_value.tv_nsec = 0L;
	fake_select_interruptions = 4;
	fake_select_calls = 0;
	use_fake_select = true;
	clock_gettime_failures = 0;
	passed = expect_cleanup(wait_for_input(STDIN_FILENO,200) == 0,
	    "expire interrupted wait at original deadline") && passed;
	passed = expect_cleanup(fake_select_calls == 4,
	    "retry each interrupted select") && passed;
	passed = expect_cleanup(fake_select_timeouts[0].tv_sec == (time_t)0,
	    "start interrupted select within current second") && passed;
	passed = expect_cleanup(
	    fake_select_timeouts[0].tv_usec == (suseconds_t)200000,
	    "start interrupted select with full timeout") && passed;
	passed = expect_cleanup(fake_select_timeouts[3].tv_sec == (time_t)0,
	    "finish interrupted select within current second") && passed;
	passed = expect_cleanup(
	    fake_select_timeouts[3].tv_usec == (suseconds_t)50000,
	    "reduce timeout after repeated interruptions") && passed;
	use_fake_select = false;
	use_fake_clock = false;
	clock_gettime_failures = 0;
	return passed;
}

int
main(void)
{
	bool passed = true;

	passed = test_failed_raw_setup_is_restored() && passed;
	passed = test_failed_restore_is_retryable() && passed;
	passed = test_close_attempts_flush_and_close() && passed;
	passed = test_input_deadlines() && passed;
	return passed ? 0 : 1;
}

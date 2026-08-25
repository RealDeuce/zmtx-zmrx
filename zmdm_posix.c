/*
 * POSIX transport adapter for the ZMODEM protocol engine.
 *
 * Copyright (c) 2026 Stephen Hurd
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Special thanks to Jacques Mattheij, formerly of Mattheij Computer Service,
 * and original author of zmtx/zmrx.
 */

#include "zmdm_posix.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#include "zmdm.h"

#ifndef ZMODEM_POSIX_CLOSE
#define ZMODEM_POSIX_CLOSE close
#endif
#ifndef ZMODEM_POSIX_CLOCK_GETTIME
#define ZMODEM_POSIX_CLOCK_GETTIME clock_gettime
#endif
#ifndef ZMODEM_POSIX_TCSETATTR
#define ZMODEM_POSIX_TCSETATTR tcsetattr
#endif
#ifndef ZMODEM_POSIX_WRITE
#define ZMODEM_POSIX_WRITE write
#endif

static int
set_input_deadline(struct timespec * deadline,int timeout_ms)
{
	if (ZMODEM_POSIX_CLOCK_GETTIME(CLOCK_MONOTONIC,deadline) != 0) {
		return -1;
	}
	deadline->tv_sec += (time_t)(timeout_ms / 1000);
	deadline->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
	if (deadline->tv_nsec >= 1000000000L) {
		deadline->tv_sec += (time_t)1;
		deadline->tv_nsec -= 1000000000L;
	}
	return 0;
}

static int
input_time_remaining(const struct timespec * deadline,struct timeval * timeout)
{
	struct timespec now;
	time_t seconds;
	long nanoseconds;

	if (ZMODEM_POSIX_CLOCK_GETTIME(CLOCK_MONOTONIC,&now) != 0) {
		return -1;
	}
	if ((now.tv_sec > deadline->tv_sec) ||
	    ((now.tv_sec == deadline->tv_sec) &&
	    (now.tv_nsec >= deadline->tv_nsec))) {
		timeout->tv_sec = 0;
		timeout->tv_usec = 0;
		return 0;
	}
	seconds = deadline->tv_sec - now.tv_sec;
	if (deadline->tv_nsec < now.tv_nsec) {
		seconds -= (time_t)1;
		nanoseconds = deadline->tv_nsec + 1000000000L - now.tv_nsec;
	}
	else {
		nanoseconds = deadline->tv_nsec - now.tv_nsec;
	}
	timeout->tv_sec = seconds;
	timeout->tv_usec = (suseconds_t)((nanoseconds + 999L) / 1000L);
	if (timeout->tv_usec >= (suseconds_t)1000000) {
		timeout->tv_sec += (time_t)1;
		timeout->tv_usec -= (suseconds_t)1000000;
	}
	return 1;
}

static int
wait_for_input(int fd,int timeout_ms)
{
	int result;
	fd_set read_set;
	struct timeval timeout;
	struct timespec deadline;
	bool timed_wait;

	if ((fd < 0) || (fd >= FD_SETSIZE)) {
		return ZMODEM_IO_ERROR;
	}
	timed_wait = timeout_ms > 0;
	if (timed_wait) {
		if (set_input_deadline(&deadline,timeout_ms) != 0) {
			return ZMODEM_IO_ERROR;
		}
	}
	for (;;) {
		if (timed_wait) {
			result = input_time_remaining(&deadline,&timeout);
			if (result <= 0) {
				return result;
			}
		}
		else {
			timeout.tv_sec = 0;
			timeout.tv_usec = 0;
		}
		FD_ZERO(&read_set);
		FD_SET(fd,&read_set);
		result = select(fd + 1,&read_set,NULL,NULL,&timeout);
		if (result >= 0) {
			break;
		}
		if (errno != EINTR) {
			break;
		}
	}

	return result;
}

static int
posix_read(void * context,uint8_t * restrict buffer,size_t capacity,
    size_t * restrict count,int timeout_ms)
{
	struct zmodem_posix_io * io = context;
	ssize_t result;
	int ready;

	*count = 0U;
	ready = wait_for_input(io->input_fd,timeout_ms);
	if (ready == 0) {
		return ZMODEM_TIMEOUT;
	}
	if (ready < 0) {
		return ZMODEM_IO_ERROR;
	}
	for (;;) {
		result = read(io->input_fd,buffer,capacity);
		if (result >= 0) {
			break;
		}
		if (errno != EINTR) {
			break;
		}
	}
	if (result <= 0) {
		return (result == 0) ? ZMODEM_TIMEOUT : ZMODEM_IO_ERROR;
	}
	*count = (size_t)result;
	return ZMODEM_OK;
}

static int
posix_flush(void * context)
{
	struct zmodem_posix_io * io = context;
	size_t offset = 0U;

	while (offset < io->output_count) {
		ssize_t result;

		for (;;) {
			result = ZMODEM_POSIX_WRITE(io->output_fd,
			    &io->output_buffer[offset],
			    io->output_count - offset);
			if (result >= 0) {
				break;
			}
			if (errno != EINTR) {
				break;
			}
		}
		if (result <= 0) {
			if (offset > 0U) {
				io->output_count -= offset;
				(void)memmove(io->output_buffer,
				    &io->output_buffer[offset],io->output_count);
			}
			return ZMODEM_IO_ERROR;
		}
		offset += (size_t)result;
	}
	io->output_count = 0U;
	return ZMODEM_OK;
}

static int
posix_write(void * context,const uint8_t * restrict buffer,size_t length)
{
	struct zmodem_posix_io * io = context;

	while (length > 0U) {
		size_t available = sizeof(io->output_buffer) - io->output_count;
		size_t copied = (length < available) ? length : available;

		(void)memcpy(&io->output_buffer[io->output_count],buffer,copied);
		io->output_count += copied;
		buffer += copied;
		length -= copied;
		if ((io->output_count == sizeof(io->output_buffer)) &&
		    (posix_flush(io) != ZMODEM_OK)) {
			return ZMODEM_IO_ERROR;
		}
	}
	return ZMODEM_OK;
}

static int
posix_poll(void * context)
{
	struct zmodem_posix_io * io = context;
	int result = wait_for_input(io->input_fd,0);

	return (result < 0) ? ZMODEM_IO_ERROR : result;
}

static int
posix_purge(void * context)
{
	struct zmodem_posix_io * io = context;
	uint8_t byte;

	for (;;) {
		ssize_t result;
		int ready = wait_for_input(io->input_fd,0);

		if (ready == 0) {
			return ZMODEM_OK;
		}
		if (ready < 0) {
			return ZMODEM_IO_ERROR;
		}
		for (;;) {
			result = read(io->input_fd,&byte,1U);
			if (result >= 0) {
				break;
			}
			if (errno != EINTR) {
				break;
			}
		}
		if (result == 0) {
			return ZMODEM_OK;
		}
		if (result < 0) {
			return ZMODEM_IO_ERROR;
		}
	}
}

void
zmodem_posix_io_init(struct zmodem_posix_io * io,int input_fd,int output_fd)
{
	io->input_fd = input_fd;
	io->output_fd = output_fd;
	io->owned_fd = -1;
	io->termios_saved = false;
	io->output_count = 0U;
}

int
zmodem_posix_ignore_sigpipe(void)
{
	struct sigaction action;

	(void)memset(&action,0,sizeof(action));
	action.sa_handler = SIG_IGN;
	if (sigemptyset(&action.sa_mask) != 0) {
		return -1;
	}
	return sigaction(SIGPIPE,&action,NULL);
}

int
zmodem_posix_io_open(struct zmodem_posix_io * io,const char * path)
{
	int fd = open(path,O_RDWR | O_NOCTTY);

	if (fd < 0) {
		return -1;
	}
	io->input_fd = fd;
	io->output_fd = fd;
	io->owned_fd = fd;
	return 0;
}

int
zmodem_posix_io_make_raw(struct zmodem_posix_io * io)
{
	struct termios attributes;

	errno = 0;
	if (isatty(io->input_fd) == 0) {
		return (errno == EBADF) ? -1 : 0;
	}
	if (tcgetattr(io->input_fd,&attributes) != 0) {
		return -1;
	}
	io->saved_termios = attributes;
	io->termios_saved = true;
	attributes.c_iflag = 0;
	attributes.c_oflag = 0;
	attributes.c_lflag = 0;
	attributes.c_cflag &= (tcflag_t)~(CSIZE | PARENB);
	attributes.c_cflag |= CS8;
	attributes.c_cc[VMIN] = 1;
	attributes.c_cc[VTIME] = 0;
	if (ZMODEM_POSIX_TCSETATTR(io->input_fd,TCSANOW,&attributes) != 0) {
		return -1;
	}
	return 0;
}

int
zmodem_posix_io_restore(struct zmodem_posix_io * io)
{
	if (io->termios_saved) {
		if (ZMODEM_POSIX_TCSETATTR(io->input_fd,TCSANOW,
		    &io->saved_termios) != 0) {
			return -1;
		}
		io->termios_saved = false;
	}
	return 0;
}

int
zmodem_posix_io_close(struct zmodem_posix_io * io)
{
	int result = 0;

	if (posix_flush(io) != ZMODEM_OK) {
		result = -1;
	}
	if (zmodem_posix_io_restore(io) != 0) {
		result = -1;
	}
	if (io->owned_fd >= 0) {
		if (ZMODEM_POSIX_CLOSE(io->owned_fd) != 0) {
			result = -1;
		}
		io->owned_fd = -1;
	}
	return result;
}

void
zmodem_posix_io_bind(struct zmodem_io * interface,struct zmodem_posix_io * io)
{
	interface->context = io;
	interface->read = posix_read;
	interface->write = posix_write;
	interface->flush = posix_flush;
	interface->poll = posix_poll;
	interface->purge = posix_purge;
}

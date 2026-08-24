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
#include <unistd.h>

#include "zmdm.h"

static int
wait_for_input(int fd,int timeout_ms)
{
	int result;
	fd_set read_set;
	struct timeval timeout;

	if ((fd < 0) || (fd >= FD_SETSIZE)) {
		return ZMODEM_IO_ERROR;
	}
	if (timeout_ms < 0) {
		timeout_ms = 0;
	}
	for (;;) {
		timeout.tv_sec = (time_t)(timeout_ms / 1000);
		timeout.tv_usec = (suseconds_t)(timeout_ms % 1000) *
		    (suseconds_t)1000;
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
posix_read(void * context,uint8_t * buffer,size_t capacity,size_t * count,
    int timeout_ms)
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
posix_write(void * context,const uint8_t * buffer,size_t length)
{
	struct zmodem_posix_io * io = context;
	size_t offset = 0U;

	while (offset < length) {
		ssize_t result;

		for (;;) {
			result = write(io->output_fd,&buffer[offset],length - offset);
			if (result >= 0) {
				break;
			}
			if (errno != EINTR) {
				break;
			}
		}
		if (result <= 0) {
			return ZMODEM_IO_ERROR;
		}
		offset += (size_t)result;
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
	if (tcsetattr(io->input_fd,TCSANOW,&attributes) != 0) {
		io->termios_saved = false;
		return -1;
	}
	return 0;
}

void
zmodem_posix_io_restore(struct zmodem_posix_io * io)
{
	if (io->termios_saved) {
		(void)tcsetattr(io->input_fd,TCSANOW,&io->saved_termios);
		io->termios_saved = false;
	}
}

void
zmodem_posix_io_close(struct zmodem_posix_io * io)
{
	zmodem_posix_io_restore(io);
	if (io->owned_fd >= 0) {
		(void)close(io->owned_fd);
		io->owned_fd = -1;
	}
}

void
zmodem_posix_io_bind(struct zmodem_io * interface,struct zmodem_posix_io * io)
{
	interface->context = io;
	interface->read = posix_read;
	interface->write = posix_write;
	interface->poll = posix_poll;
	interface->purge = posix_purge;
}

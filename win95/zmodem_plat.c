/*
 * Windows 95 transport adapter for borrowed COM and Winsock handles.
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

#include "plat.h"
#include "zmodem_plat.h"

#include <ctype.h>
#include <stdlib.h>

#ifdef ZMODEM_WIN95_TEST_MOCKS
BOOL WINAPI mock_GetCommTimeouts(HANDLE,LPCOMMTIMEOUTS);
BOOL WINAPI mock_SetCommTimeouts(HANDLE,LPCOMMTIMEOUTS);
BOOL WINAPI mock_ClearCommError(HANDLE,LPDWORD,LPCOMSTAT);
BOOL WINAPI mock_PurgeComm(HANDLE,DWORD);
BOOL WINAPI mock_ReadFile(HANDLE,LPVOID,DWORD,LPDWORD,LPOVERLAPPED);
BOOL WINAPI mock_WriteFile(HANDLE,LPCVOID,DWORD,LPDWORD,LPOVERLAPPED);
#endif

#ifndef ZMODEM_WIN95_GET_COMM_TIMEOUTS
#define ZMODEM_WIN95_GET_COMM_TIMEOUTS(handle,timeouts) \
	GetCommTimeouts((handle),(timeouts))
#endif
#ifndef ZMODEM_WIN95_SET_COMM_TIMEOUTS
#define ZMODEM_WIN95_SET_COMM_TIMEOUTS(handle,timeouts) \
	SetCommTimeouts((handle),(timeouts))
#endif
#ifndef ZMODEM_WIN95_CLEAR_COMM_ERROR
#define ZMODEM_WIN95_CLEAR_COMM_ERROR(handle,errors,status) \
	ClearCommError((handle),(errors),(status))
#endif
#ifndef ZMODEM_WIN95_PURGE_COMM
#define ZMODEM_WIN95_PURGE_COMM(handle,flags) PurgeComm((handle),(flags))
#endif
#ifndef ZMODEM_WIN95_READ_FILE
#define ZMODEM_WIN95_READ_FILE(handle,buffer,length,count,overlapped) \
	ReadFile((handle),(buffer),(length),(count),(overlapped))
#endif
#ifndef ZMODEM_WIN95_WRITE_FILE
#define ZMODEM_WIN95_WRITE_FILE(handle,buffer,length,count,overlapped) \
	WriteFile((handle),(buffer),(length),(count),(overlapped))
#endif

static DWORD clock_last;
static uint64_t clock_epoch;
static bool clock_started;

int
zmodem_win95_clock_gettime(int clock_id,struct zmodem_win95_timespec * value)
{
	DWORD ticks;
	uint64_t extended;

	(void)clock_id;
	if (value == NULL) {
		errno = EINVAL;
		return -1;
	}
	ticks = GetTickCount();
	if (clock_started && ticks < clock_last) {
		clock_epoch += (UINT64_C(1) << 32);
	}
	clock_started = true;
	clock_last = ticks;
	extended = clock_epoch + (uint64_t)ticks;
	value->tv_sec = (long)(extended / UINT64_C(1000));
	value->tv_nsec = (long)(extended % UINT64_C(1000)) * 1000000L;
	return 0;
}

static int
socket_wait(SOCKET socket_handle,bool write_ready,int timeout_ms)
{
	DWORD started = GetTickCount();

	for (;;) {
		fd_set sockets;
		struct timeval timeout;
		struct timeval * timeout_pointer = &timeout;
		int result;

		FD_ZERO(&sockets);
		FD_SET(socket_handle,&sockets);
		if (timeout_ms < 0) {
			timeout_pointer = NULL;
		}
		else if (timeout_ms > 0) {
			DWORD elapsed = GetTickCount() - started;

			if (elapsed >= (DWORD)timeout_ms) {
				return 0;
			}
			elapsed = (DWORD)timeout_ms - elapsed;
			timeout.tv_sec = (long)(elapsed / 1000UL);
			timeout.tv_usec = (long)(elapsed % 1000UL) * 1000L;
		}
		else {
			timeout.tv_sec = 0L;
			timeout.tv_usec = 0L;
		}
		result = select(0,write_ready ? NULL : &sockets,
		    write_ready ? &sockets : NULL,NULL,timeout_pointer);
		if (result >= 0) {
			return result;
		}
		if (WSAGetLastError() != WSAEINTR) {
			return -1;
		}
	}
}

static int
comm_wait(HANDLE handle,DWORD * available,int timeout_ms)
{
	DWORD started = GetTickCount();

	for (;;) {
		COMSTAT status;
		DWORD errors;

		if (!ZMODEM_WIN95_CLEAR_COMM_ERROR(handle,&errors,&status)) {
			return -1;
		}
		if (status.cbInQue != 0UL) {
			*available = status.cbInQue;
			return 1;
		}
		if (timeout_ms <= 0 ||
		    GetTickCount() - started >= (DWORD)timeout_ms) {
			*available = 0UL;
			return 0;
		}
		Sleep(1UL);
	}
}

static int
win95_read(void * context,uint8_t * restrict buffer,size_t capacity,
    size_t * restrict count,int timeout_ms)
{
	struct zmodem_plat_io * io = context;

	*count = 0U;
	if (io->transport == ZMODEM_WIN95_SOCKET) {
		int ready = socket_wait(io->socket_handle,false,timeout_ms);
		int result;

		if (ready == 0) {
			return ZMODEM_TIMEOUT;
		}
		if (ready < 0) {
			return ZMODEM_IO_ERROR;
		}
		do {
			result = recv(io->socket_handle,(char *)buffer,(int)capacity,0);
		} while (result == SOCKET_ERROR && WSAGetLastError() == WSAEINTR);
		if (result <= 0) {
			return ZMODEM_IO_ERROR;
		}
		*count = (size_t)result;
		return ZMODEM_OK;
	}
	else {
		DWORD available;
		DWORD received;
		int ready = comm_wait(io->comm_handle,&available,timeout_ms);

		if (ready == 0) {
			return ZMODEM_TIMEOUT;
		}
		if (ready < 0) {
			return ZMODEM_IO_ERROR;
		}
		if (available > (DWORD)capacity) {
			available = (DWORD)capacity;
		}
		if (!ZMODEM_WIN95_READ_FILE(io->comm_handle,buffer,available,
		    &received,NULL) ||
		    received == 0UL) {
			return ZMODEM_IO_ERROR;
		}
		*count = (size_t)received;
		return ZMODEM_OK;
	}
}

static int
write_socket(struct zmodem_plat_io * io,const uint8_t * buffer,size_t length,
    size_t * sent)
{
	size_t offset = 0U;

	*sent = 0U;
	while (offset < length) {
		int result = send(io->socket_handle,(const char *)&buffer[offset],
		    (int)(length - offset),0);

		if (result > 0) {
			offset += (size_t)result;
			*sent = offset;
			continue;
		}
		if (result == SOCKET_ERROR) {
			int error = WSAGetLastError();

			if (error == WSAEINTR) {
				continue;
			}
			if (error == WSAEWOULDBLOCK &&
			    socket_wait(io->socket_handle,true,-1) > 0) {
				continue;
			}
		}
		return ZMODEM_IO_ERROR;
	}
	return ZMODEM_OK;
}

static int
write_comm(struct zmodem_plat_io * io,const uint8_t * buffer,size_t length,
    size_t * sent)
{
	size_t offset = 0U;

	*sent = 0U;
	while (offset < length) {
		DWORD written;

		if (!ZMODEM_WIN95_WRITE_FILE(io->comm_handle,&buffer[offset],
		    (DWORD)(length - offset),&written,NULL) || written == 0UL) {
			return ZMODEM_IO_ERROR;
		}
		offset += (size_t)written;
		*sent = offset;
	}
	return ZMODEM_OK;
}

static int
win95_flush(void * context)
{
	struct zmodem_plat_io * io = context;
	size_t sent;
	int result;

	if (io->output_count == 0U) {
		return ZMODEM_OK;
	}
	result = io->transport == ZMODEM_WIN95_SOCKET ?
	    write_socket(io,io->output_buffer,io->output_count,&sent) :
	    write_comm(io,io->output_buffer,io->output_count,&sent);
	if (result == ZMODEM_OK) {
		io->output_count = 0U;
	}
	else if (sent > 0U) {
		io->output_count -= sent;
		(void)memmove(io->output_buffer,&io->output_buffer[sent],
		    io->output_count);
	}
	return result;
}

static int
win95_write(void * context,const uint8_t * restrict buffer,size_t length)
{
	struct zmodem_plat_io * io = context;

	while (length > 0U) {
		size_t available = sizeof(io->output_buffer) - io->output_count;
		size_t copied = length < available ? length : available;

		(void)memcpy(&io->output_buffer[io->output_count],buffer,copied);
		io->output_count += copied;
		buffer += copied;
		length -= copied;
		if (io->output_count == sizeof(io->output_buffer) &&
		    win95_flush(io) != ZMODEM_OK) {
			return ZMODEM_IO_ERROR;
		}
	}
	return ZMODEM_OK;
}

static int
win95_poll(void * context)
{
	struct zmodem_plat_io * io = context;

	if (io->transport == ZMODEM_WIN95_SOCKET) {
		int result = socket_wait(io->socket_handle,false,0);

		return result < 0 ? ZMODEM_IO_ERROR : result;
	}
	else {
		DWORD available;
		int result = comm_wait(io->comm_handle,&available,0);

		return result < 0 ? ZMODEM_IO_ERROR : result;
	}
}

static int
win95_purge(void * context)
{
	struct zmodem_plat_io * io = context;

	if (io->transport == ZMODEM_WIN95_COM) {
		return ZMODEM_WIN95_PURGE_COMM(io->comm_handle,PURGE_RXCLEAR) ?
		    ZMODEM_OK : ZMODEM_IO_ERROR;
	}
	for (;;) {
		char discarded[256];
		int ready = socket_wait(io->socket_handle,false,0);
		int result;

		if (ready == 0) {
			return ZMODEM_OK;
		}
		if (ready < 0) {
			return ZMODEM_IO_ERROR;
		}
		result = recv(io->socket_handle,discarded,sizeof(discarded),0);
		if (result == 0) {
			return ZMODEM_OK;
		}
		if (result == SOCKET_ERROR) {
			int error = WSAGetLastError();

			if (error == WSAEINTR || error == WSAEWOULDBLOCK) {
				continue;
			}
			return ZMODEM_IO_ERROR;
		}
	}
}

void
zmodem_plat_io_init(struct zmodem_plat_io * io,int input_fd,int output_fd)
{
	(void)input_fd;
	(void)output_fd;
	(void)memset(io,0,sizeof(*io));
	io->transport = ZMODEM_WIN95_NONE;
	io->comm_handle = INVALID_HANDLE_VALUE;
	io->socket_handle = INVALID_SOCKET;
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
	if (io->transport == ZMODEM_WIN95_SOCKET) {
		WSADATA data;

		if (WSAStartup(MAKEWORD(1,1),&data) != 0) {
			return -1;
		}
		io->winsock_started = true;
		if (LOBYTE(data.wVersion) != 1 || HIBYTE(data.wVersion) < 1) {
			(void)WSACleanup();
			io->winsock_started = false;
			return -1;
		}
		return 0;
	}
	if (!ZMODEM_WIN95_GET_COMM_TIMEOUTS(io->comm_handle,
	    &io->saved_timeouts)) {
		return -1;
	}
	else {
		COMMTIMEOUTS timeouts = io->saved_timeouts;

		timeouts.ReadIntervalTimeout = MAXDWORD;
		timeouts.ReadTotalTimeoutMultiplier = 0UL;
		timeouts.ReadTotalTimeoutConstant = 0UL;
		if (!ZMODEM_WIN95_SET_COMM_TIMEOUTS(io->comm_handle,&timeouts)) {
			return -1;
		}
		io->timeouts_saved = true;
	}
	return 0;
}

int
zmodem_plat_io_restore(struct zmodem_plat_io * io)
{
	if (io->timeouts_saved) {
		if (!ZMODEM_WIN95_SET_COMM_TIMEOUTS(io->comm_handle,
		    &io->saved_timeouts)) {
			return -1;
		}
		io->timeouts_saved = false;
	}
	return 0;
}

int
zmodem_plat_io_close(struct zmodem_plat_io * io)
{
	int result = 0;

	if (io->transport != ZMODEM_WIN95_NONE &&
	    win95_flush(io) != ZMODEM_OK) {
		result = -1;
	}
	if (zmodem_plat_io_restore(io) != 0) {
		result = -1;
	}
	if (io->winsock_started) {
		if (WSACleanup() != 0) {
			result = -1;
		}
		io->winsock_started = false;
	}
	return result;
}

void
zmodem_plat_io_bind(struct zmodem_io * interface,struct zmodem_plat_io * io)
{
	interface->context = io;
	interface->read = win95_read;
	interface->write = win95_write;
	interface->flush = win95_flush;
	interface->poll = win95_poll;
	interface->purge = win95_purge;
}

static bool
parse_handle(const char * text,unsigned long * value)
{
	char * end;
	unsigned long parsed;

	if (*text == '\0') {
		return false;
	}
	errno = 0;
	parsed = strtoul(text,&end,10);
	if (errno == ERANGE || *end != '\0' || parsed == ULONG_MAX) {
		return false;
	}
	*value = parsed;
	return true;
}

enum zmodem_plat_option_result
zmodem_plat_parse_option(struct zmodem_plat_io * io,
    enum zmodem_plat_application application,const char * argument,
    size_t * option_index)
{
	int option = toupper((unsigned char)argument[*option_index]);
	unsigned long value;

	(void)application;
	if (option == 'I') {
		io->escape_iac = true;
		return ZMODEM_PLAT_OPTION_ACCEPTED;
	}
	if (option != 'C' && option != 'T') {
		return ZMODEM_PLAT_OPTION_NOT_HANDLED;
	}
	if (io->transport != ZMODEM_WIN95_NONE ||
	    !parse_handle(&argument[*option_index + 1U],&value)) {
		return ZMODEM_PLAT_OPTION_INVALID;
	}
	if (option == 'C') {
		io->transport = ZMODEM_WIN95_COM;
		io->comm_handle = (HANDLE)(UINT_PTR)value;
	}
	else {
		io->transport = ZMODEM_WIN95_SOCKET;
		io->socket_handle = (SOCKET)(UINT_PTR)value;
	}
	*option_index = strlen(argument) - 1U;
	return ZMODEM_PLAT_OPTION_ACCEPTED;
}

int
zmodem_plat_post_parse(struct zmodem_plat_io * io,
    enum zmodem_plat_application application,int argc,char * const * argv,
    size_t first_operand)
{
	const char * program = application == ZMODEM_PLAT_ZMTX ? "zmtx" : "zmrx";

	(void)argc;
	(void)argv;
	(void)first_operand;
	if (io->transport == ZMODEM_WIN95_NONE) {
		(void)fprintf(stderr,"%s: select -cHANDLE or -tSOCKET\n",program);
		return 2;
	}
	if (zmodem_plat_io_make_raw(io) != 0) {
		(void)fprintf(stderr,"%s: can't configure transfer handle\n",program);
		return 2;
	}
	return 0;
}

void
zmodem_plat_usage(enum zmodem_plat_application application)
{
	(void)application;
	(void)printf("\t-cHANDLE    use a borrowed COM handle\n");
	(void)printf("\t-tSOCKET    use a borrowed connected Winsock socket\n");
	(void)printf("\t-i          ZMODEM-escape outbound Telnet IAC bytes\n");
}

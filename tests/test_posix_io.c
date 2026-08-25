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
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "zmdm.h"
#include "zmdm_posix.h"

static volatile sig_atomic_t caught_signals;

static bool
expect(bool condition,const char * description)
{
	if (!condition) {
		(void)fprintf(stderr,"test_posix_io: %s\n",description);
	}
	return condition;
}

static void
catch_signal(int signal_number)
{
	(void)signal_number;
	caught_signals += 1;
}

static int64_t
elapsed_milliseconds(const struct timespec * start,const struct timespec * end)
{
	return ((int64_t)end->tv_sec * INT64_C(1000) +
	    (int64_t)end->tv_nsec / INT64_C(1000000)) -
	    ((int64_t)start->tv_sec * INT64_C(1000) +
	    (int64_t)start->tv_nsec / INT64_C(1000000));
}

static bool
test_pipe_transport(void)
{
	static const uint8_t input[] = { UINT8_C(1),UINT8_C(2),UINT8_C(3) };
	static const uint8_t output[] = { UINT8_C(4),UINT8_C(5) };
	struct zmodem_posix_io posix_io;
	struct zmodem_io io;
	uint8_t received[sizeof(input)];
	uint8_t written[sizeof(output)];
	size_t count = 0U;
	int input_pipe[2];
	int output_pipe[2];
	bool passed = true;

	if ((pipe(input_pipe) != 0) || (pipe(output_pipe) != 0)) {
		return expect(false,"create pipes");
	}
	zmodem_posix_io_init(&posix_io,input_pipe[0],output_pipe[1]);
	zmodem_posix_io_bind(&io,&posix_io);
	passed = expect(io.poll(io.context) == 0,"empty poll") && passed;
	passed = expect(io.read(io.context,received,sizeof(received),&count,0) ==
	    ZMODEM_TIMEOUT,"empty read timeout") && passed;
	passed = expect(write(input_pipe[1],input,sizeof(input)) ==
	    (ssize_t)sizeof(input),"seed input pipe") && passed;
	passed = expect(io.poll(io.context) == 1,"ready poll") && passed;
	passed = expect(io.read(io.context,received,sizeof(received),&count,0) ==
	    ZMODEM_OK,"read result") && passed;
	if (expect(count == sizeof(input),"read length")) {
		passed = expect(memcmp(received,input,sizeof(input)) == 0,
		    "read bytes") && passed;
	}
	else {
		passed = false;
	}
	passed = expect(io.write(io.context,output,sizeof(output)) == ZMODEM_OK,
	    "write result") && passed;
	passed = expect(posix_io.output_count == sizeof(output),
	    "buffer output until flush") && passed;
	passed = expect(io.flush(io.context) == ZMODEM_OK,"flush result") && passed;
	passed = expect(posix_io.output_count == 0U,"empty flushed output") && passed;
	passed = expect(read(output_pipe[0],written,sizeof(written)) ==
	    (ssize_t)sizeof(output),"read output pipe") && passed;
	passed = expect(memcmp(written,output,sizeof(output)) == 0,
	    "written bytes") && passed;
	passed = expect(write(input_pipe[1],input,sizeof(input)) ==
	    (ssize_t)sizeof(input),"seed purge input") && passed;
	passed = expect(io.purge(io.context) == ZMODEM_OK,"purge result") && passed;
	passed = expect(io.poll(io.context) == 0,"purged poll") && passed;
	passed = expect(zmodem_posix_io_make_raw(&posix_io) == 0,
	    "non-terminal raw operation") && passed;
	passed = expect(zmodem_posix_io_close(&posix_io) == 0,
	    "close pipe transport") && passed;
	passed = expect(close(input_pipe[0]) == 0,"close input read") && passed;
	passed = expect(close(input_pipe[1]) == 0,"close input write") && passed;
	passed = expect(close(output_pipe[0]) == 0,"close output read") && passed;
	passed = expect(close(output_pipe[1]) == 0,"close output write") && passed;
	return passed;
}

static bool
test_full_output_buffer(void)
{
	static uint8_t output[ZMODEM_TX_BURST_CAPACITY + 3U];
	static uint8_t received[sizeof(output)];
	struct zmodem_posix_io posix_io;
	struct zmodem_io io;
	char path[] = "/tmp/zmtx-posix-output.XXXXXX";
	size_t offset = 0U;
	size_t index;
	int fd;
	bool passed = true;

	fd = mkstemp(path);
	if (fd < 0) {
		return expect(false,"create output-buffer file");
	}
	(void)unlink(path);
	for (index = 0U; index < sizeof(output); index++) {
		output[index] = (uint8_t)(index & 0xffU);
	}
	zmodem_posix_io_init(&posix_io,-1,fd);
	zmodem_posix_io_bind(&io,&posix_io);
	passed = expect(io.write(io.context,output,sizeof(output)) == ZMODEM_OK,
	    "write beyond output-buffer capacity") && passed;
	passed = expect(posix_io.output_count == 3U,
	    "flush full output buffer") && passed;
	passed = expect(io.flush(io.context) == ZMODEM_OK,
	    "flush output-buffer remainder") && passed;
	passed = expect(lseek(fd,0,SEEK_SET) == 0,"rewind output-buffer file") &&
	    passed;
	while (offset < sizeof(received)) {
		ssize_t result = read(fd,&received[offset],sizeof(received) - offset);

		if (result <= 0) {
			passed = expect(false,"read buffered output") && passed;
			break;
		}
		offset += (size_t)result;
	}
	passed = expect(offset == sizeof(received),"buffered output length") &&
	    passed;
	if (offset == sizeof(received)) {
		passed = expect(memcmp(received,output,sizeof(output)) == 0,
		    "buffered output bytes") && passed;
	}
	passed = expect(zmodem_posix_io_close(&posix_io) == 0,
	    "close output-buffer transport") && passed;
	passed = expect(close(fd) == 0,"close output-buffer file") && passed;
	return passed;
}

static bool
test_reported_failures(void)
{
	static const uint8_t full_output[ZMODEM_TX_BURST_CAPACITY] = { 0U };
	struct zmodem_posix_io posix_io;
	struct zmodem_io io;
	uint8_t byte = 0U;
	size_t count = 0U;
	bool passed = true;

	zmodem_posix_io_init(&posix_io,-1,-1);
	zmodem_posix_io_bind(&io,&posix_io);
	passed = expect(io.poll(io.context) == ZMODEM_IO_ERROR,
	    "invalid poll descriptor") && passed;
	passed = expect(io.read(io.context,&byte,1U,&count,0) == ZMODEM_IO_ERROR,
	    "invalid read descriptor") && passed;
	passed = expect(io.write(io.context,&byte,1U) == ZMODEM_OK,
	    "buffer write for invalid descriptor") && passed;
	passed = expect(io.flush(io.context) == ZMODEM_IO_ERROR,
	    "invalid write descriptor at flush") && passed;
	zmodem_posix_io_init(&posix_io,-1,-1);
	zmodem_posix_io_bind(&io,&posix_io);
	passed = expect(io.write(io.context,full_output,sizeof(full_output)) ==
	    ZMODEM_IO_ERROR,"invalid descriptor at automatic flush") && passed;
	passed = expect(io.purge(io.context) == ZMODEM_IO_ERROR,
	    "invalid purge descriptor") && passed;
	passed = expect(zmodem_posix_io_make_raw(&posix_io) != 0,
	    "invalid terminal descriptor") && passed;
	passed = expect(zmodem_posix_io_open(&posix_io,
	    "/path/that/does/not/exist/zmodem") != 0,"open failure") && passed;
	zmodem_posix_io_init(&posix_io,FD_SETSIZE,-1);
	zmodem_posix_io_bind(&io,&posix_io);
	passed = expect(io.poll(io.context) == ZMODEM_IO_ERROR,
	    "descriptor above select range") && passed;
	return passed;
}

static bool
test_eof_and_closed_descriptor(void)
{
	struct zmodem_posix_io posix_io;
	struct zmodem_io io;
	uint8_t byte;
	size_t count;
	int descriptors[2];
	int closed_fd;
	bool passed = true;

	if (pipe(descriptors) != 0) {
		return expect(false,"create EOF pipe");
	}
	zmodem_posix_io_init(&posix_io,descriptors[0],descriptors[1]);
	zmodem_posix_io_bind(&io,&posix_io);
	passed = expect(io.read(io.context,&byte,1U,&count,-1) == ZMODEM_TIMEOUT,
	    "negative timeout becomes poll") && passed;
	passed = expect(close(descriptors[1]) == 0,"close EOF writer") && passed;
	passed = expect(io.read(io.context,&byte,1U,&count,0) == ZMODEM_TIMEOUT,
	    "EOF read") && passed;
	passed = expect(io.purge(io.context) == ZMODEM_OK,"EOF purge") && passed;
	closed_fd = descriptors[0];
	passed = expect(close(descriptors[0]) == 0,"close EOF reader") && passed;
	zmodem_posix_io_init(&posix_io,closed_fd,-1);
	zmodem_posix_io_bind(&io,&posix_io);
	passed = expect(io.poll(io.context) == ZMODEM_IO_ERROR,
	    "closed descriptor select error") && passed;
	return passed;
}

static bool
test_owned_descriptor(void)
{
	struct zmodem_posix_io posix_io;
	bool passed = true;

	zmodem_posix_io_init(&posix_io,-1,-1);
	passed = expect(zmodem_posix_io_open(&posix_io,"/dev/null") == 0,
	    "open owned descriptor") && passed;
	passed = expect(posix_io.owned_fd >= 0,"owned descriptor value") && passed;
	passed = expect(posix_io.input_fd == posix_io.owned_fd,
	    "owned input descriptor") && passed;
	passed = expect(posix_io.output_fd == posix_io.owned_fd,
	    "owned output descriptor") && passed;
	passed = expect(zmodem_posix_io_make_raw(&posix_io) == 0,
	    "owned non-terminal") && passed;
	posix_io.termios_saved = true;
	passed = expect(zmodem_posix_io_restore(&posix_io) != 0,
	    "report invalid terminal restoration") && passed;
	passed = expect(posix_io.termios_saved,"retain unrestored state") && passed;
	passed = expect(zmodem_posix_io_close(&posix_io) != 0,
	    "propagate restoration failure while closing") && passed;
	passed = expect(posix_io.termios_saved,
	    "retain unrestored state after close") && passed;
	passed = expect(posix_io.owned_fd == -1,"clear owned descriptor") && passed;
	return passed;
}

static bool
test_terminal_transport(void)
{
	struct zmodem_posix_io posix_io;
	char * slave_name;
	int master_fd;
	int slave_fd;
	bool passed = true;

	master_fd = posix_openpt(O_RDWR | O_NOCTTY);
	if (master_fd < 0) {
		return expect(false,"open pseudo-terminal master");
	}
	if ((grantpt(master_fd) != 0) || (unlockpt(master_fd) != 0)) {
		(void)close(master_fd);
		return expect(false,"prepare pseudo-terminal");
	}
	slave_name = ptsname(master_fd);
	if (slave_name == NULL) {
		(void)close(master_fd);
		return expect(false,"name pseudo-terminal slave");
	}
	slave_fd = open(slave_name,O_RDWR | O_NOCTTY);
	if (slave_fd < 0) {
		(void)close(master_fd);
		return expect(false,"open pseudo-terminal slave");
	}
	zmodem_posix_io_init(&posix_io,slave_fd,slave_fd);
	passed = expect(zmodem_posix_io_make_raw(&posix_io) == 0,
	    "configure pseudo-terminal raw mode") && passed;
	passed = expect(posix_io.termios_saved,"save terminal attributes") &&
	    passed;
	passed = expect(zmodem_posix_io_restore(&posix_io) == 0,
	    "restore terminal attributes") && passed;
	passed = expect(!posix_io.termios_saved,"restore terminal attributes") &&
	    passed;
	passed = expect(close(slave_fd) == 0,"close pseudo-terminal slave") &&
	    passed;
	passed = expect(close(master_fd) == 0,"close pseudo-terminal master") &&
	    passed;
	return passed;
}

static bool
test_directory_read_failures(void)
{
	struct zmodem_posix_io posix_io;
	struct zmodem_io io;
	uint8_t byte;
	size_t count;
	int directory_fd;
	bool passed = true;

	directory_fd = open("/",O_RDONLY);
	if (directory_fd < 0) {
		return expect(false,"open directory descriptor");
	}
	zmodem_posix_io_init(&posix_io,directory_fd,-1);
	zmodem_posix_io_bind(&io,&posix_io);
	passed = expect(io.read(io.context,&byte,1U,&count,0) ==
	    ZMODEM_IO_ERROR,"directory read error") && passed;
	passed = expect(io.purge(io.context) == ZMODEM_IO_ERROR,
	    "directory purge error") && passed;
	passed = expect(close(directory_fd) == 0,"close directory descriptor") &&
	    passed;
	return passed;
}

static bool
test_interrupted_wait(void)
{
	struct zmodem_posix_io posix_io;
	struct zmodem_io io;
	struct sigaction action;
	struct sigaction previous;
	struct timespec started = { 0,0 };
	struct timespec finished = { 0,0 };
	uint8_t byte;
	uint8_t ready;
	size_t count;
	int descriptors[2];
	int ready_pipe[2];
	int start_pipe[2];
	int read_result;
	int wait_result;
	ssize_t ready_result;
	pid_t child;
	bool passed = true;

	if (pipe(descriptors) != 0) {
		return expect(false,"create interrupted-wait pipe");
	}
	if (pipe(ready_pipe) != 0) {
		(void)close(descriptors[0]);
		(void)close(descriptors[1]);
		return expect(false,"create interrupted-wait readiness pipe");
	}
	if (pipe(start_pipe) != 0) {
		(void)close(descriptors[0]);
		(void)close(descriptors[1]);
		(void)close(ready_pipe[0]);
		(void)close(ready_pipe[1]);
		return expect(false,"create interrupted-wait start pipe");
	}
	(void)memset(&action,0,sizeof(action));
	action.sa_handler = catch_signal;
	if ((sigemptyset(&action.sa_mask) != 0) ||
	    (sigaction(SIGUSR1,&action,&previous) != 0)) {
		(void)close(descriptors[0]);
		(void)close(descriptors[1]);
		(void)close(ready_pipe[0]);
		(void)close(ready_pipe[1]);
		(void)close(start_pipe[0]);
		(void)close(start_pipe[1]);
		return expect(false,"install interrupted-wait handler");
	}
	child = fork();
	if (child == 0) {
		unsigned signal_index;
		ssize_t start_result;

		(void)close(ready_pipe[0]);
		(void)close(start_pipe[1]);
		ready = UINT8_C(1);
		if (write(ready_pipe[1],&ready,sizeof(ready)) !=
		    (ssize_t)sizeof(ready)) {
			_exit(1);
		}
		(void)close(ready_pipe[1]);
		do {
			start_result = read(start_pipe[0],&ready,sizeof(ready));
		} while ((start_result < 0) && (errno == EINTR));
		(void)close(start_pipe[0]);
		if (start_result != (ssize_t)sizeof(ready)) {
			_exit(1);
		}
		for (signal_index=0U;signal_index<10U;signal_index++) {
			(void)usleep(30000U);
			if (kill(getppid(),SIGUSR1) != 0) {
				break;
			}
		}
		_exit(0);
	}
	if (child < 0) {
		(void)sigaction(SIGUSR1,&previous,NULL);
		(void)close(descriptors[0]);
		(void)close(descriptors[1]);
		(void)close(ready_pipe[0]);
		(void)close(ready_pipe[1]);
		(void)close(start_pipe[0]);
		(void)close(start_pipe[1]);
		return expect(false,"fork interrupted-wait helper");
	}
	(void)close(ready_pipe[1]);
	(void)close(start_pipe[0]);
	do {
		ready_result = read(ready_pipe[0],&ready,sizeof(ready));
	} while ((ready_result < 0) && (errno == EINTR));
	passed = expect(ready_result == (ssize_t)sizeof(ready),
	    "start interrupted-wait helper") && passed;
	passed = expect(close(ready_pipe[0]) == 0,
	    "close interrupted-wait readiness pipe") && passed;
	zmodem_posix_io_init(&posix_io,descriptors[0],descriptors[1]);
	zmodem_posix_io_bind(&io,&posix_io);
	caught_signals = 0;
	passed = expect(clock_gettime(CLOCK_MONOTONIC,&started) == 0,
	    "start interrupted-wait clock") && passed;
	ready = UINT8_C(1);
	passed = expect(write(start_pipe[1],&ready,sizeof(ready)) ==
	    (ssize_t)sizeof(ready),"release interrupted-wait helper") && passed;
	passed = expect(close(start_pipe[1]) == 0,
	    "close interrupted-wait start pipe") && passed;
	read_result = io.read(io.context,&byte,1U,&count,200);
	passed = expect(clock_gettime(CLOCK_MONOTONIC,&finished) == 0,
	    "stop interrupted-wait clock") && passed;
	passed = expect(read_result == ZMODEM_TIMEOUT,
	    "retry interrupted select to original deadline") && passed;
	passed = expect(caught_signals >= 3,
	    "deliver repeated signals during wait") && passed;
	passed = expect(elapsed_milliseconds(&started,&finished) >= INT64_C(150),
	    "interrupted wait does not expire early") && passed;
	passed = expect(elapsed_milliseconds(&started,&finished) < INT64_C(350),
	    "interruptions do not restart timeout") && passed;
	do {
		wait_result = waitpid(child,NULL,0);
	} while ((wait_result < 0) && (errno == EINTR));
	passed = expect(wait_result == child,
	    "wait for interrupted-wait helper") && passed;
	passed = expect(sigaction(SIGUSR1,&previous,NULL) == 0,
	    "restore interrupted-wait handler") && passed;
	passed = expect(close(descriptors[0]) == 0,
	    "close interrupted-wait reader") && passed;
	passed = expect(close(descriptors[1]) == 0,
	    "close interrupted-wait writer") && passed;
	return passed;
}

int
main(void)
{
	bool passed = true;

	passed = test_pipe_transport() && passed;
	passed = test_full_output_buffer() && passed;
	passed = test_reported_failures() && passed;
	passed = test_eof_and_closed_descriptor() && passed;
	passed = test_owned_descriptor() && passed;
	passed = test_terminal_transport() && passed;
	passed = test_directory_read_failures() && passed;
	passed = test_interrupted_wait() && passed;
	passed = expect(zmodem_posix_ignore_sigpipe() == 0,
	    "ignore broken-pipe signal") && passed;
	return passed ? 0 : 1;
}

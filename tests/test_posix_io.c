#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include "zmdm.h"
#include "zmdm_posix.h"

static bool
expect(bool condition,const char * description)
{
	if (!condition) {
		(void)fprintf(stderr,"test_posix_io: %s\n",description);
	}
	return condition;
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
	zmodem_posix_io_close(&posix_io);
	passed = expect(close(input_pipe[0]) == 0,"close input read") && passed;
	passed = expect(close(input_pipe[1]) == 0,"close input write") && passed;
	passed = expect(close(output_pipe[0]) == 0,"close output read") && passed;
	passed = expect(close(output_pipe[1]) == 0,"close output write") && passed;
	return passed;
}

static bool
test_reported_failures(void)
{
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
	passed = expect(io.write(io.context,&byte,1U) == ZMODEM_IO_ERROR,
	    "invalid write descriptor") && passed;
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
	zmodem_posix_io_restore(&posix_io);
	passed = expect(!posix_io.termios_saved,"clear restored state") && passed;
	zmodem_posix_io_close(&posix_io);
	passed = expect(posix_io.owned_fd == -1,"clear owned descriptor") && passed;
	return passed;
}

int
main(void)
{
	bool passed = true;

	passed = test_pipe_transport() && passed;
	passed = test_reported_failures() && passed;
	passed = test_eof_and_closed_descriptor() && passed;
	passed = test_owned_descriptor() && passed;
	return passed ? 0 : 1;
}

#include "plat.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include "zmdm.h"
#include "zmodem_plat.h"

static bool
expect(bool condition,const char * description)
{
	if (!condition) {
		(void)fprintf(stderr,"test_plat_io: %s\n",description);
	}
	return condition;
}

static bool
test_pipe_transport(void)
{
	static const uint8_t input[] = { UINT8_C(1),UINT8_C(2),UINT8_C(3) };
	static const uint8_t output[] = { UINT8_C(4),UINT8_C(5) };
	struct zmodem_plat_io plat_io;
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
	zmodem_plat_io_init(&plat_io,input_pipe[0],output_pipe[1]);
	zmodem_plat_io_bind(&io,&plat_io);
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
	passed = expect(plat_io.output_count == sizeof(output),
	    "buffer output until flush") && passed;
	passed = expect(io.flush(io.context) == ZMODEM_OK,"flush result") && passed;
	passed = expect(plat_io.output_count == 0U,"empty flushed output") && passed;
	passed = expect(read(output_pipe[0],written,sizeof(written)) ==
	    (ssize_t)sizeof(output),"read output pipe") && passed;
	passed = expect(memcmp(written,output,sizeof(output)) == 0,
	    "written bytes") && passed;
	passed = expect(write(input_pipe[1],input,sizeof(input)) ==
	    (ssize_t)sizeof(input),"seed purge input") && passed;
	passed = expect(io.purge(io.context) == ZMODEM_OK,"purge result") && passed;
	passed = expect(io.poll(io.context) == 0,"purged poll") && passed;
	passed = expect(zmodem_plat_io_make_raw(&plat_io) == 0,
	    "non-terminal raw operation") && passed;
	passed = expect(zmodem_plat_io_close(&plat_io) == 0,
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
	struct zmodem_plat_io plat_io;
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
	zmodem_plat_io_init(&plat_io,-1,fd);
	zmodem_plat_io_bind(&io,&plat_io);
	passed = expect(io.write(io.context,output,sizeof(output)) == ZMODEM_OK,
	    "write beyond output-buffer capacity") && passed;
	passed = expect(plat_io.output_count == 3U,
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
	passed = expect(zmodem_plat_io_close(&plat_io) == 0,
	    "close output-buffer transport") && passed;
	passed = expect(close(fd) == 0,"close output-buffer file") && passed;
	return passed;
}

static bool
test_reported_failures(void)
{
	static const uint8_t full_output[ZMODEM_TX_BURST_CAPACITY] = { 0U };
	struct zmodem_plat_io plat_io;
	struct zmodem_io io;
	uint8_t byte = 0U;
	size_t count = 0U;
	bool passed = true;

	zmodem_plat_io_init(&plat_io,-1,-1);
	zmodem_plat_io_bind(&io,&plat_io);
	passed = expect(io.poll(io.context) == ZMODEM_IO_ERROR,
	    "invalid poll descriptor") && passed;
	passed = expect(io.read(io.context,&byte,1U,&count,0) == ZMODEM_IO_ERROR,
	    "invalid read descriptor") && passed;
	passed = expect(io.write(io.context,&byte,1U) == ZMODEM_OK,
	    "buffer write for invalid descriptor") && passed;
	passed = expect(io.flush(io.context) == ZMODEM_IO_ERROR,
	    "invalid write descriptor at flush") && passed;
	zmodem_plat_io_init(&plat_io,-1,-1);
	zmodem_plat_io_bind(&io,&plat_io);
	passed = expect(io.write(io.context,full_output,sizeof(full_output)) ==
	    ZMODEM_IO_ERROR,"invalid descriptor at automatic flush") && passed;
	passed = expect(io.purge(io.context) == ZMODEM_IO_ERROR,
	    "invalid purge descriptor") && passed;
	passed = expect(zmodem_plat_io_make_raw(&plat_io) != 0,
	    "invalid terminal descriptor") && passed;
	passed = expect(zmodem_plat_io_open(&plat_io,
	    "/path/that/does/not/exist/zmodem") != 0,"open failure") && passed;
	zmodem_plat_io_init(&plat_io,FD_SETSIZE,-1);
	zmodem_plat_io_bind(&io,&plat_io);
	passed = expect(io.poll(io.context) == ZMODEM_IO_ERROR,
	    "descriptor above select range") && passed;
	return passed;
}

static bool
test_eof_and_closed_descriptor(void)
{
	struct zmodem_plat_io plat_io;
	struct zmodem_io io;
	uint8_t byte;
	size_t count;
	int descriptors[2];
	int closed_fd;
	bool passed = true;

	if (pipe(descriptors) != 0) {
		return expect(false,"create EOF pipe");
	}
	zmodem_plat_io_init(&plat_io,descriptors[0],descriptors[1]);
	zmodem_plat_io_bind(&io,&plat_io);
	passed = expect(io.read(io.context,&byte,1U,&count,-1) == ZMODEM_TIMEOUT,
	    "negative timeout becomes poll") && passed;
	passed = expect(close(descriptors[1]) == 0,"close EOF writer") && passed;
	passed = expect(io.read(io.context,&byte,1U,&count,0) == ZMODEM_TIMEOUT,
	    "EOF read") && passed;
	passed = expect(io.purge(io.context) == ZMODEM_OK,"EOF purge") && passed;
	closed_fd = descriptors[0];
	passed = expect(close(descriptors[0]) == 0,"close EOF reader") && passed;
	zmodem_plat_io_init(&plat_io,closed_fd,-1);
	zmodem_plat_io_bind(&io,&plat_io);
	passed = expect(io.poll(io.context) == ZMODEM_IO_ERROR,
	    "closed descriptor select error") && passed;
	return passed;
}

static bool
test_owned_descriptor(void)
{
	struct zmodem_plat_io plat_io;
	bool passed = true;

	zmodem_plat_io_init(&plat_io,-1,-1);
	passed = expect(zmodem_plat_io_open(&plat_io,"/dev/null") == 0,
	    "open owned descriptor") && passed;
	passed = expect(plat_io.owned_fd >= 0,"owned descriptor value") && passed;
	passed = expect(plat_io.input_fd == plat_io.owned_fd,
	    "owned input descriptor") && passed;
	passed = expect(plat_io.output_fd == plat_io.owned_fd,
	    "owned output descriptor") && passed;
	passed = expect(zmodem_plat_io_make_raw(&plat_io) == 0,
	    "owned non-terminal") && passed;
	plat_io.termios_saved = true;
	passed = expect(zmodem_plat_io_restore(&plat_io) != 0,
	    "report invalid terminal restoration") && passed;
	passed = expect(plat_io.termios_saved,"retain unrestored state") && passed;
	passed = expect(zmodem_plat_io_close(&plat_io) != 0,
	    "propagate restoration failure while closing") && passed;
	passed = expect(plat_io.termios_saved,
	    "retain unrestored state after close") && passed;
	passed = expect(plat_io.owned_fd == -1,"clear owned descriptor") && passed;
	return passed;
}

static bool
test_terminal_transport(void)
{
	struct zmodem_plat_io plat_io;
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
	zmodem_plat_io_init(&plat_io,slave_fd,slave_fd);
	passed = expect(zmodem_plat_io_make_raw(&plat_io) == 0,
	    "configure pseudo-terminal raw mode") && passed;
	passed = expect(plat_io.termios_saved,"save terminal attributes") &&
	    passed;
	passed = expect(zmodem_plat_io_restore(&plat_io) == 0,
	    "restore terminal attributes") && passed;
	passed = expect(!plat_io.termios_saved,"restore terminal attributes") &&
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
	struct zmodem_plat_io plat_io;
	struct zmodem_io io;
	uint8_t byte;
	size_t count;
	int directory_fd;
	bool passed = true;

	directory_fd = open("/",O_RDONLY);
	if (directory_fd < 0) {
		return expect(false,"open directory descriptor");
	}
	zmodem_plat_io_init(&plat_io,directory_fd,-1);
	zmodem_plat_io_bind(&io,&plat_io);
	passed = expect(io.read(io.context,&byte,1U,&count,0) ==
	    ZMODEM_IO_ERROR,"directory read error") && passed;
	passed = expect(io.purge(io.context) == ZMODEM_IO_ERROR,
	    "directory purge error") && passed;
	passed = expect(close(directory_fd) == 0,"close directory descriptor") &&
	    passed;
	return passed;
}

static bool
test_frontend_hooks(void)
{
	char option[] = "-l/dev/null";
	char operand[] = "payload.bin";
	char * arguments[] = { (char *)"zmtx",option,operand,NULL };
	struct zmodem_plat_io plat_io;
	size_t option_index = 1U;
	bool passed = true;

	zmodem_plat_io_init(&plat_io,-1,-1);
	passed = expect(zmodem_plat_parse_option(&plat_io,ZMODEM_PLAT_ZMTX,
	    option,&option_index) == ZMODEM_PLAT_OPTION_ACCEPTED,
	    "accept platform line option") && passed;
	passed = expect(option_index == strlen(option) - 1U,
	    "consume platform line option") && passed;
	passed = expect(strcmp(plat_io.line,"/dev/null") == 0,
	    "retain platform line value") && passed;
	passed = expect(strcmp(option,"-l/dev/null") == 0,
	    "leave option string unchanged") && passed;
	passed = expect(strcmp(operand,"payload.bin") == 0,
	    "leave operand string unchanged") && passed;
	option_index = 1U;
	passed = expect(zmodem_plat_parse_option(&plat_io,ZMODEM_PLAT_ZMTX,
	    "-d",&option_index) == ZMODEM_PLAT_OPTION_NOT_HANDLED,
	    "leave generic option for application") && passed;
	passed = expect(zmodem_plat_post_parse(&plat_io,ZMODEM_PLAT_ZMTX,3,
	    arguments,2U) == 0,"configure platform after parsing") && passed;
	passed = expect(arguments[1] == option,
	    "leave option vector entry unchanged") && passed;
	passed = expect(arguments[2] == operand,
	    "leave operand vector entry unchanged") && passed;
	passed = expect(zmodem_plat_io_close(&plat_io) == 0,
	    "close frontend platform state") && passed;
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
	passed = test_frontend_hooks() && passed;
	passed = expect(zmodem_plat_ignore_sigpipe() == 0,
	    "ignore broken-pipe signal") && passed;
	return passed ? 0 : 1;
}

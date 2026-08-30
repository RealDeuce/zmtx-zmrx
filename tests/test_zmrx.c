#include "plat.h"
#include "zmodem_plat.h"

static unsigned test_receive_buffer_size;

#undef ZMODEM_PLAT_RECEIVE_BUFFER_SIZE
#define ZMODEM_PLAT_RECEIVE_BUFFER_SIZE(io) \
	((void)(io),test_receive_buffer_size)

#define main zmrx_application_main
#include "../zmrx.c"
#undef main

static bool
expect_receiver(bool condition,const char * description)
{
	if (!condition) {
		(void)fprintf(stderr,"test_zmrx: %s\n",description);
	}
	return condition;
}

static bool
test_cleanup_failure(void)
{
	char diagnostic[128];
	ssize_t length;
	int descriptors[2];
	int saved_stderr;
	bool passed = true;

	if (pipe(descriptors) != 0) {
		return expect_receiver(false,"create cleanup diagnostic pipe");
	}
	saved_stderr = dup(STDERR_FILENO);
	if (saved_stderr < 0 || dup2(descriptors[1],STDERR_FILENO) < 0) {
		if (saved_stderr >= 0) {
			(void)close(saved_stderr);
		}
		(void)close(descriptors[0]);
		(void)close(descriptors[1]);
		return expect_receiver(false,"redirect cleanup diagnostic");
	}
	zmodem_plat_io_init(&plat_io,-1,-1);
	plat_io.output_count = 1U;
	passed = expect_receiver(cleanup(0) == EXIT_CLEANUP_FAILED,
	    "cleanup failure changes successful status") && passed;
	passed = expect_receiver(cleanup(EXIT_TRANSFER_FAILED) ==
	    EXIT_TRANSFER_FAILED,"cleanup preserves transfer failure") && passed;
	passed = expect_receiver(dup2(saved_stderr,STDERR_FILENO) >= 0,
	    "restore cleanup diagnostic output") && passed;
	passed = expect_receiver(close(saved_stderr) == 0,
	    "close saved standard error") && passed;
	passed = expect_receiver(close(descriptors[1]) == 0,
	    "close cleanup diagnostic writer") && passed;
	length = read(descriptors[0],diagnostic,sizeof(diagnostic) - 1U);
	passed = expect_receiver(length > 0,"read cleanup diagnostic") && passed;
	if (length > 0) {
		diagnostic[(size_t)length] = '\0';
		passed = expect_receiver(strstr(diagnostic,
		    "zmrx: transfer line cleanup failed\n") != NULL,
		    "diagnose cleanup failure") && passed;
	}
	passed = expect_receiver(close(descriptors[0]) == 0,
	    "close cleanup diagnostic reader") && passed;
	return passed;
}

static int
hex_digit_value(uint8_t digit)
{
	if (digit >= '0' && digit <= '9') {
		return digit - '0';
	}
	return digit - 'a' + 10;
}

static bool
test_receive_buffer_case(unsigned reported,unsigned expected,
    const char * description)
{
	uint8_t output[32];
	struct zmodem_io io;
	ssize_t length;
	unsigned advertised;
	int descriptors[2];
	bool passed = true;

	if (pipe(descriptors) != 0) {
		return expect_receiver(false,"create receiver buffer pipe");
	}
	zmodem_plat_io_init(&plat_io,-1,descriptors[1]);
	zmodem_plat_io_bind(&io,&plat_io);
	passed = expect_receiver(zmodem_init(&protocol,&io) == ZMODEM_OK,
	    "initialize receiver buffer protocol") && passed;
	test_receive_buffer_size = reported;
	opt_s = true;
	passed = expect_receiver(tx_zrinit() == 0,description) && passed;
	length = read(descriptors[0],output,sizeof(output));
	passed = expect_receiver(length >= 10,
	    "read receiver buffer advertisement") && passed;
	if (length >= 10) {
		advertised = (unsigned)hex_digit_value(output[6]) << 4;
		advertised |= (unsigned)hex_digit_value(output[7]);
		advertised |= (unsigned)hex_digit_value(output[8]) << 12;
		advertised |= (unsigned)hex_digit_value(output[9]) << 8;
		passed = expect_receiver(advertised == expected,
		    "advertise selected receiver buffer size") && passed;
	}
	passed = expect_receiver(close(descriptors[0]) == 0,
	    "close receiver buffer reader") && passed;
	passed = expect_receiver(close(descriptors[1]) == 0,
	    "close receiver buffer writer") && passed;
	return passed;
}

static bool
test_receive_buffer_advertisement(void)
{
	bool passed = true;

	passed = test_receive_buffer_case(0U,ZMAXSPLEN,
	    "advertise fallback receiver buffer size") && passed;
	passed = test_receive_buffer_case(1024U,1024U,
	    "advertise platform receiver buffer size") && passed;
	passed = test_receive_buffer_case(ZMAXSPLEN + 1U,ZMAXSPLEN,
	    "clamp oversized receiver buffer size") && passed;
	opt_s = false;
	return passed;
}

int
main(void)
{
	bool passed = true;

	passed = test_cleanup_failure() && passed;
	passed = test_receive_buffer_advertisement() && passed;
	return passed ? 0 : 1;
}

#define _XOPEN_SOURCE 600

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
	zmodem_posix_io_init(&posix_io,-1,-1);
	posix_io.output_count = 1U;
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

int
main(void)
{
	return test_cleanup_failure() ? 0 : 1;
}

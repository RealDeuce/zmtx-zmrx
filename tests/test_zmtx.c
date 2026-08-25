#define _XOPEN_SOURCE 600

#define main zmtx_application_main
#include "../zmtx.c"
#undef main

struct sender_fake_io {
	int poll_results[2];
	size_t poll_count;
	size_t poll_index;
	int read_result;
};

static bool
expect_sender(bool condition,const char * description)
{
	if (!condition) {
		(void)fprintf(stderr,"test_zmtx: %s\n",description);
	}
	return condition;
}

static int
sender_fake_read(void * context,uint8_t * buffer,size_t capacity,
    size_t * count,int timeout_ms)
{
	const struct sender_fake_io * fake = context;

	(void)buffer;
	(void)capacity;
	(void)timeout_ms;
	*count = 0U;
	return fake->read_result;
}

static int
sender_fake_write(void * context,const uint8_t * buffer,size_t length)
{
	(void)context;
	(void)buffer;
	(void)length;
	return ZMODEM_OK;
}

static int
sender_fake_flush(void * context)
{
	(void)context;
	return ZMODEM_OK;
}

static int
sender_fake_poll(void * context)
{
	struct sender_fake_io * fake = context;

	if (fake->poll_index < fake->poll_count) {
		int result = fake->poll_results[fake->poll_index];

		fake->poll_index += 1U;
		return result;
	}
	return 0;
}

static int
sender_fake_purge(void * context)
{
	(void)context;
	return ZMODEM_OK;
}

static bool
initialize_sender(struct sender_fake_io * fake)
{
	struct zmodem_io io;

	(void)memset(fake,0,sizeof(*fake));
	fake->read_result = ZMODEM_TIMEOUT;
	io.context = fake;
	io.read = sender_fake_read;
	io.write = sender_fake_write;
	io.flush = sender_fake_flush;
	io.poll = sender_fake_poll;
	io.purge = sender_fake_purge;
	if (zmodem_init(&protocol,&io) != ZMODEM_OK) {
		return false;
	}
	protocol.can_full_duplex = true;
	protocol.can_overlap_io = true;
	opt_s = false;
	opt_v = false;
	opt_d = false;
	window_size = 0U;
	receiver_buffer_size = 0U;
	subpacket_size = ZBLOCKLEN;
	max_subpacket_size = ZBLOCKLEN;
	return true;
}

static int
open_sender_source(void)
{
	static const uint8_t contents[2U * ZBLOCKLEN] = { 0U };
	char path[] = "/tmp/zmtx-sender-test.XXXXXX";
	size_t offset = 0U;
	int fd = mkstemp(path);

	if (fd < 0) {
		return -1;
	}
	(void)unlink(path);
	while (offset < sizeof(contents)) {
		ssize_t result = write(fd,&contents[offset],sizeof(contents) - offset);

		if (result > 0) {
			offset += (size_t)result;
			continue;
		}
		if (result < 0 && errno == EINTR) {
			continue;
		}
		(void)close(fd);
		return -1;
	}
	if (lseek(fd,(off_t)0,SEEK_SET) != 0) {
		(void)close(fd);
		return -1;
	}
	return fd;
}

static bool
test_polled_failure(int poll_result,int read_result,const char * description)
{
	struct sender_fake_io fake;
	off_t position;
	int file_fd;
	int result;
	bool passed;

	if (!initialize_sender(&fake)) {
		return expect_sender(false,"initialize sender protocol");
	}
	fake.poll_results[0] = poll_result;
	fake.poll_count = 1U;
	fake.read_result = read_result;
	file_fd = open_sender_source();
	if (file_fd < 0) {
		return expect_sender(false,"open sender source");
	}
	result = send_from("source",file_fd);
	position = lseek(file_fd,(off_t)0,SEEK_CUR);
	passed = expect_sender(result == ZMODEM_IO_ERROR,description);
	passed = expect_sender(position == (off_t)ZBLOCKLEN,
	    "stop after the first data block") && passed;
	passed = expect_sender(close(file_fd) == 0,"close sender source") && passed;
	return passed;
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
		return expect_sender(false,"create cleanup diagnostic pipe");
	}
	saved_stderr = dup(STDERR_FILENO);
	if (saved_stderr < 0 || dup2(descriptors[1],STDERR_FILENO) < 0) {
		if (saved_stderr >= 0) {
			(void)close(saved_stderr);
		}
		(void)close(descriptors[0]);
		(void)close(descriptors[1]);
		return expect_sender(false,"redirect cleanup diagnostic");
	}
	zmodem_posix_io_init(&posix_io,-1,-1);
	posix_io.output_count = 1U;
	passed = expect_sender(cleanup(0) == EXIT_CLEANUP_FAILED,
	    "cleanup failure changes successful status") && passed;
	passed = expect_sender(cleanup(EXIT_TRANSFER_FAILED) ==
	    EXIT_TRANSFER_FAILED,"cleanup preserves transfer failure") && passed;
	passed = expect_sender(dup2(saved_stderr,STDERR_FILENO) >= 0,
	    "restore cleanup diagnostic output") && passed;
	passed = expect_sender(close(saved_stderr) == 0,
	    "close saved standard error") && passed;
	passed = expect_sender(close(descriptors[1]) == 0,
	    "close cleanup diagnostic writer") && passed;
	length = read(descriptors[0],diagnostic,sizeof(diagnostic) - 1U);
	passed = expect_sender(length > 0,"read cleanup diagnostic") && passed;
	if (length > 0) {
		diagnostic[(size_t)length] = '\0';
		passed = expect_sender(strstr(diagnostic,
		    "zmtx: transfer line cleanup failed\n") != NULL,
		    "diagnose cleanup failure") && passed;
	}
	passed = expect_sender(close(descriptors[0]) == 0,
	    "close cleanup diagnostic reader") && passed;
	return passed;
}

int
main(void)
{
	bool passed = true;

	passed = test_polled_failure(ZMODEM_IO_ERROR,ZMODEM_TIMEOUT,
	    "propagate poll failure") && passed;
	passed = test_polled_failure(1,ZMODEM_IO_ERROR,
	    "propagate read failure") && passed;
	passed = test_cleanup_failure() && passed;
	return passed ? 0 : 1;
}

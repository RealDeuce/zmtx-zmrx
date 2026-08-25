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

int
main(void)
{
	bool passed = true;

	passed = test_polled_failure(ZMODEM_IO_ERROR,ZMODEM_TIMEOUT,
	    "propagate poll failure") && passed;
	passed = test_polled_failure(1,ZMODEM_IO_ERROR,
	    "propagate read failure") && passed;
	return passed ? 0 : 1;
}

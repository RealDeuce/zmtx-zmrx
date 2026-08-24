#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "zmdm.h"
#include "zmodem.h"

struct fuzz_io {
	const uint8_t * input;
	size_t length;
	size_t offset;
};

static int
fuzz_read(void * context,uint8_t * buffer,size_t capacity,size_t * count,
    int timeout_ms)
{
	struct fuzz_io * io = context;
	size_t available = io->length - io->offset;

	(void)timeout_ms;
	if (available == 0U) {
		*count = 0U;
		return ZMODEM_TIMEOUT;
	}
	if (available > capacity) {
		available = capacity;
	}
	(void)memcpy(buffer,&io->input[io->offset],available);
	io->offset += available;
	*count = available;
	return ZMODEM_OK;
}

static int
fuzz_write(void * context,const uint8_t * buffer,size_t length)
{
	(void)context;
	(void)buffer;
	(void)length;
	return ZMODEM_OK;
}

static int
fuzz_flush(void * context)
{
	(void)context;
	return ZMODEM_OK;
}

static int
fuzz_poll(void * context)
{
	const struct fuzz_io * io = context;

	return (io->offset < io->length) ? 1 : 0;
}

static int
fuzz_purge(void * context)
{
	struct fuzz_io * io = context;

	io->offset = io->length;
	return ZMODEM_OK;
}

static void
initialize(struct zmodem * protocol,struct fuzz_io * input,
    const uint8_t * data,size_t size)
{
	struct zmodem_io io;

	input->input = data;
	input->length = size;
	input->offset = 0U;
	io.context = input;
	io.read = fuzz_read;
	io.write = fuzz_write;
	io.flush = fuzz_flush;
	io.poll = fuzz_poll;
	io.purge = fuzz_purge;
	(void)zmodem_init(protocol,&io);
}

int
LLVMFuzzerTestOneInput(const uint8_t * data,size_t size)
{
	struct zmodem protocol;
	struct fuzz_io input;
	uint8_t packet[ZMAXSPLEN];
	size_t packet_length;
	uint8_t frame_end;
	uint8_t selector;

	if (size == 0U) {
		return 0;
	}
	selector = data[0];
	data += 1U;
	size -= 1U;
	initialize(&protocol,&input,data,size);

	switch (selector % 4U) {
		case 0U:
			(void)rx_header(&protocol,1);
			break;
		case 1U:
			protocol.receive_32_bit_data = false;
			(void)rx_data(&protocol,packet,sizeof(packet),&packet_length,
			    &frame_end);
			break;
		case 2U:
			protocol.receive_32_bit_data = true;
			(void)rx_data(&protocol,packet,sizeof(packet),&packet_length,
			    &frame_end);
			break;
		default:
			protocol.can_fcs_32 = (selector & 0x80U) != 0U;
			if (size > sizeof(packet)) {
				size = sizeof(packet);
			}
			(void)tx_data(&protocol,ZCRCE,data,size);
			break;
	}
	return 0;
}

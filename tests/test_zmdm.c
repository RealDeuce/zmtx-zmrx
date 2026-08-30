#include "plat.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "crctab.h"
#include "zmdm.h"
#include "zmodem.h"

#define TEST_BUFFER_CAPACITY (ZMODEM_TX_DATA_WIRE_CAPACITY + 32U)

struct fake_io {
	uint8_t input[TEST_BUFFER_CAPACITY];
	size_t input_length;
	size_t input_offset;
	uint8_t output[TEST_BUFFER_CAPACITY];
	size_t output_length;
	int empty_read_result;
	int write_result;
	int flush_result;
	size_t write_calls;
	size_t fail_write_call;
	bool override_read_count;
	size_t reported_read_count;
	int poll_result;
	int purge_result;
};

static int
fake_read(void * context,uint8_t * buffer,size_t capacity,size_t * count,
    int timeout_ms)
{
	struct fake_io * io = context;
	size_t available = io->input_length - io->input_offset;

	(void)timeout_ms;
	if (io->override_read_count) {
		*count = io->reported_read_count;
		return ZMODEM_OK;
	}
	if (available == 0U) {
		*count = 0U;
		return io->empty_read_result;
	}
	if (available > capacity) {
		available = capacity;
	}
	(void)memcpy(buffer,&io->input[io->input_offset],available);
	io->input_offset += available;
	*count = available;
	return ZMODEM_OK;
}

static int
fake_write(void * context,const uint8_t * buffer,size_t length)
{
	struct fake_io * io = context;

	io->write_calls += 1U;
	if ((io->fail_write_call != 0U) &&
	    (io->write_calls == io->fail_write_call)) {
		return ZMODEM_IO_ERROR;
	}
	if (io->write_result != ZMODEM_OK) {
		return io->write_result;
	}
	if (length > sizeof(io->output) - io->output_length) {
		return ZMODEM_IO_ERROR;
	}
	(void)memcpy(&io->output[io->output_length],buffer,length);
	io->output_length += length;
	return ZMODEM_OK;
}

static int
fake_flush(void * context)
{
	const struct fake_io * io = context;

	return io->flush_result;
}

static int
fake_poll(void * context)
{
	const struct fake_io * io = context;

	return io->poll_result;
}

static int
fake_purge(void * context)
{
	struct fake_io * io = context;

	io->input_offset = io->input_length;
	return io->purge_result;
}

static void
initialize(struct zmodem * protocol,struct fake_io * fake)
{
	struct zmodem_io io;

	(void)memset(fake,0,sizeof(*fake));
	fake->empty_read_result = ZMODEM_TIMEOUT;
	io.context = fake;
	io.read = fake_read;
	io.write = fake_write;
	io.flush = fake_flush;
	io.poll = fake_poll;
	io.purge = fake_purge;
	if (zmodem_init(protocol,&io) != ZMODEM_OK) {
		(void)fprintf(stderr,"test_zmdm: protocol initialization failed\n");
	}
}

static bool
expect(bool condition,const char * description)
{
	if (!condition) {
		(void)fprintf(stderr,"test_zmdm: %s\n",description);
	}
	return condition;
}

static size_t
append_escaped(uint8_t * output,size_t offset,uint8_t byte)
{
	if ((byte == ZDLE) || (byte == 0x10U) || (byte == 0x90U) ||
	    (byte == XON) || (byte == 0x91U) || (byte == XOFF) ||
	    (byte == 0x93U)) {
		output[offset] = ZDLE;
		output[offset + 1U] = (uint8_t)(byte ^ 0x40U);
		return offset + 2U;
	}
	output[offset] = byte;
	return offset + 1U;
}

static size_t
append_legacy_escape8(uint8_t * output,size_t offset,uint8_t byte)
{
	if ((byte & UINT8_C(0x80)) != 0U) {
		output[offset++] = ZDLE;
		output[offset++] = byte ^ UINT8_C(0x40);
		return offset;
	}
	return append_escaped(output,offset,byte);
}

static bool
test_header_position(void)
{
	uint8_t header[HDRLEN] = { 0U };

	zmodem_set_header_position(header,UINT32_C(0x89abcdef));
	return expect(header[ZP0] == 0xefU,"low position byte") &&
	    expect(header[ZP1] == 0xcdU,"second position byte") &&
	    expect(header[ZP2] == 0xabU,"third position byte") &&
	    expect(header[ZP3] == 0x89U,"high position byte") &&
	    expect(zmodem_header_position(header) == UINT32_C(0x89abcdef),
	    "position round trip");
}

static bool
test_transmit_hex_header(void)
{
	static const uint8_t expected[] = {
		ZPAD,ZPAD,ZDLE,ZHEX,
		'0','0','0','0','0','0','0','0','0','0',
		'0','0','0','0',CR,LF,XON
	};
	struct zmodem protocol;
	struct fake_io fake;
	uint8_t header[HDRLEN] = { ZRQINIT,0U,0U,0U,0U };

	initialize(&protocol,&fake);
	return expect(tx_hex_header(&protocol,header) == 0,
	    "transmit hex header result") &&
	    expect(fake.output_length == sizeof(expected),
	    "transmit hex header length") &&
	    expect(memcmp(fake.output,expected,sizeof(expected)) == 0,
	    "transmit hex header bytes");
}

static bool
test_receive_hex_header(void)
{
	static const uint8_t input[] = {
		ZPAD,ZPAD,ZDLE,ZHEX,
		'0','0','0','0','0','0','0','0','0','0',
		'0','0','0','0',CR,LF,XON
	};
	struct zmodem protocol;
	struct fake_io fake;

	initialize(&protocol,&fake);
	(void)memcpy(fake.input,input,sizeof(input));
	fake.input_length = sizeof(input);
	return expect(rx_header(&protocol,1000) == ZRQINIT,
	    "receive hex header") &&
	    expect(protocol.rxd_header_len == HDRLEN,"received header length");
}

static bool
test_receive_empty_data(void)
{
	struct zmodem protocol;
	struct fake_io fake;
	uint8_t data[1];
	uint8_t frame_end;
	size_t length;
	size_t offset = 0U;
	uint16_t crc = 0U;

	initialize(&protocol,&fake);
	crc = crc16_update(crc,ZCRCE);
	crc = crc16_update(crc,0U);
	crc = crc16_update(crc,0U);
	fake.input[offset++] = ZDLE;
	fake.input[offset++] = XON;
	fake.input[offset++] = XOFF;
	fake.input[offset++] = UINT8_C(0x91);
	fake.input[offset++] = UINT8_C(0x93);
	fake.input[offset++] = ZDLE;
	fake.input[offset++] = ZCRCE;
	offset = append_escaped(fake.input,offset,(uint8_t)(crc >> 8));
	offset = append_escaped(fake.input,offset,(uint8_t)crc);
	fake.input_length = offset;

	return expect(rx_data(&protocol,data,sizeof(data),&length,&frame_end) ==
	    ENDOFFRAME,"receive empty data result") &&
	    expect(length == 0U,"receive empty data length") &&
	    expect(frame_end == ZCRCE,"receive empty data frame end");
}

static bool
test_transport_errors(void)
{
	struct zmodem protocol;
	struct fake_io fake;
	uint8_t header[HDRLEN] = { ZRQINIT,0U,0U,0U,0U };
	size_t index;

	initialize(&protocol,&fake);
	fake.write_result = ZMODEM_IO_ERROR;
	if (!expect(tx_hex_header(&protocol,header) != 0,"write error")) {
		return false;
	}
	fake.write_result = ZMODEM_OK;
	fake.empty_read_result = ZMODEM_IO_ERROR;
	if (!expect(rx_raw(&protocol,1000) == ZMODEM_IO_ERROR,"read error")) {
		return false;
	}
	initialize(&protocol,&fake);
	for (index = 0U; index < 5U; index++) {
		fake.input[index] = CAN;
	}
	fake.input_length = 5U;
	for (index = 0U; index < 4U; index++) {
		if (!expect(rx_raw(&protocol,1000) == CAN,"cancel prefix")) {
			return false;
		}
	}
	return expect(rx_raw(&protocol,1000) == ZMODEM_CANCELLED,
	    "cancel status");
}

static bool
test_initialization_and_buffering(void)
{
	struct zmodem protocol;
	struct fake_io fake;
	struct zmodem_io io;
	bool passed = true;

	io.context = &fake;
	io.read = fake_read;
	io.write = fake_write;
	io.flush = fake_flush;
	io.poll = fake_poll;
	io.purge = fake_purge;
	passed = expect(zmodem_init(NULL,&io) == ZMODEM_INVALID_ARGUMENT,
	    "reject null state") && passed;
	passed = expect(zmodem_init(&protocol,NULL) == ZMODEM_INVALID_ARGUMENT,
	    "reject null transport") && passed;
	io.read = NULL;
	passed = expect(zmodem_init(&protocol,&io) == ZMODEM_INVALID_ARGUMENT,
	    "reject missing read callback") && passed;
	io.read = fake_read;
	io.write = NULL;
	passed = expect(zmodem_init(&protocol,&io) == ZMODEM_INVALID_ARGUMENT,
	    "reject missing write callback") && passed;
	io.write = fake_write;
	io.flush = NULL;
	passed = expect(zmodem_init(&protocol,&io) == ZMODEM_INVALID_ARGUMENT,
	    "reject missing flush callback") && passed;
	io.flush = fake_flush;
	io.poll = NULL;
	passed = expect(zmodem_init(&protocol,&io) == ZMODEM_INVALID_ARGUMENT,
	    "reject missing poll callback") && passed;
	io.poll = fake_poll;
	io.purge = NULL;
	passed = expect(zmodem_init(&protocol,&io) == ZMODEM_INVALID_ARGUMENT,
	    "reject missing purge callback") && passed;

	initialize(&protocol,&fake);
	fake.input[0] = UINT8_C(0x55);
	fake.input[1] = UINT8_C(0xaa);
	fake.input_length = 2U;
	passed = expect(rx_raw(&protocol,1000) == 0x55,"first buffered byte") &&
	    passed;
	passed = expect(rx_poll(&protocol) == 1,"buffered poll") && passed;
	passed = expect(rx_purge(&protocol) == ZMODEM_OK,"purge callback") &&
	    passed;
	passed = expect(rx_poll(&protocol) == 0,"poll after purge") && passed;
	fake.purge_result = ZMODEM_IO_ERROR;
	passed = expect(rx_purge(&protocol) == ZMODEM_IO_ERROR,
	    "purge error") && passed;
	fake.flush_result = ZMODEM_IO_ERROR;
	passed = expect(tx_flush(&protocol) != 0,"flush error") && passed;
	initialize(&protocol,&fake);
	fake.override_read_count = true;
	passed = expect(rx_raw(&protocol,1000) == ZMODEM_IO_ERROR,
	    "reject zero successful read") && passed;
	fake.reported_read_count = sizeof(protocol.input_buffer) + 1U;
	passed = expect(rx_raw(&protocol,1000) == ZMODEM_IO_ERROR,
	    "reject oversized successful read") && passed;
	return passed;
}

static bool
data_round_trip(bool use_crc32,bool escape_controls,uint8_t sent_frame_end,
    int expected_result)
{
	static const uint8_t payload[] = {
		'@',CR,ZDLE,XON,UINT8_C(0x00),UINT8_C(0x80),UINT8_C(0xff)
	};
	struct zmodem sender;
	struct zmodem receiver;
	struct fake_io sending_io;
	struct fake_io receiving_io;
	uint8_t received[sizeof(payload)];
	uint8_t frame_end;
	size_t length;
	unsigned prebuffered;
	bool passed = true;

	initialize(&sender,&sending_io);
	sender.can_fcs_32 = use_crc32;
	sender.want_fcs_32 = use_crc32;
	sender.escape_all_control_characters = escape_controls;
	if (!expect(tx_data(&sender,sent_frame_end,payload,sizeof(payload)) == 0,
	    "transmit data packet")) {
		return false;
	}
	for (prebuffered = 0U;prebuffered < 2U;prebuffered++) {
		size_t prefix = prebuffered != 0U ? 4U : 0U;

		initialize(&receiver,&receiving_io);
		receiver.receive_32_bit_data = use_crc32;
		receiver.receive_escaped_control_characters = escape_controls;
		if (prefix > sending_io.output_length) {
			prefix = sending_io.output_length;
		}
		(void)memcpy(receiver.input_buffer,sending_io.output,prefix);
		receiver.input_count = prefix;
		(void)memcpy(receiving_io.input,&sending_io.output[prefix],
		    sending_io.output_length - prefix);
		receiving_io.input_length = sending_io.output_length - prefix;
		passed = expect(rx_data(&receiver,received,sizeof(received),&length,
		    &frame_end) == expected_result,"receive data packet") && passed;
		passed = expect(length == sizeof(payload),"data packet length") &&
		    passed;
		passed = expect(frame_end == sent_frame_end,
		    "data packet terminator") && passed;
		passed = expect(memcmp(received,payload,sizeof(payload)) == 0,
		    "data packet payload") && passed;
	}
	return passed;
}

static bool
test_control_character_escaping(void)
{
	static const uint8_t leading_plain[] = { 'A' };
	static const uint8_t leading_cr[] = { CR };
	static const uint8_t payload[] = {
		'A',CR,'A',UINT8_C(0x8d),'@',CR,UINT8_C(0xc0),UINT8_C(0x8d)
	};
	static const uint8_t expected_normal[] = {
		'A',CR,'A',UINT8_C(0x8d),'@',ZDLE,'M',UINT8_C(0xc0),
		ZDLE,UINT8_C(0xcd)
	};
	static const uint8_t expected_escctl[] = {
		'A',ZDLE,'M','A',ZDLE,UINT8_C(0xcd),'@',ZDLE,'M',
		UINT8_C(0xc0),ZDLE,UINT8_C(0xcd)
	};
	static const uint8_t boundary_payload[] = {
		'A','A','A','@',CR,'A','A',UINT8_C(0xc0),UINT8_C(0x8d),'A'
	};
	static const uint8_t expected_boundary[] = {
		'A','A','A','@',ZDLE,'M','A','A',UINT8_C(0xc0),ZDLE,
		UINT8_C(0xcd),'A'
	};
	struct zmodem protocol;
	struct fake_io fake;
	bool passed = true;

	initialize(&protocol,&fake);
	passed = expect(tx_raw(&protocol,'@') == 0,
	    "transmit context before leading plain byte") && passed;
	passed = expect(tx_data(&protocol,ZCRCE,leading_plain,
	    sizeof(leading_plain)) == 0,
	    "transmit leading plain byte after at sign") && passed;
	passed = expect(fake.output[1] == 'A',
	    "leave leading plain byte unescaped after at sign") && passed;
	initialize(&protocol,&fake);
	passed = expect(tx_raw(&protocol,'@') == 0,
	    "transmit context before leading CR") && passed;
	passed = expect(tx_data(&protocol,ZCRCE,leading_cr,sizeof(leading_cr)) == 0,
	    "transmit leading CR after at sign") && passed;
	passed = expect(fake.output[1] == ZDLE,
	    "prefix leading CR after at sign with ZDLE") && passed;
	passed = expect(fake.output[2] == 'M',
	    "encode leading CR after at sign") && passed;

	initialize(&protocol,&fake);
	passed = expect(tx_data(&protocol,ZCRCE,payload,sizeof(payload)) == 0,
	    "transmit normal conditional CR escapes") && passed;
	passed = expect(fake.output_length >= sizeof(expected_normal),
	    "normal conditional CR wire length") && passed;
	passed = expect(memcmp(fake.output,expected_normal,
	    sizeof(expected_normal)) == 0,"normal conditional CR wire bytes") &&
	    passed;
	initialize(&protocol,&fake);
	passed = expect(tx_data(&protocol,ZCRCE,boundary_payload,
	    sizeof(boundary_payload)) == 0,"transmit word-boundary CR escapes") &&
	    passed;
	passed = expect(fake.output_length >= sizeof(expected_boundary),
	    "word-boundary CR wire length") && passed;
	passed = expect(memcmp(fake.output,expected_boundary,
	    sizeof(expected_boundary)) == 0,"word-boundary CR wire bytes") &&
	    passed;

	initialize(&protocol,&fake);
	protocol.escape_all_control_characters = true;
	passed = expect(tx_data(&protocol,ZCRCE,payload,sizeof(payload)) == 0,
	    "transmit ESCCTL CR escapes") && passed;
	passed = expect(fake.output_length >= sizeof(expected_escctl),
	    "ESCCTL CR wire length") && passed;
	passed = expect(memcmp(fake.output,expected_escctl,
	    sizeof(expected_escctl)) == 0,"ESCCTL CR wire bytes") && passed;
	return passed;
}

static bool
test_receive_control_enforcement(void)
{
	static const uint8_t payload[] = { 'A',CR,'B' };
	static const uint8_t clean_payload[] = { 'A','B' };
	struct zmodem sender;
	struct zmodem receiver;
	struct fake_io sending_io;
	struct fake_io receiving_io;
	uint8_t received[sizeof(payload)];
	uint8_t frame_end;
	size_t length;
	bool passed = true;

	initialize(&sender,&sending_io);
	passed = expect(tx_data(&sender,ZCRCE,payload,sizeof(payload)) == 0,
	    "transmit raw control packet") && passed;
	initialize(&receiver,&receiving_io);
	receiver.receive_escaped_control_characters = true;
	(void)memcpy(receiving_io.input,sending_io.output,
	    sending_io.output_length);
	receiving_io.input_length = sending_io.output_length;
	passed = expect(rx_data(&receiver,received,sizeof(received),&length,
	    &frame_end) == INVDATA,"reject under-escaped control packet") &&
	    passed;

	initialize(&sender,&sending_io);
	sender.escape_all_control_characters = true;
	passed = expect(tx_data(&sender,ZCRCE,clean_payload,
	    sizeof(clean_payload)) == 0,"transmit clean control packet") && passed;
	initialize(&receiver,&receiving_io);
	receiver.receive_escaped_control_characters = true;
	receiving_io.input[0] = sending_io.output[0];
	receiving_io.input[1] = CR;
	(void)memcpy(&receiving_io.input[2],&sending_io.output[1],
	    sending_io.output_length - 1U);
	receiving_io.input_length = sending_io.output_length + 1U;
	passed = expect(rx_data(&receiver,received,sizeof(received),&length,
	    &frame_end) == ENDOFFRAME,"ignore spurious raw control") && passed;
	passed = expect(length == sizeof(clean_payload),
	    "spurious control packet length") && passed;
	passed = expect(memcmp(received,clean_payload,sizeof(clean_payload)) == 0,
	    "spurious control packet payload") && passed;
	return passed;
}

static bool
test_escctl_hex_header_terminator(void)
{
	struct zmodem sender;
	struct zmodem receiver;
	struct fake_io sending_io;
	struct fake_io receiving_io;
	uint8_t header[HDRLEN] = { ZRQINIT,0U,0U,0U,0U };
	bool passed;

	initialize(&sender,&sending_io);
	passed = expect(tx_hex_header(&sender,header) == 0,
	    "transmit ESCCTL hex header");
	initialize(&receiver,&receiving_io);
	receiver.receive_escaped_control_characters = true;
	(void)memcpy(receiving_io.input,sending_io.output,
	    sending_io.output_length);
	receiving_io.input_length = sending_io.output_length;
	passed = expect(rx_header_and_check(&receiver,1000) == ZRQINIT,
	    "preserve ESCCTL hex header terminator") && passed;
	return passed;
}

static bool
test_span_scanner_round_trip(void)
{
	struct zmodem sender;
	struct zmodem receiver;
	struct fake_io sending_io;
	struct fake_io receiving_io;
	uint8_t payload[37];
	uint8_t received[sizeof(payload)];
	uint8_t frame_end;
	size_t length;
	size_t index;
	bool passed;

	for (index = 0U; index < sizeof(payload); index++) {
		payload[index] = (uint8_t)(UINT8_C(0x40) + index);
	}
	payload[9] = ZDLE;
	payload[20] = XON;
	payload[31] = UINT8_C(0x90);

	initialize(&sender,&sending_io);
	passed = expect(tx_data(&sender,ZCRCE,payload,sizeof(payload)) == 0,
	    "transmit span scanner packet");

	initialize(&receiver,&receiving_io);
	(void)memcpy(receiving_io.input,sending_io.output,
	    sending_io.output_length);
	receiving_io.input_length = sending_io.output_length;
	passed = expect(rx_data(&receiver,received,sizeof(received),&length,
	    &frame_end) == ENDOFFRAME,"receive span scanner packet") && passed;
	passed = expect(length == sizeof(payload),"span scanner packet length") &&
	    passed;
	passed = expect(frame_end == ZCRCE,"span scanner packet terminator") &&
	    passed;
	passed = expect(memcmp(received,payload,sizeof(payload)) == 0,
	    "span scanner packet payload") && passed;
	return passed;
}

static bool
test_maximum_escaped_data_round_trip(bool use_crc32)
{
	static const uint8_t escaped_bytes[] = {
		ZDLE,UINT8_C(0x10),UINT8_C(0x90),XON,UINT8_C(0x91),
		XOFF,UINT8_C(0x93)
	};
	static struct zmodem sender;
	static struct zmodem receiver;
	static struct fake_io sending_io;
	static struct fake_io receiving_io;
	static uint8_t payload[ZMAXSPLEN];
	static uint8_t received[ZMAXSPLEN];
	uint8_t frame_end;
	size_t index;
	size_t length;
	bool passed;

	for (index = 0U; index < sizeof(payload); index++) {
		payload[index] = escaped_bytes[index % sizeof(escaped_bytes)];
	}
	initialize(&sender,&sending_io);
	sender.can_fcs_32 = use_crc32;
	sender.want_fcs_32 = use_crc32;
	passed = expect(tx_data(&sender,ZCRCE,payload,sizeof(payload)) == 0,
	    "transmit maximum escaped packet");
	passed = expect(sending_io.output_length >= 2U * sizeof(payload),
	    "maximum packet escape-heavy wire length") && passed;

	initialize(&receiver,&receiving_io);
	receiver.receive_32_bit_data = use_crc32;
	(void)memcpy(receiving_io.input,sending_io.output,
	    sending_io.output_length);
	receiving_io.input_length = sending_io.output_length;
	passed = expect(rx_data(&receiver,received,sizeof(received),&length,
	    &frame_end) == ENDOFFRAME,
	    "receive maximum escaped packet") && passed;
	passed = expect(length == sizeof(payload),
	    "maximum escaped packet length") && passed;
	passed = expect(frame_end == ZCRCE,
	    "maximum escaped packet terminator") && passed;
	passed = expect(memcmp(received,payload,sizeof(payload)) == 0,
	    "maximum escaped packet payload") && passed;
	return passed;
}

static bool
test_eighth_bit_escaping(void)

{
	uint8_t payload[256];
	struct zmodem sender;
	struct zmodem receiver;
	struct fake_io sending_io;
	struct fake_io receiving_io;
	uint8_t received[sizeof(payload)];
	uint8_t frame_end;
	size_t length;
	size_t index;
	unsigned value;
	bool passed;
	bool found_resc_quote = false;
	bool seven_bit_wire = true;

	for (value=0;value<sizeof(payload);value++) {
		payload[value] = (uint8_t)value;
	}
	initialize(&sender,&sending_io);
	sender.escape_8th_bit = true;
	passed = expect(tx_data(&sender,ZCRCE,payload,sizeof(payload)) == 0,
	    "transmit with eighth-bit escaping");
	passed = expect(sending_io.output_length >= 4U,
	    "eighth-bit escaped wire length") && passed;
	for (index=0U;index + 1U<sending_io.output_length;index++) {
		if ((sending_io.output[index] & UINT8_C(0x80)) != 0U) {
			seven_bit_wire = false;
		}
		if (sending_io.output[index] == ZRESC &&
		    sending_io.output[index + 1U] == UINT8_C(0x40)) {
			found_resc_quote = true;
		}
	}
	if ((sending_io.output[sending_io.output_length - 1U] &
	    UINT8_C(0x80)) != 0U) {
		seven_bit_wire = false;
	}
	passed = expect(seven_bit_wire,"Omen data wire is seven-bit clean") &&
	    passed;
	passed = expect(found_resc_quote,"RLE quote for literal ZRESC") &&
	    passed;
	passed = expect(memchr(sending_io.output,SO,
	    sending_io.output_length) != NULL,
	    "SO prefixes ordinary high-bit values") && passed;
	passed = expect(memchr(sending_io.output,UINT8_C(0x73),
	    sending_io.output_length) != NULL,
	    "private escape represents 0x80") && passed;

	initialize(&receiver,&receiving_io);
	receiver.receive_escape8_format = ZMODEM_ESCAPE8_OMEN;
	receiver.receive_encoded_data = ZMODEM_ENCODED_DATA_RLE;
	(void)memcpy(receiving_io.input,sending_io.output,
	    sending_io.output_length);
	receiving_io.input_length = sending_io.output_length;
	passed = expect(rx_data(&receiver,received,sizeof(received),&length,
	    &frame_end) == ENDOFFRAME,"receive eighth-bit escaped packet") &&
	    passed;
	passed = expect(length == sizeof(payload),
	    "eighth-bit escaped packet length") && passed;
	passed = expect(frame_end == ZCRCE,
	    "eighth-bit escaped packet terminator") && passed;
	passed = expect(memcmp(received,payload,sizeof(payload)) == 0,
	    "eighth-bit escaped packet payload") && passed;
	return passed;
}

static bool
test_omen_header_round_trip(void)
{
	static const uint8_t header[HDRLEN] = {
		ZDATA,UINT8_C(0x80),SO,ZDLE,UINT8_C(0xff)
	};
	struct zmodem sender;
	struct zmodem receiver;
	struct fake_io sending_io;
	struct fake_io receiving_io;
	size_t i;
	bool passed;
	bool seven_bit_wire = true;

	initialize(&sender,&sending_io);
	sender.escape_8th_bit = true;
	passed = expect(tx_header(&sender,header) == 0,
	    "transmit Omen ESC8 header");
	passed = expect(sending_io.output_length > 4U,
	    "Omen ESC8 header length") && passed;
	passed = expect(sending_io.output[0] == ZPAD,
	    "Omen ESC8 header pad") && passed;
	passed = expect(sending_io.output[1] == ZDLE,
	    "Omen ESC8 header escape") && passed;
	passed = expect(sending_io.output[2] == ZBINR32ESC8,
	    "Omen ESC8 header indicator") && passed;
	passed = expect(sending_io.output[3] == UINT8_C(0x26),
	    "Omen ESC8 header parameter count") && passed;
	for (i=0U;i<sending_io.output_length;i++) {
		if ((sending_io.output[i] & UINT8_C(0x80)) != 0U) {
			seven_bit_wire = false;
		}
	}
	passed = expect(seven_bit_wire,"Omen header wire is seven-bit clean") &&
	    passed;

	initialize(&receiver,&receiving_io);
	(void)memcpy(receiving_io.input,sending_io.output,
	    sending_io.output_length);
	receiving_io.input_length = sending_io.output_length;
	passed = expect(rx_header(&receiver,1000) == ZDATA,
	    "receive Omen ESC8 header") && passed;
	passed = expect(receiver.rxd_header_len == HDRLEN,
	    "Omen ESC8 header length") && passed;
	passed = expect(receiver.receive_32_bit_data,
	    "Omen ESC8 selects CRC32") && passed;
	passed = expect(receiver.receive_escape8_format == ZMODEM_ESCAPE8_OMEN,
	    "Omen ESC8 selects data decoder") && passed;
	passed = expect(memcmp(receiver.rxd_header,header,sizeof(header)) == 0,
	    "Omen ESC8 header bytes") && passed;
	return passed;
}

static bool
test_omen_rle_round_trip(void)
{
	static const uint8_t streaming_ends[] = { ZCRCG,ZCRCQ };
	struct zmodem sender;
	struct zmodem receiver;
	struct fake_io sending_io;
	struct fake_io receiving_io;
	uint8_t payload[160];
	uint8_t received[sizeof(payload)];
	uint8_t frame_end;
	size_t payload_length = 0U;
	size_t length;
	size_t run;
	bool passed;

	payload[payload_length++] = 'A';
	payload[payload_length++] = 'A';
	payload[payload_length++] = ZRESC;
	payload[payload_length++] = ZRESC;
	payload[payload_length++] = UINT8_C(0x81);
	payload[payload_length++] = UINT8_C(0x81);
	for (run=0U;run<3U;run++) {
		payload[payload_length++] = UINT8_C(0x20);
	}
	payload[payload_length++] = 'C';
	for (run=0U;run<35U;run++) {
		payload[payload_length++] = UINT8_C(0x20);
	}
	for (run=0U;run<64U;run++) {
		payload[payload_length++] = 'B';
	}
	payload[payload_length++] = UINT8_C(0x82);

	initialize(&sender,&sending_io);
	sender.escape_8th_bit = true;
	sender.escape_all_control_characters = true;
	passed = expect(tx_data(&sender,ZCRCW,payload,payload_length) == 0,
	    "transmit Omen RLE variants");

	initialize(&receiver,&receiving_io);
	receiver.receive_escape8_format = ZMODEM_ESCAPE8_OMEN;
	receiver.receive_encoded_data = ZMODEM_ENCODED_DATA_RLE;
	receiver.receive_escaped_control_characters = true;
	receiving_io.input[0] = XON;
	receiving_io.input[1] = UINT8_C(1);
	(void)memcpy(&receiving_io.input[2],sending_io.output,
	    sending_io.output_length);
	receiving_io.input_length = sending_io.output_length + 2U;
	passed = expect(rx_data(&receiver,received,sizeof(received),&length,
	    &frame_end) == ENDOFFRAME,"receive Omen RLE variants") && passed;
	passed = expect(length == payload_length,"Omen RLE variant length") &&
	    passed;
	passed = expect(frame_end == ZCRCW,"Omen RLE variant terminator") &&
	    passed;
	passed = expect(memcmp(received,payload,payload_length) == 0,
	    "Omen RLE variant payload") && passed;
	for (run=0U;run<sizeof(streaming_ends);run++) {
		initialize(&sender,&sending_io);
		sender.escape_8th_bit = true;
		passed = expect(tx_data(&sender,streaming_ends[run],payload,
		    payload_length) == 0,"transmit streaming Omen RLE frame") &&
		    passed;
		initialize(&receiver,&receiving_io);
		receiver.receive_escape8_format = ZMODEM_ESCAPE8_OMEN;
		receiver.receive_encoded_data = ZMODEM_ENCODED_DATA_RLE;
		(void)memcpy(receiving_io.input,sending_io.output,
		    sending_io.output_length);
		receiving_io.input_length = sending_io.output_length;
		passed = expect(rx_data(&receiver,received,sizeof(received),&length,
		    &frame_end) == FRAMEOK,"receive streaming Omen RLE frame") &&
		    passed;
		passed = expect(frame_end == streaming_ends[run],
		    "streaming Omen RLE terminator") && passed;
	}
	return passed;
}

static bool
test_pack7_round_trip(void)

{
	static const uint8_t header[HDRLEN] = {
		ZDATA,UINT8_C(0x80),SO,ZDLE,UINT8_C(0xff)
	};
	static const uint8_t full_group[] = {
		UINT8_C(0x69),UINT8_C(0x58),UINT8_C(0x4c),
		UINT8_C(0x60),UINT8_C(0x51)
	};
	static const uint8_t partial_group[] = {
		UINT8_C(0x22),UINT8_C(0x22),UINT8_C(0x24),UINT8_C(0x74),
		UINT8_C(0x21)
	};
	struct zmodem sender;
	struct zmodem receiver;
	struct fake_io sending_io;
	struct fake_io receiving_io;
	uint8_t payload[256];
	uint8_t received[sizeof(payload)];
	uint8_t frame_end;
	size_t length;
	size_t index;
	bool passed = true;

	for (index=0U;index<sizeof(payload);index++) {
		payload[index] = (uint8_t)index;
	}
	initialize(&sender,&sending_io);
	sender.escape_8th_bit = true;
	sender.use_pack7 = true;
	passed = expect(tx_header(&sender,header) == 0,
	    "transmit Pack-7 header") && passed;
	passed = expect(sending_io.output[2] == ZBINP7,
	    "Pack-7 header indicator") && passed;
	initialize(&receiver,&receiving_io);
	(void)memcpy(receiving_io.input,sending_io.output,
	    sending_io.output_length);
	receiving_io.input_length = sending_io.output_length;
	passed = expect(rx_header(&receiver,1000) == ZDATA,
	    "receive Pack-7 header") && passed;
	passed = expect(receiver.receive_encoded_data ==
	    ZMODEM_ENCODED_DATA_PACK7,"Pack-7 header selects decoder") && passed;
	passed = expect(memcmp(receiver.rxd_header,header,sizeof(header)) == 0,
	    "Pack-7 header bytes") && passed;

	for (length=0U;length<=7U;length++) {
		initialize(&sender,&sending_io);
		sender.escape_8th_bit = true;
		sender.use_pack7 = true;
		passed = expect(tx_data(&sender,ZCRCE,payload,length) == 0,
		    "transmit Pack-7 remainder") && passed;
		for (index=0U;index<sending_io.output_length;index++) {
			passed = expect((sending_io.output[index] &
			    UINT8_C(0x80)) == 0U,"Pack-7 wire is seven-bit clean") &&
			    passed;
		}
		initialize(&receiver,&receiving_io);
		receiver.receive_encoded_data = ZMODEM_ENCODED_DATA_PACK7;
		(void)memcpy(receiving_io.input,sending_io.output,
		    sending_io.output_length);
		receiving_io.input_length = sending_io.output_length;
		passed = expect(rx_data(&receiver,received,sizeof(received),
		    &index,&frame_end) == ENDOFFRAME,
		    "receive Pack-7 remainder") && passed;
		passed = expect(index == length,"Pack-7 remainder length") && passed;
		passed = expect(memcmp(received,payload,length) == 0,
		    "Pack-7 remainder payload") && passed;
	}

	initialize(&sender,&sending_io);
	sender.escape_8th_bit = true;
	sender.use_pack7 = true;
	(void)memset(payload,UINT8_C(0xff),4U);
	passed = expect(tx_data(&sender,ZCRCE,payload,4U) == 0,
	    "transmit known full Pack-7 group") && passed;
	passed = expect(memcmp(sending_io.output,full_group,
	    sizeof(full_group)) == 0,"known full Pack-7 fixture") && passed;
	initialize(&sender,&sending_io);
	sender.escape_8th_bit = true;
	sender.use_pack7 = true;
	payload[0] = 0U;
	payload[1] = 1U;
	payload[2] = 2U;
	passed = expect(tx_data(&sender,ZCRCE,payload,3U) == 0,
	    "transmit known partial Pack-7 group") && passed;
	passed = expect(memcmp(sending_io.output,partial_group,
	    sizeof(partial_group)) == 0,"known partial Pack-7 fixture") && passed;

	for (index=0U;index<sizeof(payload);index++) {
		payload[index] = (uint8_t)index;
	}
	initialize(&sender,&sending_io);
	sender.escape_8th_bit = true;
	sender.use_pack7 = true;
	passed = expect(tx_data(&sender,ZCRCW,payload,sizeof(payload)) == 0,
	    "transmit all-byte Pack-7 packet") && passed;
	initialize(&receiver,&receiving_io);
	receiver.receive_encoded_data = ZMODEM_ENCODED_DATA_PACK7;
	(void)memcpy(receiving_io.input,sending_io.output,
	    sending_io.output_length);
	receiving_io.input_length = sending_io.output_length;
	passed = expect(rx_data(&receiver,received,sizeof(received),&length,
	    &frame_end) == ENDOFFRAME,"receive all-byte Pack-7 packet") && passed;
	passed = expect(length == sizeof(payload),"all-byte Pack-7 length") &&
	    passed;
	passed = expect(frame_end == ZCRCW,"all-byte Pack-7 terminator") &&
	    passed;
	passed = expect(memcmp(received,payload,sizeof(payload)) == 0,
	    "all-byte Pack-7 payload") && passed;
	return passed;
}

static bool
test_pack7_rejection(void)

{
	static const uint8_t malformed[][7] = {
		{ UINT8_C(0x7a),UINT8_C(0x21) },
		{ UINT8_C(0x20),UINT8_C(0x21) },
		{ UINT8_C(0x22),UINT8_C(0x21) },
		{ UINT8_C(0x79),UINT8_C(0x79),UINT8_C(0x21) },
		{ UINT8_C(0x79),UINT8_C(0x79),UINT8_C(0x79),
		    UINT8_C(0x79),UINT8_C(0x79) },
		{ ZDLE,ZCRCE },
		{ UINT8_C(0x21),UINT8_C(0x67) },
		{ UINT8_C(0x21),UINT8_C(0x6c) },
		{ UINT8_C(0x21),ZCRCE,UINT8_C(0x7a) },
		{ UINT8_C(0x21),ZCRCE,UINT8_C(0x22),UINT8_C(0x22),
		    UINT8_C(0x21) }
	};
	static const size_t malformed_length[] = {
		2U,2U,2U,3U,5U,2U,2U,2U,3U,5U
	};
	static const uint8_t payload[] = { 1U,2U,3U,4U };
	struct zmodem sender;
	struct zmodem receiver;
	struct fake_io sending_io;
	struct fake_io receiving_io;
	uint8_t received[sizeof(payload)];
	uint8_t frame_end;
	size_t length;
	size_t index;
	bool passed = true;

	for (index=0U;index<sizeof(malformed) / sizeof(malformed[0]);index++) {
		initialize(&receiver,&receiving_io);
		receiver.receive_encoded_data = ZMODEM_ENCODED_DATA_PACK7;
		(void)memcpy(receiving_io.input,malformed[index],
		    malformed_length[index]);
		receiving_io.input_length = malformed_length[index];
		passed = expect(rx_data(&receiver,received,sizeof(received),&length,
		    &frame_end) == ZMODEM_INVALID_DATA,
		    "reject malformed Pack-7 group") && passed;
	}
	initialize(&receiver,&receiving_io);
	receiver.receive_encoded_data = ZMODEM_ENCODED_DATA_PACK7;
	receiving_io.input[0] = UINT8_C(0x22);
	receiving_io.input[1] = UINT8_C(0x22);
	receiving_io.input_length = 2U;
	passed = expect(rx_data(&receiver,received,sizeof(received),&length,
	    &frame_end) == ZMODEM_TIMEOUT,"report truncated Pack-7 group") &&
	    passed;

	initialize(&sender,&sending_io);
	sender.escape_8th_bit = true;
	sender.use_pack7 = true;
	passed = expect(tx_data(&sender,ZCRCE,payload,sizeof(payload)) == 0,
	    "make Pack-7 corruption fixture") && passed;
	sending_io.output[sending_io.output_length - 1U] ^= UINT8_C(1);
	initialize(&receiver,&receiving_io);
	receiver.receive_encoded_data = ZMODEM_ENCODED_DATA_PACK7;
	(void)memcpy(receiving_io.input,sending_io.output,
	    sending_io.output_length);
	receiving_io.input_length = sending_io.output_length;
	passed = expect(rx_data(&receiver,received,sizeof(received),&length,
	    &frame_end) == ZMODEM_INVALID_DATA,"reject Pack-7 CRC corruption") &&
	    passed;

	initialize(&sender,&sending_io);
	sender.escape_8th_bit = true;
	sender.use_pack7 = true;
	passed = expect(tx_data(&sender,ZCRCE,payload,sizeof(payload)) == 0,
	    "make Pack-7 overflow fixture") && passed;
	initialize(&receiver,&receiving_io);
	receiver.receive_encoded_data = ZMODEM_ENCODED_DATA_PACK7;
	(void)memcpy(receiving_io.input,sending_io.output,
	    sending_io.output_length);
	receiving_io.input_length = sending_io.output_length;
	passed = expect(rx_data(&receiver,received,sizeof(received) - 1U,&length,
	    &frame_end) == ZMODEM_INVALID_DATA,"reject Pack-7 output overflow") &&
	    passed;
	passed = expect(length == sizeof(received) - 1U,
	    "bound Pack-7 overflow output") && passed;

	initialize(&sender,&sending_io);
	sender.escape_8th_bit = true;
	sender.use_pack7 = true;
	sending_io.write_result = ZMODEM_IO_ERROR;
	passed = expect(tx_data(&sender,ZCRCE,payload,sizeof(payload)) != 0,
	    "report Pack-7 write failure") && passed;
	initialize(&sender,&sending_io);
	sender.escape_8th_bit = true;
	sender.use_pack7 = true;
	sending_io.flush_result = ZMODEM_IO_ERROR;
	passed = expect(tx_data(&sender,ZCRCE,payload,sizeof(payload)) != 0,
	    "report Pack-7 flush failure") && passed;
	return passed;
}

static bool
test_maximum_pack7_round_trip(void)

{
	static struct zmodem sender;
	static struct zmodem receiver;
	static struct fake_io sending_io;
	static struct fake_io receiving_io;
	static uint8_t payload[ZMAXSPLEN];
	static uint8_t received[ZMAXSPLEN];
	uint8_t frame_end;
	size_t length;
	size_t index;
	bool passed;

	for (index=0U;index<sizeof(payload);index++) {
		payload[index] = (uint8_t)(index * 73U + 19U);
	}
	initialize(&sender,&sending_io);
	sender.escape_8th_bit = true;
	sender.use_pack7 = true;
	passed = expect(tx_data(&sender,ZCRCE,payload,sizeof(payload)) == 0,
	    "transmit maximum Pack-7 packet");
	initialize(&receiver,&receiving_io);
	receiver.receive_encoded_data = ZMODEM_ENCODED_DATA_PACK7;
	(void)memcpy(receiving_io.input,sending_io.output,
	    sending_io.output_length);
	receiving_io.input_length = sending_io.output_length;
	passed = expect(rx_data(&receiver,received,sizeof(received),&length,
	    &frame_end) == ENDOFFRAME,"receive maximum Pack-7 packet") && passed;
	passed = expect(length == sizeof(payload),"maximum Pack-7 length") &&
	    passed;
	passed = expect(frame_end == ZCRCE,"maximum Pack-7 terminator") && passed;
	passed = expect(memcmp(received,payload,sizeof(payload)) == 0,
	    "maximum Pack-7 payload") && passed;
	return passed;
}

static bool
test_mobyturbo_round_trip(void)

{
	static const uint8_t header[ZMODEM90_ZRPOS_HEADER_LEN] = {
		ZRPOS, XON, XOFF, ZDLE, UINT8_C(0x91), UINT8_C(0xff), 0U,
		ZMODEM90_REQUEST_MOBYTURBO
	};
	static const uint8_t payload[] = {
		XON, XOFF, UINT8_C(0x91), UINT8_C(0x93),
		CAN, CAN, CAN, CAN, CAN, ZDLE, UINT8_C(0xff), 'Z'
	};
	struct zmodem sender;
	struct zmodem receiver;
	struct fake_io sending_io;
	struct fake_io receiving_io;
	uint8_t received[sizeof(payload)];
	uint8_t frame_end;
	size_t header_length;
	size_t length;
	bool passed;

	initialize(&sender,&sending_io);
	sender.use_mobyturbo = true;
	passed = expect(tx_header_length(&sender,header,sizeof(header)) == 0,
	    "transmit MobyTurbo extended header");
	header_length = sending_io.output_length;
	passed = expect(sending_io.output[0] == ZPAD,
	    "MobyTurbo header pad") && passed;
	passed = expect(sending_io.output[1] == ZDLE,
	    "MobyTurbo header escape") && passed;
	passed = expect(sending_io.output[2] == ZBINM32,
	    "MobyTurbo header marker") && passed;
	passed = expect(sending_io.output[3] == sizeof(header) - 1U,
	    "MobyTurbo parameter count") && passed;
	passed = expect(tx_data(&sender,ZCRCE,payload,sizeof(payload)) == 0,
	    "transmit MobyTurbo data") && passed;
	passed = expect(memchr(&sending_io.output[header_length],XON,
	    sending_io.output_length - header_length) != NULL,
	    "MobyTurbo leaves XON unescaped") && passed;
	passed = expect(memchr(&sending_io.output[header_length],XOFF,
	    sending_io.output_length - header_length) != NULL,
	    "MobyTurbo leaves XOFF unescaped") && passed;

	initialize(&receiver,&receiving_io);
	(void)memcpy(receiving_io.input,sending_io.output,
	    sending_io.output_length);
	receiving_io.input_length = sending_io.output_length;
	passed = expect(rx_header(&receiver,1000) == ZRPOS,
	    "receive MobyTurbo header") && passed;
	passed = expect(receiver.rxd_header_len == sizeof(header),
	    "receive MobyTurbo extended length") && passed;
	passed = expect(receiver.receive_mobyturbo,
	    "MobyTurbo selects transparent decoder") && passed;
	passed = expect(receiver.receive_32_bit_data,
	    "MobyTurbo selects CRC32") && passed;
	passed = expect(receiver.receive_encoded_data == ZMODEM_ENCODED_DATA_NONE,
	    "MobyTurbo disables RLE") && passed;
	passed = expect(memcmp(receiver.rxd_header,header,sizeof(header)) == 0,
	    "receive MobyTurbo header bytes") && passed;
	passed = expect(rx_data(&receiver,received,sizeof(received),&length,
	    &frame_end) == ENDOFFRAME,"receive MobyTurbo data") && passed;
	passed = expect(length == sizeof(payload),
	    "MobyTurbo data length") && passed;
	passed = expect(frame_end == ZCRCE,
	    "MobyTurbo data terminator") && passed;
	passed = expect(memcmp(received,payload,sizeof(payload)) == 0,
	    "MobyTurbo preserves flow control and CAN bytes") && passed;

	initialize(&sender,&sending_io);
	sender.use_mobyturbo = true;
	sender.escape_all_control_characters = true;
	sender.escape_iac = true;
	passed = expect(tx_header_length(&sender,header,sizeof(header)) == 0,
	    "transmit MobyTurbo header with requested escaping") && passed;
	passed = expect(tx_data(&sender,ZCRCW,payload,sizeof(payload)) == 0,
	    "transmit escaped MobyTurbo data") && passed;
	initialize(&receiver,&receiving_io);
	receiver.receive_escaped_control_characters = true;
	(void)memcpy(receiving_io.input,sending_io.output,
	    sending_io.output_length);
	receiving_io.input_length = sending_io.output_length;
	passed = expect(rx_header(&receiver,1000) == ZRPOS,
	    "receive escaped MobyTurbo header") && passed;
	passed = expect(rx_data(&receiver,received,sizeof(received),&length,
	    &frame_end) == ENDOFFRAME,"receive escaped MobyTurbo data") && passed;
	passed = expect(memcmp(received,payload,sizeof(payload)) == 0,
	    "escaped MobyTurbo preserves payload") && passed;

	initialize(&sender,&sending_io);
	sender.use_mobyturbo = true;
	passed = expect(tx_data(&sender,ZCRCE,
	    (const uint8_t[]){ '@',CR },2U) == 0,
	    "transmit MobyTurbo at-CR without ESCCTL") && passed;
	initialize(&sender,&sending_io);
	sender.use_mobyturbo = true;
	sender.escape_all_control_characters = true;
	passed = expect(tx_data(&sender,ZCRCE,
	    (const uint8_t[]){ CR,'@','A','@',CR },5U) == 0,
	    "transmit MobyTurbo CR escape predicates") && passed;

	initialize(&sender,&sending_io);
	sender.use_mobyturbo = true;
	sending_io.write_result = ZMODEM_IO_ERROR;
	passed = expect(tx_data(&sender,ZCRCE,payload,sizeof(payload)) != 0,
	    "report MobyTurbo data write failure") && passed;
	initialize(&sender,&sending_io);
	sender.use_mobyturbo = true;
	sending_io.flush_result = ZMODEM_IO_ERROR;
	passed = expect(tx_data(&sender,ZCRCE,payload,sizeof(payload)) != 0,
	    "report MobyTurbo data flush failure") && passed;
	return passed;
}

static bool
test_mobyturbo_probe(void)

{
	static const uint8_t header[HDRLEN] = { ZFILE,0U,0U,0U,0U };
	struct zmodem sender;
	struct zmodem receiver;
	struct fake_io sending_io;
	struct fake_io receiving_io;
	size_t call;
	bool passed;

	initialize(&sender,&sending_io);
	passed = expect(tx_mobyturbo_probe(&sender) == 0,
	    "transmit MobyTurbo transparency probe");
	passed = expect(tx_hex_header(&sender,header) == 0,
	    "transmit header after MobyTurbo probe") && passed;
	initialize(&receiver,&receiving_io);
	(void)memcpy(receiving_io.input,sending_io.output,
	    sending_io.output_length);
	receiving_io.input_length = sending_io.output_length;
	passed = expect(rx_header(&receiver,1000) == ZFILE,
	    "scan header after MobyTurbo probe") && passed;
	passed = expect(receiver.mobyturbo_probe_passed,
	    "recognize intact MobyTurbo probe") && passed;

	receiving_io.input_offset = 0U;
	receiver.input_count = 0U;
	receiver.input_index = 0U;
	receiving_io.input[2] ^= UINT8_C(1);
	passed = expect(rx_header(&receiver,1000) == ZFILE,
	    "scan header after damaged MobyTurbo probe") && passed;
	passed = expect(!receiver.mobyturbo_probe_passed,
	    "reject damaged MobyTurbo probe") && passed;

	for (call=1U;call<=5U;call++) {
		initialize(&sender,&sending_io);
		sending_io.fail_write_call = call;
		passed = expect(tx_mobyturbo_probe(&sender) != 0,
		    "report MobyTurbo probe write failure") && passed;
	}
	initialize(&sender,&sending_io);
	passed = expect(tx_header_length(&sender,header,HDRLEN - 1U) != 0,
	    "reject short transmitted header") && passed;
	passed = expect(tx_header_length(&sender,header,ZMAXHLEN + 2U) != 0,
	    "reject oversized transmitted header") && passed;

	initialize(&receiver,&receiving_io);
	receiving_io.input[0] = ZPAD;
	receiving_io.input[1] = ZDLE;
	receiving_io.input[2] = ZBINM32;
	receiving_io.input[3] = HDRLEN - 2U;
	receiving_io.input_length = 4U;
	passed = expect(rx_header_and_check(&receiver,1000) == ZMODEM_TIMEOUT,
	    "reject short MobyTurbo parameter count") && passed;
	initialize(&receiver,&receiving_io);
	receiving_io.input[0] = ZPAD;
	receiving_io.input[1] = ZDLE;
	receiving_io.input[2] = ZBINM32;
	receiving_io.input[3] = ZMAXHLEN + 1U;
	receiving_io.input_length = 4U;
	passed = expect(rx_header_and_check(&receiver,1000) == ZMODEM_TIMEOUT,
	    "reject oversized MobyTurbo parameter count") && passed;
	initialize(&receiver,&receiving_io);
	receiving_io.input[0] = ZPAD;
	receiving_io.input[1] = ZDLE;
	receiving_io.input[2] = ZBINM32;
	receiving_io.input[3] = HDRLEN - 1U;
	receiving_io.input[4] = ZDLE;
	receiving_io.input[5] = '?';
	receiving_io.input_length = 6U;
	passed = expect(rx_header_and_check(&receiver,1000) == ZMODEM_TIMEOUT,
	    "reject invalid MobyTurbo header escape") && passed;
	return passed;
}

static bool
test_omen_header_rejection(void)
{
	static const uint8_t header[HDRLEN] = { ZDATA,1U,2U,3U,4U };
	struct zmodem sender;
	struct zmodem receiver;
	struct fake_io sending_io;
	struct fake_io receiving_io;
	size_t cut;
	bool passed = true;

	initialize(&sender,&sending_io);
	sender.escape_8th_bit = true;
	passed = expect(tx_header(&sender,header) == 0,
	    "make Omen header for rejection tests") && passed;
	for (cut=0U;cut<sending_io.output_length;cut++) {
		initialize(&receiver,&receiving_io);
		(void)memcpy(receiving_io.input,sending_io.output,cut);
		receiving_io.input_length = cut;
		passed = expect(rx_header(&receiver,1000) < 0,
		    "reject truncated Omen header") && passed;
	}

	initialize(&receiver,&receiving_io);
	(void)memcpy(receiving_io.input,sending_io.output,
	    sending_io.output_length);
	receiving_io.input_length = sending_io.output_length;
	receiving_io.input[receiving_io.input_length - 1U] ^= UINT8_C(1);
	passed = expect(rx_header_and_check(&receiver,1000) == ZMODEM_TIMEOUT,
	    "reject corrupt Omen header CRC") && passed;
	passed = expect(receiving_io.output_length > 0U,
	    "corrupt Omen header sends ZNAK") && passed;

	initialize(&receiver,&receiving_io);
	receiving_io.input[0] = ZPAD;
	receiving_io.input[1] = ZDLE;
	receiving_io.input[2] = ZBINR32ESC8;
	receiving_io.input[3] = UINT8_C(0x21);
	receiving_io.input_length = 4U;
	passed = expect(rx_header_and_check(&receiver,1000) == ZMODEM_TIMEOUT,
	    "reject short Omen parameter count") && passed;

	initialize(&receiver,&receiving_io);
	receiving_io.input[0] = ZPAD;
	receiving_io.input[1] = ZDLE;
	receiving_io.input[2] = ZBINR32ESC8;
	receiving_io.input[3] = UINT8_C(0x23) + ZMAXHLEN;
	receiving_io.input_length = 4U;
	passed = expect(rx_header_and_check(&receiver,1000) == ZMODEM_TIMEOUT,
	    "reject oversized Omen parameter count") && passed;

	initialize(&receiver,&receiving_io);
	(void)memcpy(receiving_io.input,sending_io.output,3U);
	receiving_io.input[3] = XON;
	(void)memcpy(&receiving_io.input[4],&sending_io.output[3],
	    sending_io.output_length - 3U);
	receiving_io.input_length = sending_io.output_length + 1U;
	passed = expect(rx_header(&receiver,1000) == ZDATA,
	    "ignore flow control before Omen parameter count") && passed;

	initialize(&receiver,&receiving_io);
	receiving_io.input[0] = ZPAD;
	receiving_io.input[1] = ZDLE;
	receiving_io.input[2] = ZBINR32ESC8;
	receiving_io.input[3] = UINT8_C(0x26);
	receiving_io.input[4] = ZDLE;
	receiving_io.input[5] = '?';
	receiving_io.input_length = 6U;
	passed = expect(rx_header_and_check(&receiver,1000) == ZMODEM_TIMEOUT,
	    "reject invalid Omen escape code") && passed;

	initialize(&receiver,&receiving_io);
	receiving_io.input[0] = ZPAD;
	receiving_io.input[1] = ZDLE;
	receiving_io.input[2] = ZBINR32ESC8;
	receiving_io.input[3] = UINT8_C(0x26);
	receiving_io.input[4] = SO;
	receiving_io.input_length = 5U;
	passed = expect(rx_header(&receiver,1000) == ZMODEM_TIMEOUT,
	    "report truncated Omen high-bit prefix") && passed;

	initialize(&receiver,&receiving_io);
	receiving_io.input[0] = ZPAD;
	receiving_io.input[1] = ZDLE;
	receiving_io.input[2] = ZBINR32ESC8;
	receiving_io.input[3] = UINT8_C(0x26);
	receiving_io.input[4] = ZDLE;
	receiving_io.input[5] = ZCRCE;
	receiving_io.input_length = 6U;
	passed = expect(rx_header_and_check(&receiver,1000) == ZMODEM_TIMEOUT,
	    "reject frame end in Omen header") && passed;
	return passed;
}

static bool
test_omen_buffer_boundaries(void)
{
	static uint8_t payload[ZMODEM_TX_DATA_WIRE_CAPACITY / 3U + 2U];
	struct zmodem protocol;
	struct fake_io fake;
	size_t triples = ZMODEM_TX_DATA_WIRE_CAPACITY / 3U;
	size_t remainder = ZMODEM_TX_DATA_WIRE_CAPACITY % 3U;
	size_t exact_length = triples + (remainder == 0U ? 0U : 1U);
	size_t i;
	bool passed;

	passed = expect(exact_length + 1U <= ZMAXSPLEN,
	    "Omen output buffer boundary fits one subpacket");
	for (i=0U;i<triples;i++) {
		payload[i] = (i & 1U) == 0U ? UINT8_C(0x81) : UINT8_C(0x82);
	}
	if (remainder == 1U) {
		payload[triples] = 'C';
	}
	else if (remainder == 2U) {
		payload[triples] = UINT8_C(0xff);
	}
	payload[exact_length] = UINT8_C(0x83);

	initialize(&protocol,&fake);
	protocol.escape_8th_bit = true;
	protocol.escape_all_control_characters = true;
	passed = expect(tx_data(&protocol,ZCRCE,payload,exact_length) == 0,
	    "flush exactly full Omen buffer") && passed;
	passed = expect(fake.write_calls == 2U,
	    "exact Omen buffer uses two writes") && passed;

	initialize(&protocol,&fake);
	protocol.escape_8th_bit = true;
	protocol.escape_all_control_characters = true;
	passed = expect(tx_data(&protocol,ZCRCE,payload,exact_length + 1U) == 0,
	    "flush Omen buffer before encoded byte") && passed;
	passed = expect(fake.write_calls == 2U,
	    "crossing Omen buffer uses two writes") && passed;

	initialize(&protocol,&fake);
	protocol.escape_8th_bit = true;
	protocol.escape_all_control_characters = true;
	fake.fail_write_call = 1U;
	passed = expect(tx_data(&protocol,ZCRCE,payload,exact_length + 1U) != 0,
	    "report intermediate Omen buffer write failure") && passed;
	return passed;
}

static bool
test_standard_rle_receive(void)
{
	static const uint8_t tokens[] = {
		ZRESC,UINT8_C(0x43),'A',
		ZRESC,UINT8_C(0x42),ZRESC,
		UINT8_C(0x20),UINT8_C(0x20)
	};
	static const uint8_t expected[] = {
		'A','A','A',ZRESC,ZRESC,UINT8_C(0x20),UINT8_C(0x20)
	};
	struct zmodem protocol;
	struct zmodem sender;
	struct fake_io fake;
	struct fake_io sending_io;
	uint8_t header[HDRLEN] = { ZDATA,1U,2U,3U,4U };
	uint8_t received[sizeof(expected)];
	uint8_t frame_end;
	uint32_t crc;
	size_t offset = 0U;
	size_t length;
	size_t i;
	bool passed;

	initialize(&sender,&sending_io);
	sender.can_fcs_32 = true;
	passed = expect(tx_header(&sender,header) == 0,
	    "make standard RLE header");
	sending_io.output[3] = ZBINR32;
	initialize(&protocol,&fake);
	(void)memcpy(fake.input,sending_io.output,sending_io.output_length);
	fake.input_length = sending_io.output_length;
	passed = expect(rx_header(&protocol,1000) == ZDATA,
	    "receive standard RLE header") && passed;
	passed = expect(protocol.receive_encoded_data == ZMODEM_ENCODED_DATA_RLE,
	    "standard RLE header selects decoder") && passed;
	fake.input_length = 0U;
	fake.input_offset = 0U;
	for (i=0U;i<sizeof(tokens);i++) {
		offset = append_escaped(fake.input,offset,tokens[i]);
	}
	crc = crc32_update(UINT32_MAX,tokens,sizeof(tokens));
	crc = ~crc32_byte_update(crc,ZCRCE);
	fake.input[offset++] = ZDLE;
	fake.input[offset++] = ZCRCE;
	for (i=0U;i<sizeof(crc);i++) {
		offset = append_escaped(fake.input,offset,(uint8_t)crc);
		crc >>= 8;
	}
	fake.input_length = offset;
	passed = expect(rx_data(&protocol,received,sizeof(received),&length,
	    &frame_end) == ENDOFFRAME,"receive standard RLE packet");
	passed = expect(length == sizeof(expected),"standard RLE length") &&
	    passed;
	passed = expect(memcmp(received,expected,sizeof(expected)) == 0,
	    "standard RLE payload") && passed;
	return passed;
}

static size_t
append_rle_packet(uint8_t * output,const uint8_t * tokens,size_t token_count,
    uint8_t frame_end)
{
	uint32_t crc = crc32_update(UINT32_MAX,tokens,token_count);
	size_t offset = 0U;
	size_t i;

	crc = ~crc32_byte_update(crc,frame_end);
	for (i=0U;i<token_count;i++) {
		offset = append_escaped(output,offset,tokens[i]);
	}
	output[offset++] = ZDLE;
	output[offset++] = frame_end;
	for (i=0U;i<sizeof(crc);i++) {
		offset = append_escaped(output,offset,(uint8_t)crc);
		crc >>= 8;
	}
	return offset;
}

static bool
test_rle_rejection(void)
{
	static const uint8_t invalid_counts[] = {
		UINT8_C(0x1f),UINT8_C(0x41),UINT8_C(0x80)
	};
	struct zmodem protocol;
	struct fake_io fake;
	uint8_t received[4];
	uint8_t frame_end;
	size_t length;
	size_t i;
	bool passed = true;

	initialize(&protocol,&fake);
	protocol.receive_32_bit_data = true;
	protocol.receive_encoded_data = ZMODEM_ENCODED_DATA_RLE;
	passed = expect(rx_data(&protocol,received,sizeof(received),&length,
	    &frame_end) == ZMODEM_TIMEOUT,"report empty RLE input timeout") &&
	    passed;

	for (i=0U;i<sizeof(invalid_counts);i++) {
		initialize(&protocol,&fake);
		protocol.receive_32_bit_data = true;
		protocol.receive_encoded_data = ZMODEM_ENCODED_DATA_RLE;
		fake.input[0] = ZRESC;
		fake.input[1] = invalid_counts[i];
		fake.input_length = 2U;
		passed = expect(rx_data(&protocol,received,sizeof(received),&length,
		    &frame_end) == ZMODEM_INVALID_DATA,
		    "reject invalid RLE count") && passed;
	}

	initialize(&protocol,&fake);
	protocol.receive_32_bit_data = true;
	protocol.receive_encoded_data = ZMODEM_ENCODED_DATA_RLE;
	fake.input[0] = ZRESC;
	fake.input[1] = ZDLE;
	fake.input[2] = ZCRCE;
	fake.input_length = 3U;
	passed = expect(rx_data(&protocol,received,sizeof(received),&length,
	    &frame_end) == ZMODEM_INVALID_DATA,
	    "reject frame end inside RLE token") && passed;

	initialize(&protocol,&fake);
	protocol.receive_32_bit_data = true;
	protocol.receive_encoded_data = ZMODEM_ENCODED_DATA_RLE;
	fake.input_length = append_rle_packet(fake.input,
	    (const uint8_t[]){ 'A' },1U,ZCRCE);
	fake.input[fake.input_length - 1U] ^= UINT8_C(1);
	passed = expect(rx_data(&protocol,received,sizeof(received),&length,
	    &frame_end) == ZMODEM_INVALID_DATA,"reject corrupt RLE CRC") &&
	    passed;

	initialize(&protocol,&fake);
	protocol.receive_32_bit_data = true;
	protocol.receive_encoded_data = ZMODEM_ENCODED_DATA_RLE;
	fake.input_length = append_rle_packet(fake.input,
	    (const uint8_t[]){ 'A' },1U,ZCRCE) - 1U;
	passed = expect(rx_data(&protocol,received,sizeof(received),&length,
	    &frame_end) == ZMODEM_TIMEOUT,"report truncated RLE CRC") && passed;

	initialize(&protocol,&fake);
	protocol.receive_32_bit_data = true;
	protocol.receive_encoded_data = ZMODEM_ENCODED_DATA_RLE;
	fake.input[0] = 'A';
	fake.input[1] = ZDLE;
	fake.input[2] = ZCRCE;
	fake.input[3] = ZDLE;
	fake.input[4] = ZCRCE;
	fake.input_length = 5U;
	passed = expect(rx_data(&protocol,received,sizeof(received),&length,
	    &frame_end) == ZMODEM_INVALID_DATA,
	    "reject frame end in RLE CRC") && passed;

	initialize(&protocol,&fake);
	protocol.receive_32_bit_data = true;
	protocol.receive_encoded_data = ZMODEM_ENCODED_DATA_RLE;
	fake.input_length = append_rle_packet(fake.input,
	    (const uint8_t[]){ ZRESC,UINT8_C(0x7f),'A' },3U,ZCRCE);
	passed = expect(rx_data(&protocol,received,1U,&length,&frame_end) ==
	    ZMODEM_INVALID_DATA,"reject expanded RLE overflow") && passed;
	passed = expect(length == 1U,"retain bounded RLE output") && passed;
	return passed;
}

static bool
test_legacy_escape8_receive(void)
{
	static const uint8_t payload[] = {
		UINT8_C(0x80),UINT8_C(0x91),UINT8_C(0xd3),UINT8_C(0xff)
	};
	struct zmodem protocol;
	struct fake_io fake;
	uint8_t received[sizeof(payload)];
	uint8_t frame_end;
	uint32_t crc;
	size_t offset = 0U;
	size_t length;
	size_t i;
	bool passed;

	initialize(&protocol,&fake);
	crc = crc32_update(UINT32_MAX,payload,sizeof(payload));
	crc = ~crc32_byte_update(crc,ZCRCE);
	for (i=0U;i<sizeof(payload);i++) {
		offset = append_legacy_escape8(fake.input,offset,payload[i]);
	}
	fake.input[offset++] = ZDLE;
	fake.input[offset++] = ZCRCE;
	for (i=0U;i<sizeof(crc);i++) {
		offset = append_legacy_escape8(fake.input,offset,(uint8_t)crc);
		crc >>= 8;
	}
	fake.input_length = offset;
	protocol.receive_32_bit_data = true;
	protocol.receive_escape8_format = ZMODEM_ESCAPE8_LEGACY;
	passed = expect(rx_data(&protocol,received,sizeof(received),&length,
	    &frame_end) == ENDOFFRAME,"receive legacy ESC8 packet");
	passed = expect(length == sizeof(payload),"legacy ESC8 length") && passed;
	passed = expect(memcmp(received,payload,sizeof(payload)) == 0,
	    "legacy ESC8 payload") && passed;
	return passed;
}

static bool
test_iac_escaping(void)

{
	static const uint8_t payload[] = { 1U,UINT8_C(0xff),ZDLE };
	struct zmodem sender;
	struct zmodem receiver;
	struct fake_io sending_io;
	struct fake_io receiving_io;
	uint8_t received[sizeof(payload)];
	uint8_t frame_end;
	uint8_t header[HDRLEN] = { ZDATA,UINT8_C(0xff),ZDLE,0U,0U };
	size_t length;
	bool passed;

	initialize(&sender,&sending_io);
	passed = expect(tx_data(&sender,ZCRCE,payload,sizeof(payload)) == 0,
	    "transmit data without IAC escaping");
	passed = expect(sending_io.output[1] == UINT8_C(0xff),
	    "leave IAC raw by default") && passed;

	initialize(&sender,&sending_io);
	sender.escape_iac = true;
	passed = expect(tx_data(&sender,ZCRCE,payload,sizeof(payload)) == 0,
	    "transmit data with IAC escaping") && passed;
	passed = expect(sending_io.output[1] == ZDLE,
	    "prefix escaped IAC with ZDLE") && passed;
	passed = expect(sending_io.output[2] == ZRUB1,
	    "encode escaped IAC as ZRUB1") && passed;

	initialize(&receiver,&receiving_io);
	(void)memcpy(receiving_io.input,sending_io.output,
	    sending_io.output_length);
	receiving_io.input_length = sending_io.output_length;
	passed = expect(rx_data(&receiver,received,sizeof(received),&length,
	    &frame_end) == ENDOFFRAME,"receive IAC-escaped packet") && passed;
	passed = expect(length == sizeof(payload),
	    "IAC-escaped packet length") && passed;
	passed = expect(memcmp(received,payload,sizeof(payload)) == 0,
	    "IAC-escaped packet payload") && passed;

	initialize(&sender,&sending_io);
	sender.escape_iac = true;
	sender.can_fcs_32 = true;
	sender.want_fcs_32 = false;
	passed = expect(tx_header(&sender,header) == 0,
	    "transmit binary header with IAC escaping") && passed;
	passed = expect(memchr(sending_io.output,ZRUB1,
	    sending_io.output_length) != NULL,
	    "encode IAC in binary header") && passed;
	return passed;
}

static bool
test_data_read_failures(bool use_crc32)
{
	static const uint8_t payload[] = { 1U,2U,3U };
	struct zmodem sender;
	struct zmodem receiver;
	struct fake_io sending_io;
	struct fake_io receiving_io;
	uint8_t received[sizeof(payload)];
	uint8_t frame_end;
	size_t length;
	size_t cut;
	bool passed = true;

	initialize(&sender,&sending_io);
	sender.can_fcs_32 = use_crc32;
	sender.want_fcs_32 = use_crc32;
	passed = expect(tx_data(&sender,ZCRCE,payload,sizeof(payload)) == 0,
	    "make packet for read-failure sweep") && passed;
	for (cut = 0U; cut < sending_io.output_length; cut++) {
		initialize(&receiver,&receiving_io);
		receiver.receive_32_bit_data = use_crc32;
		(void)memcpy(receiving_io.input,sending_io.output,cut);
		receiving_io.input_length = cut;
		passed = expect(rx_data(&receiver,received,sizeof(received),&length,
		    &frame_end) < 0,"truncated data packet") && passed;
	}

	initialize(&receiver,&receiving_io);
	receiver.receive_32_bit_data = use_crc32;
	(void)memcpy(receiving_io.input,sending_io.output,
	    sending_io.output_length);
	receiving_io.input_length = sending_io.output_length;
	receiving_io.input[receiving_io.input_length - 1U] ^= UINT8_C(1);
	passed = expect(rx_data(&receiver,received,sizeof(received),&length,
	    &frame_end) == INVDATA,"corrupt data packet CRC") && passed;
	return passed;
}

static bool
test_receive_escape_variants(void)
{
	struct zmodem protocol;
	struct fake_io fake;
	uint8_t received[3];
	uint8_t frame_end;
	uint16_t crc = 0U;
	size_t length;
	size_t offset = 0U;
	bool passed;

	initialize(&protocol,&fake);
	fake.input[offset++] = XON;
	fake.input[offset++] = UINT8_C(0x91);
	fake.input[offset++] = XOFF;
	fake.input[offset++] = UINT8_C(0x93);
	fake.input[offset++] = ZDLE;
	fake.input[offset++] = ZRUB0;
	fake.input[offset++] = ZDLE;
	fake.input[offset++] = ZRUB1;
	fake.input[offset++] = ZDLE;
	fake.input[offset++] = UINT8_C(0x20);
	fake.input[offset++] = UINT8_C(1);
	crc = crc16_update(crc,UINT8_C(0x7f));
	crc = crc16_update(crc,UINT8_C(0xff));
	crc = crc16_update(crc,UINT8_C(1));
	crc = crc16_update(crc,ZCRCQ);
	crc = crc16_update(crc,0U);
	crc = crc16_update(crc,0U);
	fake.input[offset++] = ZDLE;
	fake.input[offset++] = ZCRCQ;
	offset = append_escaped(fake.input,offset,(uint8_t)(crc >> 8));
	offset = append_escaped(fake.input,offset,(uint8_t)crc);
	fake.input_length = offset;
	passed = expect(rx_data(&protocol,received,sizeof(received),&length,
	    &frame_end) == FRAMEOK,"receive escape variants");
	passed = expect(length == sizeof(received),"escape variant length") &&
	    passed;
	passed = expect(received[0] == UINT8_C(0x7f),"ZRUB0 value") && passed;
	passed = expect(received[1] == UINT8_C(0xff),"ZRUB1 value") && passed;
	passed = expect(received[2] == UINT8_C(1),"value after invalid escape") &&
	    passed;
	return passed;
}

static bool
is_frame_end_byte(uint8_t byte)
{
	return (byte >= ZCRCE) && (byte <= ZCRCW);
}

static bool
test_reject_frame_end_in_data_crc(bool use_crc32)
{
	struct zmodem protocol;
	struct fake_io fake;
	uint8_t payload = 0U;
	uint8_t crc_bytes[4];
	uint8_t received[1];
	uint8_t frame_end;
	size_t crc_count = use_crc32 ? 4U : 2U;
	size_t marked_index = sizeof(crc_bytes);
	size_t index;
	size_t length;
	size_t offset = 0U;
	unsigned candidate;
	bool passed;

	for (candidate = 0U; candidate < 256U; candidate++) {
		payload = (uint8_t)candidate;
		if (use_crc32) {
			uint32_t crc = crc32_update(UINT32_MAX,&payload,1U);

			crc = ~crc32_byte_update(crc,ZCRCE);
			for (index = 0U; index < crc_count; index++) {
				crc_bytes[index] = (uint8_t)(crc >> (index * 8U));
			}
		}
		else {
			uint16_t crc = crc16_update(0U,payload);

			crc = crc16_update(crc,ZCRCE);
			crc = crc16_update(crc,0U);
			crc = crc16_update(crc,0U);
			crc_bytes[0] = (uint8_t)(crc >> 8);
			crc_bytes[1] = (uint8_t)crc;
		}
		for (index = 0U; index < crc_count; index++) {
			if (is_frame_end_byte(crc_bytes[index])) {
				marked_index = index;
				break;
			}
		}
		if (marked_index < crc_count) {
			break;
		}
	}

	initialize(&protocol,&fake);
	protocol.receive_32_bit_data = use_crc32;
	offset = append_escaped(fake.input,offset,payload);
	fake.input[offset++] = ZDLE;
	fake.input[offset++] = ZCRCE;
	for (index = 0U; index < crc_count; index++) {
		if (index == marked_index) {
			fake.input[offset++] = ZDLE;
			fake.input[offset++] = crc_bytes[index];
		}
		else {
			offset = append_escaped(fake.input,offset,crc_bytes[index]);
		}
	}
	fake.input_length = offset;
	passed = expect(marked_index < crc_count,"construct marked data CRC");
	passed = expect(rx_data(&protocol,received,sizeof(received),&length,
	    &frame_end) == ZMODEM_INVALID_DATA,
	    "reject frame end in data CRC") && passed;
	return passed;
}

static bool
test_data_packets(void)
{
	struct zmodem protocol;
	struct fake_io fake;
	uint8_t payload[1] = { 0U };
	uint8_t received[1];
	uint8_t frame_end;
	size_t length;
	bool passed = true;

	passed = test_span_scanner_round_trip() && passed;
	passed = test_control_character_escaping() && passed;
	passed = test_receive_control_enforcement() && passed;
	passed = test_escctl_hex_header_terminator() && passed;
	passed = test_maximum_escaped_data_round_trip(false) && passed;
	passed = test_maximum_escaped_data_round_trip(true) && passed;
	passed = data_round_trip(false,false,ZCRCW,ENDOFFRAME) && passed;
	passed = data_round_trip(false,true,ZCRCW,ENDOFFRAME) && passed;
	passed = data_round_trip(true,false,ZCRCW,ENDOFFRAME) && passed;
	passed = data_round_trip(true,true,ZCRCW,ENDOFFRAME) && passed;
	passed = data_round_trip(false,false,ZCRCE,ENDOFFRAME) && passed;
	passed = data_round_trip(false,false,ZCRCG,FRAMEOK) && passed;
	passed = data_round_trip(false,false,ZCRCQ,FRAMEOK) && passed;
	passed = test_eighth_bit_escaping() && passed;
	passed = test_omen_rle_round_trip() && passed;
	passed = test_omen_buffer_boundaries() && passed;
	passed = test_standard_rle_receive() && passed;
	passed = test_rle_rejection() && passed;
	passed = test_legacy_escape8_receive() && passed;
	passed = test_iac_escaping() && passed;
	passed = test_data_read_failures(false) && passed;
	passed = test_data_read_failures(true) && passed;
	passed = test_receive_escape_variants() && passed;
	passed = test_reject_frame_end_in_data_crc(false) && passed;
	passed = test_reject_frame_end_in_data_crc(true) && passed;
	initialize(&protocol,&fake);
	passed = expect(tx_data(&protocol,ZCRCE,payload,ZMAXSPLEN + 1U) != 0,
	    "reject oversized transmit packet") && passed;
	initialize(&protocol,&fake);
	fake.write_result = ZMODEM_IO_ERROR;
	passed = expect(tx_data(&protocol,ZCRCE,payload,sizeof(payload)) != 0,
	    "report data packet write failure") && passed;
	initialize(&protocol,&fake);
	passed = expect(tx_data(&protocol,ZCRCE,payload,sizeof(payload)) == 0,
	    "make corrupt packet") && passed;
	fake.input_length = fake.output_length;
	(void)memcpy(fake.input,fake.output,fake.output_length);
	fake.input[fake.input_length - 1U] ^= UINT8_C(1);
	fake.input_offset = 0U;
	passed = expect(rx_data(&protocol,received,sizeof(received),&length,
	    &frame_end) == INVDATA,"reject corrupt data CRC") && passed;
	initialize(&protocol,&fake);
	passed = expect(tx_data(&protocol,ZCRCE,payload,sizeof(payload)) == 0,
	    "make capacity-overflow packet") && passed;
	fake.input_length = fake.output_length;
	(void)memcpy(fake.input,fake.output,fake.output_length);
	fake.input_offset = 0U;
	passed = expect(rx_data(&protocol,received,0U,&length,&frame_end) ==
	    INVDATA,"reject packet exceeding destination capacity") && passed;
	return passed;
}

static bool
test_protocol_maximum_overflow(bool use_crc32)
{
	struct zmodem protocol;
	struct fake_io fake;
	uint8_t received[ZMAXSPLEN + 1U];
	uint8_t frame_end;
	size_t length;
	size_t offset = ZMAXSPLEN + 2U;

	initialize(&protocol,&fake);
	(void)memset(fake.input,0,offset);
	fake.input[offset++] = ZDLE;
	fake.input[offset++] = ZCRCE;
	if (use_crc32) {
		uint32_t crc = crc32_update(UINT32_MAX,fake.input,ZMAXSPLEN + 2U);

		crc = ~crc32_byte_update(crc,ZCRCE);
		offset = append_escaped(fake.input,offset,(uint8_t)crc);
		offset = append_escaped(fake.input,offset,(uint8_t)(crc >> 8));
		offset = append_escaped(fake.input,offset,(uint8_t)(crc >> 16));
		offset = append_escaped(fake.input,offset,(uint8_t)(crc >> 24));
	}
	else {
		uint16_t crc = 0U;
		size_t index;

		for (index = 0U; index < ZMAXSPLEN + 2U; index++) {
			crc = crc16_update(crc,0U);
		}
		crc = crc16_update(crc,ZCRCE);
		crc = crc16_update(crc,0U);
		crc = crc16_update(crc,0U);
		offset = append_escaped(fake.input,offset,(uint8_t)(crc >> 8));
		offset = append_escaped(fake.input,offset,(uint8_t)crc);
	}
	fake.input_length = offset;
	protocol.receive_32_bit_data = use_crc32;
	return expect(rx_data(&protocol,received,sizeof(received),&length,
	    &frame_end) == INVDATA,"reject protocol maximum overflow") &&
	    expect(length == ZMAXSPLEN,"protocol maximum retained length");
}

static int transmit_header_style(struct zmodem *,const uint8_t *,unsigned,
    bool);

static size_t
header_crc_bytes(bool use_crc32,const uint8_t * header,uint8_t * bytes)
{
	if (use_crc32) {
		uint32_t crc = ~crc32_update(UINT32_MAX,header,HDRLEN);
		size_t index;

		for (index = 0U; index < sizeof(crc); index++) {
			bytes[index] = (uint8_t)(crc >> (index * 8U));
		}
		return sizeof(crc);
	}
	else {
		uint16_t crc = crc16_buffer_update(0U,header,HDRLEN);

		crc = crc16_update(crc,0U);
		crc = crc16_update(crc,0U);
		bytes[0] = (uint8_t)(crc >> 8);
		bytes[1] = (uint8_t)crc;
		return 2U;
	}
}

static bool
test_reject_frame_end_in_header_body(void)
{
	struct zmodem protocol;
	struct fake_io fake;
	uint8_t header[HDRLEN] = { ZDATA,ZCRCE,2U,3U,4U };
	uint8_t crc_bytes[4];
	size_t crc_count;
	size_t index;
	size_t offset = 0U;
	bool passed;

	initialize(&protocol,&fake);
	fake.input[offset++] = ZPAD;
	fake.input[offset++] = ZDLE;
	fake.input[offset++] = ZBIN;
	for (index = 0U; index < sizeof(header); index++) {
		if (index == 1U) {
			fake.input[offset++] = ZDLE;
			fake.input[offset++] = header[index];
		}
		else {
			offset = append_escaped(fake.input,offset,header[index]);
		}
	}
	crc_count = header_crc_bytes(false,header,crc_bytes);
	for (index = 0U; index < crc_count; index++) {
		offset = append_escaped(fake.input,offset,crc_bytes[index]);
	}
	fake.input_length = offset;
	passed = expect(rx_header_and_check(&protocol,1000) == ZMODEM_TIMEOUT,
	    "reject frame end in binary header body");
	passed = expect(fake.output_length > 0U,
	    "marked binary header body sends ZNAK") && passed;
	return passed;
}

static bool
test_reject_frame_end_in_header_crc(bool use_crc32)
{
	struct zmodem protocol;
	struct fake_io fake;
	uint8_t header[HDRLEN] = { ZDATA,1U,2U,3U,0U };
	uint8_t crc_bytes[4];
	size_t crc_count = 0U;
	size_t marked_index = sizeof(crc_bytes);
	size_t index;
	size_t offset = 0U;
	unsigned candidate;
	bool passed;

	for (candidate = 0U; candidate < 256U; candidate++) {
		header[4] = (uint8_t)candidate;
		crc_count = header_crc_bytes(use_crc32,header,crc_bytes);
		for (index = 0U; index < crc_count; index++) {
			if (is_frame_end_byte(crc_bytes[index])) {
				marked_index = index;
				break;
			}
		}
		if (marked_index < crc_count) {
			break;
		}
	}

	initialize(&protocol,&fake);
	fake.input[offset++] = ZPAD;
	fake.input[offset++] = ZDLE;
	fake.input[offset++] = use_crc32 ? ZBIN32 : ZBIN;
	for (index = 0U; index < sizeof(header); index++) {
		offset = append_escaped(fake.input,offset,header[index]);
	}
	for (index = 0U; index < crc_count; index++) {
		if (index == marked_index) {
			fake.input[offset++] = ZDLE;
			fake.input[offset++] = crc_bytes[index];
		}
		else {
			offset = append_escaped(fake.input,offset,crc_bytes[index]);
		}
	}
	fake.input_length = offset;
	passed = expect(marked_index < crc_count,
	    "construct marked binary header CRC");
	passed = expect(rx_header_and_check(&protocol,1000) == ZMODEM_TIMEOUT,
	    "reject frame end in binary header CRC") && passed;
	passed = expect(fake.output_length > 0U,
	    "marked binary header CRC sends ZNAK") && passed;
	return passed;
}

static bool
header_round_trip(bool use_crc32,bool variable)
{
	struct zmodem sender;
	struct zmodem receiver;
	struct fake_io sending_io;
	struct fake_io receiving_io;
	uint8_t header[HDRLEN] = { ZDATA,1U,2U,3U,4U };

	initialize(&sender,&sending_io);
	sender.can_fcs_32 = true;
	sender.want_fcs_32 = use_crc32;
	sender.use_variable_headers = variable;
	if (!expect(tx_header(&sender,header) == 0,"transmit binary header")) {
		return false;
	}
	initialize(&receiver,&receiving_io);
	(void)memcpy(receiving_io.input,sending_io.output,
	    sending_io.output_length);
	receiving_io.input_length = sending_io.output_length;
	return expect(rx_header(&receiver,1000) == ZDATA,
	    "receive binary header") &&
	    expect(receiver.rxd_header_len == HDRLEN,
	    "binary header length") &&
	    expect(receiver.receive_32_bit_data == use_crc32,
	    "binary header CRC selection") &&
	    expect(memcmp(receiver.rxd_header,header,sizeof(header)) == 0,
	    "binary header bytes");
}

static bool
test_extended_variable_header(void)
{
	static const uint8_t input[] =
	    "**\030" "b1001000007af000000000000000000000000397e\r\212\021";
	static const uint8_t header[ZMODEM90_ZRPOS_HEADER_LEN] = {
		ZRPOS,0U,0U,0U,0U,0U,0U,0U
	};
	struct zmodem protocol;
	struct fake_io fake;
	unsigned crc32;
	bool passed;

	initialize(&protocol,&fake);
	(void)memcpy(fake.input,input,sizeof(input) - 1U);
	fake.input_length = sizeof(input) - 1U;
	passed = expect(rx_header(&protocol,1000) == ZRINIT,
	    "receive extended variable header");
	passed = expect(protocol.rxd_header_len == ZMAXHLEN + 1U,
	    "maximum variable header length") && passed;
	passed = expect(protocol.rxd_header[4] == UINT8_C(0xaf),
	    "extended variable header contents") && passed;
	for (crc32=0U;crc32<2U;crc32++) {
		struct zmodem receiver;
		struct fake_io receiving_io;

		initialize(&protocol,&fake);
		protocol.can_fcs_32 = true;
		protocol.want_fcs_32 = crc32 != 0U;
		passed = expect(tx_header_length(&protocol,header,sizeof(header)) == 0,
		    "transmit extended binary header") && passed;
		initialize(&receiver,&receiving_io);
		(void)memcpy(receiving_io.input,fake.output,fake.output_length);
		receiving_io.input_length = fake.output_length;
		passed = expect(rx_header(&receiver,1000) == ZRPOS,
		    "receive extended binary header") && passed;
		passed = expect(receiver.rxd_header_len == sizeof(header),
		    "extended binary header length") && passed;
	}
	return passed;
}

static bool
test_variable_header_rejection(void)
{
	static const uint8_t header[HDRLEN] = { ZDATA,1U,2U,3U,4U };
	struct zmodem sender;
	struct zmodem receiver;
	struct fake_io sending_io;
	struct fake_io receiving_io;
	bool passed = true;

	initialize(&receiver,&receiving_io);
	receiving_io.input[0] = ZPAD;
	receiving_io.input[1] = ZDLE;
	receiving_io.input[2] = ZVBIN;
	receiving_io.input_length = 3U;
	passed = expect(rx_header(&receiver,1000) == ZMODEM_TIMEOUT,
	    "report missing variable header length") && passed;

	initialize(&receiver,&receiving_io);
	receiving_io.input[0] = ZPAD;
	receiving_io.input[1] = ZDLE;
	receiving_io.input[2] = ZVBIN;
	receiving_io.input[3] = HDRLEN - 2U;
	receiving_io.input_length = 4U;
	passed = expect(rx_header_and_check(&receiver,1000) == ZMODEM_TIMEOUT,
	    "reject short variable header") && passed;

	initialize(&receiver,&receiving_io);
	receiving_io.input[0] = ZPAD;
	receiving_io.input[1] = ZDLE;
	receiving_io.input[2] = ZVBIN;
	receiving_io.input[3] = ZMAXHLEN + 1U;
	receiving_io.input_length = 4U;
	passed = expect(rx_header_and_check(&receiver,1000) == ZMODEM_TIMEOUT,
	    "reject oversized variable header") && passed;

	initialize(&sender,&sending_io);
	sender.can_fcs_32 = true;
	sender.use_variable_headers = true;
	passed = expect(tx_header(&sender,header) == 0,
	    "make variable RLE header") && passed;
	sending_io.output[3] = ZVBINR32;
	initialize(&receiver,&receiving_io);
	(void)memcpy(receiving_io.input,sending_io.output,
	    sending_io.output_length);
	receiving_io.input_length = sending_io.output_length;
	passed = expect(rx_header(&receiver,1000) == ZDATA,
	    "receive variable RLE header") && passed;
	passed = expect(receiver.receive_encoded_data == ZMODEM_ENCODED_DATA_RLE,
	    "variable RLE header selects decoder") && passed;
	return passed;
}

static bool
test_header_read_sweep(unsigned style)
{
	struct zmodem sender;
	struct zmodem receiver;
	struct fake_io sending_io;
	struct fake_io receiving_io;
	uint8_t header[HDRLEN] = { ZDATA,1U,2U,3U,4U };
	size_t cut;
	bool passed = true;

	initialize(&sender,&sending_io);
	passed = expect(transmit_header_style(&sender,header,style,false) == 0,
	    "make header for read-failure sweep") && passed;
	for (cut = 0U; cut < sending_io.output_length; cut++) {
		int result;

		initialize(&receiver,&receiving_io);
		(void)memcpy(receiving_io.input,sending_io.output,cut);
		receiving_io.input_length = cut;
		result = rx_header(&receiver,1000);
		if ((style == 0U) && (cut + 1U == sending_io.output_length)) {
			passed = expect(result == ZDATA,
			    "hex header without trailing XON") && passed;
		}
		else {
			passed = expect(result < 0,"truncated header") && passed;
		}
	}
	return passed;
}

static int
transmit_header_style(struct zmodem * protocol,const uint8_t * header,
    unsigned style,bool variable)
{
	protocol->use_variable_headers = variable;
	if (style == 0U) {
		return tx_hex_header(protocol,header);
	}
	if (style == 3U) {
		protocol->escape_8th_bit = true;
		return tx_header(protocol,header);
	}
	if (style == 4U) {
		protocol->use_mobyturbo = true;
		return tx_header(protocol,header);
	}
	if (style == 5U) {
		protocol->escape_8th_bit = true;
		protocol->use_pack7 = true;
		return tx_header(protocol,header);
	}
	protocol->can_fcs_32 = true;
	protocol->want_fcs_32 = style == 2U;
	return tx_header(protocol,header);
}

static bool
test_header_write_failures(void)
{
	uint8_t header[HDRLEN] = { ZDATA,ZDLE,XON,CR,UINT8_C(0xff) };
	struct zmodem protocol;
	struct fake_io fake;
	unsigned style;
	unsigned variable_value;
	bool passed = true;

	for (style = 0U; style < 6U; style++) {
		for (variable_value = 0U; variable_value < 2U; variable_value++) {
			if ((style == 3U || style == 5U) && variable_value != 0U) {
				continue;
			}
			size_t call;
			size_t successful_calls;

			initialize(&protocol,&fake);
			passed = expect(transmit_header_style(&protocol,header,style,
			    variable_value != 0U) == 0,
			    "successful header for failure sweep") && passed;
			successful_calls = fake.write_calls;
			for (call = 1U; call <= successful_calls; call++) {
				initialize(&protocol,&fake);
				fake.fail_write_call = call;
				passed = expect(transmit_header_style(&protocol,header,
				    style,variable_value != 0U) != 0,
				    "reported header write failure") && passed;
			}
		}
	}
	return passed;
}

static bool
test_header_read_failures(void)
{
	static const uint8_t prefix[] = { ZPAD,ZDLE,ZBIN };
	static const uint8_t good_hex[] = {
		ZPAD,ZPAD,ZDLE,ZHEX,
		'0','0','0','0','0','0','0','0','0','0',
		'0','0','0','0',CR,LF
	};
	struct zmodem protocol;
	struct fake_io fake;
	size_t index;
	bool passed = true;

	passed = test_header_read_sweep(0U) && passed;
	passed = test_header_read_sweep(1U) && passed;
	passed = test_header_read_sweep(2U) && passed;
	passed = test_header_read_sweep(4U) && passed;
	passed = test_reject_frame_end_in_header_body() && passed;
	passed = test_reject_frame_end_in_header_crc(false) && passed;
	passed = test_reject_frame_end_in_header_crc(true) && passed;

	initialize(&protocol,&fake);
	(void)memcpy(fake.input,prefix,sizeof(prefix));
	fake.input_length = sizeof(prefix);
	passed = expect(rx_header(&protocol,1000) == TIMEOUT,
	    "binary header timeout") && passed;
	initialize(&protocol,&fake);
	(void)memcpy(fake.input,prefix,sizeof(prefix));
	fake.input_length = sizeof(prefix);
	fake.empty_read_result = ZMODEM_IO_ERROR;
	passed = expect(rx_header(&protocol,1000) == ZMODEM_IO_ERROR,
	    "binary header input error") && passed;
	initialize(&protocol,&fake);
	(void)memcpy(fake.input,prefix,sizeof(prefix));
	for (index = 0U; index < 5U; index++) {
		fake.input[sizeof(prefix) + index] = CAN;
	}
	fake.input_length = sizeof(prefix) + 5U;
	passed = expect(rx_header(&protocol,1000) == ZMODEM_CANCELLED,
	    "binary header cancellation") && passed;

	initialize(&protocol,&fake);
	(void)memcpy(fake.input,good_hex,sizeof(good_hex));
	fake.input_length = sizeof(good_hex);
	passed = expect(rx_header_and_check(&protocol,1000) == ZRQINIT,
	    "checked valid header") && passed;
	initialize(&protocol,&fake);
	(void)memcpy(fake.input,prefix,2U);
	fake.input[2] = ZHEX;
	fake.input[3] = '/';
	fake.input_length = 4U;
	passed = expect(rx_header(&protocol,1000) == TIMEOUT,
	    "reject low invalid hex digit") && passed;
	initialize(&protocol,&fake);
	(void)memcpy(fake.input,prefix,2U);
	fake.input[2] = ZHEX;
	fake.input[3] = ':';
	fake.input_length = 4U;
	passed = expect(rx_header(&protocol,1000) == TIMEOUT,
	    "reject punctuation in hex digit") && passed;
	initialize(&protocol,&fake);
	(void)memcpy(fake.input,prefix,2U);
	fake.input[2] = ZHEX;
	fake.input[3] = 'g';
	fake.input_length = 4U;
	passed = expect(rx_header(&protocol,1000) == TIMEOUT,
	    "reject high invalid hex digit") && passed;
	initialize(&protocol,&fake);
	(void)memcpy(fake.input,prefix,2U);
	fake.input[2] = ZHEX;
	fake.input[3] = '/';
	fake.input_length = 4U;
	passed = expect(rx_header_and_check(&protocol,1000) == TIMEOUT,
	    "checked invalid hex digit") && passed;
	passed = expect(fake.output_length > 0U,
	    "invalid hex digit sends ZNAK") && passed;

	for (index = 1U; index <= 4U; index++) {
		struct zmodem sender;
		struct fake_io sending_io;

		if (index == 3U) {
			continue;
		}

		initialize(&sender,&sending_io);
		passed = expect(transmit_header_style(&sender,
		    (const uint8_t[]){ ZDATA,1U,2U,3U,4U },(unsigned)index,
		    false) == 0,"make corrupt binary header") && passed;
		initialize(&protocol,&fake);
		(void)memcpy(fake.input,sending_io.output,
		    sending_io.output_length);
		fake.input_length = sending_io.output_length;
		fake.input[fake.input_length - 1U] ^= UINT8_C(1);
		passed = expect(rx_header_and_check(&protocol,1000) == TIMEOUT,
		    "reject corrupt binary header CRC") && passed;
		passed = expect(fake.output_length > 0U,
		    "corrupt binary header sends ZNAK") && passed;
	}
	return passed;
}

static bool
test_header_variants(void)
{
	static const uint8_t good_hex[] = {
		ZPAD,ZPAD,ZDLE,ZHEX,
		'0','0','0','0','0','0','0','0','0','0',
		'0','0','0','0',CR,LF
	};
	struct zmodem protocol;
	struct fake_io fake;
	uint8_t header[HDRLEN] = { ZRQINIT,0U,0U,0U,0U };
	uint8_t input[sizeof(good_hex) + 3U];
	bool passed = true;

	passed = header_round_trip(false,false) && passed;
	passed = header_round_trip(true,false) && passed;
	passed = header_round_trip(false,true) && passed;
	passed = header_round_trip(true,true) && passed;
	passed = test_extended_variable_header() && passed;
	passed = test_variable_header_rejection() && passed;
	passed = test_omen_header_round_trip() && passed;
	passed = test_omen_header_rejection() && passed;
	passed = test_mobyturbo_round_trip() && passed;
	passed = test_mobyturbo_probe() && passed;
	initialize(&protocol,&fake);
	protocol.use_variable_headers = true;
	passed = expect(tx_hex_header(&protocol,header) == 0,
	    "transmit variable hex header") && passed;
	if (expect(fake.output_length > 4U,"variable hex header length")) {
		passed = expect(fake.output[3] == ZVHEX,"variable hex marker") &&
		    passed;
	}
	else {
		passed = false;
	}
	initialize(&protocol,&fake);
	(void)memcpy(fake.input,"**\030B00000000000001\r\n",21U);
	fake.input_length = 21U;
	passed = expect(rx_header_and_check(&protocol,1000) == TIMEOUT,
	    "invalid header followed by timeout") && passed;
	passed = expect(fake.output_length > 0U,"invalid header sends ZNAK") &&
	    passed;

	initialize(&protocol,&fake);
	(void)memcpy(fake.input,good_hex,sizeof(good_hex));
	fake.input[sizeof(good_hex) - 2U] = LF;
	fake.input_length = sizeof(good_hex) - 1U;
	passed = expect(rx_header_and_check(&protocol,1000) == ZRQINIT,
	    "hex header with LF-only terminator") && passed;

	initialize(&protocol,&fake);
	(void)memcpy(fake.input,good_hex,sizeof(good_hex));
	fake.input[sizeof(good_hex) - 2U] = 'X';
	fake.input_length = sizeof(good_hex) - 1U;
	passed = expect(rx_header_and_check(&protocol,1000) == TIMEOUT,
	    "reject invalid hex terminator") && passed;
	passed = expect(fake.output_length > 0U,
	    "invalid terminator sends ZNAK") && passed;

	initialize(&protocol,&fake);
	input[0] = ZPAD;
	input[1] = ZDLE;
	input[2] = '?';
	(void)memcpy(&input[3],good_hex,sizeof(good_hex));
	(void)memcpy(fake.input,input,sizeof(input));
	fake.input_length = sizeof(input);
	passed = expect(rx_header(&protocol,1000) == ZRQINIT,
	    "unchecked header skips unknown style") && passed;

	initialize(&protocol,&fake);
	input[0] = ZPAD;
	input[1] = 'X';
	(void)memcpy(&input[2],good_hex,sizeof(good_hex));
	(void)memcpy(fake.input,input,sizeof(good_hex) + 2U);
	fake.input_length = sizeof(good_hex) + 2U;
	passed = expect(rx_header(&protocol,1000) == ZRQINIT,
	    "header scanner skips spurious pad") && passed;

	initialize(&protocol,&fake);
	fake.input[0] = ZPAD;
	fake.input[1] = ZDLE;
	fake.input[2] = '?';
	fake.input_length = 3U;
	fake.fail_write_call = 1U;
	passed = expect(rx_header_and_check(&protocol,1000) == ZMODEM_IO_ERROR,
	    "report ZNAK write failure") && passed;
	return passed;
}

int
main(void)
{
	bool passed = true;
	static const struct {
		int result;
		const char * description;
	} descriptions[] = {
		{ ZMODEM_INVALID_ARGUMENT,"invalid protocol argument" },
		{ ZMODEM_TIMEOUT,"protocol timeout" },
		{ ZMODEM_CANCELLED,"transfer cancelled" },
		{ ZMODEM_INVALID_DATA,"invalid protocol data" },
		{ ZMODEM_INVALID_HEADER,"invalid protocol header" },
		{ ZMODEM_IO_ERROR,"transport I/O error" },
		{ ZABORT,"remote abort" },
		{ ZNAK,"negative acknowledgement" },
		{ ZFERR,"remote file error" },
		{ ZCAN,"remote cancellation" },
		{ ZRINIT,"unexpected protocol response" }
	};
	size_t index;

	for (index = 0U; index < sizeof(descriptions) / sizeof(descriptions[0]);
	    index++) {
		passed = expect(strcmp(zmodem_result_description(
		    descriptions[index].result),descriptions[index].description) == 0,
		    "protocol result description") && passed;
	}

	passed = test_header_position() && passed;
	passed = test_transmit_hex_header() && passed;
	passed = test_receive_hex_header() && passed;
	passed = test_receive_empty_data() && passed;
	passed = test_transport_errors() && passed;
	passed = test_initialization_and_buffering() && passed;
	passed = test_data_packets() && passed;
	passed = test_protocol_maximum_overflow(false) && passed;
	passed = test_protocol_maximum_overflow(true) && passed;
	passed = test_pack7_round_trip() && passed;
	passed = test_pack7_rejection() && passed;
	passed = test_maximum_pack7_round_trip() && passed;
	passed = test_header_variants() && passed;
	passed = test_header_write_failures() && passed;
	passed = test_header_read_failures() && passed;
	return passed ? 0 : 1;
}

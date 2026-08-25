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
test_eighth_bit_escaping(void)

{
	uint8_t payload[128];
	struct zmodem sender;
	struct zmodem receiver;
	struct fake_io sending_io;
	struct fake_io receiving_io;
	uint8_t received[sizeof(payload)];
	uint8_t frame_end;
	size_t length;
	unsigned value;
	bool passed;

	for (value=0;value<sizeof(payload);value++) {
		payload[value] = (uint8_t)(value | 0x80U);
	}
	initialize(&sender,&sending_io);
	sender.escape_8th_bit = true;
	passed = expect(tx_data(&sender,ZCRCE,payload,sizeof(payload)) == 0,
	    "transmit with eighth-bit escaping");
	passed = expect(sending_io.output_length >= 4U,
	    "eighth-bit escaped wire length") && passed;
	passed = expect(sending_io.output[0] == ZDLE,
	    "first eighth-bit escape marker") && passed;
	passed = expect(sending_io.output[1] ==
	    (uint8_t)(payload[0] ^ 0x40U),
	    "first eighth-bit escaped value") && passed;
	passed = expect(sending_io.output[2] == ZDLE,
	    "second eighth-bit escape marker") && passed;
	passed = expect(sending_io.output[3] ==
	    (uint8_t)(payload[1] ^ 0x40U),
	    "second eighth-bit escaped value") && passed;

	initialize(&receiver,&receiving_io);
	receiver.receive_escaped_8th_bit = true;
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
test_data_packets(void)
{
	struct zmodem protocol;
	struct fake_io fake;
	uint8_t payload[1] = { 0U };
	uint8_t received[1];
	uint8_t frame_end;
	size_t length;
	bool passed = true;

	passed = data_round_trip(false,false,ZCRCW,ENDOFFRAME) && passed;
	passed = data_round_trip(false,true,ZCRCW,ENDOFFRAME) && passed;
	passed = data_round_trip(true,false,ZCRCW,ENDOFFRAME) && passed;
	passed = data_round_trip(true,true,ZCRCW,ENDOFFRAME) && passed;
	passed = data_round_trip(false,false,ZCRCE,ENDOFFRAME) && passed;
	passed = data_round_trip(false,false,ZCRCG,FRAMEOK) && passed;
	passed = data_round_trip(false,false,ZCRCQ,FRAMEOK) && passed;
	passed = test_eighth_bit_escaping() && passed;
	passed = test_data_read_failures(false) && passed;
	passed = test_data_read_failures(true) && passed;
	passed = test_receive_escape_variants() && passed;
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

static bool
header_round_trip(bool use_crc32)
{
	struct zmodem sender;
	struct zmodem receiver;
	struct fake_io sending_io;
	struct fake_io receiving_io;
	uint8_t header[HDRLEN] = { ZDATA,1U,2U,3U,4U };

	initialize(&sender,&sending_io);
	sender.can_fcs_32 = true;
	sender.want_fcs_32 = use_crc32;
	if (!expect(tx_header(&sender,header) == 0,"transmit binary header")) {
		return false;
	}
	initialize(&receiver,&receiving_io);
	(void)memcpy(receiving_io.input,sending_io.output,
	    sending_io.output_length);
	receiving_io.input_length = sending_io.output_length;
	return expect(rx_header(&receiver,1000) == ZDATA,
	    "receive binary header") &&
	    expect(receiver.receive_32_bit_data == use_crc32,
	    "binary header CRC selection") &&
	    expect(memcmp(receiver.rxd_header,header,sizeof(header)) == 0,
	    "binary header bytes");
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

	for (style = 0U; style < 3U; style++) {
		for (variable_value = 0U; variable_value < 2U; variable_value++) {
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

	for (index = 1U; index <= 2U; index++) {
		struct zmodem sender;
		struct fake_io sending_io;

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

	passed = header_round_trip(false) && passed;
	passed = header_round_trip(true) && passed;
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
	passed = test_header_variants() && passed;
	passed = test_header_write_failures() && passed;
	passed = test_header_read_failures() && passed;
	return passed ? 0 : 1;
}

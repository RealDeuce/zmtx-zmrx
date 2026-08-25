/******************************************************************************/
/* Project : Unite!       File : zmodem general        Version : 1.02         */
/*                                                                            */
/* Copyright (c) 1994 Stephen Hurd                                            */
/* All rights reserved.                                                       */
/*                                                                            */
/* Redistribution and use in source and binary forms, with or without         */
/* modification, are permitted provided that the following conditions are met:*/
/*                                                                            */
/* 1. Redistributions of source code must retain the above copyright notice,  */
/*    this list of conditions and the following disclaimer.                   */
/*                                                                            */
/* 2. Redistributions in binary form must reproduce the above copyright       */
/*    notice, this list of conditions and the following disclaimer in the     */
/*    documentation and/or other materials provided with the distribution.    */
/*                                                                            */
/* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"*/
/* AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE  */
/* IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE */
/* ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE  */
/* LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR        */
/* CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF       */
/* SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS   */
/* INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN    */
/* CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)    */
/* ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE */
/* POSSIBILITY OF SUCH DAMAGE.                                                */
/******************************************************************************/

/*
 * Special thanks to Jacques Mattheij, formerly of Mattheij Computer Service,
 * and original author of zmtx/zmrx.
 */

/*
 * zmodem primitives and other code common to zmtx and zmrx
 */

#include <string.h>

#include "zmodem.h"
#include "zmdm.h"
#include "crctab.h"

const char *
zmodem_result_description(int result)
{
	switch (result) {
		case ZMODEM_INVALID_ARGUMENT:
			return "invalid protocol argument";
		case ZMODEM_IO_ERROR:
			return "transport I/O error";
		case ZMODEM_CANCELLED:
			return "transfer cancelled";
		case ZMODEM_INVALID_DATA:
			return "invalid protocol data";
		case ZMODEM_INVALID_HEADER:
			return "invalid protocol header";
		case ZMODEM_TIMEOUT:
			return "protocol timeout";
		case ZABORT:
			return "remote abort";
		case ZNAK:
			return "negative acknowledgement";
		case ZFERR:
			return "remote file error";
		case ZCAN:
			return "remote cancellation";
		default:
			return "unexpected protocol response";
	}
}

enum tx_class {
	TX_NORMAL = 0,
	TX_ESCAPE_ALWAYS = 1,
	TX_ESCAPE_CONTROL = 2,
	TX_ESCAPE_CR = 4,
	TX_ESCAPE_8TH = 8
};

int
zmodem_init(struct zmodem * restrict zmodem,
    const struct zmodem_io * restrict io)
{
	if ((zmodem == NULL) || (io == NULL) || (io->read == NULL) ||
	    (io->write == NULL) || (io->flush == NULL) || (io->poll == NULL) ||
	    (io->purge == NULL)) {
		return ZMODEM_INVALID_ARGUMENT;
	}
	(void)memset(zmodem,0,sizeof(*zmodem));
	zmodem->io = *io;
	zmodem->want_fcs_32 = true;
	zmodem->last_sent = -1;
	return ZMODEM_OK;
}

/*
 * read bytes as long as rdchk indicates that
 * more data is available.
 */

int
rx_purge(struct zmodem * zmodem)
{
	zmodem->input_count = 0U;
	zmodem->input_index = 0U;
	return zmodem->io.purge(zmodem->io.context);
}

/* 
 * transmit a character. 
 * this is the raw modem interface
 */

int
tx_raw(struct zmodem * zmodem,int c)
{
	uint8_t byte = (uint8_t)c;

	if (zmodem->io.write(zmodem->io.context,&byte,1U) != ZMODEM_OK) {
		return -1;
	}
	zmodem->last_sent = c & 0x7f;
	return 0;
}

static int
tx_esc(struct zmodem * zmodem,int c)

{
	if (tx_raw(zmodem,ZDLE) != 0) {
		return -1;
	}
	return tx_raw(zmodem,c ^ 0x40);
}

static void
initialize_tx_classes(struct zmodem * zmodem)

{
	unsigned c;

	for (c=0;c<256;c++) {
		unsigned action;

		if (c == ZDLE || c == 0x10 || c == 0x90 || c == XON ||
		    c == 0x91 || c == XOFF || c == 0x93) {
			action = TX_ESCAPE_ALWAYS;
		}
		else if (c == CR || c == 0x8d) {
			action = TX_ESCAPE_CR;
		}
		else if ((c & 0x60) == 0) {
			action = TX_ESCAPE_CONTROL;
		}
		else {
			action = TX_NORMAL;
		}
		if ((c & 0x80) != 0) {
			action |= TX_ESCAPE_8TH;
		}
		zmodem->tx_classes[c] = (uint8_t)action;
	}
	zmodem->tx_classes_initialized = true;
}

static unsigned
active_tx_classes(struct zmodem * zmodem)

{
	if (!zmodem->tx_classes_initialized) {
		initialize_tx_classes(zmodem);
	}
	return TX_ESCAPE_ALWAYS |
	    (zmodem->escape_all_control_characters ?
	    TX_ESCAPE_CONTROL | TX_ESCAPE_CR : 0U) |
	    (zmodem->escape_8th_bit ? TX_ESCAPE_8TH : 0U);
}

static bool
tx_byte_needs_escape(const struct zmodem * zmodem,uint8_t c,int previous,
    unsigned active)

{
	unsigned action = (unsigned)zmodem->tx_classes[c] & active;

	if (action == TX_ESCAPE_CR) {
		return previous == '@';
	}
	return action != TX_NORMAL;
}

static int
tx(struct zmodem * zmodem,uint8_t c)

{
	if (tx_byte_needs_escape(zmodem,c,zmodem->last_sent,
	    active_tx_classes(zmodem))) {
		return tx_esc(zmodem,c);
	}
	return tx_raw(zmodem,(int)c);
}

int
tx_flush(struct zmodem * zmodem)

{
	return (zmodem->io.flush(zmodem->io.context) == ZMODEM_OK) ? 0 : -1;
}

static int
tx_nibble(struct zmodem * zmodem,int n)

{
	n &= 0x0f;
	if (n < 10) {
		n += '0';
	}
	else {
		n += 'a' - 10;
	}
	return tx_raw(zmodem,n);
}

static int
tx_hex(struct zmodem * zmodem,int h)

{
	if (tx_nibble(zmodem,h >> 4) != 0) {
		return -1;
	}
	return tx_nibble(zmodem,h);
}

int
tx_hex_header(struct zmodem * restrict zmodem,
    const uint8_t * restrict p)

{
	size_t i;
	uint8_t type = p[FTYPE];
	uint16_t crc = 0;

	if (tx_raw(zmodem,ZPAD) != 0 || tx_raw(zmodem,ZPAD) != 0 ||
	    tx_raw(zmodem,ZDLE) != 0) {
		return -1;
	}
	if (zmodem->use_variable_headers) {
		if (tx_raw(zmodem,ZVHEX) != 0 || tx_hex(zmodem,HDRLEN) != 0) {
			return -1;
		}
	}
	else if (tx_raw(zmodem,ZHEX) != 0) {
		return -1;
	}

	for (i=0;i<HDRLEN;i++) {
		if (tx_hex(zmodem,*p) != 0) {
			return -1;
		}
		crc = crc16_update(crc,*p);
		p += 1U;
	}
	crc = crc16_update(crc,0U);
	crc = crc16_update(crc,0U);
	if (tx_hex(zmodem,crc >> 8) != 0 || tx_hex(zmodem,crc) != 0 ||
	    tx_raw(zmodem,CR) != 0 || tx_raw(zmodem,LF) != 0) {
		return -1;
	}
	if (type != ZACK && type != ZFIN && tx_raw(zmodem,XON) != 0) {
		return -1;
	}
	return tx_flush(zmodem);
}

static int
tx_bin32_header(struct zmodem * restrict zmodem,
    const uint8_t * restrict p)

{
	size_t i;
	uint32_t crc = UINT32_MAX;

	if (tx_raw(zmodem,ZPAD) != 0 || tx_raw(zmodem,ZPAD) != 0 ||
	    tx_raw(zmodem,ZDLE) != 0) {
		return -1;
	}
	if (zmodem->use_variable_headers) {
		if (tx_raw(zmodem,ZVBIN32) != 0 || tx(zmodem,HDRLEN) != 0) {
			return -1;
		}
	}
	else if (tx_raw(zmodem,ZBIN32) != 0) {
		return -1;
	}
	for (i=0;i<HDRLEN;i++) {
		crc = crc32_byte_update(crc,*p);
		if (tx(zmodem,*p++) != 0) {
			return -1;
		}
	}
	crc = ~crc;
	if (tx(zmodem,(uint8_t)crc) != 0 ||
	    tx(zmodem,(uint8_t)(crc >> 8)) != 0 ||
	    tx(zmodem,(uint8_t)(crc >> 16)) != 0 ||
	    tx(zmodem,(uint8_t)(crc >> 24)) != 0) {
		return -1;
	}
	return 0;
}

static int
tx_bin16_header(struct zmodem * restrict zmodem,
    const uint8_t * restrict p)

{
	size_t i;
	uint16_t crc = 0;

	if (tx_raw(zmodem,ZPAD) != 0 || tx_raw(zmodem,ZPAD) != 0 ||
	    tx_raw(zmodem,ZDLE) != 0) {
		return -1;
	}
	if (zmodem->use_variable_headers) {
		if (tx_raw(zmodem,ZVBIN) != 0 || tx(zmodem,HDRLEN) != 0) {
			return -1;
		}
	}
	else if (tx_raw(zmodem,ZBIN) != 0) {
		return -1;
	}
	for (i=0;i<HDRLEN;i++) {
		crc = crc16_update(crc,*p);
		if (tx(zmodem,*p++) != 0) {
			return -1;
		}
	}
	crc = crc16_update(crc,0U);
	crc = crc16_update(crc,0U);
	if (tx(zmodem,(uint8_t)(crc >> 8)) != 0 ||
	    tx(zmodem,(uint8_t)crc) != 0) {
		return -1;
	}
	return 0;
}

int
tx_header(struct zmodem * restrict zmodem,const uint8_t * restrict p)

{
	if (zmodem->can_fcs_32 && zmodem->want_fcs_32) {
		return tx_bin32_header(zmodem,p);
	}
	if (zmodem->can_fcs_32) {
		return tx_bin16_header(zmodem,p);
	}
	return tx_hex_header(zmodem,p);
}

static void
buffer_raw(struct zmodem * restrict zmodem,uint8_t c,
    size_t * restrict used,int * restrict previous)

{
	zmodem->tx_data_wire[*used] = c;
	*used += 1U;
	*previous = c & 0x7f;
}

static void
buffer_tx(struct zmodem * restrict zmodem,uint8_t c,
    size_t * restrict used,int * restrict previous,unsigned active)

{
	if (tx_byte_needs_escape(zmodem,c,*previous,active)) {
		buffer_raw(zmodem,ZDLE,used,previous);
		buffer_raw(zmodem,(uint8_t)(c ^ 0x40),used,previous);
	}
	else {
		buffer_raw(zmodem,c,used,previous);
	}
}

static inline size_t
tx_copy_plain_span(const uint8_t * classes,uint8_t * output,
    const uint8_t * data,size_t length)

{
	const uint64_t ones = UINT64_C(0x0101010101010101);
	const uint64_t highs = UINT64_C(0x8080808080808080);
	size_t span = 0U;

	while (length - span >= sizeof(uint64_t)) {
		uint64_t word;
		uint64_t zdle;
		uint64_t control;
		uint64_t flow;

		/* Find mandatory escapes while copying eight plain bytes at once. */
		(void)memcpy(&word,&data[span],sizeof(word));
		zdle = word ^ UINT64_C(0x1818181818181818);
		/* 0x10/0x90 share seven bits; flow controls vary in bits 1 and 7. */
		control = (word & UINT64_C(0x7f7f7f7f7f7f7f7f)) ^
		    UINT64_C(0x1010101010101010);
		flow = (word & UINT64_C(0x7d7d7d7d7d7d7d7d)) ^
		    UINT64_C(0x1111111111111111);
		if ((((zdle - ones) & ~zdle & highs) |
		    ((control - ones) & ~control & highs) |
		    ((flow - ones) & ~flow & highs)) != 0U) {
			break;
		}
		(void)memcpy(&output[span],&word,sizeof(word));
		span += sizeof(word);
	}
	while ((span < length) &&
	    (((unsigned)classes[data[span]] & TX_ESCAPE_ALWAYS) == 0U)) {
		output[span] = data[span];
		span += 1U;
	}
	return span;
}

int
tx_data(struct zmodem * restrict zmodem,uint8_t sub_frame_type,
    const uint8_t * restrict p,size_t l)

{
	int previous = zmodem->last_sent;
	size_t i;
	size_t used = 0;
	unsigned active = active_tx_classes(zmodem);

	if (l > ZMAXSPLEN) {
		return -1;
	}
	if (active == TX_ESCAPE_ALWAYS) {
		for (i=0;i<l;) {
			size_t span = tx_copy_plain_span(zmodem->tx_classes,
			    &zmodem->tx_data_wire[used],&p[i],l - i);

			i += span;
			used += span;
			if (i < l) {
				zmodem->tx_data_wire[used] = ZDLE;
				zmodem->tx_data_wire[used + 1U] =
				    (uint8_t)(p[i] ^ 0x40);
				used += 2U;
				i += 1U;
			}
		}
	}
	else {
		for (i=0;i<l;i++) {
			buffer_tx(zmodem,p[i],&used,&previous,active);
		}
	}
	buffer_raw(zmodem,ZDLE,&used,&previous);
	buffer_raw(zmodem,sub_frame_type,&used,&previous);

	if (zmodem->want_fcs_32 && zmodem->can_fcs_32) {
		uint32_t crc = crc32_update(UINT32_MAX,p,l);

		crc = ~crc32_byte_update(crc,sub_frame_type);
		buffer_tx(zmodem,(uint8_t)crc,&used,&previous,active);
		buffer_tx(zmodem,(uint8_t)(crc >> 8),&used,&previous,active);
		buffer_tx(zmodem,(uint8_t)(crc >> 16),&used,&previous,active);
		buffer_tx(zmodem,(uint8_t)(crc >> 24),&used,&previous,active);
	}
	else {
		uint16_t crc = 0;

		for (i=0;i<l;i++) {
			crc = crc16_update(crc,p[i]);
		}
		crc = crc16_update(crc,sub_frame_type);
		crc = crc16_update(crc,0U);
		crc = crc16_update(crc,0U);
		buffer_tx(zmodem,(uint8_t)(crc >> 8),&used,&previous,active);
		buffer_tx(zmodem,(uint8_t)crc,&used,&previous,active);
	}

	if (sub_frame_type == ZCRCW) {
		buffer_raw(zmodem,XON,&used,&previous);
	}
	if (zmodem->io.write(zmodem->io.context,zmodem->tx_data_wire,used) !=
	    ZMODEM_OK) {
		return -1;
	}
	zmodem->last_sent = previous;
	return tx_flush(zmodem);
}

uint32_t
zmodem_header_position(const uint8_t * header)

{
	return (uint32_t)header[ZP0] |
		((uint32_t)header[ZP1] << 8) |
		((uint32_t)header[ZP2] << 16) |
		((uint32_t)header[ZP3] << 24);
}

void
zmodem_set_header_position(uint8_t * header,uint32_t position)

{
	header[ZP0] = (uint8_t)position;
	header[ZP1] = (uint8_t)(position >> 8);
	header[ZP2] = (uint8_t)(position >> 16);
	header[ZP3] = (uint8_t)(position >> 24);
}

int
tx_pos_header(struct zmodem * zmodem,uint8_t type,uint32_t position)

{
	uint8_t header[HDRLEN] = { 0 };

	header[FTYPE] = type;
	zmodem_set_header_position(header,position);

	return tx_hex_header(zmodem,header);
}

int
tx_znak(struct zmodem * zmodem)

{
	return tx_pos_header(zmodem,ZNAK,UINT32_C(0));
}

/*
 * receive any style header within timeout milliseconds
 */

int
rx_poll(struct zmodem * zmodem)

{
	if (zmodem->input_count > 0U) {
		return 1;
	}
	return zmodem->io.poll(zmodem->io.context);
}

/*
 * rx_raw ; receive a single byte from the line.
 * reads as many are available and then processes them one at a time
 * check the data stream for 5 consecutive CAN characters;
 * and if you see them abort. this saves a lot of clutter in
 * the rest of the code; even though it is a very strange place
 * for an exit. (but that was wat session abort was all about.)
 */

struct rx_cursor {
	size_t input_count;
	size_t input_index;
	unsigned cancel_count;
};

static inline void
load_rx_cursor(const struct zmodem * restrict zmodem,
    struct rx_cursor * restrict cursor)

{
	cursor->input_count = zmodem->input_count;
	cursor->input_index = zmodem->input_index;
	cursor->cancel_count = zmodem->cancel_count;
}

static inline void
store_rx_cursor(struct zmodem * restrict zmodem,
    const struct rx_cursor * restrict cursor)

{
	zmodem->input_count = cursor->input_count;
	zmodem->input_index = cursor->input_index;
	zmodem->cancel_count = cursor->cancel_count;
}

static inline int
rx_cursor_byte(struct zmodem * restrict zmodem,int timeout_ms,
    struct rx_cursor * restrict cursor)

{
	uint8_t c;

	if (cursor->input_count == 0U) {
		size_t count;
		int result;

		/* Keep protocol state coherent while calling external I/O code. */
		store_rx_cursor(zmodem,cursor);
		result = zmodem->io.read(zmodem->io.context,
		    zmodem->input_buffer,sizeof(zmodem->input_buffer),&count,
		    timeout_ms);

		if (result != ZMODEM_OK) {
			return result;
		}
		if ((count == 0U) || (count > sizeof(zmodem->input_buffer))) {
			return ZMODEM_IO_ERROR;
		}
		cursor->input_count = count;
		cursor->input_index = 0U;
	}

	c = zmodem->input_buffer[cursor->input_index];
	cursor->input_index += 1U;
	cursor->input_count -= 1U;

	return c;
}

static inline int
rx_cursor_raw(struct zmodem * restrict zmodem,int timeout_ms,
    struct rx_cursor * restrict cursor)

{
	int result = rx_cursor_byte(zmodem,timeout_ms,cursor);

	if (result == CAN) {
		cursor->cancel_count += 1U;
		if (cursor->cancel_count == 5U) {
			result = ZMODEM_CANCELLED;
		}
	}
	else if (result >= 0) {
		cursor->cancel_count = 0U;
	}
	return result;
}

int
rx_raw(struct zmodem * zmodem,int timeout_ms)

{
	struct rx_cursor cursor;
	int result;

	load_rx_cursor(zmodem,&cursor);
	result = rx_cursor_raw(zmodem,timeout_ms,&cursor);
	store_rx_cursor(zmodem,&cursor);
	return result;
}

/*
 * rx; receive a single byte undoing any escaping at the
 * sending site. this bit looks like a mess. sorry for that
 * but there seems to be no other way without incurring a lot
 * of overhead. at least like this the path for a normal character
 * is relatively short.
 */

static bool
rx_is_flow_control(int c)
{
	return (c == 0x11) || (c == 0x91) || (c == 0x13) || (c == 0x93);
}

static bool
rx_needs_slow_path(int c)
{
	return (c == ZDLE) || rx_is_flow_control(c);
}

static int
rx_cursor_slow(struct zmodem * restrict zmodem,int timeout_ms,int c,
    struct rx_cursor * restrict cursor)
{
	for (;;) {
		while (rx_is_flow_control(c)) {
			c = rx_cursor_raw(zmodem,timeout_ms,cursor);
			if (c < 0) {
				return c;
			}
		}
		if (c != ZDLE) {
			return c;
		}

		/*
		 * ZDLE encoded sequence or session abort.
		 * (or something illegal; then back to the top)
		 */
		for (;;) {
			c = rx_cursor_raw(zmodem,timeout_ms,cursor);
			if (c < 0) {
				return c;
			}
			/*
			 * ESC8 quotes every high-bit byte.  Decode its quoted form
			 * before discarding high-bit XON and XOFF values, which can
			 * legitimately represent 0xd1 and 0xd3 here.
			 */
			if (zmodem->receive_escaped_8th_bit &&
			    (c & 0x80) != 0) {
				return c ^ 0x40;
			}

			if (rx_needs_slow_path(c)) {
				continue;
			}

			switch (c) {
				/*
				 * these four are really nasty.
				 * for convenience we just change them into 
				 * special characters by setting a bit outside the
				 * first 8. that way they can be recognized and still
				 * be processed as characters by the rest of the code.
				 */
				case ZCRCE:
				case ZCRCG:
				case ZCRCQ:
				case ZCRCW:
					return c | ZDLEESC;
				case ZRUB0:
					return 0x7f;
				case ZRUB1:
					return 0xff;
				default:
					/*
					 * legitimate escape sequence.
					 * rebuild the orignal and return it.
					 */
					if ((c & 0x60) == 0x40) {
						return c ^ 0x40;
					}
					break;
			}
			break;
		}
		c = rx_cursor_raw(zmodem,timeout_ms,cursor);
		if (c < 0) {
			return c;
		}
	}
}

static int
rx_slow(struct zmodem * zmodem,int timeout_ms,int c)

{
	struct rx_cursor cursor;

	load_rx_cursor(zmodem,&cursor);
	c = rx_cursor_slow(zmodem,timeout_ms,c,&cursor);
	store_rx_cursor(zmodem,&cursor);
	return c;
}

static inline int
rx_cursor(struct zmodem * restrict zmodem,int timeout_ms,
    struct rx_cursor * restrict cursor)

{
	int c = rx_cursor_raw(zmodem,timeout_ms,cursor);

	if (c < 0) {
		return c;
	}
	if (!rx_needs_slow_path(c)) {
		return c;
	}
	return rx_cursor_slow(zmodem,timeout_ms,c,cursor);
}

static inline size_t
rx_plain_span(const struct zmodem * restrict zmodem,
    const struct rx_cursor * restrict cursor)

{
	const uint8_t * data = &zmodem->input_buffer[cursor->input_index];
	const uint64_t ones = UINT64_C(0x0101010101010101);
	const uint64_t highs = UINT64_C(0x8080808080808080);
	size_t length = 0U;

	while (cursor->input_count - length >= sizeof(uint64_t)) {
		uint64_t word;
		uint64_t zdle;
		uint64_t flow;

		/* Detect ZDLE or flow control in eight bytes at a time. */
		(void)memcpy(&word,&data[length],sizeof(word));
		zdle = word ^ UINT64_C(0x1818181818181818);
		/* The four flow-control values vary only in bits 1 and 7. */
		flow = (word & UINT64_C(0x7d7d7d7d7d7d7d7d)) ^
		    UINT64_C(0x1111111111111111);
		if ((((zdle - ones) & ~zdle & highs) |
		    ((flow - ones) & ~flow & highs)) != 0U) {
			break;
		}
		length += sizeof(word);
	}
	while ((length < cursor->input_count) &&
	    !rx_needs_slow_path(data[length])) {
		length += 1U;
	}
	return length;
}

static inline int
rx(struct zmodem * zmodem,int timeout_ms)

{
	int c = rx_raw(zmodem,timeout_ms);

	if ((c >= 0) && rx_needs_slow_path(c)) {
		return rx_slow(zmodem,timeout_ms,c);
	}
	return c;
}

static int
rx_crc16(struct zmodem * restrict zmodem,int timeout_ms,
    uint16_t * restrict value)

{
	int high;
	int low;

	high = rx(zmodem,timeout_ms);
	if (high < 0) {
		return high;
	}
	low = rx(zmodem,timeout_ms);
	if (low < 0) {
		return low;
	}

	*value = (uint16_t)(((uint16_t)(uint8_t)high << 8) | (uint8_t)low);
	return 0;
}

static int
rx_crc32(struct zmodem * restrict zmodem,int timeout_ms,
    uint32_t * restrict value)

{
	int c;
	size_t i;
	uint32_t result = 0;

	for (i=0;i<sizeof(result);i++) {
		c = rx(zmodem,timeout_ms);
		if (c < 0) {
			return c;
		}
		result |= (uint32_t)(uint8_t)c << (i * 8);
	}

	*value = result;
	return 0;
}

/*
 * Receive a data subpacket as dictated by the last received header. Return
 * ENDOFFRAME or FRAMEOK for a valid packet, INVDATA for a corrupt or
 * oversized packet, or TIMEOUT. The caller writes the packet and sends any
 * requested acknowledgement so that the acknowledged offset is committed.
 */

/*
 * data subpacket reception
 */

static int
rx_32_data(struct zmodem * restrict zmodem,uint8_t * restrict p,
    size_t capacity,size_t * restrict l)

{
	int c;
	size_t limit = capacity < ZMAXSPLEN ? capacity : ZMAXSPLEN;
	size_t used = *l;
	uint32_t rxd_crc;
	uint32_t crc;
	int sub_frame_type;
	bool overflow = false;
	struct rx_cursor cursor;

	crc = UINT32_MAX;
	load_rx_cursor(zmodem,&cursor);

	for (;;) {
		size_t span = rx_plain_span(zmodem,&cursor);

		if (span > 0U) {
			const uint8_t * source =
			    &zmodem->input_buffer[cursor.input_index];
			size_t copied = span;

			cursor.input_index += span;
			cursor.input_count -= span;
			cursor.cancel_count = 0U;
			if (copied > limit - used) {
				copied = limit - used;
			}
			(void)memcpy(&p[used],source,copied);
			used += copied;
			if (copied < span) {
				if (!overflow) {
					crc = crc32_update(crc,p,used);
					overflow = true;
				}
				crc = crc32_update(crc,&source[copied],
				    span - copied);
			}
			continue;
		}
		c = rx_cursor(zmodem,1000,&cursor);

		if (c < 0) {
			store_rx_cursor(zmodem,&cursor);
			*l = used;
			return c;
		}
		if (c < 0x100) {
			if (used < limit) {
				p[used] = (uint8_t)c;
				used += 1U;
			}
			else {
				if (!overflow) {
					crc = crc32_update(crc,p,used);
					overflow = true;
				}
				crc = crc32_byte_update(crc,(uint8_t)c);
			}
			continue;
		}
		break;
	}
	store_rx_cursor(zmodem,&cursor);
	*l = used;

	sub_frame_type = c & 0xff;

	if (!overflow) {
		crc = crc32_update(crc,p,used);
	}
	crc = crc32_byte_update(crc,(uint8_t)sub_frame_type);

	crc = ~crc;

	{
		int result = rx_crc32(zmodem,1000,&rxd_crc);

		if (result != ZMODEM_OK) {
			return result;
		}
	}

	if (rxd_crc != crc) {
		return INVDATA;
	}
	if (overflow) {
		return INVDATA;
	}

	return sub_frame_type;
}

static int
rx_16_data(struct zmodem * restrict zmodem,uint8_t * restrict p,
    size_t capacity,size_t * restrict l)

{
	int c;
	int sub_frame_type;
	size_t limit = capacity < ZMAXSPLEN ? capacity : ZMAXSPLEN;
	size_t used = *l;
	uint16_t crc;
	uint16_t rxd_crc;
	bool overflow = false;
	struct rx_cursor cursor;

	crc = 0;
	load_rx_cursor(zmodem,&cursor);

	for (;;) {
		size_t span = rx_plain_span(zmodem,&cursor);

		if (span > 0U) {
			const uint8_t * source =
			    &zmodem->input_buffer[cursor.input_index];
			size_t copied = span;

			cursor.input_index += span;
			cursor.input_count -= span;
			cursor.cancel_count = 0U;
			if (copied > limit - used) {
				copied = limit - used;
			}
			(void)memcpy(&p[used],source,copied);
			used += copied;
			if (copied < span) {
				if (!overflow) {
					crc = crc16_buffer_update(crc,p,used);
					overflow = true;
				}
				crc = crc16_buffer_update(crc,&source[copied],
				    span - copied);
			}
			continue;
		}
		c = rx_cursor(zmodem,5000,&cursor);

		if (c < 0) {
			store_rx_cursor(zmodem,&cursor);
			*l = used;
			return c;
		}
		if (c < 0x100) {
			if (used < limit) {
				p[used] = (uint8_t)c;
				used += 1U;
			}
			else {
				if (!overflow) {
					crc = crc16_buffer_update(crc,p,used);
				}
				crc = crc16_update(crc,(uint8_t)c);
				overflow = true;
			}
			continue;
		}
		break;
	}
	store_rx_cursor(zmodem,&cursor);
	*l = used;

	sub_frame_type = c & 0xff;

	if (!overflow) {
		crc = crc16_buffer_update(crc,p,used);
	}
	crc = crc16_update(crc,(uint8_t)sub_frame_type);

	crc = crc16_update(crc,0U);
	crc = crc16_update(crc,0U);

	{
		int result = rx_crc16(zmodem,1000,&rxd_crc);

		if (result != ZMODEM_OK) {
			return result;
		}
	}

	if (rxd_crc != crc) {
		return INVDATA;
	}
	if (overflow) {
		return INVDATA;
	}

	return sub_frame_type;
}

int
rx_data(struct zmodem * restrict zmodem,uint8_t * restrict p,
    size_t capacity,size_t * restrict l,uint8_t * restrict frame_end)

{
	int sub_frame_type;

	/*
	 * receive the right type of frame
	 */

	*l = 0;
	*frame_end = 0;

	if (zmodem->receive_32_bit_data) {
		sub_frame_type = rx_32_data(zmodem,p,capacity,l);
	}
	else {	
		sub_frame_type = rx_16_data(zmodem,p,capacity,l);
	}
	if (sub_frame_type < 0) {
		return sub_frame_type;
	}
	*frame_end = (uint8_t)sub_frame_type;

	switch (sub_frame_type)  {
		/*
		 * frame continues non-stop
		 */
		case ZCRCG:
			return FRAMEOK;
		/*
		 * frame ends
		 */
		case ZCRCE:
			return ENDOFFRAME;
		/*
 		 * frame continues; ZACK expected
		 */
		case ZCRCQ:		
			return FRAMEOK;
		/*
		 * frame ends; ZACK expected
		 */
		case ZCRCW:
			return ENDOFFRAME;
		default:
			return INVDATA;
	}
}

static
int
rx_nibble(struct zmodem * zmodem,int timeout_ms)

{
	int c;

	c = rx(zmodem,timeout_ms);

	if (c < 0) {
		return c;
	}

	c &= 0x7f;
	if (c > '9') {
		if (c < 'a' || c > 'f') {
			/*
			 * illegal hex; different than expected.
			 * we might as well time out.
			 */
			return TIMEOUT;
		}

		c -= 'a' - 10;
	}
	else {
		if (c < '0') {
			/*
			 * illegal hex; different than expected.
			 * we might as well time out.
			 */
			return TIMEOUT;
		}
		c -= '0';
	}

	return c;
}

static int
rx_hex(struct zmodem * zmodem,int timeout_ms)

{
	int n1;
	int n0;

	n1 = rx_nibble(zmodem,timeout_ms);

	if (n1 < 0) {
		return n1;
	}

	n0 = rx_nibble(zmodem,timeout_ms);

	if (n0 < 0) {
		return n0;
	}

	return (n1 << 4) | n0;
}

/*
 * receive routines for each of the six different styles of header.
 * each of these leaves rxd_header_len set to 0 if the end result is
 * not a valid header.
 */

static int
rx_bin16_header(struct zmodem * zmodem,int timeout_ms)

{
	int c;
	size_t n;
	uint16_t crc;
	uint16_t rxd_crc;

	crc = 0;

	for (n=0;n<5;n++) {
		c = rx(zmodem,timeout_ms);
		if (c < 0) {
			return c;
		}
		crc = crc16_update(crc,(uint8_t)c);
		zmodem->rxd_header[n] = (uint8_t)c;
	}

	crc = crc16_update(crc,0U);
	crc = crc16_update(crc,0U);

	{
		int result = rx_crc16(zmodem,1000,&rxd_crc);

		if (result != ZMODEM_OK) {
			return result;
		}
	}

	if (rxd_crc != crc) {
		return ZMODEM_INVALID_HEADER;
	}

	zmodem->rxd_header_len = HDRLEN;
	return ZMODEM_OK;
}

static int
rx_hex_header(struct zmodem * zmodem,int timeout_ms)

{
	int c;
	size_t i;
	uint16_t crc = 0;
	uint16_t rxd_crc;

	for (i=0;i<5;i++) {
		c = rx_hex(zmodem,timeout_ms);
		if (c < 0) {
			return c;
		}
		crc = crc16_update(crc,(uint8_t)c);

		zmodem->rxd_header[i] = (uint8_t)c;
	}

	crc = crc16_update(crc,0U);

	crc = crc16_update(crc,0U);

	/*
	 * receive the crc
	 */

	c = rx_hex(zmodem,timeout_ms);

	if (c < 0) {
		return c;
	}

	rxd_crc = (uint16_t)((uint16_t)(uint8_t)c << 8);

	c = rx_hex(zmodem,timeout_ms);

	if (c < 0) {
		return c;
	}

	rxd_crc |= (uint8_t)c;

	if (rxd_crc != crc) {
		return ZMODEM_INVALID_HEADER;
	}

	/*
	 * drop the end of line sequence after a hex header
	 */
	c = rx(zmodem,timeout_ms);
	if (c < 0) {
		return c;
	}
	c &= 0x7f;
	if (c == CR) {
		/*
		 * both are expected with CR
		 */
		c = rx(zmodem,timeout_ms);
		if (c < 0) {
			return c;
		}
		c &= 0x7f;
	}
	if (c != LF) {
		return ZMODEM_INVALID_HEADER;
	}
	zmodem->rxd_header_len = HDRLEN;
	return ZMODEM_OK;
}

static int
rx_bin32_header(struct zmodem * zmodem,int timeout_ms)

{
	int c;
	size_t n;
	uint32_t crc;
	uint32_t rxd_crc;

	crc = UINT32_MAX;

	for (n=0;n<5;n++) {
		c = rx(zmodem,timeout_ms);
		if (c < 0) {
			return c;
		}
		crc = crc32_byte_update(crc,(uint8_t)c);
		zmodem->rxd_header[n] = (uint8_t)c;
	}

	crc = ~crc;

	{
		int result = rx_crc32(zmodem,timeout_ms,&rxd_crc);

		if (result != ZMODEM_OK) {
			return result;
		}
	}

	if (rxd_crc != crc) {
		return ZMODEM_INVALID_HEADER;
	}

	zmodem->rxd_header_len = HDRLEN;
	return ZMODEM_OK;
}

/*
 * receive any style header
 * if the errors flag is set than whenever an invalid header packet is
 * received INVHDR will be returned. otherwise we wait for a good header
 * also; a flag (receive_32_bit_data) will be set to indicate whether data
 * packets following this header will have 16 or 32 bit data attached.
 * variable headers are not implemented.
 */

static int
rx_header_raw(struct zmodem * zmodem,int timeout_ms,bool report_errors)

{
	int c;
	int result;

	zmodem->rxd_header_len = 0U;

	do {
		do {
			c = rx_raw(zmodem,timeout_ms);
			if (c < 0) {
				return c;
			}
		} while ((c & 0x7f) != ZPAD);

		c = rx_raw(zmodem,timeout_ms);
		if (c < 0) {
			return c;
		}

		if ((c & 0x7f) == ZPAD) {
			c = rx_raw(zmodem,timeout_ms);
			if (c < 0) {
				return c;
			}
		}

		/*
		 * spurious ZPAD check
		 */

		if ((c & 0x7f) != ZDLE) {
			continue;
		}

		/*
		 * now read the header style
		 */

		c = rx(zmodem,timeout_ms);

		if (c < 0) {
			return c;
		}

		switch (c & 0x7f) {
			case ZBIN:
				result = rx_bin16_header(zmodem,timeout_ms);
				zmodem->receive_32_bit_data = false;
				break;
			case ZHEX:
				result = rx_hex_header(zmodem,timeout_ms);
				zmodem->receive_32_bit_data = false;
				break;
			case ZBIN32:
				result = rx_bin32_header(zmodem,timeout_ms);
				zmodem->receive_32_bit_data = true;
				break;
			default:
				if (report_errors) {
					return INVHDR;
				}
				continue;
		}
		if (result == ZMODEM_IO_ERROR || result == ZMODEM_CANCELLED ||
		    result == ZMODEM_TIMEOUT) {
			return result;
		}
		if (report_errors && zmodem->rxd_header_len == 0U) {
			return ZMODEM_INVALID_HEADER;
		}

	} while (zmodem->rxd_header_len == 0U);

	return zmodem->rxd_header[0];
}

int
rx_header(struct zmodem * zmodem,int timeout_ms)

{
	return rx_header_raw(zmodem,timeout_ms,false);
}

int
rx_header_and_check(struct zmodem * zmodem,int timeout_ms)

{
	int type;
	for (;;) {
		type = rx_header_raw(zmodem,timeout_ms,true);

		if (type != INVHDR) {
			break;
		}

		if (tx_znak(zmodem) != 0) {
			return ZMODEM_IO_ERROR;
		}
	}

	return type;
}

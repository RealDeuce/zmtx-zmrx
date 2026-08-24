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

#include <stdlib.h>
#include <stdio.h>
#include <termios.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/time.h>

#include "zmodem.h"
#define ZMDM
#include "zmdm.h"
#include "crctab.h"

#if 0
#define DEBUG
#endif

bool receive_32_bit_data;
bool raw_trace;
bool want_fcs_32 = true;
static uint8_t inputbuffer[1024];
static size_t n_in_inputbuffer;
static size_t inputbuffer_index;
static bool termios_saved;

enum tx_class {
	TX_NORMAL = 0,
	TX_ESCAPE_ALWAYS = 1,
	TX_ESCAPE_CONTROL = 2,
	TX_ESCAPE_CR = 4
};

static uint8_t tx_classes[256];
static bool tx_classes_initialized;

#define TX_DATA_WIRE_CAPACITY (2 * ZMAXSPLEN + 11)
static uint8_t tx_data_wire[TX_DATA_WIRE_CAPACITY];

/*
 * routines to make the io channel raw and restore it
 * to its normal state.
 */

struct termios old_termios;

void
fd_init(void)

{
	struct termios t;

	if (tcgetattr(0,&t) != 0) {
		return;
	}
	old_termios = t;
	termios_saved = true;

	t.c_iflag = 0;

	t.c_oflag = 0;

	t.c_lflag = 0;

	t.c_cflag &= (tcflag_t)~(CSIZE | PARENB);
	t.c_cflag |= CS8;
	t.c_cc[VMIN] = 1;
	t.c_cc[VTIME] = 0;

	tcsetattr(0,TCSANOW,&t);
}

void
fd_exit(void)

{
	if (termios_saved) {
		(void)tcsetattr(0,TCSANOW,&old_termios);
		termios_saved = false;
	}
}

/*
 * read bytes as long as rdchk indicates that
 * more data is available.
 */

void
rx_purge(void)

{
	int ready;
	ssize_t nread;
	struct timeval t;
	fd_set f;
	uint8_t c;

	n_in_inputbuffer = 0;
	inputbuffer_index = 0;

	for (;;) {
		t.tv_sec = 0;
		t.tv_usec = 0;
		FD_ZERO(&f);
		FD_SET(0,&f);

		ready = select(1,&f,NULL,NULL,&t);
		if (ready <= 0) {
			break;
		}
		nread = read(0,&c,1);
		if (nread <= 0) {
			break;
		}
	}
}

int last_sent = -1;

/* 
 * transmit a character. 
 * this is the raw modem interface
 */

int
tx_raw(int c)

{
#ifdef DEBUG
	if (raw_trace) {
		fprintf(stderr,"%02x ",c);
	}
#endif

	if (putchar(c) == EOF) {
		return -1;
	}
	last_sent = c & 0x7f;
	return 0;
}

static int
tx_esc(int c)

{
	if (tx_raw(ZDLE) != 0) {
		return -1;
	}
	return tx_raw(c ^ 0x40);
}

static void
initialize_tx_classes(void)

{
	unsigned c;

	for (c=0;c<256;c++) {
		if (c == ZDLE || c == 0x10 || c == 0x90 || c == XON ||
		    c == 0x91 || c == XOFF || c == 0x93) {
			tx_classes[c] = TX_ESCAPE_ALWAYS;
		}
		else if (c == CR || c == 0x8d) {
			tx_classes[c] = TX_ESCAPE_CR;
		}
		else if ((c & 0x60) == 0) {
			tx_classes[c] = TX_ESCAPE_CONTROL;
		}
		else {
			tx_classes[c] = TX_NORMAL;
		}
	}
	tx_classes_initialized = true;
}

static unsigned
active_tx_classes(void)

{
	if (!tx_classes_initialized) {
		initialize_tx_classes();
	}
	return TX_ESCAPE_ALWAYS |
	    (escape_all_control_characters ? TX_ESCAPE_CONTROL | TX_ESCAPE_CR : 0);
}

static bool
tx_byte_needs_escape(uint8_t c,int previous,unsigned active)

{
	unsigned action = tx_classes[c] & active;

	if (action == TX_ESCAPE_CR) {
		return previous == '@';
	}
	return action != TX_NORMAL;
}

static int
tx(uint8_t c)

{
	if (tx_byte_needs_escape(c,last_sent,active_tx_classes())) {
		return tx_esc(c);
	}
	return tx_raw((int)c);
}

int
tx_flush(void)

{
	return fflush(stdout) == 0 ? 0 : -1;
}

static int
tx_nibble(int n)

{
	n &= 0x0f;
	if (n < 10) {
		n += '0';
	}
	else {
		n += 'a' - 10;
	}
	return tx_raw(n);
}

static int
tx_hex(int h)

{
	if (tx_nibble(h >> 4) != 0) {
		return -1;
	}
	return tx_nibble(h);
}

int
tx_hex_header(const uint8_t * p)

{
	size_t i;
	uint8_t type = p[FTYPE];
	uint16_t crc = 0;

	if (tx_raw(ZPAD) != 0 || tx_raw(ZPAD) != 0 || tx_raw(ZDLE) != 0) {
		return -1;
	}
	if (use_variable_headers) {
		if (tx_raw(ZVHEX) != 0 || tx_hex(HDRLEN) != 0) {
			return -1;
		}
	}
	else if (tx_raw(ZHEX) != 0) {
		return -1;
	}

	for (i=0;i<HDRLEN;i++) {
		if (tx_hex(*p) != 0) {
			return -1;
		}
		crc = UPDCRC16(*p++,crc);
	}
	crc = UPDCRC16(0,crc);
	crc = UPDCRC16(0,crc);
	if (tx_hex(crc >> 8) != 0 || tx_hex(crc) != 0 ||
	    tx_raw(CR) != 0 || tx_raw(LF) != 0) {
		return -1;
	}
	if (type != ZACK && type != ZFIN && tx_raw(XON) != 0) {
		return -1;
	}
	return tx_flush();
}

static int
tx_bin32_header(const uint8_t * p)

{
	size_t i;
	uint32_t crc = UINT32_MAX;

	if (tx_raw(ZPAD) != 0 || tx_raw(ZPAD) != 0 || tx_raw(ZDLE) != 0) {
		return -1;
	}
	if (use_variable_headers) {
		if (tx_raw(ZVBIN32) != 0 || tx(HDRLEN) != 0) {
			return -1;
		}
	}
	else if (tx_raw(ZBIN32) != 0) {
		return -1;
	}
	for (i=0;i<HDRLEN;i++) {
		crc = UPDCRC32(*p,crc);
		if (tx(*p++) != 0) {
			return -1;
		}
	}
	crc = ~crc;
	if (tx((uint8_t)crc) != 0 || tx((uint8_t)(crc >> 8)) != 0 ||
	    tx((uint8_t)(crc >> 16)) != 0 || tx((uint8_t)(crc >> 24)) != 0) {
		return -1;
	}
	return 0;
}

static int
tx_bin16_header(const uint8_t * p)

{
	size_t i;
	uint16_t crc = 0;

	if (tx_raw(ZPAD) != 0 || tx_raw(ZPAD) != 0 || tx_raw(ZDLE) != 0) {
		return -1;
	}
	if (use_variable_headers) {
		if (tx_raw(ZVBIN) != 0 || tx(HDRLEN) != 0) {
			return -1;
		}
	}
	else if (tx_raw(ZBIN) != 0) {
		return -1;
	}
	for (i=0;i<HDRLEN;i++) {
		crc = UPDCRC16(*p,crc);
		if (tx(*p++) != 0) {
			return -1;
		}
	}
	crc = UPDCRC16(0,crc);
	crc = UPDCRC16(0,crc);
	if (tx((uint8_t)(crc >> 8)) != 0 || tx((uint8_t)crc) != 0) {
		return -1;
	}
	return 0;
}

int
tx_header(const uint8_t * p)

{
	if (can_fcs_32 && want_fcs_32) {
		return tx_bin32_header(p);
	}
	if (can_fcs_32) {
		return tx_bin16_header(p);
	}
	return tx_hex_header(p);
}

static void
buffer_raw(uint8_t c,size_t * used,int * previous)

{
	tx_data_wire[(*used)++] = c;
	*previous = c & 0x7f;
}

static void
buffer_tx(uint8_t c,size_t * used,int * previous,unsigned active)

{
	if (tx_byte_needs_escape(c,*previous,active)) {
		buffer_raw(ZDLE,used,previous);
		buffer_raw((uint8_t)(c ^ 0x40),used,previous);
	}
	else {
		buffer_raw(c,used,previous);
	}
}

int
tx_data(uint8_t sub_frame_type,const uint8_t * p,size_t l)

{
	int previous = last_sent;
	size_t i;
	size_t used = 0;
	unsigned active = active_tx_classes();

	if (l > ZMAXSPLEN) {
		return -1;
	}
	for (i=0;i<l;i++) {
		buffer_tx(p[i],&used,&previous,active);
	}
	buffer_raw(ZDLE,&used,&previous);
	buffer_raw(sub_frame_type,&used,&previous);

	if (want_fcs_32 && can_fcs_32) {
		uint32_t crc = crc32_update(UINT32_MAX,p,l);

		crc = ~UPDCRC32(sub_frame_type,crc);
		buffer_tx((uint8_t)crc,&used,&previous,active);
		buffer_tx((uint8_t)(crc >> 8),&used,&previous,active);
		buffer_tx((uint8_t)(crc >> 16),&used,&previous,active);
		buffer_tx((uint8_t)(crc >> 24),&used,&previous,active);
	}
	else {
		uint16_t crc = 0;

		for (i=0;i<l;i++) {
			crc = UPDCRC16(p[i],crc);
		}
		crc = UPDCRC16(sub_frame_type,crc);
		crc = UPDCRC16(0,crc);
		crc = UPDCRC16(0,crc);
		buffer_tx((uint8_t)(crc >> 8),&used,&previous,active);
		buffer_tx((uint8_t)crc,&used,&previous,active);
	}

	if (sub_frame_type == ZCRCW) {
		buffer_raw(XON,&used,&previous);
	}
#ifdef DEBUG
	if (raw_trace) {
		for (i=0;i<used;i++) {
			fprintf(stderr,"%02x ",tx_data_wire[i]);
		}
	}
#endif
	if (fwrite(tx_data_wire,1,used,stdout) != used) {
		return -1;
	}
	last_sent = previous;
	return tx_flush();
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
tx_pos_header(uint8_t type,uint32_t position)

{
	uint8_t header[HDRLEN] = { 0 };

	header[FTYPE] = type;
	zmodem_set_header_position(header,position);

	return tx_hex_header(header);
}

int
tx_znak(void)

{
	fprintf(stderr,"tx_znak\n");

	return tx_pos_header(ZNAK,UINT32_C(0));
}

/*
 * receive any style header within timeout milliseconds
 */

int
rx_poll(void)

{
	struct timeval t;
	fd_set f;

	if (n_in_inputbuffer > 0) {
		return 1;
	}

	t.tv_sec = 0;
	t.tv_usec = 0;

	FD_ZERO(&f);
	FD_SET(0,&f);

	return select(1,&f,NULL,NULL,&t) > 0;
}

/*
 * rx_raw ; receive a single byte from the line.
 * reads as many are available and then processes them one at a time
 * check the data stream for 5 consecutive CAN characters;
 * and if you see them abort. this saves a lot of clutter in
 * the rest of the code; even though it is a very strange place
 * for an exit. (but that was wat session abort was all about.)
 */

int
rx_raw(int to)

{
	uint8_t c;
	static int n_cans = 0;
	ssize_t nread;

	if (n_in_inputbuffer == 0) {
		fd_set f;
		struct timeval timeout;
		int ready;

		if (to < 0) {
			to = 0;
		}
		do {
			timeout.tv_sec = to / 1000;
			timeout.tv_usec = (to % 1000) * 1000;
			FD_ZERO(&f);
			FD_SET(0,&f);
			ready = select(1,&f,NULL,NULL,&timeout);
		} while (ready < 0 && errno == EINTR);
		if (ready == 0) {
			return TIMEOUT;
		}
		if (ready < 0) {
			fprintf(stderr,"zmdm : fatal error waiting for device input\n");
			cleanup();
			exit(1);
		}

		do {
			nread = read(0,inputbuffer,sizeof(inputbuffer));
		} while (nread < 0 && errno == EINTR);

		if (nread < 0) {
			fprintf(stderr,"zmdm : fatal error reading device\n");
			cleanup();
			exit(1);
		}

		if (nread <= 0) {
			return TIMEOUT;
		}

		n_in_inputbuffer = (size_t)nread;
		inputbuffer_index = 0;
	}

	c = inputbuffer[inputbuffer_index++];
	n_in_inputbuffer--;

	if (c == CAN) {
		n_cans++;
		if (n_cans == 5) {
			/*
			 * the other side is serious about this. just shut up;
			 * clean up and exit.
			 */
			cleanup();

			exit(CAN);
		}
	}
	else {
		n_cans = 0;
	}

	return c;
}

/*
 * rx; receive a single byte undoing any escaping at the
 * sending site. this bit looks like a mess. sorry for that
 * but there seems to be no other way without incurring a lot
 * of overhead. at least like this the path for a normal character
 * is relatively short.
 */

static
int
rx(int to)

{
	int c;

	/*
	 * outer loop for ever so for sure something valid
	 * will come in; a timeout will occur or a session abort
	 * will be received.
	 */

	while (true) {
		while (true) {
			c = rx_raw(to);
			if (c == TIMEOUT) {
				return c;
			}
	
			switch (c) {
				case ZDLE:
					break;
				case 0x11:
				case 0x91:
				case 0x13:
				case 0x93:
					continue;
				default:
					/*
					 * normal character; return it.
					 */
					return c;
			}
			break;
		}
	
		/*
	 	 * ZDLE encoded sequence or session abort.
		 * (or something illegal; then back to the top)
		 */

		while (true) {
			c = rx_raw(to);
			if (c == TIMEOUT) {
				return c;
			}

			if (c == 0x11 || c == 0x13 || c == 0x91 || c == 0x93 || c == ZDLE) {
				/*
				 * these can be dropped.
				 */
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
	}

	/*
	 * not reached.
	 */

	return 0;
}

static int
rx_crc16(int timeout,uint16_t * value)

{
	int high;
	int low;

	high = rx(timeout);
	if (high == TIMEOUT) {
		return TIMEOUT;
	}
	low = rx(timeout);
	if (low == TIMEOUT) {
		return TIMEOUT;
	}

	*value = (uint16_t)(((uint16_t)(uint8_t)high << 8) | (uint8_t)low);
	return 0;
}

static int
rx_crc32(int timeout,uint32_t * value)

{
	int c;
	size_t i;
	uint32_t result = 0;

	for (i=0;i<sizeof(result);i++) {
		c = rx(timeout);
		if (c == TIMEOUT) {
			return TIMEOUT;
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
rx_32_data(uint8_t * p,size_t capacity,size_t * l)

{
	int c;
	uint32_t rxd_crc;
	uint32_t crc;
	int sub_frame_type;
	bool overflow = false;

#ifdef DEBUG
	fprintf(stderr,"rx_32_data\n");
#endif

	crc = UINT32_MAX;

	do {
		c = rx(1000);

		if (c == TIMEOUT) {
			return TIMEOUT;
		}
		if (c < 0x100) {
			crc = UPDCRC32(c,crc);
			if (*l < capacity && *l < ZMAXSPLEN) {
				p[*l] = (uint8_t)c;
				(*l)++;
			}
			else {
				overflow = true;
			}
			continue;
		}
	} while (c < 0x100);

	sub_frame_type = c & 0xff;

	crc = UPDCRC32(sub_frame_type, crc);

	crc = ~crc;

	if (rx_crc32(1000,&rxd_crc) == TIMEOUT) {
		return TIMEOUT;
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
rx_16_data(uint8_t * p,size_t capacity,size_t * l)

{
	int c;
	int sub_frame_type;
	uint16_t crc;
	uint16_t rxd_crc;
	bool overflow = false;

#ifdef DEBUG
	fprintf(stderr,"rx_16_data\n");
#endif

	crc = 0;

	do {
		c = rx(5000);

		if (c == TIMEOUT) {
			return TIMEOUT;
		}
		if (c < 0x100) {
			crc = UPDCRC16(c,crc);
			if (*l < capacity && *l < ZMAXSPLEN) {
				p[*l] = (uint8_t)c;
				(*l)++;
			}
			else {
				overflow = true;
			}
		}
	} while (c < 0x100);

	sub_frame_type = c & 0xff;

	crc = UPDCRC16(sub_frame_type,crc);

	crc = UPDCRC16(0,crc);
	crc = UPDCRC16(0,crc);

	if (rx_crc16(1000,&rxd_crc) == TIMEOUT) {
		return TIMEOUT;
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
rx_data(uint8_t * p,size_t capacity,size_t * l,uint8_t * frame_end)

{
	int sub_frame_type;

	/*
	 * receive the right type of frame
	 */

	*l = 0;
	*frame_end = 0;

	if (receive_32_bit_data) {
		sub_frame_type = rx_32_data(p,capacity,l);
	}
	else {	
		sub_frame_type = rx_16_data(p,capacity,l);
	}
	if (sub_frame_type < 0) {
		return sub_frame_type;
	}
	*frame_end = (uint8_t)sub_frame_type;

	switch (sub_frame_type)  {
		case TIMEOUT:
			return TIMEOUT;
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
	}

	return INVDATA;
}

static
int
rx_nibble(int to) 

{
	int c;

	c = rx(to);

	if (c == TIMEOUT) {
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

int
rx_hex(int to)

{
	int n1;
	int n0;

	n1 = rx_nibble(to);

	if (n1 == TIMEOUT) {
		return n1;
	}

	n0 = rx_nibble(to);

	if (n0 == TIMEOUT) {
		return n0;
	}

	return (n1 << 4) | n0;
}

/*
 * receive routines for each of the six different styles of header.
 * each of these leaves rxd_header_len set to 0 if the end result is
 * not a valid header.
 */

void
rx_bin16_header(int to)

{
	int c;
	size_t n;
	uint16_t crc;
	uint16_t rxd_crc;

#ifdef DEBUG
	fprintf(stderr,"rx binary header 16 bits crc\n");
#endif

	crc = 0;

	for (n=0;n<5;n++) {
		c = rx(to);
		if (c == TIMEOUT) {
#ifdef DEBUG
			fprintf(stderr,"timeout\n");
#endif
			return;
		}
		crc = UPDCRC16(c,crc);
		rxd_header[n] = (uint8_t)c;
	}

	crc = UPDCRC16(0,crc);
	crc = UPDCRC16(0,crc);

	if (rx_crc16(1000,&rxd_crc) == TIMEOUT) {
		return;
	}

	if (rxd_crc != crc) {
#ifdef DEBUG
		fprintf(stderr,"bad crc %4.4x %4.4x\n",
			(unsigned int)rxd_crc,(unsigned int)crc);
#endif
		return;
	}

	rxd_header_len = 5;
}

void
rx_hex_header(int to)

{
	int c;
	size_t i;
	uint16_t crc = 0;
	uint16_t rxd_crc;

#ifdef DEBUG
	fprintf(stderr,"rx_hex_header : ");
#endif
	for (i=0;i<5;i++) {
		c = rx_hex(to);
		if (c == TIMEOUT) {
			return;
		}
		crc = UPDCRC16(c,crc);

		rxd_header[i] = (uint8_t)c;
	}

	crc = UPDCRC16(0,crc);

	crc = UPDCRC16(0,crc);

	/*
	 * receive the crc
	 */

	c = rx_hex(to);

	if (c == TIMEOUT) {
		return;
	}

	rxd_crc = (uint16_t)((uint16_t)(uint8_t)c << 8);

	c = rx_hex(to);

	if (c == TIMEOUT) {
		return;
	}

	rxd_crc |= (uint8_t)c;

	if (rxd_crc != crc) {
#ifdef DEBUG
		fprintf(stderr,"bad crc.\n");
#endif
		return;
	}

	/*
	 * drop the end of line sequence after a hex header
	 */
	c = rx(to);
	if (c == TIMEOUT) {
		return;
	}
	c &= 0x7f;
	if (c == CR) {
		/*
		 * both are expected with CR
		 */
		c = rx(to);
		if (c == TIMEOUT) {
			return;
		}
		c &= 0x7f;
	}
	if (c != LF) {
		return;
	}
	rxd_header_len = HDRLEN;
}

void
rx_bin32_header(int to)

{
	int c;
	size_t n;
	uint32_t crc;
	uint32_t rxd_crc;

	(void)to;

#ifdef DEBUG
	fprintf(stderr,"rx binary header 32 bits crc\n");
#endif

	crc = UINT32_MAX;

	for (n=0;n<5;n++) {
		c = rx(1000);
		if (c == TIMEOUT) {
			return;
		}
		crc = UPDCRC32(c,crc);
		rxd_header[n] = (uint8_t)c;
	}

	crc = ~crc;

	if (rx_crc32(1000,&rxd_crc) == TIMEOUT) {
		return;
	}

	if (rxd_crc != crc) {
		return;
	}

	rxd_header_len = 5;
}

/*
 * receive any style header
 * if the errors flag is set than whenever an invalid header packet is
 * received INVHDR will be returned. otherwise we wait for a good header
 * also; a flag (receive_32_bit_data) will be set to indicate whether data
 * packets following this header will have 16 or 32 bit data attached.
 * variable headers are not implemented.
 */

int
rx_header_raw(int to,bool errors)

{
	int c;

#ifdef DEBUG
	fprintf(stderr,"rx header : ");
#endif
	rxd_header_len = 0;

	do {
		do {
			c = rx_raw(to);
			if (c == TIMEOUT) {
				return c;
			}
		} while ((c & 0x7f) != ZPAD);

		c = rx_raw(to);
		if (c == TIMEOUT) {
			return c;
		}

		if ((c & 0x7f) == ZPAD) {
			c = rx_raw(to);
			if (c == TIMEOUT) {
				return c;
			}
		}

		/*
		 * spurious ZPAD check
		 */

		if ((c & 0x7f) != ZDLE) {
#ifdef DEBUG
			fprintf(stderr,"expected ZDLE; got %c\n",c);
#endif
			continue;
		}

		/*
		 * now read the header style
		 */

		c = rx(to);

		if (c == TIMEOUT) {
			return c;
		}

#ifdef DEBUG
		fprintf(stderr,"\n");
#endif
		switch (c & 0x7f) {
			case ZBIN:
				rx_bin16_header(to);
				receive_32_bit_data = false;
				break;
			case ZHEX:
				rx_hex_header(to);
				receive_32_bit_data = false;
				break;
			case ZBIN32:
				rx_bin32_header(to);
				receive_32_bit_data = true;
				break;
			default:
				/*
				 * unrecognized header style
				 */
#ifdef DEBUG
				fprintf(stderr,"unrecognized header style %c\n",c);
#endif
				if (errors) {
					return INVHDR;
				}

				continue;
				break;
		}
		if (errors && rxd_header_len == 0) {
			return INVHDR;
		}

	} while (rxd_header_len == 0);

	/*
 	 * this appears to have been a valid header.
	 * return its type.
	 */

#ifdef DEBUG
	fprintf(stderr,"type %d\n",rxd_header[0]);
#endif

	return rxd_header[0];
}

int
rx_header(int timeout)

{
	return rx_header_raw(timeout,false);
}

int
rx_header_and_check(int timeout)

{
	int type;
	while (true) {
		type = rx_header_raw(timeout,true);

		if (type != INVHDR) {
			break;
		}

		tx_znak();
	}

	return type;
}

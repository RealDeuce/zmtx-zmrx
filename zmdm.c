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
/* AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE   */
/* IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE */
/* ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE  */
/* LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR         */
/* CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF       */
/* SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS   */
/* INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN     */
/* CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)     */
/* ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE  */
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
#include <signal.h>
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
uint32_t ack_file_pos;			/* file position used in acknowledgement of correctly */
								/* received data subpackets */

/*
 * routines to make the io channel raw and restore it
 * to its normal state.
 */

struct termios old_termios;

void
fd_init(void)

{
	struct termios t;

	tcgetattr(0,&old_termios);

	tcgetattr(0,&t);

	t.c_iflag = 0;

	t.c_oflag = 0;

	t.c_lflag = 0;

	t.c_cflag |= CS8;

	tcsetattr(0,TCSANOW,&t);
}

void
fd_exit(void)

{
	tcsetattr(0,TCSANOW,&old_termios);
}

/*
 * read bytes as long as rdchk indicates that
 * more data is available.
 */

void
rx_purge(void)

{
	struct timeval t;
	fd_set f;
	uint8_t c;

	t.tv_sec = 0;
	t.tv_usec = 0;

	FD_ZERO(&f);
	FD_SET(0,&f);

	while (select(1,&f,NULL,NULL,&t)) {
		read(0,&c,1);		
	}

}

int last_sent = -1;

/* 
 * transmit a character. 
 * this is the raw modem interface
 */

void
tx_raw(int c)

{
#ifdef DEBUG
	if (raw_trace) {
		fprintf(stderr,"%02x ",c);
	}
#endif

	last_sent = c & 0x7f;

	putchar(c);
}

/*
 * transmit a character ZDLE escaped
 */

void
tx_esc(int c)

{
	tx_raw(ZDLE);
	/*
	 * exclusive or; not an or so ZDLE becomes ZDLEE
	 */
	tx_raw(c ^ 0x40);
}

/*
 * transmit a character; ZDLE escaping if appropriate
 */

void
tx(uint8_t c)

{
	switch (c) {
		case ZDLE:
			tx_esc(c);
			return;
			break;
		case 0x8d:
		case 0x0d:
			if (escape_all_control_characters && last_sent == '@') {
				tx_esc(c);
				return;
			}
			break;
		case 0x10:
		case 0x90:
		case 0x11:
		case 0x91:
		case 0x13:
		case 0x93:
			tx_esc(c);
			return;
			break;
		default:
			if (escape_all_control_characters && (c & 0x60) == 0) {
				tx_esc(c);
				return;
			}
			break;
	}
	/*
	 * anything that ends here is so normal we might as well transmit it.
	 */
	tx_raw((int) c);
}

/*
 * send the bytes accumulated in the output buffer.
 */

void
tx_flush(void)

{
	fflush(stdout);
}

/* 
 * transmit a hex header.
 * these routines use tx_raw because we're sure that all the
 * characters are not to be escaped.
 */

void
tx_nibble(int n)

{
	n &= 0x0f;
	if (n < 10) {
		n += '0';
	}
	else {
		n += 'a' - 10;
	}

	tx_raw(n);
}

void
tx_hex(int h)

{
	tx_nibble(h >> 4);
	tx_nibble(h);
}

void
tx_hex_header(const uint8_t * p)

{
	size_t i;
	uint16_t crc;

#ifdef DEBUG
	fprintf(stderr,"tx_hheader : ");
#endif

	tx_raw(ZPAD);
	tx_raw(ZPAD);
	tx_raw(ZDLE);

	if (use_variable_headers) {
		tx_raw(ZVHEX);
		tx_hex(HDRLEN);
	}
	else {
		tx_raw(ZHEX);
	}

	/*
 	 * initialise the crc
	 */

	crc = 0;

	/*
 	 * transmit the header
	 */

	for (i=0;i<HDRLEN;i++) {
		tx_hex(*p);
		crc = UPDCRC16(*p, crc);
		p++;
	}

	/*
 	 * update the crc as though it were zero
	 */

	crc = UPDCRC16(0,crc);
	crc = UPDCRC16(0,crc);

	/* 
	 * transmit the crc
	 */

	tx_hex(crc >> 8);
	tx_hex(crc);

	/*
	 * end of line sequence
	 */

	tx_raw(0x0d);
	tx_raw(0x0a);

	tx_raw(XON);

	tx_flush();

#ifdef DEBUG
	fprintf(stderr,"\n");
#endif
}

/*
 * Send ZMODEM binary header hdr
 */

void
tx_bin32_header(const uint8_t * p)

{
	size_t i;
	uint32_t crc;

#ifdef DEBUG
	fprintf(stderr,"tx binary header 32 bits crc\n");
	raw_trace = true;
#endif

	tx_raw(ZPAD);
	tx_raw(ZPAD);
	tx_raw(ZDLE);

	if (use_variable_headers) {
		tx_raw(ZVBIN32);
		tx(HDRLEN);
	}
	else {
		tx_raw(ZBIN32);
	}

	crc = UINT32_MAX;

	for (i=0;i<HDRLEN;i++) {
		crc = UPDCRC32(*p,crc);
		tx(*p++);
	}

	crc = ~crc;

	tx((uint8_t)crc);
	tx((uint8_t)(crc >> 8));
	tx((uint8_t)(crc >> 16));
	tx((uint8_t)(crc >> 24));
}

void
tx_bin16_header(const uint8_t * p)

{
	size_t i;
	uint16_t crc;

#ifdef DEBUG
	fprintf(stderr,"tx binary header 16 bits crc\n");
#endif

	tx_raw(ZPAD);
	tx_raw(ZPAD);
	tx_raw(ZDLE);

	if (use_variable_headers) {
		tx_raw(ZVBIN);
		tx(HDRLEN);
	}
	else {
		tx_raw(ZBIN);
	}

	crc = 0;

	for (i=0;i<HDRLEN;i++) {
		crc = UPDCRC16(*p,crc);
		tx(*p++);
	}

	crc = UPDCRC16(0,crc);
	crc = UPDCRC16(0,crc);

	tx((uint8_t)(crc >> 8));
	tx((uint8_t)crc);
}


/* 
 * transmit a header using either hex 16 bit crc or binary 32 bit crc
 * depending on the receivers capabilities
 * we dont bother with variable length headers. I dont really see their
 * advantage and they would clutter the code unneccesarily
 */

void
tx_header(const uint8_t * p)

{
	if (can_fcs_32) {
		if (want_fcs_32) {
			tx_bin32_header(p);
		}
		else {
			tx_bin16_header(p);
		}
	}
	else {
		tx_hex_header(p);
	}
}

/*
 * data subpacket transmission
 */

void
tx_32_data(uint8_t sub_frame_type,const uint8_t * p,size_t l)

{
	uint32_t crc;

#ifdef DEBUG
	fprintf(stderr,"tx_32_data\n");
#endif

	crc = UINT32_MAX;

	while (l > 0) {
		crc = UPDCRC32(*p,crc);
		tx(*p++);
		l--;
	}

	crc = UPDCRC32(sub_frame_type, crc);

	tx_raw(ZDLE);
	tx_raw(sub_frame_type);

	crc = ~crc;

	tx((uint8_t)crc);
	tx((uint8_t)(crc >> 8));
	tx((uint8_t)(crc >> 16));
	tx((uint8_t)(crc >> 24));
}

void
tx_16_data(uint8_t sub_frame_type,const uint8_t * p,size_t l)

{
	uint16_t crc;

#ifdef DEBUG
	fprintf(stderr,"tx_16_data\n");
#endif

	crc = 0;

	while (l > 0) {
		crc = UPDCRC16(*p,crc);
		tx(*p++);
		l--;
	}

	crc = UPDCRC16(sub_frame_type,crc);

	tx_raw(ZDLE); 
	tx_raw(sub_frame_type);
	
	crc = UPDCRC16(0,crc);
	crc = UPDCRC16(0,crc);

	tx((uint8_t)(crc >> 8));
	tx((uint8_t)crc);
}

/*
 * send a data subpacket using crc 16 or crc 32 as desired by the receiver
 */

void
tx_data(uint8_t sub_frame_type,const uint8_t * p,size_t l)

{
	if (want_fcs_32 && can_fcs_32) {
		tx_32_data(sub_frame_type,p,l);
	}
	else {	
		tx_16_data(sub_frame_type,p,l);
	}

	if (sub_frame_type == ZCRCW) {
		tx_raw(XON);
	}

	tx_flush();
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

void
tx_pos_header(uint8_t type,uint32_t position)

{
	uint8_t header[HDRLEN] = { 0 };

	header[FTYPE] = type;
	zmodem_set_header_position(header,position);

	tx_hex_header(header);
}

void
tx_znak(void)

{
	fprintf(stderr,"tx_znak\n");

	tx_pos_header(ZNAK,ack_file_pos);
}

void
tx_zskip(void)

{
	tx_pos_header(ZSKIP,UINT32_C(0));
}

/*
 * receive any style header within timeout milliseconds
 */

void
alrm(int a)

{
	(void)a;
	signal(SIGALRM,SIG_IGN);
}

int
rx_poll(void)

{
	struct timeval t;
	fd_set f;

	t.tv_sec = 0;
	t.tv_usec = 0;

	FD_ZERO(&f);
	FD_SET(0,&f);

	if (select(1,&f,NULL,NULL,&t)) {
		return 1;
	}

	return 0;
}

uint8_t inputbuffer[1024];
size_t n_in_inputbuffer = 0;
size_t inputbuffer_index;

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
		/*
		 * change the timeout into seconds; minimum is 1
		 */

		to /= 1000;
		if (to == 0) {
			to++;
		}

		/*
	 	 * setup an alarm in case io takes too long
		 */

		signal(SIGALRM,alrm);

		to /= 1000;

		if (to == 0) {
			to = 2;
		}

		alarm((unsigned int)to);

		nread = read(0,inputbuffer,sizeof(inputbuffer));

		/*
	 	 * cancel the alarm in case it did not go off yet
		 */

		signal(SIGALRM,SIG_IGN);

		if (nread < 0 && errno != EINTR) {
			fprintf(stderr,"zmdm : fatal error reading device\n");
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

		/*
	 	 * fake do loop so we may continue
		 * in case a character should be dropped.
		 */

		do {
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
					break;
				default:
					/*
	 				 * if all control characters should be escaped and 
					 * this one wasnt then its spurious and should be dropped.
					 */
					if (escape_all_control_characters && (c & 0x60) == 0) {
						continue;
					}
					/*
					 * normal character; return it.
					 */
					return c;
			}
		} while (false);
	
		/*
	 	 * ZDLE encoded sequence or session abort.
		 * (or something illegal; then back to the top)
		 */

		do {
			c = rx_raw(to);

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
					return (c | ZDLEESC);
					break;
				case ZRUB0:
					return 0x7f;
					break;
				case ZRUB1:
					return 0xff;
					break;
				default:
					if (escape_all_control_characters && (c & 0x60) == 0) {
						/*
						 * a not escaped control character; probably
						 * something from a network. just drop it.
						 */
						continue;
					}
					/*
					 * legitimate escape sequence.
					 * rebuild the orignal and return it.
					 */
					if ((c & 0x60) == 0x40) {
						return c ^ 0x40;
					}
					break;
			}
		} while (false);
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
 * receive a data subpacket as dictated by the last received header.
 * return 2 with correct packet and end of frame
 * return 1 with correct packet frame continues
 * return 0 with incorrect frame.
 * return TIMEOUT with a timeout
 * if an acknowledgement is requested it is generated automatically
 * here. 
 */

/*
 * data subpacket reception
 */

int
rx_32_data(uint8_t * p,size_t * l)

{
	int c;
	uint32_t rxd_crc;
	uint32_t crc;
	int sub_frame_type;

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
			*p++ = (uint8_t)c;
			(*l)++;
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
		return 0;
	}

	if (*l > (size_t)(UINT32_MAX - ack_file_pos)) {
		return 0;
	}
	ack_file_pos += (uint32_t)*l;

	return sub_frame_type;
}

int
rx_16_data(uint8_t * p,size_t * l)

{
	int c;
	int sub_frame_type;
	uint16_t crc;
	uint16_t rxd_crc;

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
			*p++ = (uint8_t)c;
			(*l)++;
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
		return 0;
	}

	if (*l > (size_t)(UINT32_MAX - ack_file_pos)) {
		return 0;
	}
	ack_file_pos += (uint32_t)*l;

	return sub_frame_type;
}

int
rx_data(uint8_t * p,size_t * l)

{
	int sub_frame_type;
	uint32_t pos;

	/*
	 * fill in the file pointer in case acknowledgement is requested.	
	 * the ack file pointer will be updated in the subpacket read routine;
	 * so we need to get it now
	 */

	pos = ack_file_pos;

	/*
	 * receive the right type of frame
	 */

	*l = 0;

	if (receive_32_bit_data) {
		sub_frame_type = rx_32_data(p,l);
	}
	else {	
		sub_frame_type = rx_16_data(p,l);
	}

	switch (sub_frame_type)  {
		case TIMEOUT:
			return TIMEOUT;
			break;
		/*
		 * frame continues non-stop
		 */
		case ZCRCG:
			return FRAMEOK;
			break;
		/*
		 * frame ends
		 */
		case ZCRCE:
			return ENDOFFRAME;
			break;
		/*
 		 * frame continues; ZACK expected
		 */
		case ZCRCQ:		
			tx_pos_header(ZACK,pos);
			return FRAMEOK;
			break;
		/*
		 * frame ends; ZACK expected
		 */
		case ZCRCW:
			tx_pos_header(ZACK,pos);
			return ENDOFFRAME;
			break;
	}

	return 0;
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

	if (rxd_crc == crc) {
		rxd_header_len = 5;
	}
#ifdef DEBUG
	else {
		fprintf(stderr,"bad crc.\n");
	}
#endif

	/*
	 * drop the end of line sequence after a hex header
	 */
	c = rx(to);
	if (c == CR) {
		/*
		 * both are expected with CR
		 */
		c = rx(to);
	}
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
		} while (c != ZPAD);

		c = rx_raw(to);
		if (c == TIMEOUT) {
			return c;
		}

		if (c == ZPAD) {
			c = rx_raw(to);
			if (c == TIMEOUT) {
				return c;
			}
		}

		/*
		 * spurious ZPAD check
		 */

		if (c != ZDLE) {
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
		switch (c) {
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

	if (rxd_header[0] == ZDATA) {
		ack_file_pos = zmodem_header_position(rxd_header);
	}

	if (rxd_header[0] == ZFILE) {
		ack_file_pos = UINT32_C(0);
	}

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

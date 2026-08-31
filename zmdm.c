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

#include "plat.h"

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
	TX_ESCAPE_8TH = 8,
	TX_ESCAPE_IAC = 16
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

/*
 * DSZ.EXE's ZMODEM-90 seven-bit encoder.  Most high-bit bytes use SO
 * followed by their low seven bits.  Values which would collide with the
 * framing grammar use the private ZDLE codes below instead.  Applying
 * ESCCTL after the SO prefix can expand a byte to three wire bytes.
 */
static size_t
omen_encode_byte(const struct zmodem * zmodem,uint8_t c,uint8_t output[3])

{
	uint8_t low = c & UINT8_C(0x7f);
	uint8_t code = 0U;

	switch (c) {
		case SO: code = UINT8_C(0x6e); break;
		case UINT8_C(0x10): code = UINT8_C(0x50); break;
		case XON: code = UINT8_C(0x51); break;
		case XOFF: code = UINT8_C(0x53); break;
		case ZDLE: code = ZDLEE; break;
		case UINT8_C(0x80): code = UINT8_C(0x73); break;
		case UINT8_C(0x8e): code = UINT8_C(0x6f); break;
		case UINT8_C(0x90): code = UINT8_C(0x70); break;
		case UINT8_C(0x91): code = UINT8_C(0x71); break;
		case UINT8_C(0x93): code = UINT8_C(0x72); break;
		case UINT8_C(0x98): code = UINT8_C(0x74); break;
		case UINT8_C(0xff): code = ZRUB1; break;
		case UINT8_C(0x7f):
			if (zmodem->escape_all_control_characters) {
				code = ZRUB0;
			}
			break;
		default:
			break;
	}
	if (code != 0U) {
		output[0] = ZDLE;
		output[1] = code;
		return 2U;
	}
	if ((c & UINT8_C(0x80)) != 0U) {
		output[0] = SO;
		if (zmodem->escape_all_control_characters && low < UINT8_C(0x20)) {
			output[1] = ZDLE;
			output[2] = low ^ UINT8_C(0x40);
			return 3U;
		}
		output[1] = low;
		return 2U;
	}
	if (zmodem->escape_all_control_characters && low < UINT8_C(0x20)) {
		output[0] = ZDLE;
		output[1] = low ^ UINT8_C(0x40);
		return 2U;
	}
	output[0] = low;
	return 1U;
}

static int
tx_omen_byte(struct zmodem * zmodem,uint8_t c)

{
	uint8_t encoded[3];
	size_t count = omen_encode_byte(zmodem,c,encoded);
	size_t i;

	for (i=0U;i<count;i++) {
		if (tx_raw(zmodem,encoded[i]) != 0) {
			return -1;
		}
	}
	return 0;
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
			action = TX_ESCAPE_CONTROL | TX_ESCAPE_CR;
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
		if (c == 0xff) {
			action |= TX_ESCAPE_IAC;
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
	    TX_ESCAPE_CONTROL : 0U) |
	    (zmodem->escape_8th_bit ? TX_ESCAPE_8TH : 0U) |
	    (zmodem->escape_iac ? TX_ESCAPE_IAC : 0U);
}

static bool
tx_byte_needs_escape(const struct zmodem * zmodem,uint8_t c,int previous,
    unsigned active)

{
	unsigned classes = zmodem->tx_classes[c];
	unsigned action = classes & active;

	if (((classes & TX_ESCAPE_CR) != 0U) && previous == '@') {
		return true;
	}
	return action != TX_NORMAL;
}

static int
tx(struct zmodem * zmodem,uint8_t c)

{
	if (tx_byte_needs_escape(zmodem,c,zmodem->last_sent,
	    active_tx_classes(zmodem))) {
		if (zmodem->escape_iac && c == UINT8_C(0xff)) {
			if (tx_raw(zmodem,ZDLE) != 0) {
				return -1;
			}
			return tx_raw(zmodem,ZRUB1);
		}
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

static int
tx_hex_header_length(struct zmodem * restrict zmodem,
    const uint8_t * restrict p,size_t count)

{
	size_t i;
	uint8_t type = p[FTYPE];
	uint16_t crc = 0;

	if (tx_raw(zmodem,ZPAD) != 0 || tx_raw(zmodem,ZPAD) != 0 ||
	    tx_raw(zmodem,ZDLE) != 0) {
		return -1;
	}
	if (count != HDRLEN) {
		if (tx_raw(zmodem,ZVHEX) != 0 ||
		    tx_hex(zmodem,(int)(count - 1U)) != 0) {
			return -1;
		}
	}
	else if (tx_raw(zmodem,ZHEX) != 0) {
		return -1;
	}

	for (i=0;i<count;i++) {
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

int
tx_hex_header(struct zmodem * restrict zmodem,
    const uint8_t * restrict p)

{
	return tx_hex_header_length(zmodem,p,HDRLEN);
}

static int
tx_bin32_header(struct zmodem * restrict zmodem,
    const uint8_t * restrict p,size_t count)

{
	size_t i;
	uint32_t crc = UINT32_MAX;

	if (tx_raw(zmodem,ZPAD) != 0 || tx_raw(zmodem,ZPAD) != 0 ||
	    tx_raw(zmodem,ZDLE) != 0) {
		return -1;
	}
	if (count != HDRLEN) {
		if (tx_raw(zmodem,ZVBIN32) != 0 ||
		    tx(zmodem,(uint8_t)(count - 1U)) != 0) {
			return -1;
		}
	}
	else if (tx_raw(zmodem,ZBIN32) != 0) {
		return -1;
	}
	for (i=0;i<count;i++) {
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
    const uint8_t * restrict p,size_t count)

{
	size_t i;
	uint16_t crc = 0;

	if (tx_raw(zmodem,ZPAD) != 0 || tx_raw(zmodem,ZPAD) != 0 ||
	    tx_raw(zmodem,ZDLE) != 0) {
		return -1;
	}
	if (count != HDRLEN) {
		if (tx_raw(zmodem,ZVBIN) != 0 ||
		    tx(zmodem,(uint8_t)(count - 1U)) != 0) {
			return -1;
		}
	}
	else if (tx_raw(zmodem,ZBIN) != 0) {
		return -1;
	}
	for (i=0;i<count;i++) {
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

/* This byte string is part of the CRC input in Omen's 0x31/0x32/0x33 headers. */
static const uint8_t omen_header_crc_suffix[] =
    "Copyright 1989 Omen Technology INC All Rights Reserved";

static int
tx_omen_header(struct zmodem * restrict zmodem,
    uint8_t style,const uint8_t * restrict p,size_t count)

{
	size_t i;
	uint32_t crc = UINT32_MAX;

	if (tx_raw(zmodem,ZPAD) != 0) {
		return -1;
	}
	if (tx_raw(zmodem,ZDLE) != 0) {
		return -1;
	}
	if (tx_raw(zmodem,style) != 0) {
		return -1;
	}
	if (tx_raw(zmodem,UINT8_C(0x22) + (uint8_t)(count - 1U)) != 0) {
		return -1;
	}
	for (i=0U;i<count;i++) {
		crc = crc32_byte_update(crc,p[i]);
		if (tx_omen_byte(zmodem,p[i]) != 0) {
			return -1;
		}
	}
	crc = crc32_update(crc,omen_header_crc_suffix,
	    sizeof(omen_header_crc_suffix) - 1U);
	crc = ~crc;
	for (i=0U;i<sizeof(crc);i++) {
		if (tx_omen_byte(zmodem,(uint8_t)crc) != 0) {
			return -1;
		}
		crc >>= 8;
	}
	return 0;
}

static unsigned
active_mobyturbo_classes(struct zmodem * zmodem)

{
	(void)active_tx_classes(zmodem);
	return (zmodem->escape_all_control_characters ?
	    TX_ESCAPE_CONTROL : 0U) |
	    (zmodem->escape_iac ? TX_ESCAPE_IAC : 0U);
}

static bool
mobyturbo_byte_needs_escape(const struct zmodem * zmodem,uint8_t c,
    int previous,unsigned active)

{
	unsigned classes = zmodem->tx_classes[c];

	if (c == ZDLE || (classes & active) != 0U) {
		return true;
	}
	if (!zmodem->escape_all_control_characters) {
		return false;
	}
	if (previous != '@') {
		return false;
	}
	return (classes & TX_ESCAPE_CR) != 0U;
}

static int
tx_mobyturbo_byte(struct zmodem * zmodem,uint8_t c)

{
	if (!mobyturbo_byte_needs_escape(zmodem,c,zmodem->last_sent,
	    active_mobyturbo_classes(zmodem))) {
		return tx_raw(zmodem,c);
	}
	/* A quoted 0xff reaches here only when IAC escaping selected it. */
	if (c == UINT8_C(0xff)) {
		if (tx_raw(zmodem,ZDLE) != 0) {
			return -1;
		}
		return tx_raw(zmodem,ZRUB1);
	}
	return tx_esc(zmodem,c);
}

static int
tx_mobyturbo_header(struct zmodem * restrict zmodem,
    const uint8_t * restrict p,size_t count)

{
	size_t i;
	uint32_t crc = UINT32_MAX;

	if (tx_raw(zmodem,ZPAD) != 0 || tx_raw(zmodem,ZDLE) != 0 ||
	    tx_raw(zmodem,ZBINM32) != 0 ||
	    tx_mobyturbo_byte(zmodem,(uint8_t)(count - 1U)) != 0) {
		return -1;
	}
	for (i=0U;i<count;i++) {
		crc = crc32_byte_update(crc,p[i]);
		if (tx_mobyturbo_byte(zmodem,p[i]) != 0) {
			return -1;
		}
	}
	crc = crc32_update(crc,omen_header_crc_suffix,
	    sizeof(omen_header_crc_suffix) - 1U);
	crc = ~crc;
	for (i=0U;i<sizeof(crc);i++) {
		if (tx_mobyturbo_byte(zmodem,(uint8_t)crc) != 0) {
			return -1;
		}
		crc >>= 8;
	}
	return 0;
}

int
tx_header_length(struct zmodem * restrict zmodem,
    const uint8_t * restrict p,size_t count)

{
	if (count < HDRLEN || count > ZMAXHLEN + 1U) {
		return -1;
	}
	if (zmodem->escape_8th_bit) {
		return tx_omen_header(zmodem,
		    zmodem->use_pack7 ? ZBINP7 : ZBINR32ESC8,p,count);
	}
	if (zmodem->use_mobyturbo) {
		return tx_mobyturbo_header(zmodem,p,count);
	}
	if (zmodem->can_fcs_32 && zmodem->want_fcs_32) {
		return tx_bin32_header(zmodem,p,count);
	}
	if (zmodem->can_fcs_32) {
		return tx_bin16_header(zmodem,p,count);
	}
	return tx_hex_header_length(zmodem,p,count);
}

int
tx_header(struct zmodem * restrict zmodem,const uint8_t * restrict p)

{
	return tx_header_length(zmodem,p,HDRLEN);
}

int
tx_mobyturbo_probe(struct zmodem * zmodem)

{
	static const uint8_t probe[] = {
		UINT8_C(0x23), UINT8_C(0xc1), UINT8_C(0xd4),
		UINT8_C(0x93), UINT8_C(0x11)
	};
	size_t i;

	for (i=0U;i<sizeof(probe);i++) {
		if (tx_raw(zmodem,probe[i]) != 0) {
			return -1;
		}
	}
	return 0;
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
		if (c == UINT8_C(0xff)) {
			buffer_raw(zmodem,ZRUB1,used,previous);
		}
		else {
			buffer_raw(zmodem,(uint8_t)(c ^ 0x40),used,previous);
		}
	}
	else {
		buffer_raw(zmodem,c,used,previous);
	}
}

static void
buffer_mobyturbo(struct zmodem * restrict zmodem,uint8_t c,
    size_t * restrict used,int * restrict previous,unsigned active)

{
	if (mobyturbo_byte_needs_escape(zmodem,c,*previous,active)) {
		buffer_raw(zmodem,ZDLE,used,previous);
		if (zmodem->escape_iac && c == UINT8_C(0xff)) {
			buffer_raw(zmodem,ZRUB1,used,previous);
		}
		else {
			buffer_raw(zmodem,(uint8_t)(c ^ 0x40),used,previous);
		}
	}
	else {
		buffer_raw(zmodem,c,used,previous);
	}
}

struct omen_tx_buffer {
	struct zmodem * zmodem;
	size_t used;
	int previous;
};

static int
flush_omen_tx_buffer(struct omen_tx_buffer * output)

{
	if (output->used == 0U) {
		return 0;
	}
	if (output->zmodem->io.write(output->zmodem->io.context,
	    output->zmodem->tx_data_wire,output->used) != ZMODEM_OK) {
		return -1;
	}
	output->used = 0U;
	return 0;
}

static int
buffer_omen_raw(struct omen_tx_buffer * output,uint8_t c)

{
	if (output->used == sizeof(output->zmodem->tx_data_wire)) {
		if (flush_omen_tx_buffer(output) != 0) {
			return -1;
		}
	}
	output->zmodem->tx_data_wire[output->used++] = c;
	output->previous = c & 0x7f;
	return 0;
}

static int
buffer_omen_byte(struct omen_tx_buffer * output,uint8_t c)

{
	uint8_t encoded[3];
	size_t count = omen_encode_byte(output->zmodem,c,encoded);

	if (sizeof(output->zmodem->tx_data_wire) - output->used < count) {
		if (flush_omen_tx_buffer(output) != 0) {
			return -1;
		}
	}
	output->zmodem->tx_data_wire[output->used++] = encoded[0];
	if (count > 1U) {
		output->zmodem->tx_data_wire[output->used++] = encoded[1];
	}
	if (count > 2U) {
		output->zmodem->tx_data_wire[output->used++] = encoded[2];
	}
	output->previous = output->zmodem->tx_data_wire[output->used - 1U] & 0x7f;
	return 0;
}

static int
buffer_omen_symbol(struct omen_tx_buffer * output,uint8_t c,uint32_t * crc)

{
	*crc = crc32_byte_update(*crc,c);
	return buffer_omen_byte(output,c);
}

static bool
omen_rle_requires_marker(size_t run,uint8_t value)

{
	if (run != 2U) {
		return true;
	}
	if (value == ZRESC) {
		return true;
	}
	return (value & UINT8_C(0x80)) != 0U;
}

static bool
omen_rle_uses_space_run(size_t run,uint8_t value)

{
	if (value != UINT8_C(0x20)) {
		return false;
	}
	return run <= 34U;
}

static int
tx_omen_data(struct zmodem * restrict zmodem,uint8_t sub_frame_type,
    const uint8_t * restrict p,size_t length)

{
	struct omen_tx_buffer output;
	uint32_t crc = UINT32_MAX;
	size_t offset = 0U;

	output.zmodem = zmodem;
	output.used = 0U;
	output.previous = zmodem->last_sent;

	while (offset < length) {
		uint8_t value = p[offset];
		size_t run = 1U;

		while (run < 63U) {
			if (run >= length - offset) {
				break;
			}
			if (p[offset + run] != value) {
				break;
			}
			run += 1U;
		}
		if (run == 1U) {
			if (buffer_omen_symbol(&output,value,&crc) != 0) {
				return -1;
			}
			if (value == ZRESC) {
				if (buffer_omen_symbol(&output,UINT8_C(0x40),&crc) != 0) {
					return -1;
				}
			}
		}
		else if (!omen_rle_requires_marker(run,value)) {
			if (buffer_omen_symbol(&output,value,&crc) != 0) {
				return -1;
			}
			if (buffer_omen_symbol(&output,value,&crc) != 0) {
				return -1;
			}
		}
		else {
			if (buffer_omen_symbol(&output,ZRESC,&crc) != 0) {
				return -1;
			}
			if (omen_rle_uses_space_run(run,value)) {
				if (buffer_omen_symbol(&output,
				    (uint8_t)(run + UINT8_C(0x1d)),&crc) != 0) {
					return -1;
				}
			}
			else {
				if (buffer_omen_symbol(&output,
				    (uint8_t)(run + UINT8_C(0x40)),&crc) != 0) {
					return -1;
				}
				if (buffer_omen_symbol(&output,value,&crc) != 0) {
					return -1;
				}
			}
		}
		offset += run;
	}
	crc = crc32_byte_update(crc,sub_frame_type);
	crc = ~crc;
	if (buffer_omen_raw(&output,ZDLE) != 0) {
		return -1;
	}
	if (buffer_omen_raw(&output,sub_frame_type) != 0) {
		return -1;
	}
	for (offset=0U;offset<sizeof(crc);offset++) {
		if (buffer_omen_byte(&output,(uint8_t)crc) != 0) {
			return -1;
		}
		crc >>= 8;
	}
	if (sub_frame_type == ZCRCW) {
		if (buffer_omen_raw(&output,XON) != 0) {
			return -1;
		}
	}
	if (flush_omen_tx_buffer(&output) != 0) {
		return -1;
	}
	zmodem->last_sent = output.previous;
	return tx_flush(zmodem);
}

/*
 * DSZ Pack-7 represents one through four bytes as a big-endian integer in
 * exactly two through five base-88 digits.  The printable digit alphabet is
 * 0x22 through 0x79.  Since 88^5 is greater than 2^32, every four-byte value
 * has one unique representation.
 */
static size_t
pack7_encode_group(const uint8_t * input,size_t count,uint8_t output[5])

{
	uint32_t value = 0U;
	size_t digits = count + 1U;
	size_t i;

	for (i=0U;i<count;i++) {
		value = (value << 8) | input[i];
	}
	for (i=digits;i>0U;i--) {
		output[i - 1U] = (uint8_t)(value % UINT32_C(88)) + UINT8_C(0x22);
		value /= UINT32_C(88);
	}
	return digits;
}

static void
buffer_pack7_group(struct omen_tx_buffer * output,const uint8_t * input,
    size_t count)

{
	uint8_t encoded[5];
	size_t digits = pack7_encode_group(input,count,encoded);
	size_t i;

	for (i=0U;i<digits;i++) {
		(void)buffer_omen_raw(output,encoded[i]);
	}
}

static int
tx_pack7_data(struct zmodem * restrict zmodem,uint8_t sub_frame_type,
    const uint8_t * restrict data,size_t length)

{
	struct omen_tx_buffer output;
	uint8_t crc_bytes[4];
	uint32_t crc;
	size_t offset;

	output.zmodem = zmodem;
	output.used = 0U;
	output.previous = zmodem->last_sent;
	for (offset=0U;offset<length;) {
		size_t count = length - offset;

		if (count > 4U) {
			count = 4U;
		}
		buffer_pack7_group(&output,&data[offset],count);
		offset += count;
	}
	(void)buffer_omen_raw(&output,UINT8_C(0x21));
	(void)buffer_omen_raw(&output,sub_frame_type);
	crc = ~crc32_byte_update(crc32_update(UINT32_MAX,data,length),
	    sub_frame_type);
	for (offset=0U;offset<sizeof(crc_bytes);offset++) {
		crc_bytes[offset] = (uint8_t)crc;
		crc >>= 8;
	}
	buffer_pack7_group(&output,crc_bytes,sizeof(crc_bytes));
	if (sub_frame_type == ZCRCW) {
		(void)buffer_omen_raw(&output,XON);
	}
	if (flush_omen_tx_buffer(&output) != 0) {
		return -1;
	}
	zmodem->last_sent = output.previous;
	return tx_flush(zmodem);
}

#if defined(UINT64_MAX) && !defined(ZMODEM_FORCE_32BIT_SPAN)
typedef uint64_t span_word;
#define SPAN_WORD_ONES UINT64_C(0x0101010101010101)
#define SPAN_WORD_HIGHS UINT64_C(0x8080808080808080)
#define SPAN_WORD_ZDLE UINT64_C(0x1818181818181818)
#define SPAN_WORD_CONTROL_MASK UINT64_C(0x7f7f7f7f7f7f7f7f)
#define SPAN_WORD_SPECIAL_MASK UINT64_C(0x7c7c7c7c7c7c7c7c)
#define SPAN_WORD_SPECIAL UINT64_C(0x1010101010101010)
#define SPAN_WORD_CR UINT64_C(0x0d0d0d0d0d0d0d0d)
#define SPAN_WORD_FLOW_MASK UINT64_C(0x7d7d7d7d7d7d7d7d)
#define SPAN_WORD_FLOW UINT64_C(0x1111111111111111)
#else
typedef uint32_t span_word;
#define SPAN_WORD_ONES UINT32_C(0x01010101)
#define SPAN_WORD_HIGHS UINT32_C(0x80808080)
#define SPAN_WORD_ZDLE UINT32_C(0x18181818)
#define SPAN_WORD_CONTROL_MASK UINT32_C(0x7f7f7f7f)
#define SPAN_WORD_SPECIAL_MASK UINT32_C(0x7c7c7c7c)
#define SPAN_WORD_SPECIAL UINT32_C(0x10101010)
#define SPAN_WORD_CR UINT32_C(0x0d0d0d0d)
#define SPAN_WORD_FLOW_MASK UINT32_C(0x7d7d7d7d)
#define SPAN_WORD_FLOW UINT32_C(0x11111111)
#endif

static inline size_t
tx_copy_plain_span(const uint8_t * classes,uint8_t * output,
    const uint8_t * data,size_t length,int * previous)

{
	const span_word ones = SPAN_WORD_ONES;
	const span_word highs = SPAN_WORD_HIGHS;
	size_t span = 0U;

	if (*previous == '@' &&
	    (((unsigned)classes[data[0]] & TX_ESCAPE_CR) != 0U)) {
		return 0U;
	}
	while (length - span >= sizeof(span_word)) {
		span_word word;
		span_word zdle;
		span_word special;
		span_word cr;

		/* Find mandatory or conditional escapes in a plain word. */
		(void)memcpy(&word,&data[span],sizeof(word));
		zdle = word ^ SPAN_WORD_ZDLE;
		/* 0x10 through 0x13 cover the mandatory controls except ZDLE. */
		special = (word & SPAN_WORD_SPECIAL_MASK) ^ SPAN_WORD_SPECIAL;
		cr = (word & SPAN_WORD_CONTROL_MASK) ^ SPAN_WORD_CR;
		if ((((zdle - ones) & ~zdle & highs) |
		    ((special - ones) & ~special & highs) |
		    ((cr - ones) & ~cr & highs)) != 0U) {
			break;
		}
		(void)memcpy(&output[span],&word,sizeof(word));
		span += sizeof(word);
	}
	if (span > 0U) {
		*previous = data[span - 1U] & 0x7f;
	}
	while ((span < length) &&
	    (((unsigned)classes[data[span]] & TX_ESCAPE_ALWAYS) == 0U) &&
	    ((((unsigned)classes[data[span]] & TX_ESCAPE_CR) == 0U) ||
	    *previous != '@')) {
		output[span] = data[span];
		*previous = data[span] & 0x7f;
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
	if (zmodem->escape_8th_bit) {
		if (zmodem->use_pack7) {
			return tx_pack7_data(zmodem,sub_frame_type,p,l);
		}
		return tx_omen_data(zmodem,sub_frame_type,p,l);
	}
	if (zmodem->use_mobyturbo) {
		active = active_mobyturbo_classes(zmodem);
		for (i=0U;i<l;i++) {
			buffer_mobyturbo(zmodem,p[i],&used,&previous,active);
		}
	}
	else if (active == TX_ESCAPE_ALWAYS) {
		for (i=0;i<l;) {
			size_t span = tx_copy_plain_span(zmodem->tx_classes,
			    &zmodem->tx_data_wire[used],&p[i],l - i,&previous);

			i += span;
			used += span;
			if (i < l) {
				buffer_raw(zmodem,ZDLE,&used,&previous);
				buffer_raw(zmodem,(uint8_t)(p[i] ^ 0x40),&used,
				    &previous);
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

	if (zmodem->use_mobyturbo ||
	    (zmodem->want_fcs_32 && zmodem->can_fcs_32)) {
		uint32_t crc = crc32_update(UINT32_MAX,p,l);

		crc = ~crc32_byte_update(crc,sub_frame_type);
		if (zmodem->use_mobyturbo) {
			buffer_mobyturbo(zmodem,(uint8_t)crc,&used,&previous,active);
			buffer_mobyturbo(zmodem,(uint8_t)(crc >> 8),&used,&previous,
			    active);
			buffer_mobyturbo(zmodem,(uint8_t)(crc >> 16),&used,&previous,
			    active);
			buffer_mobyturbo(zmodem,(uint8_t)(crc >> 24),&used,&previous,
			    active);
		}
		else {
			buffer_tx(zmodem,(uint8_t)crc,&used,&previous,active);
			buffer_tx(zmodem,(uint8_t)(crc >> 8),&used,&previous,active);
			buffer_tx(zmodem,(uint8_t)(crc >> 16),&used,&previous,active);
			buffer_tx(zmodem,(uint8_t)(crc >> 24),&used,&previous,active);
		}
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

	if (result == CAN && !zmodem->receive_mobyturbo) {
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
rx_is_unescaped_control(const struct zmodem * zmodem,int c)
{
	return zmodem->receive_escaped_control_characters && c != ZDLE &&
	    (c & 0x60) == 0;
}

static bool
rx_needs_slow_path(const struct zmodem * zmodem,int c)
{
	if (zmodem->receive_mobyturbo) {
		return c == ZDLE;
	}
	return (c == ZDLE) || rx_is_flow_control(c) ||
	    rx_is_unescaped_control(zmodem,c);
}

/*
 * Decoded bytes occupy 0 through UINT8_MAX and errors are negative.  Mark
 * data-subpacket terminators with the next positive value so the result also
 * fits in the minimum range of a conforming C int.
 */
enum {
	RX_FRAME_END_FLAG = UINT8_MAX + 1
};

static int
rx_cursor_slow(struct zmodem * restrict zmodem,int timeout_ms,int c,
    struct rx_cursor * restrict cursor)
{
	for (;;) {
		while (rx_is_flow_control(c) ||
		    rx_is_unescaped_control(zmodem,c)) {
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
			if (zmodem->receive_escape8_format == ZMODEM_ESCAPE8_LEGACY &&
			    (c & 0x80) != 0) {
				return c ^ 0x40;
			}

			if (rx_needs_slow_path(zmodem,c)) {
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
					return c | RX_FRAME_END_FLAG;
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
	if (!rx_needs_slow_path(zmodem,c)) {
		return c;
	}
	return rx_cursor_slow(zmodem,timeout_ms,c,cursor);
}

/* Decode DSZ's seven-bit-safe ZMODEM-90 quoting. */
static int
rx_omen_cursor(struct zmodem * restrict zmodem,int timeout_ms,
    struct rx_cursor * restrict cursor)

{
	bool high = false;

	for (;;) {
		int c = rx_cursor_raw(zmodem,timeout_ms,cursor);

		if (c < 0) {
			return c;
		}
		c &= 0x7f;
		if (rx_is_flow_control(c)) {
			continue;
		}
		if (c == SO) {
			high = true;
			continue;
		}
		if (c != ZDLE) {
			if (rx_is_unescaped_control(zmodem,c)) {
				continue;
			}
			return c | (high ? 0x80 : 0);
		}

		do {
			c = rx_cursor_raw(zmodem,timeout_ms,cursor);
			if (c < 0) {
				return c;
			}
			c &= 0x7f;
		} while (rx_is_flow_control(c));

		switch (c) {
			case ZCRCE:
			case ZCRCG:
			case ZCRCQ:
			case ZCRCW:
				return c | RX_FRAME_END_FLAG;
			case ZRUB0: c = 0x7f; break;
			case ZRUB1: c = 0xff; break;
			case UINT8_C(0x6e): c = 0x0e; break;
			case UINT8_C(0x6f): c = 0x8e; break;
			case UINT8_C(0x70): c = 0x90; break;
			case UINT8_C(0x71): c = 0x91; break;
			case UINT8_C(0x72): c = 0x93; break;
			case UINT8_C(0x73): c = 0x80; break;
			case UINT8_C(0x74): c = 0x98; break;
			default:
				if ((c & 0x60) != 0x40) {
					return ZMODEM_INVALID_DATA;
				}
				c ^= 0x40;
				break;
		}
		return c | (high ? 0x80 : 0);
	}
}

static int
rx_omen_byte_cursor(struct zmodem * restrict zmodem,int timeout_ms,
    int invalid_result,struct rx_cursor * restrict cursor)

{
	int c = rx_omen_cursor(zmodem,timeout_ms,cursor);

	if (c < 0) {
		return c;
	}
	if (c > UINT8_MAX) {
		return invalid_result;
	}
	return c;
}

static inline size_t
rx_plain_span(const struct zmodem * restrict zmodem,
    const struct rx_cursor * restrict cursor)

{
	const uint8_t * data = &zmodem->input_buffer[cursor->input_index];
	const span_word ones = SPAN_WORD_ONES;
	const span_word highs = SPAN_WORD_HIGHS;
	size_t length = 0U;

	if (zmodem->receive_escaped_control_characters) {
		if (zmodem->receive_mobyturbo) {
			while (length < cursor->input_count) {
				if (data[length] == ZDLE) {
					break;
				}
				length += 1U;
			}
			return length;
		}
		while ((length < cursor->input_count) &&
		    !rx_needs_slow_path(zmodem,data[length])) {
			length += 1U;
		}
		return length;
	}

	while (cursor->input_count - length >= sizeof(span_word)) {
		span_word word;
		span_word zdle;
		span_word flow;

		/* Detect ZDLE or flow control in a word at a time. */
		(void)memcpy(&word,&data[length],sizeof(word));
		zdle = word ^ SPAN_WORD_ZDLE;
		if (zmodem->receive_mobyturbo) {
			if (((zdle - ones) & ~zdle & highs) != 0U) {
				break;
			}
			length += sizeof(word);
			continue;
		}
		/* The four flow-control values vary only in bits 1 and 7. */
		flow = (word & SPAN_WORD_FLOW_MASK) ^ SPAN_WORD_FLOW;
		if ((((zdle - ones) & ~zdle & highs) |
		    ((flow - ones) & ~flow & highs)) != 0U) {
			break;
		}
		length += sizeof(word);
	}
	while ((length < cursor->input_count) &&
	    !rx_needs_slow_path(zmodem,data[length])) {
		length += 1U;
	}
	return length;
}

static inline int
rx(struct zmodem * zmodem,int timeout_ms)

{
	int c = rx_raw(zmodem,timeout_ms);

	if ((c >= 0) && rx_needs_slow_path(zmodem,c)) {
		return rx_slow(zmodem,timeout_ms,c);
	}
	return c;
}

static int
rx_byte(struct zmodem * zmodem,int timeout_ms,int invalid_result)

{
	int c = rx(zmodem,timeout_ms);

	if (c < 0) {
		return c;
	}
	if (c > UINT8_MAX) {
		return invalid_result;
	}
	return c;
}

static int
rx_hex_terminator_byte(struct zmodem * zmodem,int timeout_ms)
{
	int c;

	do {
		c = rx_raw(zmodem,timeout_ms);
		if (c < 0) {
			return c;
		}
	} while (rx_is_flow_control(c));
	return c;
}

static int
rx_crc16(struct zmodem * restrict zmodem,int timeout_ms,
	int invalid_result,uint16_t * restrict value)

{
	int high;
	int low;

	high = rx_byte(zmodem,timeout_ms,invalid_result);
	if (high < 0) {
		return high;
	}
	low = rx_byte(zmodem,timeout_ms,invalid_result);
	if (low < 0) {
		return low;
	}

	*value = (uint16_t)(((uint16_t)(uint8_t)high << 8) | (uint8_t)low);
	return 0;
}

static int
rx_crc32(struct zmodem * restrict zmodem,int timeout_ms,
	int invalid_result,uint32_t * restrict value)

{
	int c;
	size_t i;
	uint32_t result = 0;

	for (i=0;i<sizeof(result);i++) {
		c = rx_byte(zmodem,timeout_ms,invalid_result);
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
		if (c <= UINT8_MAX) {
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

	sub_frame_type = (uint8_t)c;

	if (!overflow) {
		crc = crc32_update(crc,p,used);
	}
	crc = crc32_byte_update(crc,(uint8_t)sub_frame_type);

	crc = ~crc;

	{
		int result = rx_crc32(zmodem,1000,ZMODEM_INVALID_DATA,
		    &rxd_crc);

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
		if (c <= UINT8_MAX) {
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

	sub_frame_type = (uint8_t)c;

	if (!overflow) {
		crc = crc16_buffer_update(crc,p,used);
	}
	crc = crc16_update(crc,(uint8_t)sub_frame_type);

	crc = crc16_update(crc,0U);
	crc = crc16_update(crc,0U);

	{
		int result = rx_crc16(zmodem,1000,ZMODEM_INVALID_DATA,
		    &rxd_crc);

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
rx_rle_cursor(struct zmodem * restrict zmodem,int timeout_ms,
    struct rx_cursor * restrict cursor)

{
	if (zmodem->receive_escape8_format == ZMODEM_ESCAPE8_OMEN) {
		return rx_omen_cursor(zmodem,timeout_ms,cursor);
	}
	return rx_cursor(zmodem,timeout_ms,cursor);
}

static int
rx_rle_byte_cursor(struct zmodem * restrict zmodem,int timeout_ms,
    struct rx_cursor * restrict cursor)

{
	int c = rx_rle_cursor(zmodem,timeout_ms,cursor);

	if (c > UINT8_MAX) {
		return ZMODEM_INVALID_DATA;
	}
	return c;
}

enum omen_rle_count_kind {
	OMEN_RLE_COUNT_INVALID,
	OMEN_RLE_COUNT_LITERAL,
	OMEN_RLE_COUNT_SPACES,
	OMEN_RLE_COUNT_VALUE
};

static enum omen_rle_count_kind
classify_omen_rle_count(int c)

{
	enum omen_rle_count_kind kind = OMEN_RLE_COUNT_INVALID;

	if (c == 0x40) {
		kind = OMEN_RLE_COUNT_LITERAL;
	}
	else if (c >= 0x20) {
		if (c <= 0x3f) {
			kind = OMEN_RLE_COUNT_SPACES;
		}
		else if (c >= 0x42) {
			if (c <= 0x7f) {
				kind = OMEN_RLE_COUNT_VALUE;
			}
		}
	}
	return kind;
}

static int
rx_rle_data(struct zmodem * restrict zmodem,uint8_t * restrict p,
    size_t capacity,size_t * restrict l)

{
	enum omen_rle_state {
		OMEN_RLE_NORMAL,
		OMEN_RLE_COUNT,
		OMEN_RLE_VALUE
	} state = OMEN_RLE_NORMAL;
	struct rx_cursor cursor;
	size_t limit = capacity < ZMAXSPLEN ? capacity : ZMAXSPLEN;
	size_t used = 0U;
	size_t run = 0U;
	uint32_t crc = UINT32_MAX;
	uint32_t rxd_crc = 0U;
	bool overflow = false;
	int sub_frame_type;
	size_t i;

	load_rx_cursor(zmodem,&cursor);
	for (;;) {
		int c = rx_rle_cursor(zmodem,1000,&cursor);

		if (c < 0) {
			store_rx_cursor(zmodem,&cursor);
			*l = used;
			return c;
		}
		if (c > UINT8_MAX) {
			if (state != OMEN_RLE_NORMAL) {
				store_rx_cursor(zmodem,&cursor);
				*l = used;
				return ZMODEM_INVALID_DATA;
			}
			sub_frame_type = (uint8_t)c;
			crc = crc32_byte_update(crc,(uint8_t)sub_frame_type);
			break;
		}
		crc = crc32_byte_update(crc,(uint8_t)c);
		switch (state) {
			case OMEN_RLE_NORMAL:
				if ((uint8_t)c == ZRESC) {
					state = OMEN_RLE_COUNT;
					break;
				}
				if (used < limit) {
					p[used++] = (uint8_t)c;
				}
				else {
					overflow = true;
				}
				break;
			case OMEN_RLE_COUNT:
				switch (classify_omen_rle_count(c)) {
				case OMEN_RLE_COUNT_LITERAL:
					if (used < limit) {
						p[used++] = ZRESC;
					}
					else {
						overflow = true;
					}
					state = OMEN_RLE_NORMAL;
					break;
				case OMEN_RLE_COUNT_SPACES:
					run = (size_t)(c - 0x1d);
					while (run-- > 0U) {
						if (used < limit) {
							p[used++] = UINT8_C(0x20);
						}
						else {
							overflow = true;
						}
					}
					state = OMEN_RLE_NORMAL;
					break;
				case OMEN_RLE_COUNT_VALUE:
					run = (size_t)(c - 0x40);
					state = OMEN_RLE_VALUE;
					break;
				default:
					store_rx_cursor(zmodem,&cursor);
					*l = used;
					return ZMODEM_INVALID_DATA;
				}
				break;
			case OMEN_RLE_VALUE:
				while (run-- > 0U) {
					if (used < limit) {
						p[used++] = (uint8_t)c;
					}
					else {
						overflow = true;
					}
				}
				state = OMEN_RLE_NORMAL;
				break;
		}
	}
	crc = ~crc;
	for (i=0U;i<sizeof(rxd_crc);i++) {
		int c = rx_rle_byte_cursor(zmodem,1000,&cursor);

		if (c < 0) {
			store_rx_cursor(zmodem,&cursor);
			*l = used;
			return c;
		}
		rxd_crc |= (uint32_t)(uint8_t)c << (i * 8U);
	}
	store_rx_cursor(zmodem,&cursor);
	*l = used;
	if (rxd_crc != crc) {
		return ZMODEM_INVALID_DATA;
	}
	if (overflow) {
		return ZMODEM_INVALID_DATA;
	}
	return sub_frame_type;
}

static int
rx_pack7_group(struct zmodem * restrict zmodem,int timeout_ms,
    struct rx_cursor * restrict cursor,uint8_t output[4],size_t * count)

{
	uint32_t value = 0U;
	size_t digits = 0U;
	size_t bytes;
	size_t i;

	for (;;) {
		int c = rx_omen_cursor(zmodem,timeout_ms,cursor);
		uint32_t digit;

		if (c < 0) {
			return c;
		}
		if (c > UINT8_MAX) {
			return ZMODEM_INVALID_DATA;
		}
		if (c == 0x21) {
			if (digits == 1U) {
				return ZMODEM_INVALID_DATA;
			}
			break;
		}
		if (c < 0x22 || c > 0x79) {
			return ZMODEM_INVALID_DATA;
		}
		digit = (uint32_t)(c - 0x22);
		if (value > (UINT32_MAX - digit) / UINT32_C(88)) {
			return ZMODEM_INVALID_DATA;
		}
		value = value * UINT32_C(88) + digit;
		digits += 1U;
		if (digits == 5U) {
			break;
		}
	}
	bytes = digits == 0U ? 0U : digits - 1U;
	if (bytes < 4U && value >= (UINT32_C(1) << (bytes * 8U))) {
		return ZMODEM_INVALID_DATA;
	}
	for (i=bytes;i>0U;i--) {
		output[i - 1U] = (uint8_t)value;
		value >>= 8;
	}
	*count = bytes;
	return ZMODEM_OK;
}

static int
rx_pack7_data(struct zmodem * restrict zmodem,uint8_t * restrict p,
    size_t capacity,size_t * restrict length)

{
	uint8_t decoded[4];
	size_t limit = capacity < ZMAXSPLEN ? capacity : ZMAXSPLEN;
	size_t used = 0U;
	uint32_t crc = UINT32_MAX;
	uint32_t received_crc;
	bool overflow = false;
	int sub_frame_type;
	struct rx_cursor cursor;

	load_rx_cursor(zmodem,&cursor);
	for (;;) {
		size_t count;
		size_t i;
		int result = rx_pack7_group(zmodem,1000,&cursor,decoded,&count);

		if (result != ZMODEM_OK) {
			store_rx_cursor(zmodem,&cursor);
			*length = used;
			return result;
		}
		for (i=0U;i<count;i++) {
			crc = crc32_byte_update(crc,decoded[i]);
			if (used < limit) {
				p[used++] = decoded[i];
			}
			else {
				overflow = true;
			}
		}
		if (count < 4U) {
			break;
		}
	}
	{
		int c = rx_omen_cursor(zmodem,1000,&cursor);

		if (c < 0) {
			store_rx_cursor(zmodem,&cursor);
			*length = used;
			return c;
		}
		if (c < ZCRCE || c > ZCRCW) {
			store_rx_cursor(zmodem,&cursor);
			*length = used;
			return ZMODEM_INVALID_DATA;
		}
		sub_frame_type = c;
	}
	crc = ~crc32_byte_update(crc,(uint8_t)sub_frame_type);
	{
		size_t count;
		int result = rx_pack7_group(zmodem,1000,&cursor,decoded,&count);

		if (result != ZMODEM_OK || count != sizeof(decoded)) {
			store_rx_cursor(zmodem,&cursor);
			*length = used;
			return result == ZMODEM_OK ? ZMODEM_INVALID_DATA : result;
		}
	}
	received_crc = (uint32_t)decoded[0] |
	    ((uint32_t)decoded[1] << 8) |
	    ((uint32_t)decoded[2] << 16) |
	    ((uint32_t)decoded[3] << 24);
	store_rx_cursor(zmodem,&cursor);
	*length = used;
	if (received_crc != crc || overflow) {
		return ZMODEM_INVALID_DATA;
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

	if (zmodem->receive_encoded_data != ZMODEM_ENCODED_DATA_NONE) {
		if (zmodem->receive_encoded_data == ZMODEM_ENCODED_DATA_PACK7) {
			sub_frame_type = rx_pack7_data(zmodem,p,capacity,l);
		}
		else {
			sub_frame_type = rx_rle_data(zmodem,p,capacity,l);
		}
	}
	else if (zmodem->receive_32_bit_data) {
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

	c = rx_byte(zmodem,timeout_ms,ZMODEM_INVALID_HEADER);

	if (c < 0) {
		return c;
	}

	c &= 0x7f;
	if (c > '9') {
		if (c < 'a' || c > 'f') {
			/*
			 * Illegal hex is a malformed header, not an input timeout.
			 */
			return ZMODEM_INVALID_HEADER;
		}

		c -= 'a' - 10;
	}
	else {
		if (c < '0') {
			/*
			 * Illegal hex is a malformed header, not an input timeout.
			 */
			return ZMODEM_INVALID_HEADER;
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
rx_bin16_header(struct zmodem * zmodem,int timeout_ms,size_t count)

{
	int c;
	size_t n;
	uint16_t crc;
	uint16_t rxd_crc;

	crc = 0;

	for (n=0U;n<count;n++) {
		c = rx_byte(zmodem,timeout_ms,ZMODEM_INVALID_HEADER);
		if (c < 0) {
			return c;
		}
		crc = crc16_update(crc,(uint8_t)c);
		zmodem->rxd_header[n] = (uint8_t)c;
	}

	crc = crc16_update(crc,0U);
	crc = crc16_update(crc,0U);

	{
		int result = rx_crc16(zmodem,1000,ZMODEM_INVALID_HEADER,
		    &rxd_crc);

		if (result != ZMODEM_OK) {
			return result;
		}
	}

	if (rxd_crc != crc) {
		return ZMODEM_INVALID_HEADER;
	}

	zmodem->rxd_header_len = count;
	return ZMODEM_OK;
}

static int
rx_hex_header(struct zmodem * zmodem,int timeout_ms,size_t count)

{
	int c;
	size_t i;
	uint16_t crc = 0;
	uint16_t rxd_crc;

	for (i=0U;i<count;i++) {
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
	c = rx_hex_terminator_byte(zmodem,timeout_ms);
	if (c < 0) {
		return c;
	}
	c &= 0x7f;
	if (c == CR) {
		/*
		 * both are expected with CR
		 */
		c = rx_hex_terminator_byte(zmodem,timeout_ms);
		if (c < 0) {
			return c;
		}
		c &= 0x7f;
	}
	if (c != LF) {
		return ZMODEM_INVALID_HEADER;
	}
	zmodem->rxd_header_len = count;
	return ZMODEM_OK;
}

static int
rx_bin32_header(struct zmodem * zmodem,int timeout_ms,size_t count)

{
	int c;
	size_t n;
	uint32_t crc;
	uint32_t rxd_crc;

	crc = UINT32_MAX;

	for (n=0U;n<count;n++) {
		c = rx_byte(zmodem,timeout_ms,ZMODEM_INVALID_HEADER);
		if (c < 0) {
			return c;
		}
		crc = crc32_byte_update(crc,(uint8_t)c);
		zmodem->rxd_header[n] = (uint8_t)c;
	}

	crc = ~crc;

	{
		int result = rx_crc32(zmodem,timeout_ms,ZMODEM_INVALID_HEADER,
		    &rxd_crc);

		if (result != ZMODEM_OK) {
			return result;
		}
	}

	if (rxd_crc != crc) {
		return ZMODEM_INVALID_HEADER;
	}

	zmodem->rxd_header_len = count;
	return ZMODEM_OK;
}

static int
rx_variable_length(struct zmodem * zmodem,int timeout_ms,bool hex)

{
	int count = hex ? rx_hex(zmodem,timeout_ms) :
	    rx_byte(zmodem,timeout_ms,ZMODEM_INVALID_HEADER);

	if (count < 0) {
		return count;
	}
	if ((size_t)count < HDRLEN - 1U) {
		return ZMODEM_INVALID_HEADER;
	}
	if ((size_t)count > ZMAXHLEN) {
		return ZMODEM_INVALID_HEADER;
	}
	return count + 1;
}

static int
rx_omen_header(struct zmodem * zmodem,int timeout_ms,
    enum zmodem_encoded_data_format format)

{
	struct rx_cursor cursor;
	uint32_t crc = UINT32_MAX;
	uint32_t rxd_crc = 0U;
	size_t parameter_count;
	size_t count;
	size_t i;
	int c;

	load_rx_cursor(zmodem,&cursor);
	do {
		c = rx_cursor_raw(zmodem,timeout_ms,&cursor);
		if (c < 0) {
			store_rx_cursor(zmodem,&cursor);
			return c;
		}
		c &= 0x7f;
	} while (rx_is_flow_control(c));
	if (c < 0x22) {
		store_rx_cursor(zmodem,&cursor);
		return ZMODEM_INVALID_HEADER;
	}
	parameter_count = (size_t)(c - 0x22);
	if (parameter_count > ZMAXHLEN) {
		store_rx_cursor(zmodem,&cursor);
		return ZMODEM_INVALID_HEADER;
	}
	count = parameter_count + 1U;
	for (i=0U;i<count;i++) {
		c = rx_omen_byte_cursor(zmodem,timeout_ms,ZMODEM_INVALID_HEADER,
		    &cursor);
		if (c < 0) {
			store_rx_cursor(zmodem,&cursor);
			return c;
		}
		zmodem->rxd_header[i] = (uint8_t)c;
		crc = crc32_byte_update(crc,(uint8_t)c);
	}
	crc = crc32_update(crc,omen_header_crc_suffix,
	    sizeof(omen_header_crc_suffix) - 1U);
	crc = ~crc;
	for (i=0U;i<sizeof(rxd_crc);i++) {
		c = rx_omen_byte_cursor(zmodem,timeout_ms,ZMODEM_INVALID_HEADER,
		    &cursor);
		if (c < 0) {
			store_rx_cursor(zmodem,&cursor);
			return c;
		}
		rxd_crc |= (uint32_t)(uint8_t)c << (i * 8U);
	}
	store_rx_cursor(zmodem,&cursor);
	if (rxd_crc != crc) {
		return ZMODEM_INVALID_HEADER;
	}
	zmodem->rxd_header_len = count;
	zmodem->receive_32_bit_data = true;
	zmodem->receive_encoded_data = format;
	zmodem->receive_escape8_format = ZMODEM_ESCAPE8_OMEN;
	return ZMODEM_OK;
}

static int
rx_mobyturbo_header(struct zmodem * zmodem,int timeout_ms)

{
	uint32_t crc = UINT32_MAX;
	uint32_t rxd_crc = 0U;
	size_t parameter_count;
	size_t count;
	size_t i;
	int c;

	/* The count and all following bytes use MobyTurbo transparency. */
	zmodem->receive_mobyturbo = true;
	c = rx_byte(zmodem,timeout_ms,ZMODEM_INVALID_HEADER);
	if (c < 0) {
		return c;
	}
	parameter_count = (size_t)c;
	if (parameter_count < HDRLEN - 1U || parameter_count > ZMAXHLEN) {
		return ZMODEM_INVALID_HEADER;
	}
	count = parameter_count + 1U;
	for (i=0U;i<count;i++) {
		c = rx_byte(zmodem,timeout_ms,ZMODEM_INVALID_HEADER);
		if (c < 0) {
			return c;
		}
		zmodem->rxd_header[i] = (uint8_t)c;
		crc = crc32_byte_update(crc,(uint8_t)c);
	}
	crc = crc32_update(crc,omen_header_crc_suffix,
	    sizeof(omen_header_crc_suffix) - 1U);
	crc = ~crc;
	for (i=0U;i<sizeof(rxd_crc);i++) {
		c = rx_byte(zmodem,timeout_ms,ZMODEM_INVALID_HEADER);
		if (c < 0) {
			return c;
		}
		rxd_crc |= (uint32_t)(uint8_t)c << (i * 8U);
	}
	if (rxd_crc != crc) {
		return ZMODEM_INVALID_HEADER;
	}
	zmodem->rxd_header_len = count;
	zmodem->receive_32_bit_data = true;
	zmodem->receive_encoded_data = ZMODEM_ENCODED_DATA_NONE;
	zmodem->receive_escape8_format = ZMODEM_ESCAPE8_NONE;
	return ZMODEM_OK;
}

/*
 * receive any style header
 * if the errors flag is set than whenever an invalid header packet is
 * received INVHDR will be returned. otherwise we wait for a good header
 * also; flags are set to select the checksum, RLE, and quoting used by data
 * packets following this header.  Fixed and variable headers are accepted.
 */

static int
rx_header_raw(struct zmodem * zmodem,int timeout_ms,bool report_errors)

{
	static const uint8_t mobyturbo_probe[] = {
		UINT8_C(0x23), UINT8_C(0xc1), UINT8_C(0xd4),
		UINT8_C(0x93), UINT8_C(0x11)
	};
	size_t probe_index = 0U;
	bool probe_passed = false;
	int c;
	int result;

	zmodem->rxd_header_len = 0U;
	zmodem->mobyturbo_probe_passed = false;

	do {
		do {
			c = rx_raw(zmodem,timeout_ms);
			if (c < 0) {
				return c;
			}
			if ((uint8_t)c == mobyturbo_probe[probe_index]) {
				probe_index += 1U;
				if (probe_index == sizeof(mobyturbo_probe)) {
					probe_passed = true;
					probe_index = 0U;
				}
			}
			else {
				probe_index = (uint8_t)c == mobyturbo_probe[0] ? 1U : 0U;
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
			case ZBINR32ESC8:
				zmodem->receive_mobyturbo = false;
				result = rx_omen_header(zmodem,timeout_ms,
				    ZMODEM_ENCODED_DATA_RLE);
				break;
			case ZBINP7:
				zmodem->receive_mobyturbo = false;
				result = rx_omen_header(zmodem,timeout_ms,
				    ZMODEM_ENCODED_DATA_PACK7);
				break;
			case ZBINM32:
				result = rx_mobyturbo_header(zmodem,timeout_ms);
				break;
			case ZBIN:
				zmodem->receive_mobyturbo = false;
				result = rx_bin16_header(zmodem,timeout_ms,HDRLEN);
				zmodem->receive_32_bit_data = false;
				zmodem->receive_encoded_data = ZMODEM_ENCODED_DATA_NONE;
				break;
			case ZHEX:
				zmodem->receive_mobyturbo = false;
				result = rx_hex_header(zmodem,timeout_ms,HDRLEN);
				zmodem->receive_32_bit_data = false;
				zmodem->receive_encoded_data = ZMODEM_ENCODED_DATA_NONE;
				break;
			case ZBIN32:
				zmodem->receive_mobyturbo = false;
				result = rx_bin32_header(zmodem,timeout_ms,HDRLEN);
				zmodem->receive_32_bit_data = true;
				zmodem->receive_encoded_data = ZMODEM_ENCODED_DATA_NONE;
				break;
			case ZBINR32:
				zmodem->receive_mobyturbo = false;
				result = rx_bin32_header(zmodem,timeout_ms,HDRLEN);
				zmodem->receive_32_bit_data = true;
				zmodem->receive_encoded_data = ZMODEM_ENCODED_DATA_RLE;
				break;
			case ZVBIN:
				zmodem->receive_mobyturbo = false;
				result = rx_variable_length(zmodem,timeout_ms,false);
				if (result >= 0) {
					result = rx_bin16_header(zmodem,timeout_ms,
					    (size_t)result);
				}
				zmodem->receive_32_bit_data = false;
				zmodem->receive_encoded_data = ZMODEM_ENCODED_DATA_NONE;
				break;
			case ZVHEX:
				zmodem->receive_mobyturbo = false;
				result = rx_variable_length(zmodem,timeout_ms,true);
				if (result >= 0) {
					result = rx_hex_header(zmodem,timeout_ms,
					    (size_t)result);
				}
				zmodem->receive_32_bit_data = false;
				zmodem->receive_encoded_data = ZMODEM_ENCODED_DATA_NONE;
				break;
			case ZVBIN32:
			case ZVBINR32:
				zmodem->receive_mobyturbo = false;
				{
					int count = rx_variable_length(zmodem,timeout_ms,
					    false);

					result = count;
					if (count >= 0) {
						result = rx_bin32_header(zmodem,
						    timeout_ms,(size_t)count);
					}
				}
				zmodem->receive_32_bit_data = true;
				zmodem->receive_encoded_data =
				    (c & 0x7f) == ZVBINR32 ?
				    ZMODEM_ENCODED_DATA_RLE :
				    ZMODEM_ENCODED_DATA_NONE;
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

	zmodem->mobyturbo_probe_passed = probe_passed;
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

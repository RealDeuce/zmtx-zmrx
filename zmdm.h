/*
 * zmdm.h
 * zmodem primitives prototypes and state
 *
 * Copyright (c) 1994 Stephen Hurd
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Special thanks to Jacques Mattheij, formerly of Mattheij Computer Service,
 * and original author of zmtx/zmrx.
 */

#ifndef ZMDM_H_INCLUDED
#define ZMDM_H_INCLUDED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zmodem.h"

enum zmodem_result {
	ZMODEM_INVALID_ARGUMENT = -6,
	ZMODEM_IO_ERROR = -5,
	ZMODEM_CANCELLED = -4,
	ZMODEM_INVALID_DATA = -3,
	ZMODEM_INVALID_HEADER = -2,
	ZMODEM_TIMEOUT = -1,
	ZMODEM_OK = 0,
	ZMODEM_FRAME_OK = 1,
	ZMODEM_END_OF_FRAME = 2
};

enum zmodem_escape8_format {
	ZMODEM_ESCAPE8_NONE,
	ZMODEM_ESCAPE8_LEGACY,
	ZMODEM_ESCAPE8_OMEN
};

enum zmodem_encoded_data_format {
	ZMODEM_ENCODED_DATA_NONE,
	ZMODEM_ENCODED_DATA_RLE,
	ZMODEM_ENCODED_DATA_PACK7
};

#define ENDOFFRAME ZMODEM_END_OF_FRAME
#define FRAMEOK ZMODEM_FRAME_OK
#define TIMEOUT ZMODEM_TIMEOUT
#define INVHDR ZMODEM_INVALID_HEADER
#define INVDATA ZMODEM_INVALID_DATA
#define HDRLEN UINT8_C(5)
#define ZMODEM_INPUT_CAPACITY (2U * ZMAXSPLEN)
#define ZMODEM_TX_DATA_WIRE_CAPACITY (2U * ZMAXSPLEN + 11U)
#define ZMODEM_TX_BINARY_HEADER_WIRE_CAPACITY 24U
#define ZMODEM_TX_BURST_CAPACITY \
	(ZMODEM_TX_BINARY_HEADER_WIRE_CAPACITY + ZMODEM_TX_DATA_WIRE_CAPACITY)

struct zmodem_io {
	void * context;
	int (*read)(void *,uint8_t *,size_t,size_t *,int);
	int (*write)(void *,const uint8_t *,size_t);
	int (*flush)(void *);
	int (*poll)(void *);
	int (*purge)(void *);
};

struct zmodem {
	struct zmodem_io io;
	/* Frame type followed by at most ZMAXHLEN information bytes. */
	uint8_t rxd_header[ZMAXHLEN + 1U];
	size_t rxd_header_len;
	bool can_full_duplex;
	bool can_overlap_io;
	bool can_break;
	bool can_fcs_32;
	bool can_rle;
	bool escape_all_control_characters;
	bool escape_8th_bit;
	bool escape_iac;
	enum zmodem_escape8_format receive_escape8_format;
	bool peer_can_variable_headers;
	bool management_newer;
	bool management_clobber;
	bool management_protect;
	bool receive_32_bit_data;
	enum zmodem_encoded_data_format receive_encoded_data;
	bool use_pack7;
	bool use_mobyturbo;
	bool receive_mobyturbo;
	bool mobyturbo_probe_passed;
	bool want_fcs_32;
	uint8_t input_buffer[ZMODEM_INPUT_CAPACITY];
	size_t input_count;
	size_t input_index;
	unsigned cancel_count;
	int last_sent;
	uint8_t tx_classes[256];
	bool tx_classes_initialized;
	uint8_t tx_data_wire[ZMODEM_TX_DATA_WIRE_CAPACITY];
	bool receive_escaped_control_characters;
};

/*
 * Pointer arguments qualified with restrict designate independent storage for
 * the duration of the call.  In particular, receive destinations and result
 * objects must not overlap one another or protocol state modified by rx_data.
 * Transmit sources must not overlap protocol storage modified while encoding.
 */
int zmodem_init(struct zmodem * restrict,const struct zmodem_io * restrict);
const char * zmodem_result_description(int);
int rx_poll(struct zmodem *);
int rx_purge(struct zmodem *);
int rx_raw(struct zmodem *,int);
int rx_data(struct zmodem * restrict,uint8_t * restrict,size_t,
    size_t * restrict,uint8_t * restrict);
int rx_header_and_check(struct zmodem *,int);
uint32_t zmodem_header_position(const uint8_t *);
void zmodem_set_header_position(uint8_t *,uint32_t);
int tx_data(struct zmodem * restrict,uint8_t,
    const uint8_t * restrict,size_t);
int tx_flush(struct zmodem *);
int tx_header(struct zmodem * restrict,const uint8_t * restrict);
int tx_header_length(struct zmodem * restrict,const uint8_t * restrict,
    size_t);
int tx_hex_header(struct zmodem * restrict,const uint8_t * restrict);
int tx_mobyturbo_probe(struct zmodem *);
int tx_pos_header(struct zmodem *,uint8_t,uint32_t);
int tx_raw(struct zmodem *,int);
int tx_znak(struct zmodem *);
int rx_header(struct zmodem *,int);

#endif

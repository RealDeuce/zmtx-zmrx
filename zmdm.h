/*
 * zmdm.h
 * zmodem primitives prototypes and global data
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

#ifndef _ZMDM_H

#define _ZMDM_H

#ifdef ZMDM
#define EXTERN
#else
#define EXTERN extern
#endif

#include <errno.h>

#define TRUE  1
#define FALSE 0

#define ENDOFFRAME 2
#define FRAMEOK    1
#define TIMEOUT   -1											/* rx routine did not receive a character within timeout */
#define INVHDR    -2											/* invalid header received; but within timeout */
#define INVDATA   -3											/* invalid data subpacket received */
#define ZDLEESC 0x8000											/* one of ZCRCE; ZCRCG; ZCRCQ or ZCRCW was received; ZDLE escaped */

#define HDRLEN     5											/* size of a zmodme header */

EXTERN int in_fp;												/* input file descriptor */
EXTERN int out_fp;												/* output file descriptor */
EXTERN unsigned char rxd_header[ZMAXHLEN];						/* last received header */
EXTERN int rxd_header_len;										/* last received header size */

/*
 * receiver capability flags
 * extracted from the ZRINIT frame as received
 */

EXTERN int can_full_duplex;
EXTERN int can_overlap_io;
EXTERN int can_break;
EXTERN int can_fcs_32;
EXTERN int escape_all_control_characters;						/* guess */
EXTERN int escape_8th_bit;

EXTERN int use_variable_headers;								/* use variable length headers */

/*
 * file management options.
 * only one should be on
 */

EXTERN int management_newer;
EXTERN int management_clobber;
EXTERN int management_protect;

void
fd_init(void);													/* make the io channel raw */

void
fd_exit(void);													/* reset io channel to state before zmtx was called */

void
tx_hheader(unsigned char * buf,int n);

void
tx_bheader(unsigned char * buf,int n);

int
rx_header(int to);												/* receive any header with timeout in milliseconds */

#endif


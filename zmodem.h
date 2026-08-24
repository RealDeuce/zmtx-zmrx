/*
 * zmodem.h
 * zmodem constants
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

#ifndef ZMODEM_H_INCLUDED

#define ZMODEM_H_INCLUDED

#include <stdint.h>

/*
 * ascii constants
 */

#define	SOH			UINT8_C(0x01)
#define	STX			UINT8_C(0x02)
#define	EOT			UINT8_C(0x04)
#define	ENQ			UINT8_C(0x05)
#define	ACK			UINT8_C(0x06)
#define	LF			UINT8_C(0x0a)
#define	CR			UINT8_C(0x0d)
#define	XON			UINT8_C(0x11)
#define	XOFF		UINT8_C(0x13)
#define	NAK			UINT8_C(0x15)
#define	CAN			UINT8_C(0x18)

/*
 * zmodem constants
 */

#define ZMAXHLEN    UINT8_C(0x10)   /* maximum header information length */
#define ZBLOCKLEN   UINT16_C(0x0400) /* standard maximum subpacket length */
#define ZMAXSPLEN   UINT16_C(0x2000) /* ZedZap maximum subpacket length */


#define	ZPAD		UINT8_C(0x2a)	/* pad character; begins frames */
#define	ZDLE		UINT8_C(0x18)	/* ctrl-x zmodem escape */
#define	ZDLEE		UINT8_C(0x58)	/* escaped ZDLE */

#define	ZBIN		UINT8_C(0x41)	/* binary frame indicator (CRC16) */
#define	ZHEX		UINT8_C(0x42)	/* hex frame indicator */
#define	ZBIN32		UINT8_C(0x43)	/* binary frame indicator (CRC32) */
#define	ZBINR32		UINT8_C(0x44)	/* run length encoded binary frame (CRC32) */

#define	ZVBIN		UINT8_C(0x61)	/* binary frame indicator (CRC16) */
#define	ZVHEX		UINT8_C(0x62)	/* hex frame indicator */
#define	ZVBIN32		UINT8_C(0x63)	/* binary frame indicator (CRC32) */
#define	ZVBINR32	UINT8_C(0x64)	/* run length encoded binary frame (CRC32) */

#define	ZRESC		UINT8_C(0x7e)	/* run length encoding flag / escape character */

/*
 * zmodem frame types
 */

#define	ZRQINIT		UINT8_C(0x00)	/* request receive init (s->r) */
#define	ZRINIT		UINT8_C(0x01)	/* receive init (r->s) */
#define	ZSINIT		UINT8_C(0x02)	/* send init sequence (optional) (s->r) */
#define	ZACK		UINT8_C(0x03)	/* ack to ZRQINIT ZRINIT or ZSINIT (s<->r) */
#define	ZFILE		UINT8_C(0x04)	/* file name (s->r) */
#define	ZSKIP		UINT8_C(0x05)	/* skip this file (r->s) */
#define	ZNAK		UINT8_C(0x06)	/* last packet was corrupted (?) */
#define	ZABORT		UINT8_C(0x07)	/* abort batch transfers (?) */
#define	ZFIN		UINT8_C(0x08)	/* finish session (s<->r) */
#define	ZRPOS		UINT8_C(0x09)	/* resume data transmission here (r->s) */
#define	ZDATA		UINT8_C(0x0a)	/* data packet(s) follow (s->r) */
#define	ZEOF		UINT8_C(0x0b)	/* end of file reached (s->r) */
#define	ZFERR		UINT8_C(0x0c)	/* fatal read or write error detected (?) */
#define	ZCRC		UINT8_C(0x0d)	/* request for file CRC and response (?) */
#define	ZCHALLENGE	UINT8_C(0x0e)	/* security challenge (r->s) */
#define	ZCOMPL		UINT8_C(0x0f)	/* request is complete (?) */
#define	ZCAN		UINT8_C(0x10)	/* pseudo frame;
								   other end cancelled session with 5* CAN */
#define	ZFREECNT	UINT8_C(0x11)	/* request free bytes on file system (s->r) */
#define	ZCOMMAND	UINT8_C(0x12)	/* issue command (s->r) */
#define	ZSTDERR		UINT8_C(0x13)	/* output data to stderr (??) */

/*
 * ZDLE sequences
 */

#define	ZCRCE		UINT8_C(0x68)	/* CRC next, frame ends, header packet follows */
#define	ZCRCG		UINT8_C(0x69)	/* CRC next, frame continues nonstop */
#define	ZCRCQ		UINT8_C(0x6a)	/* CRC next, frame continuous, ZACK expected */
#define	ZCRCW		UINT8_C(0x6b)	/* CRC next, frame ends,       ZACK expected */
#define	ZRUB0		UINT8_C(0x6c)	/* translate to rubout 0x7f */
#define	ZRUB1		UINT8_C(0x6d)	/* translate to rubout 0xff */

/*
 * frame specific data.
 * entries are prefixed with their location in the header array.
 */

/*
 * Byte positions within header array
 */

#define FTYPE UINT8_C(0)		/* frame type */

#define ZF0	UINT8_C(4)		/* First flags byte */
#define ZF1	UINT8_C(3)
#define ZF2	UINT8_C(2)
#define ZF3	UINT8_C(1)

#define ZP0	UINT8_C(1)		/* Low order 8 bits of position */
#define ZP1	UINT8_C(2)
#define ZP2	UINT8_C(3)
#define ZP3	UINT8_C(4)		/* High order 8 bits of file position */

/*
 * ZRINIT frame
 * zmodem receiver capability flags
 */

#define	ZF0_CANFDX		UINT8_C(0x01)	/* Receiver can send and receive true full duplex */
#define	ZF0_CANOVIO		UINT8_C(0x02)	/* receiver can receive data during disk I/O */
#define	ZF0_CANBRK		UINT8_C(0x04)	/* receiver can send a break signal */
#define	ZF0_CANCRY		UINT8_C(0x08)	/* Receiver can decrypt DONT USE */
#define	ZF0_CANLZW		UINT8_C(0x10)	/* Receiver can uncompress DONT USE */
#define	ZF0_CANFC32		UINT8_C(0x20)	/* Receiver can use 32 bit Frame Check */
#define	ZF0_ESCCTL		UINT8_C(0x40)	/* Receiver expects ctl chars to be escaped */
#define	ZF0_ESC8		UINT8_C(0x80)	/* Receiver expects 8th bit to be escaped */

#define ZF1_CANVHDR		UINT8_C(0x01)	/* Variable headers OK */

/*
 * ZSINIT frame
 * zmodem sender capability
 */

#define ZF0_TESCCTL 	UINT8_C(0x40)	/* Transmitter expects ctl chars to be escaped */
#define ZF0_TESC8   	UINT8_C(0x80)	/* Transmitter expects 8th bit to be escaped */

#define ZATTNLEN		UINT8_C(0x20)	/* Max length of attention string */
#define ALTCOFF			ZF1		/* Offset to alternate canit string, 0 if not used */

/*
 * ZFILE frame
 */

/*
 * Conversion options one of these in ZF0
 */

#define ZF0_ZCBIN		UINT8_C(1)	/* Binary transfer - inhibit conversion */
#define ZF0_ZCNL		UINT8_C(2)	/* Convert NL to local end of line convention */
#define ZF0_ZCRESUM		UINT8_C(3)	/* Resume interrupted file transfer */

/*
 * Management include options, one of these ored in ZF1
 */

#define ZF1_ZMSKNOLOC	UINT8_C(0x80)	/* Skip file if not present at rx */
#define ZF1_ZMMASK		UINT8_C(0x1f)	/* Mask for the choices below */
#define ZF1_ZMNEWL		UINT8_C(1)	/* Transfer if source newer or longer */
#define ZF1_ZMCRC		UINT8_C(2)	/* Transfer if different file CRC or length */
#define ZF1_ZMAPND		UINT8_C(3)	/* Append contents to existing file (if any) */
#define ZF1_ZMCLOB		UINT8_C(4)	/* Replace existing file */
#define ZF1_ZMNEW		UINT8_C(5)	/* Transfer if source newer */
#define ZF1_ZMDIFF		UINT8_C(6)	/* Transfer if dates or lengths different */
#define ZF1_ZMPROT		UINT8_C(7)	/* Protect destination file */
#define ZF1_ZMCHNG		UINT8_C(8)	/* Change filename if destination exists */

/*
 * Transport options, one of these in ZF2
 */

#define ZF2_ZTNOR		UINT8_C(0)	/* no compression */
#define ZF2_ZTLZW		UINT8_C(1)	/* Lempel-Ziv compression */
#define ZF2_ZTRLE		UINT8_C(3)	/* Run Length encoding */

/*
 * Extended options for ZF3, bit encoded
 */

#define ZF3_ZCANVHDR	UINT8_C(0x01)	/* Variable headers OK */
								/* Receiver window size override */
#define ZF3_ZRWOVR 		UINT8_C(0x04)	/* byte position for receive window override/256 */
#define ZF3_ZXSPARS		UINT8_C(0x40)	/* encoding for sparse file operations */

/*
 * ZCOMMAND frame
 */

#define ZF0_ZCACK1		UINT8_C(0x01)	/* Acknowledge, then do command */

#endif

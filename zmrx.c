/******************************************************************************/
/* Project : Unite!       File : zmodem receive        Version : 1.02         */
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

#include "plat.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "version.h"

#include "zmodem.h"
#include "zmdm.h"
#include "zmodem_plat.h"

#define MAX_RETRIES 10
#define EXIT_TRANSFER_FAILED 4
#define EXIT_CLEANUP_FAILED 5

enum receive_result {
	RECEIVE_FAILED = -1,
	RECEIVE_RETRY = 0,
	RECEIVE_SKIPPED = 1,
	RECEIVE_SUCCEEDED = 2
};

static struct zmodem protocol;
static struct zmodem_plat_io plat_io;

static FILE * fp = NULL;				/* fp of file being received or NULL */
static time_t mdate;					/* file date of file being received */
static bool mdate_known;
static char filename[0x80];				/* filename of file being received */
static char * name;					/* pointer to the part of the filename used in the actual open */

static bool opt_v = false;				/* show progress output */
static bool opt_d = false;				/* show debug output */
static bool opt_q = false;
static bool opt_s = ZMODEM_PLAT_DEFAULT_NONSTREAMING;
static bool opt_m = false;
static bool opt_M = false;
static bool opt_escape_control = false;
static bool opt_escape_8th_bit = ZMODEM_PLAT_DEFAULT_ESCAPE_8TH_BIT;
static bool opt_pack7 = ZMODEM_PLAT_DEFAULT_PACK7;
static bool junk_pathnames = ZMODEM_PLAT_DEFAULT_JUNK_PATHNAMES;	/* junk incoming path names or keep them */
static uint8_t rx_data_subpacket[ZMAXSPLEN];
static uint8_t attention_sequence[ZATTNLEN];

static uintmax_t current_file_size;
static bool current_file_size_known;
static ZMODEM_PLAT_TIMESPEC transfer_start;
static bool transfer_clock_started;
static bool receive_error_reported;
static bool current_mobyturbo;
static bool extended_zrpos_sent;

static void
report_receiver_errno(const char * operation,const char * name,int error)
{
	if (!opt_q) {
		(void)fprintf(stderr,"zmrx: %s %s: %s\n",operation,name,
		    ZMODEM_PLAT_STRERROR(error));
	}
	receive_error_reported = true;
}

static void
report_receiver_protocol(const char * operation,int result)
{
	if (!opt_q) {
		(void)fprintf(stderr,"zmrx: %s: %s\n",operation,
		    zmodem_result_description(result));
	}
	receive_error_reported = true;
}

static void
report_receiver_file(const char * operation,const char * name)
{
	if (!opt_q) {
		(void)fprintf(stderr,"zmrx: %s: %s\n",operation,name);
	}
	receive_error_reported = true;
}

static uintmax_t
elapsed_seconds(void)
{
	ZMODEM_PLAT_TIMESPEC now;
	time_t seconds;

	if (!transfer_clock_started) {
		return UINTMAX_C(1);
	}
	if (ZMODEM_PLAT_CLOCK_GETTIME(ZMODEM_PLAT_CLOCK_MONOTONIC,&now) != 0) {
		return UINTMAX_C(1);
	}
	seconds = now.tv_sec - transfer_start.tv_sec;
	if (now.tv_nsec < transfer_start.tv_nsec) {
		if (seconds > 0) {
			seconds -= 1;
		}
	}
	if (seconds == 0) {
		return UINTMAX_C(1);
	}
	return (uintmax_t)seconds;
}

/* 
 * show the progress of the transfer like this:
 * zmrx: receiving file "garbage" 4096 bytes ( 20%)
 * avoids the use of floating point.
 */

static void
show_progress(char * name,FILE * fp)

{
	int percentage;
	uintmax_t duration;
	intmax_t cps;
	ZMODEM_PLAT_OFF_T position;

	position = ZMODEM_PLAT_FTELLO(fp);

	percentage = 100;
	if (current_file_size_known) {
		if (current_file_size != 0U) {
			if (position >= 0) {
				if ((uintmax_t)position < current_file_size) {
					percentage = (int)(((uintmax_t)position *
					    UINTMAX_C(100)) / current_file_size);
				}
			}
		}
	}

	duration = elapsed_seconds();
	cps = position < 0 ? 0 : (intmax_t)((uintmax_t)position / duration);

	(void)fprintf(stderr,"receiving file \"%s\" %8" PRIdMAX " bytes (%3d %%/%5" PRIdMAX " cps)\r",
		name,(intmax_t)position,percentage,cps);
}

static bool
file_position(FILE * file,uint32_t * position)

{
	ZMODEM_PLAT_OFF_T offset;

	if (file == NULL) {
		return false;
	}
	offset = ZMODEM_PLAT_FTELLO(file);
	if (offset < 0) {
		return false;
	}
#if UINTMAX_MAX > UINT32_MAX
	if ((uintmax_t)offset > UINT32_MAX) {
		return false;
	}
#endif

	*position = (uint32_t)offset;
	return true;
}

static bool
terminal_receive_result(int result)

{
	return result <= ZMODEM_CANCELLED;
}

static int
tx_zrpos(uint32_t position)

{
	uint8_t header[ZMODEM90_ZRPOS_HEADER_LEN] = {
		ZRPOS, 0, 0, 0, 0, 0, 0, 0
	};
	uint8_t flags = 0U;
	bool request_mobyturbo = false;
	bool request_pack7 = false;
	int result;

	if (!extended_zrpos_sent) {
		request_mobyturbo = current_mobyturbo;
		if (opt_pack7) {
			request_pack7 = protocol.peer_can_variable_headers;
		}
	}
	zmodem_set_header_position(header,position);
	if (request_mobyturbo) {
		flags |= ZMODEM90_REQUEST_MOBYTURBO;
	}
	if (request_pack7) {
		flags |= ZMODEM90_REQUEST_PACK7;
	}
	if (flags == 0U) {
		return tx_header(&protocol,header);
	}
	header[ZMODEM90_ZRPOS_FLAGS] = flags;
	result = tx_header_length(&protocol,header,sizeof(header));
	if (result == 0) {
		extended_zrpos_sent = true;
	}
	return result;
}

/*
 * receive a header and check for garbage
 */

/*
 * receive file data until the end of the file or until something goes wrong.
 * the name is only used to show progress
 */

static int
receive_file_data(char * name,FILE * fp)

{
	unsigned errors = 0;
	uint8_t frame_end;
	uint32_t pos;
	size_t n;
	int type;

	if (!file_position(fp,&pos)) {
		int error = errno;

		(void)tx_pos_header(&protocol,ZFERR,UINT32_C(0));
		report_receiver_errno("can't determine file position for",name,error);
		return ZFERR;
	}
	if (tx_zrpos(pos) != 0) {
		report_receiver_protocol("can't request file data",
		    ZMODEM_IO_ERROR);
		return ZFERR;
	}

	for (;;) {
		type = rx_header(&protocol,10000);
		if (type == TIMEOUT) {
			errors += 1U;
			if (errors >= MAX_RETRIES) {
				return TIMEOUT;
			}
			if (tx_zrpos(pos) != 0) {
				report_receiver_protocol("can't retry file data",
				    ZMODEM_IO_ERROR);
				return TIMEOUT;
			}
			continue;
		}
		if (type == ZEOF) {
			if (zmodem_header_position(protocol.rxd_header) == pos) {
				return ZEOF;
			}
			/* A mismatched ZEOF is ignored; a later timeout resynchronizes. */
			continue;
		}
		if (type == ZFILE) {
			int data_result = rx_data(&protocol,rx_data_subpacket,
			    sizeof(rx_data_subpacket),&n,&frame_end);

			if (terminal_receive_result(data_result)) {
				return data_result;
			}
			if (tx_zrpos(pos) != 0) {
				report_receiver_protocol("can't resume file data",
				    ZMODEM_IO_ERROR);
				return ZFERR;
			}
			continue;
		}
		if (type != ZDATA) {
			return type;
		}
		if (zmodem_header_position(protocol.rxd_header) != pos) {
			if (rx_purge(&protocol) != ZMODEM_OK) {
				report_receiver_protocol("can't purge transfer input",
				    ZMODEM_IO_ERROR);
				return ZMODEM_IO_ERROR;
			}
			errors += 1U;
			if (errors >= MAX_RETRIES) {
				return INVDATA;
			}
			if (tx_zrpos(pos) != 0) {
				report_receiver_protocol("can't request corrected file position",
				    ZMODEM_IO_ERROR);
				return INVDATA;
			}
			continue;
		}

			do {
				bool send_acknowledgement;
				uint32_t new_pos;

			type = rx_data(&protocol,rx_data_subpacket,
			    sizeof(rx_data_subpacket),&n,&frame_end);
			if (terminal_receive_result(type)) {
				return type;
			}
			if (type != FRAMEOK) {
				if (type != ENDOFFRAME) {
					if (rx_purge(&protocol) != ZMODEM_OK) {
						report_receiver_protocol(
						    "can't purge invalid file data",
						    ZMODEM_IO_ERROR);
						return ZMODEM_IO_ERROR;
					}
					errors += 1U;
					if (errors >= MAX_RETRIES) {
						return type;
					}
					if (tx_zrpos(pos) != 0) {
						report_receiver_protocol(
						    "can't request retransmission",
						    ZMODEM_IO_ERROR);
						return type;
					}
					break;
				}
			}
#if SIZE_MAX > UINT32_MAX
			if (n > UINT32_MAX) {
				(void)tx_pos_header(&protocol,ZFERR,pos);
				report_receiver_protocol("file block exceeds ZMODEM limit",
				    ZMODEM_INVALID_DATA);
				return ZFERR;
			}
#endif
			if (pos + (uint32_t)n < pos) {
				(void)tx_pos_header(&protocol,ZFERR,pos);
				report_receiver_protocol("file position exceeds ZMODEM limit",
				    ZMODEM_INVALID_DATA);
				return ZFERR;
			}
			if (fwrite(rx_data_subpacket,1,n,fp) != n) {
				int error = errno != 0 ? errno : ZMODEM_PLAT_ERROR_IO;

				(void)tx_pos_header(&protocol,ZFERR,pos);
				report_receiver_errno("can't write file",name,error);
				return ZFERR;
			}
			if (!file_position(fp,&new_pos)) {
				int error = errno;

				(void)tx_pos_header(&protocol,ZFERR,pos);
				report_receiver_errno("can't determine file position for",name,
				    error);
				return ZFERR;
			}
			if (new_pos != pos + (uint32_t)n) {
				(void)tx_pos_header(&protocol,ZFERR,pos);
				report_receiver_file("unexpected local file position",name);
				return ZFERR;
			}
			pos = new_pos;
			errors = 0;
				send_acknowledgement = false;
				if (frame_end == ZCRCQ) {
					send_acknowledgement = true;
				}
				if (frame_end == ZCRCW) {
					send_acknowledgement = true;
				}
				if (send_acknowledgement) {
					if (tx_pos_header(&protocol,ZACK,pos) != 0) {
						report_receiver_protocol("can't acknowledge file data",
						    ZMODEM_IO_ERROR);
						return ZFERR;
				}
			}
			if (opt_v) {
				show_progress(name,fp);
			}
		} while (type == FRAMEOK);
	}
}

static int
tx_zrinit(void)

{
	unsigned receive_buffer_size = ZMODEM_PLAT_RECEIVE_BUFFER_SIZE(&plat_io);
	uint8_t zrinit_header[] = {
		ZRINIT, 0, 0, 0, ZF0_CANBRK | ZF0_CANFDX | ZF0_CANOVIO |
		    ZF0_CANRLE | ZF0_CANFC32
	};

	if (opt_pack7) {
		zrinit_header[ZF1] |= ZF1_CANVHDR;
	}
	if (opt_m) {
		if (!opt_M) {
			zrinit_header[ZF1] |= ZF1_CANVHDR;
		}
	}

	if (opt_s) {
		if (receive_buffer_size == 0U || receive_buffer_size > ZMAXSPLEN) {
			receive_buffer_size = ZMAXSPLEN;
		}
		zrinit_header[ZP0] = (uint8_t)receive_buffer_size;
		zrinit_header[ZP1] = (uint8_t)(receive_buffer_size >> 8);
		zrinit_header[ZF0] &= (uint8_t)~ZF0_CANOVIO;
	}
	if (opt_escape_control) {
		zrinit_header[ZF0] |= ZF0_ESCCTL;
		protocol.receive_escaped_control_characters = true;
	}
	if (opt_escape_8th_bit) {
		zrinit_header[ZF0] |= ZF0_ESC8;
		protocol.receive_escape8_format = ZMODEM_ESCAPE8_LEGACY;
	}

	return tx_hex_header(&protocol,zrinit_header);
}

static int
receive_sender_init(void)

{
	uint8_t frame_end;
	uint8_t flags = protocol.rxd_header[ZF0];
	size_t length;
	int result;

	protocol.escape_all_control_characters =
	    (flags & ZF0_TESCCTL) != 0U;
	protocol.escape_8th_bit = (flags & ZF0_TESC8) != 0U;
	result = rx_data(&protocol,attention_sequence,sizeof(attention_sequence),
	    &length,&frame_end);
	if (terminal_receive_result(result)) {
		return result;
	}
	if ((result != ENDOFFRAME) || (frame_end != ZCRCW) ||
	    (length == 0U) ||
	    (memchr(attention_sequence,'\0',length) !=
	    &attention_sequence[length - 1U])) {
		return tx_znak(&protocol) == 0 ? ZNAK : ZMODEM_IO_ERROR;
	}
	return tx_pos_header(&protocol,ZACK,UINT32_C(0)) == 0 ?
	    ZACK : ZMODEM_IO_ERROR;
}

static bool
parse_mdate(const char * restrict text,time_t * restrict value)

{
	char * end;
	time_t converted;
	uintmax_t wire_value;

	while (*text == ' ') {
		text++;
	}
	if (*text == '-') {
		return false;
	}

	errno = 0;
	wire_value = strtoumax(text,&end,8);
	if (text == end) {
		return false;
	}
	if (errno == ERANGE) {
		return false;
	}
	if (wire_value == 0U) {
		return false;
	}
	if (*end != '\0') {
		if (*end != ' ') {
			return false;
		}
	}

	converted = (time_t)wire_value;
	if (difftime(converted,(time_t)0) < 0) {
		return false;
	}
	if ((uintmax_t)converted != wire_value) {
		return false;
	}

	*value = converted;
	return true;
}


/*
 * receive a file
 * if the file header info packet was garbled then send a ZNAK and return
 * (using ZABORT frame)
 */

static enum receive_result
receive_file(void)

{
	uint32_t position;
	uintmax_t parsed_size;
	ZMODEM_PLAT_STAT_T s;
	FILE * received_file;
	int received_fd = -1;
	int type;
	size_t l;
	size_t filename_length;
	bool clobber = false;
	bool protect = false;
	bool newer = false;
	bool exists = false;
	bool create_exclusively = false;
	uint8_t management;
	ZMODEM_PLAT_UTIMBUF tv;
	const char * mode = "wb";
	char * file_info = (char *)rx_data_subpacket;
	char * metadata;
	char * size_field;
	char * date_field;
	uint8_t frame_end;
	uint8_t * pathname_end;
	bool management_selected = false;

	receive_error_reported = false;
	extended_zrpos_sent = false;
	protocol.peer_can_variable_headers =
	    (protocol.rxd_header[ZF3] & ZF3_ZCANVHDR) != 0U;
	current_mobyturbo = opt_m;
	if (opt_M) {
		current_mobyturbo = false;
	}
	if (opt_escape_8th_bit) {
		current_mobyturbo = false;
	}
	if (!protocol.peer_can_variable_headers) {
		current_mobyturbo = false;
	}
	if ((protocol.rxd_header[ZF3] & ZF3_ZMOBY) == 0U) {
		current_mobyturbo = false;
	}
	if (!protocol.mobyturbo_probe_passed) {
		current_mobyturbo = false;
	}
	if (opt_d) {
		if (opt_m) {
			(void)fprintf(stderr,"zmrx: MobyTurbo %s\n",
			    current_mobyturbo ? "selected" : "not negotiated");
		}
	}

	mdate_known = false;
	current_file_size = 0;
	current_file_size_known = false;

	/*
	 * fetch the management info bits from the ZRFILE header
	 */

	/*
	 * management option
	 */

	management = protocol.rxd_header[ZF1] & ZF1_ZMMASK;
	if (protocol.management_protect) {
		protect = true;
		management_selected = true;
	}
	if (!management_selected) {
		if (management == ZF1_ZMPROT) {
			protect = true;
			management_selected = true;
		}
	}
	if (!management_selected) {
		if (protocol.management_clobber) {
			clobber = true;
			management_selected = true;
		}
	}
	if (!management_selected) {
		if (management == ZF1_ZMCLOB) {
			clobber = true;
			management_selected = true;
		}
	}
	if (!management_selected) {
		if (protocol.management_newer) {
			newer = true;
			management_selected = true;
		}
	}
	if (!management_selected) {
		if (management == ZF1_ZMNEW) {
			newer = true;
		}
	}

	/*
	 * read the data subpacket containing the file information
	 */

	type = rx_data(&protocol,rx_data_subpacket,sizeof(rx_data_subpacket),&l,
	    &frame_end);

	if (terminal_receive_result(type)) {
		report_receiver_protocol("can't receive file information",type);
		return RECEIVE_FAILED;
	}
	if (type != ENDOFFRAME) {
		if (type != TIMEOUT) {
			(void)tx_znak(&protocol);
		}
		return RECEIVE_RETRY;
	}
	if (frame_end != ZCRCW) {
		/*
		 * file info data subpacket was trashed
		 */
		(void)tx_znak(&protocol);
		return RECEIVE_RETRY;
	}

	/*
	 * extract the relevant info from the header.
	 */

	if (l < 2U) {
		(void)tx_znak(&protocol);
		return RECEIVE_RETRY;
	}
	if (rx_data_subpacket[l - 1U] != 0U) {
		(void)tx_znak(&protocol);
		return RECEIVE_RETRY;
	}
	pathname_end = memchr(rx_data_subpacket,0,l - 1);
	if (pathname_end == NULL) {
		(void)tx_znak(&protocol);
		return RECEIVE_RETRY;
	}
	if (pathname_end == rx_data_subpacket) {
		(void)tx_znak(&protocol);
		return RECEIVE_RETRY;
	}
	filename_length = (size_t)(pathname_end - rx_data_subpacket);
	if (filename_length >= sizeof(filename)) {
		(void)tx_pos_header(&protocol,ZSKIP,UINT32_C(0));
		return RECEIVE_SKIPPED;
	}
	memcpy(filename,file_info,filename_length + 1);

	if (junk_pathnames) {
		name = strrchr(filename,'/');
		if (name != NULL) {
			name++;
		}
		else {
			name = filename;
		}
	}
	else {
		name = filename;
	}
	if (*name == '\0') {
		(void)tx_pos_header(&protocol,ZSKIP,UINT32_C(0));
		return RECEIVE_SKIPPED;
	}

	if (opt_v) {
		(void)fprintf(stderr,"receiving file \"%s\"\r",name);
	}

	metadata = (char *)pathname_end + 1;
	size_field = metadata;
	while (*size_field == ' ') {
		size_field++;
	}
	date_field = size_field;
	if (*size_field != '\0') {
		errno = 0;
		parsed_size = strtoumax(size_field,&date_field,10);
		bool valid_size = *size_field != '-';

		if (size_field == date_field) {
			valid_size = false;
		}
		if (errno == ERANGE) {
			valid_size = false;
		}
		if (*date_field != '\0') {
			if (*date_field != ' ') {
				valid_size = false;
			}
		}
		if (valid_size) {
			current_file_size = parsed_size;
			current_file_size_known = true;
		}
	}

	if (*date_field == ' ') {
		mdate_known = parse_mdate(date_field + 1,&mdate);
	}

	/*
	 * decide whether to transfer the file or skip it
	 */

	if (ZMODEM_PLAT_STAT_FILE(name,&s) == 0) {
		exists = true;
	}
	else {
		if (errno != ZMODEM_PLAT_ERROR_NOT_FOUND) {
			int error = errno;

			(void)tx_pos_header(&protocol,ZFERR,UINT32_C(0));
			report_receiver_errno("can't inspect file",name,error);
			return RECEIVE_FAILED;
		}
	}

	/*
	 * if the file already exists here the management options need to
	 * be checked..
	 */
	if (exists) {
		bool recover = mdate_known;

		if (recover) {
			if (mdate != s.st_mtime) {
				recover = false;
			}
		}
		if (recover) {
			if (s.st_size < 0) {
				recover = false;
			}
		}
		if (recover) {
#if UINTMAX_MAX > UINT32_MAX
			if ((uintmax_t)s.st_size > UINT32_MAX) {
				recover = false;
			}
#endif
		}
		if (recover) {
			if (current_file_size_known) {
				if ((uintmax_t)s.st_size > current_file_size) {
					recover = false;
				}
			}
		}
		if (recover) {
			/*
			 * this is crash recovery
			 */
			mode = "ab";
		}
		else {
			/*
		 	 * if the file needs to be protected then exit here.
			 */
			if (protect) {		
				(void)tx_pos_header(&protocol,ZSKIP,UINT32_C(0));
				return RECEIVE_SKIPPED;
			}
			/*
			 * if it is not ok to just overwrite it
			 */
			if (!clobber) {
				/*
				 * if the remote file has to be newer
				 */
				if (newer) {
					if (!mdate_known) {
						(void)tx_pos_header(&protocol,ZSKIP,UINT32_C(0));
						return RECEIVE_SKIPPED;
					}
					if (mdate <= s.st_mtime) {
						(void)tx_pos_header(&protocol,ZSKIP,UINT32_C(0));
						/*
					 	 * and it isnt then exit here.
					 	 */
						return RECEIVE_SKIPPED;
					}
				}
			}
		}
	}
	if (!exists) {
		create_exclusively = protect;
	}

	/*
 	 * transfer the file
	 * either not present; remote newer; ok to clobber or no options set.
	 * (no options->clobber anyway)
	 */

	if (create_exclusively) {
		received_fd = ZMODEM_PLAT_OPEN(name,ZMODEM_PLAT_OPEN_WRITE_ONLY | ZMODEM_PLAT_OPEN_CREATE | ZMODEM_PLAT_OPEN_EXCLUSIVE,(ZMODEM_PLAT_MODE_T)0666);
		if (received_fd >= 0) {
			received_file = ZMODEM_PLAT_FDOPEN(received_fd,mode);
			if (received_file == NULL) {
				int fdopen_error = errno;

				(void)ZMODEM_PLAT_CLOSE(received_fd);
				errno = fdopen_error;
			}
		}
		else {
			received_file = NULL;
		}
	}
	else {
		received_file = fopen(name,mode);
	}
	fp = received_file;

	if (received_file == NULL) {
		int error = errno;

		(void)tx_pos_header(&protocol,ZFERR,UINT32_C(0));
		report_receiver_errno("can't open file",name,error);
		return RECEIVE_FAILED;
	}

	transfer_clock_started =
	    ZMODEM_PLAT_CLOCK_GETTIME(ZMODEM_PLAT_CLOCK_MONOTONIC,&transfer_start) == 0;
	type = receive_file_data(filename,received_file);
	if (type != ZEOF) {
		if (type != ZFERR) {
			(void)tx_pos_header(&protocol,ZFERR,UINT32_C(0));
		}
		(void)fclose(received_file);
		fp = NULL;
		if (!receive_error_reported) {
			report_receiver_protocol("file-data transfer failed",type);
		}
		return RECEIVE_FAILED;
	}
	if (!file_position(received_file,&position)) {
		int error = errno;

		(void)tx_pos_header(&protocol,ZFERR,UINT32_C(0));
		(void)fclose(received_file);
		fp = NULL;
		report_receiver_errno("can't determine file position for",name,error);
		return RECEIVE_FAILED;
	}

	/*
	 * close and exit
	 */

	if (ZMODEM_PLAT_FFLUSH(received_file) != 0) {
		int error = errno;

		(void)tx_pos_header(&protocol,ZFERR,position);
		(void)fclose(received_file);
		fp = NULL;
		report_receiver_errno("can't flush file",name,error);
		return RECEIVE_FAILED;
	}
	if (fclose(received_file) != 0) {
		int error = errno;

		fp = NULL;
		(void)tx_pos_header(&protocol,ZFERR,position);
		report_receiver_errno("can't close file",name,error);
		return RECEIVE_FAILED;
	}
	fp = NULL;

	/*
	 * set the time
	 */

	if (mdate_known) {
		tv.actime = mdate;
		tv.modtime = mdate;

		(void)ZMODEM_PLAT_UTIME(name, &tv);
	}

	/*
	 * and close the input file
	 */

	if (opt_v) {
		(void)fprintf(stderr,"zmrx: received file \"%s\"\n",name);
	}

	return RECEIVE_SUCCEEDED;
}

static int
cleanup(int status)

{
	ZMODEM_PLAT_UTIMBUF tv;

	if (fp) {
		(void)ZMODEM_PLAT_FFLUSH(fp);
		(void)fclose(fp);
		/*
		 * set the time (so crash recovery may work)
		 */

		if (mdate_known) {
			tv.actime = mdate;
			tv.modtime = mdate;

			(void)ZMODEM_PLAT_UTIME(name, &tv);
		}
	}

	if (zmodem_plat_io_close(&plat_io) != 0) {
		(void)fprintf(stderr,"zmrx: transfer line cleanup failed\n");
		if (status == 0) {
			status = EXIT_CLEANUP_FAILED;
		}
	}
	return status;
}


static void
usage(void)

{
	(void)printf("zmrx %s Copyright (c) 1994 Stephen Hurd\n",VERSION);
	(void)printf("usage : zmrx options\n");
	(void)printf("	-j    	    junk pathnames\n");
	(void)printf("	-n          transfer if source is newer\n");
	(void)printf("	-o          overwrite if exists\n");
	(void)printf("	-p          protect (don't overwrite if exists)\n");
	(void)printf("\n");
	(void)printf("	-d          debug output\n");
	(void)printf("	-v          verbose output\n");
	(void)printf("	-q          quiet\n");
	(void)printf("	-s          request non-streaming transfers\n");
	(void)printf("	-e          request control-character escaping\n");
	(void)printf("	-b          request high-bit-byte escaping\n");
	(void)printf("	-7          request Omen Pack-7 (implies -b)\n");
	(void)printf("	-m          request Omen MobyTurbo on transparent links\n");
	(void)printf("	-M          refuse Omen MobyTurbo\n");
	(void)printf("	(only one of -n -o or -p may be specified)\n");
	zmodem_plat_usage(ZMODEM_PLAT_ZMRX);

	exit(cleanup(1));
}

int
main(int argc,char ** argv)

{
	bool transfer_failed = false;
	int i;
	int type;
	size_t argument_index = 1U;
	size_t first_operand;
	struct zmodem_io io;

	zmodem_plat_io_init(&plat_io,ZMODEM_PLAT_STDIN,ZMODEM_PLAT_STDOUT);
	zmodem_plat_io_bind(&io,&plat_io);
	if (zmodem_init(&protocol,&io) != ZMODEM_OK) {
		(void)fprintf(stderr,"zmrx: can't initialize protocol state\n");
		return 2;
	}

	while ((argument_index < (size_t)argc) &&
	    (argv[argument_index][0] == '-')) {
		const char * argument = argv[argument_index];
		size_t option_index;

		for (option_index = 1U; argument[option_index] != '\0';
		    option_index++) {
			enum zmodem_plat_option_result platform_result;
			int raw_option = (unsigned char)argument[option_index];
			int option = toupper((unsigned char)argument[option_index]);

			platform_result = zmodem_plat_parse_option(&plat_io,
			    ZMODEM_PLAT_ZMRX,argument,&option_index);
			if (platform_result == ZMODEM_PLAT_OPTION_INVALID) {
				usage();
			}
			if (platform_result == ZMODEM_PLAT_OPTION_ACCEPTED) {
				continue;
			}
			if (raw_option == 'm') {
				opt_m = true;
				continue;
			}
			if (raw_option == 'M') {
				opt_M = true;
				continue;
			}
			if (raw_option == '7') {
				opt_pack7 = true;
				opt_escape_8th_bit = true;
				continue;
			}

			switch (option) {
				case 'B':
					opt_escape_8th_bit = true;
					break;
				case 'D':
					opt_d = true;
					break;
				case 'V':
					opt_v = true;
					break;
				case 'Q':
					opt_q = true;
					break;
				case 'S':
					opt_s = true;
					break;
				case 'E':
					opt_escape_control = true;
					break;
				case 'N':
					protocol.management_newer = true;
					break;
				case 'O':
					protocol.management_clobber = true;
					break;
				case 'P':
					protocol.management_protect = true;
					break;
				case 'J':
					junk_pathnames = true;
					break;
				default:
					(void)printf("zmrx: bad option %c\n",
					    argument[option_index]);
					usage();
			}
		}
		argument_index += 1U;
	}
	first_operand = argument_index;

	if (opt_d) {
		opt_v = true;
	}

	if (opt_q) {
		opt_v = false;
		opt_d = false;
	}

	if (((unsigned)protocol.management_newer +
	    (unsigned)protocol.management_clobber +
	    (unsigned)protocol.management_protect) > 1U ||
	    first_operand != (size_t)argc) {
		usage();
	}

	i = zmodem_plat_post_parse(&plat_io,ZMODEM_PLAT_ZMRX,argc,argv,
	    first_operand);
	if (i != 0) {
		return cleanup(i);
	}
	protocol.escape_iac = ZMODEM_PLAT_ESCAPE_IAC(&plat_io);
	if (ZMODEM_PLAT_REQUIRES_NONSTREAMING(&plat_io)) {
		opt_s = true;
	}

	/*
	 * establish contact with the sender
	 */

	if (opt_v) {
		(void)fprintf(stderr,"zmrx: establishing contact with sender\n");
	}

	/*
	 * make sure we dont get any old garbage
	 */

	if (rx_purge(&protocol) != ZMODEM_OK) {
		(void)fprintf(stderr,"zmrx: can't purge transfer input\n");
		return cleanup(3);
	}

	/*
	 * loop here until contact is established.
	 * another packet than a ZRQINIT should be received.
	 */

	i = 0;
	do {
		i++;
		if (i > 10) {
			(void)fprintf(stderr,"zmrx: can't establish contact with sender\n");
			exit(cleanup(3));
		}

		if (tx_zrinit() != 0) {
			(void)fprintf(stderr,"zmrx: output error establishing contact\n");
			exit(cleanup(3));
		}
		type = rx_header(&protocol,7000);
	} while (type == TIMEOUT || type == ZRQINIT);
	if (type < 0) {
		report_receiver_protocol("input error establishing contact",type);
		return cleanup(3);
	}

	if (opt_v) {
		(void)fprintf(stderr,"zmrx: contact established\n");
		(void)fprintf(stderr,"zmrx: starting file transfer\n");
	}

	/* 
	 * and receive files
	 * (other packets are acknowledged with a ZCOMPL but ignored.)
	 */

	while (type != ZFIN) {
		bool invite = false;
		unsigned attempts;

		if (transfer_failed) {
			break;
		}
		if (type < 0) {
			report_receiver_protocol("file-session input failed",type);
			transfer_failed = true;
			break;
		}

		if (type == ZSINIT) {
			int result = receive_sender_init();

			if (result < 0) {
				report_receiver_protocol("can't receive sender initialization",
				    result);
				transfer_failed = true;
				break;
			}
		}
		else if (type == ZFILE) {
			enum receive_result result = receive_file();

			if (result == RECEIVE_FAILED) {
				if (!receive_error_reported) {
					report_receiver_file("file transfer failed",filename);
				}
				transfer_failed = true;
				break;
			}
			invite = result == RECEIVE_SUCCEEDED;
		}
		else if (type == ZRQINIT || type == ZEOF) {
			invite = true;
		}
		else if (tx_pos_header(&protocol,ZCOMPL,UINT32_C(0)) != 0) {
			report_receiver_protocol("can't acknowledge session request",
			    ZMODEM_IO_ERROR);
			transfer_failed = true;
			break;
		}

		type = TIMEOUT;
		for (attempts=0;attempts<MAX_RETRIES;attempts++) {
			if (invite && tx_zrinit() != 0) {
				report_receiver_protocol("can't invite next file",
				    ZMODEM_IO_ERROR);
				transfer_failed = true;
				break;
			}
			type = rx_header(&protocol,7000);
			if (type != TIMEOUT) {
				break;
			}
			invite = true;
		}
		if (type == TIMEOUT) {
			report_receiver_protocol("waiting for next file",TIMEOUT);
			transfer_failed = true;
		}
	}

	/*
	 * close the session
	 */

	if (opt_v) {
		(void)fprintf(stderr,"zmrx: closing the session\n");
	}

	{
		uint8_t zfin_header[] = { ZFIN, 0, 0, 0, 0 };

		if (tx_hex_header(&protocol,zfin_header) != 0) {
			if (!transfer_failed) {
				report_receiver_protocol("can't close session",
				    ZMODEM_IO_ERROR);
			}
			transfer_failed = true;
		}
	}

	/*
	 * wait for the over and out sequence
	 */

	{
		int c;
		do {
			c = rx_raw(&protocol,1000);
		} while (c >= 0 && c != 'O');

		if (c == 'O') {
			do {
				c = rx_raw(&protocol,1000);
			} while (c >= 0 && c != 'O');
		}
		if (c != 'O') {
			if (!transfer_failed) {
				report_receiver_protocol("waiting for session completion",c);
			}
			transfer_failed = true;
		}
	}

	if (opt_d) {
		(void)fprintf(stderr,"zmrx: cleanup and exit\n");
	}

	exit(cleanup(transfer_failed ? EXIT_TRANSFER_FAILED : 0));

	return 0;		/* to stop the compiler from complaining */
}

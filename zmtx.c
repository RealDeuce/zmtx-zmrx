/******************************************************************************/
/* Project : Unite!       File : zmodem transmit       Version : 1.02         */
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

enum send_result {
	SEND_FAILED = -1,
	SEND_SKIPPED = 0,
	SEND_SUCCEEDED = 1
};

static struct zmodem protocol;
static struct zmodem_plat_io plat_io;

static bool opt_v = false;				/* show progress output */
static bool opt_d = false;				/* show debug output */
static bool opt_s = ZMODEM_PLAT_DEFAULT_NONSTREAMING;	/* disable streaming */
static bool opt_m = false;				/* offer MobyTurbo */
static bool opt_M = false;				/* refuse MobyTurbo */
static char * window_argument;
static size_t subpacket_size = ZBLOCKLEN;		/* current data subpacket size */
static size_t max_subpacket_size = ZBLOCKLEN;		/* selected maximum data subpacket size */
static uint16_t receiver_buffer_size;
static uint32_t window_size;
static int n_files_remaining;
static uint8_t tx_data_subpacket[ZMAXSPLEN];

static ZMODEM_PLAT_OFF_T current_file_size;
static ZMODEM_PLAT_TIMESPEC transfer_start;
static bool transfer_clock_started;
static bool send_error_reported;

static void
report_sender_errno(const char * operation,const char * name,int error)
{
	(void)fprintf(stderr,"zmtx: %s %s: %s\n",operation,name,
	    ZMODEM_PLAT_STRERROR(error));
	send_error_reported = true;
}

static void
report_sender_protocol(const char * operation,int result)
{
	(void)fprintf(stderr,"zmtx: %s: %s\n",operation,
	    zmodem_result_description(result));
	send_error_reported = true;
}

static void
report_sender_file(const char * operation,const char * name)
{
	(void)fprintf(stderr,"zmtx: %s: %s\n",operation,name);
	send_error_reported = true;
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

static bool
file_position(int file_fd,ZMODEM_PLAT_OFF_T * position)
{
	ZMODEM_PLAT_OFF_T result = ZMODEM_PLAT_LSEEK(file_fd,(ZMODEM_PLAT_OFF_T)0,ZMODEM_PLAT_SEEK_CURRENT);

	if (result < 0) {
		return false;
	}
	*position = result;
	return true;
}

static bool
recovery_position_value(ZMODEM_PLAT_OFF_T result,uint32_t * position)
{
	if (result < 0) {
		return false;
	}
#if UINTMAX_MAX > UINT32_MAX
	if ((uintmax_t)result > UINT32_MAX) {
		return false;
	}
#endif
	*position = (uint32_t)result;
	return true;
}

static bool
recovery_position(const char * name,int file_fd,uint32_t * position)
{
	ZMODEM_PLAT_OFF_T result;

	if (!file_position(file_fd,&result)) {
		report_sender_errno("can't determine recovery position for",name,
		    errno);
		return false;
	}
	if (!recovery_position_value(result,position)) {
		report_sender_file("invalid recovery position",name);
		return false;
	}
	return true;
}

static bool
read_file_block(int file_fd,uint8_t * restrict buffer,size_t capacity,
    size_t * restrict count,bool * restrict end_of_file)
{
	size_t used = 0U;

	*end_of_file = false;
	while (used < capacity) {
		ZMODEM_PLAT_SSIZE_T result;

		for (;;) {
			result = ZMODEM_PLAT_READ(file_fd,&buffer[used],capacity - used);
			if (result >= 0) {
				break;
			}
			if (errno != ZMODEM_PLAT_ERROR_INTERRUPTED) {
				break;
			}
		}
		if (result < 0) {
			return false;
		}
		if (result == 0) {
			*end_of_file = true;
			break;
		}
		used += (size_t)result;
	}
	*count = used;
	return true;
}

static void
parse_zrinit(void)

{
	protocol.can_full_duplex =
	    (protocol.rxd_header[ZF0] & ZF0_CANFDX) != 0;
	protocol.can_overlap_io =
	    (protocol.rxd_header[ZF0] & ZF0_CANOVIO) != 0;
	protocol.can_break = (protocol.rxd_header[ZF0] & ZF0_CANBRK) != 0;
	protocol.can_rle = (protocol.rxd_header[ZF0] & ZF0_CANRLE) != 0;
	protocol.can_fcs_32 = (protocol.rxd_header[ZF0] & ZF0_CANFC32) != 0;
	protocol.escape_all_control_characters =
	    (protocol.rxd_header[ZF0] & ZF0_ESCCTL) != 0;
	protocol.escape_8th_bit =
	    (protocol.rxd_header[ZF0] & ZF0_ESC8) != 0;
	protocol.peer_can_variable_headers =
	    (protocol.rxd_header[ZF1] & ZF1_CANVHDR) != 0;
	receiver_buffer_size = (uint16_t)protocol.rxd_header[ZP0] |
	    (uint16_t)((uint16_t)protocol.rxd_header[ZP1] << 8);
}

static void
select_zrpos_encoding(void)

{
	uint8_t flags = protocol.rxd_header_len > ZMODEM90_ZRPOS_FLAGS ?
	    protocol.rxd_header[ZMODEM90_ZRPOS_FLAGS] : 0U;
	bool mobyturbo_requested =
	    (flags & ZMODEM90_REQUEST_MOBYTURBO) != 0U;
	bool pack7_requested = (flags & ZMODEM90_REQUEST_PACK7) != 0U;

	protocol.use_pack7 = pack7_requested && protocol.escape_8th_bit;
	protocol.use_mobyturbo = mobyturbo_requested && !opt_M &&
	    !protocol.escape_8th_bit;
	if (opt_d && pack7_requested) {
		(void)fprintf(stderr,"zmtx: receiver %s Pack-7\n",
		    protocol.use_pack7 ? "selected" : "requested unavailable");
	}
	if (opt_d && mobyturbo_requested) {
		(void)fprintf(stderr,"zmtx: receiver %s MobyTurbo\n",
		    protocol.use_mobyturbo ? "selected" : "requested unavailable");
	}
}

static void
increase_subpacket_size(void)

{
	if (subpacket_size < max_subpacket_size) {
		subpacket_size *= 2;
		if (subpacket_size > max_subpacket_size) {
			subpacket_size = max_subpacket_size;
		}
	}
}

static void
reduce_subpacket_size(void)

{
	if (subpacket_size == max_subpacket_size) {
		if (max_subpacket_size > ZBLOCKLEN) {
			max_subpacket_size /= 2;
		}
	}
	if (subpacket_size > 128) {
		subpacket_size /= 2;
	}
	if (opt_d) {
		(void)fprintf(stderr,"zmtx: reducing data subpacket size to %zu bytes"
		    " (maximum %zu)\n",subpacket_size,max_subpacket_size);
	}
}

static bool
account_recovery(uint32_t position,uint32_t * furthest_position,
    unsigned * attempts)
{
	if (position > *furthest_position) {
		*furthest_position = position;
		*attempts = 0U;
		return true;
	}
	*attempts += 1U;
	return false;
}

static bool
parse_window_size(const char * restrict text,uint32_t * restrict value)

{
	char * end;
	uintmax_t multiplier = 1;
	uintmax_t parsed;

	if (text == NULL) {
		return false;
	}
	if (*text == '\0') {
		return false;
	}
	if (*text == '-') {
		return false;
	}
	errno = 0;
	parsed = strtoumax(text,&end,10);
	if (text == end) {
		return false;
	}
	if (errno == ERANGE) {
		return false;
	}
	if (*end != '\0') {
		if (end[1] != '\0') {
			return false;
		}
		switch (toupper((unsigned char)*end)) {
			case 'K':
				multiplier = UINTMAX_C(1024);
				break;
			case 'M':
				multiplier = UINTMAX_C(1024) * UINTMAX_C(1024);
				break;
			default:
				return false;
		}
	}
	if (parsed == 0) {
		return false;
	}
	if (parsed > UINT32_MAX / multiplier) {
		return false;
	}
	*value = (uint32_t)(parsed * multiplier);
	return true;
}

static bool
accept_acknowledgement(uint32_t sent_position,uint32_t * acknowledged)

{
	uint32_t position = zmodem_header_position(protocol.rxd_header);

	if (position > sent_position) {
		if (opt_d) {
			(void)fprintf(stderr,"zmtx: invalid acknowledgement position %" PRIu32
			    " beyond sent position %" PRIu32 "\n",position,sent_position);
		}
		return false;
	}
	if (position > *acknowledged) {
		*acknowledged = position;
	}
	return true;
}

/* 
 * show the progress of the transfer like this:
 * zmtx: sending file "garbage" 4096 bytes ( 20%)
 */

static void
show_progress(const char * name,int file_fd)

{
	uintmax_t duration;
	intmax_t cps;
	int percentage;
	ZMODEM_PLAT_OFF_T position;

	if (!file_position(file_fd,&position)) {
		position = (ZMODEM_PLAT_OFF_T)-1;
	}

	percentage = 100;
	if (current_file_size > 0) {
		if (position >= 0) {
			if (position < current_file_size) {
				percentage = (int)(((uintmax_t)position * UINTMAX_C(100)) /
				    (uintmax_t)current_file_size);
			}
		}
	}

	duration = elapsed_seconds();
	cps = position < 0 ? 0 : (intmax_t)((uintmax_t)position / duration);

	(void)fprintf(stderr,"sending file \"%s\" %8" PRIdMAX " bytes (%3d %%/%5" PRIdMAX " cps)\r",
		name,(intmax_t)position,percentage,cps);
}

/*
 * send from the current position in the file
 * all the way to end of file or until something goes wrong.
 * (ZNAK or ZRPOS received)
 * the name is only used to show progress
 * A recovery retransmission ends its first nonempty subpacket with ZCRCW
 * and waits for ZACK, flushing stale data before streaming resumes.
 */

static int
send_from(const char * name,int file_fd,bool synchronize_recovery)

{
	bool frame_open = false;
	bool stop_after_ack;
	bool end_of_file;
	bool window_enabled = window_size != 0 && protocol.can_full_duplex;
	bool wait_each_block = opt_s || !protocol.can_overlap_io ||
	    (window_size != 0 && !protocol.can_full_duplex);
	size_t n;
	size_t read_size;
	size_t segment_sent = 0;
	ZMODEM_PLAT_OFF_T position;
	uint32_t acknowledged_position;
	uint32_t acknowledgement_interval = window_size / 4;
	uint32_t last_ack_request;
	uint8_t zdata_frame[] = { ZDATA, 0, 0, 0, 0 };

	if (!file_position(file_fd,&position)) {
		report_sender_errno("can't determine file position for",name,errno);
		return ZFERR;
	}
	if (position < 0) {
		report_sender_file("invalid file position",name);
		return ZFERR;
	}
#if UINTMAX_MAX > UINT32_MAX
	if ((uintmax_t)position > UINT32_MAX) {
		report_sender_file("file position exceeds ZMODEM limit",name);
		return ZFERR;
	}
#endif
	acknowledged_position = (uint32_t)position;
	last_ack_request = acknowledged_position;
	/*
	 * send the data in the file
	 */

	for (;;) {
		uint8_t frame_end;
		uint32_t wire_position;

		if (!file_position(file_fd,&position)) {
			report_sender_errno("can't determine file position for",name,
			    errno);
			return ZFERR;
		}
		if (position < 0) {
			report_sender_file("invalid file position",name);
			return ZFERR;
		}
#if UINTMAX_MAX > UINT32_MAX
		if ((uintmax_t)position > UINT32_MAX) {
			report_sender_file("file position exceeds ZMODEM limit",name);
			return ZFERR;
		}
#endif
		wire_position = (uint32_t)position;
		if (window_enabled) {
			while (wire_position - acknowledged_position >= window_size) {
				int type = rx_header(&protocol,10000);

				if (type == ZACK) {
					if (!accept_acknowledgement(wire_position,
					    &acknowledged_position)) {
						return ZNAK;
					}
					continue;
				}
				return type;
			}
		}
		if (!frame_open) {
			zmodem_set_header_position(zdata_frame,wire_position);
			if (tx_header(&protocol,zdata_frame) != 0) {
				report_sender_protocol("can't send file-data header",
				    ZMODEM_IO_ERROR);
				return ZFERR;
			}
			frame_open = true;
		}

		if (opt_v) {
			show_progress(name,file_fd);
		}

		/*
		 * read a block from the file
		 */
		read_size = subpacket_size;
		if (window_enabled) {
			if (read_size >
			    window_size - (wire_position - acknowledged_position)) {
				read_size = window_size -
				    (wire_position - acknowledged_position);
			}
		}
		if (receiver_buffer_size != 0U) {
			if (read_size > (size_t)receiver_buffer_size - segment_sent) {
				read_size = (size_t)receiver_buffer_size - segment_sent;
			}
		}
		if (!read_file_block(file_fd,tx_data_subpacket,read_size,&n,
		    &end_of_file)) {
			int error = errno;

			(void)tx_pos_header(&protocol,ZFERR,wire_position);
			report_sender_errno("can't read file",name,error);
			return ZFERR;
		}
#if SIZE_MAX > UINT32_MAX
		if (n > UINT32_MAX) {
			(void)tx_pos_header(&protocol,ZFERR,wire_position);
			report_sender_file("file block exceeds ZMODEM limit",name);
			return ZFERR;
		}
#endif
		if (wire_position + (uint32_t)n < wire_position) {
			(void)tx_pos_header(&protocol,ZFERR,wire_position);
			report_sender_file("file position exceeds ZMODEM limit",name);
			return ZFERR;
		}
		position += (ZMODEM_PLAT_OFF_T)n;
		stop_after_ack = (wait_each_block || synchronize_recovery) &&
		    end_of_file;
		if ((n == 0U) || (!synchronize_recovery && !wait_each_block &&
		    end_of_file)) {
			frame_end = ZCRCE;
		}
		else if (synchronize_recovery || wait_each_block ||
		    ((receiver_buffer_size != 0U) &&
		    (segment_sent + n == receiver_buffer_size))) {
			frame_end = ZCRCW;
		}
		else if (window_enabled &&
		    (uint32_t)position - last_ack_request >= acknowledgement_interval) {
			frame_end = ZCRCQ;
		}
		else {
			frame_end = ZCRCG;
		}
		if (tx_data(&protocol,frame_end,tx_data_subpacket,n) != 0) {
			report_sender_protocol("can't send file data",
			    ZMODEM_IO_ERROR);
			return ZFERR;
		}
		if (frame_end == ZCRCE) {
			current_file_size = position;
			if (opt_d) {
				(void)fprintf(stderr,"end of file\n");
			}
			return ZACK;
		}
		segment_sent += n;

		if (frame_end == ZCRCW) {
			int type;

			for (;;) {
				type = rx_header(&protocol,10000);
				if (type != ZACK) {
					break;
				}
				if (zmodem_header_position(protocol.rxd_header) ==
				    (uint32_t)position) {
					break;
				}
			}
			if (type != ZACK) {
				return type;
			}
			acknowledged_position = (uint32_t)position;
			last_ack_request = acknowledged_position;
			frame_open = false;
			segment_sent = 0;
			synchronize_recovery = false;
			increase_subpacket_size();
			if (stop_after_ack) {
				current_file_size = position;
				return ZACK;
			}
			continue;
		}
		if (frame_end == ZCRCQ) {
			last_ack_request = (uint32_t)position;
		}

		/* 
		 * characters from the other side
		 * check out that header
		 */

		for (;;) {
			int type;
			int c;
			int ready = rx_poll(&protocol);

			if (ready < 0) {
				return ready;
			}
			if (ready == 0) {
				break;
			}
			c = rx_raw(&protocol,1000);
			if (c < 0) {
				return c;
			}
			if ((c & 0x7f) == ZPAD) {
				type = rx_header(&protocol,1000);
				if (type == ZACK) {
					if (!accept_acknowledgement((uint32_t)position,
					    &acknowledged_position)) {
						return ZNAK;
					}
				}
				else if (type != TIMEOUT) {
					return type;
				}
			}
		}
		increase_subpacket_size();
	}
}

/*
 * send a file; returns true when session is aborted.
 * (using ZABORT frame)
 */

static bool
seek_sender(int file_fd,uint32_t position)

{
	ZMODEM_PLAT_OFF_T offset = (ZMODEM_PLAT_OFF_T)position;
	ZMODEM_PLAT_STAT_T s;

	if (offset < 0) {
		return false;
	}
	if ((uintmax_t)offset != position) {
		return false;
	}
	if (ZMODEM_PLAT_FSTAT(file_fd,&s) != 0) {
		return false;
	}
	if (s.st_size < 0) {
		return false;
	}
	if ((uintmax_t)position > (uintmax_t)s.st_size) {
		return false;
	}
	return ZMODEM_PLAT_LSEEK(file_fd,offset,ZMODEM_PLAT_SEEK_START) == offset;
}

static enum send_result
send_file(const char * name)

{
	unsigned attempts;
	uint32_t pos;
	uint32_t furthest_attempted_position;
	uint32_t furthest_receiver_position;
	uint32_t size;
	ZMODEM_PLAT_STAT_T s;
	int file_fd;
	uintmax_t wire_mdate;
	char * p;
	size_t remaining;
	uint8_t zfile_frame[] = { ZFILE, 0, 0, 0, 0 };
	uint8_t zeof_frame[] = { ZEOF, 0, 0, 0, 0 };
	bool synchronize_recovery = false;
	int type;
	int written;
	const char * n;

	send_error_reported = false;

	if (opt_v) {
		(void)fprintf(stderr,"zmtx: sending file \"%s\"\r",name);
	}

	/*
	 * before doing a lot of unnecessary work check if the file exists
	 */

	file_fd = ZMODEM_PLAT_OPEN(name,ZMODEM_PLAT_OPEN_READ_ONLY,
	    (ZMODEM_PLAT_MODE_T)0);

	if (file_fd < 0) {
		report_sender_errno("can't open file",name,errno);
		return SEND_FAILED;
	}

	if (ZMODEM_PLAT_FSTAT(file_fd,&s) != 0) {
		int error = errno;

		report_sender_errno("can't stat file",name,error);
		(void)ZMODEM_PLAT_CLOSE(file_fd);
		return SEND_FAILED;
	}
	if (s.st_size < 0) {
		report_sender_file("file is too large for ZMODEM",name);
		(void)ZMODEM_PLAT_CLOSE(file_fd);
		return SEND_FAILED;
	}
#if UINTMAX_MAX > UINT32_MAX
	if ((uintmax_t)s.st_size > UINT32_MAX) {
		report_sender_file("file is too large for ZMODEM",name);
		(void)ZMODEM_PLAT_CLOSE(file_fd);
		return SEND_FAILED;
	}
#endif
	size = (uint32_t)s.st_size;
	current_file_size = s.st_size;
	if (difftime(s.st_mtime,(time_t)0) < 0) {
		wire_mdate = 0;
	}
	else {
		wire_mdate = (uintmax_t)s.st_mtime;
	}

	/*
	 * the file exists. now build the ZFILE frame
	 */

	/*
	 * set conversion option
	 * (not used; always binary)
	 */

	zfile_frame[ZF0] = ZF0_ZCBIN;

	/*
	 * management option
	 */

	if (protocol.management_protect) {
		zfile_frame[ZF1] = ZF1_ZMPROT;		
		if (opt_d) {
			(void)fprintf(stderr,"zmtx: protecting destination\n");
		}
	}

	if (protocol.management_clobber) {
		zfile_frame[ZF1] = ZF1_ZMCLOB;
		if (opt_d) {
			(void)fprintf(stderr,"zmtx: overwriting destination\n");
		}
	}

	if (protocol.management_newer) {
		zfile_frame[ZF1] = ZF1_ZMNEW;
		if (opt_d) {
			(void)fprintf(stderr,"zmtx: overwriting destination if newer\n");
		}
	}

	/*
	 * transport options
	 * (just plain normal transfer)
	 */

	zfile_frame[ZF2] = ZF2_ZTNOR;

	/*
	 * extended options
	 */

	zfile_frame[ZF3] = ZF3_ZCANVHDR;
	if (opt_m && !opt_M) {
		zfile_frame[ZF3] |= ZF3_ZMOBY;
	}

	/*
 	 * now build the data subpacket with the file name and lots of other
	 * useful information.
	 */

	/*
	 * first enter the name and a 0
	 */

	p = (char *)tx_data_subpacket;

	/*
	 * strip the path name from the filename
	 */

	n = strrchr(name,'/');
	if (n == NULL) {
		n = name;
	}
	else {
		n++;
	}

	if (strlen(n) >= sizeof(tx_data_subpacket)) {
		report_sender_file("file name is too long",name);
		(void)ZMODEM_PLAT_CLOSE(file_fd);
		return SEND_FAILED;
	}
	memcpy(p,n,strlen(n) + 1);
	p += strlen(n) + 1;
	remaining = sizeof(tx_data_subpacket) - (size_t)(p - (char *)tx_data_subpacket);
	written = snprintf(p,remaining,"%" PRIu32 " %" PRIoMAX " 0 0 %d 0",
		size,wire_mdate,n_files_remaining);
	if (written < 0) {
		report_sender_file("can't encode file metadata",name);
		(void)ZMODEM_PLAT_CLOSE(file_fd);
		return SEND_FAILED;
	}
	if ((size_t)written >= remaining) {
		report_sender_file("file metadata is too long",name);
		(void)ZMODEM_PLAT_CLOSE(file_fd);
		return SEND_FAILED;
	}
	p += (size_t)written + 1;

	type = TIMEOUT;
	protocol.use_mobyturbo = false;
	protocol.use_pack7 = false;
	for (attempts=0;attempts<MAX_RETRIES;attempts++) {
		bool stale_zrinit = false;

		/*
	 	 * send the header and the data
	 	 */

		if (tx_mobyturbo_probe(&protocol) != 0) {
			report_sender_protocol("can't send file header",
			    ZMODEM_IO_ERROR);
			(void)ZMODEM_PLAT_CLOSE(file_fd);
			return SEND_FAILED;
		}
		if (tx_header(&protocol,zfile_frame) != 0) {
			report_sender_protocol("can't send file header",
			    ZMODEM_IO_ERROR);
			(void)ZMODEM_PLAT_CLOSE(file_fd);
			return SEND_FAILED;
		}
		if (tx_data(&protocol,ZCRCW,tx_data_subpacket,
		    (size_t)(p - (char *)tx_data_subpacket)) != 0) {
			report_sender_protocol("can't send file information",
			    ZMODEM_IO_ERROR);
			(void)ZMODEM_PLAT_CLOSE(file_fd);
			return SEND_FAILED;
		}
	
		/*
		 * wait for anything but an ZACK packet
		 */

		for (;;) {
			type = rx_header(&protocol,10000);
			if (type == ZACK) {
				continue;
			}
			if (type == ZRINIT && !stale_zrinit) {
				parse_zrinit();
				stale_zrinit = true;
				continue;
			}
			break;
		}

		if (opt_d) {
			(void)fprintf(stderr,"type : %d\n",type);
		}

		if (type == ZSKIP) {
			(void)ZMODEM_PLAT_CLOSE(file_fd);
			if (opt_v) {
				(void)fprintf(stderr,"zmtx: skipped file \"%s\"                       \n",name);
			}
			return SEND_SKIPPED;
		}
		if (type == ZRPOS) {
			select_zrpos_encoding();
			break;
		}
	}
	if (type != ZRPOS) {
		report_sender_protocol("can't start file transfer",type);
		(void)ZMODEM_PLAT_CLOSE(file_fd);
		return SEND_FAILED;
	}

	transfer_clock_started =
	    ZMODEM_PLAT_CLOCK_GETTIME(ZMODEM_PLAT_CLOCK_MONOTONIC,&transfer_start) == 0;
	pos = zmodem_header_position(protocol.rxd_header);
	/* Keep authoritative receiver progress separate from speculative output. */
	furthest_attempted_position = pos;
	furthest_receiver_position = pos;
	attempts = 0U;

	/* Only recovery requests without net progress consume the retry budget. */
	while (attempts < MAX_RETRIES) {
		unsigned finish_attempts;
		bool resume = false;

		if (!seek_sender(file_fd,pos)) {
			(void)tx_pos_header(&protocol,ZFERR,pos);
			report_sender_file("can't seek file to requested position",name);
			(void)ZMODEM_PLAT_CLOSE(file_fd);
			return SEND_FAILED;
		}
		type = send_from(n,file_fd,synchronize_recovery);
		synchronize_recovery = false;
		if (type == ZSKIP) {
			(void)ZMODEM_PLAT_CLOSE(file_fd);
			return SEND_SKIPPED;
		}
		if (type == ZRPOS) {
			uint32_t requested =
			    zmodem_header_position(protocol.rxd_header);

			if (account_recovery(requested,&furthest_receiver_position,
			    &attempts)) {
				/* Start a new attempted-progress epoch. */
				furthest_attempted_position = requested;
			}
			pos = requested;
			synchronize_recovery = true;
			reduce_subpacket_size();
			continue;
		}
		if (type == ZNAK) {
			uint32_t observed;

			if (!recovery_position(name,file_fd,&observed)) {
				(void)ZMODEM_PLAT_CLOSE(file_fd);
				return SEND_FAILED;
			}
			(void)account_recovery(observed,&furthest_attempted_position,
			    &attempts);
			synchronize_recovery = true;
			reduce_subpacket_size();
			continue;
		}
		if (type == TIMEOUT) {
			uint32_t observed;

			if (!recovery_position(name,file_fd,&observed)) {
				(void)ZMODEM_PLAT_CLOSE(file_fd);
				return SEND_FAILED;
			}
			(void)account_recovery(observed,&furthest_attempted_position,
			    &attempts);
			synchronize_recovery = true;
			reduce_subpacket_size();
			continue;
		}
		if (type != ZACK) {
			if (!send_error_reported) {
				report_sender_protocol("file-data transfer failed",type);
			}
			(void)ZMODEM_PLAT_CLOSE(file_fd);
			return SEND_FAILED;
		}

		zmodem_set_header_position(zeof_frame,(uint32_t)current_file_size);
		for (finish_attempts=0;finish_attempts<MAX_RETRIES;finish_attempts++) {
			if (tx_hex_header(&protocol,zeof_frame) != 0) {
				report_sender_protocol("can't send end-of-file header",
				    ZMODEM_IO_ERROR);
				(void)ZMODEM_PLAT_CLOSE(file_fd);
				return SEND_FAILED;
			}
			type = rx_header(&protocol,10000);
			if (type == ZRINIT) {
				parse_zrinit();
				(void)ZMODEM_PLAT_CLOSE(file_fd);
				if (opt_v) {
					(void)fprintf(stderr,"zmtx: sent file \"%s\"                                    \n",name);
				}
				return SEND_SUCCEEDED;
			}
			if (type == ZRPOS) {
				uint32_t requested =
				    zmodem_header_position(protocol.rxd_header);

				if (account_recovery(requested,
				    &furthest_receiver_position,&attempts)) {
					furthest_attempted_position = requested;
				}
				pos = requested;
				synchronize_recovery = true;
				resume = true;
				break;
			}
			if (type == ZSKIP) {
				(void)ZMODEM_PLAT_CLOSE(file_fd);
				return SEND_SKIPPED;
			}
			if (type != ZACK) {
				if (type == TIMEOUT) {
					continue;
				}
				(void)ZMODEM_PLAT_CLOSE(file_fd);
				return SEND_FAILED;
			}
		}
		if (!resume) {
			report_sender_protocol("can't finish file transfer",type);
			(void)ZMODEM_PLAT_CLOSE(file_fd);
			return SEND_FAILED;
		}
	}

	(void)ZMODEM_PLAT_CLOSE(file_fd);
	report_sender_protocol("file transfer retries exhausted",type);
	return SEND_FAILED;
}

static int
cleanup(int status)

{
	if (zmodem_plat_io_close(&plat_io) != 0) {
		(void)fprintf(stderr,"zmtx: transfer line cleanup failed\n");
		if (status == 0) {
			status = EXIT_CLEANUP_FAILED;
		}
	}
	return status;
}

static void
usage(void)

{
	(void)printf("zmtx %s Copyright (c) 1994 Stephen Hurd\n",VERSION);
	(void)printf("usage : zmtx options files\n");
#if !REDUCED_MEMORY
	(void)printf("	-4          use ZedZap 4 KiB data subpackets\n");
	(void)printf("	-8          use ZedZap 8 KiB data subpackets\n");
#endif
	(void)printf("	-s          wait for an acknowledgement after each block\n");
	(void)printf("	-m          offer Omen MobyTurbo on transparent links\n");
	(void)printf("	-M          refuse Omen MobyTurbo\n");
	(void)printf("	-wbytes     limit unacknowledged data (K and M suffixes allowed)\n");
	(void)printf("	-n          transfer if source is newer\n");
	(void)printf("	-o          overwrite if exists\n");
	(void)printf("	-p          protect (don't overwrite if exists)\n");
	(void)printf("\n");
	(void)printf("	-d          debug output\n");
	(void)printf("	-v          verbose output\n");
	(void)printf("	(only one of -n -o or -p may be specified)\n");
	zmodem_plat_usage(ZMODEM_PLAT_ZMTX);

	exit(cleanup(1));
}

int
main(int argc,char ** argv)

{
	bool transfer_failed = false;
	int i;
	size_t argument_index = 1U;
	size_t first_operand;
	size_t file_index;
	struct zmodem_io io;

	zmodem_plat_io_init(&plat_io,ZMODEM_PLAT_STDIN,ZMODEM_PLAT_STDOUT);
	zmodem_plat_io_bind(&io,&plat_io);
	if (zmodem_init(&protocol,&io) != ZMODEM_OK) {
		(void)fprintf(stderr,"zmtx: can't initialize protocol state\n");
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
			    ZMODEM_PLAT_ZMTX,argument,&option_index);
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

			switch (option) {
#if !REDUCED_MEMORY
				case '4':
					max_subpacket_size = 4096U;
					break;
				case '8':
					max_subpacket_size = ZMAXSPLEN;
					break;
#endif
				case 'D':
					opt_d = true;
					break;
				case 'V':
					opt_v = true;
					break;
				case 'S':
					opt_s = true;
					break;
				case 'W':
					window_argument = (char *)&argument[option_index + 1U];
					option_index = strlen(argument) - 1U;
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
				default:
					(void)printf("zmtx: bad option %c\n",
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
	if (window_argument != NULL &&
	    (!parse_window_size(window_argument,&window_size) ||
	    window_size < 4 * max_subpacket_size)) {
		(void)fprintf(stderr,"zmtx: window must hold at least four maximum-size blocks\n");
		usage();
	}
	if (opt_s && window_size != 0) {
		(void)fprintf(stderr,"zmtx: -s and -w cannot be used together\n");
		usage();
	}

	if (((unsigned)protocol.management_newer +
	    (unsigned)protocol.management_clobber +
	    (unsigned)protocol.management_protect) > 1U ||
	    first_operand >= (size_t)argc) {
		usage();
	}

	i = zmodem_plat_post_parse(&plat_io,ZMODEM_PLAT_ZMTX,argc,argv,
	    first_operand);
	if (i != 0) {
		return cleanup(i);
	}
	protocol.escape_iac = ZMODEM_PLAT_ESCAPE_IAC(&plat_io);
	if (ZMODEM_PLAT_REQUIRES_NONSTREAMING(&plat_io)) {
		if (window_size != 0U) {
			(void)fprintf(stderr,
			    "zmtx: selected transport cannot use a transmit window\n");
			usage();
		}
		opt_s = true;
	}

	/*
	 * clear the input queue from any possible garbage
	 * this also clears a possible ZRINIT from an already started
	 * zmodem receiver. this doesn't harm because we reinvite to
	 * receive again below and it may be that the receiver whose
	 * ZRINIT we are about to wipe has already died.
	 */

	if (rx_purge(&protocol) != ZMODEM_OK) {
		(void)fprintf(stderr,"zmtx: can't purge transfer input\n");
		return cleanup(3);
	}

	/*
	 * establish contact with the receiver
	 */

	if (opt_v) {
		(void)fprintf(stderr,"zmtx: establishing contact with receiver\n");
	}

	i = 0;
	do {
		uint8_t zrqinit_header[] = { ZRQINIT, 0, 0, 0, 0 };
		i++;
		if (i > 10) {
			(void)fprintf(stderr,"zmtx: can't establish contact with receiver\n");
			exit(cleanup(3));
		}

		if (tx_raw(&protocol,'z') != 0) {
			(void)fprintf(stderr,"zmtx: output error establishing contact\n");
			exit(cleanup(3));
		}
		if (tx_raw(&protocol,'m') != 0) {
			(void)fprintf(stderr,"zmtx: output error establishing contact\n");
			exit(cleanup(3));
		}
		if (tx_raw(&protocol,CR) != 0) {
			(void)fprintf(stderr,"zmtx: output error establishing contact\n");
			exit(cleanup(3));
		}
		if (tx_hex_header(&protocol,zrqinit_header) != 0) {
			(void)fprintf(stderr,"zmtx: output error establishing contact\n");
			exit(cleanup(3));
		}
	} while (rx_header(&protocol,7000) != ZRINIT);

	if (opt_v) {
		(void)fprintf(stderr,"zmtx: contact established\n");
		(void)fprintf(stderr,"zmtx: starting file transfer\n");
	}

	/*
	 * decode receiver capability flags
	 * forget about encryption and compression.
	 */

	parse_zrinit();
	if (window_size != 0U) {
		if (!protocol.can_full_duplex) {
			if (opt_v) {
				(void)fprintf(stderr,"zmtx: receiver is not full duplex; using one-block acknowledgements\n");
			}
		}
	}

	if (opt_d) {
		(void)fprintf(stderr,"receiver %s full duplex\n"          ,protocol.can_full_duplex               ? "can"      : "can't");
		(void)fprintf(stderr,"receiver %s overlap io\n"           ,protocol.can_overlap_io                ? "can"      : "can't");
		(void)fprintf(stderr,"receiver %s break\n"                ,protocol.can_break                     ? "can"      : "can't");
		(void)fprintf(stderr,"receiver %s decode rle\n"           ,protocol.can_rle                       ? "can"      : "can't");
		(void)fprintf(stderr,"receiver %s fcs 32\n"               ,protocol.can_fcs_32                    ? "can"      : "can't");
		(void)fprintf(stderr,"receiver %s escaped control chars\n",protocol.escape_all_control_characters ? "requests" : "doesn't request");
		(void)fprintf(stderr,"receiver %s escaped 8th bit\n"      ,protocol.escape_8th_bit                ? "requests" : "doesn't request");
		(void)fprintf(stderr,"receiver %s use variable headers\n" ,protocol.peer_can_variable_headers     ? "can"      : "can't");
		(void)fprintf(stderr,"receiver buffer size: %" PRIu16 " bytes\n",receiver_buffer_size);
	}

	/* 
	 * and send each file in turn
	 */

	n_files_remaining = argc - (int)first_operand;
	file_index = first_operand;

	while (file_index < (size_t)argc) {
		enum send_result result = send_file(argv[file_index]);

		if (result == SEND_FAILED) {
			if (!send_error_reported) {
				report_sender_file("file transfer failed",argv[file_index]);
			}
			transfer_failed = true;
			break;
		}

		n_files_remaining--;
		file_index += 1U;
	}

	/*
	 * close the session
	 */

	if (opt_v) {
		(void)fprintf(stderr,"zmtx: closing the session\n");
	}

	{
		unsigned attempts;
		int type;
		uint8_t zfin_header[] = { ZFIN, 0, 0, 0, 0 };

		type = TIMEOUT;
		for (attempts=0;attempts<MAX_RETRIES;attempts++) {
			if (tx_hex_header(&protocol,zfin_header) != 0) {
				if (!transfer_failed) {
					report_sender_protocol("can't close session",
					    ZMODEM_IO_ERROR);
				}
				transfer_failed = true;
				break;
			}
			type = rx_header(&protocol,10000);
			if (type == ZFIN) {
				break;
			}
		}
		
		/*
		 * these Os are formally required; but they don't do a thing
		 * unfortunately many programs require them to exit 
		 * (both programs already sent a ZFIN so why bother ?)
		 */

		if (type == ZFIN) {
			int finish_result;

			finish_result = tx_raw(&protocol,'O');
			if (finish_result == 0) {
				finish_result = tx_raw(&protocol,'O');
			}
			if (finish_result == 0) {
				finish_result = tx_flush(&protocol);
			}
			if (finish_result != 0) {
				if (!transfer_failed) {
					report_sender_protocol("can't finish session",
					    ZMODEM_IO_ERROR);
				}
				transfer_failed = true;
			}
		}
		else {
			if (!transfer_failed) {
				report_sender_protocol("can't close session",type);
			}
			transfer_failed = true;
		}
	}

	/*
	 * c'est fini
	 */

	if (opt_d) {
		(void)fprintf(stderr,"zmtx: cleanup and exit\n");
	}

	exit(cleanup(transfer_failed ? EXIT_TRANSFER_FAILED : 0));

	return 0;
}

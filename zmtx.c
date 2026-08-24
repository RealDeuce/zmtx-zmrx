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

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <inttypes.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "version.h"

#include "zmodem.h"
#include "zmdm.h"
#include "opts.h"

#define MAX_RETRIES 10
#define EXIT_TRANSFER_FAILED 4

enum send_result {
	SEND_FAILED = -1,
	SEND_SKIPPED = 0,
	SEND_SUCCEEDED = 1
};

char * line = NULL;												/* device to use for io */
bool opt_v = false;											/* show progress output */
bool opt_d = false;											/* show debug output */
bool opt_s = false;											/* disable streaming */
char * window_argument;
size_t subpacket_size = ZBLOCKLEN;							/* current data subpacket size */
size_t max_subpacket_size = ZBLOCKLEN;						/* selected maximum data subpacket size */
uint16_t receiver_buffer_size;
uint32_t window_size;
int n_files_remaining;
uint8_t tx_data_subpacket[ZMAXSPLEN];

off_t current_file_size;
time_t transfer_start;

static void
parse_zrinit(void)

{
	can_full_duplex = (rxd_header[ZF0] & ZF0_CANFDX) != 0;
	can_overlap_io = (rxd_header[ZF0] & ZF0_CANOVIO) != 0;
	can_break = (rxd_header[ZF0] & ZF0_CANBRK) != 0;
	can_fcs_32 = (rxd_header[ZF0] & ZF0_CANFC32) != 0;
	escape_all_control_characters = (rxd_header[ZF0] & ZF0_ESCCTL) != 0;
	escape_8th_bit = (rxd_header[ZF0] & ZF0_ESC8) != 0;
	use_variable_headers = (rxd_header[ZF1] & ZF1_CANVHDR) != 0;
	receiver_buffer_size = (uint16_t)rxd_header[ZP0] |
	    (uint16_t)((uint16_t)rxd_header[ZP1] << 8);
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
	if (subpacket_size == max_subpacket_size &&
	    max_subpacket_size > ZBLOCKLEN) {
		max_subpacket_size /= 2;
	}
	if (subpacket_size > 128) {
		subpacket_size /= 2;
	}
	if (opt_d) {
		fprintf(stderr,"zmtx: reducing data subpacket size to %zu bytes"
		    " (maximum %zu)\n",subpacket_size,max_subpacket_size);
	}
}

static bool
parse_window_size(const char * text,uint32_t * value)

{
	char * end;
	uintmax_t multiplier = 1;
	uintmax_t parsed;

	if (text == NULL || *text == '\0' || *text == '-') {
		return false;
	}
	errno = 0;
	parsed = strtoumax(text,&end,10);
	if (text == end || errno == ERANGE) {
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
	if (parsed == 0 || parsed > UINT32_MAX / multiplier) {
		return false;
	}
	*value = (uint32_t)(parsed * multiplier);
	return true;
}

static bool
accept_acknowledgement(uint32_t sent_position,uint32_t * acknowledged)

{
	uint32_t position = zmodem_header_position(rxd_header);

	if (position > sent_position) {
		if (opt_d) {
			fprintf(stderr,"zmtx: invalid acknowledgement position %" PRIu32
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

void
show_progress(char * name,FILE * fp)

{
	double duration;
	intmax_t cps;
	int percentage;
	off_t position;

	position = ftello(fp);

	if (current_file_size > 0 && position >= 0) {
		if (position >= current_file_size) {
			percentage = 100;
		}
		else {
			percentage = (int)(100.0 * (double)position /
				(double)current_file_size);
		}
	}
	else {
		percentage = 100;
	}

	duration = difftime(time(NULL),transfer_start);

	if (duration <= 0.0) {
		duration = 1.0;
	}

	cps = position < 0 ? 0 : (intmax_t)((double)position / duration);

	fprintf(stderr,"sending file \"%s\" %8" PRIdMAX " bytes (%3d %%/%5" PRIdMAX " cps)\r",
		name,(intmax_t)position,percentage,cps);
}

/*
 * send from the current position in the file
 * all the way to end of file or until something goes wrong.
 * (ZNAK or ZRPOS received)
 * the name is only used to show progress
 */

int
send_from(char * name,FILE * fp)

{
	bool frame_open = false;
	bool stop_after_ack;
	bool window_enabled = window_size != 0 && can_full_duplex;
	bool wait_each_block = opt_s || !can_overlap_io ||
	    (window_size != 0 && !can_full_duplex);
	size_t n;
	size_t read_size;
	size_t segment_sent = 0;
	off_t position;
	uint32_t acknowledged_position;
	uint32_t acknowledgement_interval = window_size / 4;
	uint32_t last_ack_request;
	uint8_t zdata_frame[] = { ZDATA, 0, 0, 0, 0 };

	position = ftello(fp);
	if (position < 0 || (uintmax_t)position > UINT32_MAX) {
		return ZFERR;
	}
	acknowledged_position = (uint32_t)position;
	last_ack_request = acknowledged_position;
	/*
	 * send the data in the file
	 */

	for (;;) {
		uint8_t frame_end;
		uint32_t wire_position;

		position = ftello(fp);
		if (position < 0 || (uintmax_t)position > UINT32_MAX) {
			return ZFERR;
		}
		wire_position = (uint32_t)position;
		if (window_enabled) {
			while (wire_position - acknowledged_position >= window_size) {
				int type = rx_header(10000);

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
			if (tx_header(zdata_frame) != 0) {
				return ZFERR;
			}
			frame_open = true;
		}

		if (opt_v) {
			show_progress(name,fp);
		}

		/*
		 * read a block from the file
		 */
		read_size = subpacket_size;
		if (window_enabled &&
		    read_size > window_size - (wire_position - acknowledged_position)) {
			read_size = window_size - (wire_position - acknowledged_position);
		}
		if (receiver_buffer_size != 0 &&
		    read_size > (size_t)receiver_buffer_size - segment_sent) {
			read_size = (size_t)receiver_buffer_size - segment_sent;
		}
		n = fread(tx_data_subpacket,1,read_size,fp);
		if (ferror(fp)) {
			(void)tx_pos_header(ZFERR,wire_position);
			return ZFERR;
		}
		if (n > UINT32_MAX || wire_position > UINT32_MAX - (uint32_t)n) {
			(void)tx_pos_header(ZFERR,wire_position);
			return ZFERR;
		}
		position += (off_t)n;
		stop_after_ack = wait_each_block && n < read_size;
		if (n == 0) {
			frame_end = ZCRCE;
		}
		else if (wait_each_block) {
			frame_end = ZCRCW;
		}
		else if (n < read_size) {
			frame_end = ZCRCE;
		}
		else if (receiver_buffer_size != 0 &&
		    segment_sent + n == receiver_buffer_size) {
			frame_end = ZCRCW;
		}
		else if (window_enabled &&
		    ((uint32_t)position - last_ack_request >= acknowledgement_interval ||
		    (uint32_t)position - acknowledged_position == window_size)) {
			frame_end = ZCRCQ;
		}
		else {
			frame_end = ZCRCG;
		}
		if (tx_data(frame_end,tx_data_subpacket,n) != 0) {
			return ZFERR;
		}
		if (frame_end == ZCRCE) {
			current_file_size = position;
			if (opt_d) {
				fprintf(stderr,"end of file\n");
			}
			return ZACK;
		}
		segment_sent += n;

		if (frame_end == ZCRCW) {
			int type;

			for (;;) {
				type = rx_header(10000);
				if (type != ZACK ||
				    zmodem_header_position(rxd_header) == (uint32_t)position) {
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

		while (rx_poll()) {
			int type;
			int c;
			c = rx_raw(1000);
			if ((c & 0x7f) == ZPAD) {
				type = rx_header(1000);
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
seek_sender(FILE * fp,uint32_t position)

{
	off_t offset = (off_t)position;
	struct stat s;

	if (offset < 0 || (uintmax_t)offset != position ||
	    fstat(fileno(fp),&s) != 0 || s.st_size < 0 ||
	    (uintmax_t)position > (uintmax_t)s.st_size) {
		return false;
	}
	return fseeko(fp,offset,SEEK_SET) == 0;
}

enum send_result
send_file(char * name)

{
	unsigned attempts;
	uint32_t pos;
	uint32_t size;
	struct stat s;
	FILE * fp;
	uintmax_t wire_mdate;
	char * p;
	size_t remaining;
	uint8_t zfile_frame[] = { ZFILE, 0, 0, 0, 0 };
	uint8_t zeof_frame[] = { ZEOF, 0, 0, 0, 0 };
	int type;
	int written;
	char * n;

	if (opt_v) {
		fprintf(stderr,"zmtx: sending file \"%s\"\r",name);
	}

	/*
	 * before doing a lot of unnecessary work check if the file exists
	 */

	fp = fopen(name,"rb");

	if (fp == NULL) {
		if (opt_v) {
			fprintf(stderr,"zmtx: can't open file %s\n",name);
		}
		return SEND_FAILED;
	}

	if (fstat(fileno(fp),&s) != 0) {
		if (opt_v) {
			fprintf(stderr,"zmtx: can't stat file %s\n",name);
		}
		fclose(fp);
		return SEND_FAILED;
	}
	if (s.st_size < 0 || (uintmax_t)s.st_size > UINT32_MAX) {
		if (opt_v) {
			fprintf(stderr,"zmtx: file is too large for ZMODEM: %s\n",name);
		}
		fclose(fp);
		return SEND_FAILED;
	}
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

	if (management_protect) {
		zfile_frame[ZF1] = ZF1_ZMPROT;		
		if (opt_d) {
			fprintf(stderr,"zmtx: protecting destination\n");
		}
	}

	if (management_clobber) {
		zfile_frame[ZF1] = ZF1_ZMCLOB;
		if (opt_d) {
			fprintf(stderr,"zmtx: overwriting destination\n");
		}
	}

	if (management_newer) {
		zfile_frame[ZF1] = ZF1_ZMNEW;
		if (opt_d) {
			fprintf(stderr,"zmtx: overwriting destination if newer\n");
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

	zfile_frame[ZF3] = 0;

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
		fclose(fp);
		return SEND_FAILED;
	}
	memcpy(p,n,strlen(n) + 1);
	p += strlen(n) + 1;
	remaining = sizeof(tx_data_subpacket) - (size_t)(p - (char *)tx_data_subpacket);
	written = snprintf(p,remaining,"%" PRIu32 " %" PRIoMAX " 0 0 %d 0",
		size,wire_mdate,n_files_remaining);
	if (written < 0 || (size_t)written >= remaining) {
		fclose(fp);
		return SEND_FAILED;
	}
	p += (size_t)written + 1;

	type = TIMEOUT;
	for (attempts=0;attempts<MAX_RETRIES;attempts++) {
		bool stale_zrinit = false;

		/*
	 	 * send the header and the data
	 	 */

		if (tx_header(zfile_frame) != 0 ||
		    tx_data(ZCRCW,tx_data_subpacket,
		    (size_t)(p - (char *)tx_data_subpacket)) != 0) {
			fclose(fp);
			return SEND_FAILED;
		}
	
		/*
		 * wait for anything but an ZACK packet
		 */

		for (;;) {
			type = rx_header(10000);
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
			fprintf(stderr,"type : %d\n",type);
		}

		if (type == ZSKIP) {
			fclose(fp);
			if (opt_v) {
				fprintf(stderr,"zmtx: skipped file \"%s\"                       \n",name);
			}
			return SEND_SKIPPED;
		}
		if (type == ZRPOS) {
			break;
		}
	}
	if (type != ZRPOS) {
		fclose(fp);
		return SEND_FAILED;
	}

	transfer_start = time(NULL);
	pos = zmodem_header_position(rxd_header);

	for (attempts=0;attempts<MAX_RETRIES;attempts++) {
		unsigned finish_attempts;
		bool resume = false;

		if (!seek_sender(fp,pos)) {
			(void)tx_pos_header(ZFERR,pos);
			fclose(fp);
			return SEND_FAILED;
		}
		type = send_from(n,fp);
		if (type == ZSKIP) {
			fclose(fp);
			return SEND_SKIPPED;
		}
		if (type == ZRPOS) {
			pos = zmodem_header_position(rxd_header);
			reduce_subpacket_size();
			continue;
		}
		if (type == ZNAK || type == TIMEOUT) {
			reduce_subpacket_size();
			continue;
		}
		if (type != ZACK) {
			fclose(fp);
			return SEND_FAILED;
		}

			zmodem_set_header_position(zeof_frame,(uint32_t)current_file_size);
		for (finish_attempts=0;finish_attempts<MAX_RETRIES;finish_attempts++) {
			if (tx_hex_header(zeof_frame) != 0) {
				fclose(fp);
				return SEND_FAILED;
			}
			type = rx_header(10000);
			if (type == ZRINIT) {
				parse_zrinit();
				fclose(fp);
				if (opt_v) {
					fprintf(stderr,"zmtx: sent file \"%s\"                                    \n",name);
				}
				return SEND_SUCCEEDED;
			}
			if (type == ZRPOS) {
				pos = zmodem_header_position(rxd_header);
				resume = true;
				break;
			}
			if (type == ZSKIP) {
				fclose(fp);
				return SEND_SKIPPED;
			}
			if (type != ZACK && type != TIMEOUT) {
				fclose(fp);
				return SEND_FAILED;
			}
		}
		if (!resume) {
			fclose(fp);
			return SEND_FAILED;
		}
	}

	fclose(fp);
	return SEND_FAILED;
}

void
cleanup(void)

{
	fd_exit();
}

void
usage(void)

{
	printf("zmtx %s Copyright (c) 1994 Stephen Hurd\n",VERSION);
	printf("usage : zmtx options files\n");
	printf("	-lline      line to use for io\n");
	printf("	-4          use ZedZap 4 KiB data subpackets\n");
	printf("	-8          use ZedZap 8 KiB data subpackets\n");
	printf("	-s          wait for an acknowledgement after each block\n");
	printf("	-wbytes     limit unacknowledged data (K and M suffixes allowed)\n");
	printf("	-n          transfer if source is newer\n");
	printf("	-o          overwrite if exists\n");
	printf("	-p          protect (don't overwrite if exists)\n");
	printf("\n");
	printf("	-d          debug output\n");
	printf("	-v          verbose output\n");
	printf("	(only one of -n -c or -p may be specified)\n");

	cleanup();

	exit(1);
}

int
main(int argc,char ** argv)

{
	bool transfer_failed = false;
	int i;
	char * s;

	(void)signal(SIGPIPE,SIG_IGN);

	argv++;
	while (--argc > 0 && ((*argv)[0] == '-')) {
		for (s = argv[0]+1; *s != '\0'; s++) {
			switch (toupper((unsigned char)*s)) {
				case '4':
					max_subpacket_size = 4096;
					break;
				case '8':
					max_subpacket_size = ZMAXSPLEN;
					break;
				OPT_BOOL('D',opt_d);
				OPT_BOOL('V',opt_v);
				OPT_BOOL('S',opt_s);
				OPT_STRING('W',window_argument);

				OPT_BOOL('N',management_newer);
				OPT_BOOL('O',management_clobber);
				OPT_BOOL('P',management_protect);
				OPT_STRING('L',line);
				default:
					printf("zmtx: bad option %c\n",*s);
					usage();
			}
		}
		argv++;
	}

	if (opt_d) {
		opt_v = true;
	}
	if (window_argument != NULL &&
	    (!parse_window_size(window_argument,&window_size) ||
	    window_size < 4 * max_subpacket_size)) {
		fprintf(stderr,"zmtx: window must hold at least four maximum-size blocks\n");
		usage();
	}
	if (opt_s && window_size != 0) {
		fprintf(stderr,"zmtx: -s and -w cannot be used together\n");
		usage();
	}

	if ((management_newer + management_clobber + management_protect) > 1 || argc == 0) {
		usage();
	}

	if (line != NULL) {	
		if (freopen(line,"r",stdin) == NULL) {
			fprintf(stderr,"zmtx can't open line for input %s\n",line);
			exit(2);
		}
		if (freopen(line,"w",stdout) == NULL) {
			fprintf(stderr,"zmtx can't open line for output %s\n",line);
			exit(2);
		}
	}

	/*
	 * set the io device to transparent
	 */

	fd_init();	

	/*
	 * clear the input queue from any possible garbage
	 * this also clears a possible ZRINIT from an already started
	 * zmodem receiver. this doesn't harm because we reinvite to
	 * receive again below and it may be that the receiver whose
	 * ZRINIT we are about to wipe has already died.
	 */

	rx_purge();

	/*
	 * establish contact with the receiver
	 */

	if (opt_v) {
		fprintf(stderr,"zmtx: establishing contact with receiver\n");
	}

	i = 0;
	do {
		uint8_t zrqinit_header[] = { ZRQINIT, 0, 0, 0, 0 };
		i++;
		if (i > 10) {
			fprintf(stderr,"zmtx: can't establish contact with receiver\n");
			cleanup();
			exit(3);
		}

		if (tx_raw('z') != 0 || tx_raw('m') != 0 || tx_raw(CR) != 0 ||
		    tx_hex_header(zrqinit_header) != 0) {
			fprintf(stderr,"zmtx: output error establishing contact\n");
			cleanup();
			exit(3);
		}
	} while (rx_header(7000) != ZRINIT);

	if (opt_v) {
		fprintf(stderr,"zmtx: contact established\n");
		fprintf(stderr,"zmtx: starting file transfer\n");
	}

	/*
	 * decode receiver capability flags
	 * forget about encryption and compression.
	 */

	parse_zrinit();
	if (window_size != 0 && !can_full_duplex && opt_v) {
		fprintf(stderr,"zmtx: receiver is not full duplex; using one-block acknowledgements\n");
	}

	if (opt_d) {
		fprintf(stderr,"receiver %s full duplex\n"          ,can_full_duplex               ? "can"      : "can't");
		fprintf(stderr,"receiver %s overlap io\n"           ,can_overlap_io                ? "can"      : "can't");
		fprintf(stderr,"receiver %s break\n"                ,can_break                     ? "can"      : "can't");
		fprintf(stderr,"receiver %s fcs 32\n"               ,can_fcs_32                    ? "can"      : "can't");
		fprintf(stderr,"receiver %s escaped control chars\n",escape_all_control_characters ? "requests" : "doesn't request");
		fprintf(stderr,"receiver %s escaped 8th bit\n"      ,escape_8th_bit                ? "requests" : "doesn't request");
		fprintf(stderr,"receiver %s use variable headers\n" ,use_variable_headers          ? "can"      : "can't");
		fprintf(stderr,"receiver buffer size: %" PRIu16 " bytes\n",receiver_buffer_size);
	}

	/* 
	 * and send each file in turn
	 */

	n_files_remaining = argc;

	while (argc) {
		enum send_result result = send_file(*argv);

		if (result == SEND_FAILED) {
			if (opt_v) {
				fprintf(stderr,"zmtx: transfer failed.\n");
			}
			transfer_failed = true;
			break;
		}

		n_files_remaining--;
		argc--;
		argv++;
	}

	/*
	 * close the session
	 */

	if (opt_v) {
		fprintf(stderr,"zmtx: closing the session\n");
	}

	{
		unsigned attempts;
		int type;
		uint8_t zfin_header[] = { ZFIN, 0, 0, 0, 0 };

		type = TIMEOUT;
		for (attempts=0;attempts<MAX_RETRIES;attempts++) {
			if (tx_hex_header(zfin_header) != 0) {
				transfer_failed = true;
				break;
			}
			type = rx_header(10000);
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
			if (tx_raw('O') != 0 || tx_raw('O') != 0 || tx_flush() != 0) {
				transfer_failed = true;
			}
		}
		else {
			transfer_failed = true;
		}
	}

	/*
	 * c'est fini
	 */

	if (opt_d) {
		fprintf(stderr,"zmtx: cleanup and exit\n");
	}

	cleanup();

	exit(transfer_failed ? EXIT_TRANSFER_FAILED : 0);

	return 0;
}

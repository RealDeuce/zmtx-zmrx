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

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <utime.h>
#include <unistd.h>
#include "version.h"

#include "zmodem.h"
#include "zmdm.h"
#include "zmdm_posix.h"

#define MAX_RETRIES 10
#define EXIT_TRANSFER_FAILED 4

enum receive_result {
	RECEIVE_FAILED = -1,
	RECEIVE_RETRY = 0,
	RECEIVE_SKIPPED = 1,
	RECEIVE_SUCCEEDED = 2
};

static struct zmodem protocol;
static struct zmodem_posix_io posix_io;

static FILE * fp = NULL;												/* fp of file being received or NULL */
static time_t mdate;													/* file date of file being received */
static bool mdate_known;
static char filename[0x80];											/* filename of file being received */
static char * name;													/* pointer to the part of the filename used in the actual open */

static char * line = NULL;												/* device to use for io */
static bool opt_v = false;												/* show progress output */
static bool opt_d = false;												/* show debug output */
static bool opt_q = false;
static bool opt_s = false;
static bool junk_pathnames = false;										/* junk incoming path names or keep them */
static uint8_t rx_data_subpacket[ZMAXSPLEN];

static uintmax_t current_file_size;
static bool current_file_size_known;
static struct timespec transfer_start;
static bool transfer_clock_started;

static uintmax_t
elapsed_seconds(void)
{
	struct timespec now;
	time_t seconds;

	if (!transfer_clock_started) {
		return UINTMAX_C(1);
	}
	if (clock_gettime(CLOCK_MONOTONIC,&now) != 0) {
		return UINTMAX_C(1);
	}
	seconds = now.tv_sec - transfer_start.tv_sec;
	if (now.tv_nsec < transfer_start.tv_nsec) {
		if (seconds > 0) {
			seconds -= 1;
		}
	}
	if (seconds <= 0) {
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
	off_t position;

	position = ftello(fp);

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
	off_t offset;

	if (file == NULL) {
		return false;
	}
	offset = ftello(file);
	if (offset < 0) {
		return false;
	}
	if ((uintmax_t)offset > UINT32_MAX) {
		return false;
	}

	*position = (uint32_t)offset;
	return true;
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
		(void)tx_pos_header(&protocol,ZFERR,UINT32_C(0));
		return ZFERR;
	}
	if (tx_pos_header(&protocol,ZRPOS,pos) != 0) {
		return ZFERR;
	}

	for (;;) {
		type = rx_header(&protocol,10000);
		if (type == TIMEOUT) {
			errors += 1U;
			if (errors >= MAX_RETRIES) {
				return TIMEOUT;
			}
			if (tx_pos_header(&protocol,ZRPOS,pos) != 0) {
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
			(void)rx_data(&protocol,rx_data_subpacket,
			    sizeof(rx_data_subpacket),&n,&frame_end);
			if (tx_pos_header(&protocol,ZRPOS,pos) != 0) {
				return ZFERR;
			}
			continue;
		}
		if (type != ZDATA) {
			return type;
		}
		if (zmodem_header_position(protocol.rxd_header) != pos) {
			if (rx_purge(&protocol) != ZMODEM_OK) {
				return ZMODEM_IO_ERROR;
			}
			errors += 1U;
			if (errors >= MAX_RETRIES) {
				return INVDATA;
			}
			if (tx_pos_header(&protocol,ZRPOS,pos) != 0) {
				return INVDATA;
			}
			continue;
		}

			do {
				bool send_acknowledgement;
				uint32_t new_pos;

			type = rx_data(&protocol,rx_data_subpacket,
			    sizeof(rx_data_subpacket),&n,&frame_end);
			if (type != FRAMEOK) {
				if (type != ENDOFFRAME) {
					if (rx_purge(&protocol) != ZMODEM_OK) {
						return ZMODEM_IO_ERROR;
					}
					errors += 1U;
					if (errors >= MAX_RETRIES) {
						return type;
					}
					if (tx_pos_header(&protocol,ZRPOS,pos) != 0) {
						return type;
					}
					break;
				}
			}
			if (n > UINT32_MAX) {
				(void)tx_pos_header(&protocol,ZFERR,pos);
				return ZFERR;
			}
			if (pos > UINT32_MAX - (uint32_t)n) {
				(void)tx_pos_header(&protocol,ZFERR,pos);
				return ZFERR;
			}
			if (fwrite(rx_data_subpacket,1,n,fp) != n) {
				(void)tx_pos_header(&protocol,ZFERR,pos);
				return ZFERR;
			}
			if (!file_position(fp,&new_pos)) {
				(void)tx_pos_header(&protocol,ZFERR,pos);
				return ZFERR;
			}
			if (new_pos != pos + (uint32_t)n) {
				(void)tx_pos_header(&protocol,ZFERR,pos);
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
	uint8_t zrinit_header[] = {
		ZRINIT, 0, 0, 0, ZF0_CANBRK | ZF0_CANFDX | ZF0_CANOVIO | ZF0_CANFC32
	};

	if (opt_s) {
		zrinit_header[ZP0] = (uint8_t)ZMAXSPLEN;
		zrinit_header[ZP1] = (uint8_t)(ZMAXSPLEN >> 8);
		zrinit_header[ZF0] &= (uint8_t)~ZF0_CANOVIO;
	}

	return tx_hex_header(&protocol,zrinit_header);
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
	struct stat s;
	FILE * received_file;
	int type;
	size_t l;
	size_t filename_length;
	bool clobber = false;
	bool protect = false;
	bool newer = false;
	bool exists;
	uint8_t management;
	struct utimbuf tv;
	char * mode = "wb";
	char * file_info = (char *)rx_data_subpacket;
	char * metadata;
	char * size_field;
	char * date_field;
	uint8_t frame_end;
	uint8_t * pathname_end;
	bool management_selected = false;

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

	fp = fopen(name,"rb");

	if (fp != NULL) {
		exists = fstat(fileno(fp),&s) == 0;

		(void)fclose(fp);
	}
	else {
		exists = false;
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
			if ((uintmax_t)s.st_size > UINT32_MAX) {
				recover = false;
			}
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

	/*
 	 * transfer the file
	 * either not present; remote newer; ok to clobber or no options set.
	 * (no options->clobber anyway)
	 */

	received_file = fopen(name,mode);
	fp = received_file;

	if (received_file == NULL) {
		(void)tx_pos_header(&protocol,ZFERR,UINT32_C(0));
		if (opt_v) {
			(void)fprintf(stderr,"zmrx: can't open file %s\n",name);
		}
		return RECEIVE_FAILED;
	}

	transfer_clock_started =
	    clock_gettime(CLOCK_MONOTONIC,&transfer_start) == 0;
	type = receive_file_data(filename,received_file);
	if (type != ZEOF) {
		if (type != ZFERR) {
			(void)tx_pos_header(&protocol,ZFERR,UINT32_C(0));
		}
		(void)fclose(received_file);
		fp = NULL;
		return RECEIVE_FAILED;
	}
	if (!file_position(received_file,&position)) {
		(void)tx_pos_header(&protocol,ZFERR,UINT32_C(0));
		(void)fclose(received_file);
		fp = NULL;
		return RECEIVE_FAILED;
	}

	/*
	 * close and exit
	 */

	if (fflush(received_file) != 0) {
		(void)tx_pos_header(&protocol,ZFERR,position);
		(void)fclose(received_file);
		fp = NULL;
		return RECEIVE_FAILED;
	}
	if (fclose(received_file) != 0) {
		fp = NULL;
		(void)tx_pos_header(&protocol,ZFERR,position);
		return RECEIVE_FAILED;
	}
	fp = NULL;

	/*
	 * set the time
	 */

	if (mdate_known) {
		tv.actime = mdate;
		tv.modtime = mdate;

		(void)utime(name, &tv);
	}

	/*
	 * and close the input file
	 */

	if (opt_v) {
		(void)fprintf(stderr,"zmrx: received file \"%s\"\n",name);
	}

	return RECEIVE_SUCCEEDED;
}

static void
cleanup(void)

{
	struct utimbuf tv;

	if (fp) {
		(void)fflush(fp);
		(void)fclose(fp);
		/*
		 * set the time (so crash recovery may work)
		 */

		if (mdate_known) {
			tv.actime = mdate;
			tv.modtime = mdate;

			(void)utime(name, &tv);
		}
	}

	zmodem_posix_io_close(&posix_io);
}


static void
usage(void)

{
	(void)printf("zmrx %s Copyright (c) 1994 Stephen Hurd\n",VERSION);
	(void)printf("usage : zmrx options\n");
	(void)printf("	-lline      line to use for io\n");
	(void)printf("	-j    	    junk pathnames\n");
	(void)printf("	-n          transfer if source is newer\n");
	(void)printf("	-o          overwrite if exists\n");
	(void)printf("	-p          protect (don't overwrite if exists)\n");
	(void)printf("\n");
	(void)printf("	-d          debug output\n");
	(void)printf("	-v          verbose output\n");
	(void)printf("	-q          quiet\n");
	(void)printf("	-s          request non-streaming transfers\n");
	(void)printf("	(only one of -n -c or -p may be specified)\n");

	cleanup();

	exit(1);
}

int
main(int argc,char ** argv)

{
	bool transfer_failed = false;
	int i;
	int type;
	struct zmodem_io io;

	if (zmodem_posix_ignore_sigpipe() != 0) {
		(void)fprintf(stderr,"zmrx: can't configure broken-pipe handling\n");
		return 2;
	}
	zmodem_posix_io_init(&posix_io,STDIN_FILENO,STDOUT_FILENO);
	zmodem_posix_io_bind(&io,&posix_io);
	if (zmodem_init(&protocol,&io) != ZMODEM_OK) {
		(void)fprintf(stderr,"zmrx: can't initialize protocol state\n");
		return 2;
	}

	argv++;
	while (--argc > 0 && ((*argv)[0] == '-')) {
		const char * argument = argv[0];
		size_t option_index;

		for (option_index = 1U; argument[option_index] != '\0';
		    option_index++) {
			int option = toupper((unsigned char)argument[option_index]);

			switch (option) {
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
				case 'L':
					line = (char *)&argument[option_index + 1U];
					option_index = strlen(argument) - 1U;
					break;
				default:
					(void)printf("zmrx: bad option %c\n",
					    argument[option_index]);
					usage();
			}
		}
		argv++;
	}

	if (opt_d) {
		opt_v = true;
	}

	if (opt_q) {
		opt_v = false;
		opt_d = false;
	}

	if (((unsigned)protocol.management_newer +
	    (unsigned)protocol.management_clobber +
	    (unsigned)protocol.management_protect) > 1U || argc != 0) {
		usage();
	}

	if (line != NULL) {
		if (zmodem_posix_io_open(&posix_io,line) != 0) {
			(void)fprintf(stderr,"zmrx can't open line for input/output %s\n",line);
			exit(2);
		}
	}

	/*
	 * set the io device to transparent
	 */

	if (zmodem_posix_io_make_raw(&posix_io) != 0) {
		(void)fprintf(stderr,"zmrx: can't configure transfer line\n");
		cleanup();
		return 2;
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
		cleanup();
		return 3;
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
			cleanup();
			exit(3);
		}

		if (tx_zrinit() != 0) {
			(void)fprintf(stderr,"zmrx: output error establishing contact\n");
			cleanup();
			exit(3);
		}
		type = rx_header(&protocol,7000);
	} while (type == TIMEOUT || type == ZRQINIT);

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

		if (type == ZFILE) {
			enum receive_result result = receive_file();

			if (result == RECEIVE_FAILED) {
				transfer_failed = true;
				break;
			}
			invite = result == RECEIVE_SUCCEEDED;
		}
		else if (type == ZRQINIT || type == ZEOF) {
			invite = true;
		}
		else if (tx_pos_header(&protocol,ZCOMPL,UINT32_C(0)) != 0) {
			transfer_failed = true;
			break;
		}

		type = TIMEOUT;
		for (attempts=0;attempts<MAX_RETRIES;attempts++) {
			if (invite && tx_zrinit() != 0) {
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
		} while (c != 'O' && c != TIMEOUT);

		if (c != TIMEOUT) {
			do {
				c = rx_raw(&protocol,1000);
			} while (c != 'O' && c != TIMEOUT);
		}
		if (c == TIMEOUT) {
			transfer_failed = true;
		}
	}

	if (opt_d) {
		(void)fprintf(stderr,"zmrx: cleanup and exit\n");
	}

	cleanup();

	exit(transfer_failed ? EXIT_TRANSFER_FAILED : 0);

	return 0;		/* to stop the compiler from complaining */
}

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
#include "version.h"

#include "zmodem.h"
#include "zmdm.h"
#include "opts.h"

FILE * fp = NULL;												/* fp of file being received or NULL */
time_t mdate;													/* file date of file being received */
bool mdate_known;
char filename[0x80];											/* filename of file being received */
char * name;													/* pointer to the part of the filename used in the actual open */

char * line = NULL;												/* device to use for io */
bool opt_v = false;												/* show progress output */
bool opt_d = false;												/* show debug output */
bool opt_q = false;
bool junk_pathnames = false;										/* junk incoming path names or keep them */
uint8_t rx_data_subpacket[8192];								/* zzap = 8192 */

uint32_t current_file_size;
time_t transfer_start;

/* 
 * show the progress of the transfer like this:
 * zmrx: receiving file "garbage" 4096 bytes ( 20%)
 * avoids the use of floating point.
 */

void
show_progress(char * name,FILE * fp)

{
	int percentage;
	double duration;
	intmax_t cps;
	off_t position;

	position = ftello(fp);

	if (current_file_size > 0 && position >= 0) {
		percentage = (int)(100.0 * (double)position /
			(double)current_file_size);
	}
	else {
		percentage = 100;
	}

	duration = difftime(time(NULL),transfer_start);

	if (duration <= 0.0) {
		duration = 1.0;
	}

	cps = position < 0 ? 0 : (intmax_t)((double)position / duration);

	fprintf(stderr,"receiving file \"%s\" %8" PRIdMAX " bytes (%3d %%/%5" PRIdMAX " cps)\r",
		name,(intmax_t)position,percentage,cps);
}

static bool
file_position(FILE * file,uint32_t * position)

{
	off_t offset;

	offset = ftello(file);
	if (offset < 0 || (uintmax_t)offset > UINT32_MAX) {
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

int 
receive_file_data(char * name,FILE * fp)

{
	uint32_t pos;
	uint32_t wanted_pos;
	size_t n;
	int type;

	/*
 	 * create a ZRPOS frame and send it to the other side
	 */

	if (!file_position(fp,&wanted_pos)) {
		tx_pos_header(ZFERR,UINT32_C(0));
		return ZFERR;
	}
	tx_pos_header(ZRPOS,wanted_pos);

/*	fprintf(stderr,"re-transmit from %d\n",ftell(fp));
*/
	/*
	 * wait for a ZDATA header with the right file offset
	 * or a timeout or a ZFIN
	 */

	do {
		do {
			type = rx_header(10000);
			if (type == TIMEOUT) {
				return TIMEOUT;
			}
		} while (type != ZDATA);

		pos = zmodem_header_position(rxd_header);
	} while (pos != wanted_pos);
		
	do {
		type = rx_data(rx_data_subpacket,&n);

/*		fprintf(stderr,"packet len %d type %d\n",n,type);
*/
		if (type == ENDOFFRAME || type == FRAMEOK) {
			fwrite(rx_data_subpacket,1,n,fp);
		}

		if (opt_v) {
			show_progress(name,fp);
		}

	} while (type == FRAMEOK);

	return type;
}

void
tx_zrinit(void)

{
	uint8_t zrinit_header[] = { ZRINIT, 0, 0, 0, 4 | ZF0_CANFDX | ZF0_CANOVIO | ZF0_CANFC32 };

	tx_hex_header(zrinit_header);
}

static bool
parse_mdate(const char *text,time_t *value)

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
	if (text == end || errno == ERANGE || wire_value == 0) {
		return false;
	}
	if (*end != '\0' && *end != ' ') {
		return false;
	}

	converted = (time_t)wire_value;
	if (difftime(converted,(time_t)0) < 0 ||
	    (uintmax_t)converted != wire_value) {
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

void
receive_file(void)

{
	uint32_t size = 0;
	uint32_t position;
	uintmax_t parsed_size;
	struct stat s;
	int type = 0;
	size_t l;
	bool clobber = false;
	bool protect = false;
	bool newer = false;
	bool exists;
	bool size_invalid = false;
	struct utimbuf tv;
	char * mode = "wb";
	char * file_info = (char *)rx_data_subpacket;
	char * metadata;
	char * size_field;
	char * date_field;

	mdate_known = false;

	/*
	 * fetch the management info bits from the ZRFILE header
	 */

	/*
	 * management option
	 */

	if (management_protect || (rxd_header[ZF1] & ZF1_ZMPROT)) {
		protect = true;
	}
	else {
		if (management_clobber || (rxd_header[ZF1] & ZF1_ZMCLOB)) {
			clobber = true;
		}
	}

	if (management_newer || (rxd_header[ZF1] & ZF1_ZMNEW)) {
		newer = true;
	}

	/*
	 * read the data subpacket containing the file information
	 */

	type = rx_data(rx_data_subpacket,&l);

	if (type != FRAMEOK && type != ENDOFFRAME) {
		if (type != TIMEOUT) {
			/*
			 * file info data subpacket was trashed
			 */
			tx_znak();
		}
		return;
	}

	/*
	 * extract the relevant info from the header.
	 */

	strcpy(filename,file_info);

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

	if (opt_v) {
		fprintf(stderr,"receiving file \"%s\"\r",name);
	}

	metadata = file_info + strlen(file_info) + 1;
	size_field = metadata;
	while (*size_field == ' ') {
		size_field++;
	}
	date_field = size_field;
	if (*size_field != '\0') {
		errno = 0;
		parsed_size = strtoumax(size_field,&date_field,10);
		if (*size_field == '-' || size_field == date_field ||
		    errno == ERANGE || parsed_size > UINT32_MAX) {
			size_invalid = true;
		}
		else {
			size = (uint32_t)parsed_size;
		}
	}

	if (*date_field == ' ') {
		mdate_known = parse_mdate(date_field + 1,&mdate);
	}
	if (size_invalid) {
		tx_pos_header(ZSKIP,UINT32_C(0));
		return;
	}

	current_file_size = size;

	/*
	 * decide whether to transfer the file or skip it
	 */

	fp = fopen(name,"rb");

	if (fp != NULL) {
		exists = fstat(fileno(fp),&s) == 0;

		fclose(fp);
	}
	else {
		exists = false;
	}

	/*
	 * if the file already exists here the management options need to
	 * be checked..
	 */
	if (exists) {
		if (mdate_known && mdate == s.st_mtime && s.st_size >= 0 &&
		    (uintmax_t)s.st_size <= UINT32_MAX) {
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
				tx_pos_header(ZSKIP,UINT32_C(0));
				return;
			}
			/*
			 * if it is not ok to just overwrite it
			 */
			if (!clobber) {
				/*
				 * if the remote file has to be newer
				 */
				if (newer) {
					if (!mdate_known || mdate < s.st_mtime) {
						tx_pos_header(ZSKIP,UINT32_C(0));
						/*
					 	 * and it isnt then exit here.
					 	 */
						return;
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

	fp = fopen(name,mode);

	if (fp == NULL) {
		tx_pos_header(ZSKIP,UINT32_C(0));
		if (opt_v) {
			fprintf(stderr,"zmrx: can't open file %s\n",name);
		}
		return;
	}

	transfer_start = time(NULL);

	for (;;) {
		if (!file_position(fp,&position)) {
			tx_pos_header(ZFERR,UINT32_C(0));
			type = ZFERR;
			break;
		}
		if (position == size) {
			break;
		}
		type = receive_file_data(filename,fp);
		if (type == ZEOF || type == ZFERR) {
			break;
		}
	}
	if (type == ZFERR) {
		fclose(fp);
		fp = NULL;
		return;
	}

	/*
 	 * wait for the eof header
	 */

	while (type != ZEOF) {
		type = rx_header_and_check(10000);
	} 

	/*
	 * close and exit
	 */

	fclose(fp);

	fp = NULL;

	/*
	 * set the time
	 */

	if (mdate_known) {
		tv.actime = mdate;
		tv.modtime = mdate;

		utime(name, &tv);
	}

	/*
	 * and close the input file
	 */

	if (opt_v) {
		fprintf(stderr,"zmrx: received file \"%s\"\n",name);
	}
}

void
cleanup(void)

{
	struct utimbuf tv;

	if (fp) {
		fflush(fp);
		fclose(fp);
		/*
		 * set the time (so crash recovery may work)
		 */

		if (mdate_known) {
			tv.actime = mdate;
			tv.modtime = mdate;

			utime(name, &tv);
		}
	}

	fd_exit();
}


void
usage(void)

{
	printf("zmrx %s Copyright (c) 1994 Stephen Hurd\n",VERSION);
	printf("usage : zmrx options\n");
	printf("	-lline      line to use for io\n");
	printf("	-j    		junk pathnames\n");
	printf("	-n    		transfer if source is newer\n");
	printf("	-o    	    overwrite if exists\n");
	printf("	-p          protect (don't overwrite if exists)\n");
	printf("\n");
	printf("	-d          debug output\n");
	printf("	-v          verbose output\n");
	printf("	-q          quiet\n");
	printf("	(only one of -n -c or -p may be specified)\n");

	cleanup();

	exit(1);
}

int
main(int argc,char ** argv)

{
	int i;
	char * s;
	int type;

	argv++;
	while (--argc > 0 && ((*argv)[0] == '-')) {
		for (s = argv[0]+1; *s != '\0'; s++) {
			switch (toupper(*s)) {
				OPT_BOOL('D',opt_d);
				OPT_BOOL('V',opt_v);
				OPT_BOOL('Q',opt_q);

				OPT_BOOL('N',management_newer);
				OPT_BOOL('O',management_clobber);
				OPT_BOOL('P',management_protect);
				OPT_BOOL('J',junk_pathnames);
				OPT_STRING('L',line);
				default:
					printf("zmrx: bad option %c\n",*s);
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

#if 0
	if (!opt_v) {
		freopen("/usr/src/utils/zmnew/trace","w",stderr);
		setbuf(stderr,NULL);
	}
#endif

	if ((management_newer + management_clobber + management_protect) > 1 || argc != 0) {
		usage();
	}

	if (line != NULL) {	
		if (freopen(line,"r",stdin) == NULL) {
			fprintf(stderr,"zmrx can't open line for input %s\n",line);
			exit(2);
		}
		if (freopen(line,"w",stdout) == NULL) {
			fprintf(stderr,"zmrx can't open line for output %s\n",line);
			exit(2);
		}
	}

	/*
	 * set the io device to transparent
	 */

	fd_init();	

	/*
	 * establish contact with the sender
	 */

	if (opt_v) {
		fprintf(stderr,"zmrx: establishing contact with sender\n");
	}

	/*
	 * make sure we dont get any old garbage
	 */

	rx_purge();

	/*
	 * loop here until contact is established.
	 * another packet than a ZRQINIT should be received.
	 */

	i = 0;
	do {
		i++;
		if (i > 10) {
			fprintf(stderr,"zmrx: can't establish contact with sender\n");
			cleanup();
			exit(3);
		}

		tx_zrinit();
		type = rx_header(7000);
	} while (type == TIMEOUT || type == ZRQINIT);

	if (opt_v) {
		fprintf(stderr,"zmrx: contact established\n");
		fprintf(stderr,"zmrx: starting file transfer\n");
	}

	/* 
	 * and receive files
	 * (other packets are acknowledged with a ZCOMPL but ignored.)
	 */

	do {
		switch (type) {
			case ZFILE:
				receive_file();
				break;
			default:
				tx_pos_header(ZCOMPL,UINT32_C(0));
				break;
		}

		do {
			tx_zrinit();

			type = rx_header(7000);
		} while (type == TIMEOUT);
	} while (type != ZFIN);

	/*
	 * close the session
	 */

	if (opt_v) {
		fprintf(stderr,"zmrx: closing the session\n");
	}

	{
		uint8_t zfin_header[] = { ZFIN, 0, 0, 0, 0 };

		tx_hex_header(zfin_header);
	}

	/*
	 * wait for the over and out sequence
	 */

	{
		int c;
		do {
			c = rx_raw(1000);
		} while (c != 'O' && c != TIMEOUT);

		if (c != TIMEOUT) {
			do {
				c = rx_raw(1000);
			} while (c != 'O' && c != TIMEOUT);
		}
	}

	if (opt_d) {
		fprintf(stderr,"zmrx: cleanup and exit\n");
	}

	cleanup();

	exit(0);

	return 0;		/* to stop the compiler from complaining */
}

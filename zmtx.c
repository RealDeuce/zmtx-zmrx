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
#include <stdio.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "version.h"

#include "zmodem.h"
#include "zmdm.h"
#include "opts.h"

#define MAX_SUBPACKETSIZE 1024

char * line = NULL;												/* device to use for io */
bool opt_v = false;											/* show progress output */
bool opt_d = false;											/* show debug output */
size_t subpacket_size = MAX_SUBPACKETSIZE;						/* data subpacket size. may be modified during a session */
int n_files_remaining;
uint8_t tx_data_subpacket[1024];

off_t current_file_size;
time_t transfer_start;

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
	size_t n;
	int type = ZCRCG;
	off_t position;
	uint8_t zdata_frame[] = { ZDATA, 0, 0, 0, 0 };

	/*
 	 * put the file position in the ZDATA frame
	 */

	position = ftello(fp);
	if (position < 0 || (uintmax_t)position > UINT32_MAX) {
		return ZFERR;
	}
	zmodem_set_header_position(zdata_frame,(uint32_t)position);

	tx_header(zdata_frame);
	/*
	 * send the data in the file
	 */

	while (!feof(fp)) {
		if (opt_v) {
			show_progress(name,fp);
		}

		/*
		 * read a block from the file
		 */
		n = fread(tx_data_subpacket,1,subpacket_size,fp);

		if (n == 0) {
			/*
			 * nothing to send ?
			 */
			break;
		}

		/*
		 * at end of file wait for an ACK
		 */
		if (ftello(fp) == current_file_size) {
			type = ZCRCW;
		}

		tx_data((uint8_t)type,tx_data_subpacket,n);

		if (type == ZCRCW) {
			int type;
			do {
				type = rx_header(10000);
				if (type == ZNAK || type == ZRPOS) {
					return type;
				}
			} while (type != ZACK);

			if (ftello(fp) == current_file_size) {
				if (opt_d) {
					fprintf(stderr,"end of file\n");
				}
				return ZACK;
			}
		}

		/* 
		 * characters from the other side
		 * check out that header
		 */

		while (rx_poll()) {
			int type;
			int c;
			c = rx_raw(1000);
			if (c == ZPAD) {
				type = rx_header(1000);
				if (type != TIMEOUT && type != ACK) {
					return type;
				}
			}
		}
	}

	/*
	 * end of file reached.
	 * should receive something... so fake ZACK
	 */

	return ZACK;
}

/*
 * send a file; returns true when session is aborted.
 * (using ZABORT frame)
 */

bool
send_file(char * name)

{
	uint32_t pos;
	uint32_t size;
	off_t seek_pos;
	struct stat s;
	FILE * fp;
	uintmax_t wire_mdate;
	char * p;
	uint8_t zfile_frame[] = { ZFILE, 0, 0, 0, 0 };
	uint8_t zeof_frame[] = { ZEOF, 0, 0, 0, 0 };
	int type;
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
		return false;
	}

	if (fstat(fileno(fp),&s) != 0) {
		if (opt_v) {
			fprintf(stderr,"zmtx: can't stat file %s\n",name);
		}
		fclose(fp);
		return false;
	}
	if (s.st_size < 0 || (uintmax_t)s.st_size > UINT32_MAX) {
		if (opt_v) {
			fprintf(stderr,"zmtx: file is too large for ZMODEM: %s\n",name);
		}
		fclose(fp);
		return false;
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

	strcpy(p,n);

	p += strlen(p) + 1;

	/*
	 * next the file size
	 */

	sprintf(p,"%" PRIu32 " ",size);

	p += strlen(p);

	/*
 	 * modification date
	 */

	sprintf(p,"%" PRIoMAX " ",wire_mdate);

	p += strlen(p);

	/*
	 * file mode
	 */

	sprintf(p,"0 ");

	p += strlen(p);

	/*
	 * serial number (??)
	 */

	sprintf(p,"0 ");

	p += strlen(p);

	/*
	 * number of files remaining
	 */

	sprintf(p,"%d ",n_files_remaining);

	p += strlen(p);

	/*
	 * file type
	 */

	sprintf(p,"0");

	p += strlen(p) + 1;

	do {
		/*
	 	 * send the header and the data
	 	 */

		tx_header(zfile_frame);
		tx_data(ZCRCW,tx_data_subpacket,
			(size_t)(p - (char *)tx_data_subpacket));
	
		/*
		 * wait for anything but an ZACK packet
		 */

		do {
			type = rx_header(10000);
		} while (type == ZACK);

		if (opt_d) {
			fprintf(stderr,"type : %d\n",type);
		}

		if (type == ZSKIP) {
			fclose(fp);
			if (opt_v) {
				fprintf(stderr,"zmtx: skipped file \"%s\"                       \n",name);
			}
			return false;
		}

	} while (type != ZRPOS);

	transfer_start = time(NULL);

	do {
		/*
		 * fetch pos from the ZRPOS header
		 */

		if (type == ZRPOS) {
			pos = zmodem_header_position(rxd_header);
		}

		/*
 		 * seek to the right place in the file
		 */
		seek_pos = (off_t)pos;
		if (seek_pos < 0 || (uintmax_t)seek_pos != pos ||
		    fseeko(fp,seek_pos,SEEK_SET) != 0) {
			fclose(fp);
			return true;
		}

		/*
		 * and start sending
		 */

		type = send_from(n,fp);

		if (type == ZFERR || type == ZABORT) {
 			fclose(fp);
			return true;
		}

	} while (type == ZRPOS || type == ZNAK);

	/*
	 * file sent. send end of file frame
	 * and wait for zrinit. if it doesnt come then try again
	 */

	zmodem_set_header_position(zeof_frame,size);

	do {
		tx_hex_header(zeof_frame);
		type = rx_header(10000);
	} while (type != ZRINIT);

	/*
	 * and close the input file
	 */

	if (opt_v) {
		fprintf(stderr,"zmtx: sent file \"%s\"                                    \n",name);
	}

	fclose(fp);

	return false;
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
	printf("	-n    		transfer if source is newer\n");
	printf("	-o    	    overwrite if exists\n");
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
	int i;
	char * s;

	argv++;
	while (--argc > 0 && ((*argv)[0] == '-')) {
		for (s = argv[0]+1; *s != '\0'; s++) {
			switch (toupper(*s)) {
				OPT_BOOL('D',opt_d);
				OPT_BOOL('V',opt_v);

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

		tx_raw('z');
		tx_raw('m');
		tx_raw(13);
		tx_hex_header(zrqinit_header);
	} while (rx_header(7000) != ZRINIT);

	if (opt_v) {
		fprintf(stderr,"zmtx: contact established\n");
		fprintf(stderr,"zmtx: starting file transfer\n");
	}

	/*
	 * decode receiver capability flags
	 * forget about encryption and compression.
	 */

	can_full_duplex					= (rxd_header[ZF0] & ZF0_CANFDX)  != 0;
	can_overlap_io					= (rxd_header[ZF0] & ZF0_CANOVIO) != 0;
	can_break						= (rxd_header[ZF0] & ZF0_CANBRK)  != 0;
	can_fcs_32						= (rxd_header[ZF0] & ZF0_CANFC32) != 0;
	escape_all_control_characters	= (rxd_header[ZF0] & ZF0_ESCCTL)  != 0;
	escape_8th_bit					= (rxd_header[ZF0] & ZF0_ESC8)    != 0;

	use_variable_headers			= (rxd_header[ZF1] & ZF1_CANVHDR) != 0;

	if (opt_d) {
		fprintf(stderr,"receiver %s full duplex\n"          ,can_full_duplex               ? "can"      : "can't");
		fprintf(stderr,"receiver %s overlap io\n"           ,can_overlap_io                ? "can"      : "can't");
		fprintf(stderr,"receiver %s break\n"                ,can_break                     ? "can"      : "can't");
		fprintf(stderr,"receiver %s fcs 32\n"               ,can_fcs_32                    ? "can"      : "can't");
		fprintf(stderr,"receiver %s escaped control chars\n",escape_all_control_characters ? "requests" : "doesn't request");
		fprintf(stderr,"receiver %s escaped 8th bit\n"      ,escape_8th_bit                ? "requests" : "doesn't request");
		fprintf(stderr,"receiver %s use variable headers\n" ,use_variable_headers          ? "can"      : "can't");
	}

	/* 
	 * and send each file in turn
	 */

	n_files_remaining = argc;

	while (argc) {
		if (send_file(*argv)) {
			if (opt_v) {
				fprintf(stderr,"zmtx: remote aborted.\n");
			}
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
		int type;
		uint8_t zfin_header[] = { ZFIN, 0, 0, 0, 0 };

		tx_hex_header(zfin_header);
		do {
			type = rx_header(10000);
		} while (type != ZFIN && type != TIMEOUT);
		
		/*
		 * these Os are formally required; but they don't do a thing
		 * unfortunately many programs require them to exit 
		 * (both programs already sent a ZFIN so why bother ?)
		 */

		if (type != TIMEOUT) {
			tx_raw('O');
			tx_raw('O');
		}
	}

	/*
	 * c'est fini
	 */

	if (opt_d) {
		fprintf(stderr,"zmtx: cleanup and exit\n");
	}

	cleanup();

	exit(0);

	return 0;
}

/*
 * POSIX transport adapter for the ZMODEM protocol engine.
 */

#ifndef ZMDM_POSIX_H_INCLUDED
#define ZMDM_POSIX_H_INCLUDED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <termios.h>

#include "zmdm.h"

struct zmodem_posix_io {
	int input_fd;
	int output_fd;
	int owned_fd;
	bool termios_saved;
	struct termios saved_termios;
	uint8_t output_buffer[ZMODEM_TX_BURST_CAPACITY];
	size_t output_count;
};

void zmodem_posix_io_init(struct zmodem_posix_io *, int, int);
int zmodem_posix_ignore_sigpipe(void);
int zmodem_posix_io_open(struct zmodem_posix_io *, const char *);
int zmodem_posix_io_make_raw(struct zmodem_posix_io *);
void zmodem_posix_io_restore(struct zmodem_posix_io *);
void zmodem_posix_io_close(struct zmodem_posix_io *);
void zmodem_posix_io_bind(struct zmodem_io *, struct zmodem_posix_io *);

#endif

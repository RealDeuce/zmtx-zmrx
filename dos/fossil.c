/* FSC-0015 FOSSIL serial backend. */

#include "plat.h"
#include "zmodem_plat.h"

#include <i86.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "zmdm.h"
#include "zmodem_dos_serial.h"

#define FOSSIL_SIGNATURE 0x1954U
#define FOSSIL_DATA_READY 0x0100U
#define FOSSIL_OVERRUN 0x0200U

static unsigned fossil_max_function;

static unsigned
fossil_simple(unsigned function,unsigned value,unsigned port)
{
	union REGS input;
	union REGS output;

	input.x.ax = (function << 8) | (value & 0xffU);
	input.x.bx = 0U;
	input.x.cx = 0U;
	input.x.dx = port;
	input.x.si = 0U;
	input.x.di = 0U;
	(void)int86(0x14,&input,&output);
	return output.x.ax;
}

static unsigned
fossil_block(unsigned function,unsigned port,void * buffer,size_t length)
{
	union REGS input;
	union REGS output;
	struct SREGS segments;
	void __far * far_buffer = buffer;

	input.x.ax = function << 8;
	input.x.bx = 0U;
	input.x.cx = length;
	input.x.dx = port;
	input.x.si = 0U;
	input.x.di = FP_OFF(far_buffer);
	segread(&segments);
	segments.es = FP_SEG(far_buffer);
	(void)int86x(0x14,&input,&output,&segments);
	return output.x.ax;
}

static int
fossil_rate_code(uint32_t rate,unsigned * code)
{
	switch (rate) {
		case UINT32_C(19200): *code = 0x03U; break;
		case UINT32_C(38400): *code = 0x23U; break;
		case UINT32_C(300): *code = 0x43U; break;
		case UINT32_C(600): *code = 0x63U; break;
		case UINT32_C(1200): *code = 0x83U; break;
		case UINT32_C(2400): *code = 0xa3U; break;
		case UINT32_C(4800): *code = 0xc3U; break;
		case UINT32_C(9600): *code = 0xe3U; break;
		default: return -1;
	}
	return 0;
}

int
zmodem_dos_fossil_init(struct zmodem_plat_io * io)
{
	union REGS input;
	union REGS output;

	input.x.ax = 0x0400U;
	input.x.bx = 0U;
	input.x.cx = 0U;
	input.x.dx = io->port;
	input.x.si = 0U;
	input.x.di = 0U;
	(void)int86(0x14,&input,&output);
	if (output.x.ax != FOSSIL_SIGNATURE) {
		return 1;
	}
	fossil_max_function = output.h.bl;
	if (io->rate_selected) {
		unsigned code;

		if (fossil_rate_code(io->rate,&code) != 0) {
			(void)fossil_simple(0x05U,0U,io->port);
			return -1;
		}
		(void)fossil_simple(0x00U,code,io->port);
	}
	if (io->flow_selected) {
		unsigned mask = 0xf0U;

		if ((io->flow & ZMODEM_DOS_FLOW_XON) != 0U) {
			mask |= 0x09U;
		}
		if ((io->flow & ZMODEM_DOS_FLOW_HARDWARE) != 0U) {
			mask |= 0x02U;
		}
		(void)fossil_simple(0x0fU,mask,io->port);
	}
	return 0;
}

int
zmodem_dos_fossil_close(struct zmodem_plat_io * io)
{
	(void)fossil_simple(0x05U,0U,io->port);
	return 0;
}

int
zmodem_dos_fossil_poll(struct zmodem_plat_io * io)
{
	unsigned status = fossil_simple(0x03U,0U,io->port);

	if ((status & FOSSIL_OVERRUN) != 0U) {
		return ZMODEM_IO_ERROR;
	}
	return (status & FOSSIL_DATA_READY) != 0U ? 1 : 0;
}

int
zmodem_dos_fossil_read(struct zmodem_plat_io * io,uint8_t * buffer,
    size_t capacity,size_t * count,int timeout_ms)
{
	clock_t start = clock();

	*count = 0U;
	if (capacity == 0U) {
		return ZMODEM_IO_ERROR;
	}
	for (;;) {
		if (fossil_max_function >= 0x18U) {
			unsigned received = fossil_block(0x18U,io->port,buffer,capacity);

			if (received != 0U) {
				*count = received;
				return ZMODEM_OK;
			}
		}
		else {
			int ready = zmodem_dos_fossil_poll(io);

			if (ready < 0) {
				return ready;
			}
			if (ready != 0) {
				buffer[0] = (uint8_t)fossil_simple(0x02U,0U,io->port);
				*count = 1U;
				return ZMODEM_OK;
			}
		}
		if (zmodem_dos_timeout_expired(start,timeout_ms)) {
			return ZMODEM_TIMEOUT;
		}
		zmodem_dos_idle();
	}
}

int
zmodem_dos_fossil_write(struct zmodem_plat_io * io,const uint8_t * buffer,
    size_t length)
{
	size_t used = 0U;

	while (used < length) {
		if (fossil_max_function >= 0x19U) {
			unsigned written = fossil_block(0x19U,io->port,
			    (void *)&buffer[used],length - used);

			if (written == 0U) {
				zmodem_dos_idle();
				continue;
			}
			used += written;
		}
		else {
			(void)fossil_simple(0x01U,buffer[used],io->port);
			used += 1U;
		}
	}
	return ZMODEM_OK;
}

int
zmodem_dos_fossil_flush(struct zmodem_plat_io * io)
{
	(void)fossil_simple(0x08U,0U,io->port);
	return ZMODEM_OK;
}

int
zmodem_dos_fossil_purge(struct zmodem_plat_io * io)
{
	(void)fossil_simple(0x0aU,0U,io->port);
	return ZMODEM_OK;
}

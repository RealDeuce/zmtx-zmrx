/* IBM-compatible BIOS INT 14h serial backend. */

#include "plat.h"
#include "zmodem_plat.h"

#include <bios.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "zmdm.h"
#include "zmodem_dos_serial.h"

#define BIOS_DATA_READY 0x0100U
#define BIOS_TX_EMPTY 0x4000U
#define BIOS_TIMEOUT 0x8000U

static int
bios_rate_code(uint32_t rate,unsigned * code)
{
	switch (rate) {
		case UINT32_C(110): *code = _COM_110; break;
		case UINT32_C(150): *code = _COM_150; break;
		case UINT32_C(300): *code = _COM_300; break;
		case UINT32_C(600): *code = _COM_600; break;
		case UINT32_C(1200): *code = _COM_1200; break;
		case UINT32_C(2400): *code = _COM_2400; break;
		case UINT32_C(4800): *code = _COM_4800; break;
		case UINT32_C(9600): *code = _COM_9600; break;
		default: return -1;
	}
	return 0;
}

int
zmodem_dos_bios_init(struct zmodem_plat_io * io)
{
	if (io->flow_selected && io->flow != 0U) {
		return -1;
	}
	if (io->rate_selected) {
		unsigned code;

		if (bios_rate_code(io->rate,&code) != 0 ||
		    (_bios_serialcom(_COM_INIT,io->port,
		    code | _COM_CHR8 | _COM_STOP1 | _COM_NOPARITY) &
		    BIOS_TIMEOUT) != 0U) {
			return -1;
		}
	}
	return 0;
}

int
zmodem_dos_bios_close(struct zmodem_plat_io * io)
{
	(void)io;
	return 0;
}

int
zmodem_dos_bios_poll(struct zmodem_plat_io * io)
{
	unsigned status = _bios_serialcom(_COM_STATUS,io->port,0U);

	if ((status & BIOS_TIMEOUT) != 0U) {
		return ZMODEM_IO_ERROR;
	}
	return (status & BIOS_DATA_READY) != 0U ? 1 : 0;
}

int
zmodem_dos_bios_read(struct zmodem_plat_io * io,uint8_t * buffer,
    size_t capacity,size_t * count,int timeout_ms)
{
	clock_t start = clock();

	*count = 0U;
	if (capacity == 0U) {
		return ZMODEM_IO_ERROR;
	}
	for (;;) {
		int ready = zmodem_dos_bios_poll(io);

		if (ready < 0) {
			return ready;
		}
		if (ready != 0) {
			unsigned result = _bios_serialcom(_COM_RECEIVE,io->port,0U);

			if ((result & BIOS_TIMEOUT) != 0U) {
				return ZMODEM_IO_ERROR;
			}
			buffer[0] = (uint8_t)result;
			*count = 1U;
			return ZMODEM_OK;
		}
		if (zmodem_dos_timeout_expired(start,timeout_ms)) {
			return ZMODEM_TIMEOUT;
		}
		zmodem_dos_idle();
	}
}

int
zmodem_dos_bios_write(struct zmodem_plat_io * io,const uint8_t * buffer,
    size_t length)
{
	size_t i;

	for (i = 0U; i < length; i++) {
		if ((_bios_serialcom(_COM_SEND,io->port,buffer[i]) &
		    BIOS_TIMEOUT) != 0U) {
			return ZMODEM_IO_ERROR;
		}
	}
	return ZMODEM_OK;
}

int
zmodem_dos_bios_flush(struct zmodem_plat_io * io)
{
	while ((_bios_serialcom(_COM_STATUS,io->port,0U) & BIOS_TX_EMPTY) == 0U) {
		zmodem_dos_idle();
	}
	return ZMODEM_OK;
}

int
zmodem_dos_bios_purge(struct zmodem_plat_io * io)
{
	while ((_bios_serialcom(_COM_STATUS,io->port,0U) &
	    BIOS_DATA_READY) != 0U) {
		(void)_bios_serialcom(_COM_RECEIVE,io->port,0U);
	}
	return ZMODEM_OK;
}

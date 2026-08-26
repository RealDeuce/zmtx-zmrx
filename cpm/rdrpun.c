/* Generic blocking CP/M 2.2 RDR:/PUN: modem overlay. */

#include "plat.h"

#include <cpm.h>
#include <stddef.h>
#include <stdint.h>

#include "zmdm.h"
#include "zmodem_cpm_driver.h"

int
zmodem_cpm_driver_init(void)
{
	return 0;
}

int
zmodem_cpm_driver_close(void)
{
	return 0;
}

int
zmodem_cpm_driver_read(void * context,uint8_t * buffer,size_t capacity,
    size_t * count,int timeout_ms)
{
	(void)context;
	(void)timeout_ms;
	*count = 0U;
	if (capacity == 0U) {
		return ZMODEM_IO_ERROR;
	}
	buffer[0] = (uint8_t)bdos(CPM_RRDR,0);
	*count = 1U;
	return ZMODEM_OK;
}

int
zmodem_cpm_driver_write(void * context,const uint8_t * buffer,size_t length)
{
	size_t i;

	(void)context;
	for (i = 0U; i < length; i++) {
		(void)bdos(CPM_WPUN,(int)buffer[i]);
	}
	return ZMODEM_OK;
}

int
zmodem_cpm_driver_flush(void * context)
{
	(void)context;
	return ZMODEM_OK;
}

int
zmodem_cpm_driver_poll(void * context)
{
	(void)context;
	return 0;
}

int
zmodem_cpm_driver_purge(void * context)
{
	(void)context;
	return ZMODEM_OK;
}

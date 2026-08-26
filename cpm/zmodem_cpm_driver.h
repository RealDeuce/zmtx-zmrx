/* Compile-time modem overlay contract for CP/M. */

#ifndef ZMODEM_CPM_DRIVER_H_INCLUDED
#define ZMODEM_CPM_DRIVER_H_INCLUDED

#include <stddef.h>
#include <stdint.h>

int zmodem_cpm_driver_init(void);
int zmodem_cpm_driver_close(void);
int zmodem_cpm_driver_read(void *,uint8_t *,size_t,size_t *,int);
int zmodem_cpm_driver_write(void *,const uint8_t *,size_t);
int zmodem_cpm_driver_flush(void *);
int zmodem_cpm_driver_poll(void *);
int zmodem_cpm_driver_purge(void *);

#endif

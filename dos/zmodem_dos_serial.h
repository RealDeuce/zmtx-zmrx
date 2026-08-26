/* Private DOS serial backend contract. */

#ifndef ZMODEM_DOS_SERIAL_H_INCLUDED
#define ZMODEM_DOS_SERIAL_H_INCLUDED

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "zmodem_plat.h"

int zmodem_dos_bios_init(struct zmodem_plat_io *);
int zmodem_dos_bios_close(struct zmodem_plat_io *);
int zmodem_dos_bios_read(struct zmodem_plat_io *,uint8_t *,size_t,size_t *,int);
int zmodem_dos_bios_write(struct zmodem_plat_io *,const uint8_t *,size_t);
int zmodem_dos_bios_flush(struct zmodem_plat_io *);
int zmodem_dos_bios_poll(struct zmodem_plat_io *);
int zmodem_dos_bios_purge(struct zmodem_plat_io *);

int zmodem_dos_fossil_init(struct zmodem_plat_io *);
int zmodem_dos_fossil_close(struct zmodem_plat_io *);
int zmodem_dos_fossil_read(struct zmodem_plat_io *,uint8_t *,size_t,size_t *,int);
int zmodem_dos_fossil_write(struct zmodem_plat_io *,const uint8_t *,size_t);
int zmodem_dos_fossil_flush(struct zmodem_plat_io *);
int zmodem_dos_fossil_poll(struct zmodem_plat_io *);
int zmodem_dos_fossil_purge(struct zmodem_plat_io *);

int zmodem_dos_uart_init(struct zmodem_plat_io *);
int zmodem_dos_uart_close(struct zmodem_plat_io *);
int zmodem_dos_uart_read(struct zmodem_plat_io *,uint8_t *,size_t,size_t *,int);
int zmodem_dos_uart_write(struct zmodem_plat_io *,const uint8_t *,size_t);
int zmodem_dos_uart_flush(struct zmodem_plat_io *);
int zmodem_dos_uart_poll(struct zmodem_plat_io *);
int zmodem_dos_uart_purge(struct zmodem_plat_io *);

bool zmodem_dos_timeout_expired(clock_t,int);
void zmodem_dos_idle(void);

#endif

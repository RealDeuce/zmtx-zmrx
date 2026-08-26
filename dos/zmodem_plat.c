/* Open Watcom real-mode DOS platform and serial frontend. */

#include "plat.h"
#include "zmodem_plat.h"

#include <ctype.h>
#include <errno.h>
#include <i86.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "zmdm.h"
#include "zmodem_dos_serial.h"

static struct zmodem_plat_io * cleanup_io;

static void
dos_atexit_cleanup(void)
{
	if (cleanup_io != NULL) {
		(void)zmodem_plat_io_close(cleanup_io);
	}
}

int
zmodem_dos_clock_gettime(int clock_id,struct zmodem_dos_timespec * value)
{
	clock_t ticks;

	(void)clock_id;
	if (value == NULL) {
		errno = EINVAL;
		return -1;
	}
	ticks = clock();
	value->tv_sec = (long)(ticks / CLOCKS_PER_SEC);
	value->tv_nsec = (long)((ticks % CLOCKS_PER_SEC) *
	    (1000000000L / CLOCKS_PER_SEC));
	return 0;
}

bool
zmodem_dos_timeout_expired(clock_t start,int timeout_ms)
{
	clock_t elapsed;
	uint32_t required;

	if (timeout_ms <= 0) {
		return true;
	}
	elapsed = clock() - start;
	required = ((uint32_t)(unsigned)timeout_ms *
	    (uint32_t)CLOCKS_PER_SEC + UINT32_C(999)) / UINT32_C(1000);
	return (uint32_t)elapsed >= required;
}

void
zmodem_dos_idle(void)
{
	union REGS input;
	union REGS output;

	input.x.ax = 0U;
	input.x.bx = 0U;
	input.x.cx = 0U;
	input.x.dx = 0U;
	input.x.si = 0U;
	input.x.di = 0U;
	(void)int86(0x28,&input,&output);
}

void
zmodem_plat_io_init(struct zmodem_plat_io * io,int input,int output)
{
	(void)input;
	(void)output;
	(void)memset(io,0,sizeof(*io));
	io->requested_backend = ZMODEM_DOS_AUTO;
	io->active_backend = ZMODEM_DOS_AUTO;
	io->port = 0U;
}

int
zmodem_plat_ignore_sigpipe(void)
{
	return 0;
}

int
zmodem_plat_io_open(struct zmodem_plat_io * io,const char * path)
{
	(void)io;
	(void)path;
	errno = ENOSYS;
	return -1;
}

int
zmodem_plat_io_make_raw(struct zmodem_plat_io * io)
{
	(void)io;
	return 0;
}

int
zmodem_plat_io_restore(struct zmodem_plat_io * io)
{
	(void)io;
	return 0;
}

static int
backend_close(struct zmodem_plat_io * io)
{
	switch (io->active_backend) {
		case ZMODEM_DOS_FOSSIL:
			return zmodem_dos_fossil_close(io);
		case ZMODEM_DOS_UART:
			return zmodem_dos_uart_close(io);
		case ZMODEM_DOS_BIOS:
			return zmodem_dos_bios_close(io);
		default:
			return 0;
	}
}

int
zmodem_plat_io_close(struct zmodem_plat_io * io)
{
	int result;

	if (!io->initialized) {
		return 0;
	}
	result = backend_close(io);
	io->initialized = false;
	io->active_backend = ZMODEM_DOS_AUTO;
	if (cleanup_io == io) {
		cleanup_io = NULL;
	}
	return result;
}

static int
dos_read(void * context,uint8_t * buffer,size_t capacity,size_t * count,
    int timeout_ms)
{
	struct zmodem_plat_io * io = context;

	switch (io->active_backend) {
		case ZMODEM_DOS_FOSSIL:
			return zmodem_dos_fossil_read(io,buffer,capacity,count,
			    timeout_ms);
		case ZMODEM_DOS_UART:
			return zmodem_dos_uart_read(io,buffer,capacity,count,
			    timeout_ms);
		case ZMODEM_DOS_BIOS:
			return zmodem_dos_bios_read(io,buffer,capacity,count,
			    timeout_ms);
		default:
			*count = 0U;
			return ZMODEM_IO_ERROR;
	}
}

static int
dos_write(void * context,const uint8_t * buffer,size_t length)
{
	struct zmodem_plat_io * io = context;

	switch (io->active_backend) {
		case ZMODEM_DOS_FOSSIL:
			return zmodem_dos_fossil_write(io,buffer,length);
		case ZMODEM_DOS_UART:
			return zmodem_dos_uart_write(io,buffer,length);
		case ZMODEM_DOS_BIOS:
			return zmodem_dos_bios_write(io,buffer,length);
		default:
			return ZMODEM_IO_ERROR;
	}
}

static int
dos_flush(void * context)
{
	struct zmodem_plat_io * io = context;

	switch (io->active_backend) {
		case ZMODEM_DOS_FOSSIL: return zmodem_dos_fossil_flush(io);
		case ZMODEM_DOS_UART: return zmodem_dos_uart_flush(io);
		case ZMODEM_DOS_BIOS: return zmodem_dos_bios_flush(io);
		default: return ZMODEM_IO_ERROR;
	}
}

static int
dos_poll(void * context)
{
	struct zmodem_plat_io * io = context;

	switch (io->active_backend) {
		case ZMODEM_DOS_FOSSIL: return zmodem_dos_fossil_poll(io);
		case ZMODEM_DOS_UART: return zmodem_dos_uart_poll(io);
		case ZMODEM_DOS_BIOS: return zmodem_dos_bios_poll(io);
		default: return ZMODEM_IO_ERROR;
	}
}

static int
dos_purge(void * context)
{
	struct zmodem_plat_io * io = context;

	switch (io->active_backend) {
		case ZMODEM_DOS_FOSSIL: return zmodem_dos_fossil_purge(io);
		case ZMODEM_DOS_UART: return zmodem_dos_uart_purge(io);
		case ZMODEM_DOS_BIOS: return zmodem_dos_bios_purge(io);
		default: return ZMODEM_IO_ERROR;
	}
}

void
zmodem_plat_io_bind(struct zmodem_io * interface,struct zmodem_plat_io * io)
{
	interface->context = io;
	interface->read = dos_read;
	interface->write = dos_write;
	interface->flush = dos_flush;
	interface->poll = dos_poll;
	interface->purge = dos_purge;
}

static bool
select_backend(struct zmodem_plat_io * io,enum zmodem_dos_backend backend)
{
	if (io->requested_backend != ZMODEM_DOS_AUTO &&
	    io->requested_backend != backend) {
		return false;
	}
	io->requested_backend = backend;
	return true;
}

static bool
parse_number(const char * text,int base,uint32_t maximum,uint32_t * value)
{
	char * end;
	unsigned long parsed;

	if (*text == '\0') {
		return false;
	}
	errno = 0;
	parsed = strtoul(text,&end,base);
	if (errno == ERANGE || *end != '\0' || parsed > maximum) {
		return false;
	}
	*value = (uint32_t)parsed;
	return true;
}

static enum zmodem_plat_option_result
parse_attached(struct zmodem_plat_io * io,int option,const char * argument,
    size_t * option_index)
{
	const char * value_text = &argument[*option_index + 1U];
	uint32_t value;

	switch (option) {
		case 'C':
			if (!parse_number(value_text,10,4U,&value) || value == 0U) {
				return ZMODEM_PLAT_OPTION_INVALID;
			}
			io->port = (unsigned)value - 1U;
			break;
		case 'A':
			if (!parse_number(value_text,16,UINT16_MAX - 7U,&value) ||
			    value == 0U || !select_backend(io,ZMODEM_DOS_UART)) {
				return ZMODEM_PLAT_OPTION_INVALID;
			}
			io->base = (unsigned)value;
			io->base_selected = true;
			break;
		case 'G':
			if (!parse_number(value_text,10,15U,&value) || value < 2U ||
			    !select_backend(io,ZMODEM_DOS_UART)) {
				return ZMODEM_PLAT_OPTION_INVALID;
			}
			io->irq = (unsigned)value;
			io->irq_selected = true;
			break;
		case 'R':
			if (!parse_number(value_text,10,UINT32_C(115200),&value) ||
			    value == 0U) {
				return ZMODEM_PLAT_OPTION_INVALID;
			}
			io->rate = value;
			io->rate_selected = true;
			break;
		default:
			return ZMODEM_PLAT_OPTION_NOT_HANDLED;
	}
	*option_index = strlen(argument) - 1U;
	return ZMODEM_PLAT_OPTION_ACCEPTED;
}

enum zmodem_plat_option_result
zmodem_plat_parse_option(struct zmodem_plat_io * io,
    enum zmodem_plat_application application,const char * argument,
    size_t * option_index)
{
	int option = toupper((unsigned char)argument[*option_index]);
	enum zmodem_dos_backend backend;

	(void)application;
	if (option == 'A' || option == 'C' || option == 'G' || option == 'R') {
		return parse_attached(io,option,argument,option_index);
	}
	switch (option) {
		case 'F': backend = ZMODEM_DOS_FOSSIL; break;
		case 'U': backend = ZMODEM_DOS_UART; break;
		case 'I': backend = ZMODEM_DOS_BIOS; break;
		case 'H':
			if (io->flow_none_selected) {
				return ZMODEM_PLAT_OPTION_INVALID;
			}
			io->flow_selected = true;
			io->flow |= ZMODEM_DOS_FLOW_HARDWARE;
			return ZMODEM_PLAT_OPTION_ACCEPTED;
		case 'X':
			if (io->flow_none_selected) {
				return ZMODEM_PLAT_OPTION_INVALID;
			}
			io->flow_selected = true;
			io->flow |= ZMODEM_DOS_FLOW_XON;
			return ZMODEM_PLAT_OPTION_ACCEPTED;
		case 'K':
			if (io->flow_selected && io->flow != 0U) {
				return ZMODEM_PLAT_OPTION_INVALID;
			}
			io->flow_selected = true;
			io->flow_none_selected = true;
			io->flow = 0U;
			return ZMODEM_PLAT_OPTION_ACCEPTED;
		default:
			return ZMODEM_PLAT_OPTION_NOT_HANDLED;
	}
	return select_backend(io,backend) ? ZMODEM_PLAT_OPTION_ACCEPTED :
	    ZMODEM_PLAT_OPTION_INVALID;
}

static void
apply_port_defaults(struct zmodem_plat_io * io)
{
	static const unsigned bases[4] = { 0x3f8U,0x2f8U,0x3e8U,0x2e8U };
	static const unsigned irqs[4] = { 4U,3U,4U,3U };

	if (!io->base_selected) {
		io->base = bases[io->port];
	}
	if (!io->irq_selected) {
		io->irq = irqs[io->port];
	}
}

static int
start_backend(struct zmodem_plat_io * io,enum zmodem_dos_backend backend)
{
	switch (backend) {
		case ZMODEM_DOS_FOSSIL: return zmodem_dos_fossil_init(io);
		case ZMODEM_DOS_UART: return zmodem_dos_uart_init(io);
		case ZMODEM_DOS_BIOS: return zmodem_dos_bios_init(io);
		default: return -1;
	}
}

int
zmodem_plat_post_parse(struct zmodem_plat_io * io,
    enum zmodem_plat_application application,int argc,char * const * argv,
    size_t first_operand)
{
	int result;

	(void)application;
	(void)argc;
	(void)argv;
	(void)first_operand;
	apply_port_defaults(io);
	if (io->requested_backend != ZMODEM_DOS_AUTO) {
		result = start_backend(io,io->requested_backend);
		if (result != 0) {
			(void)fprintf(stderr,"zmodem: requested DOS serial backend is unavailable or incompatible\n");
			return 2;
		}
		io->active_backend = io->requested_backend;
	}
	else {
		result = start_backend(io,ZMODEM_DOS_FOSSIL);
		if (result == 0) {
			io->active_backend = ZMODEM_DOS_FOSSIL;
		}
		else if (result < 0) {
			(void)fprintf(stderr,"zmodem: FOSSIL driver rejected the selected line configuration\n");
			return 2;
		}
		else {
			result = start_backend(io,ZMODEM_DOS_UART);
			if (result == 0) {
				io->active_backend = ZMODEM_DOS_UART;
			}
			else if (result < 0) {
				(void)fprintf(stderr,"zmodem: UART rejected the selected line configuration\n");
				return 2;
			}
			else if (start_backend(io,ZMODEM_DOS_BIOS) == 0) {
				io->active_backend = ZMODEM_DOS_BIOS;
			}
			else {
				(void)fprintf(stderr,"zmodem: no usable DOS serial backend\n");
				return 2;
			}
		}
	}
	io->requires_nonstreaming = io->active_backend == ZMODEM_DOS_BIOS ||
	    (io->active_backend == ZMODEM_DOS_UART && io->flow == 0U);
	io->initialized = true;
	cleanup_io = io;
	if (atexit(dos_atexit_cleanup) != 0) {
		(void)zmodem_plat_io_close(io);
		return 2;
	}
	return 0;
}

void
zmodem_plat_usage(enum zmodem_plat_application application)
{
	(void)application;
	(void)printf("\t-f|-u|-i   force FOSSIL, 16550 UART, or BIOS serial I/O\n");
	(void)printf("\t-cN         use COM1 through COM4 (default COM1)\n");
	(void)printf("\t-aHEX -gN   set a direct UART base address and IRQ 2 through 15\n");
	(void)printf("\t-rRATE      set an explicit 8N1 line rate\n");
	(void)printf("\t-h|-x|-k    RTS/CTS, XON/XOFF, or no flow control (-hx: both)\n");
}

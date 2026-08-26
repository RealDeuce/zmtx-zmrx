#include "plat.h"
#include "zmodem_plat.h"

#include <stdarg.h>

int
zmodem_c99_clock_gettime(int clock_id,struct zmodem_c99_timespec * value)
{
	(void)clock_id;
	value->tv_sec = (time_t)0;
	value->tv_nsec = 0L;
	return 0;
}

int
zmodem_c99_open(const char * path,int flags,...)
{
	va_list arguments;

	(void)path;
	(void)flags;
	va_start(arguments,flags);
	va_end(arguments);
	return -1;
}

int
zmodem_c99_close(int descriptor)
{
	(void)descriptor;
	return 0;
}

long
zmodem_c99_read(int descriptor,void * buffer,size_t length)
{
	(void)descriptor;
	(void)buffer;
	(void)length;
	return -1L;
}

long
zmodem_c99_lseek(int descriptor,long offset,int origin)
{
	(void)descriptor;
	(void)origin;
	return offset;
}

int
zmodem_c99_fstat(int descriptor,struct zmodem_c99_stat * status)
{
	(void)descriptor;
	status->st_size = 0L;
	status->st_mtime = (time_t)0;
	return 0;
}

int
zmodem_c99_stat_file(const char * path,struct zmodem_c99_stat * status)
{
	(void)path;
	status->st_size = 0L;
	status->st_mtime = (time_t)0;
	return -1;
}

FILE *
zmodem_c99_fdopen(int descriptor,const char * mode)
{
	(void)descriptor;
	(void)mode;
	return NULL;
}

long
zmodem_c99_ftell(FILE * stream)
{
	return ftell(stream);
}

int
zmodem_c99_utime(const char * path,const struct zmodem_c99_utimbuf * times)
{
	(void)path;
	(void)times;
	return 0;
}

static int
c99_read(void * context,uint8_t * buffer,size_t capacity,size_t * count,
    int timeout_ms)
{
	(void)context;
	(void)buffer;
	(void)capacity;
	(void)timeout_ms;
	*count = 0U;
	return ZMODEM_IO_ERROR;
}

static int
c99_write(void * context,const uint8_t * buffer,size_t length)
{
	(void)context;
	(void)buffer;
	(void)length;
	return ZMODEM_IO_ERROR;
}

static int
c99_flush(void * context)
{
	(void)context;
	return ZMODEM_OK;
}

static int
c99_poll(void * context)
{
	(void)context;
	return 0;
}

static int
c99_purge(void * context)
{
	(void)context;
	return ZMODEM_OK;
}

void
zmodem_plat_io_init(struct zmodem_plat_io * io,int input,int output)
{
	(void)input;
	(void)output;
	io->unused = 0;
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

int
zmodem_plat_io_close(struct zmodem_plat_io * io)
{
	(void)io;
	return 0;
}

void
zmodem_plat_io_bind(struct zmodem_io * interface,struct zmodem_plat_io * io)
{
	interface->context = io;
	interface->read = c99_read;
	interface->write = c99_write;
	interface->flush = c99_flush;
	interface->poll = c99_poll;
	interface->purge = c99_purge;
}

enum zmodem_plat_option_result
zmodem_plat_parse_option(struct zmodem_plat_io * io,
    enum zmodem_plat_application application,const char * argument,
    size_t * option_index)
{
	(void)io;
	(void)application;
	(void)argument;
	(void)option_index;
	return ZMODEM_PLAT_OPTION_NOT_HANDLED;
}

int
zmodem_plat_post_parse(struct zmodem_plat_io * io,
    enum zmodem_plat_application application,int argc,char * const * argv,
    size_t first_operand)
{
	(void)io;
	(void)application;
	(void)argc;
	(void)argv;
	(void)first_operand;
	return 0;
}

void
zmodem_plat_usage(enum zmodem_plat_application application)
{
	(void)application;
}

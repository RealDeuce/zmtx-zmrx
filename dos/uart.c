/* Interrupt-driven 16550 backend for real-mode DOS. */

#include "plat.h"
#include "zmodem_plat.h"

#include <conio.h>
#include <dos.h>
#include <i86.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "zmdm.h"
#include "zmodem.h"
#include "zmodem_dos_serial.h"

#define UART_RING_SIZE 2048U
#define UART_RING_MASK (UART_RING_SIZE - 1U)
#define UART_RING_HIGH 1536U
#define UART_RING_LOW 512U

#define UART_RBR 0U
#define UART_THR 0U
#define UART_DLL 0U
#define UART_IER 1U
#define UART_DLM 1U
#define UART_IIR 2U
#define UART_FCR 2U
#define UART_LCR 3U
#define UART_MCR 4U
#define UART_LSR 5U
#define UART_MSR 6U

#define UART_IER_RX 0x01U
#define UART_IER_TX 0x02U
#define UART_IER_LINE 0x04U
#define UART_IER_MODEM 0x08U
#define UART_IIR_PENDING 0x01U
#define UART_LCR_DLAB 0x80U
#define UART_LSR_DATA 0x01U
#define UART_LSR_ERROR 0x1eU
#define UART_LSR_THRE 0x20U
#define UART_LSR_TEMT 0x40U
#define UART_MCR_DTR 0x01U
#define UART_MCR_RTS 0x02U
#define UART_MCR_OUT2 0x08U
#define UART_MSR_CTS 0x10U

struct uart_runtime {
	volatile uint8_t ring[UART_RING_SIZE];
	volatile unsigned head;
	volatile unsigned tail;
	volatile bool error;
	volatile bool tx_paused;
	volatile int pending_flow;
	unsigned base;
	unsigned irq;
	unsigned vector;
	unsigned ier;
	unsigned saved_ier;
	unsigned saved_lcr;
	unsigned saved_mcr;
	unsigned saved_dll;
	unsigned saved_dlm;
	unsigned saved_master_mask;
	unsigned saved_slave_mask;
	bool saved_fifo;
	bool high_irq;
	bool xon_flow;
	bool hardware_flow;
	bool installed;
	void (__interrupt __far * old_handler)(void);
};

static struct uart_runtime uart;

static unsigned
ring_count(void)
{
	return (uart.head - uart.tail) & UART_RING_MASK;
}

static void
set_ier(unsigned value)
{
	uart.ier = value;
	(void)outp(uart.base + UART_IER,value);
}

static void
schedule_flow(int byte)
{
	if (!uart.xon_flow || uart.pending_flow == byte) {
		return;
	}
	uart.pending_flow = byte;
	if ((inp(uart.base + UART_LSR) & UART_LSR_THRE) != 0U) {
		(void)outp(uart.base + UART_THR,(unsigned)byte);
		uart.pending_flow = -1;
	}
	else {
		set_ier(uart.ier | UART_IER_TX);
	}
}

static void
update_receive_flow(void)
{
	unsigned count = ring_count();
	unsigned mcr;

	if (count >= UART_RING_HIGH) {
		schedule_flow(XOFF);
		if (uart.hardware_flow) {
			mcr = inp(uart.base + UART_MCR);
			(void)outp(uart.base + UART_MCR,mcr & ~UART_MCR_RTS);
		}
	}
	else if (count <= UART_RING_LOW) {
		schedule_flow(XON);
		if (uart.hardware_flow) {
			mcr = inp(uart.base + UART_MCR);
			(void)outp(uart.base + UART_MCR,mcr | UART_MCR_RTS);
		}
	}
}

static void
receive_byte(uint8_t byte)
{
	unsigned next;

	if (uart.xon_flow && byte == XOFF) {
		uart.tx_paused = true;
		return;
	}
	if (uart.xon_flow && byte == XON) {
		uart.tx_paused = false;
		return;
	}
	next = (uart.head + 1U) & UART_RING_MASK;
	if (next == uart.tail) {
		uart.error = true;
		return;
	}
	uart.ring[uart.head] = byte;
	uart.head = next;
}

static void
send_eoi(void)
{
	if (uart.high_irq) {
		(void)outp(0xa0U,0x20U);
	}
	(void)outp(0x20U,0x20U);
}

static void __interrupt __far
uart_interrupt(void)
{
	bool handled = false;
	unsigned iir;

	while (((iir = inp(uart.base + UART_IIR)) & UART_IIR_PENDING) == 0U) {
		handled = true;
		switch (iir & 0x0eU) {
			case 0x06U:
				if ((inp(uart.base + UART_LSR) & UART_LSR_ERROR) != 0U) {
					uart.error = true;
				}
				break;
			case 0x04U:
			case 0x0cU:
				while ((inp(uart.base + UART_LSR) & UART_LSR_DATA) != 0U) {
					receive_byte((uint8_t)inp(uart.base + UART_RBR));
				}
				update_receive_flow();
				break;
			case 0x02U:
				if (uart.pending_flow >= 0) {
					(void)outp(uart.base + UART_THR,
					    (unsigned)uart.pending_flow);
					uart.pending_flow = -1;
				}
				set_ier(uart.ier & ~UART_IER_TX);
				break;
			case 0x00U:
				(void)inp(uart.base + UART_MSR);
				break;
			default:
				uart.error = true;
				break;
		}
	}
	if (!handled && uart.old_handler != NULL) {
		_chain_intr(uart.old_handler);
	}
	send_eoi();
}

static bool
at_class_machine(void)
{
	union REGS input;
	union REGS output;
	struct SREGS segments;

	input.x.ax = 0xc000U;
	input.x.bx = 0U;
	input.x.cx = 0U;
	input.x.dx = 0U;
	input.x.si = 0U;
	input.x.di = 0U;
	segread(&segments);
	(void)int86x(0x15,&input,&output,&segments);
	return output.x.cflag == 0U;
}

static unsigned
irq_vector(unsigned irq)
{
	return irq < 8U ? 8U + irq : 0x70U + irq - 8U;
}

static bool
probe_fifo(unsigned base,bool * was_enabled)
{
	unsigned initial = inp(base + UART_IIR);
	unsigned result;

	*was_enabled = (initial & 0xc0U) == 0xc0U;
	(void)outp(base + UART_FCR,0x01U);
	result = inp(base + UART_IIR);
	(void)outp(base + UART_FCR,*was_enabled ? 0x01U : 0x00U);
	return (result & 0xc0U) == 0xc0U;
}

static bool
valid_divisor(uint32_t rate,unsigned * divisor)
{
	uint32_t value;

	if (rate == 0U || rate > UINT32_C(115200) ||
	    (UINT32_C(115200) % rate) != 0U) {
		return false;
	}
	value = UINT32_C(115200) / rate;
	if (value == 0U || value > UINT16_MAX) {
		return false;
	}
	*divisor = (unsigned)value;
	return true;
}

int
zmodem_dos_uart_init(struct zmodem_plat_io * io)
{
	unsigned divisor = 0U;
	unsigned irq = io->irq;
	unsigned lcr;

	if (io->rate_selected && !valid_divisor(io->rate,&divisor)) {
		return -1;
	}
	if (!probe_fifo(io->base,&uart.saved_fifo)) {
		return 1;
	}
	if (irq == 2U && at_class_machine()) {
		irq = 9U;
	}
	uart.base = io->base;
	uart.irq = irq;
	uart.vector = irq_vector(irq);
	uart.high_irq = irq >= 8U;
	uart.xon_flow = (io->flow & ZMODEM_DOS_FLOW_XON) != 0U;
	uart.hardware_flow =
	    (io->flow & ZMODEM_DOS_FLOW_HARDWARE) != 0U;
	uart.head = 0U;
	uart.tail = 0U;
	uart.error = false;
	uart.tx_paused = false;
	uart.pending_flow = -1;

	_disable();
	uart.saved_lcr = inp(uart.base + UART_LCR);
	lcr = uart.saved_lcr & ~UART_LCR_DLAB;
	(void)outp(uart.base + UART_LCR,lcr);
	uart.saved_ier = inp(uart.base + UART_IER);
	uart.saved_mcr = inp(uart.base + UART_MCR);
	(void)outp(uart.base + UART_LCR,lcr | UART_LCR_DLAB);
	uart.saved_dll = inp(uart.base + UART_DLL);
	uart.saved_dlm = inp(uart.base + UART_DLM);
	if (io->rate_selected) {
		(void)outp(uart.base + UART_DLL,divisor & 0xffU);
		(void)outp(uart.base + UART_DLM,divisor >> 8);
		lcr = 0x03U;
	}
	(void)outp(uart.base + UART_LCR,lcr);
	(void)outp(uart.base + UART_FCR,0xc7U);
	(void)outp(uart.base + UART_MCR,
	    uart.saved_mcr | UART_MCR_DTR | UART_MCR_RTS | UART_MCR_OUT2);
	uart.old_handler = _dos_getvect(uart.vector);
	_dos_setvect(uart.vector,uart_interrupt);
	uart.saved_master_mask = inp(0x21U);
	uart.saved_slave_mask = inp(0xa1U);
	if (uart.high_irq) {
		(void)outp(0xa1U,uart.saved_slave_mask & ~(1U << (irq - 8U)));
		(void)outp(0x21U,uart.saved_master_mask & ~(1U << 2));
	}
	else {
		(void)outp(0x21U,uart.saved_master_mask & ~(1U << irq));
	}
	uart.ier = UART_IER_RX | UART_IER_LINE |
	    (uart.hardware_flow ? UART_IER_MODEM : 0U);
	set_ier(uart.ier);
	uart.installed = true;
	_enable();
	return 0;
}

int
zmodem_dos_uart_close(struct zmodem_plat_io * io)
{
	(void)io;
	if (!uart.installed) {
		return 0;
	}
	_disable();
	(void)outp(uart.base + UART_IER,0U);
	_dos_setvect(uart.vector,uart.old_handler);
	(void)outp(0x21U,uart.saved_master_mask);
	if (uart.high_irq) {
		(void)outp(0xa1U,uart.saved_slave_mask);
	}
	(void)outp(uart.base + UART_LCR,
	    (uart.saved_lcr & ~UART_LCR_DLAB) | UART_LCR_DLAB);
	(void)outp(uart.base + UART_DLL,uart.saved_dll);
	(void)outp(uart.base + UART_DLM,uart.saved_dlm);
	(void)outp(uart.base + UART_LCR,uart.saved_lcr & ~UART_LCR_DLAB);
	(void)outp(uart.base + UART_MCR,uart.saved_mcr);
	(void)outp(uart.base + UART_FCR,uart.saved_fifo ? 0x01U : 0x00U);
	(void)outp(uart.base + UART_IER,uart.saved_ier);
	uart.installed = false;
	_enable();
	return 0;
}

int
zmodem_dos_uart_poll(struct zmodem_plat_io * io)
{
	(void)io;
	if (uart.error) {
		return ZMODEM_IO_ERROR;
	}
	return uart.head != uart.tail ? 1 : 0;
}

int
zmodem_dos_uart_read(struct zmodem_plat_io * io,uint8_t * buffer,
    size_t capacity,size_t * count,int timeout_ms)
{
	clock_t start = clock();

	(void)io;
	*count = 0U;
	if (capacity == 0U) {
		return ZMODEM_IO_ERROR;
	}
	for (;;) {
		if (uart.error) {
			return ZMODEM_IO_ERROR;
		}
		_disable();
		while (*count < capacity && uart.tail != uart.head) {
			buffer[*count] = uart.ring[uart.tail];
			uart.tail = (uart.tail + 1U) & UART_RING_MASK;
			*count += 1U;
		}
		update_receive_flow();
		_enable();
		if (*count != 0U) {
			return ZMODEM_OK;
		}
		if (zmodem_dos_timeout_expired(start,timeout_ms)) {
			return ZMODEM_TIMEOUT;
		}
		zmodem_dos_idle();
	}
}

static bool
transmit_ready(void)
{
	if (uart.tx_paused) {
		return false;
	}
	if (uart.hardware_flow &&
	    (inp(uart.base + UART_MSR) & UART_MSR_CTS) == 0U) {
		return false;
	}
	return (inp(uart.base + UART_LSR) & UART_LSR_THRE) != 0U;
}

int
zmodem_dos_uart_write(struct zmodem_plat_io * io,const uint8_t * buffer,
    size_t length)
{
	size_t i;

	(void)io;
	for (i = 0U; i < length; i++) {
		while (!transmit_ready()) {
			if (uart.error) {
				return ZMODEM_IO_ERROR;
			}
			zmodem_dos_idle();
		}
		_disable();
		if (uart.pending_flow >= 0) {
			(void)outp(uart.base + UART_THR,(unsigned)uart.pending_flow);
			uart.pending_flow = -1;
			_enable();
			while (!transmit_ready()) {
				zmodem_dos_idle();
			}
			_disable();
		}
		(void)outp(uart.base + UART_THR,buffer[i]);
		_enable();
	}
	return ZMODEM_OK;
}

int
zmodem_dos_uart_flush(struct zmodem_plat_io * io)
{
	(void)io;
	while (uart.pending_flow >= 0 ||
	    (inp(uart.base + UART_LSR) & UART_LSR_TEMT) == 0U) {
		zmodem_dos_idle();
	}
	return uart.error ? ZMODEM_IO_ERROR : ZMODEM_OK;
}

int
zmodem_dos_uart_purge(struct zmodem_plat_io * io)
{
	(void)io;
	_disable();
	uart.head = 0U;
	uart.tail = 0U;
	uart.error = false;
	(void)outp(uart.base + UART_FCR,0xc7U);
	update_receive_flow();
	_enable();
	return ZMODEM_OK;
}

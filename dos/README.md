# 16-bit DOS port

This is a secondary Open Watcom port for real-mode IBM-compatible DOS. POSIX
remains the only first-class platform.

After loading Open Watcom's environment, build with its `wmake` rather than
the system make:

```bat
wmake -f makefile.dos
```

The build produces `build/dos/zmtx.exe` and `build/dos/zmrx.exe`. It targets
the 8086 and uses Open Watcom's small memory model: one code segment and one
near-data/stack segment with an 8 KiB stack. The normal 8 KiB protocol buffers
remain enabled. Only the CRC-32 slicing tables are disabled, leaving several
KiB of headroom in the 64 KiB data segment. The
`wmake -f makefile.dos check` target enforces that data-segment budget.

## Serial transports

One executable contains three runtime-selectable transports. With no explicit
selection it tries FOSSIL, then an interrupt-driven 16550-compatible UART,
then BIOS INT 14h.

- `-f`, `-u`, or `-i` forces FOSSIL, direct UART, or BIOS I/O.
- `-cN` selects COM1 through COM4; COM1 is the default.
- `-aHEX` and `-gN` override the direct UART base address and IRQ. IRQs 2
  through 15 are accepted; IRQ 2 is mapped to IRQ 9 on AT-class machines.
- `-rRATE` changes the existing line configuration to the requested rate and
  8N1. Without it, the port preserves the current rate and framing.
- `-h`, `-x`, `-hx`, and `-k` select RTS/CTS, XON/XOFF, both, or no flow
  control. ZMODEM already escapes XON and XOFF on the wire.

The direct UART has an interrupt-driven 2 KiB receive ring and polled
transmit. FOSSIL and flow-controlled UART operation permit normal streaming.
BIOS I/O, and direct UART I/O without flow control, automatically request
acknowledged blocks. The BIOS receiver also advertises a 128-byte buffer so a
sender cannot overrun its polling interface.

The standard COM mappings are 3F8/IRQ4, 2F8/IRQ3, 3E8/IRQ4, and 2E8/IRQ3.
Use explicit `-a` and `-g` values for non-standard hardware.

## Emulator tests

`tests/test_dos.py` uses DOSBox's null-modem support to transfer an exact
binary payload in both directions for every backend. Its FOSSIL tests use
X00 1.50, installed as `X00.EXE`:

```sh
wmake -f makefile.dos check-emulator X00=/path/to/X00.EXE
```

The test still requires native POSIX `zmtx` and `zmrx` programs in the source
directory. DOS filenames, filesystem metadata, BIOS behavior, and UART
compatibility remain subject to the actual DOS installation and hardware.

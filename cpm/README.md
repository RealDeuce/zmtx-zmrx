# CP/M 2.2 platform

The CP/M build uses Z88DK's classic C library and its `+cpm` target:

```sh
make -f makefile.cpm
```

This produces `build/cpm/zmtx.com` and `build/cpm/zmrx.com`. It deliberately
enables `REDUCED_MEMORY=1` and defaults to acknowledged, nonstreaming ZMODEM
data. Define `CPM_STREAMING=1` only when the selected modem driver can
reliably overlap input and output.

## Modem driver

`rdrpun.c` is the generic CP/M 2.2 driver. It reads the `RDR:` device and
writes the `PUN:` device using BDOS calls. Its read is blocking, and its poll
and purge operations are no-ops because CP/M 2.2 has no portable status or
flush calls for these devices.

The generic driver currently requires `RDR:` and `PUN:` to preserve all eight
bits. This is a stronger requirement than the CP/M 2.2 BIOS contract, which
defines these logical devices as ASCII with the high bit clear. Many serial
BIOS implementations are eight-bit transparent, but portable CP/M software
cannot assume that they are. A system whose BIOS masks or uses bit 7 needs a
machine-specific driver that accesses an eight-bit-clean serial device
directly. The current `-b` mode does not make a seven-bit transport safe.

A machine-specific overlay can replace it without changing the frontend:

```sh
make -f makefile.cpm CPM_DRIVER=path/to/driver.c
```

The replacement implements the functions declared by
`zmodem_cpm_driver.h`. In particular, `read` returns a zmdm result code and
places the number of bytes read in `count`; `write`, `flush`, and `purge`
return zmdm result codes; and `poll` returns nonzero only when input is ready.
Hardware-specific serial status, timeouts, flow control, and input purging
belong in this overlay.

## Emulator testing

Run the live native-to-CP/M and CP/M-to-native checks with:

```sh
make -f makefile.cpm TNYLPO=/path/to/tnylpo check
```

tnylpo 1.2 and earlier use fully buffered stdio for raw `PUN:` files. That is
fine for ordinary file output but deadlocks an interactive protocol connected
through a FIFO. Apply `tests/tnylpo-unbuffered-output.patch` to tnylpo before
building the test executable. This changes only the emulator's raw character
device buffering; it is not needed on real CP/M hardware. Raw tnylpo devices
also preserve all eight bits, so these tests model the class of CP/M systems
on which the generic driver can operate.

CP/M 2.2 records do not preserve an exact byte length for arbitrary files.
Files whose length is not a multiple of 128 bytes may therefore be sent with
record padding. Filenames are also subject to the normal CP/M 8.3 rules.

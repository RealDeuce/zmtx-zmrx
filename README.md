# zmtx/zmrx

this file was abstracted from the original `zmodem.doc` after one
night I decided I had enough of all this marketing hype and wanted
to know what zmodem was all about without having to wade hip-deep
through advertising slogans.

## License

zmtx/zmrx is available under the [2-clause BSD license](LICENSE).

Special thanks to Jacques Mattheij, formerly of Mattheij Computer Service,
and original author of zmtx/zmrx.

## Building and installation

Build and test with BSD make, GNU make, or another compatible implementation:

```sh
make
make check
```

POSIX is the default platform. Select another platform implementation by
pointing `ZMODEM_PLATFORM` at a directory containing `plat.h`,
`zmodem_plat.h`, and `zmodem_plat.c`:

```sh
make clean
make ZMODEM_PLATFORM=path/to/platform
```

The [platform porting contract](PORTING.md) documents the exact POSIX-shaped
file, clock, transport, and frontend subset a platform must provide. POSIX
maps these operations directly to the native APIs without added wrapper calls.

Memory-constrained targets can select a standard-ZMODEM profile:

```sh
make clean
make REDUCED_MEMORY=1
make REDUCED_MEMORY=1 check
```

This profile limits data subpackets to 1 KiB and uses the existing bytewise
CRC-32 implementation instead of the slicing-by-8 tables. Large files are
still supported; they are transferred as additional 1 KiB subpackets. The
smaller limit also reduces the protocol input, encoded-output, sender, and
receiver buffers. It omits the non-standard `zmtx -4` and `zmtx -8` options,
and `zmrx` rejects oversized ZedZap subpackets after consuming them.

`REDUCED_MEMORY` changes public structure sizes, so every translation unit
that includes the protocol headers must be built consistently. Run
`make clean` when switching profiles. Defining `REDUCED_MEMORY` directly for
a non-make build has the same effect.

Optional end-to-end link tests exercise corruption recovery, asymmetric
bandwidth, and interoperability with `lrzsz` when either `lsz`/`lrz` or
`sz`/`rz` are installed:

```sh
make check-link
```

Set `LSZ` or `LRZ` to select non-default `lrzsz` executables. The link tests
use small temporary files and remove received files and partial transfers when
each test finishes.

The [implemented protocol state machines](docs/protocol-state-machine.md)
show the sender and receiver control flow, including streaming acknowledgements,
recovery, retries, and session cleanup.

The `install` target installs `zmtx` and `zmrx` under `/usr/local/bin` and
their manual pages under `/usr/local/share/man/man1`. The paths and install
tools are overridable; for example, a staged package installation can use:

```sh
make prefix=/usr DESTDIR=/tmp/zmtx-package install
make prefix=/usr DESTDIR=/tmp/zmtx-package uninstall
```

Use `make install-strip` to strip only the installed copies of the binaries.
FreeBSD ports may instead include stripping in `BSD_INSTALL_PROGRAM`, in which
case the regular `install` target honors it.

`exec_prefix`, `bindir`, `mandir`, `INSTALL`, `INSTALL_PROGRAM`,
`INSTALL_DATA`, `MKDIR_P`, and `PYTHON` may also be overridden. Uppercase
`PREFIX`, `BINDIR`, `MANDIR`, and `MKDIR` aliases are provided for BSD
makefiles; FreeBSD ports' `BSD_INSTALL_PROGRAM` and `BSD_INSTALL_MAN` are used
as fallbacks. The `uninstall` target removes only the four files installed by
`install` and leaves their containing directories intact.

## Transfer modes

The default sender uses standard ZMODEM data subpackets of at most 1 KiB.
`zmtx -4` and `zmtx -8` enable the non-standard 4 KiB and 8 KiB ZedZap
extensions. The sender starts at 1 KiB and grows toward the selected maximum
while the transfer remains error-free. Because ZedZap has no wire-level
negotiation, these options should be used only with compatible receivers.
Normal `zmrx` builds accept all three sizes automatically. Reduced-memory
builds use standard 1 KiB subpackets exclusively.

`zmtx -s` waits for a committed-position acknowledgement after every data
subpacket. `zmrx -s` requests the same behavior from a conforming sender.
The transmitter also selects this conservative mode automatically when the
receiver does not advertise overlapped I/O. If a streaming receiver advertises
a finite buffer, `zmtx` ends and acknowledges each segment before that buffer
would be exceeded.

`zmrx -e` requests control-character escaping from the sender, while `zmrx -b`
requests escaping of every byte with the eighth bit set. The options may be
combined as `-eb` when both forms of transport protection are required.

`zmtx -w32K` limits transmitted but unacknowledged data to a fixed 32 KiB
window. Values are byte counts with an optional binary `K` or `M` suffix. A
window must contain at least four selected maximum-size subpackets, and cannot
be combined with `-s`. If the receiver is not full duplex, the sender reports
the limitation and safely falls back to one-block acknowledgements.

## Intended audience

the intended audience of this document are programmers looking for
a compact reference text on how zmodem works and what you should
know to be able to implement conforming zmodem send and receive
software. this is definitely not an 'end user' document and the
examples and data structures are strongly biased towards the `C`
language. (what ? are there other languages ??)

a lot of work went into the preparation of this document; although
its correctness cannot be guaranteed.

## Changes relative to `zmodem.doc` as provided by various sources

- removal of all historical information
- removal of all plugs relating to omen technology products etc.
  if something is public domain then leave it at that
- removal of all 'poetry'
- removal of all references to xmodem ymodem kermit and so on
- removal of all overstrike typesetting tricks which make this
  file practically uneditable and unviewable
- removal of a lot of irrelevant but nice facts about the wheater and
  some other nice subjects for conversation
- removal of all implementation specific details referring to those
  antiques of telecom `rz` and `sz`
- manifest constants added in the text.
- moved footnotes to the appropriate place in the text
- changed number base from octal to hex (welcome to the nineties)
  admitted it looks less ivory tower but it reads a lot easier
  for those who started programming after 1959

## Some recomendations

a lot has changed since the original zmodem came out. not so much in the
protocol as well as in the world around it. I would like to de-advertise
several of zmodem's advanced features:

- **command sending.**
  this is the hackers dream come true. a formalized backdoor into any
  site supporting this file transfer protocol with a relatively easy
  defeated security mechanism. don't implement it; just refuse it.

- **file translation.**
  the zmodem protocol specification below contains a number of facilities
  to change a file between one os and the next. THIS IS NOT THE PLACE !
  a file TRANSFER protocol should do just that and with a minimum of fuss.
  if you have to start worrying about wheter zmodem just garbled that 4MB
  zip file of yours just downloaded from the states at $1 a minute you're
  ready for some agression. another point may be that the file size will
  change which may give rise to a lot of bugs in zmodem implementations on
  the far side of wherever you are downloading to / from. stick to
  binary. it helps.

- **use `CRC32` and not `CRC16`.**
  apart from the obvious (better error detection) the original `CRC16`
  implementation is buggy

- **do not send the serial number in the `ZFILE` frame.**
  this is not a very useful function

- in many places in the orignal `zmodem.doc` it was suggested that if
  this or that failed you should step down and attempt a ymodem
  transfer. don't do that ! users know pretty good what they want
  and if they specify a zmodem transfer give them one or give them
  nothing. don't try to be smart. probably something is wrong and it
  is better to exit with some informative message than to go ahead
  with the wrong protocol; apart from keeping your source clean.

- **don't use or implement the run length encoding.**
  it is greatly hampered by not checking if run length encoding is needed.
  if you specify that ability you're stuck with it. nowadays with
  zip 2.0/unzip 5.0 and better there is absolutely no need for a file
  transfer protocol to busy itself with compression. for $200
  you can buy a mnp class 10 modem which does all that and more
  completely transparently without possibly triggering a host of
  bugs in a relatively little exercised part of your hosts software.

- **lzw encoding;** see run length encoding.

- **don't use the `ZXSPARSE` option.**
  chances of finding a system that implements it are small and even then
  10 : 1 that the file will be sent compressed.

- **don't send the `rz\r` used in the original documentation.**
  this is a very nasty way of making a public domain protocol
  dependant on a company. (sorry had to abide by that in the end;
  some implementations trigger auto downloads on this)

## General guidance

In general; keep it simple ! stick to multiple file binary transfers and
try to get some speed out of those boxes. time is spent well on optimizing
and cleaning your source rather than on some obscure seldomely used
feature which will clutter your code.

Whenever I give an example of how not to program in `C` I refer to the
`rz.c` and `sz.c` sources. In more than one way these are true 'classics'.
If you intend to implement zmodem don't bother with these dinosaurs
(to use a popular term); better to write it clean from the start.

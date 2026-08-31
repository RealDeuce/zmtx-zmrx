# ZMODEM Protocol Reference

> This document describes the internals of the ZMODEM protocol. See the
> [project README](README.md) for implementation background and the
> [current source](https://github.com/RealDeuce/zmtx-zmrx) for a compact,
> working implementation.

## Contents

- [General](#general)
- [Sample transaction](#sample-transaction)
- [Requirements](#requirements)
- [Link escape encoding](#link-escape-encoding)
- [Headers and data subpackets](#headers-and-data-subpackets)
- [Protocol transaction overview](#protocol-transaction-overview)
- [Frame types](#frame-types)
- [ZFILE frame file information subpacket][zfile-info]
- [Streaming techniques and error recovery][streaming]
- [Constants](#constants)

[zfile-info]: #zfile-frame-file-information-subpacket
[streaming]: #streaming-techniques-and-error-recovery

## License

Copyright (c) 1994 Stephen Hurd
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

Special thanks to Jacques Mattheij, formerly of Mattheij Computer Service,
and original author of zmtx/zmrx.

## General

Documentation about the ZMODEM protocol internals; should be sufficient
to implement a completely functional ZMODEM protocol suite.

ZMODEM is a file transfer protocol that attempts to maximize bandwidth
and minimize transfer times. It is a unidirectional protocol; i.e., the
return channel only transfers control information, not data. Either side
may initiate the transfer, but the downloading site may respond to
an initialization frame by auto-starting the download software.

Schematically a ZMODEM file transfer in progress looks like this:

```text
             |----------<< back channel <<-------------|
       ------+-------                          --------+------
       |   Sender   |                          |   Receiver  |
       |  (upload)  |                          |  (download) |
       --------------                          --------+------
             |---------->> data channel >>-------------|
```

Multiple files may be transferred in one session.

## Sample transaction

All ZMODEM transactions are done using frames. A frame consists
of a header followed by one or more data subpackets.
A typical (simple) ZMODEM file transfer looks like this:

| Sender | Receiver |
| --- | --- |
| `ZRQINIT(0)` | |
| | `ZRINIT` |
| `ZFILE` | |
| | `ZRPOS` |
| `ZDATA` data ... | |
| `ZEOF` | |
| | `ZRINIT` |
| `ZFIN` | |
| | `ZFIN` |
| `OO` | |

ZMODEM continuously transmits data unless the receiver interrupts
the sender to request retransmission of garbled data.
ZMODEM in effect uses the entire file as a window.

## Requirements

ZMODEM requires an 8-bit transfer medium, but allows encoded packets
for less transparent media.
ZMODEM escapes network control characters to allow operation with
packet switched networks.

To support full streaming, the transmission path should either assert
flow control or pass full speed transmission without loss of data.
Otherwise the ZMODEM sender must manage the window size.

ZMODEM places no constraints on the content files.

## Link escape encoding

ZMODEM achieves data transparency by extending the 8-bit character set
(256 codes) with escape sequences based on the ZMODEM data link escape
character `ZDLE`.

Link Escape coding permits variable length data subpackets without the
overhead of a separate byte count.  It allows the beginning of frames to
be detected without special timing techniques, facilitating rapid error
recovery.

Link Escape coding does add some overhead.  The worst case, a file
consisting entirely of escaped characters, would incur a 50% overhead.

The `ZDLE` character is special. `ZDLE` represents a control
sequence of some sort. If a `ZDLE` character appears in binary data,
it is prefixed with `ZDLE`, then sent as `ZDLEE`.

Five consecutive `CAN` characters abort a ZMODEM session.

Since `CAN` is not used in normal terminal operations, interactive
applications and communications programs can monitor the data flow for
`ZDLE`.  The following characters can be scanned to detect the `ZRQINIT`
header, the invitation to automatically download commands or files.

Receipt of five successive `CAN` characters will abort a ZMODEM session.
Eight `CAN` characters are sent (just to be on the safe side).

The receiving program decodes any sequence of `ZDLE` followed by a byte with
bit 6 set and bit 5 reset (uppercase letter, either parity) to the
equivalent control character by inverting bit 6.  This allows the
transmitter to escape any control character that cannot be sent by the
communications medium. In addition, the receiver recognizes escapes for
`0x7f` and `0xff` should these characters need to be escaped.

ZMODEM software escapes `ZDLE` (`0x18`), `0x10`, `0x90`, `0x11`, `0x91`, `0x13`,
and `0x93`.
If preceded by `0x40` or `0xc0` (@), `0x0d` and `0x8d` are also escaped to
protect the Telenet command escape `CR`-@-`CR`. The receiver ignores
`0x11`, `0x91`, `0x13`, and `0x93` characters in the data stream.

## Headers and data subpackets

All ZMODEM frames begin with a header which may be sent in binary or HEX
form. Either form of the header contains the same raw information:

- A type byte
- Four bytes of data indicating flags and/or numeric quantities depending
  on the frame type

The maximum header information length is 16 bytes.
The data subpackets following the header are a maximum of 1024 bytes long.

```text
             M         L
      FTYPE  F3 F2 F1 F0   (flags frame)

             L         M
      FTYPE  P0 P1 P2 P3   (numeric frame)
```

> **Beware of the catch:** flags and numbers are indexed the other way around!

### 16-bit CRC binary header

A binary header is sent by the sending program to the receiving
program. All bytes in a binary header are `ZDLE` encoded.

A binary header begins with the sequence `ZPAD`, `ZDLE`, `ZBIN`.

Zero or more binary data subpackets with 16-bit CRC will follow depending
on the frame type.

```text
* ZDLE A TYPE F3/P0 F2/P1 F1/P2 F0/P3 CRC-1 CRC-2
```

### 32-bit CRC binary header

A 32-bit CRC binary header is similar to a binary header, except the
`ZBIN` (A) character is replaced by a `ZBIN32` (C) character, and four
characters of CRC are sent.

Zero or more binary data subpackets with 32-bit CRC will follow depending
on the frame type.

```text
* ZDLE C TYPE F3/P0 F2/P1 F1/P2 F0/P3 CRC-1 CRC-2 CRC-3 CRC-4
```

### Hex header

The receiver sends responses in hex headers.  The sender also uses hex
headers when they are not followed by binary data subpackets.

Hex encoding protects the reverse channel from random control
characters. The hex header receiving routine ignores parity.

Use of hex headers by the receiving program allows control characters
to be used to interrupt the sender when errors are detected.
A HEX header may be used in place of a binary header
wherever convenient.
If a data packet follows a HEX header, it is protected with CRC-16.

A hex header begins with the sequence `ZPAD`, `ZPAD`, `ZDLE`, `ZHEX`.
The extra `ZPAD` character allows the sending program to detect
an asynchronous header (indicating an error condition) and then
get the rest of the header with a non-error-specific routine.

The type byte, the four position/flag bytes, and the 16-bit CRC
thereof are sent in hex using the character set `0123456789abcdef`.
Uppercase hex digits are not allowed.
Since this form of hex encoding detects many patterns of errors,
especially missing characters, a hex header with 32-bit CRC has not
been defined.

A carriage return and line feed are sent with HEX headers.  The
receive routine expects to see at least one of these characters, two
if the first is `CR`.

An `XON` character is appended to all HEX packets except `ZACK` and `ZFIN`.
The `XON` releases the sender from spurious `XOFF` flow control characters
generated by line noise. `XON` is not sent after `ZACK` headers to protect
flow control in streaming situations. `XON` is not sent after a `ZFIN`
header to allow proper session cleanup.

Zero or more data subpackets will follow depending on the frame type.

```text
* * ZDLE B TYPE F3/P0 F2/P1 F1/P2 F0/P3 CRC-1 CRC-2 CR LF XON
```

(TYPE, F3...F0, CRC-1, and CRC-2 are each sent as two hex digits.)

### Binary data subpackets

Binary data subpackets immediately follow the associated binary header
packet.  A binary data packet contains 0 to 1024 bytes of data.
Recommended length values are 256 bytes below 2400 bps, 512 at
2400 bps, and 1024 above 4800 bps or when the data link is known to
be relatively error free.

No padding is used with binary data subpackets.  The data bytes are
`ZDLE` encoded and transmitted.  A `ZDLE` and frame end are then sent,
followed by two or four `ZDLE` encoded CRC bytes.  The CRC accumulates
the data bytes and frame end.

## Protocol transaction overview

ZMODEM timing is receiver driven.  The
transmitter should not time out at all, except to abort the program if no
headers are received for an extended period of time, say one minute.

### Session startup

To start a ZMODEM file transfer session, the sending program is called
with the names of the desired file(s) and option(s).

Then the sender may send a `ZRQINIT` header. The `ZRQINIT` header causes a
previously started receive program to send its `ZRINIT` header without
delay.

In an interactive or conversational mode, the receiving application
may monitor the data stream for `ZDLE`.  The following characters may be
scanned for `B00` indicating a `ZRQINIT` header, a command to download
data.

The sending program awaits a command from the receiving program to
start file transfers.

In case of garbled data, the sending program can repeat the invitation
to receive a number of times until a session starts.

When the ZMODEM receive program starts, it immediately sends a `ZRINIT`
header to initiate ZMODEM file transfers, or a `ZCHALLENGE` header to
verify the sending program.  The receive program resends its header at
response-time (default 10-second) intervals for a suitable period of
time (40 seconds total) before exiting.

If the receiving program receives a `ZRQINIT` header, it resends the
`ZRINIT` header.  If the sending program receives the `ZCHALLENGE` header,
it places the data in `ZP0`...`ZP3` in an answering `ZACK` header.

If the receiving program receives a `ZRINIT` header, it is an echo
indicating that the sending program is not operational.

Eventually the sending program correctly receives the `ZRINIT` header.

The sender may then send an optional `ZSINIT` frame to define the
receiving program's Attn sequence, or to specify complete control
character escaping. If the receiver specifies the same or higher
level of escaping the `ZSINIT` frame need not be sent unless an Attn
sequence is needed.

If the `ZSINIT` header specifies `ESCCTL` or `ESC8`, a HEX header is used,
and the receiver activates the specified ESC modes before reading the
following data subpacket.

The receiver sends a `ZACK` header in response, containing 0.

### File transmission

The sender then sends a `ZFILE` header with ZMODEM Conversion,
Management, and Transport options (see `ZFILE` header type) followed
by a `ZCRCW` data subpacket containing the file name, file length, and
modification date.

The receiver examines the file name, length, and date information
provided by the sender in the context of the specified transfer
options, the current state of its file system(s), and local security
requirements.  The receiving program should ensure the pathname and
options are compatible with its operating environment and local
security requirements.

The receiver may respond with a `ZSKIP` header, which makes the sender
proceed to the next file (if any) in the batch.

If the receiver has a file with the same name and length, it may respond
with a `ZCRC` header containing a byte count. This requires the sender to
perform a 32-bit CRC on the specified number of bytes in the file and
transmit the complement of the CRC in an answering `ZCRC` header. The CRC
is initialized to `0xffffffff`; a byte count of 0 implies the entire file.
The receiver uses this information to determine whether to accept or skip
the file. This sequence may be triggered by the `ZMCRC` Management Option.

A `ZRPOS` header from the receiver initiates transmission of the file
data starting at the offset in the file specified in the `ZRPOS` header.
Normally the receiver specifies the data transfer to begin at
offset 0 in the file.

The receiver may start the transfer further down in the
file.  This allows a file transfer interrupted by a loss
of carrier or system crash to be completed on the next
connection without requiring the entire file to be
retransmitted. If downloading a file from a timesharing
system that becomes sluggish, the transfer can be
interrupted and resumed later with no loss of data.

The sender sends a `ZDATA` binary header (with file position) followed
by one or more data subpackets.

The receiver compares the file position in the `ZDATA` header with the
number of characters successfully received to the file. If they do not
agree, a `ZRPOS` error response is generated to force the sender to the
right position within the file. (If the `ZTSPARS` option is used, the
receiver instead seeks to the position given in the `ZDATA` header.)

A data subpacket terminated by `ZCRCG` and CRC does not elicit a
response unless an error is detected; more data subpacket(s) follow
immediately.

`ZCRCQ` data subpackets expect a `ZACK` response with the
receiver's file offset if no error, otherwise a `ZRPOS`
response with the last good file offset.  Another data
subpacket continues immediately.  `ZCRCQ` subpackets are
not used if the receiver does not indicate full duplex ability
with the `CANFDX` bit.

`ZCRCW` data subpackets expect a response before the next frame is sent.
If the receiver does not indicate overlapped I/O capability with the
`CANOVIO` bit, or sets a buffer size, the sender uses the `ZCRCW` to allow
the receiver to write its buffer before sending more data.

A zero length data frame may be used as an idle
subpacket to prevent the receiver from timing out in
case data is not immediately available to the sender.

In the absence of fatal error, the sender eventually encounters end of
file.  If the end of file is encountered within a frame, the frame is
closed with a `ZCRCE` data subpacket which does not elicit a response
except in case of error.

The sender sends a `ZEOF` header with the file ending offset equal to
the number of characters in the file.  The receiver compares this
number with the number of characters received. If the receiver has
received all of the file, it closes the file.  If the file close was
satisfactory, the receiver responds with `ZRINIT`.  If the receiver has
not received all the bytes of the file, the receiver ignores the `ZEOF`
because a new `ZDATA` is coming. If the receiver cannot properly close
the file, a `ZFERR` header is sent.

After all files are processed, any further protocol
errors should not prevent the sending program from
returning with a success status.

### Session cleanup

The sender closes the session with a `ZFIN` header.  The receiver
acknowledges this with its own `ZFIN` header.

When the sender receives the acknowledging header, it sends two
characters, "OO" (Over and Out) and exits.
The receiver waits briefly for the "O" characters, then exits
whether they were received or not.

### Session abort sequence

If the receiver is receiving data in streaming mode, the Attn
sequence is executed to interrupt data transmission before the Cancel
sequence is sent.  The Cancel sequence consists of eight `CAN`
characters and ten backspace characters.  ZMODEM only requires five
Cancel characters, the other three are "insurance".

The trailing backspace characters attempt to erase the effects of the
`CAN` characters if they are received by a command interpreter.

```c
char ses_abort_seq[] = {
	24, 24, 24, 24, 24, 24, 24, 24,
	8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 0
};
```

### Attention sequence

The receiving program sends the Attn sequence whenever it detects an
error and needs to interrupt the sending program.

The default Attn string value is empty (no Attn sequence).  The
receiving program resets Attn to the empty default before each
transfer session.

The sender specifies the Attn sequence in its optional `ZSINIT` frame.
The Attn string is terminated with a null.

## Frame types

The numeric values are listed at the end of this file.
Unused bits and unused bytes in the header (`ZP0`...`ZP3`) are set to 0.

### `ZRQINIT`

Sent by the sending program, to trigger the receiving program to send
its `ZRINIT` header.
The sending program may
repeat the receive invitation (including `ZRQINIT`) if a response is
not obtained at first.

`ZF0` contains `ZCOMMAND` if the program is attempting to send a command,
0 otherwise.

### `ZRINIT`

Sent by the receiving program. `ZF0` and `ZF1` contain the bitwise OR
of the receiver capability flags:

| Flag | Capability |
| --- | --- |
| `CANFDX` | Receiver can send and receive true full duplex |
| `CANOVIO` | Receiver can receive data during disk I/O |
| `CANBRK` | Receiver can send a break signal |
| `CANRLE` | Receiver can decode run-length encoding |
| `CANLZW` | Receiver can uncompress |
| `CANFC32` | Receiver can use 32-bit Frame Check |
| `ESCCTL` | Receiver expects control characters to be escaped |
| `ESC8` | Receiver expects the eighth bit to be escaped |

`ZP0` and `ZP1` contain the size of the receiver's buffer in bytes, or `0`
if nonstop I/O is allowed.

> **Compatibility note:** While all these capabilities are nice in theory,
> most ZMODEM implementations balk at anything other than `0,0`. I.e., Telix
> starts sending 35-byte packets when `CANFC32` is on.

### `ZSINIT`

The sender sends flags followed by a binary data subpacket terminated
with `ZCRCW`.

| Flag | Meaning |
| --- | --- |
| `TESCCTL` | Transmitter expects control characters to be escaped |
| `TESC8` | Transmitter expects the eighth bit to be escaped |

The data subpacket contains the null-terminated Attn sequence,
with a maximum length of 32 bytes including the terminating null.

### `ZACK`

Acknowledgment to a `ZSINIT` frame, `ZCHALLENGE` header, `ZCRCQ` or `ZCRCW`
data subpacket.  `ZP0` to `ZP3` contain file offset.  The response to
`ZCHALLENGE` contains the same 32-bit number received in the `ZCHALLENGE`
header.

### `ZFILE`

This frame denotes the beginning of a file transmission attempt.
`ZF0`, `ZF1`, and `ZF2` may contain options. A value of 0 in each of these
bytes implies no special treatment.  Options specified to the
receiver override options specified to the sender with the exception
of `ZCBIN`.  A `ZCBIN` from the sender overrides any other Conversion
Option given to the receiver except `ZCRESUM`.  A `ZCBIN` from the
receiver overrides any other Conversion Option sent by the sender.

#### `ZF0`: Conversion option

If the receiver does not recognize the Conversion Option, an
application-dependent default conversion may apply.

- **`ZCBIN` — “Binary” transfer.** Inhibit conversion unconditionally.

- **`ZCNL` — Convert received end of line to the local convention.** The
  supported end-of-line conventions are `CR`/`LF` (most ASCII-based operating
  systems except Unix and Macintosh), and NL (Unix). Either of these two
  conventions meets the permissible ASCII definitions for Carriage Return
  and Line Feed/New Line. Neither the ASCII code nor ZMODEM `ZCNL` encompasses
  lines separated only by carriage returns. Other processing appropriate to
  ASCII text files and the local operating system may also be applied by the
  receiver (filtering `RUBOUT`, `NULL`, Ctrl-Z, etc.).

- **`ZCRESUM` — Recover/resume an interrupted file transfer.** `ZCRESUM` is
  also useful for updating a remote copy of a file that grows without
  resending old data. If the destination file exists and is no longer than
  the source, append to the destination file and start transfer at the offset
  corresponding to the receiver's end of file. This option does not apply if
  the source file is shorter. Files that have been converted (e.g., `ZCNL`) or
  are subject to a single-ended Transport Option cannot have their transfers
  recovered.

#### `ZF1`: Management option

If the receiver does not recognize the Management Option, the
file should be transferred normally.

The `ZMSKNOLOC` bit instructs the receiver to bypass the
current file if the receiver does not have a file with the
same name.

Five bits (defined by `ZMMASK`) define the following set of
mutually exclusive management options.

- **`ZMNEWL`** — Transfer the file if the destination file is absent.
  Otherwise, overwrite the destination if the source file is newer or longer.
- **`ZMCRC`** — Compare the source and destination files. Transfer if file
  lengths or file polynomials differ.
- **`ZMAPND`** — Append the source file contents to the end of the existing
  destination file (if any).
- **`ZMCLOB`** — Replace the existing destination file (if any).
- **`ZMDIFF`** — Transfer the file if the destination file is absent.
  Otherwise, overwrite the destination if the files have different lengths
  or dates.
- **`ZMPROT`** — Protect the destination by transferring only if it is absent.
- **`ZMNEW`** — Transfer the file if the destination file is absent.
  Otherwise, overwrite the destination if the source file is newer.

#### `ZF2`: Transport option

If the receiver does not implement the particular transport
option, the file is copied without conversion for later processing.

> **Compatibility note:** Better not to use these; see the
> [project README](README.md).

- **`ZTLZW` — Lempel-Ziv compression.** Transmitted data will be identical to
  that produced by compress 4.0 operating on a computer with VAX byte
  ordering, using 12-bit encoding.
- **`ZTCRYPT` — Encryption.** An initial null-terminated string identifies the
  key. Details to be determined.
- **`ZTRLE` — Run Length encoding.** Details to be determined.

A `ZCRCW` data subpacket follows with file name, file length,
modification date, and other information described in a later
chapter.

#### `ZF3`: Extended options

The Extended Options are bit encoded.

- **`ZTSPARS` — Special processing for sparse files, or sender-managed
  selective retransmission.** Each file segment is transmitted as a separate
  frame, where the frames are not necessarily contiguous. The sender should
  end each segment with a `ZCRCW` data subpacket and process the expected
  `ZACK` to ensure no data is lost. `ZTSPARS` cannot be used with `ZCNL`.

### `ZSKIP`

Sent by the receiver in response to `ZFILE`, makes the sender skip to
the next file.

### `ZNAK`

Indicates last header was garbled.  (See also `ZRPOS`).

### `ZABORT`

Sent by the receiver to terminate batch file transfers when requested by
the user. The sender responds with a `ZFIN` sequence (or `ZCOMPL` in server
mode).

### `ZFIN`

Sent by sending program to terminate a ZMODEM session. Receiver
responds with its own `ZFIN`.

### `ZRPOS`

Sent by receiver to force file transfer to resume at file offset
given in `ZP0`...`ZP3`.

### `ZDATA`

`ZP0`...`ZP3` contain file offset. One or more data subpackets follow.

### `ZEOF`

Sender reports End of File.  `ZP0`...`ZP3` contain the ending file
offset.

### `ZFERR`

Error in reading or writing file, protocol equivalent to `ZABORT`.

### `ZCRC`

Request (receiver) and response (sender) for file polynomial.
`ZP0`...`ZP3` contain file polynomial.

### `ZCHALLENGE`

Request sender to echo a random number in `ZP0`...`ZP3` in a `ZACK` frame.
Sent by the receiving program to the sending program to verify that
it is connected to an operating program, and was not activated by
spurious data or a Trojan Horse message.

> **Security note:** This is the most simply defeated security system ever
> invented. Don't rely on it and better still don't use or implement it.
> Build your security measures around starting the download at all and
> disallow explicit path names.

### `ZCOMPL`

Request now completed.

### `ZCAN`

This is a pseudo-frame type in response to a Session Abort sequence.

### `ZFREECNT`

Sending program requests a `ZACK` frame with `ZP0`...`ZP3` containing the
number of free bytes on the current file system. A value of `0`
represents an indefinite amount of free space.

### `ZCOMMAND`

`ZCOMMAND` is sent in a binary frame.  `ZF0` contains 0 or `ZCACK1` (see
below).

A `ZCRCW` data subpacket follows, with the ASCII text command string
terminated with a `NULL` character.  If the command is intended to be
executed by the operating system hosting the receiving program
(e.g., "shell escape"), it must have `!` as the first character.
Otherwise the command is meant to be executed by the application
program which receives the command.

If the receiver detects an illegal or badly formed command, the
receiver immediately responds with a `ZCOMPL` header with an error
code in `ZP0`...`ZP3`.

If `ZF0` contained `ZCACK1`, the receiver immediately responds with a
`ZCOMPL` header with 0 status.

Otherwise, the receiver responds with a `ZCOMPL` header when the
operation is completed.  The exit status of the completed command is
stored in `ZP0`...`ZP3`.  A 0 exit status implies nominal completion of
the command.

If the command causes a file to be transmitted, the command sender
will see a `ZRQINIT` frame from the other computer attempting to send
data.

The sender examines `ZF0` of the received `ZRQINIT` header to verify it
is not an echo of its own `ZRQINIT` header.  It is illegal for the
sending program to command the receiving program to send a command.

If the receiver program does not implement command downloading, it
may display the command to the standard error output, then return a
`ZCOMPL` header.

### `ZFILE` frame file information subpacket

ZMODEM sends the same file information with the `ZFILE` frame data.

The pathname (file name) field is mandatory. Each field after
pathname is optional and separated from the previous one by
a space. Fields may not be skipped. The use of the optional
fields is (by definition) optional to the receiver.

#### Pathname

The pathname (conventionally, the file name) is sent as a
null-terminated ASCII string.
No spaces are included in the pathname.

##### Filename considerations

- File names should be translated to lower case unless the
  sending system supports uppercase/lowercase file names. This
  is a convenience for users of systems (such as Unix) which
  store filenames in upper and lower case.

- The receiver should accommodate file names in lowercase and
  uppercase.

- When transmitting files between different operating
  systems, file names must be acceptable to both the sender
  and receiving operating systems.  If not, transformations
  should be applied to make the file names acceptable. If
  the transformations are unsuccessful, a new file name may
  be invented by the receiving program.

- If directories are included, they are delimited by `/`; i.e.,
  `subdir/foo` is acceptable, while `subdir\foo` is not.

#### Length

The length field is stored as a decimal string
counting the number of data bytes in the file.

The ZMODEM receiver uses the file length as an estimate only.
It may be used to display an estimate of the transmission time,
and may be compared with the amount of free disk space.  The
actual length of the received file is determined by the data
transfer. A file may grow after transmission commences, and
all the data will be sent.

#### Modification date

The modification date is sent as an octal number giving the time the
contents of the file were last changed measured in seconds from
January 1, 1970, Universal Coordinated Time (GMT). A date of 0
implies the modification date is unknown and should be left as
the date the file is received.

This standard format was chosen to eliminate ambiguities
arising from transfers between different time zones.

#### File mode

The file mode is stored as an octal string.
Unless the file originated from a Unix system, the file mode is
set to 0.

#### Serial number

Set this field to 0.

#### Number of files remaining

This field is coded as a decimal number, and includes the
current file.  This field is an estimate, and incorrect values
must not be allowed to cause loss of data.

#### Number of bytes remaining

This field is coded as a decimal number, and includes the
current file.  This field is an estimate, and incorrect values
must not be allowed to cause loss of data.

#### File type

Set this field to 0.

The file information is terminated by a null. If only the pathname
is sent, the pathname is terminated with two nulls. The length of
the file information subpacket, including the trailing null, must
not exceed 1024 bytes; a typical length is less than 64 bytes.

## Streaming techniques and error recovery

ZMODEM provides several data streaming methods
selected according to the limitations of the sending environment,
receiving environment, and transmission channel(s).

### Window management

When sending data through a network, some nodes of the network store
data while it is transferred to the receiver.  7000 bytes and more of
transient storage have been observed.  Such a large amount of storage
causes the transmitter to "get ahead" of the receiver.
This condition is not fatal but it does slow error recovery.

To manage the window size, the sending program uses `ZCRCQ` data
subpackets to trigger `ZACK` headers from the receiver.  The returning
`ZACK` headers inform the sender of the receiver's progress.  When the
window size (current transmitter file offset - last reported receiver
file offset) exceeds a specified value, the sender waits for a
`ZACK` packet with a receiver file offset that reduces the window
size. `ZRPOS` and other error packets are handled normally.

### Full streaming with sampling

If the receiver can overlap serial I/O with disk I/O, and if the
sender can sample the reverse channel for the presence of data
without having to wait, full streaming can be used with no Attn
sequence required.  The sender begins data transmission with a `ZDATA`
header and continuous `ZCRCG` data subpackets.  When the receiver
detects an error, it executes the Attn sequence and then sends a
`ZRPOS` header with the correct position within the file.

At the end of each transmitted data subpacket, the sender checks for
the presence of an error header from the receiver.  To do this, the
sender samples the reverse data stream for the presence of either a
`ZPAD` or `CAN` character (using `rdchk()` or a similar mechanism).
Flow control characters (if present) are acted upon.

Other characters (indicating line noise) increment a counter which is
reset whenever the sender waits for a header from the receiver.  If
the counter overflows, the sender sends the next data subpacket as
`ZCRCW`, and waits for a response.

`ZPAD` indicates some sort of error header from the receiver.  A `CAN`
suggests the user is attempting to "stop the bubble machine" by
keyboarding `CAN` characters.  If one of these characters is seen, an
empty `ZCRCE` data subpacket is sent.  Normally, the receiver will have
sent an `ZRPOS` or other error header, which will force the sender to
resume transmission at a different address, or take other action.  In
the unlikely event the `ZPAD` or `CAN` character was spurious, the
receiver will time out and send a `ZRPOS` header.
The obvious choice of `ZCRCW` packet, which would trigger an `ZACK` from
the receiver, is not used because multiple in transit frames could
result if the channel has a long propagation delay.

Then the receiver's response header is read and acted upon, starting
with a resynchronize.

A `ZRPOS` header resets the sender's file offset to the correct
position.  If possible, the sender should purge its output buffers
and/or networks of all unprocessed output data, to minimize the
amount of unwanted data the receiver must discard before receiving
data starting at the correct file offset.  The next transmitted data
frame should be a `ZCRCW` frame followed by a wait to guarantee
complete flushing of the network's memory.

If the receiver gets a `ZACK` header with an address that disagrees
with the sender address, it is ignored, and the sender waits for
another header.  A `ZFIN`, `ZABORT`, or `TIMEOUT` terminates the session; a
`ZSKIP` terminates the processing of this file.

The reverse channel is then sampled for the presence of another
header from the receiver (if sampling is possible). If one is detected,
another error header is read. Otherwise,
transmission resumes at the (possibly reset) file offset with a `ZDATA`
header followed by data subpackets.

### Full streaming with reverse interrupt

The above method cannot be used if the reverse data stream cannot be
sampled without entering an I/O wait.  An alternate method is to
instruct the receiver to interrupt the sending program when an error
is detected.

The receiver can interrupt the sender with a control character, break
signal, or combination thereof, as specified in the Attn sequence.
After executing the Attn sequence, the receiver sends a hex `ZRPOS`
header to force the sender to resend the lost data.

When the sending program responds to this interrupt, it reads a HEX
header (normally `ZRPOS`) from the receiver and takes the action
described in the previous section.

### Full streaming with sliding window

If none of the above methods is applicable, hope is not yet lost.  If
the sender can buffer responses from the receiver, the sender can use
`ZCRCQ` data subpackets to get ACKs from the receiver without
interrupting the transmission of data. After a sufficient number of
`ZCRCQ` data subpackets have been sent, the sender can read one of the
headers that should have arrived in its receive interrupt buffer.

A problem with this method is the possibility of wasting an excessive
amount of time responding to the receiver's error header.  It may be
possible to program the receiver's Attn sequence to flush the
sender's interrupt buffer before sending the `ZRPOS` header.

### Segmented streaming

If the receiver cannot overlap serial and disk I/O, it uses the
`ZRINIT` frame to specify a buffer length which the sender will not
overflow.  The sending program sends a `ZCRCW` data subpacket and waits
for a `ZACK` header before sending the next segment of the file.

A sufficiently large receiving buffer allows throughput to closely
approach that of full streaming.  For example, 16 KB segmented
streaming adds about 3 per cent to full streaming ZMODEM file
transfer times when the round trip delay is five seconds.

## Omen ZMODEM-90 seven-bit, Pack-7, and RLE framing

The published text names `ESC8`, `ZBINR32`, and `ZTRLE` without completely
specifying their wire representation. The following format is recovered from
Omen Technology's 1997 `DSZ.EXE`, which is treated as normative where the text
is incomplete. The binary hashes, routine addresses, and reproduction command
are recorded in the [DSZ disassembly evidence](docs/omen-esc8-disassembly.md).

An ESC8 header begins `ZPAD ZDLE 0x31`. The next raw seven-bit byte is
`0x22 + parameter_count`; the ordinary four-parameter header therefore uses
`0x26`. The quoted frame type and parameters are followed by little-endian
CRC32. That CRC covers the header bytes followed by the literal string
`Copyright 1989 Omen Technology INC All Rights Reserved`.

For seven-bit quoting, an ordinary high-bit byte is sent as `SO` (`0x0e`)
followed by its low seven bits. The private ZDLE codes `l` through `t` encode,
respectively, `0x7f`, `0xff`, `0x0e`, `0x8e`, `0x90`, `0x91`, `0x93`, `0x80`,
and `0x98`. Other control bytes retain normal ZDLE/XOR-`0x40` quoting. With
`ESCCTL`, a high-bit control can therefore become three wire bytes:
`SO ZDLE (low7 ^ 0x40)`.

Mode `0x31` applies RLE before seven-bit quoting and uses CRC32 over the RLE
token stream plus the data-subpacket terminator. `ZRESC` (`0x7e`) introduces
the following tokens:

- `ZRESC 0x40` represents one literal `ZRESC`.
- `ZRESC (run + 0x1d)` represents 3 through 34 spaces.
- `ZRESC (run + 0x40) value` represents 2 through 63 copies of `value`.
- A two-byte run of an ordinary low-seven-bit value remains literal.

Long runs are split at 63 bytes. Standard `ZBINR32` uses the same RLE token
grammar and CRC32 calculation with ordinary ZDLE quoting instead of the
seven-bit layer.

### Pack-7 framing

Pack-7 is negotiated with bit `0x02` in parameter six of an extended `ZRPOS`.
The receiver sends that extended header only when the sender advertised
variable-header support with `ZFILE.ZF3` bit `0x01`. It uses the same quoted,
salted-CRC32 header as `0x31`, with indicator `0x32`. The sender remains in
`0x31` mode when the request is absent or ignored.

Each group of one through four data bytes is interpreted as a big-endian
integer and encoded as exactly two through five base-88 digits. Digits are
most-significant first and use values `0x22` through `0x79`. The sender emits
`0x21` after the final group, followed directly by the raw subpacket terminator.
CRC32 covers the decoded data and terminator; its four little-endian bytes are
encoded as one final five-digit group. `ZCRCW` retains the normal trailing
`XON`.

Canonical partial groups contain zero, two, three, or four digits before
`0x21`, yielding zero through three bytes. Receivers reject digits outside the
base-88 alphabet, one-digit partial groups, arithmetic overflow, premature
terminators, and non-five-digit CRC groups.

### MobyTurbo transparent framing

Omen's MobyTurbo is a negotiated ZMODEM-90 mode for a fully transparent
connection. There is no equivalent standard mode: the ordinary receive
grammar discards raw XON/XOFF bytes, while `ESCCTL` requests more quoting and
cannot request less.

Before `ZFILE`, a Moby-capable sender emits the raw transparency probe
`23 c1 d4 93 11`. The sender offers MobyTurbo by setting bit `0x04` in
`ZFILE.ZF3` and variable-header support with `ZFILE.ZF3` bit `0x01`; a receiver
may also request it locally. Only a receiver which saw the complete probe
unchanged and the variable-header offer requests the mode. Its `ZRPOS` is a
variable seven-parameter header: parameters four and five are zero and bit
`0x01` of parameter six requests MobyTurbo. The sender changes modes only
after that request, so the `ZFILE` header and metadata retain ordinary framing.

A MobyTurbo header begins `ZPAD ZDLE 0x33`, followed by the unoffset parameter
count, the frame type and parameters, and little-endian CRC32. As with Omen's
`0x31` header, the CRC covers the header followed by the literal copyright
string above. Header bytes use MobyTurbo quoting.

MobyTurbo data is not compressed. Every byte except `ZDLE` is sent literally;
`ZDLE` retains its normal `ZDLE ZDLEE` representation. A data-subpacket end is
`ZDLE` plus its `ZCRC*` code, followed by little-endian CRC32 over the original
data and that code. Implementations may still apply requested `ESCCTL` or
transport-specific IAC quoting, which is valid but reduces the overhead
benefit.

## Constants

### ASCII control characters

| Name | Value |
| --- | ---: |
| `SOH` | `0x01` |
| `STX` | `0x02` |
| `EOT` | `0x04` |
| `ENQ` | `0x05` |
| `ACK` | `0x06` |
| `LF` | `0x0a` |
| `CR` | `0x0d` |
| `SO` | `0x0e` |
| `XON` | `0x11` |
| `XOFF` | `0x13` |
| `NAK` | `0x15` |
| `CAN` | `0x18` |

### ZMODEM link constants

| Name | Value | Meaning |
| --- | ---: | --- |
| `ZPAD` | `0x2a` | Pad character; begins frames |
| `ZDLE` | `0x18` | Ctrl-X ZMODEM escape |
| `ZDLEE` | `0x58` | Escaped `ZDLE` |
| `ZBIN` | `0x41` | Binary frame indicator (CRC16) |
| `ZHEX` | `0x42` | Hex frame indicator |
| `ZBIN32` | `0x43` | Binary frame indicator (CRC32) |
| `ZBINR32` | `0x44` | Run Length encoded binary frame (CRC32) |
| `ZBINR32ESC8` | `0x31` | Omen RLE, CRC32, and seven-bit-safe eighth-bit quoting |
| `ZBINP7` | `0x32` | Omen Pack-7 and CRC32 framing |
| `ZBINM32` | `0x33` | Omen MobyTurbo transparent CRC32 framing |
| `ZVBIN` | `0x61` | Binary frame indicator (CRC16) |
| `ZVHEX` | `0x62` | Hex frame indicator |
| `ZVBIN32` | `0x63` | Binary frame indicator (CRC32) |
| `ZVBINR32` | `0x64` | Run Length encoded binary frame (CRC32) |
| `ZRESC` | `0x7e` | Run Length encoding flag / escape character |

### Frame type values

| Name | Value | Meaning |
| --- | ---: | --- |
| `ZRQINIT` | `0x00` | Request receive init (sender → receiver) |
| `ZRINIT` | `0x01` | Receive init (receiver → sender) |
| `ZSINIT` | `0x02` | Send init sequence, optional (sender → receiver) |
| `ZACK` | `0x03` | Acknowledge `ZRQINIT`, `ZRINIT`, or `ZSINIT` (bidirectional) |
| `ZFILE` | `0x04` | File name (sender → receiver) |
| `ZSKIP` | `0x05` | Skip this file (receiver → sender) |
| `ZNAK` | `0x06` | Last packet was corrupted (?) |
| `ZABORT` | `0x07` | Abort batch transfers (?) |
| `ZFIN` | `0x08` | Finish session (bidirectional) |
| `ZRPOS` | `0x09` | Resume data transmission here (receiver → sender) |
| `ZDATA` | `0x0a` | Data packet(s) follow (sender → receiver) |
| `ZEOF` | `0x0b` | End of file reached (sender → receiver) |
| `ZFERR` | `0x0c` | Fatal read or write error detected (?) |
| `ZCRC` | `0x0d` | Request for file CRC and response (?) |
| `ZCHALLENGE` | `0x0e` | Security challenge (receiver → sender) |
| `ZCOMPL` | `0x0f` | Request is complete (?) |
| `ZCAN` | `0x10` | Pseudo-frame: other end cancelled with five `CAN` bytes |
| `ZFREECNT` | `0x11` | Request free bytes on file system (sender → receiver) |
| `ZCOMMAND` | `0x12` | Issue command (sender → receiver) |
| `ZSTDERR` | `0x13` | Output data to standard error (??) |

### `ZDLE` sequences

| Name | Value | Meaning |
| --- | ---: | --- |
| `ZCRCE` | `0x68` | CRC next; frame ends; header packet follows |
| `ZCRCG` | `0x69` | CRC next; frame continues nonstop |
| `ZCRCQ` | `0x6a` | CRC next; frame continues; `ZACK` expected |
| `ZCRCW` | `0x6b` | CRC next; `ZACK` expected; end of frame |
| `ZRUB0` | `0x6c` | Translate to rubout `0x7f` |
| `ZRUB1` | `0x6d` | Translate to rubout `0xff` |

### Receiver capability flags

| Name | Value | Meaning |
| --- | ---: | --- |
| `CANFDX` | `0x01` | Receiver can send and receive true full duplex |
| `CANOVIO` | `0x02` | Receiver can receive data during disk I/O |
| `CANBRK` | `0x04` | Receiver can send a break signal |
| `CANRLE` | `0x08` | Receiver can decode run-length encoding |
| `CANLZW` | `0x10` | Receiver can uncompress |
| `CANFC32` | `0x20` | Receiver can use 32-bit Frame Check |
| `ESCCTL` | `0x40` | Receiver expects control characters to be escaped |
| `ESC8` | `0x80` | Receiver expects the eighth bit to be escaped |

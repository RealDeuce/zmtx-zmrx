# Platform porting contract

POSIX is the primary zmtx/zmrx platform.  Other targets are supported by
providing the small POSIX-shaped subset described here; the protocol and
application sources do not contain target-specific branches.

## Platform directory

Set `ZMODEM_PLATFORM` to a directory containing these three files:

- `plat.h` is included before every other header in every translation unit.
  Use it for compiler workarounds, feature-test macros, replacement standard
  headers, or adjustments such as defining `ZMODEM_FORCE_32BIT_SPAN` on a
  compiler that provides slow emulated 64-bit arithmetic.
- `zmodem_plat.h` defines the type, constant, and function-call macros below
  and declares the transport and frontend hooks.
- `zmodem_plat.c` implements those hooks and is compiled as `plat.o`.

The platform include directory precedes the project directory, allowing a
port to provide replacement `stdint.h` or `inttypes.h`.  Run `make clean`
before changing `ZMODEM_PLATFORM` because object layouts and compiler
workarounds may differ.

## Application macros

The application sources require the following type macros.  A replacement
type must support the same operations and named fields as its POSIX mapping.

| Macro | POSIX expansion | Required behavior |
| --- | --- | --- |
| `ZMODEM_PLAT_OFF_T` | `off_t` | Signed seek and stream position type |
| `ZMODEM_PLAT_SSIZE_T` | `ssize_t` | Signed byte-count result type |
| `ZMODEM_PLAT_TIMESPEC` | `struct timespec` | `tv_sec` and `tv_nsec` fields |
| `ZMODEM_PLAT_STAT_T` | `struct stat` | `st_size` and `st_mtime` fields |
| `ZMODEM_PLAT_UTIMBUF` | `struct utimbuf` | `actime` and `modtime` fields |
| `ZMODEM_PLAT_MODE_T` | `mode_t` | File creation mode type |

The application sources require these constant macros:

| Macro | POSIX expansion |
| --- | --- |
| `ZMODEM_PLAT_STDIN` | `STDIN_FILENO` |
| `ZMODEM_PLAT_STDOUT` | `STDOUT_FILENO` |
| `ZMODEM_PLAT_CLOCK_MONOTONIC` | `CLOCK_MONOTONIC` |
| `ZMODEM_PLAT_SEEK_CURRENT` | `SEEK_CUR` |
| `ZMODEM_PLAT_SEEK_START` | `SEEK_SET` |
| `ZMODEM_PLAT_OPEN_READ_ONLY` | `O_RDONLY` |
| `ZMODEM_PLAT_OPEN_WRITE_ONLY` | `O_WRONLY` |
| `ZMODEM_PLAT_OPEN_CREATE` | `O_CREAT` |
| `ZMODEM_PLAT_OPEN_EXCLUSIVE` | `O_EXCL` |
| `ZMODEM_PLAT_ERROR_INTERRUPTED` | `EINTR` |
| `ZMODEM_PLAT_ERROR_NOT_FOUND` | `ENOENT` |
| `ZMODEM_PLAT_ERROR_IO` | `EIO` |
| `ZMODEM_PLAT_DEFAULT_NONSTREAMING` | `false` |
| `ZMODEM_PLAT_DEFAULT_JUNK_PATHNAMES` | `false` |

`ZMODEM_PLAT_REQUIRES_NONSTREAMING(io)` is evaluated after the platform's
post-parse hook has selected and opened its transport. It must be a Boolean
expression and may force acknowledged blocks for a runtime-selected transport
that cannot safely overlap input and output. The POSIX implementation expands
it directly to `false`.

`ZMODEM_PLAT_RECEIVE_BUFFER_SIZE(io)` is the maximum data segment a selected
transport can receive before returning an acknowledgement, or zero when no
additional limit is needed. It is used only when non-streaming operation is
selected. POSIX expands it to zero; a polling serial implementation can use it
to prevent an otherwise conforming sender from overrunning a shallow device
buffer.

The two default macros must be constant Boolean expressions. They select the
initial `-s` behavior and whether a receiver strips incoming directory names;
command-line options may still change the selected behavior.

The following function-like macros must preserve the corresponding POSIX
return conventions and `errno` behavior.  The POSIX definitions expand
directly to the native calls and add no wrapper functions.

| Macro | POSIX operation | Required subset |
| --- | --- | --- |
| `ZMODEM_PLAT_CLOCK_GETTIME` | `clock_gettime` | Monotonic seconds and nanoseconds |
| `ZMODEM_PLAT_OPEN` | `open` | Read-only open and `0666` exclusive creation |
| `ZMODEM_PLAT_CLOSE` | `close` | Close a descriptor and report failure |
| `ZMODEM_PLAT_READ` | `read` | Short reads, EOF as zero, interrupted retry |
| `ZMODEM_PLAT_LSEEK` | `lseek` | Absolute and current positions |
| `ZMODEM_PLAT_FSTAT` | `fstat` | Open-file size and modification time |
| `ZMODEM_PLAT_STAT_FILE` | `stat` | Destination existence, size, and mtime |
| `ZMODEM_PLAT_FDOPEN` | `fdopen` | Convert an exclusively created file to a stream |
| `ZMODEM_PLAT_FTELLO` | `ftello` | Position including buffered stream data |
| `ZMODEM_PLAT_FFLUSH` | `fflush` | Commit buffered received data, or defer this to the immediately following `fclose` |
| `ZMODEM_PLAT_UTIME` | `utime` | Best-effort access/modification time restoration |
| `ZMODEM_PLAT_STRERROR` | `strerror` | Human-readable text for a saved platform error |

The generic sources use C99 `FILE` streams for received data and diagnostics.
Consequently a port also needs the ordinary C99 behavior of `fopen`, `fwrite`,
`fclose`, `fprintf`, `snprintf`, and `errno`. Received
stream positions must advance when `fwrite` accepts data even if an eventual
flush reports a delayed error.

ZMODEM file positions are limited to `UINT32_MAX`.  Wider platform file types
are accepted, but every conversion to the wire range is checked.

## Transport and frontend hooks

`struct zmodem_plat_io` owns platform transport state.  The functions named
`zmodem_plat_io_init`, `zmodem_plat_io_bind`, `zmodem_plat_io_open`,
`zmodem_plat_io_make_raw`, `zmodem_plat_io_restore`, and
`zmodem_plat_io_close` provide the existing `struct zmodem_io` callbacks and
cleanup lifecycle.  A transport callback must implement the result contract
documented in `zmdm.h`. A capable transport should implement nonblocking poll,
purge, and bounded reads. A blocking-only target may instead return one byte
per read, ignore the requested timeout, and make poll and purge no-ops; such a
frontend should default to nonstreaming transfers and cannot enforce protocol
deadlines while blocked.

The application offers each leading command-line option to
`zmodem_plat_parse_option` before its protocol options.  The hook returns
not-handled, accepted, or invalid and may advance the index within a bundled
option.  It must not modify `argc`, `argv`, or argument strings.
`zmodem_plat_post_parse` receives the original argument vector and the index
of the first operand after validation.  `zmodem_plat_usage` appends platform
options to the normal help display.

The POSIX frontend uses these hooks for `-lpath`, process-wide `SIGPIPE`
suppression, optional device opening, and raw 8-bit terminal setup.

## POSIX subset used by the reference platform

The reference implementation uses only:

- `select`, `read`, and `write`, including short operations and `EINTR`;
- `clock_gettime(CLOCK_MONOTONIC)` for absolute input deadlines;
- `open`, `close`, `stat`, `fstat`, `lseek`, `fdopen`, `ftello`, and `utime`;
- `sigaction`, `sigemptyset`, and `SIGPIPE` suppression;
- `isatty`, `tcgetattr`, and `tcsetattr` with an 8-bit raw termios mode.

A non-POSIX port implements equivalent behavior only where its frontend uses
the feature.  It does not need general POSIX compliance.

## CP/M 2.2 reference port

The `cpm` directory is a Z88DK classic-library implementation of this
contract. Its `plat.h` supplies compiler compatibility, including the missing
`inline` keyword, and its local `inttypes.h` provides only the C99 integer
format and conversion facilities used by the frontends. Its prelude defines
`ZMODEM_FORCE_32BIT_SPAN`, so the protocol core selects its portable 32-bit
span scanner without altering standard-library limit macros.

The generic `cpm/rdrpun.c` transport uses blocking CP/M 2.2 `RDR:` and `PUN:`
BDOS calls. Build it with `make -f makefile.cpm`; set `CPM_DRIVER` to replace
it with a target-specific source file implementing
`cpm/zmodem_cpm_driver.h`. The CP/M frontend also maps timestamps to
best-effort no-ops, defaults to basename-only received files, and leaves the
final stream flush to `fclose` because the Z88DK classic `fflush` does not work
correctly on the `fdopen` stream used here.

## 16-bit DOS reference port

The `dos` directory implements the contract for Open Watcom's 16-bit C
compiler and real-mode DOS. Its prelude explicitly selects the 32-bit span
scanner. The build retains the normal protocol buffers but defines
`REDUCED_CRC=1` independently to keep the small-model near-data segment below
64 KiB.

The frontend selects among FOSSIL, interrupt-driven 16550-compatible UART,
and BIOS INT 14h transports at runtime. Its non-streaming and advertised
receive-buffer macros describe the selected transport without adding wrapper
calls to POSIX builds. Build and platform details are in `dos/README.md`.

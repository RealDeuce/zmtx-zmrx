# Platform porting contract

POSIX is the primary zmtx/zmrx platform.  Other targets are supported by
providing the small POSIX-shaped subset described here; the protocol and
application sources do not contain target-specific branches.

## Platform directory

Set `ZMODEM_PLATFORM` to a directory containing these three files:

- `plat.h` is included before every other header in every translation unit.
  Use it for compiler workarounds, feature-test macros, replacement standard
  headers, or adjustments such as undefining `UINT64_MAX`.
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
| `ZMODEM_PLAT_UTIME` | `utime` | Best-effort access/modification time restoration |

The generic sources use C99 `FILE` streams for received data and diagnostics.
Consequently a port also needs the ordinary C99 behavior of `fopen`, `fwrite`,
`fflush`, `fclose`, `fprintf`, `snprintf`, `errno`, and `strerror`.  Received
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
documented in `zmdm.h`, including nonblocking poll and bounded reads.

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
